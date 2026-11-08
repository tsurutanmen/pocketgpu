# 案 2「JPEG の GPU 待ちを受け手の主の糸から外す」の設計（2026-10-10）

印：**[事実]** はソースや仕様で確かめたこと。**[推測]** は私の推論。
読んだのは `app\vk\icd\icd.c`・`app\vk\remote\server.c`・`app\vk\remote\proto.h`・`work\perf\report.md` だけ。書き換えていない。
行番号は 2026-10-10 14:49〜15 時台に読んだ時点の物。server.c は読んでいる間にも約 30 行ずれた。関数の名前で探してほしい。

## 0. 要点

- [事実・report.md] 受け手の主の糸は、毎コマ JPEG のために GPU を待つ（3.6〜5.2 ms）。JPEG 用の写しにも 0.8〜1.4 ms かかる。
- [事実] 待っているのは `VR_READ_JPEG_PIPE` の中の `drain_upto(last_with_mem(mid))`。写しは `jp_start` の `memcpy`。どちらも主の糸。
- 段取り：空の submit に**専用の fence**（3 個の輪）を付け、**縮める糸**がその fence を待ってから写して縮める。主の糸は GPU を待たない。
- その前に、2 つの直しが要る。どちらも単独で効く。
  - 前提 1：`VR_RESET_QUERY_POOL` の全部待ちをやめる。今は 1 コマ 44.6 回、出した仕事を全部待っている（report.md）。これが残る限り、主の糸は毎コマどこかで GPU を全部待つので、JPEG だけ外しても効かない。[推測]
  - 前提 2：submit のたびに、終わった仕事を一覧から外す。

## 1. 今の流れ

### ドライバ（icd.c）

- [事実] `jpeg_request`（4861 行〜）：swapchain の画像 j ごとに別の読み戻し用メモリ `sc->rbmem[j]` を使う。先送り（`VR_CAP_JPEG_PIPE`）と、返事を待たない形（`IPHONE_VK_ASYNC_PRESENT`）が既定。前のコマの返事を `rpc_take_deferred` で受け取り、今のコマを `rpc_call_deferred(VR_READ_JPEG_PIPE)` で頼む（4881〜4882 行）。

### 受け手（server.c）

- [事実] `case VR_READ_JPEG_PIPE`（3013 行〜）
  1. `drain_upto(last_with_mem(mid))`：そのメモリを使う最後の submit まで、主の糸で GPU を待つ
  2. `jp_join()`（1969 行〜）：前のコマを縮めている糸が終わるのを待つ
  3. 前のコマの JPEG を返事に入れる
  4. `jp_start()`（1986 行〜）：今のコマを、マップしたメモリから `g_jp.px` に**主の糸で** `memcpy` し、縮める糸を **1 コマごとに作る**（`CreateThread`・`pthread_create`）
  5. 返事を送る
- [事実] `last_with_mem`（931 行〜）は、一覧 `g_fl` を後ろから探し、そのメモリを使う記録を持つ submit の位置を返す。
- [事実] `drain_upto`（781 行〜）は、その位置までの fence を `vkWaitForFences` で待ち、一覧から外す。自前の fence は reset して使い回しの池に戻す。
- [事実] 一覧から外すのは、待ったとき（`drain_upto`）と `VR_GET_PROGRESS`（3866 行〜）だけ。submit では外さない。だから一覧が伸び、64 本（`VKR_MAXFL`、3725 行付近）や 256 本（3756 行）で古い物を待つ。

## 2. 前提 1：`VR_RESET_QUERY_POOL` の全部待ちをやめる

- [事実] 受け手の `handle` の頭（2613 行）で、`VR_DESTROY_QUERY_POOL` と `VR_RESET_QUERY_POOL` の前に `drain()`（全部待つ）をしている。
- [事実] ドライバは query ごとに「最後に書いた submit の番号」を持っている（`qpv_wrote`・`qpv_wmsg`、icd.c 8250〜8276 行付近）。`get_qpool_results` はそれを送り、受け手はその番号まで待つ（`VR_GET_QUERY_POOL_RESULTS` の中）。
- やり方
  1. ドライバの `reset_qpool`（icd.c 8423 行）で、末尾に `qpv_wmsg(ID(qp), first, n)` を足して送る。
  2. 受け手は `VR_RESET_QUERY_POOL` を頭の全部待ちの並びから外し、`case VR_RESET_QUERY_POOL`（4483 行）の中で、末尾に番号があれば（`r->at + 8 <= r->n`）その番号の submit までだけ待つ。無ければ今どおり全部待つ。
