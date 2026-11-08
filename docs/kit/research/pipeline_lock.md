# 案 4「ドライバが待つ間に g_lock を放す」の設計（2026-10-10）

印：**[事実]** はソースで確かめたこと。**[推測]** は私の推論。
読んだのは `app\vk\icd\icd.c`・`app\vk\remote\server.c`・`app\vk\remote\proto.h`・`work\perf\report.md` だけ。書き換えていない。
行番号は 2026-10-10 14:49 に読んだ時点の物。server.c は読んでいる間にも約 30 行ずれた（別の係が直しているらしい）。関数の名前で探してほしい。

## 0. 要点

- [事実] ドライバは、返事の要る命令の往復の間ずっと全体の錠 `g_lock` を持つ（`rpc_call`）。送るだけの命令（`rpc_send`）も同じ錠を取るので、DXVK の記録の糸も、往復が終わるまで止まる。
- [事実] 受け手は 1 本の糸で、届いた順に命令を処理し、返事も頼まれた順に返す（`serve_client` と `handle`）。返事を送るのは `reply()` の 1 か所だけ（server.c 1162 行）。
- → 返事は「頼んだ順」に必ず来る。なので、**錠を持ったまま待たなくても、頼んだ順の番号（札）で返事を受け取れる**。[推測] これが案 4 の土台になる。
- 段取りは 4 段。1 段ごとに、環境変数で前のやり方に戻せるようにする。
  - P1：返事を受け取る専用の糸を立て、待つ糸は錠を持たずに自分の札の返事を待つ（約束事は変えない）
  - P2：送る専用の糸を立て、LZ4 と送信をアプリの糸から外す（約束事は変えない）
  - P3：受け手が、どの返事にも「終わった submit の番号」を付ける（約束事に足す。caps で守る）
  - P4：GET_PROGRESS の往復をさらに減らす（約束事に足す。caps で守る）

## 1. 今の錠の持ち方と、待つ所（ドライバ・icd.c）

| 場所 | 行 | 錠 | 何をするか |
|---|---|---|---|
| `g_lock` | 230 | — | 送る列 `g_q`・ソケットの送りと受け・`g_defer`・数えの `g_stat` を、全部 1 つで守る |
| `flush_locked` | 255〜280 | 持って呼ぶ | 1 KB 以上なら LZ4 で縮めて送る。送るのはこの糸 |
| `put_msg_locked` | 282〜287 | 持って呼ぶ | 送る列に積むだけ |
| `rpc_send` | 293〜301 | 短く取る | 積む。8 MB を超えたら、その場で送る。`VR_QUEUE_SUBMIT` なら `g_submit_msgs` を 1 つ進める |
| `rpc_flush` | 303〜307 | 取る | 送る列を送る（LZ4 も）。`queue_submit` の最後（8086 行）から、`d->lock` を持ったまま呼ぶ |
| `recv_head` | 355〜393 | 持って呼ぶ | 返事の頭を待つ。1 秒ごとに起きて、3 秒（GPU を待つ命令は 20 秒）で帯を出す |
| `read_deferred_locked` | 395〜408 | 持って呼ぶ | あとで受け取る返事（窓の絵）を、ほかの返事より先に読んで取っておく |
| `rpc_call_deferred`・`rpc_take_deferred` | 411〜438 | 取る | 窓の絵の返事を頼む・受け取る。受け取るときは、届くまで錠を持って待つ |
| `rpc_call` | 440〜467 | **往復の間ずっと持つ** | 積む → 送る → 前の窓の絵の返事を読む → 自分の返事を読む |
| `lz_read` | 1446〜1489 | **往復の間ずっと持つ** | 遅らせた読み戻しの、MEM_READ をいくつも頼んで全部受け取る |
| `read_back_many` | 1909〜1997 | **往復の間ずっと持つ** | GPU が書いた所の読み戻し。MEM_READ（と DELTA）をいくつも頼んで全部受け取る。受け取りは写し（shadow）に直接書く |
| `progress_poll` | 2058〜2067 | `rpc_call` 経由 | `VR_GET_PROGRESS` の往復 |
| `progress_wait` | 2070〜2079 | — | 番号に届くまで `progress_poll` を繰り返す。4 回目から 1 ms ずつ眠る |

