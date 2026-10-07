# geometry shader を iPhone（MoltenVK）で真似る：調べたことと作り方

2026-10-07。調べた・読んだ・試作した物はすべてこのフォルダの中。D:\iphone-rpc\app の中は読んだだけで、何も変えていない。git は触っていない。

## 0. 先に結論

- Apple の GPU で GS を真似ている実物は、Asahi の Mesa（いまは src/poly）。GS を「出力の頂点ごとに走る頂点シェーダー」に直し、VS は compute で先に走らせる。出す形が決まっている GS では、GS のための compute を省く。
- MoltenVK 本体は 1.4.2 でも GS を持たない。mesh shader で真似る PR はあるが、どれもマージされていない。
- 点を受ける GS なら、VS と GS の SPIR-V を合わせて VS を 1 本作り、描画を「元の点 1 つをインスタンス 1 つ」に変えるだけで済む。compute も新しい buffer も要らない。
- これを PC 側のドライバ（ICD）の中だけで試作した。受け手（server.c）と約束事（proto.h）は変えていない。だから iPhone のアプリを組み直さずに試せる。
- PC の代役の受け手を iPhone のふり（VKR_LIKE_IOS=1）で動かして、FluidCS11 と NBodyGravityCS11 の粒が出た。FluidCS11 の粒の範囲は、ふつうの DirectX の絵と画素が全部一致した。iPhone の実機ではまだ試していない。
- おすすめは、まずこの形（下の B を ICD に置く）を入れること。三角を受ける GS（影・cube map の層・草）は、その次に Mesa と同じ形（A）を受け手に作るかを決める。

## 1. 先例

| 名前 | やり方 | SPIR-V で変換するか | 状態 |
|---|---|---|---|
| Asahi の Mesa（src/poly/nir/poly_nir_lower_gs.c） | VS を compute にして結果を buffer へ。GS は「数える compute」「本当の GS の compute」「間接描画を作る 1x1x1 の compute」と、出力の頂点ごとに GS を走らせ直して自分の番の頂点を選ぶ頂点シェーダー（rast shader）に分ける。形が静的なら compute を省いて、VS の compute と GS の頂点シェーダーだけになる | NIR の上で変換する。SPIR-V ではない | 本番で動いている。OpenGL 4.6・Vulkan 1.3 の適合 |
| MoltenVK 本体 | GS を持たない。要望 #1524 は 2022 年から開いたまま | ― | 1.4.2 の MVKExtensions.def に mesh shader の拡張も無い |
| MoltenVK の PR #1815（CodeWeavers の js6i）と #2786（2026-07） | GS を Metal の object＋mesh の段で動かす。対の SPIRV-Cross の PR は #2200 と #2654 | SPIR-V から MSL を作るときに変換する。compute ではなく mesh | どちらも下書きのまま。SPIRV-Cross の保守者は 2023 年に「tess・GS・XFB の組み合わせを考えていない」と反対し、2026-07-31 にも同じ懸念を書いている |
| Apple の Game Porting Toolkit と Metal shader converter | HLSL の GS と tess を Metal の mesh shader に直す、と MoltenVK の保守者が #1524 に書いている（2023-06-12） | DXIL から。SPIR-V ではない | Apple の資料そのものでは確かめていない |
| Ryujinx | 「頂点シェーダーだけの追加の描画で GS を真似る」と #1524 の書き込み（2022-11-29）が同ブログを引いている | 確かでない | 本家のリポジトリは 2024 年に消えた。中身は読めていない |
| KosmicKrisp（LunarG の Vulkan on Metal・Mesa） | GS はまだ無い。予定には入っている | ― | macOS 向け。iOS で使えるかは確かでない |
| DXVK・VKD3D-Proton | GS を真似ない。GPU が持つ前提。DXVK 3.x は geometryShader を必須にした | ― | app/docs/dxvk3_plan.md の §5 |
| Unity | Metal では geometry 段のある shader を使えない、と説明書にある | ― | 説明書 |
| ANGLE | README の表で Metal は ES 3.0 まで。3.1・3.2 は空欄。GS は 3.2 の機能なので無いとみられる | ― | 表の読み違いがありうる。確かでない |
| Unreal Engine | 調べていない | ― | ― |

