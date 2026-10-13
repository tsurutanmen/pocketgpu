#!/usr/bin/env bash
# PocketGPU for Android（Linux）：スマホに受け手を入れて起動し、USB でこの PC につなぐ。Windows のキットの android\start_android.cmd と同じ流れ。
# 管理者の権限は要らない。スマホには何もインストールせず、root も要らない。受け手は /data/local/tmp の中のファイル。
#   ./start-android.sh                 受け手（android/server_aarch64）をスマホに置いて起動し、adb forward でこの PC の 50300 番につなぐ
#   ./start-android.sh --app           PocketGPU のアプリの中の受け手を使う（アプリの GPU をオンにして、50300 番を端末のアプリの口につなぐ。アプリ 0.1.4 以上は localabstract:pocketgpu-gpu・0.1.3 以下は tcp:50060）
#   ./start-android.sh --serial 番号    スマホが何台かあるとき、adb devices のシリアルで選ぶ（環境変数 ANDROID_SERIAL でも選べる）
#   PGPU_PORT=50301 ./start-android.sh  50300 番が使えないときの別の番号
# そのあと：./check-android.sh でチェック・./run-on-android-gpu.sh vkcube で起動・終わったら ./stop-android.sh
# 画面の文は下書き。言い回しは android/common.sh の M と、ここの M '日本語' 'English' の所で直す
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"
set -u
self=$(readlink -f -- "${BASH_SOURCE[0]}")
KIT=$(dirname -- "$self")
SELF=./start-android.sh
LOGFILE="$KIT/pocketgpu-android-start-log.txt"
TOTAL=4
USE_APP=0
# shellcheck source=android/common.sh
. "$KIT/android/common.sh"
usage() {
    say '使い方：./start-android.sh [--app] [--serial シリアル]' 'Usage: ./start-android.sh [--app] [--serial SERIAL]'
    say '  引数なし   受け手をスマホに入れて起動し、この PC の 50300 番につなぎます' '  no option  put the receiver on the phone, start it, and connect port 50300 on this PC to it'
    say '  --app      PocketGPU のアプリの中の受け手を使います。50300 番をスマホのアプリにつなぎます（アプリ 0.1.4 以上は USB 用の口 pocketgpu-gpu・0.1.3 以下は 50060 番）' '  --app      use the receiver inside the PocketGPU app. Port 50300 is connected to the app on the phone (the USB socket pocketgpu-gpu for app 0.1.4 or newer, port 50060 for 0.1.3 or older)'
    say '  --serial   スマホが何台かあるとき、adb devices のシリアルで選びます。環境変数 ANDROID_SERIAL でも選べます' '  --serial   choose a phone by its serial from adb devices when more than one is connected. ANDROID_SERIAL works too'
    say '  PGPU_PORT=50301 ./start-android.sh   50300 番を使えないときの別の番号' '  PGPU_PORT=50301 ./start-android.sh   another port, when 50300 is used by another program'
    say 'そのあとは ./check-android.sh でチェックして、./run-on-android-gpu.sh vkcube で起動します。終わったら ./stop-android.sh を実行します。' 'Then check with ./check-android.sh, run with ./run-on-android-gpu.sh vkcube, and finish with ./stop-android.sh.'
}
while [ $# -gt 0 ]; do
    case "$1" in
    --app) USE_APP=1 ;;
    --serial) export ANDROID_SERIAL=${2:-}; shift ;;
    -h | --help) usage; exit 0 ;;
    *) say "知らない引数です：$1" "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

out "$(M 'PocketGPU for Android を開始します' 'Start PocketGPU for Android')"
out "$(M 'Android のスマホの GPU を使うために、スマホに受け手を入れて起動します。管理者権限は必要ありません。' 'This puts the receiver on the Android phone and starts it, so that the phone GPU can be used. No administrator rights are needed.')"

# ==== 1. この PC ====
step 1 "$(M 'この PC' 'This PC')"
ARCH=$(uname -m)
if [ "$ARCH" != x86_64 ]; then
    ng "$(M "このキットは x86_64 の PC 用です。この PC は $ARCH です。" "This kit is for x86_64 PCs. This PC is $ARCH.")"
    stop_here
fi
ok "$(M "${OS_NAME:-Linux}・x86_64" "${OS_NAME:-Linux}, x86_64")"
LOG+=("      family=$FAMILY")
[ -x "$KIT/run-on-android-gpu.sh" ] || chmod +x "$KIT/run-on-android-gpu.sh" "$KIT/run-on-iphone-gpu.sh" "$KIT/bin/pgpu-probe" 2> /dev/null

# ==== 2. adb ====
while :; do
    step 2 adb
    if android_find_adb; then
        ok "$(M "adb が見つかりました：$ADB" "adb was found: $ADB")"
        LOG+=("      $("$ADB" version 2> /dev/null < /dev/null | head -n 1)")
        break
    fi
    android_adb_missing
    ask_retry || stop_here
done