- [事実] 返事の要る命令（`rpc_call` の呼び出し）：WAIT_FOR_FENCES・QUEUE_WAIT_IDLE・GET_QUERY_POOL_RESULTS・GET_FENCE_STATUS（各 2 か所）、WAIT_SEMAPHORES・READ_JPEG・READ_H264・HELLO・GET_STATUS・GET_SEMAPHORE_COUNTER・GET_PROGRESS・GET_IMAGE_SUBRESOURCE_LAYOUT・GET_IMAGE_FORMAT_PROPS・GET_FORMAT_PROPS・GET_BUFFER_ADDRESS・DEVICE_WAIT_IDLE・CREATE_IMAGE・CREATE_GRAPHICS_PIPELINE・CREATE_DEVICE・CREATE_COMPUTE_PIPELINE・CREATE_BUFFER・ALLOC_MEMORY（各 1 か所）。ほかに `rpc_call_deferred` の READ_JPEG_PIPE（4882 行）と READ_H264（5235 行）、`lz_read` と `read_back_many` の MEM_READ・MEM_READ_DELTA。
- [事実] 進み具合を待つ所：`wait_fences` の 7704〜7714 行（`progress_wait`）、`fence_status` の 7757〜7760 行（`progress_poll`）、`queue_wait_idle`（8097 行）、`device_wait_idle`（8112 行）。
- [事実] `heat_check`（5347 行）は 2 秒ごとに `VR_GET_STATUS` を同期の往復で聞く。
- [事実] `queue_submit` は `rpc_send` のあと、錠を持たずに `g_submit_msgs` を読んで fence の番号にする（8073 行）。別の装置の糸が間に submit を送ると、自分の番号より大きい値を読む。[推測] 待つのが長くなるだけで、早く「終わった」と答えることは無い。

### 受け手の側（server.c）

- [事実] `serve_client`（4546 行〜）が、1 本の糸で命令を読んで `handle`（2574 行〜）に渡す。LZ4 の塊（VR_PACKED）も中の命令を順に処理する。
- [事実] 返事は `reply()`（1162 行）だけが送る。頭は `[結果 u32][長さ u32]` の 8 バイト。
- [事実] `VR_GET_PROGRESS`（3866 行〜）は、終わった頭の方の仕事を一覧から外し、「終わった submit の番号」を返す。

### なぜ交互にしか動けないか

- [事実・report.md] ドライバは 1 コマ 18〜20 ms を受け手待ちで過ごす。
- [推測] 待っている間 `g_lock` を持つので、DXVK の記録の糸が `rpc_send` で止まる。受け手が返事を作っている間（GPU を待つ命令なら数 ms）、ドライバは次の命令を 1 つも送れない。受け手は返事を送ったあと、次の命令が届くまで空く。本物の iPhone では往復が 1 回 1 ms（report.md）あるので、この空きが 1 回ごとに増える。

### 今もある、詰まりの危なさ（推測）

- `rpc_send` は 8 MB を超えると、窓の絵の返事を読まずに送る（flush_locked だけ呼ぶ）。受け手がちょうど大きな窓の絵の返事（4K で 1 MB 超）を送っている最中だと、受け手は送りで止まり、ドライバも送りで止まる。ソケットの受け皿が小さいと、両方が止まったまま動かない形がありうる。今まで起きていないのは、受け皿が足りているからだと思う。P1 の「返事を受け取る専用の糸」は、これも消す。

## 2. 段取り

### P0：先に測る（直さない）

- 直す関数：`rpc_send`・`rpc_flush`・`rpc_call` などで `g_lock` を取る所すべて。
- やり方：錠を取るまで待った時間と、持っていた時間を、命令ごとに `g_stat` に足す（`IPHONE_VK_STATS=1` のとき）。
- 確かめ方：代役の Heaven 1080p で、錠を待った時間の合計が 1 コマ何 ms かを出す。P1 以降の効き目の基準にする。
- 戻し方：数えるだけなので要らない。

### P1：返事を受け取る専用の糸（約束事は変えない）

