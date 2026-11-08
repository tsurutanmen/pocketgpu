# sparse を真似る設計の下調べ（2026-10-10）

印：**[事実]** はソースや一次の文書で確かめたこと。**[推測]** は私の推論。外の例の多くは調べ物の担当が要約器経由で読んだもので、その旨を書く。
うちのソースは読むだけで、書き換えていない。行番号は 2026-10-10 13 時台の作業コピーのもの。

## 0. 今のうちの状態

- [事実] 機能はすべて「無い」と答える：`mask_features`（`icd.c` 852〜854 行）で sparseBinding・sparseResidency 系を偽にしている。
- [事実] sparse の性質（`sparseProperties`）はドライバで直していない。受け手（MoltenVK）の答えのまま。MoltenVK は sparse を 1 つも出さず、`sparseAddressSpaceSize` も 0（MoltenVK の `MVKDevice.mm` 2796〜2866 行・3146 行）。
- [事実] 画像の sparse の形の問い合わせは 0 個と答える：`get_sparse_format`・`get_sparse_format2`（`icd.c` 2690〜2698 行）、`dev_image_sparse_reqs`（8598〜8601 行）。
- [事実] `vkQueueBindSparse` と `vkGetImageSparseMemoryRequirements(2)` は「まだ中継していない入口」：`NOT_YET`（`icd.c` 8643 行）。呼ばれると名前を記録に出して `VK_ERROR_FEATURE_NOT_PRESENT` を返す。
- [事実] buffer と image を作るとき、アプリの flags をそのまま受け手に送る（`create_buffer` の `icd.c` 3001 行、`create_image` の 3109 行）。SPARSE の印が付いて来たら、そのまま MoltenVK に届く。
- [事実] キューの種類は受け手の答えのまま（`get_queue_families`、`icd.c` 2499〜2514 行）。sparse binding のキューは無い。

## 1. vkd3d-proton が何を要るか（原文で確認）

- [事実] `d3d12_device_determine_tiled_resources_tier`（vkd3d-proton の `libs/vkd3d/device.c` 8938〜8962 行）。
  - Tier 0（NOT_SUPPORTED）になる条件：sparseBinding・sparseResidencyAliased・sparseResidencyBuffer・sparseResidencyImage2D・residencyStandard2DBlockShape のどれかが無い、または sparse binding のキューが無い。
  - Tier 1 で止まる条件：shaderResourceResidency・shaderResourceMinLod・residencyNonResidentStrict のどれかが無い、residencyAlignedMipSize が真、または `filterMinmaxSingleComponentFormats`（Vulkan 1.2 の性質）が無い。
  - それ以外は Tier 2。3D の sparse もあれば Tier 4。
- [事実] 12_0 の条件は TiledResourcesTier 2 以上（`device.c` 9656〜9661 行）。
- [事実] 環境変数 `VKD3D_FEATURE_LEVEL` を 12_0 にすると、上の検査をせずに TiledResourcesTier 2 と 12_0 を名乗らせる（`d3d12_device_caps_override`、`device.c` 9944〜10012 行）。
- [事実] Windows では、vkd3d-proton は環境変数を `GetEnvironmentVariableA` で読む（`libs/vkd3d-common/platform.c` 387〜405 行の `_WIN32` の側）。上書きを読むのは装置を作るとき（`device.c` 10047 行）。

## 2. 案

### S0：VKD3D_FEATURE_LEVEL=12_0 を立てる（いちばん小さい）

- やり方：次のどちらか。
  1. 配るキットの `pocketgpu.cmd` に `set VKD3D_FEATURE_LEVEL=12_0` を足す（`app\pc\make_kit.py` の `LAUNCH`、15〜28 行）。
  2. ドライバが vkd3d-proton を見分けた所（`create_instance`、`icd.c` 2118 行）で `SetEnvironmentVariableA("VKD3D_FEATURE_LEVEL", "12_0")` を呼ぶ。利用者が自分で値を入れていたら上書きしない。
- 難しさ：とても小さい（数行）。
- 落とし穴
  - [事実] 名乗るだけで、sparse は足りないまま。
  - [推測] ゲームが本当に tiled resource（`CreateReservedResource`）を作ると、vkd3d-proton は SPARSE の印つきで buffer や image を作ろうとする。うちのドライバは印をそのまま受け手に送るので（0 章）、MoltenVK で何が起きるかは確かめていない。落ちるか、印を無視して中身の無い物ができるか。
  - [推測] 2 の方法は、ドライバがアプリのプロセスの環境を書き換える。ほかの部品が同じ変数を読むことは考えにくいが、記録に出しておくと調べやすい。
