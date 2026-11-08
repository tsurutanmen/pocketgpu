# UPDATE_AFTER_BIND の set と、アドレスで読む buffer の見張りの穴（2026-10-10）

印：**[事実]** はソースで確かめたこと。**[推測]** は私の推論。
読んだのは `app\vk\icd\icd.c`・`app\vk\remote\server.c`・`app\vk\remote\proto.h` と、作業用の場所に落とした vkd3d-proton の原文（master の `libs/vkd3d/device.c`・`state.c`・`resource.c`・`vkd3d_private.h`）。どれも書き換えていないし、実行していない。行番号は 2026-10-10 15 時台に読んだ時点の物。

## 0. 要点

1. [事実] vkd3d-proton は、うちのドライバの上では **descriptor heap を UPDATE_AFTER_BIND の set で作る**。descriptor buffer の拡張をうちが名乗らないので、その道は通らない。
2. [事実] それとは別に、vkd3d-proton は **root の SRV・UAV を、いつも buffer device address（アドレスで直接読む形）で渡す**。root の CBV も、GPU の会社が NVIDIA・Qualcomm でなければアドレスで渡す。本物の iPhone（Apple）では CBV もアドレス。代役（NVIDIA）では CBV は push descriptor。
3. [事実] ドライバも受け手も、「記録が読む・書く buffer」を **set を結んだ時点の set の中身から**作る。ドライバは、アプリが書いたページを submit の前に送るとき、その範囲**だけ**を送る。
4. [推測] 1〜3 から、(a) set を結んだ**あとで** heap に書いた descriptor の buffer と、(b) アドレスで読む buffer は、**ドライバが中身を送らず**、**受け手も待たない**。D3D12 では descriptor を結んだあとに書くのがふつうなので、(a) はまれな形ではない。(b) は root descriptor を使えば必ず起きる。
5. [事実] 一方で、受け手は heap の set に書くたびに、その set を使う走っている仕事を全部待つ。heap の set はどの仕事も結ぶので、[推測] 書くたびにほぼ GPU を空にして待つことになり、待ちすぎ。
6. 直し方は「正しさを先に（多めに送る・多めに待つ）」→「速さ（待ちを減らす）」の順。

## 1. vkd3d-proton は UPDATE_AFTER_BIND の set を本当に使うか

| 段 | 場所 | 事実 |
|---|---|---|
| ドライバが名乗る | `icd.c` 2286〜2310（`mask_ext_features` の Vulkan 1.2 の所、`vkd3d()` のとき） | `descriptorBinding*UpdateAfterBind` を全部、`descriptorBindingUpdateUnusedWhilePending`・`PartiallyBound`・`runtimeDescriptorArray`・`descriptorBindingVariableDescriptorCount`（受け手が `VR_CAP_DESC_FLAGS` のとき）を名乗る。`bufferDeviceAddress` も名乗る（2282 行・`VR_CAP_BDA` のとき） |
| ドライバのコメント | `icd.c` 2270〜2272・2287〜2288 | 「受け手は set を書き換える前に、その set を使う仕事を待つので、使っている間に書き換えるは待つ形で守られる」「binding の flags と確保の数は受け手に運ぶ（`VR_CAP_DESC_FLAGS`）」 |
| descriptor buffer を名乗るか | `icd.c` 全体 | `descriptor_buffer` の文字は 1 つも無い。名乗っていない |
| vkd3d-proton が descriptor buffer を使う条件 | `vkd3d_private.h` 6006〜6009 | `d3d12_device_uses_descriptor_buffers()` ＝ 全体の descriptor buffer のアドレスが 0 でない。それを作るには `VK_EXT_descriptor_buffer` が要る（`device.c` 129・1553・10598） |
| 無いときの heap の set の形 | `state.c` 7503〜7517 | binding の印 `UPDATE_AFTER_BIND_BIT | UPDATE_UNUSED_WHILE_PENDING_BIT | PARTIALLY_BOUND_BIT | VARIABLE_DESCRIPTOR_COUNT_BIT`、layout の印 `UPDATE_AFTER_BIND_POOL_BIT` |
| mutable 型のとき | `state.c` 7790〜7799 付近 | 同じく `UPDATE_AFTER_BIND_POOL_BIT` |
| heap の pool | `resource.c` 9648 | `VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT`（shader から見えない heap で mutable なら HOST_ONLY） |
| root の SRV・UAV | `state.c` 7863〜7865 | 「16 ビットの storage の揃えのため、root の SRV と UAV はアドレスで渡す」→ いつも `VKD3D_RAW_VA_ROOT_DESCRIPTOR_SRV_UAV` |
| root の CBV | `state.c` 7866〜7873 | `FORCE_RAW_VA_CBV` の設定か、会社が NVIDIA・Qualcomm でなければ `VKD3D_RAW_VA_ROOT_DESCRIPTOR_CBV` |
| 会社の番号 | `server.c` 142〜160（`like_ios_*`）・`icd.c` 3447 ほか | 受け手の本物の番号をそのまま返す。代役の `VKR_LIKE_IOS` も会社の番号は変えない。→ 代役は NVIDIA、iPhone は Apple |
| push descriptor | `icd.c` 6453〜6456・6582〜6595 | ドライバがふつうの set に書いて、その場で結ぶ（`update_dsets` → `cmd_bind_sets`）。→ 代役の root CBV は、結んだ時点の中身が新しいので帳簿に載る |

