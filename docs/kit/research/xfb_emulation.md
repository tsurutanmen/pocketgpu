# transform feedback を真似る設計の下調べ（2026-10-10）

印：**[事実]** はソースや一次の文書で確かめたこと。**[推測]** は私の推論。外の例の多くは調べ物の担当が要約器経由で読んだもので、その旨を書く。
うちのソースは読むだけで、書き換えていない。行番号は 2026-10-10 13 時台の作業コピーのもの。

## 0. 今のうちの状態

- [事実] vkd3d-proton のときだけ「名乗るだけ」をしている。
  - 拡張の一覧に足す：`icd.c` 937 行
  - 性質（上限・transformFeedbackQueries・transformFeedbackDraw を真）：`icd.c` 2441〜2454 行
  - 機能（transformFeedback・geometryStreams を真）：`icd.c` 2489〜2492 行
- [事実] 記録の命令はすべて飛ばしている：`icd.c` 8456〜8487 行（`cmd_bind_xfb`・`cmd_begin_xfb`・`cmd_end_xfb`・`cmd_draw_xfb_count`）。番号つきの query は 0 番だけふつうの query に回す（8475〜8481 行）。
- [事実] シェーダーの直し（`spv_fix`、`icd.c` 5640 行〜）は Xfb の飾りに触らない。Xfb つきの SPIR-V はそのまま受け手に行く。`icd.c` 935〜936 行のコメントにも「stream output を使う pipeline が作れるかは受け手の GPU しだい」とある。
- [事実] transform feedback の query pool（`VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT`）は、そのまま受け手に送る（`create_qpool`、`icd.c` 8289〜8306 行）。受け手は作れなければ記録に出すだけ（`server.c` 4344〜4355 行）。その後の開始と終わりは、pool が無ければ飛ばす（`server.c` 1604〜1613 行）。結果の読み出しは、pool が無いと `VK_ERROR_INITIALIZATION_FAILED` を返す（`server.c` 4379 行）。
- [事実] GS を真似る仕組みは、Xfb の実行モードを持つ GS を断る（`gs_emu.c` 1149 行）。
- [事実] DXVK（DirectX 11）と zink（OpenGL）には名乗っていない。
- [事実] MoltenVK は頂点の段から buffer に書ける（`vertexPipelineStoresAndAtomics = true`、MoltenVK の `MVKDevice.mm` 2819 行）。

## 1. 誰が困っているか

- [事実・要約経由] zink は OpenGL 3.0 に VK_EXT_transform_feedback を要る（Mesa の zink の文書）。zink は拡張が無いと stream output の数を 0 にする（`zink_screen.c` の `zink_init_screen_caps`、要約経由）。→ OpenGL 3.0 の入口。
- [推測] DirectX 11・12 のゲームで stream output を使うもの（パーティクルを GS で作って書き出す、など）。今は書き出しが空になる。使うゲームの数は調べていない。

## 2. 案

### X0：飾りを外して pipeline を通し、zink にも名乗る（いちばん小さい）

- やり方
  1. `spv_fix` に足す：`OpCapability TransformFeedback`（53）と `GeometryStreams`（54）、`OpExecutionMode Xfb`（11）、飾りの `XfbBuffer`（36）・`XfbStride`（37）を消す。`Offset`（35）はブロックの中でも使うので、出力の変数に付いた物だけ消す。
  2. zink にも、vkd3d-proton と同じ「名乗るだけ」を広げる（937・2441〜2454・2489〜2492 行の `vkd3d()` の条件）。
- 入れる場所：`icd.c` 5640 行〜（`spv_fix`）、5849〜5880 行（`create_shader`）、937・2454・2492 行の条件。
- 難しさ：小（数十行）。[推測]
- 落とし穴
  - [推測] 書き出しを使うアプリは、静かに空の結果を受け取る。OpenGL 3.x のゲームの多くは transform feedback を使わないと思うが、数えていない。
  - [事実] zink は VK_EXT_conditional_rendering が無くても「条件つき描画ができる」と名乗り、条件を見ずに描く（`zink_screen.c` 995 行・`zink_query.c` 1325〜1328 行。2026-10-10 に追記。くわしくは `cond_render.md`）。3.0 の判定で止めているのは stream output の数のほう（`zink_screen.c` 973〜975 行）。前の版では「conditional rendering も要る」と書いていたが、誤りだった。
  - [推測] Xfb の飾りつきの SPIR-V を MoltenVK（SPIRV-Cross）が今どう扱うかは確かめていない。断るなら X0 の 1 だけで直る物がある。
