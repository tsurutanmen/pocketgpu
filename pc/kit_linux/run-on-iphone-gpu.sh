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
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"
set -u
self=$(readlink -f -- "${BASH_SOURCE[0]}")
KIT=$(dirname -- "$self")
JSON="$KIT/lib/x86_64/iphone_icd.x86_64.json"
PROBE="$KIT/bin/pgpu-probe"

ja=0
case "${PGPU_LANG:-${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}}" in ja*) ja=1 ;; esac
M() { if [ "$ja" = 1 ]; then printf '%s\n' "$1"; else printf '%s\n' "$2"; fi; }

pgpu_env() {
    export IPHONE_VK="${IPHONE_VK:-usb}"
    if [ "${IPHONE_VK_ONLY:-1}" = 1 ]; then
        # VK_DRIVER_FILES is read by Vulkan loaders from 1.3.207, VK_ICD_FILENAMES by older ones.
        # Steam's container, pressure-vessel, reads both on the host and passes only this driver into the container.
        export VK_DRIVER_FILES="$JSON"
        export VK_ICD_FILENAMES="$JSON"
        unset VK_ADD_DRIVER_FILES
    else
        export VK_ADD_DRIVER_FILES="$JSON${VK_ADD_DRIVER_FILES:+:$VK_ADD_DRIVER_FILES}"
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
    fi
}

is_usb() { case "${IPHONE_VK:-usb}" in usb | usb:*) return 0 ;; *) return 1 ;; esac; }

# The USB helper hands the usbmuxd connection to the driver inside Steam's container, where /run/usbmuxd cannot be seen.
# It does not copy any data. It stops when this script ends.
start_broker() {
    BROKER_PID=
    is_usb || return 0
    [ -x "$PROBE" ] || return 0
    "$PROBE" broker-check > /dev/null 2>&1 && return 0
    "$PROBE" mux > /dev/null 2>&1 || return 0
    "$PROBE" broker --quiet > /dev/null 2>&1 &
    BROKER_PID=$!
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        "$PROBE" broker-check > /dev/null 2>&1 && return 0
        sleep 0.1
    done
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
    M "USB の補助プログラムを起動します。iPhone の GPU を使う間は、このウィンドウを閉じないでください。終了するときは Ctrl+C を押してください。" \
        "Starting the USB helper. Keep this window open while you use the iPhone GPU. Press Ctrl+C to stop it."
    exec "$PROBE" broker
    ;;
esac

pgpu_env
start_broker
"$@"
rc=$?
[ -n "$BROKER_PID" ] && kill "$BROKER_PID" 2> /dev/null
exit $rc