# ==== 3. スマホ ====
while :; do
    step 3 "$(M 'Android のスマホ' 'Android phone')"
    if android_select_device; then
        android_phone_info
        ok "$(M "使用するスマホ：${ANDROID_MODEL:-?}・Android ${ANDROID_REL:-?}" "Phone to use: ${ANDROID_MODEL:-?}, Android ${ANDROID_REL:-?}")"
        [ "$((${#A_UNAUTH[@]} + ${#A_OFFLINE[@]} + ${#A_NOPERM[@]}))" -gt 0 ] && warn "$(M '使えない状態のスマホがほかにもつながっています。無視します。' 'Other phones that cannot be used are also connected. They are ignored.')"
        [ "${ANDROID_SDK:-30}" -lt 30 ] 2> /dev/null && warn "$(M 'Android 11 より前のスマホです。動かない可能性があります。' 'This phone is older than Android 11. It may not work.')"
        break
    fi
    android_device_problem
    ask_retry || stop_here
done
SRV=
case "$ANDROID_ABI" in
arm64-v8a) SRV=server_aarch64 ;;
x86_64) [ -f "$KIT/android/server_x86_64" ] && SRV=server_x86_64 ;;
esac
if [ "$USE_APP" = 0 ] && [ -z "$SRV" ]; then
    ng "$(M "このスマホの CPU には対応していません。対応しているのは 64 ビット ARM のスマホです。このスマホの値：${ANDROID_ABI:-?}" "This phone's CPU is not supported. Only 64-bit ARM phones are supported. This phone says: ${ANDROID_ABI:-?}")"
    stop_here
fi
if [ "$USE_APP" = 0 ] && [ ! -f "$KIT/android/$SRV" ]; then
    ng "$(M "$KIT/android/$SRV がありません。キットをすべて展開し直してください。" "$KIT/android/$SRV is missing. Extract the whole kit again.")"
    stop_here
fi

# ==== 4. 受け手 ====
if [ "$USE_APP" = 1 ]; then
    while :; do
        step 4 "$(M 'アプリの中の受け手' 'Receiver in the app')"
        if android_app_installed; then
            ver=$(android_app_version)
            ok "$(M "PocketGPU のアプリが入っています：${ver:-版は不明}" "The PocketGPU app is installed: ${ver:-unknown version}")"
            break
        fi
        android_install_app && continue
        ask_retry || stop_here
    done
    # キットの受け手が動いていると、アプリは自分の受け手を起こさない。先に止める
    ash "kill \$(pidof $DEV_NAME) 2>/dev/null; true" > /dev/null 2>&1
    # 0.1.4 以上のアプリは USB 用に localabstract で待つ。0.1.3 以下は TCP の 50060 番
    if version_ge "${ver:-}" "$APP_MIN_GPU_USB"; then APP_FWD="localabstract:$APP_GPU_USB"; else APP_FWD="tcp:$APP_GPU_PORT"; fi
    LOG+=("      app GPU forward target: $APP_FWD")
    FW=$("$ADB" forward "tcp:$PGPU_PORT" "$APP_FWD" 2>&1 < /dev/null)
    if [ $? -ne 0 ]; then
        ng "$(M "この PC の $PGPU_PORT 番を使えません。ほかのプログラムが使っているかもしれません。" "Cannot use port $PGPU_PORT on this PC. Another program may be using it.")"
        info "$(M "./stop-android.sh を実行するか、別の番号を使ってください：PGPU_PORT=$((PGPU_PORT + 1)) $SELF --app" "Run ./stop-android.sh, or use another port: PGPU_PORT=$((PGPU_PORT + 1)) $SELF --app")"
        LOG+=("      $(printf '%s' "$FW" | tr -d '\r')")
        stop_here
    fi
    ash "am start -n $APP_ACT --ez gpu true" > /dev/null 2>&1
    if [ "${APP_FWD#tcp:}" = "$APP_FWD" ]; then
        ok "$(M "アプリの GPU をオンにして、この PC の $PGPU_PORT 番をスマホのアプリの USB 用の口（$APP_GPU_USB）につなぎました。" "Turned on the GPU in the app and connected port $PGPU_PORT on this PC to the USB socket of the app on the phone ($APP_GPU_USB).")"
    else
        ok "$(M "アプリの GPU をオンにして、この PC の $PGPU_PORT 番をスマホの $APP_GPU_PORT 番につなぎました。" "Turned on the GPU in the app and connected port $PGPU_PORT on this PC to port $APP_GPU_PORT on the phone.")"
    fi
    info "$(M 'スマホにカメラやマイクの許可を求められたら「許可」をタップしてください。' 'If the phone asks for camera or microphone access, tap Allow.')"
