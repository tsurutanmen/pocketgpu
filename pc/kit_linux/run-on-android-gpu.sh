#!/usr/bin/env bash
# PocketGPU for Android: run one program or game on the Android phone GPU. Run ./start-android.sh first.
#   ./run-on-android-gpu.sh vkcube
#   ./run-on-android-gpu.sh /path/to/game [arguments...]
#   Steam: put this line in the game's Properties > Launch Options:
#       "/path/to/this/folder/run-on-android-gpu.sh" %command%
#   ./run-on-android-gpu.sh --env      print the settings it uses, as shell lines
# It sets the same things as run-on-iphone-gpu.sh, and then starts the program through it. The only differences:
#   - the driver connects to 127.0.0.1:50300, the port that start-android.sh opened, instead of the iPhone over USB
#   - DXVK_FILTER_DEVICE_NAME is PocketGPU, because the phone GPU is shown as "PocketGPU phone (...)"
#   - no USB helper is started. The connection to 127.0.0.1 also works inside Steam's container and in Flatpak Steam.
# Only the program started from here sees the phone GPU. Nothing is installed and nothing is written outside this folder.
# You can change these before starting it:
#   PGPU_PORT=50301       the port given to start-android.sh. The default is 50300.
#   IPHONE_VK_ONLY=0      also show the PC's own GPUs. By default only the phone GPU is shown, so games cannot pick another GPU.
#   IPHONE_VK_LOG=1       print what the driver does. With Proton, add PROTON_LOG=1 to get it in ~/steam-<appid>.log.
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"
set -u
self=$(readlink -f -- "${BASH_SOURCE[0]}")
KIT=$(dirname -- "$self")
RUN="$KIT/run-on-iphone-gpu.sh"

ja=0
case "${PGPU_LANG:-${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}}" in ja*) ja=1 ;; esac
M() { if [ "$ja" = 1 ]; then printf '%s\n' "$1"; else printf '%s\n' "$2"; fi; }

PORT=${PGPU_PORT:-50300}
case "$PORT" in '' | *[!0-9]*) PORT=50300 ;; esac

if [ ! -r "$RUN" ]; then
    M "エラー：$RUN がありません。キットをすべて展開し直してください。" "ERROR: $RUN is missing. Extract the whole kit again." >&2
    exit 1
fi
[ -x "$RUN" ] || chmod +x "$RUN" 2> /dev/null

# The driver reads IPHONE_VK. A host:port set by the user is kept; "usb" or nothing becomes the port of start-android.sh.
case "${IPHONE_VK:-usb}" in usb | usb:*) export IPHONE_VK="127.0.0.1:$PORT" ;; *) export IPHONE_VK ;; esac
export DXVK_FILTER_DEVICE_NAME="${DXVK_FILTER_DEVICE_NAME:-PocketGPU}"
export VKD3D_FILTER_DEVICE_NAME="${VKD3D_FILTER_DEVICE_NAME:-PocketGPU}"

case "${1:-}" in
"" | -h | --help)
    M "使い方：./run-on-android-gpu.sh コマンド 引数..." "Usage: ./run-on-android-gpu.sh command arguments..."
    M "先に ./start-android.sh を実行してください。" "Run ./start-android.sh first."
    M "Steam のゲームは、プロパティの「起動オプション」に次の 1 行を入力してください：" "For a Steam game, put this line in Properties > Launch Options:"
    printf '    "%s" %%command%%\n' "$self"
    M "最初に ./check-android.sh で、必要な項目をチェックしてください。" "Run ./check-android.sh first to check what is needed."
    exit 2
    ;;
--env)
    exec "$RUN" --env
    ;;
--broker)
    M "Android では USB の補助プログラムは要りません。./start-android.sh を実行してください。" "The USB helper is not needed for Android. Run ./start-android.sh." >&2
    exit 2
    ;;
esac

# Say it early when nothing listens on the port. This only reads /proc/net/tcp and does not connect. The program still starts.
port_listening() {
    local hex
    hex=$(printf '%04X' "$1")
    awk -v p="$hex" '$4 == "0A" && toupper(substr($2, length($2) - 3)) == p {f = 1} END {exit !f}' /proc/net/tcp /proc/net/tcp6 2> /dev/null
}
if [ -r /proc/net/tcp ] && ! port_listening "$PORT"; then
    M "注意：この PC の $PORT 番がつながっていません。先に ./start-android.sh を実行してください。" "Note: port $PORT on this PC is not connected. Run ./start-android.sh first." >&2
fi
exec "$RUN" "$@"
