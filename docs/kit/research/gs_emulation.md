# geometry shader を真似る設計の下調べ（2026-10-10）

印：**[事実]** はソースや一次の文書で確かめたこと。**[推測]** は私の推論。外の例の多くは調べ物の担当が要約器経由で読んだもので、その旨を書く。
うちのソースは読むだけで、書き換えていない。行番号は 2026-10-10 13 時台の作業コピーのもの。

## 0. 先に：うちにはもう仕組みがある

- [事実] `app\vk\icd\gs\` に、GS を「頂点シェーダー 1 本」で真似る仕組みがある（`gs_emu.c`・`gs_emu.h`・`gs_icd.inc`）。先例は Mesa の `src/poly/nir/poly_nir_lower_gs.c` の `create_gs_rast_shader`（`gs_emu.c` 1〜3 行の説明）。Mesa は NIR の上で、うちは SPIR-V の上でやる。
- [事実] 入口は pipeline を作る所：受け手の GPU が GS を持たないときだけ `gs_emu_pipeline` に回す（`icd.c` 3487〜3493 行）。受け手は何も変えない（`gs_icd.inc` 1〜3 行）。
- [事実] 形は 2 つ（`gs_emu.h`）。
  - B：点を受ける GS・番号なし・インスタンス 1 つの描画だけ。
  - C1：作った VS が index と頂点の buffer を storage buffer として自分で読む。番号つき・インスタンスつき・線や三角を受ける GS・GS の instancing・帯の途中の RestartStrip まで。描画ごとの値は 256 バイトの「記録」に書き、pipeline layout の後ろに足した set で読ませる。
- [事実] 名乗り：新しい DXVK には geometryShader を名乗る。zink には名乗らない（`mask_features`、`icd.c` 845〜850 行）。理由はコメントにある：名乗ると zink は四角形や頂点の順の真似に GS を作り、10/7 は PrimitiveId の出力で受け手の GPU が止まった。
- [事実] 847〜848 行のコメントは「GS を使う pipeline は作らずに失敗を返す」と書いている。今は 3489 行で真似に回すので、このコメントは古い可能性がある（ファイルの更新時刻：`gs/` の中は 10/7、`icd.c` は 10/10）。

## 1. 今の穴（断っている所・飛ばしている所）

| 穴 | 場所 | 何が起きるか |
|---|---|---|
| 間接の描画 | `gs_icd.inc` 630〜636 行（`icd.c` 7114 行から呼ぶ） | [事実] 描画を飛ばし、最初の 1 回だけ記録に出す |
| transform feedback つきの GS | `gs_emu.c` 1149 行 | [事実] 真似を断る → pipeline を作れない |
| 0 番以外の stream に出す GS | `gs_emu.c` 1210 行 | [事実] 断る |
| GS が buffer や画像に書く | `gs_emu.c` 492〜498 行 | [事実] 断る |
| OutputVertices が 1024 を超える・instancing が 32 を超える | `gs_emu.c` 1161〜1162 行 | [事実] 断る |
| テッセレーションと一緒の GS | `gs_icd.inc` 264 行 | [事実] 断る |
| 動的な頂点の入力（VK_DYNAMIC_STATE_VERTEX_INPUT_EXT） | `gs_icd.inc` 278 行 | [事実] 断る |
| GS が書く PrimitiveId | `gs_emu.c` 1268〜1270 行 | [事実] VS からは出せないので捨てる。[推測] PS が読む gl_PrimitiveID は、展開したあとの描画の番号になる |
| 重さ | 方式そのもの | [推測] 出力の頂点ごとに GS の本体を走らせるので、出力 N 頂点なら本体の計算は約 N 倍。測っていない |

## 2. 案

### G1：間接の描画を真似る

- やり方：間接の引数（`VkDrawIndirectCommand` など）は GPU の中にある。ドライバが作る小さな compute で、引数を「真似た描画の引数」（頂点の数＝1 インスタンスの頂点の数、インスタンスの数＝プリミティブの数 × インスタンスの数 × GS の instancing）に書き直し、記録（`GSR_*`）の値も GPU で書く。そのあと `VR_CMD_DRAW_INDIRECT` で描く。
- 入れる場所：`gs_skip`（`gs_icd.inc` 630〜636 行）を、上の処理に置き換える。間接の描画の入口は `draw_indirect`（`icd.c` 7112〜7121 行）。compute の SPIR-V は `gs/spv_table.h` と同じく語の並びで埋め込む。
- 難しさ：中。[推測] 小さな compute の pipeline を持つ仕組みは、transform feedback の X2 と同じ物を使い回せる。
- 落とし穴：[推測] `vkCmdDrawIndirectCount`（数も GPU の中）は、今は `NOT_YET` のまま（`icd.c` 8643 行）。G1 の範囲外。
- 確かめ方：Vulkan CTS の `dEQP-VK.geometry.*` のうち間接の描画を使う物。DXVK のゲームで、GS を使う物体が間接の描画のときに消えていないか。

### G2：zink にも geometryShader を名乗る（OpenGL 3.2 の入口）

- やり方：`icd.c` 850 行の `!g_engine_zink` を外す。その前に、10/7 に止まった原因（PrimitiveId の出力）が今の `gs_emu.c` 1268〜1270 行の扱いで直っているかを確かめる。
- 入れる場所：`icd.c` 845〜850 行。
- 難しさ：小（試すだけなら 1 行）。ただし受け手の GPU が止まる恐れがあるので、試す順番が大事。[推測]
- 落とし穴
  - [事実] コメントによると、zink は名乗ると四角形（GL_QUADS）や頂点の順（provoking vertex）の真似に GS を作る。GS を使わないアプリでも、GS の真似を通るようになる。重さが増える。[推測]
  - [事実・要約経由] zink の OpenGL 3.2 には depthClamp と VK_EXT_depth_clip_enable も要る。どちらもうちは名乗れる（MoltenVK の depthClamp は真、depth clip はドライバが名乗る：`icd.c` 929 行）。3.0 の transform feedback は別の話（`xfb_emulation.md`）。conditional rendering は、zink が拡張無しでも名乗るので壁ではない（`cond_render.md`）。
- 確かめ方：先に PC の代役（受け手の代わりに PC の GPU で動かす物）で、zink の OpenGL 3.2 の簡単なアプリを走らせる。そのあと iPhone で。

### G3：compute で前もって形を作る（重い GS と、transform feedback つきの GS のため）

- やり方：GS を compute として走らせて、出力の頂点と index を buffer に書き、ふつうの描画で描く。出力の数が呼び出しごとに違う GS は、数える段と書く段の 2 回に分け、間を prefix sum でつなぐ。
- 先例
  - Mesa poly：出力の数と形が前もって決まるかで分ける。決まらない物（DYNAMIC_INDEXED）は、数える compute と index buffer を書く処理を使う（[要約経由] `poly_nir_lower_gs.c`）。
  - Asahi：古い方式は VS と GS を 1 つの compute にまとめていた。新しい方式は、compute の前処理で index buffer だけ作り、頂点は VS で 1 つずつ描く。理由は、メモリの量を GS の出力ではなく VS の出力に比例させるため（[要約経由・抜粋] asahi/mesa の MR 256）。
  - VMware の特許：VS の出力を一時の buffer に書き、GS を compute で並列に走らせ、使わなかった index を -1（primitive restart）で埋める（[要約経由] US11227425B2）。
- 入れる場所：新しく作る。今の C1 の「記録」と set を足す仕組み（`gs_icd.inc` 141〜183 行・437〜535 行）は使い回せる。[推測]
- 難しさ：大。[推測]
- 落とし穴：[推測] 出力の上限ぶんのメモリを先に取る必要がある。増幅する GS では大きくなる。パスの間の仕切り（barrier）が増える。
- 使いどころ：transform feedback つきの GS（`gs_emu.c` 1149 行の穴）を埋めるとき、ほぼ必須になる。出力の数が変わる GS で、前の呼び出しが何個書いたかを数え上げるため。[推測]

### G4：Metal の mesh shader に写す（参考。勧めない）

- やり方：VS を object 関数、GS を mesh 関数に写す。DXMT がこの方式（[要約経由] `src/airconv/dxbc_converter_gs.cpp`）。
- 勧めない理由：[推測] うちの受け手は MoltenVK を通している。MoltenVK に mesh shader の Vulkan 拡張があるかは確かめていないが、無ければ受け手で Metal を直接使う大きな作り替えになる。mesh shader の上限（頂点 256・プリミティブ 512 という二次の情報）と、対応する機種も確定できていない。

### 参考：レイヤーと viewport

- [事実] うちの真似は、GS が gl_Layer か gl_ViewportIndex を書くとき、VS からそれを書く拡張（`SPV_EXT_shader_viewport_index_layer`）を足す（`gs_emu.c` 1191〜1194 行・1328 行・1345 行）。
- [事実・Apple の文書] Metal は頂点の出力の `[[viewport_array_index]]` で viewport を選べる（Apple family 5 以上）。同じプリミティブの頂点は同じ番号を返す必要がある。
- [事実・Apple の文書] Metal の vertex amplification は、同じ頂点を複数の slice や viewport に描く（cube map の 1 回描きなど）のに使える。
- [二次] iOS で頂点シェーダーから slice を選ぶと、全部 slice 0 に描かれるという報告がある（Unity のフォーラム）。うちで確かめるなら、レイヤーつきの描画の簡単な試験が要る。

## 3. 外の例

| 例 | やり方の要点 | 確かめ方 | 出典 |
|---|---|---|---|
| Mesa poly | 出力の頂点ごとに VS が走り、自分の番を選ぶ（うちの先例）。出力が動的なら compute で数えて index を作る | 要約経由 | https://fuchsia.googlesource.com/third_party/mesa/+/refs/heads/gitlab/main/src/poly/nir/poly_nir_lower_gs.c |
| Asahi | compute の前処理で index buffer だけ作り、頂点は VS で描く | 要約経由（抜粋） | https://gitlab.freedesktop.org/asahi/mesa/-/merge_requests/256 |
| DXMT | D3D11 の GS を Metal の mesh パイプラインに写す | 要約経由 | https://raw.githubusercontent.com/3Shain/dxmt/main/src/airconv/dxbc_converter_gs.cpp |
| Apple の Game Porting Toolkit | GS を「翻訳」すると講演で話している。仕組みは公開されていない | 要約経由 | https://developer.apple.com/videos/play/wwdc2023/10123 |
| VMware の特許 | VS の出力を一時の buffer に書き、GS を compute で走らせる | 要約経由 | https://patents.google.com/patent/US11227425B2/en |
| Metal の vertex amplification | 同じ頂点を複数の slice・viewport に描く | Apple の文書を読んだ | https://developer.apple.com/documentation/metal/improving-rendering-performance-with-vertex-amplification.md |
| Metal の viewport の選択 | 頂点の出力で viewport を選ぶ | Apple の文書を読んだ | https://developer.apple.com/documentation/metal/rendering-to-multiple-viewports-in-a-draw-command.md |
| MoltenVK | GS は無い。GS を足す PR や fork は見つからなかった | 要約経由 | https://raw.githubusercontent.com/KhronosGroup/MoltenVK/main/Docs/MoltenVK_Runtime_UserGuide.md |

見つからなかったもの：Mesa の原文の行番号、Asahi の MR の本文、DXMT の GS の動作状況、D3DMetal の仕組み、VS 方式と compute 方式の速さの比較。調べ物のメモの全文は作業用の場所の `notes_gs.md` にある。

## 4. 3 つのうち、どれから手を付けるか

1. **transform feedback の X0**（`xfb_emulation.md`）：小さく、OpenGL 3.0 の入口の 1 つ。
2. **geometry shader の G2 と G1**：G2 は試すだけなら 1 行で、OpenGL 3.2 の入口になる。ただし受け手の GPU が止まる恐れがあるので、PC の代役で先に試す。G1 は DXVK のゲームで GS の物体が消えるのを防ぐ。
3. **transform feedback の X1・X2**。
4. **sparse**（`sparse_emulation.md`）：S0（環境変数）は数行なので、12_0 を名乗らせたいなら順番と関係なくすぐできる。S1 から先は最後。
5. **geometry shader の G3**：transform feedback つきの GS を埋めるときに一緒にやる。

理由：[推測] 名乗りの壁を先に越えると、動くアプリの数が増える。中身の真似は、実際に使うアプリで試せるようになってからのほうが確かめやすい。