- やり方
  1. 送る錠（今の `g_lock` の役のうち、`g_q` とソケットの送りだけ）と、返事の受け取りを分ける。
  2. 返事の要る命令を積むとき、送る錠の中で**札**（返事の要る命令の通し番号）を振り、「札・命令の番号・返事の置き場」を待ち行列に足す。札は積んだ順に振るので、受け手が返す順と同じになる。
  3. 返事を受け取る専用の糸を 1 本立てる。ソケットから返事を 1 つずつ読み、待ち行列の先頭の札の置き場に入れて、待っている糸を起こす。
  4. 待つ糸は、送る錠を放してから、自分の札の置き場が埋まるまで待つ（条件変数）。
  5. 窓の絵（`g_defer`）は、ただの札の 1 つになる。`read_deferred_locked` は要らなくなる。
  6. MEM_READ の返事は、置き場に「書き込み先」（写しの場所）を持たせ、受け取る糸が直接そこに書く。DELTA は今と同じく受け取ってからほどく。
- 直す関数：`rpc_call`・`rpc_call_deferred`・`rpc_take_deferred`・`read_deferred_locked`・`lz_read`・`read_back_many`・`recv_head`（帯の判断は受け取る糸に移す。先頭の札の命令で、3 秒か 20 秒かを決める）・`conn_lost`（待っている札を全部失敗で起こす）。
- 確かめ方
  - 代役で Heaven 1080p の速さと、P0 の錠の待ち時間。
  - CTS（2 章の「確かめ方」）。
  - わざと壊した版：受け取る糸が札を 1 つずらして渡す版を作り、試験が落ちるかを見る（落ちなければ、試験が返事の取り違えを拾えていない）。
- 戻し方：環境変数（例 `IPHONE_VK_PIPE=0`）で、今の関数をそのまま使う道に戻す。P1 のあいだは今の関数を消さずに残す。
- 難しさ：中〜高。[推測]

### P2：送る専用の糸（約束事は変えない）

- やり方
  1. `rpc_send` は送る列に積むだけにする。`rpc_flush` は「送る糸を起こす」だけにして、待たない。
  2. 送る糸は、送る列を 2 枚で入れ替えながら（積む側と送る側）、LZ4 で縮めて送る。
  3. 送る列が大きくなりすぎたら（今の 8 MB）、積む側を待たせる（押し返し）。
- 直す関数：`rpc_send`・`rpc_flush`・`flush_locked`・`put_msg_locked`。`queue_submit` の最後の `rpc_flush`（8086 行）は `d->lock` を持ったまま呼んでいるので、待たなくなれば `d->lock` を持つ時間も短くなる。
- 守ること
  - **命令の順番**：積む順 ＝ 送る順。送る列は 1 本だけにする。
  - **札と送る順**：P1 の札は、積むときに振る（送るときではない）。
  - **`g_submit_msgs`**：今どおり積むときに進める。`rpc_send` が振った番号を返すようにすると、`queue_submit` が錠の外で読み直す（8073 行）必要が無くなる。[推測]
  - **送り損ねたとき**：送る糸で `conn_lost` を呼び、待っている札を全部失敗で起こす。
- 確かめ方：P1 と同じ。わざと壊した版：送る糸が 2 枚の列の順を入れ替える版で、試験が落ちるか。
- 戻し方：環境変数で、アプリの糸で送る今の道に戻す。
- 難しさ：中。[推測]

### P3：返事に「終わった submit の番号」を付ける（約束事に足す）

- やり方
  1. 受け手は caps に `VR_CAP_REPLY_TAIL` を名乗る。
  2. ドライバは、受け手がそれを名乗ったときだけ、HELLO のあとに `VR_SET_OPTIONS`（印 `VR_OPT_REPLY_TAIL`）を送る。
  3. 受け手は、それを受けたつなぎでだけ、`reply()` で返事の中身の後ろに 16 バイトの尾を付ける：「終わった submit の番号（u64）」「返事の通し番号（u32）」「空き（u32）」。返事の長さ（頭の 2 つ目）は尾を含める。
  4. ドライバの受け取る糸は、尾を外してから置き場に渡す。終わった番号で `g_done_msg` を進める。通し番号が札とずれていたら、返事の取り違えなので切る（`conn_lost`）。
  5. `fence_status`（7757 行）と `progress_wait`（2070 行）は、まず `g_done_msg` を見る。ほかの返事のついでに進んでいれば、往復しない。