→ **使う**（事実）。iPhone では、root の CBV・SRV・UAV がアドレス、表の descriptor が UPDATE_AFTER_BIND の heap の set。代役では、root の CBV だけが push descriptor。

## 2. 足りなくなる筋書き

### 共通の前提（事実）

- ドライバは、set を結ぶとき、その時点の set の中身を見て「読む範囲」（`cb_reads`）と「書くかもしれない範囲」（`wr_cand`）を記録に足す：`cmd_bind_sets`（`icd.c` 6902〜6926）。
- ドライバがアプリの書いたページを受け手に送るのは、記録の読む範囲・書く範囲だけ：`queue_submit` の頭（`icd.c` 7941〜7954 の `send_dirty`）と `pb_release`（2013〜2014）。`vkFlushMappedMemoryRanges`（2935）と `vkUnmapMemory`（2932）は何もしない。
- 受け手は、記録の中で set を結んだとき、その set の buffer の一覧 `se->list` を記録の「使うメモリ」に足す：`server.c` 1281〜1283。`se->list` は descriptor を書くたびに足すだけで、上書きされた物を外さない（3416・3445・3457 行の `idlist_add`。`idlist_add` は 207〜213 行・重複の確かめは線形）。
- 受け手の MEM_WRITE と MEM_READ は、記録の「使うメモリ」で待つ相手を探す：`last_with_mem`（931〜939）。
- ドライバは、fence にも timeline semaphore にも、合図を出す submit が分かれば往復せずに「届いた」と答える（版 3 の待ち方）。timeline：`wait_sems`（`icd.c` 7862〜7885）。vkd3d-proton は D3D12 の fence を timeline semaphore で作る。

### 筋書き 1：結んだあとに書いた descriptor の buffer の中身が送られない（ドライバ）

1. アプリ（vkd3d-proton）が記録を始め、heap の set を結ぶ → `cmd_bind_sets`（6902〜）が、その時点の heap の中身だけを読む範囲にする。
2. アプリが heap の空いた所に、upload heap（CPU から見えるメモリ）の buffer B の CBV か SRV を書く → `update_dsets` → 受け手へ `VR_UPDATE_DESCRIPTOR_SETS`。B は記録の読む範囲に載らない。
3. アプリが B に、このコマの値を CPU で書く（マップしたメモリ ＝ ドライバの写しのページに印が付く）。
4. 描く命令を記録して、submit → `queue_submit`（7938〜）は、読む範囲・書く範囲のページだけを送る。**B のページは送られない。**
5. 受け手の GPU は、B の古い中身（前に送られた中身か、何も送られていなければ初めの中身）を読む。
- [推測] D3D12 のエンジンは、コマごとに heap の輪の中に descriptor を書いてから表を指すので、1 と 2 の順（結んでから書く）はふつう。vkd3d-proton は heap の set を記録の頭で結び、表の位置は push constant で渡す作りなので、なおさら。

