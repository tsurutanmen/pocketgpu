#!/bin/bash
# Linux 用（WSL・Steam Deck を含む）を gcc で組む。窓は X11（xcb・Xlib）に JPEG の絵で出す（IPHONE_VK_VIDEO=1 で H.264。FFmpeg の libavcodec でほどく）。読み戻しの遅延は無い版。
# X・libturbojpeg・libavcodec は動くときに dlopen するので、組むときに要るのは gcc と Vulkan のヘッダだけ。
#   vk/build_linux.sh [出す所]                  既定は vk/build_linux
#   vk/build_linux.sh [出す所] icd tests        icd・icd32・tests・server・probe から選ぶ（icd32 は 32 ビットのゲーム用。gcc なら -m32 の libc が要る）（server は Linux で受け手を組む。lavapipe などが受け手になる）
#                                               probe はキットの道具 pgpu-probe（start-pocketgpu.sh と run-on-iphone-gpu.sh が使う）
# 配るキット用に古い glibc に合わせて組むときは LINUX_CC で組む物を替える（pc/make_linux_kit.py は Windows でもこれと同じ zig cc で組む）：
#   LINUX_CC="zig cc -target x86_64-linux-gnu.2.31 -mcpu=baseline" vk/build_linux.sh 出す所 icd probe
# Vulkan のヘッダ：VK_HEADERS=フォルダ があればそれ、無ければ third_party/Vulkan-Headers/include、それも無ければシステムの物
# 使い方（組んだあと）：
#   VK_ICD_FILENAMES=出す所/iphone_icd_linux.json IPHONE_VK=受け手の住所:50061 出す所/vecadd iPhone
#   VK_ICD_FILENAMES=出す所/iphone_icd_linux.json IPHONE_VK=usb 出す所/wsitri iPhone 60      （USB の iPhone・窓に三角形）
set -e
cd "$(dirname "$0")"
OUT=${1:-$PWD/build_linux}
shift || true
WHAT=${*:-icd tests}
if [ -n "$VK_HEADERS" ]; then VKH=-I$VK_HEADERS
elif [ -d ../../third_party/Vulkan-Headers/include ]; then VKH=-I$PWD/../../third_party/Vulkan-Headers/include
else VKH=; fi
CC="${LINUX_CC:-gcc} -O2 -g -Wall -Wextra -Wno-unused-parameter"
mkdir -p "$OUT"
for w in $WHAT; do
    case $w in
    icd)
        # -rdynamic：落ちたときの呼び出し元の名前を出すため（IPHONE_VK_LOG=1）
        $CC -fPIC -shared -fvisibility=hidden -rdynamic $VKH icd/icd.c -o "$OUT/libiphone_icd.so" -lpthread -ldl
        cp -f icd/iphone_icd_linux.json "$OUT/" # library_path は横の libiphone_icd.so
        echo "ドライバ：$OUT/libiphone_icd.so（VK_ICD_FILENAMES=$OUT/iphone_icd_linux.json）"
        ;;
    icd32)
        # 32 ビットのゲーム用（出す所/i386）。zig なら LINUX_CC="zig cc -target x86-linux-gnu.2.31 -mcpu=baseline"
        mkdir -p "$OUT/i386"
        if [ -n "$LINUX_CC" ]; then C32="$CC"; else C32="$CC -m32"; fi
        $C32 -fPIC -shared -fvisibility=hidden -rdynamic $VKH icd/icd.c -o "$OUT/i386/libiphone_icd.so" -lpthread -ldl
        echo "32 ビットのドライバ：$OUT/i386/libiphone_icd.so"
        ;;
    tests)
        # 試験は Windows 用のままの .c を使う。#include <windows.h> は tests/linux_shim/windows.h（薄い置き換え）に当たる
        for t in vecadd tri tri13 copyimg pushdesc copydesc extlist; do
            $CC $VKH -Itests/linux_shim tests/$t.c -o "$OUT/$t" -ldl -l:libvulkan.so.1
        done
        # 窓（X11）の試験と、書き込み監視の速さの試験。X と Vulkan のローダーは dlopen で読む
        $CC $VKH -Itests tests/wsitri.c -o "$OUT/wsitri" -ldl
        $CC $VKH -Itests/linux_shim tests/wwbench.c -o "$OUT/wwbench" -ldl -l:libvulkan.so.1
        echo "試験：$OUT/{vecadd,tri,tri13,copyimg,pushdesc,copydesc,extlist,wsitri,wwbench}"
        ;;
    probe)
        $CC $VKH tools/pgpu_probe.c -o "$OUT/pgpu-probe" -ldl
        echo "キットの道具：$OUT/pgpu-probe"
        ;;
    server)
        $CC -w $VKH -DVR_SERVER_MAIN remote/server.c -o "$OUT/vk_server_linux" -lpthread -ldl -lm -l:libvulkan.so.1
        echo "Linux の受け手：$OUT/vk_server_linux 127.0.0.1 50330"
        ;;
    esac
done
