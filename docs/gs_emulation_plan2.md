# geometry shader の真似 第 2 段：番号つき・インスタンスつき・三角を受ける GS

2026-10-07。作った物・試した物はすべて scratchpad の gs2 の中。D:\iphone-rpc\app の中は読んだだけで、何も変えていない。git は触っていない。

## 0. 先に結論

- 第 1 段（B）の横に、C1 の形を作った。作った VS が index と頂点の buffer を storage buffer として自分で読み、元の VS をプリミティブの頂点の数だけ呼び、GS の本体を走らせて、自分の番の頂点を選ぶ。
- 描画ごとの値（index の場所・BaseVertex・各 binding の場所と間隔など）は、ドライバが 256 バイトの「記録」に書く。作った VS はそれを、アプリの pipeline layout の後ろに 1 つ足した set の dynamic uniform buffer として読む。BDA は使わない。
- 受け手（server.c）と約束事（proto.h）は変えていない。iPhone のアプリの組み直しは要らない。
- できるようになった物：番号つき（16・32 ビット・StartIndex・負の BaseVertex）、インスタンスつき（インスタンスごとの頂点の属性も）、GS の instancing（[instance(n)]）、線・三角・隣つき（triangleadj・lineadj）を受ける GS、帯の途中の RestartStrip（出力を並びに直す）、SV_RenderTargetArrayIndex（VS から gl_Layer を書く）。
- PC の代役（VKR_LIKE_IOS=1）で、試験の 8 場面のうち 7 場面と、重さの 6 場面が、DXVK 1.10.3 と 3.1.1 の両方で、ふつうの DirectX（RTX）と画素まで一致した。残る 1 場面は「番号つきの帯の途中の restart の値」で、分かっている限界（§6）。対照（第 1 段だけのドライバ）は、この 6 場面で 1 画素も描かない。
- 重さ（RTX・代役）：GS の無い対照はドライバ越しでも DXVK 直と同じ。三角 3 つを出す GS は 1.74 倍、層 4 枚に出す GS（並びに直る）は 1.93 倍。iPhone の GPU では測っていない。
- iPhone の実機ではまだ試していない。

## 1. index buffer を VS から読む方法の比較

| 案 | 中身 | 良い所 | 悪い所 |
|---|---|---|---|
| descriptor を足す（採った） | layout の後ろに set を 1 つ。binding 0 記録（dynamic uniform）、1 index、2+b 頂点の buffer b（どれも storage・offset 0・全体） | Vulkan 1.0 の範囲で済む。受け手を変えない。BDA が要らない | アプリの pipeline layout ごとに、set を足した layout をもう 1 つ作る。描画ごとに set を 1 つつなぐ。頂点・index の buffer に STORAGE_BUFFER の使い道を足す |
| BDA | 記録に buffer のアドレスを書く | set を作り直さずに済む | 全部のメモリに DEVICE_ADDRESS の印と、buffer に使い道が要る。DXVK 1.10 は BDA を使わない。iPhone の受け手で BDA が通るかは確かめていない（DXVK 3 は装置を作る所まで）。記録の場所を渡す道はやはり要る |
| push constant | 範囲を伸ばして値を入れる | 描画ごとの値を入れやすい | 同じ段の範囲は 1 つまで、という規則にぶつかる。SPIR-V の push constant の変数は入口ごとに 1 つまでなので、VS と GS の block を合わせる必要がある。DXVK 3 の PS が push constant を使う |
| CPU で並べ直す | 記録の時点で index を読み、頂点を並べた buffer を作る | shader が単純 | GPU が書いた index、記録から submit までの書き換えに追いつけない。CPU に写しの無いメモリ（device local）は読めない |

描画ごとの値の道は、dynamic な uniform buffer の offset を使う。記録は 256 バイト（minUniformBufferOffsetAlignment の上限）ごとに並べる。

## 2. 作り方

### 2.1 記録（gs_emu.h の GSR_*）