### 筋書き 2：結んだあとに書いた descriptor の buffer を、受け手が待たずに上書きする（受け手）

1. コマ N：筋書き 1 の 1〜2 で、記録の「使うメモリ」（受け手の `cbinfo->mems`）に B が載らない（`server.c` 1281〜1283 は結んだ時点の `se->list`）。
2. コマ N の submit → 受け手の一覧に入る。
3. アプリが D3D12 の fence を待つ → ドライバの `wait_sems` は往復せずに「届いた」と答える（GPU はまだコマ N を描いている）。
4. アプリがコマ N+1 の値を B に書き、何かのついでに B のページが送られる（B が別の記録の読む範囲に載ったとき）→ 受け手の `VR_MEM_WRITE`（2952〜）は `last_with_mem(B のメモリ)` で待つ相手を探すが、コマ N の記録に B は無いので**待たない**。
5. GPU がまだ読んでいる B を上書きする → コマ N の絵に、コマ N+1 の値が混ざる。

### 筋書き 3：アドレスで読む buffer は、送られず、待たれず、読み戻されない

1. アプリが root の CBV（iPhone）・SRV・UAV（代役と iPhone）で buffer B を指す → vkd3d-proton は B のアドレスを push constant などで渡す。どの set にも載らない。
2. ドライバの `cmd_bind_sets` にも `cb_reads` にも B は出てこない → **B のページは submit の前に送られない**（筋書き 1 の 4 と同じ）。
3. 受け手の記録の「使うメモリ」にも B は無い → **MEM_WRITE も MEM_READ も B を待たない**（筋書き 2 の 4 と同じ）。
4. root の UAV で、CPU から見えるメモリ（D3D12 の CUSTOM heap など）に GPU が書いた場合、書く範囲に載らないので**読み戻されない**（`complete`、`icd.c` 2081〜 は記録の書く範囲だけを引き取る）。
- [事実] ドライバのコメント（`icd.c` 2280〜2281）も「shader がアドレスでたどって書いた中身は、ドライバの写しの見張りに入らない」と書いている。読む側（送られない）は書かれていない。
- [推測] upload heap の定数を root の CBV で読むのは、D3D12 でいちばんよくある形の 1 つ。iPhone では root の CBV がアドレスなので、ここが壊れると、多くのゲームで定数（カメラの行列など）が古いまま描かれる。代役では root の CBV が push descriptor なので壊れない。**代役で動いて iPhone で崩れる**形になりうる。

## 3. 待ちすぎている所

| 所 | 場所 | 何が起きるか |
|---|---|---|
| heap の set に書くたびに待つ | `server.c` 3368〜（`drain_upto(last_with_set(sid))`、3398 付近） | [事実] 書き込みごとに、その set を結んだ走っている仕事まで待つ。[推測] heap の set はどの記録も結ぶので、ほぼ「出した仕事を全部待つ」になる。vkd3d-proton は毎コマたくさん descriptor を書くので、受け手の主の糸が何度も GPU を空にして待つ |
| `se->list` が減らない | `server.c` 207〜213・3416・3445・3457 | [事実] 上書きされた buffer も一覧に残る。[推測] 記録の「使うメモリ」が、もう heap に無い buffer まで含み、MEM_WRITE がそれらで待つ。一覧が長いほど `idlist_add` の重複の確かめ（線形）と、結ぶときの `use_buffer` が重くなる |
| ドライバが heap の set を結ぶたびに全部を見る | `icd.c` 6913〜6925 | [事実] 結ぶたびに set の書かれた項目を全部回す。[推測] heap が大きいと、結ぶたびに重い |

- 待つ理由（事実）：ドライバが fence と timeline に早めに答えるので、アプリが「終わった」と思って書き換える物を、受け手が守る必要がある（`icd.c` 2270〜2272 のコメント）。
- [事実・Vulkan の決まり] UPDATE_UNUSED_WHILE_PENDING の binding は、走っている記録が**使わない** descriptor なら書き換えてよい。D3D12 でも、アプリは使っている descriptor を GPU が終わるまで書き換えない決まり。→ 待つ必要があるのは「アプリの待ちに、ドライバが早めに答えたから」だけ。待ちを本物の進み具合で答えれば、heap の set の書き込みは待たなくてよい。[推測]