「SPIR-V を変換して、compute と頂点の描画に分ける」形にいちばん近いのは Mesa。ただし Mesa は NIR の上でやるので、そのまま借りられない。SPIR-V から SPIR-V へ GS を下ろす既製の道具は見つからなかった。SPIRV-Tools にそういう pass があるかは確かでない（探しきってはいない）。

Mesa の要点。poly_nir_lower_gs.c（commit 4cc14eaa のころ）。
- 664-668 行：GS を「出力の頂点ごとに並んで走る頂点シェーダー」にする create_gs_rast_shader。
- 526-543 行：select_rast_output。EmitVertex の番号が自分の番なら出力を選ぶ。今回の試作と同じ考え方。
- 1167-1182 行：形が静的な GS は、compute を省いて「VS の compute＋GS の頂点シェーダー」だけにする説明。
- 974 行：間接描画を作る pre-GS。495 行：数える shader。

## 2. DXVK が出す GS の SPIR-V（実物で確かめた）

取り方。FluidCS11 を DXVK_SHADER_DUMP_PATH つきで、RTX 直とドライバ越しの両方で動かした。書き出した物は dump_*、dumpicd_* にある。読める形（.txt）は spvdis.py で作った。

### DXVK-macOS 1.10.3（Gcenx の 4948b30）

- SPIR-V 1.3。Logical・GLSL450。
- GS の main は、入力の変数を Private の配列 shader_in に写してから、本体の関数 gs_main を呼ぶ。emitGsFinalize（dxbc_compiler.cpp 7008 行）。
- 入力は、レジスタごとの配列（点なら 1 要素）と、SV_Position のための gl_PerVertex の配列。emitDclInputArray（7177 行）・emitDclInputPerVertex（7210 行）。
- 出力は gl_PerVertex の Block（BuiltIn Position の 1 メンバー）と Location の変数。emitGsInit（6828 行）。
- Emit は「o# を出力の変数へ写す」＋ OpEmitVertex、Cut は OpEndPrimitive。emitGeometryEmit（2273 行）。transform feedback のときだけ流れの番号が付く。
- SV_VertexID は VertexIndex − BaseVertex。emitVsSystemValueLoad（6113 行）。
- stream output だけで GS が無いとき、DXVK が自分で点を流すだけの GS を作る。processXfbPassthrough（203 行）。

### DXVK 3.1.1（b1a1c99。シェーダーは dxbc-spirv の bf14419）

- SPIR-V 1.6。PhysicalStorageBuffer64・Vulkan の記憶モデル。spirv_builder.cpp の emitMemoryModel（2823 行）、spirv_types.h の 15 行。
- 組み込みは Block に入れず、変数 1 つずつ。入出力は署名の要素ごとに Location と Component を付ける。emitDclIoVar（848 行）。
- GS の Emit と Cut は、流れが 1 つなら OpEmitVertex と OpEndPrimitive。emitGsEmit（2699 行）。ただし流れが 1 つでも OpCapability GeometryStreams を宣言していた（FluidCS11 の実物）。
- SV_VertexID は符号つき整数の VertexIndex − BaseVertex。emitBuiltInDrawParameter（889 行）・emitLoadDrawParameterBuiltIn（3395 行）。
- gl_Layer は GS 以外の段なら ShaderLayer の capability を足す。969 行。
- ドライバ越しと RTX 直で、GS の SPIR-V は同じだった。VS は違った（RTX では NVIDIA の raw access chain を使う）。

### よくある形

- 点を四角に広げる：FluidCS11 の GS は InputPoints・OutputTriangleStrip・OutputVertices 4・Invocations 1。4 回の OpEmitVertex のあとに OpEndPrimitive。制御の分かれ道は無い。NBodyGravityCS11 も同じ形で、VS が頂点バッファの COLOR も読む。
- 三角を流すだけ・SV_RenderTargetArrayIndex を書く：手元で組めたサンプルに無かったので、実物の SPIR-V は見ていない。ソースからは、三角の入力は 3 要素の配列になり、Layer は BuiltIn Layer の出力になる、と読める。確かでない。
- 三角を受ける GS は、DirectX SDK の FX11 のサンプル（Tutorial13 の爆発・Instancing の草 maxvertexcount 90・FixedFuncEMU）にある。どれも exe が組まれていなかった。