- **穴（事実）**：ドライバは、query の結果を buffer に写す命令（`cmd_copy_qpool_results`）を記録しても、`cb_note_q` を呼んでいない。番号が付くのは reset の命令（8435 行）・query の開始（8452 行）・時刻（8491 行）だけ。→ まだ走っている submit が結果を写す前に、CPU からの reset が先に効いてしまう恐れがある。**`cmd_copy_qpool_results` にも `cb_note_q` を足す**のが前提 1 の一部。
- 約束事（proto.h）：caps は要らない。古い受け手は末尾を読まずに全部待つ（今のまま）。新しい受け手と古いドライバは、末尾が無いので全部待つ（今のまま）。`VR_MEM_WRITE` の末尾の番号と同じ足し方（server.c 2952 行〜の `if (r->at + 8 <= r->n)`）。
- [推測] 番号を知らない pool（`qpv_wmsg` が `UINT64_MAX` を返す）なら、全部待つ（安全側）。
- 確かめ方：CTS の `dEQP-VK.query_pool.*`（とくに CPU からの reset と、結果の写しが絡む物）。わざと壊した版：`cmd_copy_qpool_results` の `cb_note_q` を外した版で、試験が落ちるか。
- 戻し方：受け手の環境変数（例 `VKR_QRESET_UPTO=0`）で、全部待ちに戻す。
- 難しさ：小。[推測]

## 3. 前提 2：submit のたびに、終わった仕事を一覧から外す

- やり方：`case VR_QUEUE_SUBMIT`（3644 行〜）の頭で、`VR_GET_PROGRESS` と同じことをする。一覧の頭から `vkGetFenceStatus` を聞き、終わっている所まで `drain_upto(k)` で外す（もう終わっているので待たない）。
- 効き目（推測）：一覧が短くなるので、`last_with_mem`・`last_with_set`（report.md で 0.5〜3.0 ms）・`done_msg` の探す手間が減る。64 本・256 本で古い物を待つことも減る。
- 落とし穴：[推測] submit 1 回ごとに、終わった数ぶん `vkGetFenceStatus` を呼ぶ。1 コマ 7.5 回の submit（report.md）なら軽いと思うが、MoltenVK での重さは測っていない。
- 確かめ方：CTS の `dEQP-VK.synchronization.*`。代役で、主の糸の `last_with_set` の時間が減るか（report.md の測り方）。
- 戻し方：環境変数（例 `VKR_PRUNE=0`）。
- 難しさ：小。[推測]

## 4. 本体：専用の fence を 3 個の輪で使い、縮める糸が待つ

### 仕様の裏付け

- [事実・Vulkan の仕様（同期の章、原文を確認）] 「vkQueueSubmit が決める fence の合図は、そのキューで submit の順にそれより前に出たすべての命令を、最初の同期の範囲に含める」。→ **命令の無い空の submit に fence を付けても、同じキューの前の仕事が全部終わってから立つ。**
  - 出典：https://docs.vulkan.org/spec/latest/chapters/synchronization.html （Fences の節）
- [事実] 受け手は submit どうしを semaphore の鎖でつないで、前の submit が終わってから次が始まるようにしている（server.c の「submit を GPU の上で順に並べる鎖」）。[推測] なので、最後にそのメモリを使った submit のキューに空の submit を出せば、それより前の submit も終わっている。鎖を切る設定で動かすときは、使ったキューごとに空の submit が要る。

### やり方

1. **縮める糸を 1 本、ずっと立てておく**（今は 1 コマごとに作っている）。仕事の受け渡しは、長さ 3 の輪。
2. **専用の fence を 3 個**、装置を作ったときに作る。輪の 1 つの枠に 1 個。主の糸だけが reset と submit をし、縮める糸は待つだけ。
3. `VR_READ_JPEG_PIPE` で、主の糸は次をする（GPU は待たない）
   1. `k = last_with_mem(mid)`。`k >= 0` なら、空いている枠の fence を reset し、`g_fl[k].q` に `vkQueueSubmit(q, 0, NULL, 枠の fence)` を出す。`k < 0` なら、GPU の仕事が残っていないので fence は要らない。
   2. 空いている枠が無ければ、いちばん古い枠が空くまで待つ（今の `jp_join` と同じ押し返し）。
   3. 前のコマの JPEG ができていれば返事に入れる。今と同じく「ひとつ前のコマ」を返すので、前のコマの仕事の終わりだけは待つ（縮めるのに 1 コマぶんの時間があったので、たいていもう終わっている）。[推測]
   4. 今のコマの仕事（枠・メモリの番号・マップした場所・形）を輪に入れ、そのメモリに「縮める糸が読んでいる」印を付ける。
   5. 返事を送る。
4. 縮める糸は、仕事ごとに：枠の fence を待つ → マップしたメモリから写す → 「読んでいる」印を外す → 縮める → できたと知らせる。

### 写し用のメモリの使い回しで、ぶつかる所と防ぎ方

