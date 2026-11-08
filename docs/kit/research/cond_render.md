# 条件つきで描く機能（VK_EXT_conditional_rendering）を真似る設計の下調べ（2026-10-10）

印：**[事実]** はソースや一次の文書で確かめたこと。**[推測]** は私の推論。外の例の多くは調べ物の担当が要約器経由で読んだもので、その旨を書く。
うちのソースは読むだけで、書き換えていない。行番号は 2026-10-10 の作業コピーのもの。zink のソースは Fuchsia のミラー（`gitlab/main`、版は不明）を私が落として読んだ。

## 0. 先に：zink の OpenGL 3.0 の壁はこれではなかった

- [事実] zink は、拡張の有無に関係なく「条件つき描画ができる」と名乗る：`zink_screen.c` 995 行 `caps->conditional_render = true;`、824 行 `caps->conditional_render_inverted = true;`。
- [事実] 拡張が無ければ、条件つき描画を始めずに戻る：`zink_query.c` 1325〜1328 行（`zink_start_conditional_render`）と 1343〜1347 行（stop）。つまり**条件を見ずに、いつも描く**。
- [事実] 一方で stream output の数は、transform feedback の拡張が無いと 0：`zink_screen.c` 973〜975 行。
- [推測] よって zink の OpenGL 3.0 で足りないのは transform feedback のほう。条件つき描画は、真似なくても 3.0 の判定は通る。Mesa の zink の文書は 3.0 の要件に VK_EXT_conditional_rendering を挙げているが（[事実・要約経由] `docs/drivers/zink.rst`）、コードはそれで止めていない。
- [事実] 同じ文書は「一部の要件は検証されておらず、描画の崩れ・検証エラー・落ちることがある」と注意している（[要約経由]）。
- [推測] したがって、この設計の目的は「3.0 を名乗るため」ではなく、「条件つき描画を使う OpenGL のアプリで、絵を正しくするため」。条件を無視すると、本来消える物が描かれる（例：壁の向こうの太陽のレンズフレアが、遮蔽の query を見ずに描かれる）。

この点は、前に書いた `xfb_emulation.md` と `gs_emulation.md` の「zink の 3.0 には conditional rendering も要る」という記述を直す。今日あわせて直した。

## 1. 今のうちの状態

- [事実] ドライバにも受け手にも、条件つき描画の扱いは無い（`icd.c`・`server.c` を `ConditionalRendering`・`conditional_rendering` で探して 0 件）。拡張も名乗っていない。
- [事実] MoltenVK も持たない（MoltenVK の user guide の拡張の一覧と `MVKDevice.mm` を全文で探して 0 件。前回の調べで落とした物）。
- [事実] 2 次の command buffer は、呼んだ側に中身を写す作り（`icd.c` 7070〜7089 行の `cmd_exec`）。受け手は 2 次を使わない。→ 引き継ぎ（inheritedConditionalRendering）は、ドライバの中の状態を引き継ぐだけで済む。[推測]

## 2. Vulkan の決まり（要点）

- [事実・要約経由] buffer の offset にある 32 ビットの値が 0 なら、描画は捨てられる。INVERTED の印なら逆。offset は 4 の倍数。
- [事実・要約経由] 対象は描画・compute の dispatch・attachment の消去（vkCmdClearAttachments）。copy と blit は対象外。
- [事実・要約経由] 途中で値が変わったときの扱いは実装しだい。→ 始めた時点の値で決めてよい。[推測]
- 出典：https://docs.vulkan.org/refpages/latest/refpages/source/VkConditionalRenderingBeginInfoEXT.html ／ https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBeginConditionalRenderingEXT.html

## 3. zink の使い方（原文で確認）

- [事実] 条件の準備：`zink_render_condition`（`zink_query.c` 1353 行〜）。まず render pass を閉じ（`zink_batch_no_rp`、1361 行）、query の結果を「predicate」の buffer に GPU で写す（`copy_results_to_buffer`）。結果が複数あるなどの場合は CPU で読んで書く（`force_cpu_read`、1399 行）。
- [事実] 条件つき描画の開始は、新しい render pass を始めた直後（`zink_context.c` 3660〜3664 行）。止めるのは render pass を閉じる前（`zink_batch_no_rp_safe`、3678〜3681 行）。compute の dispatch では、render pass の外で始める（`zink_draw.cpp` 1314〜1315 行）。
- [事実] 消去の周りでは、止めてから始め直している所がある（`zink_clear.c` 609〜660 行）。中身は読んでいない。
- [推測] つまり、うちのドライバに Begin が届くのは「render pass の始まりの直後」。条件の値を書く命令は、その render pass より前に済んでいる。

## 4. 案

### C0：何もしない（今のまま）

- [事実] zink は拡張が無ければいつも描く（0 章）。OpenGL 3.0 の判定には響かない。
- 落とし穴：[推測] 遮蔽の query で「見えないなら描かない」を使うアプリで、見えない物が描かれる。多くは重くなるだけで絵は同じだが、レンズフレアのように「見えるときだけ出す」物は絵が変わる。

