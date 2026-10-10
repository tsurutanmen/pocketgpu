#!/bin/bash
# Android 用の受け手（server.c）を NDK の clang で組む。Visual Studio も Android Studio も要らない。
#   build_android.sh [出す所]       既定は D:/iphone-rpc/work/android/out
# arm64（実機）と x86_64（エミュレーター）の 2 つを出す。-g で組んで、実行ファイルの横に .debug は付けない（adb push は strip しない）
set -e
NDK=${NDK:-/d/android-sdk/ndk/27.3.13750724}
BIN=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin
API=${API:-30}
cd "$(dirname "$0")"
OUT=${1:-/d/iphone-rpc/work/android/out}
VKH=-I/d/iphone-rpc/third_party/Vulkan-Headers/include
mkdir -p "$OUT"
for a in aarch64 x86_64; do
    # libvulkan.so は Android に最初からある。-llog は logcat 用
    "$BIN/${a}-linux-android${API}-clang.cmd" -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable \
        -Wno-unused-function $VKH -DVR_SERVER_MAIN remote/server.c -o "$OUT/server_$a" -lvulkan -llog -lm
    echo "受け手（$a）：$OUT/server_$a"
done