| 語 | 中身 |
|---|---|
| 0 | 1：番号つき。2：帯の番号つき（restart の値を見る） |
| 1 | index buffer の中で最初の index の場所（バイト）＝bind の offset ＋ firstIndex × index の大きさ |
| 2 | 番号つきなら vertexOffset、番号なしなら firstVertex |
| 3 | firstInstance |
| 4 | 1 インスタンスあたりのプリミティブの数 P |
| 5・6 | index 1 つのバイト数と、取り出しの印 |
| 7 | 並び・帯・扇 |
| 8+2b・9+2b | binding b の bind の offset と間隔（動的な間隔なら vkCmdBindVertexBuffers2 の値、そうでなければ pipeline の値） |

### 2.2 描き方

元の描画（番号つきでも、なしでも）を `vkCmdDraw(D, P × インスタンスの数 × GS の instancing, 0, 0)` にする。D は 1 インスタンスの頂点の数（帯なら GS の OutputVertices の M、並びなら 3×(M−2)、線の並びなら 2×(M−1)）。
作った VS の中では InstanceIndex から（GS の instancing の番号・プリミティブ i・元のインスタンス j）を割り算で取り出す。VertexIndex が受け持つ頂点（並びなら三角の番号と角）。firstVertex と firstInstance は 0 なので、BaseVertex・BaseInstance は要らない。

### 2.3 作る VS（gs_emu.c の prologue_pull）

1. 記録を読む。
2. 入力のプリミティブの頂点ごと（点 1・線 2・三角 3・隣つきの線 4・隣つきの三角 6）に、入力の流れの中の場所を出す。三角の帯は Vulkan の決まり（i 番は i, i+1+i%2, i+2−i%2）、扇は (i+1, i+2, 0)。
3. 番号つきなら index を storage buffer から読む（16 ビットは語の中から取り出す）。番号なしなら 0 番地を読んで捨てる（読む先がいつも正しい buffer になるように、index buffer が無いときは記録の buffer をつなぐ）。
4. VS の組み込みの入力（VertexIndex・InstanceIndex・BaseVertex・BaseInstance・DrawIndex）と、頂点の属性を Private の変数に入れる。属性は pipeline の頂点の入力の形（形式・offset・binding・インスタンスごとか）を見て、storage buffer から読んでほどく。
   - ほどける形式：R8・R8G8・R8G8B8A8・B8G8R8A8 の UNORM・SNORM・USCALED・SSCALED・UINT・SINT、A2B10G10R10 の同じ 6 種、R16・R16G16・R16G16B16A16 の 6 種と SFLOAT、R32 の 1〜4 成分の UINT・SINT・SFLOAT。ほかの形式なら C1 の pipeline を作らない。
5. 元の VS を関数として呼び、出力を GS の入力の配列の c 番に写す。2〜5 を頂点の数だけ繰り返す。
6. GS の PrimitiveId に i、InvocationId に GS の instancing の番号を入れて、GS の本体を走らせる。

### 2.4 出力の選び方

- 帯（第 1 段と同じ）：Emit のたびに「数えた数 <= k」なら写す。足りなければ最後の頂点が残り、三角は潰れる。
- 並び（list_mode）：帯の途中で RestartStrip する GS（文の順で Cut のあとに Emit がある、またはループの中で Cut する）は、出力を三角の並びに直す。Emit のたびに直前 2 つの頂点の出力を写しておき、帯の中の j 番の三角が受け持ちの三角なら、角を (w_j, w_{j+1+j%2}, w_{j+2−j%2}) から選ぶ。線は (w_j, w_{j+1})。どの三角にもならない受け持ちは、3 つの角がどれも画面の外の同じ 1 点になって描かれない。
- ループの中かは、OpLoopMerge からその merge の block までの間か、で見る。DXVK 1.10 は GS の本体を別の関数に置くので、「別の関数の中で Cut」を理由にすると FluidCS11 まで並びになってしまう。そうしないようにした（FluidCS11 は今も B）。