- 確かめ方：feature level を表示する小さな D3D12 のテスト（`D3D12CreateDevice` を 12_0 で呼ぶだけ）で、作れるかを見る。vkd3d-proton の記録（`VKD3D_DEBUG=info`）に feature level が出る。

### S1：名乗って、中身は全部ふつうのメモリで持つ（「名乗りだけ」の sparse）

- やり方
  - 機能・性質・sparse のキューを名乗る。
  - sparse の buffer と image は、受け手ではふつうの物として作る。ドライバが隠しメモリを仮の大きさ全部ぶん取り、作ったときにつないでしまう。
  - `vkQueueBindSparse` は中身を動かさない。待つ・知らせる semaphore と fence だけを、空の submit として送る（`queue_submit`、`icd.c` 7938 行を使う）。
  - 先例：Intel の Mesa ドライバ（ANV）の `fake_sparse`。名乗るが実装は不完全で、起動時に有無だけ見るゲームのため（[要約経由] MR !24046。マージの状態は資料が食い違い未確認）。
- 入れる場所
  - 機能：`mask_features`（`icd.c` 852〜854 行）の偽にする所を、設定で外す。shaderResourceResidency と shaderResourceMinLod も要る。
  - 性質：`get_props`（`icd.c` 2164 行）か `get_props2` で `sparseProperties` の residencyStandard2DBlockShape・residencyNonResidentStrict を真、residencyAlignedMipSize を偽にする。`sparseAddressSpaceSize` も 0 から上げる。
  - キュー：`get_queue_families`（2499〜2514 行）で 0 番の種類に `VK_QUEUE_SPARSE_BINDING_BIT` を足す。
  - 形の問い合わせ：`get_sparse_format`（2690〜2698 行）と `dev_image_sparse_reqs`（8598〜8601 行）で、標準のブロックの形を返す。2D・1 サンプルで 64KB（32 ビットの形なら 128×128）[要約経由・Vulkan の仕様]。
  - 作成：`create_buffer`（2993 行〜）と `create_image`（3081 行〜）で SPARSE の印を外して送り、`struct buf`（1057 行）と `struct img`（3073 行）に「sparse」と覚える。隠しメモリは `alloc_memory`（2835 行）と `bind_buffer`（3053 行）・`bind_image`（3178 行）を中で呼んで作る。
  - 入口：`NOT_YET`（8643 行）から `vkQueueBindSparse` と `vkGetImageSparseMemoryRequirements(2)` を外して、本物の関数にする。
  - シェーダー：`OpImageSparse*`（`OpImageSparseSampleImplicitLod` など）を、ふつうの読みと「いつも resident」の値に直す。直す場所は `spv_fix`（5640 行〜）。
- 難しさ：中。[推測]
- 嘘になる所
  - [推測] 結び付けていない所を読んでも 0 にならない（residencyNonResidentStrict の約束を破る）。
  - [推測] メモリの頁を別のタイルに付け替えても、中身はついて来ない。
  - [推測] 同じ頁を 2 か所に結んでも、中身は共有されない（sparseResidencyAliased の約束を破る）。
  - [推測] `OpImageSparseTexelsResident` がいつも「ある」と答える。仮想テクスチャで「無いタイルを読み込む」仕組みを持つゲームは、読み込みを頼まなくなる。
  - [推測] 仮の大きさ全部ぶんのメモリを取る。仮想テクスチャ用の大きな tiled resource だと、iPhone のメモリに収まらないおそれがある。zink の第三者フォークの commit にも「sparse buffer を最大の大きさで 1 対 1 に取る」のが限界として書かれている（[要約経由]）。
- 確かめ方：Vulkan CTS の `dEQP-VK.sparse_resources.buffer.*` と `image_sparse_binding.*` を走らせ、どこが落ちるかを一覧にする（落ちるのは想定内。どの約束が破れているかを知るため）。

### S2：S1 に「付け替えたら写す」を足す

- やり方
  - メモリの頁ごとに「今どの物のどのタイルに結んでいるか」を覚える。
  - 頁を別のタイルに結び直したら、前の場所の中身を新しい場所に写す（buffer は `VR_CMD_COPY_BUFFER`、image は `VR_CMD_COPY_IMAGE`）。
  - 結び付けを外したら、その範囲を 0 で埋める（buffer は `VR_CMD_FILL_BUFFER`、image は消去の命令）。