## 3. 作り方の候補

いまのドライバは、GS を含む pipeline を作らずに失敗を返す（icd.c の create_graphics_pipelines の先頭、13 時半の写しで 1993 行）。受け手は記録の命令を VR_RECORD_COMMAND_BUFFER の時点で本物の command buffer に積み直している（server.c の replay）。

### A. 受け手で「VS を compute・GS を頂点ごとの VS」に分ける（Mesa の形）

- server.c の VR_CREATE_GRAPHICS_PIPELINE で GS を見つけたら、VS を compute に、GS を rast VS に直す。形が動的な GS には、数える compute と index buffer を作る compute も要る。
- VS の compute には、頂点の属性を自分で読む処理（format ごとのほどき）と、index buffer を読む処理が要る。Mesa は agx_nir_lower_vbo でやっている。
- replay の中で、描画の前に dispatch を差す。dispatch は render pass の中に置けないので、VR_CMD_BEGIN_RENDER_PASS か VR_CMD_BEGIN_RENDERING の所で END まで先読みし、GS の描画のぶんの dispatch を render pass の前へ出す。先読みで足りないときは render pass を切って load で始め直す。Apple の GPU はタイルで描くので、切ると重い。
- 一時の buffer を command buffer ごとに持ち、descriptor set を 1 つ後ろに足した pipeline layout で読ませる。後ろに足すなら、アプリの set との互換は保てる。
- SPIR-V を直す道具が受け手に要る。SPIRV-Tools を iOS のアプリに入れるか、自前で書く。
- 約束事：新しい命令は要らないが、caps に印を 1 つ足す（次の空きは 8192）。ドライバは印を見たら、GS の pipeline を断らずに送る。
- 描画の命令：vkCmdDraw を「dispatch（VS）＋ barrier ＋ 描画（必要なら間接）」に置き換える。
- 記録のバッファ：受け手の replay で差し込むので、ドライバの記録は変わらない。受け手が使うメモリの一覧（use_buffer）に一時の buffer を足す。
- 手間：大。iOS のアプリの組み直しが毎回要る。GitHub Actions の残りを食う。
- 効き目：三角・線・点、数が動的な GS、インスタンス、番号つきまで全部。transform feedback もこの上に作れる。

### B. 点を受ける GS を、VS 1 本とインスタンス描画に直す（今回の試作）

- 「点を四角に」の形を見分けるのではなく、点を受ける GS ならどれでも、GS の本体をそのまま使う。
- VS と GS の SPIR-V を 1 本の VS に合わせる。VS の本体は関数として 1 回呼ぶ。GS の本体はそのまま走らせ、OpEmitVertex のたびに「数えた数 <= k なら下書きの出力を本当の出力に写す」。k は gl_VertexIndex。k 番目を出さなかったときは最後の頂点のままなので、帯の三角は潰れて描かれない。何も出さなかったときは画面の外の 1 点。
- 描画：vkCmdDraw(n, 1, fv, fi) を vkCmdDraw(M, n, 0, fv) にする。M は GS の OutputVertices。帯はインスタンスごとに切れる。firstInstance に fv を入れるので、作った VS の中で InstanceIndex が元の VertexIndex と同じ値になり、BaseInstance が元の BaseVertex になる。頂点の属性は binding を INSTANCE にして同じ要素を読む。
- 新しい buffer・compute・render pass の切り直しは要らない。
- 手間：小〜中。試作は gs_emu.c が約 1000 行（ほかに自動で作る表）、ドライバへの差し込みが gs_icd.inc の約 230 行と icd.c の 30 行ほどの書き換え。
- 効き目：点を受ける GS だけ。DirectX SDK の FluidCS11 と NBodyGravityCS11 で効いた。ゲームの粒子や看板（billboard）にこの形がどれだけあるかは確かでない。

### B を置く場所：ドライバ（PC）か受け手（iPhone）か

ドライバに置くのを勧める。
- 受け手と約束事を変えずに済む。受け手から見ると、普通の VS＋PS の pipeline と普通の vkCmdDraw が届くだけ。
- iPhone のアプリを組み直さずに試せる。Actions を使わない。
- ドライバは Windows で zig で組めて、PC の代役（VKR_LIKE_IOS=1）ですぐ確かめられる。
- 受け手に置くと、受け手が draw を書き換えるために pipeline ごとの情報を覚える必要があり、試すたびに iOS の組み直しが要る。

