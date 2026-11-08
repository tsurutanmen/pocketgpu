# d3d12_rootcbv：帳簿に載らない形の壊れを見せる DirectX 12 の試験

`research\uab_tracking.md` の U0。今の作りで「違う」が出ることを先に確かめ、直したあとに「正しい」になることを確かめるための試験。

## 4 つの場面が拾う壊れ方（1 行ずつ）

- **rootcbv**：root の CBV（vkd3d-proton は Apple ではアドレスで渡す）で読む upload の値が、ドライバの「読む範囲」に載らず、submit の前に受け手へ送られない壊れ（uab_tracking.md の筋書き 3）。
- **rootsrv**：root の SRV（vkd3d-proton はどの会社でもアドレスで渡す）で読む upload の buffer が、同じく送られない壊れ（筋書き 3。代役の NVIDIA でも起きるはず）。
- **heapafter**：heap の set を結んだあと（dispatch を記録したあと）に書いた descriptor の buffer が、ドライバの「読む範囲」と受け手の「使うメモリ」に載らず、送られない壊れ（筋書き 1・2）。
- **readback**：root の UAV で CPU から見える buffer（CUSTOM heap）に GPU が書いた値が、ドライバの「書く範囲」に載らず、fence のあとも CPU に読み戻されない壊れ（筋書き 3 の 4）。

## 組み方

```
export PATH="/d/tools/zig-x86_64-windows-0.17.0:$PATH"
zig cc -target x86_64-windows-gnu -O2 -g -Wall -Wextra -Wno-unused-parameter d3d12_rootcbv.c -o build/d3d12_rootcbv.exe
```

- `app\vk\build_win.sh` の d3d12test と同じ指定（14 行の `CC` と 60 行の旗）。
- 2026-10-10 に組んで、警告 0（同じ指定で、わざと警告を 2 つ入れたファイルを組むと 2 つ出ることも確かめた）。

## 走らせ方

```
d3d12_rootcbv.exe [rootcbv|rootsrv|heapafter|readback|all]
```

- 横に vkd3d-proton の `d3d12.dll` と `d3d12core.dll`、それと `d3dcompiler_47.dll` を置く（d3d12test と同じ）。無ければ Windows のふつうの DirectX 12 で動く（全部「正しい」になるはず。試験そのものが正しいかの確かめになる）。
- 出し方は d3d12test と同じ：「情報 …」「値 …」「場面 名前：正しい／違う」、最後に「結果 全部正しい」か「結果 n 件違う」（n は違ったコマの数の合計）。
- 違ったコマは、はじめの 5 つを「何コマ目・入っていた値・何の値か（前のコマの値・次のコマの値・0（届いていない）・別のコマの値・ほかの値）」で書く。

### 切り替え

| 環境変数 | 意味 |
|---|---|
| `D3D12_ROOTCBV_FRAMES=数` | 1 場面のコマの数（標準 60・最大 240） |
| `D3D12_ROOTCBV_SPIN=数` | GPU に回らせる回数（標準 20000）。GPU の仕事を少し長くして、早めの「終わった」と上書きがぶつかりやすくする |
| `D3D12_ROOTCBV_TRACK=1` | **対照**。rootcbv・rootsrv は同じ buffer を heap の表からも指す（帳簿に載る形）。heapafter は descriptor を結ぶ**前**に書く。readback は同じ buffer を heap の表の UAV（u1）からも指し、**シェーダーがそこにも同じ値を書く**（2026-10-10 15:14 に足した） |

- readback の対照で「書く」まで入れる理由（事実）：ドライバは、表で指しただけの UAV は読み戻さない。結んだ pipeline のシェーダーがその場所（set と binding）に書くときだけ「書く範囲」にする（`app\vk\icd\icd.c` の `wr_may_cb`、6862〜6876 行・`wr_resolve`、6878〜6885 行）。指すだけの対照では、帳簿に載らず、対照にならない。
| `D3D12_ROOTCBV_DEBUG=1` | 調べる層（ふつうの DirectX 12 だけ） |

### 結果の読み方（推測。走らせて確かめる）

| 場面 | Windows のふつうの DX12 | 代役（NVIDIA と答える） | 代役（`VKR_LIKE_IOS_VENDOR=1`） | 本物の iPhone |
|---|---|---|---|---|
| rootcbv | 正しい | 正しい見込み（root の CBV は push descriptor） | 違う見込み | 違う見込み |
| rootsrv | 正しい | 違う見込み | 違う見込み | 違う見込み |
| heapafter | 正しい | 違う見込み | 違う見込み | 違う見込み |
| readback | 正しい | 違う見込み | 違う見込み | 違う見込み |

- 2026-10-10 の代役の結果（iPhone RPC のセッションが走らせた。readback の対照を足す前の版）：NVIDIA と答える代役は rootcbv だけ正しく、ほかの 3 つが違う（180 件）。Apple と答える代役は 4 つとも違う（240 件・値は 0 のまま）。Apple と答える代役の対照では、rootcbv・rootsrv・heapafter が正しく、readback だけ違う（60 件）。readback は、その版には対照が無く、ふつうと同じ動きだった。
- 対照（`D3D12_ROOTCBV_TRACK=1`）で「正しい」、ふつうで「違う」なら、試験は「帳簿に載らない形」の壊れを見分けている。
- 対照でも「違う」なら、別の原因（試験の書き間違い・別の壊れ）を先に疑う。
- ふつうの DirectX 12 で「違う」なら、試験の書き間違い。

## 作りの注意

- rootcbv・rootsrv の値は、1 MB の upload の buffer の真ん中（512 KB の所）に置く。ほかの物と同じページに乗って、たまたま一緒に受け手へ送られる道を避けるため。d3d12test の bind 場面は、root の CBV と表の CBV が同じ buffer の同じページにあり、表の側で送られるので、Apple と答えても正しく出る形になっていると思う（推測）。
- heapafter は、vkd3d-proton が descriptor の表を dispatch のときに結ぶことを前提に、dispatch を記録したあと・submit の前に descriptor を書く。root signature 1.0 の descriptor は volatile なので、D3D12 の決まりの中。CBV が指す所はコマごとに 4 KB ずつ離した別のページ。
- readback の値は root の定数（帳簿に載る道）で渡し、読み戻しだけを試す。対照では、root の UAV と表の UAV の両方に同じ値を同じ所へ書く（同じ resource の 2 つの見え方。値も場所も同じなので、結果は変わらない）。
- どの場面も、1 コマごとに submit して fence を待つ（d3d12test の `run_and_wait` と同じ）。答えは rootcbv・rootsrv・heapafter は最後にまとめて読み戻し（コピーは帳簿に載る道）、readback はコマごとに CPU で直接読む。
