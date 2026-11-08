# 案 5 の後半「受け手の set の使用中の判定を速くする」の設計（2026-10-10）

印：**[事実]** はソースで確かめたこと。**[推測]** は私の推論。
読んだのは `app\vk\icd\icd.c`・`app\vk\remote\server.c`・`app\vk\remote\proto.h`・`work\perf\report.md`・`app\vk\tools\slow_link.py` だけ。書き換えていない。行番号は 2026-10-10 15 時台に読んだ時点の物（案 5 の前半 `uds_*` が入った後）。

## 0. 要点

- [事実] descriptor を書く前に、受け手は「その set を使う、まだ走っている仕事」を探して、そこまで待つ（`last_with_set` → `drain_upto`）。書き込み 1 つごとに探す。
- [事実] 探し方は 4 重の線形：走っている仕事（新しい方から）× その仕事の記録 × 記録を探す `cbi()`（**全部の記録を前から探す**）× 記録が結んだ set。
- [事実・report.md] 1 コマ約 5,000 回で 0.5〜3 ms。
- 段取り：submit のときに、set ごとに「最後に使った submit の番号」を書いておく。書き込みのときは、表を引いて、一覧を二分探索するだけにする（D2）。その前に、同じ知らせの中の同じ set の答えを使い回す小さな直し（D1）と、`cbi()` を表引きにする直し（D3）。約束事は変えない。

## 1. 今の作り（受け手・server.c）

| 場所 | 行 | 何をするか |
|---|---|---|
| `cbinfo` | 675 | 記録（command buffer）ごとに、結んだ set の並び `sets` と、使うメモリの一覧 `mems` |
| `cbi()` | 717〜723 | 記録の番号から `cbinfo` を探す。`g_cbi` を前から線形に探す |
| `cbi_add_set` | 725〜729 | 記録に set を足す（同じ物は足さない。これも線形） |
| 記録の中の set を結ぶ命令 | 1270〜1285 | `cbi_add_set` と、その set の中の buffer を記録の「使うメモリ」に足す |
| `last_with_set` | 921〜929 | 一覧 `g_fl` を新しい方から、記録ごとに `cbi()` で探し、set の並びを線形に見る。見つかった仕事の位置を返す |
| `case VR_UPDATE_DESCRIPTOR_SETS` | 3368〜 | 書き込み 1 つごとに `drain_upto(last_with_set(sid))`（3398 行付近）。そのあと `uds_add` でためて、最後に 1 回の `vkUpdateDescriptorSets`（案 5 の前半） |
| 調べ用（`vkr_check()`） | 3377〜3397 付近 | submit した時点の set の写し（`snap`）とも比べて、取りこぼしを記録に出す |
| `case VR_RECORD_COMMAND_BUFFER` | 3536〜 | 記録し直しの前に、その記録を使う仕事まで待ち（`last_with_cb`）、`ci->n = 0` で set の並びを空にする |
| `case VR_QUEUE_SUBMIT` | 3644〜 | 一覧 `g_fl` に足す。`msg` は届いた順の番号 |
| `drain_upto` | 781〜 | 位置までの fence を待って一覧から外す。CPU からの合図を待つ物は残す |

- [事実] 一覧 `g_fl` は、submit の番号 `msg` の小さい順に並んでいる（足すのは後ろ・外しても順は変わらない。`drain_upto` は残す物を前に詰める）。→ 番号から位置を二分探索できる。
- [事実] 受け手は 2 次の command buffer を使わない（ドライバが中身を呼んだ側に写す。icd.c 7070〜7089 行）。push descriptor もドライバがふつうの set に直す（icd.c 926 行）。→ set を使うのは「記録の中で結んだ set」だけを見ればよい。

## 2. 段取り

### D0：先に測る（直さない）

- 受け手：`last_with_set` の呼ばれた回数と ms、`g_nfl` の大きさ、`g_ncbi`（記録の数）。`VKR_STATS` のような調べ用の切り替えのときだけ。
- 分かること：4 重のどこが重いか。`g_ncbi` が大きければ D3 が効く。

### D1：同じ知らせの中で、同じ set の答えを使い回す（小）

- [事実] 1 つの `VR_UPDATE_DESCRIPTOR_SETS` の中では submit は起きない。最初の書き込みで `drain_upto(k)` したあと、同じ set の次の書き込みでは、待つ物はもう無い。
- やり方：知らせの中で「さっき待った set」を覚え、同じ set なら `last_with_set` を呼ばない。
- [推測] DXVK や vkd3d-proton は、1 つの set に続けて何個も書くことが多いので、それだけで回数が減る。D0 で、続けて同じ set が来る割合を測る。
- 直す関数：`case VR_UPDATE_DESCRIPTOR_SETS`。
- 確かめ方：調べ用の `vkr_check()` を入れた状態で、取りこぼしの記録が出ないこと。
- 戻し方：環境変数（例 `VKR_SETCACHE=0`）。

### D2：set ごとに「最後に使った submit の番号」を持つ（本体）

- やり方
  1. 表 `set → 最後に使った submit の番号` を持つ（番号から番号への開番地のハッシュ表）。
  2. `case VR_QUEUE_SUBMIT` で submit が成功したら、その submit の記録ごとに、`cbinfo` の `sets` の全部について、表の番号を `my_msg` にする。
  3. `last_with_set(sid)`：表を引く。無ければ `-1`。あれば、一覧 `g_fl` を二分探索して「番号がそれ以下の最後の位置」を返す。今と同じく、その位置まで `drain_upto` で待つ。
  4. 一覧の先頭の番号より小さければ（もう終わって外されている）、探さずに `-1`。