### 2.5 B と C1 の使い分け（ドライバ）

GS を含む pipeline を作るとき、両方を作れたら両方作る。

- B：点を受ける GS で、インスタンスごとの頂点の入力が無く、帯の途中で切らないとき。アプリに返すのは B の pipeline。番号なしでインスタンス 1 つの描画は B で描く。
- C1：set を足した layout があれば作る。番号つき・インスタンスつきの描画は、C1 の pipeline に付け替えて描き、B に戻す。B が無い GS（線・三角など）は、C1 をそのままアプリに返す。
- 描画のたびに、記録を書き、set（記録・index・頂点の buffer の組が変わったときだけ新しく取る）をつなぎ、描く。アプリが同じ番号に set をつないでいたら戻す。
- 記録の buffer（64 KiB）と descriptor pool（64 set）は command buffer ごとに持ち、記録し直すときに使い回す。記録は submit のときに、ほかの CPU のメモリと同じ道で受け手に送られる（cb_reads）。

### 2.6 icd.c に足す物（apply_gs2.py・14 か所）

| 場所 | 足す物 |
|---|---|
| struct cb | `struct gs_cbst *gsx` |
| cb_clear・cb_free | 記録の buffer と pool を使い回す・捨てる |
| create_buffer | 受け手が GS を持たないとき、頂点・index の buffer に STORAGE_BUFFER を足す |
| create_playout・destroy_playout | 後ろに set を 1 つ足した layout も作る（アプリが set の形を先に消してもよいように、この時に作る）・消す |
| destroy_pipeline・destroy_shader | 覚えた物を捨てる。C1 の pipeline も消す |
| cmd_bind_sets・cmd_bind_vbs・cmd_bind_vbs2・cmd_bind_ib | つないだ set・頂点・index の buffer を覚える |
| cmd_draw_indexed | 番号つきの描画を C1 で描く（第 1 段は飛ばしていた） |

gs フォルダの gs_emu.c・gs_emu.h・gs_icd.inc は置き換え、spv_table.h はそのまま。

## 3. DXVK が出す GS の典型（ソースで確かめた）

- DXVK 1.10.3（_ref_dxvk110_dxbc_compiler.cpp）
  - 入力の形は ExecutionMode で、点・線・三角・隣つきの線・隣つきの三角（1270〜1293 行）。入力は頂点の数の配列と、gl_PerVertex の配列。
  - SV_RenderTargetArrayIndex は、GS では BuiltIn Layer の uint の出力の変数（6374 行から）。GS 以外の段なら shader_viewport_index_layer を使う。
  - [instance(n)] は Invocations（1421 行）。Emit は「出力を写す＋OpEmitVertex」、Cut は OpEndPrimitive（2273 行）。
  - GS の main は入力を Private に写してから本体の関数を呼ぶ（7008 行）。今回の試験の層の GS は、本体の関数の中に OpLoopMerge のループがあった。
- DXVK 3.1.1（dxbc-spirv の spirv_builder.cpp）
  - Layer・ViewportIndex は GS 以外の段なら ShaderLayer・ShaderViewportIndex の capability を足す（969〜981 行）。Invocations は 4063 行、TrianglesAdjacency は 4077 行。
  - 入出力は要素ごとの Location と Component（VS の出力の uint 2 つを 1 つの Location に詰めていた）。
  - dump の SPIR-V では set 1・2 を使っていたのに、ドライバに届いた pipeline layout は 0〜3 個の set だった。pipeline を作るときに番号を付け直しているとみられる（ソースでは確かめていない）。
- DXVK 自身の GS（GitHub の v1.10.3 の src/dxvk/dxvk_meta_blit.cpp と shaders/dxvk_fullscreen_geom.geom）
  - 層のある画像の blit などで、VK_EXT_shader_viewport_index_layer が無いときだけ、三角を受けて gl_Layer を書く GS（max_vertices 3）を使う。拡張があれば VS で層を書く。iPhone でこの拡張が無いなら、C1 でも層は書けないので、この GS を真似る意味は無い。