### C. ほか

- C1. 三角を受ける GS も、A を使わず VS だけで真似る。作った VS の中で元の VS を 3 回呼び、頂点の属性と index は shader の中で自分で読む。GS の本体は出力の頂点ごとに丸ごと走るので、M が大きい GS（草の 90）では重い。手間は中。
- C2. MoltenVK を PR #2786 と SPIRV-Cross #2654 で組み直す。マージされていない。保守者が反対している。iOS 向けに MoltenVK を組むのに Actions を使う。今は勧めない。
- C3. 受け手で GS を mesh shader に直す。MoltenVK 1.4.2 は VK_EXT_mesh_shader を持たないので、Vulkan の上からはできない。
- C4. いまのまま GS の描画を飛ばす。

| 候補 | 置き場 | 新しい命令・印 | 描画の変え方 | 手間 | 効く範囲 |
|---|---|---|---|---|---|
| A | 受け手 | 印 1 つ | dispatch＋barrier＋描画 | 大 | GS 全部 |
| B | ドライバ | 無し | vkCmdDraw(M, n, 0, fv) | 小〜中（試作あり） | 点を受ける GS |
| C1 | ドライバか受け手 | 無しか印 1 つ | B と同じ考えで M×プリミティブ数 | 中 | 三角も。ただし重い |
| C2 | MoltenVK | 無し | MoltenVK の中 | 大・不確か | PR の範囲 |

## 4. おすすめと最初の一歩

### 一歩目：B をドライバに入れる（試作は動いた）

icd.c に入れる物。apply_icd_patch.py が、13 時半の icd.c の写しに当てた差分を icd_try/icd_gs.diff に出してある。
- struct cb に 3 つ足す：いまつないでいる GS の pipeline、アプリが決めた形、その形が決まったか。cb_clear で戻す。
- create_shader：GS を持たない GPU のときだけ、Vertex と Geometry の module の SPIR-V を写して持つ（gs_keep）。destroy_shader で捨てる（gs_forget）。
- create_dsl・create_playout・cmd_push：stageFlags に GEOMETRY があれば VERTEX も足す（gs_stages）。作った VS が GS の cbuffer などを読むため。MoltenVK は段ごとに Metal の資源の番号を割るので、これが無いと VS から見えないはず。
- create_graphics_pipelines：GS を断っていた所で gs_emu_pipeline を呼ぶ。VS と GS を合わせた module を作り、段を「作った VS＋PS」に、形を GS の出す形に、頂点の入力を INSTANCE に変えて、普通の道で送る。VS と GS の specialization を合わせる。
- cmd_bind_pipeline：GS を真似た pipeline かを覚える。形が動的な pipeline なら GS の形を送り、外れたらアプリの形に戻す（gs_bind・gs_set_topology）。
- cmd_draw：vkCmdDraw(M, n, 0, fv) を積む（gs_draw）。番号つき・間接・インスタンスつきは、いまは飛ばして 1 回だけ記録に出す。
- 記録のバッファ：描画は記録の時点で書き換えるので、2 次の command buffer を写す cmd_exec もそのまま正しい。GPU が読む範囲の記録は変わらない。

FluidCS11 の粒を出すための最小は、これで全部。受け手は今のままでよい。

### 実機で試す手順（まだやっていない）

1. 今の icd.c に apply_icd_patch.py を当てる。合わない所があれば止まる。
2. gs_emu.c・gs_emu.h・spv_table.h・gs_icd.inc を一緒に組む。下の「組み方」。
3. iPhone の受け手はそのまま。IPHONE_VK_LOG=1 で「geometry shader を頂点シェーダーで真似た」の行を見る。
4. 粒が出なければ、まず MoltenVK が作った VS を受け付けたか（graphics pipeline を作れない、の行）を見る。

### 二歩目より先