- 意味が今と同じか（[推測]）
  - 今：その set を結んだ記録を持つ、いちばん新しい仕事の位置。
  - D2：その set を最後に使った submit の番号の、一覧での位置。同じ仕事を指す。
  - 違いが出るのは、submit のあとで記録し直した場合。今は記録の今の中身を見るが、記録し直す前に、その記録を使う仕事まで待っている（3536 行〜）ので、走っている仕事の記録はもう変わらない。D2 は submit の時点で番号を書くので、こちらの方がむしろ素直。
- 表を空にする時期
  - `VR_FREE_DESCRIPTOR_SETS`・`VR_RESET_DESCRIPTOR_POOL`・`VR_DESTROY_DESCRIPTOR_POOL`・装置を消す：どれも頭で全部待つ命令（`handle` の頭の並び）なので、待ったあとに表を全部空にしてよい。[推測] 空にし忘れても、古い番号は「もう終わった」と分かるので待ちは生まない。害は表が大きくなることだけ。
  - set の番号がドライバで使い回されても、上と同じ理由で害は無い。
- 直す関数：`case VR_QUEUE_SUBMIT`・`last_with_set`・`case VR_FREE_DESCRIPTOR_SETS` ほか表を空にする所。
- 手間：submit のたびに、記録が結んだ set の数だけ表に書く。今の調べ用の `snap` も同じことをしているので、手間の見当はつく。[推測] 1 コマ 7.5 回の submit（report.md）なら、書き込み 5,000 回の探索より軽い。
- 確かめ方
  - `vkr_check()` を入れて、取りこぼしの記録（3377〜3397 行付近）が出ないこと。
  - CTS の `dEQP-VK.binding_model.*`（descriptor の書き込みと使用）、`dEQP-VK.synchronization.*`。
  - わざと壊した版：表を書き込む所を、submit の**前**の番号にずらした版（1 つ前の submit の番号を書く）。取りこぼしの記録か CTS で拾えるか。
  - 速さ：代役の Heaven で、`last_with_set` の ms が消えるか（report.md の見積もりでは案 5 全体で +2〜4 コマ）。
- 戻し方：環境変数（例 `VKR_SETMSG=0`）で今の線形の探し方に戻す。両方を残しておく。
- 難しさ：小〜中。[推測]

### D3：`cbi()` を表引きにする

- [事実] `cbi()` は、記録の番号で `g_cbi` を前から線形に探す。`last_with_set`・`last_with_mem`・`last_with_cb`・記録の処理から呼ばれる。
- やり方：記録の番号から位置への表（ハッシュ）を持つ。記録を放すときに外す。
- 効き目：D2 を入れたあとも、`last_with_mem`・記録の処理が軽くなる。
- 戻し方：環境変数で線形に戻す。

### D4：メモリも同じ形にする（`memwrite_ranges.md` の M4）

- `last_with_mem` も、submit のときにメモリごとに番号を書いておく形にできる。MEM_WRITE・MEM_READ・JPEG の待ちの探索が軽くなる。

## 3. 終わった仕事を一覧から外す処理（`jpeg_async.md` の前提 2）との関係

- **D2 は前提 2 を要らない**：表を引いて二分探索するので、手間は一覧の長さにほとんどよらない。
- **前提 2 は D2 を要らない**：一覧が短くなるだけで、今の線形の探し方も軽くなる。
- **両方入れると**（[推測]）：
  - 表の番号が一覧の先頭より古いことが多くなり、二分探索もせずに「待たない」と分かる。
  - 前提 2 で外した仕事の fence は池に戻るので、受け手の fence の数も増えない。
- 順番の案：前提 2 は小さく単独で効くので先に。D1 → D2 → D3 の順。D2 の確かめで、前提 2 の有り無しの両方で取りこぼしが出ないことを見る。

## 4. 段ごとのまとめ

| 段 | 直す関数 | やり方 | 確かめ方 | 戻し方 |
|---|---|---|---|---|
| D0 | 数えるだけ | `last_with_set` の回数と ms、`g_nfl`・`g_ncbi` | 代役の Heaven | 要らない |
| D1 | `case VR_UPDATE_DESCRIPTOR_SETS` | 同じ知らせの中の同じ set は探さない | `vkr_check()` の取りこぼしが出ない | `VKR_SETCACHE=0` |
| D2 | `case VR_QUEUE_SUBMIT`・`last_with_set`・表を空にする所 | set → 最後に使った submit の番号。一覧を二分探索 | `vkr_check()`・`dEQP-VK.binding_model.*`・番号をずらした壊した版・速さ | `VKR_SETMSG=0` |
| D3 | `cbi()` と記録を放す所 | 記録の番号の表引き | D2 と同じ | 環境変数で線形 |
| D4 | `case VR_QUEUE_SUBMIT`・`last_with_mem` | メモリごとの番号 | MEM_WRITE・MEM_READ・JPEG の CTS | 環境変数で線形 |

## 5. 見つけたこと（範囲の外）

- [推測] UPDATE_AFTER_BIND の set（vkd3d-proton の descriptor heap が多く使う）は、走っている仕事が使っていても、使っていない場所なら書き換えてよい決まり。今の受け手は、その set を使う仕事が終わるまで待つので、vkd3d-proton では待ちすぎになっている可能性がある。D2 でも同じ。直すなら、set の layout の UPDATE_AFTER_BIND の印を見て待たない形になるが、確かめていない。
- [推測] 記録の「使うメモリ」は、set を結んだ時点の set の中身から作る（1281〜1283 行）。UPDATE_AFTER_BIND で後から書き換えた buffer は載らない。`memwrite_ranges.md` の 4 章と同じ話。