- 受け手の「終わった番号」の出し方：`done_msg()`（842 行）は、一覧の頭から fence を順に聞く。[推測] 一覧が短ければ安い。JPEG の設計の前提 2（submit のたびに終わった仕事を一覧から外す。`jpeg_async.md`）を先に入れると、ここも安くなる。
- 直す関数：受け手の `reply()`・`handle` の `VR_SET_OPTIONS`（新しい case）・`serve_client`（つなぎごとに印を消す）。ドライバの受け取る糸・`hello`（906 行で caps を読む所の後）・`fence_status`・`progress_wait`。
- 確かめ方：本物の iPhone か、遅い回線の真似（3 章）で、1 コマの GET_PROGRESS の回数が減ったか（`IPHONE_VK_STATS` の calls）。わざと壊した版：受け手が通し番号を 1 つずらす版で、ドライバが切ることを確かめる。
- 戻し方：ドライバが `VR_SET_OPTIONS` を送らなければ、受け手は今のまま。環境変数でドライバに送らせない。
- 難しさ：中。[推測]

### P4：GET_PROGRESS の往復をさらに減らす（約束事に足す）

- 問題：P3 のあとも、ほかの返事が流れていないときに fence を待つと、`progress_wait` は往復を繰り返す（4 回目からは 1 ms ずつ眠りながら）。
- やり方：受け手が `VR_CAP_WAIT_PROGRESS` を名乗ったら、ドライバは `VR_WAIT_PROGRESS`（番号・最大の待ち ms）を送る。受け手は、その番号の submit の fence を、最大の待ち（例 4 ms）まで受け手の中で待ってから、終わった番号を返す。往復は 1 回で済む。
- 守ること（重要）
  - **受け手は 1 本の糸**なので、待っている間はほかの命令を処理できない。ほかのアプリの糸の命令が後ろで待つ。だから待ちは短く区切る（数 ms）。[推測]
  - **CPU からの合図を待つ submit**（timeline の値を `vkSignalSemaphore` で後から立てる物）を待つと、その合図の命令が後ろで止まり、永久に終わらない。受け手は `mark_blocked`（server.c 743 行〜）で、止まっている submit を見分けられる。止まっている物は待たずにすぐ返す。
- 直す関数：受け手の `handle` に新しい case。ドライバの `progress_wait`。
- 確かめ方：CTS の timeline の試験（3 章）。わざと壊した版：最大の待ちを無くした版で、timeline の試験が止まる（詰まる）ことを確かめる。
- 戻し方：ドライバが送らなければ今のまま。
- 難しさ：中。[推測]

## 3. 確かめ方

- **CTS の範囲**（[推測] 選び方の理由つき）
  - `dEQP-VK.synchronization.*`（とくに `basic.*`・`timeline_semaphore.*`・`op.single_queue.*`）：fence・semaphore・submit の番号の約束
  - `dEQP-VK.query_pool.*`：GET_QUERY_POOL_RESULTS の待ちと、submit の番号
  - `dEQP-VK.memory.mapping.*`：MEM_WRITE と読み戻しの順
  - `dEQP-VK.api.object_management.multithreaded_*`：いくつもの糸から同時に命令を出す形
  - `dEQP-VK.api.command_buffers.*`：記録と submit
- **遅い回線の真似（slow_link）**：`app\vk\tools\slow_link.py` にある（2026-10-10 に教えてもらって追記）。[事実] 片道に rtt/2 ms の遅れを足し、向きごとに 1 秒あたりの量の上限を付ける中継。例：`python slow_link.py --listen 50071 --to 50070 --rtt 1 --mbps 80`（USB くらい）、`--rtt 31 --mbps 20`（Wi-Fi と Tailscale くらい）。ドライバを `--listen` の口につなぐ。
  - [推測] P1〜P4 の効き目は往復の長さで変わるので、`--rtt 0`（代役のまま）・`--rtt 1`・`--rtt 31` の 3 つで測ると、本物の iPhone での効き目を見積もれる。
  - [推測] Windows のタイマーは粗いので、道具は最後の数 ms を回って待つ（道具のコメント）。CPU を 1 つ食うので、速さを測るときは、その分を差し引いて考える。
- **わざと壊した版**：各段の「わざと壊した版」で、試験が壊れを拾えるかを先に確かめる（拾えない試験で「通った」と言わない）。
- **速さ**：代役の Heaven 1080p（report.md と同じ測り方）と、本物の iPhone。P1・P2 の効き目は、代役より iPhone のほうが大きいはず（往復が長いので）。[推測]

## 4. 守るべき約束（まとめ）