- インスタンスつきの描画：頂点の番号に元のインスタンスを入れる（M×インスタンス数の頂点、帯は三角の並びに直す）。
- 番号つきの点の描画：index buffer を shader で読むか、PC で並べ直す。
- EndPrimitive が途中にある GS：三角の並び（list）に直して、帯の切れ目ごとに数え直す。いまは 2 本目からの帯を描かない。
- 三角を受ける GS：C1 か A。SV_RenderTargetArrayIndex は、MoltenVK 1.4.2 が A12 より新しい GPU で shaderOutputLayer を持つので、VS から gl_Layer を書ける（MVKDevice.mm の 2570 行と 2837 行）。どちらにするかは、実物のゲームで GS の形を数えてから決める。

## 5. 試作のファイル

| ファイル | 中身 |
|---|---|
| gs_emu.h・gs_emu.c | VS と GS の SPIR-V から、GS を真似る VS を作る。型の重ね合わせ、入出力の Private への付け替え、Emit の置き換え、入口の作り直し。SPIR-V 1.3 と 1.6 の両方 |
| spv_table.h・gen_spv_table.py | どの語が id かを引く表。SPIR-V の命令表（_ref_spirv.core.grammar.json）から作る |
| gs_icd.inc・apply_icd_patch.py | ドライバへの差し込み。元の icd.c は変えず、写しに当てる |
| icd_try/icd_gs.diff | 13 時半の icd.c の写しとの差分 |
| icd_try/bin1/iphone_icd.dll | 差し込みつきで組んだドライバ。bin0 は差し込み無しで組んだ対照 |
| gs_emu_test.c | .spv の VS と GS から、作った VS を書き出す |
| spv_check.c | 作った VS と PS で RTX に pipeline を作らせ、検証レイヤーの文句を出す |
| spvdis.py | 小さな逆アセンブラ（spirv-dis の代わり） |
| out/ | 作った VS（DXVK 3.1.1 と 1.10.3 の FluidCS11）と、その逆アセンブル |
| dump*/ | DXVK が書き出した SPIR-V |
| shots/ | 撮った絵と記録 |
| _ref_* | 読んだ出典の写し（Mesa・DXVK・dxbc-spirv・命令表） |

組み方（zig は D:/tools/zig-x86_64-windows-0.17.0）。

```
python apply_icd_patch.py <icd.c> icd_try/icd/icd.c
cd icd_try/icd
zig cc -shared -O2 -target x86_64-windows-gnu -I D:/iphone-rpc/third_party/Vulkan-Headers/include \
  -o ../bin1/iphone_icd.dll icd.c ../../gs_emu.c -lws2_32 -lgdi32 -luser32 -lole32 -lwindowscodecs -ldbghelp -loleaut32
```

icd_try の下に remote/proto.h・wire.h と third_party/bcdec を写してある。icd.c の相対の include のため。

## 6. 測った結果

すべて PC の中。受け手は vkbuild の vk_server.exe を写して、VKR_LIKE_IOS=1 で自分の番号 50077 に立てた。終わったら止めた。

| 確かめた事 | 結果 |
|---|---|
| 作った VS を検証レイヤー（spirv-val を含む）と RTX に通す | DXVK 3.1.1 版・1.10.3 版とも、module と graphics pipeline を作れた。誤り 0 件 |
| 対照：わざと壊す | DrawParameters を外した物と、GS をそのまま VS に渡した物は、検証レイヤーが誤りを出した。検証は効いている |
| FluidCS11・DXVK 3.1.1・最初のコマ・左下 240×240 の範囲 | 差し込み無し：基準と 32 より違う画素 52900／57600、赤い画素 0。差し込みあり：違う画素 0、赤い画素 52900。基準のふつうの DirectX も 52900 |
| FluidCS11・DXVK 1.10.3 | 差し込みありで粒が出た（shots/gs_dxvk110.png）。差し込み無しは出ない |
| FluidCS11・200 コマ目 | 粒が流れて、密度の色も付いている（shots/gs_dxvk3_f200.png） |
| NBodyGravityCS11・DXVK 3.1.1 | 初めの配置が毎回変わるので、ふつうの DirectX どうしでも 41598 画素違う（差の平均 19.36）。差し込みありと基準の差は 41745 画素（19.39）で、この揺れと同じ大きさ。差し込み無しは、RTX 直の DXVK の絵との差が 50476 画素（34.49）で、明るい画素は 0。差し込みありは 49440、基準は 49093 |
| GS を使わない SimpleSample11 | 差し込みの有無で絵が 1 画素も変わらない。ただしほぼ一色の絵なので、壊していないことの弱い確かめでしかない |