- 入れる場所：S1 の `vkQueueBindSparse` の中。写す命令を積んだ一時の command buffer を、待つ semaphore と知らせる semaphore の間に挟んで送る。
- 難しさ：大。[推測] 画像はタイルの形が形式ごとに違い、mip tail もある。
- 嘘になる所：[推測] 結び付けていない所への書き込みは捨てられない。同じ頁を 2 か所に同時に結ぶ（aliasing）のは、まだ再現できない。
- 先例：コピーで sparse を真似た例は見つからなかった。いちばん近いのは、Mali の Panthor で、結び付けていない所をダミーの物に循環させて向けるもの（[要約経由] カーネルのパッチ系列）。

### S3：受け手で Metal の本物の sparse を使う

- やり方：受け手（`server.c`）が MoltenVK を通さずに Metal の sparse heap と sparse texture を作る。MoltenVK の `VK_EXT_metal_objects` で Vulkan の image として取り込む。結び付けは `MTLResourceStateCommandEncoder` の `updateTextureMapping` で行う。
- 難しさ：とても大きい。約束事（`proto.h`）に命令を足す必要がある。[推測]
- 落とし穴
  - [事実・要約経由] Metal の sparse の文書が扱うのは texture だけ。sparse の buffer は見つからなかった。
  - [事実・要約経由] MoltenVK の issue #1700 のメンテナの書き込み：Metal は sparse のタイルを heap の決まった場所に置けない。Vulkan と D3D12 は置ける。→ aliasing は真似られない。
  - [要約経由] 同じ issue に「Metal 4 で placement sparse が入った」という書き込みがある。公式の資料は読めていない。
- 確かめ方：まず Metal 4 の placement sparse の公式の文書を読む。

## 3. 外の例

| 例 | やり方の要点 | 確かめ方 | 出典 |
|---|---|---|---|
| vkd3d-proton | sparse が無ければ TiledResourcesTier を 0 にする。環境変数で上書きできる | 原文を読んだ | https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/master/libs/vkd3d/device.c ／ https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/master/libs/vkd3d-common/platform.c |
| ANV（Intel の Mesa ドライバ） | `fake_sparse`：名乗るが完全には実装しない | 要約経由 | https://www.phoronix.com/news/Intel-ANV-Fake-Sparse-Support （見出しと検索の要約のみ） |
| lavapipe（CPU の Vulkan） | ソフトで sparse を実装した。64KB のタイルの並び | 要約経由（検索結果） | https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/29408 |
| Panthor（Mali） | 結び付けていない所をダミーの物に循環させて向ける | 要約経由 | https://lkml.iu.edu/2604.0/05941.html |
| MoltenVK | sparse は無い。issue #1700 は open | 要約経由 | https://github.com/KhronosGroup/MoltenVK/issues/1700 |
| Metal | sparse heap と sparse texture。マップしたタイルは初期化されない | 要約経由 | https://developer.apple.com/documentation/metal/creating-sparse-heaps-and-sparse-textures.md ／ https://developer.apple.com/documentation/metal/assigning-memory-to-sparse-textures.md |
| Honeykrisp（Apple の GPU の Vulkan） | 2024-10 の時点で sparse は「次」。2025-03 のカーネルのパッチで VM_BIND を使って対応 | 要約経由 | https://alyssarosenzweig.ca/blog/aaa-gaming-on-m1.html ／ https://lists.infradead.org/pipermail/linux-arm-kernel/2025-March/1014102.html |
| Vulkan の仕様 | 結び付けていない所は、読むと 0・書くと捨てる（residencyNonResidentStrict のとき）。標準のブロックは 64KB | 要約経由 | https://docs.vulkan.org/spec/latest/chapters/sparsemem.html |

調べ物のメモの全文は作業用の場所の `notes_sparse.md` にある。

## 4. 確かめていないこと（先に見たい順）

1. iPhone の MoltenVK に SPARSE の印つきの buffer と image を作らせたとき、何が起きるか（S0 の落とし穴）。
2. MoltenVK が `filterMinmaxSingleComponentFormats` を A19 Pro で真にするか。MoltenVK の user guide は samplerFilterMinmax に「Apple の GPU の family 10 と iOS 26」を求めると書いていた（前回の調べ）。真でないと、S1 でも Tier 1 で止まる。
3. tiled resource を実際に使うゲームの数。

## 5. 3 つのうち、どれから手を付けるか

1. **transform feedback の X0**（`xfb_emulation.md`）：小さく、OpenGL 3.0 の入口の 1 つ。
2. **geometry shader の穴**（`gs_emulation.md`）：仕組みはもうある。
3. **sparse**：S0 は数行なので、12_0 を名乗らせたいなら順番と関係なくすぐできる。S1 から先は嘘が多く、メモリも食うので最後。

理由：[推測] sparse を実際に使うゲームは、stream output や geometry shader を使うゲームより少ないと思う。数えてはいない。