- 確かめ方：`IPHONE_VK_SPV_DUMP` で受け手に送る SPIR-V を書き出し、`spirv-dis` で Xfb の飾りが消えたかを見る。zink で `glxinfo` 相当（`wglinfo` など）を出して OpenGL の版が上がるかを見る。

### X1：最後の頂点の段で storage buffer に書く（本命・GS の仕組みを使い回す）

- やり方
  - Xfb つきの頂点シェーダーを、GS を真似る「C1（自分で読む）」と同じ形に直す。作った VS は、出力の頂点 1 つにつき 1 回走る（番号なし・インスタンス 1 つの描画に直す）。プリミティブ p の c 番目の角なら、書く場所は `基点 + (p × 角の数 + c) × stride + offset`。
  - 帯と扇は、出力をばらした三角に直してから書く。Vulkan の決まりがそうなっている（[事実・要約経由] 仕様の Transform Feedback の節）。C1 はもう帯・扇を読める（`GSR_TOPO`、`gs_emu.h`）。
  - 基点は、描画ごとに CPU が数える（ANGLE と同じ考え。直接の描画なら頂点の数は CPU が知っている）。
  - 1 プリミティブぶんの空きが無ければ書かない（[事実・要約経由] 仕様）。シェーダーの中で比べる。
- 入れる場所
  - SPIR-V を覚える：`create_shader`（`icd.c` 5849〜5880 行）。今の `gs_keep` は GS を持たない GPU の VS と GS だけを覚える（`gs_icd.inc` 88 行）。Xfb つきの VS も覚えるようにする。
  - pipeline を差し替える：`create_graphics_pipelines_raw`（`icd.c` 3478 行〜）の GS の分かれ道（3487〜3493 行）の横に、Xfb の分かれ道を足す。
  - 書き先の set：GS の C1 と同じく、pipeline layout の後ろに set を 1 つ足す（`gs_note_layout`、`gs_icd.inc` 141 行）。
  - buffer の使い道：`create_buffer`（`icd.c` 3008 行の `gs_buffer_usage` の横）で、TRANSFORM_FEEDBACK_BUFFER と COUNTER_BUFFER の印を STORAGE_BUFFER に置き換える。
  - 記録：8462〜8487 行の空の関数を、command buffer（`struct cb`、`icd.c` 1130 行）に「つないだ buffer・始めたか」を覚える物に置き換える。`cmd_draw`（7091 行）と `cmd_draw_indexed`（7101 行）の頭で、`gs_draw` と同じように横取りする。
  - 読み戻し：書いた範囲を `cb_writes`（`icd.c` 6819 行）に伝える。CPU から見えるメモリなら、待ちのあとに引き取られる。
  - 受け手：変えない（GS の真似と同じ考え。`gs_icd.inc` 3 行）。
- 難しさ：中。C1 の取り出しの仕組みがあるので、新しく作るのは「書く命令を足す」「基点を数える」部分。[推測]
- 落とし穴
  - [推測] set を 1 つ足すので、アプリが set を上限まで使うと足せない。上限は MoltenVK の maxBoundDescriptorSets しだい（確かめていない）。
  - [事実・要約経由] counter buffer で続きから書く（D3D の追記）と、基点が GPU の中にしか無い。CPU では数えられない。X2 が要る。
  - [事実・要約経由] 間接の描画は頂点の数が GPU の中にしか無い。Asahi の MR の要約でも「間接の描画は未対応」。最初は飛ばして記録に出す。
  - [推測] 同じ頂点を 2 回走らせても同じ値を同じ場所に書くので、壊れない。
- 確かめ方：Vulkan CTS の `dEQP-VK.transform_feedback.simple.*` のうち、直接の描画の物を通す。OpenGL なら piglit の `ext_transform_feedback` の基本の物。

### X2：counter buffer・query・書いた量で描く

- やり方
  - Begin で counter buffer の値を一時の場所に写す（`VR_CMD_COPY_BUFFER`）。VS はそこから基点を読む。
  - End で「基点 + 書いた量」を counter に戻す。足し算が要るので、ドライバが作る小さな compute の pipeline を 1 つ持つ（`VR_CMD_DISPATCH`）。
  - query（書いた数・要った数）は、ドライバの中で答える。今の `q_local`（`icd.c` 8328 行）と同じ形で、隠した buffer から読む。
  - `vkCmdDrawIndirectByteCountEXT` は、compute で間接描画の引数を作ってから `VR_CMD_DRAW_INDIRECT` で描く。Asahi は CPU に読み戻して直接の描画にしている（[事実・要約経由] `agx_draw_vbo_from_xfb`）。うちでは往復が高いので GPU で作る。[推測]