else
    step 4 "$(M 'スマホに受け手を入れて起動' 'Put the receiver on the phone and start it')"
    info "$(M "受け手：android/$SRV・口：$PGPU_PORT" "Receiver: android/$SRV, port: $PGPU_PORT")"
    info "$(M '前の受け手があれば止めます。' 'Stopping an old receiver, if any.')"
    ash "kill \$(pidof $DEV_NAME) 2>/dev/null; rm -f $DEV_DIR/$DEV_NAME; true" > /dev/null 2>&1
    info "$(M '受け手をスマホにコピーしています。' 'Copying the receiver to the phone.')"
    PUSH=$("$ADB" push "$KIT/android/$SRV" "$DEV_DIR/$DEV_NAME" 2>&1 < /dev/null)
    if [ $? -ne 0 ]; then
        ng "$(M '受け手をスマホにコピーできませんでした。adb のメッセージ：' 'Could not copy the receiver to the phone. adb says:')"
        while IFS= read -r l; do [ -n "$l" ] && info "$l"; done <<< "$(printf '%s' "$PUSH" | tr -d '\r' | tail -n 3)"
        stop_here
    fi
    info "$(M '受け手を起動しています。' 'Starting the receiver.')"
    ash "cd $DEV_DIR && chmod 755 $DEV_NAME && (nohup ./$DEV_NAME 127.0.0.1 $PGPU_PORT > $DEV_NAME.log 2>&1 &)" > /dev/null 2>&1
    sleep 2
    PID=$(ashr "pidof $DEV_NAME")
    if [ -z "$PID" ]; then
        ng "$(M '受け手がスマホで動き続けませんでした。受け手の記録：' 'The receiver did not stay running on the phone. Its log:')"
        while IFS= read -r l; do info "$l"; done <<< "$(ashr "cat $DEV_DIR/$DEV_NAME.log" | tail -n 15)"
        info "$(M 'この記録と、スマホの機種名と Android のバージョンを Discord で送ってください。' 'Please send this log with the phone model and the Android version on Discord.')"
        stop_here
    fi
    ok "$(M "受け手がスマホで動いています。口：127.0.0.1:$PGPU_PORT" "The receiver is running on the phone. Port: 127.0.0.1:$PGPU_PORT")"
    FW=$("$ADB" forward "tcp:$PGPU_PORT" "tcp:$PGPU_PORT" 2>&1 < /dev/null)
    if [ $? -ne 0 ]; then
        ng "$(M "この PC の $PGPU_PORT 番を使えません。ほかのプログラムが使っているかもしれません。" "Cannot use port $PGPU_PORT on this PC. Another program may be using it.")"
        info "$(M "./stop-android.sh を実行するか、別の番号を使ってください：PGPU_PORT=$((PGPU_PORT + 1)) $SELF" "Run ./stop-android.sh, or use another port: PGPU_PORT=$((PGPU_PORT + 1)) $SELF")"
        LOG+=("      $(printf '%s' "$FW" | tr -d '\r')")
        stop_here
    fi
    ok "$(M "この PC の $PGPU_PORT 番をスマホにつなぎました。" "Connected port $PGPU_PORT on this PC to the phone.")"
fi

# ==== 使い方 ====
out ""
out "$(M '準備ができました。' 'Ready.')" "$C_OK"
out ""
out "$(M "・GPU が見えるかをチェックするには：./check-android.sh" "- To check that the phone GPU is seen: ./check-android.sh")"
out "$(M "・ターミナルで次のように実行します：" "- In a terminal, run:")"
if [ "$PGPU_PORT" != 50300 ]; then
    out "    PGPU_PORT=$PGPU_PORT \"$KIT/run-on-android-gpu.sh\" vkcube"
else
    out "    \"$KIT/run-on-android-gpu.sh\" vkcube"
fi
out "$(M "・Steam のゲームは、プロパティの「起動オプション」に次の 1 行を入力します。Proton のゲームもこのままで動作します。" "- For a Steam game, put this line in Properties > Launch Options. Proton games work the same way:")"
if [ "$PGPU_PORT" != 50300 ]; then
    out "    PGPU_PORT=$PGPU_PORT \"$KIT/run-on-android-gpu.sh\" %command%"
else
    out "    \"$KIT/run-on-android-gpu.sh\" %command%"
fi
if command -v flatpak > /dev/null 2>&1 && flatpak info com.valvesoftware.Steam > /dev/null 2>&1; then
    out "$(M '・Flatpak 版の Steam がインストールされています。Flatpak 版で使うには、次の 1 回だけの設定が必要です：' '- The Flatpak Steam is installed. To use it there, set this once:')"
    out "    flatpak override --user --filesystem=\"$KIT\":ro com.valvesoftware.Steam"
fi
[ "$FAMILY" = steamos ] && out "$(M '・ゲームモードでも動作する見込みですが、まだ確認していません。まずデスクトップモードで試してください。' '- Game Mode is expected to work but has not been checked yet. Try Desktop Mode first.')"
out "$(M '・アンチチートを導入しているゲームでは使用しないでください。アカウントが停止されることがあります。' '- Do not use games with anti-cheat. Your account may be banned.')"
out "$(M '・使用中は、スマホのロックを解除したまま、画面をオンにしておいてください。' '- While you use it, keep the phone unlocked with the screen on.')"
out "$(M '・スマホをマイクとカメラとして使うには：./start-android-mic-camera.sh' '- To use the phone as a microphone and a camera: ./start-android-mic-camera.sh')"
out "$(M '・終わったら：./stop-android.sh' '- When you are finished: ./stop-android.sh')"
finish 0