1. 命令の順番：アプリが呼んだ順に、受け手が処理する。送る列は 1 本。
2. 返事の対応：返事は頼んだ順に来る。札は積むときに振る。P3 の通し番号で取り違えを見つける。
3. fence と submit の番号：fence に付ける番号は、その submit の番号以上（小さくしない）。小さいと、終わる前に「終わった」と答えてしまう。
4. MEM_WRITE と読み戻し：MEM_WRITE は submit の前に送る列に入る（`queue_submit` の頭の `send_dirty`）。読み戻しの MEM_READ は、それを使う submit より後に送る列に入る。送る列が 1 本なら崩れない。受け取りが別の糸になっても、送る順は変わらない。
5. 写しへの書き込み：読み戻しの返事は、受け取る糸が写し（shadow）に書く。今もアプリの糸は写しを錠なしで書いている（マップしたメモリ）ので、ぶつかり方は今と同じ。[推測]
6. 切れたとき：待っている札を全部失敗で起こす。送る糸と受け取る糸の両方を止める。

## 5. proto.h に足す物（案）

```c
#define VR_CAP_REPLY_TAIL 2097152u      // VR_SET_OPTIONS の VR_OPT_REPLY_TAIL を受けたつなぎで、どの返事にも 16 バイトの尾を付ける：
                                        // 終わった submit の番号（u64）・返事の通し番号（u32・0 から）・空き（u32）。返事の長さは尾を含む
#define VR_CAP_WAIT_PROGRESS 4194304u   // VR_WAIT_PROGRESS を受ける
#define VR_OPT_REPLY_TAIL 1u
// enum vr_op の VR_GET_PROGRESS の後ろ（VR_OP_COUNT の前）に足す
    VR_SET_OPTIONS,   // 印（u32）。返事なし。このつなぎの間だけ効く（VR_CAP_REPLY_TAIL）
    VR_WAIT_PROGRESS, // [同期] submit の番号（u64）・最大の待ち（ms・u32）→ 終わった submit の番号（u64）（VR_CAP_WAIT_PROGRESS）
```

- 古い受け手と新しいドライバ：受け手が caps を名乗らないので、ドライバは送らない。今のまま。
- 新しい受け手と古いドライバ：ドライバが `VR_SET_OPTIONS` を送らないので、受け手は尾を付けない。今のまま。
- caps の番号は、今使っている一番大きい物（`VR_CAP_PROGRESS` = 1048576）の次から。
- `VR_PROTO_VERSION` は上げない（今の約束事も、新しい物は caps で足す形）。

## 6. 段ごとのまとめ

| 段 | 直す関数 | やり方 | 確かめ方 | 戻し方 |
|---|---|---|---|---|
| P0 | `g_lock` を取る所すべて | 錠の待ち時間と持つ時間を数える | 代役の Heaven で基準を取る | 要らない |
| P1 | `rpc_call`・`rpc_call_deferred`・`rpc_take_deferred`・`read_deferred_locked`・`lz_read`・`read_back_many`・`recv_head`・`conn_lost` | 返事を受け取る専用の糸。札で受け取る | CTS・代役の速さ・札をずらす壊した版 | 環境変数で今の道 |
| P2 | `rpc_send`・`rpc_flush`・`flush_locked`・`put_msg_locked` | 送る専用の糸。2 枚の列 | CTS・速さ・列の順を入れ替える壊した版 | 環境変数で今の道 |
| P3 | 受け手の `reply()`・`handle`・`serve_client`、ドライバの受け取る糸・`hello`・`fence_status`・`progress_wait` | 返事に終わった番号と通し番号の尾を付ける | GET_PROGRESS の回数・通し番号をずらす壊した版 | ドライバが VR_SET_OPTIONS を送らない |
| P4 | 受け手の `handle`、ドライバの `progress_wait` | 受け手の中で短く待つ VR_WAIT_PROGRESS | timeline の CTS・待ちの上限を無くした壊した版 | ドライバが送らない |

## 7. 確かめていないこと

- P1〜P4 の効き目の大きさ。report.md は案 4 全体で「代役 55〜70 コマ」と見積もっているが、段ごとの分け前は分からない。
- 受け手の `done_msg()` を、どの返事にも付けたときの重さ（一覧が長いと fence を何度も聞く）。
- MoltenVK で、別の糸から同時に Vulkan を呼んでよい組み合わせ（受け手は P1〜P4 でも 1 本の糸のままなので、今回は関係しない）。
