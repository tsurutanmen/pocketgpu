#!/bin/bash
# Linux 用（WSL・Steam Deck を含む）を gcc で組む。窓（swapchain）・動画・書き込み監視は無く、計算と「描いて読み戻す」所だけ動く版。
#   vk/build_linux.sh [出す所]                  既定は vk/build_linux
#   vk/build_linux.sh [出す所] icd tests        icd・tests・server から選ぶ（server は Linux で受け手を組む。lavapipe などが受け手になる）
# Vulkan のヘッダ：VK_HEADERS=フォルダ があればそれ、無ければ third_party/Vulkan-Headers/include、それも無ければシステムの物
# 使い方（組んだあと）：
#   VK_ICD_FILENAMES=出す所/iphone_icd_linux.json IPHONE_VK=受け手の住所:50061 出す所/vecadd iPhone
set -e
cd "$(dirname "$0")"
OUT=${1:-$PWD/build_linux}
shift || true
WHAT=${*:-icd tests}
if [ -n "$VK_HEADERS" ]; then VKH=-I$VK_HEADERS
elif [ -d ../../third_party/Vulkan-Headers/include ]; then VKH=-I$PWD/../../third_party/Vulkan-Headers/include
else VKH=; fi
CC="gcc -O2 -g -Wall -Wextra -Wno-unused-parameter"
mkdir -p "$OUT"
for w in $WHAT; do
    case $w in
    icd)
        # -rdynamic：落ちたときの呼び出し元の名前を出すため（IPHONE_VK_LOG=1）
        $CC -fPIC -shared -fvisibility=hidden -rdynamic $VKH icd/icd.c -o "$OUT/libiphone_icd.so" -lpthread -ldl
        cp -f icd/iphone_icd_linux.json "$OUT/" # library_path は横の libiphone_icd.so
        echo "ドライバ：$OUT/libiphone_icd.so（VK_ICD_FILENAMES=$OUT/iphone_icd_linux.json）"
        ;;
    tests)
        # 試験は Windows 用のままの .c を使う。#include <windows.h> は tests/linux_shim/windows.h（薄い置き換え）に当たる
        for t in vecadd tri tri13 copyimg pushdesc copydesc extlist; do
            $CC $VKH -Itests/linux_shim tests/$t.c -o "$OUT/$t" -ldl -l:libvulkan.so.1
        done
        echo "試験：$OUT/{vecadd,tri,tri13,copyimg,pushdesc,copydesc,extlist}"
        ;;
    server)
        $CC -w $VKH -DVR_SERVER_MAIN remote/server.c -o "$OUT/vk_server_linux" -lpthread -ldl -lm -l:libvulkan.so.1
        echo "Linux の受け手：$OUT/vk_server_linux 127.0.0.1 50330"
        ;;
    esac
done