## 7. 確かでないこと・残り

- iPhone の実機の MoltenVK で、作った VS が通るか。気になる所は 3 つ。作った VS が出力の変数を読み戻す所（SPIRV-Cross の MSL で問題ないはずだが、確かめていない）。BaseInstance（MoltenVK 1.4.2 は shaderDrawParameters を true にしている。MVKDevice.mm の 169 行）。GEOMETRY に VERTEX を足した descriptor set layout の扱い。
- 速さ。点 1 つにつき VS が 4 回走る。FluidCS11 の 16384 粒では軽いはずだが、測っていない。
- 形が動的な pipeline の道（gs_bind・gs_set_topology）は、試したサンプルでは使われず、通っていない。
- インスタンスつき・番号つき・間接の描画、途中の EndPrimitive、transform feedback、GS の instancing は未対応。そのときは描画を飛ばして記録に 1 回出す。
- 文字が出ない件。ドライバ越しでは、差し込みの有無にかかわらず FluidCS11 の文字が出なかった。NBody では出た。GS とは関係が無い。原因は調べていない。
- 押し込んだ push constant の範囲が、アプリの別の範囲と同じ段で重なると、規則違反になる。DXVK では起きないと見ているが、確かめていない。
- ANGLE と Unreal Engine、Apple の Metal shader converter の中身は、一次の資料で確かめていない。
- 試作は 13 時半の icd.c に当てた。別のセッションが icd.c を直し続けているので、今の icd.c に当てると合わない所が出るかもしれない。そのときは apply_icd_patch.py が止まる。

## 8. 出典

Web。
- MoltenVK の GS の要望 https://github.com/KhronosGroup/MoltenVK/issues/1524
- MoltenVK の PR https://github.com/KhronosGroup/MoltenVK/pull/1815 ・ https://github.com/KhronosGroup/MoltenVK/pull/2786
- SPIRV-Cross の PR https://github.com/KhronosGroup/SPIRV-Cross/pull/2200 ・ https://github.com/KhronosGroup/SPIRV-Cross/pull/2654
- MoltenVK v1.4.2（db66022）の MoltenVK/MoltenVK/Layers/MVKExtensions.def（102 行 shader_draw_parameters・181 行 viewport_index_layer・mesh shader は無し）と GPUObjects/MVKDevice.mm（169・2570・2782・2836-2837 行）
- Mesa の poly https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/poly 。写しは _ref_poly_nir_lower_gs.c
- Asahi の記事（2024-02-14）https://alyssarosenzweig.ca/blog/conformant-gl46-on-the-m1.html 。GS・tess・transform feedback を compute にする、とある
- KosmicKrisp https://developer.blender.org/docs/features/gpu/vulkan/kosmic_krisp/ ・ https://www.lunarg.com/wp-content/uploads/2025/10/XDC-2025-KosmicKrisp-Overview.pdf
- Unity の説明書 https://docs.unity3d.com/2021.1/Documentation/Manual/SL-ShaderCompileTargets.html
- ANGLE の README https://chromium.googlesource.com/angle/angle/+show/master/README.md
- Metal shader converter https://developer.apple.com/metal/shader-converter/ （GS の記述はこの頁では見つからなかった）

ソース。
- DXVK-macOS 4948b30 の src/dxbc/dxbc_compiler.cpp。写しは _ref_dxvk110_dxbc_compiler.cpp。行は §2
- DXVK 3.1.1 b1a1c99 と dxbc-spirv bf14419 の spirv/spirv_builder.cpp・spirv/spirv_types.h。写しは _ref_dxbcspirv_*
- このプロジェクト：D:\iphone-rpc\app\vk\icd\icd.c（create_graphics_pipelines・create_shader・create_dsl・create_playout・cmd_bind_pipeline・cmd_push・cmd_draw・cmd_set_topology・struct cb）、D:\iphone-rpc\app\vk\remote\server.c（replay・VR_CREATE_GRAPHICS_PIPELINE・VR_RECORD_COMMAND_BUFFER）、proto.h、app/docs/dxvk3_plan.md の §5
- サンプル：D:\iphone-rpc\vkbuild\d3d11samples\FluidCS11\FluidRender.hlsl・NBodyGravityCS11\ParticleDraw.hlsl