## 4. 直し方の案と段取り

### U0：壊れを見せる試験を先に作る

- 試験 A（アドレスの root SRV。代役でも iPhone でも起きるはず）：upload heap の小さな buffer に、コマごとに違う色を CPU で書き、root SRV（ByteAddressBuffer か StructuredBuffer）で読んで画面を塗る。届いた JPEG の色が、コマごとに変わるかを見る。[推測] 今は変わらない（初めの色のまま）か、遅れて変わる。
- 試験 B（root CBV）：A と同じで root CBV。代役では正しく、iPhone では崩れるはず。代役で iPhone と同じ形を作るには、vkd3d-proton の `FORCE_RAW_VA_CBV` の設定を使う（原文の `state.c` 7870 で印は見つけたが、`VKD3D_CONFIG` の中での名前は見つからなかった）。見つからなければ、受け手の代役に「会社の番号を Apple と答える」調べ用の切り替えを足す手がある。
- 試験 C（heap を結んだあとに書く）：コマごとに、heap の新しい位置に upload buffer の CBV を書いてから、その表で描く。色がコマごとに変わるかを見る。
- 試験 D（読み戻し）：root UAV で CPU から見える buffer に GPU が数を書き、fence を待ってから CPU で読む。
- d3d12test で再現できるか：d3d12test の中身は読んでいないので分からない。上の A〜D のどれかの形（とくに root CBV か root SRV で upload の値を毎コマ読む）を含んでいれば、iPhone で崩れが見えるはず。含んでいなければ、A〜D を d3d12test に足すのが早い。[推測]
- **ここは「わざと壊す」のではなく、今の作りがすでに壊れているかを確かめる試験**。今の作りで A〜D が落ちることを先に確かめ、直したあとに通ることを確かめる。

### U1：ドライバ：アドレスで使うメモリと heap の set のページを、多めに送る（正しさを先に）

- やり方
  1. CPU から見えるメモリのうち、`VkMemoryAllocateFlagsInfo` に `DEVICE_ADDRESS` の印が付いた物を覚える（確保の所 `alloc_memory`、2835〜。印を受け手に運ぶ所は 2862）。
  2. `queue_submit` の頭で、記録の読む範囲に加えて、覚えたメモリの書かれたページを**全部**送る（`send_dirty` をメモリ全体に）。
  3. heap の set（UPDATE_AFTER_BIND の layout）を結んだ記録は、submit の時点で set の今の中身を見直して、読む範囲・書く範囲を足す（`cmd_bind_sets` が結んだ時点で見るのをやめ、submit で見る）。
- 直す関数：`alloc_memory`・`queue_submit`・`cmd_bind_sets`・`wr_resolve`（書く範囲を submit で決める所、6877〜）。
- 落とし穴：[推測] 2 は、upload heap が大きいと、submit のたびに `GetWriteWatch` でメモリ全体を調べる手間が増える。今の `collect_dirty`（1270〜）の重さを M0 のように先に測る。読み戻し（筋書き 3 の 4）は、これでは直らない。CPU から見えるメモリに GPU がアドレスで書く形はまれなので、まず記録に出すだけにする手もある。
- 確かめ方：U0 の A・B・C が通る。速さが落ちすぎないか（代役の Heaven は DX11 なので関係しない。DX12 の物で測る）。
- 戻し方：環境変数（例 `IPHONE_VK_BDA_SYNC=0`）で今の送り方に戻す。
- 難しさ：中。[推測]

### U2：受け手：記録の「使うメモリ」を submit の時点で作る

- やり方
  1. `case VR_QUEUE_SUBMIT` で、記録が結んだ set（`cbinfo->sets`）の**今の** `se->list` を見て、使うメモリを作り直す。
  2. アドレスで使うメモリ（DEVICE_ADDRESS の印。受け手は `VR_ALLOC_MEMORY` の末尾の flags で知っている）は、どの submit も使うことにする（多めに待つ）。
  3. `descriptor_tracking.md` の D4（メモリごとに最後に使った submit の番号）と組むと、submit のときに番号を書くだけで済む。
