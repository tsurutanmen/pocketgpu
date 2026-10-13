#!/usr/bin/env bash
# PocketGPU: run one program or game on the iPhone GPU.
#   ./run-on-iphone-gpu.sh vkcube
#   ./run-on-iphone-gpu.sh /path/to/game [arguments...]
#   Steam: put this line in the game's Properties > Launch Options:
#       "/path/to/this/folder/run-on-iphone-gpu.sh" %command%
#   ./run-on-iphone-gpu.sh --broker   keep the USB helper running in this terminal (Flatpak Steam, see README.txt)
#   ./run-on-iphone-gpu.sh --env      print the settings it uses, as shell lines
# Only the program started from here sees the iPhone GPU. Nothing is installed and nothing is written outside this folder.
# You can change these before starting it:
#   IPHONE_VK_ONLY=0      also show the PC's own GPUs. By default only the iPhone GPU is shown, so games cannot pick another GPU.
#   IPHONE_VK=host:port   connect over the network instead of USB. The default is usb.
#   IPHONE_VK_LOG=1       print what the driver does. With Proton, add PROTON_LOG=1 to get it in ~/steam-<appid>.log.
#   IPHONE_VK_VIDEO=1     receive the window picture as H.264 instead of JPEG. Needs FFmpeg's libavcodec on this PC.
#   IPHONE_VK_KEEP_WAYLAND=1  keep WAYLAND_DISPLAY. By default it is removed when X11 or Xwayland is there, so programs open X11 windows.
#
# USB の補助プログラム（受け渡し役）：Steam の入れ物の中のゲームは /run/usbmuxd が見えないので、外で動く pgpu-probe broker が
# usbmuxd につないだ口を渡す。起動のたびに、名前（PGPU_BROKER）と合言葉（PGPU_BROKER_TOKEN）を新しく作り、
# そのゲームにだけ環境変数で渡す。受け渡し役は、同じ利用者で同じ合言葉を知っている相手にだけ口を渡す（vk/tools/pgpu_broker.h）。
# 起動ごとに別の受け渡し役なので、2 本目のゲームを同時に起動しても、1 本目が終わったときに 2 本目の口が無くなることはない。
# --broker（Flatpak の Steam 用）は、名前は pocketgpu-usbmuxd-<uid> のまま、合言葉を tools/broker-token（この利用者だけが読める）に書く。
# Flatpak の中の run-on-iphone-gpu.sh はそれを読んでゲームに渡す。--broker を止めると消す。
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"
set -u
self=$(readlink -f -- "${BASH_SOURCE[0]}")
KIT=$(dirname -- "$self")
JSON="$KIT/lib/x86_64/iphone_icd.x86_64.json"
JSON32="$KIT/lib/i386/iphone_icd.i386.json"
PROBE="$KIT/bin/pgpu-probe"
TOKEN_FILE="$KIT/tools/broker-token"

ja=0
case "${PGPU_LANG:-${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}}" in ja*) ja=1 ;; esac
M() { if [ "$ja" = 1 ]; then printf '%s\n' "$1"; else printf '%s\n' "$2"; fi; }

pgpu_env() {
    export IPHONE_VK="${IPHONE_VK:-usb}"
    # 32-bit games use the i386 driver. The Vulkan loader skips the driver of the other architecture.
    local files="$JSON"
    [ -r "$JSON32" ] && files="$JSON:$JSON32"
    if [ "${IPHONE_VK_ONLY:-1}" = 1 ]; then
        # VK_DRIVER_FILES is read by Vulkan loaders from 1.3.207, VK_ICD_FILENAMES by older ones.
        # Steam's container, pressure-vessel, reads both on the host and passes only this driver into the container.
        export VK_DRIVER_FILES="$files"
        export VK_ICD_FILENAMES="$files"
        unset VK_ADD_DRIVER_FILES
    else
        export VK_ADD_DRIVER_FILES="$files${VK_ADD_DRIVER_FILES:+:$VK_ADD_DRIVER_FILES}"
    fi
    # DXVK and vkd3d-proton in Proton pick the GPU whose name contains this text
    export DXVK_FILTER_DEVICE_NAME="${DXVK_FILTER_DEVICE_NAME:-iPhone}"
    export VKD3D_FILTER_DEVICE_NAME="${VKD3D_FILTER_DEVICE_NAME:-iPhone}"
    # The driver shows windows through X11 or Xwayland only. Turn off the gamescope layer that changes X11 windows into Wayland ones
    export ENABLE_GAMESCOPE_WSI=0
    export DISABLE_GAMESCOPE_WSI=1
    if [ -n "${DISPLAY:-}" ]; then
        export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
        export SDL_VIDEO_DRIVER="${SDL_VIDEO_DRIVER:-x11}"
        # GLFW 3.4, winit, Qt, GTK and others open Wayland windows when WAYLAND_DISPLAY is set, and the driver cannot draw those. Make them use X11 or Xwayland
        [ "${IPHONE_VK_KEEP_WAYLAND:-0}" = 1 ] || unset WAYLAND_DISPLAY
    fi
}

is_usb() { case "${IPHONE_VK:-usb}" in usb | usb:*) return 0 ;; *) return 1 ;; esac; }