| ぶつかる所 | なぜ | 防ぎ方 |
|---|---|---|
| 次に同じ `rbmem[j]` に GPU が書く submit | 縮める糸が写す前に、GPU が次のコマで上書きする | 主の糸は `VR_QUEUE_SUBMIT` を出す前に、その submit の記録が使うメモリ（`cbinfo` の `mems`）に「読んでいる」印の物があれば、その印が外れるまで待つ。印は最大 3 個なので、比べるのは安い。[推測] |
| `VR_MEM_WRITE` で同じメモリに CPU の中身を書く | 写す途中の中身が混ざる | `case VR_MEM_WRITE`（2952 行〜）の頭で、印があれば外れるまで待つ |
| `VR_FREE_MEMORY`・メモリのマップの外し・装置を消す | 写す途中でマップが消える | それぞれの頭で、印があれば外れるまで待つ。つなぎが切れたときは `jp_reset` で全部待つ |
| `VR_MEM_READ` で同じメモリを読む | 読むだけなので、ぶつからない | 何もしない |
| 縮める糸の `g_jp.px`（写した絵） | 輪に 3 つ入っていても、縮める糸は 1 つずつ写して縮めるので、1 枚で足りる | 写してから縮める順を崩さない。写すのと縮めるのを重ねたいなら、2 枚にする |
| 専用の fence | 主の糸が reset している間に、縮める糸が待つとぶつかる | 枠が空いてから（縮める糸が待ち終えてから）だけ、主の糸が reset する |
| キュー | Vulkan では、同じキューへの submit は外で順番を守る必要がある | submit は主の糸だけ。縮める糸はキューを触らない |

### 直す関数

- 受け手：`case VR_READ_JPEG_PIPE`・`jp_start`・`jp_join`・`jp_thread`・`jp_reset`・`case VR_QUEUE_SUBMIT`・`case VR_MEM_WRITE`・`VR_FREE_MEMORY` と装置を消す所。専用の fence を作る所（装置を作る `VR_CREATE_DEVICE` のあと）と消す所。
- ドライバ：変えない（前提 1 の `reset_qpool` と `cmd_copy_qpool_results` だけ）。
- 約束事：変えない。
- あとで同じ形で：`case VR_READ_H264` と `vp_*`（H.264 の先送り）。

### 確かめ方

- **速さ**：代役の Heaven 1080p。report.md の見積もりは +5〜8 コマ。主の糸の内訳で、READ_JPEG_PIPE（命令の番号 73）の GPU 待ちが消えるか。今は番号 64 以上を数えていないので、report.md の案 8（128 まで数える）を先に入れる。
- **絵が正しいか**：コマごとに違う単色で塗るだけの試験のアプリを用意し、届いた JPEG が全部「1 色だけ」で、色の順が飛ばずに進むかを見る。GPU を重くしても崩れないか。
- **わざと壊した版**：(a) submit の前の「読んでいる」印の待ちを外した版、(b) 専用の fence を待たずに写す版。どちらも試験のアプリで、2 色が混ざった絵か、前のコマの絵が出ることを確かめる（拾えなければ、試験が弱い）。
- **CTS**：本体は描く命令に関係しないので、前提 1・2 の CTS（2・3 章）で足りる。[推測]
- **戻し方**：環境変数（例 `VKR_JPEG_ASYNC=0`）で、今の `drain_upto` と `jp_start` の道に戻す。

### 難しさ

- 中。[推測] 新しく作るのは「縮める糸の輪」と「読んでいる印」。危ないのは、印の待ちを入れ忘れた所で、絵が崩れること（GPU は失わない）。

## 5. 入れる順番

| 段 | 直す関数 | やり方 | 確かめ方 | 戻し方 |
|---|---|---|---|---|
| J1（前提 1） | ドライバ `reset_qpool`・`cmd_copy_qpool_results`、受け手 `handle` の頭・`case VR_RESET_QUERY_POOL` | reset に「最後に使った submit の番号」を付け、そこまでだけ待つ | `dEQP-VK.query_pool.*`・`cb_note_q` を外した壊した版 | `VKR_QRESET_UPTO=0` |
| J2（前提 2） | 受け手 `case VR_QUEUE_SUBMIT` | 終わった頭の仕事を外す | `dEQP-VK.synchronization.*`・`last_with_set` の時間 | `VKR_PRUNE=0` |
| J3 | 受け手 `jp_start`・`jp_join`・`jp_thread` | 縮める糸を立てっぱなしにする（中身はまだ今と同じ） | 速さが落ちないこと | `VKR_JPEG_ASYNC=0` |
| J4 | 受け手 `case VR_READ_JPEG_PIPE`・`VR_QUEUE_SUBMIT`・`VR_MEM_WRITE`・`VR_FREE_MEMORY` ほか | 専用の fence の輪と、読んでいる印 | 単色の試験のアプリ・壊した版 (a)(b)・速さ | `VKR_JPEG_ASYNC=0` |
| J5 | 受け手 `case VR_READ_H264`・`vp_*` | J4 と同じ形 | J4 と同じ | 同上 |

- J1 と J2 は、それだけで主の糸の全部待ちと探す手間を減らす。[推測] 先に入れて測れば、J4 の効き目を分けて見られる。

## 6. 確かめていないこと

- MoltenVK で、命令の無い空の submit に fence を付けたときに、仕様どおり前の仕事の後で立つか（仕様は確かめたが、MoltenVK の実装は見ていない）。
- 受け手の semaphore の鎖が、どの設定でも全部の submit をつないでいるか（鎖を切る設定があるかは見ていない）。
- 本物の iPhone での効き目。report.md は「GPU が 3060 より遅ければ案 2 が効く」と書いている。