- 直す関数：`case VR_QUEUE_SUBMIT`・`last_with_mem`・`case VR_ALLOC_MEMORY`。
- 確かめ方：U0 の C と、筋書き 2 の形（コマ N+1 の値が混ざる）。
- 戻し方：環境変数で、結んだ時点の一覧に戻す。

### U3：受け手：`se->list` を「今の中身」にする

- やり方：set ごとに「位置 → buffer」の表を持ち、上書きのときは前の buffer を外す。一覧の重複の確かめも表引きにする。
- 効き目：記録の「使うメモリ」が正確になり、MEM_WRITE の待ちすぎが減る。書き込みの手間も一定になる。
- 直す関数：`case VR_UPDATE_DESCRIPTOR_SETS` の `idlist_add` の 3 か所・`idlist_add`。
- 戻し方：環境変数で今の一覧に戻す。

### U4：待ちすぎを減らす（アプリの待ちを本物にしてから）

- やり方
  1. vkd3d-proton のときは、fence と timeline の待ちを本物の進み具合で答える（`wait_sems` の往復しない道を、REAL_FENCE と同じく進み具合で確かめる形に）。往復が増えるので、`pipeline_lock.md` の P3・P4（返事に終わった番号を付ける・受け手の中で短く待つ）を先に入れる。
  2. そのうえで、受け手は UPDATE_UNUSED_WHILE_PENDING の binding への書き込みでは待たない（binding の印は `VR_CAP_DESC_FLAGS` で受け手に届いている）。
- 守ること：アプリの待ちが**全部**本物になってからでないと、筋書き 2 と同じ壊れ方を、descriptor の側で起こす。
- 確かめ方：U0 の C。CTS の `dEQP-VK.descriptor_indexing.*`（update after bind の物）・`dEQP-VK.binding_model.*`・`dEQP-VK.synchronization.timeline_semaphore.*`。わざと壊した版：1 をせずに 2 だけ入れた版で、C が落ちることを確かめる（試験が拾えるか）。
- 戻し方：環境変数で今の待ち方に戻す。
- 難しさ：中〜高。[推測]

## 5. 段ごとのまとめ

| 段 | 直す関数 | やり方 | 確かめ方 | 戻し方 |
|---|---|---|---|---|
| U0 | 試験を作る（d3d12test に足すか、小さな見本） | root SRV・root CBV・結んだあとの heap・読み戻しの 4 つ | 今の作りで落ちることを先に確かめる | 要らない |
| U1 | ドライバ `alloc_memory`・`queue_submit`・`cmd_bind_sets`・`wr_resolve` | アドレスで使うメモリのページを全部送る。heap の set は submit で見直す | A・B・C が通る。DX12 の物で速さ | `IPHONE_VK_BDA_SYNC=0` など |
| U2 | 受け手 `case VR_QUEUE_SUBMIT`・`last_with_mem`・`case VR_ALLOC_MEMORY` | 使うメモリを submit で作る。アドレスのメモリはいつも使う扱い | C と筋書き 2 の形 | 環境変数 |
| U3 | 受け手 `case VR_UPDATE_DESCRIPTOR_SETS`・`idlist_add` | set の中身を「今の中身」に | MEM_WRITE の待ちが減る | 環境変数 |
| U4 | ドライバ `wait_sems`・fence の待ち、受け手 `case VR_UPDATE_DESCRIPTOR_SETS` | アプリの待ちを本物にしてから、UNUSED_WHILE_PENDING には待たない | C・CTS・1 を抜いた壊した版 | 環境変数 |

順番の考え方：[推測] U0 で今の壊れを確かめる → U1・U2 で正しさを取る（多めに送る・多めに待つ。遅くなってもよい）→ U3・U4 で速さを戻す。

## 6. 確かめていないこと

- 実際の DX12 のゲームや d3d12test で、筋書き 1〜3 が今起きているか（U0 で確かめる）。
- vkd3d-proton の `FORCE_RAW_VA_CBV` の設定の、`VKD3D_CONFIG` での名前。
- vkd3d-proton が heap の set を記録のどこで結ぶか（`command.c` の該当の所は今回読んでいない）。筋書き 1 の 1 と 2 の順は、D3D12 の一般の使い方からの推測。
- U1 の「メモリ全体を調べる」重さ。