- 試験の場面と、ゲームでよく見る形の対応
  - cube map を 1 回で描く：ループで層ごとに 3 つ出して Cut（layer の左半分）、または [instance(n)]（右半分）。
  - 影の押し出し：triangleadj を受けて輪郭の辺から四角を出す（adj。輪郭の辺を太い四角で出す形で試した）。
  - 爆発・面の向きの色：三角を受けて 3 つ出す（tri）。
  - 粒の四角：点を受ける（pidx・pinst・FluidCS11・NBody）。

## 4. 試したこと（すべて PC の中）

受け手は vkbuild の vk_server_new.exe を gs2/bin に写し、VKR_LIKE_IOS=1 で自分の番号 50093 に立てた。終わったら止めた。絵は窓を作らずに、絵の外の的に描いて読んだ（DXUT のサンプルだけ、窓をサブモニターに出した）。

### 4.1 作った VS を検証レイヤーに通す

DXVK の dump の VS・GS・PS から C1 の VS を作り、RTX に pipeline を作らせた（tests/spv_check.exe・検証レイヤーに spirv-val を含む）。DXVK 1.10.3 と 3.1.1 の 14 組で誤り 0 件。わざと StorageBuffer を Uniform に書き換えた物は誤り 2 件で、検証は効いている。

### 4.2 試験の場面（tests/gstest.c）

| 場面 | 中身 | DXVK 1.10.3 | DXVK 3.1.1 | 対照（第 1 段） |
|---|---|---|---|---|
| pidx | 点の GS・番号なし 1 回、16 ビット（StartIndex 5）、32 ビット（buffer の 8 バイト先・StartIndex 2・BaseVertex 3） | 一致 | 一致 | 0 画素 |
| pinst | 点の GS・インスタンスつき（インスタンスごとの属性・StartVertex 4・StartInstance 1）、番号つき＋インスタンスつき | 一致 | 一致 | 0 画素 |
| tri | 三角の GS・番号つきの立方体・面の向きで色・SV_PrimitiveID | 一致 | 一致 | 0 画素 |
| cut | 三角から 2 つの三角（間で RestartStrip）・三角の帯の入力・線の入力 | 一致 | 一致 | 0 画素 |
| layer | 4 層の配列に描く：ループ＋RestartStrip（M 12・並び）と [instance(4)]（M 3） | 一致 | 一致 | 0 画素 |
| adj | triangleadj・輪郭の辺を太い四角で出す（M 12・並び） | 一致 | 一致 | 0 画素 |
| fmt | 半精度・10-10-10-2・BGRA・SNORM8・SINT32・UNORM16・UINT8・SNORM16、2 つ目の buffer の offset、負の BaseVertex | 一致 | 一致 | 345 画素（基準 2636） |
| strip | 番号つきの三角の帯。2 回目は途中に 0xffff | 275 画素違う | 275 画素違う | ― |

「一致」は、どの画素もどの色も差が 0（検算の値も同じ）。ふつうの DirectX と、DXVK で RTX に直接描いた絵も、全場面で画素まで同じだった。
strip の違いは右下（restart の後ろの帯）だけで、形は同じ。restart の後の偶奇と SV_PrimitiveID がずれて、プリミティブ番号で決まる内側の色が違う（§6）。
最後に、14:51 の icd.c に台本を当てて組み直した物でも、DXVK 1.10.3 で 7 場面（strip を除く）が一致した。

### 4.3 前からのサンプル（壊していないか）

| サンプル | DXVK | 新しいドライバと今のドライバ | 新しいドライバと DirectX |
|---|---|---|---|
| FluidCS11 | 1.10.3・3.1.1 | 画素の差 0 | 画素の差 0 |
| SimpleSample11 | 1.10.3・3.1.1 | 画素の差 0 | 画素の差 0 |
| NBodyGravityCS11 | 1.10.3・3.1.1 | 差 32 より大きい画素 41318・41593 | 41635・41706 |