### C1：間接の描画に直し、compute で数を 0 にする（本命）

- やり方
  1. 拡張と機能を名乗る（conditionalRendering と inheritedConditionalRendering）。
  2. Begin を受けたら、command buffer に「条件つきの区間」と、predicate の buffer・offset・INVERTED を覚える。
  3. 区間の中の描画は、直接の描画も間接の描画に直す。引数（頂点の数・インスタンスの数など）は、ドライバが持つ隠し buffer の「原本」の欄に CPU で書く。CPU から見えるメモリにしておけば、submit のときにメモリの写しの仕組みで受け手に届く。
  4. 小さな compute を 1 回走らせて、predicate を読み、「原本 × (条件が真なら 1・偽なら 0)」を「使う欄」に書く。原本は書き換えないので、同じ command buffer を何度 submit しても正しい。
  5. compute は render pass の中では走らせられない。そこで、Begin が render pass の中で届いたら、**その render pass を始めた命令の手前**に compute と仕切り（compute の書き込み → 間接描画の読み）を差し込む。記録はドライバのメモリ（`struct cb` の `s`）に submit までためてあるので、後から差し込める。[推測] render pass の中では transfer の命令が使えないので、render pass の始まりと Begin の間で predicate が書き換わることは、シェーダーが書かない限り無い。
  6. 区間の外（compute の dispatch のとき）なら、Begin の場所にそのまま差し込む。
  7. dispatch は `VR_CMD_DISPATCH_INDIRECT` に直す（数を 0 にする）。
- 先例
  - [事実・要約経由] Asahi の `libagx_predicate_indirect`（`src/asahi/libagx/draws.cl`）：drawIndirectCount の実装で、compute で間接の引数を写し、数の外の描画は 0 で埋めて飛ばす。条件つき描画ではないが、「GPU で引数を書き換えて描画を飛ばす」先例。
  - 出典：https://fuchsia.googlesource.com/third_party/mesa/+/main/src/asahi/libagx/draws.cl
- 入れる場所
  - 名乗り：拡張の一覧（`icd.c` 925〜937 行の並び）と、`get_features2`（2467 行〜）。入口の表（8655 行〜の `F(...)`）に `vkCmdBeginConditionalRenderingEXT` と `vkCmdEndConditionalRenderingEXT` を足す。
  - 状態：`struct cb`（`icd.c` 1130〜1157 行）に、区間の印・predicate・INVERTED・「今の render pass を始めた命令の、記録の中での位置」を足す。位置は `cmd_begin_rp`（7038 行）と `cmd_begin_rendering`（7510 行）で覚える。`cb_clear`（6708 行）で消す。
  - 描画：`cmd_draw`（7091 行）・`cmd_draw_indexed`（7101 行）・`draw_indirect`（7112 行）の頭で、区間の中なら間接に直す。
  - dispatch：`cmd_dispatch`（6936 行）・`cmd_dispatch_base`（6944 行）・`cmd_dispatch_indirect`（6955 行）。
  - 隠し buffer：GS の真似の「記録」の buffer（`gs_rec`、`gs_icd.inc` 437〜469 行）と同じ作り方を使い回せる。
  - 受け手：変えない。使う命令（`VR_CMD_DISPATCH`・`VR_CMD_DRAW_INDIRECT`・`VR_CMD_DRAW_INDEXED_INDIRECT`・`VR_CMD_DISPATCH_INDIRECT`・`VR_CMD_PIPELINE_BARRIER`）はもうある（`proto.h`）。
- 難しさ：中。[推測] ドライバが持つ小さな compute の pipeline の仕組みは、まだ無い。transform feedback の X2 と geometry shader の G1 でも同じ物が要るので、1 つ作れば 3 つで使える。
- 落とし穴
  - [推測] vkCmdClearAttachments には間接の形が無い。区間の中の消去は、(a) 全画面の四角を描く消去に直して間接で描く、(b) 消去は条件を見ずに行う（嘘）、のどちらか。zink は消去の周りで条件を止め直している（3 章）ので、zink では (b) でも困らない可能性がある。確かめていない。
  - [推測] GS を真似た描画（`gs_draw`、`icd.c` 7093 行）は、数を CPU で計算して直接の描画で出している。区間の中では、それも間接に直す必要がある。GS の真似の間接の描画は今は飛ばしている（`gs_icd.inc` 630〜636 行）ので、そこも一緒に直すことになる。
  - [推測] 1 つの render pass の中で Begin と End が何度もあると、差し込む compute も増える。zink は render pass ごとに 1 回なので、困らないと思う。
  - [推測] 引数の数が 0 の間接の描画・dispatch を MoltenVK がどう扱うかは確かめていない。Vulkan では、インスタンスの数 0 の描画は何も描かないだけ。
- 確かめ方
  - Vulkan CTS の `dEQP-VK.conditional_rendering.*`。
  - OpenGL なら piglit の `nv_conditional_render-*`。遮蔽の query で「見えないなら描かない」を確かめる。