# The USB helper hands the usbmuxd connection to the driver inside Steam's container, where /run/usbmuxd cannot be seen.
# It does not copy any data. Each start gets its own helper with a new name and a new secret, given only to this program.
# It stops when this script ends.
start_broker() {
    BROKER_PID=
    unset PGPU_BROKER PGPU_BROKER_TOKEN
    is_usb || return 0
    [ -x "$PROBE" ] || return 0
    if ! "$PROBE" mux > /dev/null 2>&1; then
        # usbmuxd is not reachable here, for example inside Flatpak Steam: use the helper started with --broker, if any
        if [ -r "$TOKEN_FILE" ]; then
            PGPU_BROKER_TOKEN=$(head -c 32 "$TOKEN_FILE")
            export PGPU_BROKER_TOKEN
        fi
        return 0
    fi
    local tok rnd
    tok=$("$PROBE" token) || return 0
    rnd=$("$PROBE" token) || return 0
    PGPU_BROKER="pocketgpu-usbmuxd-$(id -u)-${rnd:0:12}"
    PGPU_BROKER_TOKEN=$tok
    export PGPU_BROKER PGPU_BROKER_TOKEN
    "$PROBE" broker --quiet > /dev/null 2>&1 &
    BROKER_PID=$!
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        "$PROBE" broker-check > /dev/null 2>&1 && return 0
        kill -0 "$BROKER_PID" 2> /dev/null || break
        sleep 0.1
    done
    # The helper did not start. The program can still use usbmuxd directly when it runs outside a container.
    [ -n "$BROKER_PID" ] && kill "$BROKER_PID" 2> /dev/null
    BROKER_PID=
    unset PGPU_BROKER PGPU_BROKER_TOKEN
}

run_broker_here() {
    if ! "$PROBE" mux > /dev/null 2>&1; then
        M "エラー：usbmuxd に接続できません。iPhone を USB で接続して、./start-pocketgpu.sh でチェックしてください。" \
            "ERROR: cannot connect to usbmuxd. Plug the iPhone in with USB and check with ./start-pocketgpu.sh." >&2
        exit 1
    fi
    unset PGPU_BROKER
    if [ -r "$TOKEN_FILE" ] && PGPU_BROKER_TOKEN=$(head -c 32 "$TOKEN_FILE") "$PROBE" broker-check > /dev/null 2>&1; then
        M "USB の補助プログラムは、すでに別のウィンドウで起動しています。" "The USB helper is already running in another window."
        exit 0
    fi
    local tok
    tok=$("$PROBE" token) || exit 1
    if ! (umask 077 && printf '%s\n' "$tok" > "$TOKEN_FILE.new" && mv -f "$TOKEN_FILE.new" "$TOKEN_FILE"); then
        M "エラー：$TOKEN_FILE に書き込めません。キットを、ホームフォルダなど書き込める場所に展開してください。" \
            "ERROR: cannot write $TOKEN_FILE. Extract the kit in a place you can write to, such as your home folder." >&2
        exit 1
    fi
    M "USB の補助プログラムを起動します。iPhone の GPU を使う間は、このウィンドウを閉じないでください。終了するときは Ctrl+C を押してください。" \
        "Starting the USB helper. Keep this window open while you use the iPhone GPU. Press Ctrl+C to stop it."
    PGPU_BROKER_TOKEN=$tok "$PROBE" broker &
    local bp=$!
    trap 'kill "$bp" 2> /dev/null; rm -f "$TOKEN_FILE"' EXIT
    trap 'exit 130' INT TERM HUP
    wait "$bp"
    local rc=$?
    # 3：名前が使われている（同じ利用者の別のプログラムが先に取った）
    [ $rc = 3 ] && M "エラー：USB の補助プログラムを起動できません。ほかのプログラムが同じ名前を使っています。PC を再起動してから、もう一度試してください。"         "ERROR: cannot start the USB helper. Another program uses the same name. Restart the PC and try again." >&2
    exit $rc
}

usage() {
    M "使い方：./run-on-iphone-gpu.sh コマンド 引数..." "Usage: ./run-on-iphone-gpu.sh command arguments..."
    M "Steam のゲームは、プロパティの「起動オプション」に次の 1 行を入力してください：" "For a Steam game, put this line in Properties > Launch Options:"
    printf '    "%s" %%command%%\n' "$self"
    M "最初に ./start-pocketgpu.sh で、必要な項目をチェックしてください。" "Run ./start-pocketgpu.sh first to check what is needed."
}

if [ ! -r "$JSON" ]; then
    M "エラー：$JSON がありません。キットをすべて展開し直してください。" "ERROR: $JSON is missing. Extract the whole kit again." >&2
    exit 1
fi
[ -x "$PROBE" ] || chmod +x "$PROBE" 2> /dev/null

case "${1:-}" in
"" | -h | --help)
    usage
    exit 2
    ;;
--env)
    pgpu_env
    for k in IPHONE_VK VK_DRIVER_FILES VK_ICD_FILENAMES VK_ADD_DRIVER_FILES DXVK_FILTER_DEVICE_NAME VKD3D_FILTER_DEVICE_NAME ENABLE_GAMESCOPE_WSI DISABLE_GAMESCOPE_WSI SDL_VIDEODRIVER SDL_VIDEO_DRIVER; do
        [ -n "${!k+x}" ] && printf 'export %s=%q\n' "$k" "${!k}"
    done
    exit 0
    ;;
--broker)
    run_broker_here
    ;;
esac

pgpu_env
start_broker
"$@"
rc=$?
[ -n "$BROKER_PID" ] && kill "$BROKER_PID" 2> /dev/null
exit $rc