NBody は初めの粒の配置が毎回変わるので、どの組でも同じ大きさだけ違う（今のドライバと DirectX でも 41693・41589）。絵を見て、粒の雲が出ていることを確かめた。FluidCS11 も NBody も B で描かれている。

### 4.4 重さ（RTX・代役・1 コマに 64 回描く・2 回測った平均）

| 場面 | 1 回の描画 | DirectX | DXVK で RTX 直 | ドライバ越し（真似） | ドライバ越し ÷ DXVK 直 |
|---|---|---|---|---|---|
| GS なし（対照） | 三角 32768 | 0.54 ms | 0.63 ms | 0.64 ms | 1.02 |
| 三角 3 つを出す GS（帯） | 三角 32768 | 1.02 | 1.08 | 1.88 | 1.74 |
| 層 4 枚に出す GS（ループ・並び） | 三角 32768 | 3.93 | 3.95 | 7.63 | 1.93 |
| [instance(4)] の GS | 三角 32768 | 3.97 | 4.20 | 6.55 | 1.56 |
| 点を四角にする GS（番号なし・B） | 点 262144 | 8.82 | 10.14 | 12.98 | 1.28 |
| 同じ・番号つき（C1） | 点 262144 | 10.83 | 13.50 | 13.40 | 0.99 |

- 対照が同じなので、差はほぼ真似の分。どの場面の絵も DirectX と画素まで一致した。
- 1 つの三角あたりの仕事：三角 3 つの GS は VS 9 回＋GS 3 回（本物は VS 3 回＋GS 1 回）。層 4 枚は VS 90 回＋GS 30 回。
- RTX は本物の GS がもともと遅いので、比が小さく出ている見込みがある。Apple の GPU で同じ比になるかは分からない。

### 4.5 見えたこと（GS とは関係ない・調べていない）

重さの試験で、D3D11 の EVENT のクエリの GetData が、ドライバ越しでは返ってこなかった（DirectX と DXVK 直では返る）。ドライバの VkEvent は記録の時点で立つ作りなので、それと関係があるかもしれない。試験は 1 画素を staging に写して Map する待ち方に変えた。

## 5. 当て方・組み方

```
python gs2/apply_gs2.py <今の icd.c> <書き出す icd.c>    # 第 1 段が入っている icd.c に当てる。合わなければ・もう当ててあれば止まる
gs2/gs の gs_emu.c・gs_emu.h・gs_icd.inc（と spv_table.h）を icd/gs/ に置く
zig cc -target x86_64-windows-gnu -O2 -g -Wall -Wextra -Wno-unused-parameter -shared -I<Vulkan-Headers>/include icd.c -o iphone_icd.dll \
  -lws2_32 -lgdi32 -luser32 -ldbghelp -lole32 -loleaut32 -lwindowscodecs -luuid -lwinmm
```

- gs2/build.sh がこれをまとめてやる（`build.sh <icd.c> <出す dll のフォルダ>`）。
- 14:51 の icd.c は timeBeginPeriod を使うので -lwinmm が要る（差し込みとは関係ない）。
- 受け手はそのまま。iPhone では IPHONE_VK_LOG=1 で「geometry shader を頂点シェーダーで真似た：B …・C1 …」の行を見る。C1 が「pipeline を作れない」なら、MoltenVK が作った VS を受け付けていない。

## 6. 確かでないこと・残り

