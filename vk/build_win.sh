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
    server)
        $CC -Wall -Wextra -Wno-unused-parameter $VKH -DVR_SERVER_MAIN remote/server.c -o "$OUT/vk_server_new.exe" \
            -lws2_32 -lole32 -loleaut32 -lwindowscodecs -luuid -ldbghelp
        echo "PC の代役の受け手：$OUT/vk_server_new.exe（動いている vk_server.exe とは別の名前で出す）"
        ;;
    tools)
        $CC -Wall tools/stackdump.c -o "$OUT/stackdump.exe" -ldbghelp
        $CC -Wall $VKH tools/vklist.c -o "$OUT/vklist.exe" $VKLIB
        echo "道具：stackdump・vklist"
        ;;
    tests)
        for t in tri tri13 vecadd extlist; do $CC $VKH tests/$t.c -o "$OUT/$t.exe" $VKLIB; done
        for t in d3d11tri d3d11cube d3d11event; do $CC -Wall -Wno-unused-parameter tests/$t.c -o "$OUT/d3d11/$t.exe" -ld3d11 -ldxguid -luuid -luser32 -lgdi32; done
        # geometry shader の真似の試験（場面と比べ方は tests/gs/gstest.c と cmp.py）
        $CC -Wall -Wno-unused-parameter tests/gs/gstest.c -o "$OUT/d3d11/gstest.exe" -ld3d11 -ldxguid -luuid -luser32 -lgdi32
        echo "試験：tri・tri13・vecadd・extlist・d3d11/d3d11tri・d3d11/d3d11cube・d3d11/d3d11event・d3d11/gstest"
        ;;
    *) echo "知らない名前：$w"; exit 2 ;;
    esac
done