### C2：受け手の中で待って、描くかを決める

- やり方：受け手が、Begin の手前で submit を区切り、そこまでの仕事が終わるのを受け手の中で待ち、predicate の値を読んで、続きを記録し直して出す。USB の往復は増えない（受け手の中で待つだけ。`proto.h` 6〜8 行の「受け手の中で待つぶんには往復が要らない」と同じ考え）。
- 先例：[事実・要約経由] Mesa の softpipe・llvmpipe・panfrost は、CPU で query の結果を待って決める。zink も一部の query では CPU で読む（`force_cpu_read`）。
- 落とし穴：[推測] zink では Begin が render pass の中で来るので、区切るには render pass を閉じて開き直すことになる。iPhone の GPU（タイルで描く）では、render pass を閉じるたびに描きかけの絵をメモリに書き出すので重い。GPU も毎回空になって待つ。
- 難しさ：大（受け手の記録の仕組みを変える）。[推測]
- 勧めない。C1 のほうが軽く、受け手も変えずに済む。

### C3：頂点シェーダーで消す

- やり方：区間の中で使う pipeline の頂点シェーダーに、predicate を読んで偽なら位置を画面の外（または NaN）にする命令を足す。
- 落とし穴：[推測] 区間の中で使う pipeline ごとに別版が要る。描画の数は減らないので、頂点の計算は残る。dispatch には効かない。
- C1 が使えない場合（Begin と render pass の始まりの間に、predicate を書くシェーダーがある、など）の予備。

### 参考：Metal の道具は使えない

- [事実・要約経由] Metal の visibility result は、深度・ステンシルを通った数を buffer に書くだけで、任意の値で描画を止める仕組みではない。
- [事実・要約経由] Metal 4 の `executeCommands(buffer:indirectBuffer:)` は、実行する範囲を GPU の buffer から読める（iOS 26 以降）。ただし MoltenVK の下で、うちから使う道は無い。受け手で Metal を直接使う大きな作り替えになる。

## 5. 外の例

| 例 | やり方 | 確かめ方 | 出典 |
|---|---|---|---|
| zink | 拡張が無ければ、条件を見ずにいつも描く。3.0 の判定は止めない | 原文を読んだ | https://fuchsia.googlesource.com/third_party/mesa/+/refs/heads/gitlab/main/src/gallium/drivers/zink/zink_query.c ／ https://fuchsia.googlesource.com/third_party/mesa/+/refs/heads/gitlab/main/src/gallium/drivers/zink/zink_screen.c |
| softpipe・llvmpipe・panfrost | CPU で query の結果を待って、描くかを決める | 要約経由 | https://fuchsia.googlesource.com/third_party/mesa/+/refs/heads/gitlab/main/src/gallium/drivers/llvmpipe/lp_query.c ／ https://fuchsia.googlesource.com/third_party/mesa/+/refs/heads/gitlab/main/src/gallium/drivers/panfrost/pan_context.c |
| v3d | 条件つき描画を名乗ると OpenGL 3.0 と 3.1 が誤って出るので、名乗るのをやめた（2023） | 要約経由 | https://fuchsia.googlesource.com/third_party/mesa/+/117b967321603eda83c590125a8f821fa005a4cb%5E%21 |
| Asahi の libagx | drawIndirectCount を、compute で引数を書き換えて実装 | 要約経由 | https://fuchsia.googlesource.com/third_party/mesa/+/main/src/asahi/libagx/draws.cl |
| DXVK | D3D11 の SetPredication は「Stub」と記録に出すだけ | 要約経由 | https://raw.githubusercontent.com/doitsujin/dxvk/master/src/d3d11/d3d11_context.cpp |
| OpenGL の NV_conditional_render | NO_WAIT は「結果がまだ無いとき」に限り、待たずに描いてよい | 要約経由 | https://registry.khronos.org/OpenGL/extensions/NV/NV_conditional_render.txt |

見つからなかったもの：vkd3d-proton が拡張の無いときに D3D12 の predication をどう扱うか、lavapipe の判定の中身、引数 0 の間接の描画を MoltenVK がどう扱うか。調べ物のメモの全文は作業用の場所の `notes_condrender.md` にある。

## 6. どれから手を付けるか

- [推測] 条件つき描画は、OpenGL 3.0 の壁ではなかったので、急がなくてよい。順番は `xfb_emulation.md` の 4 章のとおり。transform feedback の X0 → geometry shader の G2 と G1 → transform feedback の X1・X2 → sparse。
- 条件つき描画の C1 は、**ドライバが持つ小さな compute の pipeline**を作ったあとに足すのが安い。その仕組みは transform feedback の X2 と geometry shader の G1 でも使うので、そのどちらかと一緒にやるのがよい。
- それまでは C0（何もしない）で困らない。困るのは、遮蔽の query で「見えるときだけ出す」物を描く OpenGL のアプリだけ。