- 入れる場所：`create_qpool`（8289 行）・`get_qpool_results`（8344 行）・`cmd_draw_xfb_count`（8482 行）。compute の SPIR-V は `gs/spv_table.h` と同じく、手で書いた語の並びを埋め込む。
- 難しさ：中。[推測]
- 落とし穴：[推測] query の「書いた数」は、空きが足りないときだけ「要った数」と違う。最初は両方を同じにして答える手もある（嘘になるのは溢れたときだけ）。

### X3：GS と transform feedback を一緒に使う

- やり方：GS の真似が出力の頂点ごとに走るので、そこで書く。ただし GS 1 回が出すプリミティブの数が呼び出しごとに違うと、前の呼び出しが何個書いたかを数え上げる段（prefix sum）が要る。Asahi は数える段を compute で先に走らせている（[事実・要約経由] `agx_nir_lower_gs.c` と `geometry.cl` の `libagx_previous_xfb_primitives`）。出す数が決まっている GS なら、数え上げは要らない（同じ関数の static_count の道）。
- 入れる場所：`gs_emu.c` 1149 行の断りを外し、書く命令を足す。
- 難しさ：大。[推測]

## 3. 外の例

| 例 | やり方の要点 | 確かめ方 | 出典 |
|---|---|---|---|
| Mesa Asahi（Apple の GPU・固定の xfb が無い） | 出力の頂点ごとに書く場所を計算（`libagx_xfb_vertex_offset`）。帯の頂点は複数のプリミティブに重ねて書く。GS つきは数える段を compute で先に走らせる | 要約経由 | https://fuchsia.googlesource.com/third_party/mesa/+/main/src/asahi/libagx/geometry.cl ／ https://fuchsia.googlesource.com/third_party/mesa/+/main/src/asahi/lib/agx_nir_lower_gs.c ／ https://fuchsia.googlesource.com/third_party/mesa/+/main/src/gallium/drivers/asahi/agx_streamout.c |
| Honeykrisp（Apple の GPU の Vulkan） | ハードの xfb 無しで transformFeedback と transformFeedbackQueries を名乗る。transformFeedbackDraw は偽 | 要約経由 | https://fuchsia.googlesource.com/third_party/mesa/+/main/src/asahi/vulkan/hk_physical_device.c |
| ANGLE（Vulkan と Metal の両方） | 描画ごとに CPU で書き始めの位置を計算して渡す。プリミティブの数も CPU で数える | 要約経由 | https://chromium.googlesource.com/angle/angle/+/main/src/libANGLE/renderer/vulkan/TransformFeedbackVk.cpp ／ https://raw.githubusercontent.com/google/angle/main/src/libANGLE/renderer/metal/TransformFeedbackMtl.mm |
| Ryujinx（MoltenVK の issue での引用） | 頂点の段の最後で storage buffer に書く。GS と tessellation があると使えない、という指摘つき | 要約経由（二次） | https://github.com/KhronosGroup/MoltenVK/issues/1588 |
| Vulkan の仕様 | 帯と扇はばらして書く。1 プリミティブぶんの空きが無ければ書かず、数えない | 要約経由 | https://docs.vulkan.org/spec/latest/chapters/vertexpostproc.html |
| zink | 拡張が無いと stream output の数を 0 にする。代わりの道は見つからなかった | 要約経由 | https://fuchsia.googlesource.com/third_party/mesa/+/main/src/gallium/drivers/zink/zink_screen.c |

見つからなかったもの：DXMT・Game Porting Toolkit・CrossOver の stream output の扱い、Ryujinx の Metal の実装の本体、vkd3d-proton の stream output の中身。調べ物のメモの全文は作業用の場所の `notes_xfb.md` にある。

## 4. 3 つのうち、どれから手を付けるか

1. **transform feedback の X0**（小）。zink の OpenGL 3.0 の判定で止めているのはこれ（`cond_render.md` の 0 章）。vkd3d-proton の stream output つきの pipeline が受け手で断られる恐れも減る。
2. **geometry shader の穴**（`gs_emulation.md`）。仕組みはもうあるので、間接の描画と zink への名乗りを足すのが近い。OpenGL 3.2 の入口になる。
3. **transform feedback の X1・X2**（中）。書き出しを本当に使うゲームのため。
4. **sparse**（`sparse_emulation.md`）。12_0 を名乗るだけなら環境変数で今すぐできる（S0）。本当に真似るのは嘘が多く、いちばん重いので最後。

理由：[推測] 名乗りの壁を先に越えると、動くアプリの数が増える。中身の真似は、実際に使うアプリで試せるようになってからのほうが確かめやすい。
