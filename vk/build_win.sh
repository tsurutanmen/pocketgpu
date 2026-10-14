#!/bin/bash
# Windows 用を zig cc で組む（Visual Studio は要らない）。
#   vk/build_win.sh [出す所]        既定は D:/iphone-rpc/vkbuild
#   vk/build_win.sh [出す所] icd    ドライバだけ（icd・server・tools・tests から選ぶ。並べてもよい）
# 関数名を記録に出すので、どれも -g で組み、.pdb を横に置く
set -e
export PATH="/d/tools/zig-x86_64-windows-0.17.0:$PATH"
cd "$(dirname "$0")"
OUT=${1:-/d/iphone-rpc/vkbuild}
shift || true
WHAT=${*:-icd server tools tests}
VKH=-I/d/iphone-rpc/third_party/Vulkan-Headers/include
VKLIB=/c/Windows/System32/vulkan-1.dll
CC="zig cc -target x86_64-windows-gnu -O2 -g"
mkdir -p "$OUT" "$OUT/d3d11"
for w in $WHAT; do
    case $w in
    icd)
        # winmm は present の区切り（timeBeginPeriod）に使う
        $CC -Wall -Wextra -Wno-unused-parameter -shared $VKH icd/icd.c -o "$OUT/iphone_icd.dll" \
            -lws2_32 -lgdi32 -luser32 -ldbghelp -lole32 -loleaut32 -lwindowscodecs -luuid -lwinmm
        echo "ドライバ：$OUT/iphone_icd.dll"
        ;;
    icd32)
        # 32 ビットのアプリ用（Unigine Heaven など）。$OUT/x86 に置く。stdcall の @数字 を外した名前で書き出すため icd32.def を渡す
        mkdir -p "$OUT/x86"
        zig cc -target x86-windows-gnu -O2 -g -Wall -Wextra -Wno-unused-parameter -shared $VKH icd/icd.c icd/icd32.def -o "$OUT/x86/iphone_icd.dll" \
            -lws2_32 -lgdi32 -luser32 -ldbghelp -lole32 -loleaut32 -lwindowscodecs -luuid -lwinmm
        cp -f "$OUT/iphone_icd.json" "$OUT/x86/" # 中身は同じ（横の iphone_icd.dll を指す）
        echo "32 ビットのドライバ：$OUT/x86/iphone_icd.dll"
        ;;
    server)
        $CC -Wall -Wextra -Wno-unused-parameter $VKH -DVR_SERVER_MAIN remote/server.c -o "$OUT/vk_server_new.exe" \
            -lws2_32 -lole32 -loleaut32 -lwindowscodecs -luuid -ldbghelp
        echo "PC の代役の受け手：$OUT/vk_server_new.exe（動いている vk_server.exe とは別の名前で出す）"
        ;;
    tools)
        $CC -Wall tools/stackdump.c -o "$OUT/stackdump.exe" -ldbghelp
        $CC -Wall $VKH tools/vklist.c -o "$OUT/vklist.exe" $VKLIB
        # 窓を主のモニターに一瞬も出さずにサブモニターへ出す起動の道具（iphone-vk.cmd が IPHONE_VK_MONITOR のときに使う）
        $CC -Wall -municode ../win/subwin.c -o "$OUT/subwin.exe" -luser32
        $CC -Wall -shared ../win/subwin_hook.c -o "$OUT/subwin_hook.dll" -luser32
        rm -f "$OUT/subwin_hook.lib"
        # 32 ビットのアプリ用（64 ビットの subwin.exe が x86 に任せる）
        mkdir -p "$OUT/x86"
        zig cc -target x86-windows-gnu -O2 -Wall -municode ../win/subwin.c -o "$OUT/x86/subwin.exe" -luser32
        zig cc -target x86-windows-gnu -O2 -Wall -shared ../win/subwin_hook.c -o "$OUT/x86/subwin_hook.dll" -luser32
        rm -f "$OUT/x86/subwin_hook.lib" "$OUT/x86/subwin.pdb" "$OUT/x86/subwin_hook.pdb"
        echo "道具：stackdump・vklist・subwin（x86 も）"
        ;;
    tests)
        for t in tri tri13 vecadd pushdesc copydesc copyimg extlist; do $CC $VKH tests/$t.c -o "$OUT/$t.exe" $VKLIB; done
        for t in d3d11tri d3d11cube d3d11event; do $CC -Wall -Wno-unused-parameter tests/$t.c -o "$OUT/d3d11/$t.exe" -ld3d11 -ldxguid -luuid -luser32 -lgdi32; done
        # DirectX 9・10 の小さな試験（tests/d3d9tri.c・d3d10tri.c）。横に DXVK の d3d9.dll か d3d10core.dll・d3d11.dll・dxgi.dll を置くとドライバ越し。32 ビット版は x86 に出す（Need for Speed Carbon のような 32 ビットのゲームの代わり）
        mkdir -p "$OUT/dx910/x64" "$OUT/dx910/x86"
        $CC -Wall -Wno-unused-parameter tests/d3d9tri.c -o "$OUT/dx910/x64/d3d9tri.exe" -ld3d9 -luser32 -lgdi32
        $CC -Wall -Wno-unused-parameter tests/d3d10tri.c -o "$OUT/dx910/x64/d3d10tri.exe" -ld3d10 -ldxguid -luuid -luser32 -lgdi32
        zig cc -target x86-windows-gnu -O2 -g -Wall -Wno-unused-parameter tests/d3d9tri.c -o "$OUT/dx910/x86/d3d9tri.exe" -ld3d9 -luser32 -lgdi32
        zig cc -target x86-windows-gnu -O2 -g -Wall -Wno-unused-parameter tests/d3d10tri.c -o "$OUT/dx910/x86/d3d10tri.exe" -ld3d10 -ldxguid -luuid -luser32 -lgdi32
        # geometry shader の真似の試験（場面と比べ方は tests/gs/gstest.c と cmp.py）
        $CC -Wall -Wno-unused-parameter tests/gs/gstest.c -o "$OUT/d3d11/gstest.exe" -ld3d11 -ldxguid -luuid -luser32 -lgdi32
        # DirectX 11 の機能を 1 つずつ試す 29 の場面（feat.exe list・比べ方は tests/feat/cmp_all.py）
        $CC -Wall -Wno-unused-parameter tests/feat/feat.c -o "$OUT/d3d11/feat.exe" -ld3d11 -ldxguid -luuid -luser32 -lgdi32
        # DirectX 12 の小さな試験（tests/d3d12/d3d12test.c）。横に vkd3d-proton の d3d12.dll と d3d12core.dll を置けばドライバ越し、無ければふつうの DirectX 12
        mkdir -p "$OUT/d3d12"
        $CC -Wall -Wextra -Wno-unused-parameter tests/d3d12/d3d12test.c -o "$OUT/d3d12/d3d12test.exe"
        echo "試験：tri・tri13・vecadd・pushdesc・copydesc・copyimg・extlist・d3d11/d3d11tri・d3d11/d3d11cube・d3d11/d3d11event・d3d11/gstest・d3d11/feat・d3d12/d3d12test"
        ;;
    *) echo "知らない名前：$w"; exit 2 ;;
    esac
done
