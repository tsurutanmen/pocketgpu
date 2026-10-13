#!/usr/bin/env bash
# PocketGPU for Android（Linux）：スマホの受け手を止めて、この PC の口を閉じる。Windows のキットの android\stop_android.cmd と同じ流れ。
#   ./stop-android.sh              受け手を止めてスマホから消し、50300 番を閉じる
#   ./stop-android.sh --all        そのうえで、カメラとマイクの口（50064 番・50063 番）も閉じ、PocketGPU のアプリの機能をすべてオフにする
#   ./stop-android.sh --serial 番号  スマホが何台かあるとき、adb devices のシリアルで選ぶ
# 受け手の記録 /data/local/tmp/pgpu_server.log はスマホに残る。
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"
set -u
self=$(readlink -f -- "${BASH_SOURCE[0]}")
KIT=$(dirname -- "$self")
SELF=./stop-android.sh
LOGFILE="$KIT/pocketgpu-android-stop-log.txt"
TOTAL=2
ALL=0
# shellcheck source=android/common.sh
. "$KIT/android/common.sh"
usage() {
    say '使い方：./stop-android.sh [--all] [--serial シリアル]' 'Usage: ./stop-android.sh [--all] [--serial SERIAL]'
    say '  引数なし   スマホの受け手を止めて消し、この PC の 50300 番を閉じます' '  no option  stop the receiver on the phone, remove it, and close port 50300 on this PC'
    say '  --all      そのうえで、カメラとマイクの口を閉じ、PocketGPU のアプリの機能をすべてオフにします' '  --all      also close the camera and mic ports and turn off every feature of the PocketGPU app'
    say '  --serial   スマホが何台かあるとき、adb devices のシリアルで選びます' '  --serial   choose a phone by its serial from adb devices when more than one is connected'
}
while [ $# -gt 0 ]; do
    case "$1" in
    --all) ALL=1 ;;
    --serial) export ANDROID_SERIAL=${2:-}; shift ;;
    -h | --help) usage; exit 0 ;;
    *) say "知らない引数です：$1" "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

out "$(M 'PocketGPU for Android を止めます' 'Stop PocketGPU for Android')"

step 1 adb
if ! android_find_adb; then
    android_adb_missing
    finish 1
fi
ok "$(M "adb が見つかりました：$ADB" "adb was found: $ADB")"

step 2 "$(M 'スマホの受け手と、この PC の口' 'The receiver on the phone, and the ports on this PC')"
if ! android_select_device; then
    # スマホが無くても、この PC の口は閉じられる
    fw_remove "$PGPU_PORT"
    [ "$ALL" = 1 ] && { fw_remove "$CAM_PORT"; fw_remove "$AUD_PORT"; }
    android_device_problem
    ng "$(M 'スマホの受け手は止めていません。スマホを接続してから、もう一度実行するか、スマホを再起動してください。この PC の口は閉じました。' 'The receiver on the phone was not stopped. Connect the phone and run this again, or restart the phone. The ports on this PC are closed.')"
    finish 1
fi
ash "kill \$(pidof $DEV_NAME) 2>/dev/null; rm -f $DEV_DIR/$DEV_NAME; true" > /dev/null 2>&1
fw_remove "$PGPU_PORT"
ok "$(M "スマホの受け手を止めて、スマホから消しました。この PC の $PGPU_PORT 番を閉じました。記録は $DEV_DIR/$DEV_NAME.log に残っています。" "Stopped the receiver on the phone and removed it. Closed port $PGPU_PORT on this PC. Its log stays at $DEV_DIR/$DEV_NAME.log.")"
if [ "$ALL" = 1 ]; then
    fw_remove "$CAM_PORT"
    fw_remove "$AUD_PORT"
    if android_app_installed; then
        ash "am start -n $APP_ACT --ez gpu false --ez webcam false --ez audio false" > /dev/null 2>&1
        ok "$(M 'アプリの GPU・ウェブカメラ・スピーカーとマイクをオフにして、この PC のカメラとマイクの口を閉じました。' 'Turned off the GPU, the webcam, and the speaker and mic in the app, and closed the camera and mic ports on this PC.')"
    else
        ok "$(M 'この PC のカメラとマイクの口を閉じました。' 'Closed the camera and mic ports on this PC.')"
    fi
fi
finish 0