- iPhone の実機で試していない。気になる所：
  - 頂点の段で storage buffer と dynamic な uniform buffer を読むこと、set の数が 1 つ増えること（MoltenVK の maxBoundDescriptorSets は kMVKMaxDescriptorSetCount。値は見ていない。DXVK 1.10 は set 1 つなので、足すのは 1 番）。
  - Metal の段ごとの buffer の上限 31。argument buffer を使わない設定だと、記録・index・頂点の buffer の分だけ増える。
  - 層を書く VS：MoltenVK の写しでは、A12（Apple5）より新しい GPU で layeredRendering が真になり、shaderOutputLayer がそれに従う（MVKDevice.mm 2607・2874 行）。どの版の MoltenVK の写しかは確かめていない。
  - 頂点・index の buffer に STORAGE_BUFFER の使い道を足すことの影響。Metal では使い道で buffer は変わらないはずだが、確かめていない。
- 番号つきの帯で restart の値（0xffff など）があると、またぐ三角は描かないが、その後の三角の偶奇と SV_PrimitiveID が本物とずれる（strip で 275 画素）。正しくするには、index の流れの中で直前の restart を探す必要がある。DXVK は帯で restart を有効にするので、帯＋番号つき＋GS では起きうる。
- 間接の描画（DrawInstancedIndirect・DrawIndexedInstancedIndirect）は飛ばす。描画の数を CPU が知らないので、C1 の P が決まらない。間接の buffer を compute で読み替える（A の形）か、受け手で読む必要がある。
- 隣つきの三角の帯、パッチの入力、transform feedback、0 番以外の流れ、UAV に書く VS・GS、動的な頂点の入力、divisor が 1 でないインスタンスの属性（ドライバは拡張を見せていない）、§2.3 に無い頂点の形式は、C1 を作らない（GS の描画は飛ぶ）。
- 試験で通っていない道：動的な形（vkCmdSetPrimitiveTopology）、C1 の set と同じ番号にアプリの set があったときに戻す所、2 次の command buffer、扇の入力（D3D11 に無い）、8 ビットの index。
- 並びに直す GS は、受け持ちの数が 3×(M−2) になる。M の大きい GS（草の maxvertexcount 90 など）は重い。どれだけ重いかは iPhone で測っていない。
- 4 GiB を超える buffer の中の場所は扱わない（記録は 32 ビット）。
- 受け手が GS を持たないとき、アプリの pipeline layout をすべて 2 つずつ作る。GS を含む pipeline は B と C1 の 2 つを作る（compile が 2 回）。
- C1 の set を差したあと、アプリが同じ番号につないでいた set は戻すが、その set を別の layout でつないでいた場合に、それより前の番号の set を乱すかは確かめていない。

## 7. ファイル（gs2 の中）

| ファイル | 中身 |
|---|---|
| gs/gs_emu.c・gs_emu.h | VS と GS の SPIR-V から、GS を真似る VS を作る。B と C1。gs_emu_build2 が C1 |
| gs/gs_icd.inc | ドライバへの差し込み。B と C1 の pipeline、記録、set、描画 |
| gs/spv_table.h・gen_spv_table.py | 第 1 段のまま |
| apply_gs2.py | icd.c に当てる台本（14 か所） |
| build.sh | 当てて組む |
| plan2.md | これ |
| tests/gstest.c | DirectX 11 の試験（8 場面と重さの 6 場面） |
| tests/run_gs.sh・run_samples.sh・cmp.py・sheet.py | 3 通りで動かす台本、絵を比べる・並べる |
| tests/gs_emu_test2.c・spv_check.c・spvdis.py | C1 の VS を作って書き出す、検証レイヤーで確かめる、逆アセンブル |
| shots/ | 撮った絵と記録。native が基準、v2_110・v2_3・v3_110・v4_110 が新しいドライバ、ctl110・ctl3・b0_* が対照 |
| dump/ | 試験の場面の DXVK の SPIR-V と、作った VS |
| base/ | 当てる前の icd.c の写し（14:19 と 14:51）と第 1 段の gs |
| bin/ | 受け手の写し、組んだ dll（b0 対照・b1 14:19 版に当てた物・b2 14:51 版に当てた物） |
| samples/ | DXUT のサンプル 3 つと要る素材だけの写し（DXVK の dll つき） |
