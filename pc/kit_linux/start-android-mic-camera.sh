#!/usr/bin/env bash
# PocketGPU for Android（Linux）：Android のスマホを PC のマイクとウェブカメラとして使う。スマホの GPU は使わない。Windows のキットの android\android_app.cmd と、Linux 版の start-mic-camera.sh を合わせた物。
# 管理者の権限は要らない（v4l2loopback を入れるときだけ、出したコマンドを利用者が sudo で実行する）。PC には何もインストールしない。
#   ./start-android-mic-camera.sh                        カメラとマイクを始める
#   ./start-android-mic-camera.sh --list                 USB でつながっているスマホの一覧
#   ./start-android-mic-camera.sh --serial R5CT1234ABC   スマホが何台かあるとき、adb devices のシリアルで選ぶ（環境変数 ANDROID_SERIAL でも選べる）
#   --no-mic・--no-camera・--speaker（PC の音をスマホから出す。出力で「PocketGPU Android Speaker」を選ぶ）
#   --camera auto|back|front|ultrawide|tele・--quality auto|standard|high|max・--camera-out auto|pipewire|v4l2
#   --keep-app-on                                         止めるとき、PocketGPU のアプリの機能をオフにしない
# 流れ：adb で PocketGPU のアプリに入り、アプリのウェブカメラとスピーカー・マイクをオンにして、次の 2 本をつなぐ。
#   adb forward tcp:50064 localabstract:pocketgpu-webcam   → bin/webcam_rx --addr 127.0.0.1:50064（カメラ → v4l2loopback か PipeWire の仮想のカメラ）
#   adb forward tcp:50063 localabstract:pocketgpu-audio    → bin/ipaudio --connect 127.0.0.1:50063（マイク・スピーカー → PipeWire か PulseAudio）
#   アプリが 0.1.1 のときは、古い形の adb forward tcp:50064 tcp:50064・tcp:50063 tcp:50063 に替える（アプリの版を見て決める。版が読めないときは新しい形を先に試して、だめなら古い形）。
#   どちらも、切れたら 3 秒後に起こし直す。q キー・Ctrl+C・このウィンドウを閉じると、両方を止めて、この PC の口を閉じる。
#   PulseAudio だけの PC（Ubuntu 22.04 など）では、pactl で仮想のマイク（null-sink と remap-source）を作り、止めるときに消す。
# 画面の文は下書き。M '日本語' 'English' の組で並べてあるので、言い回しはここで直す。
#
# 試験用の差し替え（試す人は使わない）：
#   PGPU_NOPROMPT=1             聞かずに進み、q キーを待たない
#   PGPU_MICCAM_SECONDS=N       N 秒で止める
#   PGPU_MICCAM_MICWAV=パス     マイクを仮想のマイクでなく WAV に書く（音の仕組みの無い PC で試す）
#   PGPU_MICCAM_CAMOUT=…        カメラの出す先を webcam_rx の --out にそのまま渡す（none・y4m:パス）
#   PGPU_ANDROID_FORM=new|old   USB の口の向け方を決める
#   ADB=パス・PGPU_LANG=ja|en
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"
set -u
self=$(readlink -f -- "${BASH_SOURCE[0]}")
KIT=$(dirname -- "$self")
SELF=./start-android-mic-camera.sh
RX="$KIT/bin/webcam_rx"
IPA="$KIT/bin/ipaudio"
LOGFILE="$KIT/pocketgpu-android-miccam-log.txt"
LOGDIR="$KIT/logs"
TOTAL=4

# ---- 引数 ----
LIST=0
WANT_CAMERA=1
WANT_MIC=1
SPEAKER=0
CAMERA=auto
QUALITY=auto
CAMOUT=auto
KEEP_APP=0
# shellcheck source=android/common.sh
. "$KIT/android/common.sh"
usage() {
    say '使い方：./start-android-mic-camera.sh [オプション]' 'Usage: ./start-android-mic-camera.sh [options]'
    say '  Android のスマホをマイクとウェブカメラにします。スマホの GPU は使いません。' '  Uses the Android phone as a microphone and a webcam. The phone GPU is not used.'
    say '  --list            USB でつながっているスマホの一覧' '  --list            list the phones connected over USB'
    say '  --serial シリアル  スマホが何台かあるとき、adb devices のシリアルで選びます。環境変数 ANDROID_SERIAL でも選べます' '  --serial SERIAL   choose a phone by its serial from adb devices when more than one is connected. ANDROID_SERIAL works too'
    say '  --no-camera       カメラを使いません' '  --no-camera       do not use the camera'
    say '  --no-mic          マイクを使いません' '  --no-mic          do not use the microphone'
    say '  --speaker         PC の音をスマホから出します。出力で「PocketGPU Android Speaker」を選びます' '  --speaker         play the PC sound on the phone. Pick PocketGPU Android Speaker as the output'
    say '  --camera auto|back|front|ultrawide|tele' '  --camera auto|back|front|ultrawide|tele'
    say '  --quality auto|standard|high|max' '  --quality auto|standard|high|max'
    say '  --camera-out auto|pipewire|v4l2   カメラの映像を出す先' '  --camera-out auto|pipewire|v4l2   where to show the camera picture'
    say '  --keep-app-on     止めるとき、PocketGPU のアプリの機能をオフにしません' '  --keep-app-on     do not turn off the app features when stopping'
}
while [ $# -gt 0 ]; do
    case "$1" in
    --list) LIST=1 ;;
    --serial | --device) export ANDROID_SERIAL=${2:-}; shift ;;
    --no-mic) WANT_MIC=0 ;;
    --no-camera) WANT_CAMERA=0 ;;
    --speaker) SPEAKER=1 ;;
    --camera) CAMERA=${2:-auto}; shift ;;
    --quality) QUALITY=${2:-auto}; shift ;;
    --camera-out) CAMOUT=${2:-auto}; shift ;;
    --keep-app-on) KEEP_APP=1 ;;
    -h | --help) usage; exit 0 ;;
    *) say "知らない引数です：$1" "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

SECONDS_LIMIT=${PGPU_MICCAM_SECONDS:-0}
SCREEN=
# 画面に出す。子の記録を訳す糸（サブシェル）が出した行は、ログに入れるために SCREEN のファイルにも書く
out() {
    printf '%s%s%s\n' "${2:-}" "$1" "${2:+$C_END}"
    LOG+=("$1")
    if [ -n "$SCREEN" ] && [ "$BASH_SUBSHELL" -gt 0 ]; then printf '%s\n' "$1" >> "$SCREEN"; fi
}
save_log() {
    local h u
    if [ -n "$SCREEN" ] && [ -r "$SCREEN" ]; then
        mapfile -t extra < "$SCREEN"
        LOG+=("      ---- $(M '動かしている間の表示' 'shown while running') ----" "${extra[@]}")
        rm -f "$SCREEN"
        SCREEN=
    fi
    h=$(cat /proc/sys/kernel/hostname 2> /dev/null || hostname 2> /dev/null || true)
    u=${USER:-$(id -un 2> /dev/null)}
    printf '%s\n' "${LOG[@]}" | sed -e "s#${HOME:-/nonexistent}#~#g" ${u:+-e "s#$u#(user)#g"} ${h:+-e "s#$h#(pc)#g"} ${ANDROID_SERIAL:+-e "s#$ANDROID_SERIAL#(serial)#g"} > "$LOGFILE" 2> /dev/null
}

hint_ffmpeg() {
    case "$FAMILY" in
    debian) cmdline "sudo apt install libavcodec-extra" ;;
    fedora) cmdline "sudo dnf install libavcodec-free" ;;
    arch | steamos) cmdline "sudo pacman -S --needed ffmpeg" ;;
    suse) cmdline "sudo zypper install libavcodec61" ;;
    *) info "$(M 'パッケージマネージャで FFmpeg の libavcodec をインストールしてください。' 'Install the FFmpeg library libavcodec with your package manager.')" ;;
    esac
}
hint_v4l2() {
    info "$(M 'Zoom・Discord・ブラウザなど、ふつうのカメラとして選べるようにするには v4l2loopback が必要です。一度だけ次を実行してください：' 'So that Zoom, Discord, browsers and others can pick it as a normal camera, v4l2loopback is needed. Run this once:')"
    case "$FAMILY" in
    debian) cmdline "sudo apt install v4l2loopback-dkms" ;;
    fedora) cmdline "sudo dnf install v4l2loopback   # RPM Fusion" ;;
    arch) cmdline "sudo pacman -S --needed v4l2loopback-dkms linux-headers" ;;
    steamos) info "$(M 'SteamOS では v4l2loopback を使えません。PipeWire のカメラを使います。' 'SteamOS cannot use v4l2loopback. The PipeWire camera is used.')"; return ;;
    *) info "$(M 'パッケージマネージャで v4l2loopback をインストールしてください。' 'Install v4l2loopback with your package manager.')" ;;
    esac
    cmdline "sudo modprobe v4l2loopback exclusive_caps=1 card_label=\"PocketGPU Android Camera\""
    info "$(M 'PC を再起動すると消えます。そのときは modprobe の行をもう一度実行してください。' 'It is gone after a restart. Then run the modprobe line again.')"
}

out "$(M 'Android のスマホをマイクとカメラとして使います' 'Use the Android phone as a microphone and a camera')"
out "$(M 'スマホの GPU は使いません。管理者権限は必要ありません。' 'The phone GPU is not used. No administrator rights are needed.')"
if [ "$WANT_CAMERA" = 0 ] && [ "$WANT_MIC" = 0 ] && [ "$SPEAKER" = 0 ]; then
    ng "$(M 'カメラもマイクもスピーカーも使わない設定になっています。' 'Camera, microphone, and speaker are all turned off.')"
    finish 2
fi
for f in "$RX" "$IPA"; do
    [ -x "$f" ] || chmod +x "$f" 2> /dev/null
    if [ ! -x "$f" ]; then
        ng "$(M "$f がありません。キットをすべて展開し直してください。" "$f is missing. Extract the whole kit again.")"
        stop_here
    fi
done

# ==== 1. adb ====
while :; do
    step 1 adb
    if android_find_adb; then
        ok "$(M "adb が見つかりました：$ADB" "adb was found: $ADB")"
        LOG+=("      $("$ADB" version 2> /dev/null < /dev/null | head -n 1)")
        break
    fi
    android_adb_missing
    ask_retry || stop_here
done

# ==== 2. スマホ ====
while :; do
    step 2 "$(M 'Android のスマホ' 'Android phone')"
    if [ "$LIST" = 1 ]; then
        android_select_device > /dev/null
        if [ ${#A_READY[@]} = 0 ]; then
            info "$(M 'USB で接続されているスマホはありません。' 'No phone is connected over USB.')"
        else
            android_list_ready
        fi
        finish 0
    fi
    if android_select_device; then
        android_phone_info
        ok "$(M "使用するスマホ：${ANDROID_MODEL:-?}・Android ${ANDROID_REL:-?}" "Phone to use: ${ANDROID_MODEL:-?}, Android ${ANDROID_REL:-?}")"
        [ "$((${#A_UNAUTH[@]} + ${#A_OFFLINE[@]} + ${#A_NOPERM[@]}))" -gt 0 ] && warn "$(M '使えない状態のスマホがほかにもつながっています。無視します。' 'Other phones that cannot be used are also connected. They are ignored.')"
        break
    fi
    android_device_problem
    ask_retry || stop_here
done

# ==== 3. PocketGPU のアプリ ====
step 3 "$(M 'スマホの PocketGPU のアプリ' 'PocketGPU app on the phone')"
while :; do
    if android_app_installed; then break; fi
    android_install_app && continue
    ask_retry || stop_here
    step 3 "$(M 'スマホの PocketGPU のアプリ' 'PocketGPU app on the phone')"
done
APPVER=$(android_app_version)
ok "$(M "PocketGPU のアプリが入っています：${APPVER:-版は不明}" "The PocketGPU app is installed: ${APPVER:-unknown version}")"
FORM=
FORCED=0
case "${PGPU_ANDROID_FORM:-}" in new | old) FORM=$PGPU_ANDROID_FORM; FORCED=1 ;; esac
if [ -z "$FORM" ]; then
    if [ -n "$APPVER" ]; then
        if version_ge "$APPVER" "$APP_MIN_NEW"; then FORM=new; else FORM=old; fi
    else
        FORM=new # 版が読めないときは、新しい形を先に試す
    fi
fi
FWD_CAM=0
FWD_AUD=0
APP_ON=()
AM=()
WANT_AUDIO=0
{ [ "$WANT_MIC" = 1 ] || [ "$SPEAKER" = 1 ]; } && WANT_AUDIO=1
fw_open() {
    local what=$1
    if ! fw_add "$FORM" "$what"; then
        ng "$(M "この PC の口を開けませんでした。ほかのプログラムが 50064 番か 50063 番を使っているかもしれません。adb のメッセージ：$FWERR" "Could not open a port on this PC. Another program may be using port 50064 or 50063. adb says: $FWERR")"
        stop_here
    fi
}
if [ "$WANT_CAMERA" = 1 ]; then fw_open cam; FWD_CAM=1; AM+=(--ez webcam true); APP_ON+=(webcam); fi
if [ "$WANT_AUDIO" = 1 ]; then fw_open aud; FWD_AUD=1; AM+=(--ez audio true); APP_ON+=(audio); fi
if [ "$FORM" = old ] && version_ge "$APPVER" "$APP_MIN_NEW"; then AM+=(--ez legacy_tcp true); fi # 0.1.2 以降で古い形を強制したとき
info "$(M 'スマホのロックを解除して、画面をオンにしてください。アプリを開いて、機能をオンにします。' 'Unlock the phone and turn the screen on. The app is opened and its features are turned on.')"
ash "am start -n $APP_ACT ${AM[*]}" > /dev/null 2>&1
info "$(M 'スマホでアプリを開き、機能をオンにしました。カメラやマイクの許可を求められたら、スマホで「許可」をタップしてください。' 'Opened the app on the phone and turned the features on. If the phone asks for camera or microphone access, tap Allow.')"
info "$(M "口の形：$([ "$FORM" = new ] && echo 'localabstract' || echo 'tcp')" "Port form: $([ "$FORM" = new ] && echo 'localabstract' || echo 'tcp')")"

# 端末で口が開くのを待つ（最長 10 秒）。許可の質問が出ているときは、押すまで開かない
open_state() { # $1 = cam か aud、$2 = new か old
    case "$1$2" in
    camnew) snap_unix "$CAM_ABSTRACT" ;;
    camold) snap_tcp "$CAM_PORT" ;;
    audnew) snap_unix "$AUD_ABSTRACT" ;;
    audold) snap_tcp "$AUD_PORT" ;;
    esac
}
for _ in 1 2 3 4 5 6 7 8 9 10; do
    android_snapshot
    all=1
    [ "$FWD_CAM" = 1 ] && ! open_state cam "$FORM" && all=0
    [ "$FWD_AUD" = 1 ] && ! open_state aud "$FORM" && all=0
    [ $all = 1 ] && break
    sleep 1
done
OTHER=old
[ "$FORM" = old ] && OTHER=new
for what in cam aud; do
    [ "$what" = cam ] && [ "$FWD_CAM" = 0 ] && continue
    [ "$what" = aud ] && [ "$FWD_AUD" = 0 ] && continue
    if open_state "$what" "$FORM"; then
        if [ "$what" = cam ]; then ok "$(M 'アプリでウェブカメラがオンになっています。' 'The webcam is on in the app.')"; else ok "$(M 'アプリでスピーカーとマイクがオンになっています。' 'The speaker and mic are on in the app.')"; fi
    elif [ -z "$APPVER" ] && [ "$FORCED" = 0 ] && open_state "$what" "$OTHER"; then
        # 版が読めなくて、新しい形が開いていない。古い形が開いているので、古い形に替える
        FORM_SAVE=$FORM
        FORM=$OTHER
        fw_open "$what"
        FORM=$FORM_SAVE
        ok "$(M 'アプリは古い形の口で待っています。古い形に替えました。' 'The app waits on the old form of the port. Switched to the old form.')"
    elif [ "$what" = cam ]; then
        warn "$(M 'アプリのウェブカメラの口がまだ開いていません。スマホで許可を求められていれば「許可」をタップしてください。アプリでウェブカメラがオンになっているかも確認してください。オンになると自動で接続します。' 'The webcam port in the app is not open yet. If the phone asks for permission, tap Allow. Also check that the webcam is on in the app. It connects automatically when it is on.')"
    else
        warn "$(M 'アプリのスピーカーとマイクの口がまだ開いていません。スマホで許可を求められていれば「許可」をタップしてください。アプリでスピーカーとマイクがオンになっているかも確認してください。オンになると自動で接続します。' 'The speaker and mic port in the app is not open yet. If the phone asks for permission, tap Allow. Also check that the speaker and mic are on in the app. It connects automatically when they are on.')"
    fi
done
info "$(M '使っている間は、スマホのロックを解除したままにして、アプリを閉じないでください。' 'While you use it, keep the phone unlocked and do not close the app.')"

# ==== 4. PC のカメラとマイク ====
step 4 "$(M 'PC のカメラとマイク' 'Camera and microphone on this PC')"
CAM_ARGS=()
if [ "$WANT_CAMERA" = 1 ]; then
    CHK=$("$RX" --check 2> /dev/null)
    LOG+=("      $(printf '%s' "$CHK" | tr '\n' ' ')")
    if ! printf '%s\n' "$CHK" | grep -q '^AVCODEC OK'; then
        ng "$(M 'カメラの映像を表示するには FFmpeg の libavcodec が必要です。次のコマンドでインストールしてください。今回はカメラを使わずに続けます。' 'The FFmpeg library libavcodec is needed for the camera picture. Install it with the command below. This time it continues without the camera.')"
        hint_ffmpeg
        WANT_CAMERA=0
    else
        V4L=$(printf '%s\n' "$CHK" | awk '$1 == "V4L2" && $2 == "OK" {print $3; exit}')
        PWV=$(printf '%s\n' "$CHK" | awk '$1 == "PIPEWIRE" {print $2; exit}')
        if [ -n "${PGPU_MICCAM_CAMOUT:-}" ]; then
            CAM_ARGS=(--out "$PGPU_MICCAM_CAMOUT")
            ok "$(M 'カメラ：試験用の出力 ' 'Camera: test output ')$PGPU_MICCAM_CAMOUT"
        elif [ -n "$V4L" ] && [ "$CAMOUT" != pipewire ]; then
            CAM_ARGS=(--out v4l2 --v4l2 "$V4L")
            ok "$(M "カメラ：仮想カメラ $V4L に表示します。Zoom・Discord・OBS・ブラウザなどで選べます。" "Camera: shown on the virtual camera $V4L. Zoom, Discord, OBS, browsers and others can pick it.")"
        elif [ "$PWV" = OK ] && [ "$CAMOUT" != v4l2 ]; then
            CAM_ARGS=(--out pipewire)
            ok "$(M 'カメラ：PipeWire の仮想カメラ「PocketGPU Android Camera」に表示します。' 'Camera: shown on the PipeWire camera "PocketGPU Android Camera".')"
            warn "$(M 'PipeWire のカメラを選べるのは、PipeWire のカメラに対応したアプリだけです。たとえば、OBS の PipeWire のビデオキャプチャデバイスや、一部のブラウザです。' 'Only apps that support PipeWire cameras can pick it. Examples are OBS with the PipeWire Video Capture Device, and some browsers.')"
            hint_v4l2
        else
            ng "$(M 'カメラを表示する仮想カメラがありません。v4l2loopback も PipeWire も見つかりません。今回はカメラを使わずに続けます。' 'There is no virtual camera to show the picture on. Neither v4l2loopback nor PipeWire was found. This time it continues without the camera.')"
            hint_v4l2
            WANT_CAMERA=0
        fi
    fi
fi
AUD_ARGS=()
PA_MODULES=()
if [ "$WANT_MIC" = 1 ] || [ "$SPEAKER" = 1 ]; then
    if [ -n "${PGPU_MICCAM_MICWAV:-}" ]; then
        AUD_ARGS=(--backend none --mic-wav "$PGPU_MICCAM_MICWAV")
        [ "$SPEAKER" = 1 ] && AUD_ARGS+=(--gen)
        ok "$(M 'マイク：試験用に WAV に書きます ' 'Microphone: written to a WAV file for testing ')$PGPU_MICCAM_MICWAV"
    else
        ACHK=$("$IPA" --check 2> /dev/null)
        LOG+=("      $(printf '%s' "$ACHK" | tr '\n' ' ')")
        SERVER=$(LC_ALL=C pactl info 2> /dev/null | sed -n 's/^Server Name: //p')
        LOG+=("      pactl: ${SERVER:-none}")
        if printf '%s\n' "$ACHK" | grep -q '^PIPEWIRE OK' && { [ -z "$SERVER" ] || [ "${SERVER#*PipeWire}" != "$SERVER" ]; }; then
            AUD_ARGS=(--backend pipewire)
            [ "$WANT_MIC" = 1 ] && ok "$(M 'マイク：PipeWire に仮想マイク「PocketGPU Android Mic」を作ります。通話アプリのマイクで選んでください。' 'Microphone: a virtual microphone "PocketGPU Android Mic" is made in PipeWire. Pick it as the microphone in your call app.')"
            [ "$SPEAKER" = 1 ] && ok "$(M 'スピーカー：出力の一覧に「PocketGPU Android Speaker」を作ります。音をスマホから出すには、これを選んでください。' 'Speaker: "PocketGPU Android Speaker" is added to the outputs. Pick it to play sound on the phone.')"
        elif printf '%s\n' "$ACHK" | grep -q '^PULSE OK' && [ -n "$SERVER" ]; then
            AUD_ARGS=(--backend pulse)
            if [ "$WANT_MIC" = 1 ]; then
                m1=$(pactl load-module module-null-sink sink_name=pocketgpu_android_mic_sink sink_properties=device.description=PocketGPU_Android_Mic_internal 2> /dev/null)
                m2=$(pactl load-module module-remap-source master=pocketgpu_android_mic_sink.monitor source_name=pocketgpu_android_mic source_properties=device.description=PocketGPU_Android_Mic 2> /dev/null)
                [ -n "$m1" ] && PA_MODULES+=("$m1")
                [ -n "$m2" ] && PA_MODULES+=("$m2")
                if [ -n "$m1" ] && [ -n "$m2" ]; then
                    AUD_ARGS+=(--pulse-mic-sink pocketgpu_android_mic_sink)
                    ok "$(M 'マイク：PulseAudio に仮想マイク「PocketGPU_Android_Mic」を作りました。通話アプリのマイクで選んでください。止めると消えます。' 'Microphone: made the virtual microphone "PocketGPU_Android_Mic" in PulseAudio. Pick it in your call app. It is removed when you stop.')"
                else
                    ng "$(M 'PulseAudio に仮想マイクを作れませんでした。今回はマイクを使わずに続けます。' 'Could not make a virtual microphone in PulseAudio. This time it continues without the microphone.')"
                    WANT_MIC=0
                fi
            fi
            if [ "$SPEAKER" = 1 ]; then
                m3=$(pactl load-module module-null-sink sink_name=pocketgpu_android_speaker sink_properties=device.description=PocketGPU_Android_Speaker 2> /dev/null)
                if [ -n "$m3" ]; then
                    PA_MODULES+=("$m3")
                    AUD_ARGS+=(--pulse-spk-source pocketgpu_android_speaker.monitor)
                    ok "$(M 'スピーカー：出力の一覧に「PocketGPU_Android_Speaker」を作りました。音をスマホから出すには、これを選んでください。' 'Speaker: added "PocketGPU_Android_Speaker" to the outputs. Pick it to play sound on the phone.')"
                else
                    ng "$(M 'PulseAudio に仮想スピーカーを作れませんでした。' 'Could not make a virtual speaker in PulseAudio.')"
                    SPEAKER=0
                fi
            fi
        else
            ng "$(M 'PipeWire も PulseAudio も見つかりません。マイクとスピーカーは使えません。' 'Neither PipeWire nor PulseAudio was found. The microphone and speaker cannot be used.')"
            WANT_MIC=0
            SPEAKER=0
        fi
    fi
fi
if [ "$WANT_CAMERA" = 0 ] && [ "$WANT_MIC" = 0 ] && [ "$SPEAKER" = 0 ]; then
    # 何も動かさないので、開けた口を閉じる
    [ "$FWD_CAM" = 1 ] && fw_remove "$CAM_PORT"
    [ "$FWD_AUD" = 1 ] && fw_remove "$AUD_PORT"
    stop_here
fi

# ==== 始める ====
mkdir -p "$LOGDIR"
SCREEN="$LOGDIR/.screen.txt"
: > "$SCREEN"
RUNDIR=$(mktemp -d "${TMPDIR:-/tmp}/pocketgpu-miccam.XXXXXX")
cleanup() {
    touch "$RUNDIR/stop"
    for f in "$RUNDIR"/*.pid; do [ -r "$f" ] && kill "$(cat "$f")" 2> /dev/null; done
    wait 2> /dev/null
    for m in "${PA_MODULES[@]}"; do pactl unload-module "$m" 2> /dev/null; done
    rm -rf "$RUNDIR"
    # この PC の口を閉じて、アプリの機能をオフにする
    [ "$FWD_CAM" = 1 ] && fw_remove "$CAM_PORT"
    [ "$FWD_AUD" = 1 ] && fw_remove "$AUD_PORT"
    if [ "$KEEP_APP" = 0 ] && [ ${#APP_ON[@]} -gt 0 ]; then
        local off=() k
        for k in "${APP_ON[@]}"; do off+=(--ez "$k" false); done
        ash "am start -n $APP_ACT ${off[*]}" > /dev/null 2>&1
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

conv_camera() {
    local l=$1
    case "$l" in
    *"決まった形："* | *"形が変わった："*)
        local w h fps kbps cam what name
        w=$(sed -n 's/.*：\([0-9]*\) x \([0-9]*\)・\([0-9]*\) コマ\/秒・\([0-9]*\) kbps・カメラ \([0-9]\).*/\1 \2 \3 \4 \5/p' <<< "$l")
        read -r w h fps kbps cam <<< "$w"
        case "$cam" in 1) name=$(M '前面カメラ' 'front camera') ;; 2) name=$(M '超広角' 'ultra wide') ;; 3) name=$(M '望遠' 'telephoto') ;; *) name=$(M '背面カメラ' 'back camera') ;; esac
        case "$l" in *"決まった形"*) what=$(M '接続中です' 'Connected') ;; *) what=$(M '切り替わりました' 'Switched') ;; esac
        out "  $(M 'カメラ：' 'Camera: ')$what$(M '。' '. ')${w}x${h}$(M '・' ', ')$fps $(M 'コマ/秒' 'fps')$(M '・' ', ')$name$(M '・' ', ')$(awk "BEGIN{printf \"%.1f\", ${kbps:-0}/1000}") Mbps" ;;
    *"iPhone を待っています"* | *"つなげない"*) out "  $(M 'カメラ：スマホの接続を待っています。スマホの PocketGPU のアプリで、ウェブカメラをオンにしてください。' 'Camera: waiting for the phone. Turn on the webcam in the PocketGPU app on the phone.')" ;;
    *"iPhone との接続が切れた"* | *"iPhone の返事が来ない"*) out "  $(M 'カメラ：スマホとの接続が切れました。再接続を待っています。アプリでウェブカメラがオンになっているか、確認してください。' 'Camera: the connection to the phone was lost. Waiting to reconnect. Check that the webcam is on in the app.')" ;;
    *"iPhone が断った（status 1）"*) out "  $(M 'カメラ：カメラの許可がありません。スマホの「設定」>「アプリ」>「PocketGPU」>「権限」で、カメラを許可してください。' 'Camera: camera access is not allowed. Allow the camera in the phone Settings > Apps > PocketGPU > Permissions.')" ;;
    *"iPhone が断った（status 2）"*) out "  $(M 'カメラ：選んだカメラがこのスマホにありません。--camera を変えてください。' 'Camera: the camera you chose is not on this phone. Change --camera.')" ;;
    *"iPhone が断った（status 3）"*) out "  $(M 'カメラ：PocketGPU のアプリを前面に表示してください。' 'Camera: bring the PocketGPU app to the front.')" ;;
    *"iPhone が断った（status 4）"*) out "  $(M 'カメラ：スマホが映像の圧縮を始められませんでした。--quality standard を試してください。' 'Camera: the phone could not start the video encoder. Try --quality standard.')" ;;
    *"iPhone が断った"*)
        st=${l##*status }
        out "  $(M 'カメラ：スマホが開始できませんでした。コード ' 'Camera: the phone could not start. Code ')${st%%[!0-9]*}" ;;
    *"使うアプリが"*"を選んだ"*) out "  $(M 'カメラ：アプリがカメラを使い始めました。' 'Camera: an app started using the camera.')" ;;
    *"形（YUYV"*"を決められない"*) out "  $(M 'カメラ：仮想カメラの形を変えられません。カメラを使っているアプリをすべて閉じてから、もう一度起動してください。' 'Camera: cannot set the virtual camera format. Close every app that uses the camera and start again.')" ;;
    esac
}
conv_audio() {
    local l=$1
    case "$l" in
    "つながった。iPhone："*"mic=ok"*)
        local k
        k=$(sed -n 's/.*hw_rate=\([0-9]*\).*/\1/p' <<< "$l")
        out "  $(M "マイク：接続中です。スマホのマイクを $(awk "BEGIN{printf \"%.1f\", ${k:-48000}/1000}") kHz で取り込んでいます。" "Microphone: connected. Recording the phone microphone at $(awk "BEGIN{printf \"%.1f\", ${k:-48000}/1000}") kHz.")" ;;
    "つながった。iPhone："*) out "  $(M 'スピーカー：接続中です。' 'Speaker: connected.')" ;;
    *"マイクはアプリを前に"*) out "  $(M 'マイク：スマホのアプリを前面に表示すると開始します。' 'Microphone: it starts when the app on the phone is in front.')" ;;
    "iPhone のマイクが開始されました。"*) out "  $(M 'マイク：開始しました。' 'Microphone: started.')" ;;
    *"あいさつが通らない"* | "iPhone を待っています"*) out "  $(M 'マイク：スマホの接続を待っています。スマホの PocketGPU のアプリで、スピーカーとマイクをオンにしてください。' 'Microphone: waiting for the phone. Turn on the speaker and mic in the PocketGPU app on the phone.')" ;;
    esac
}

# 子：切れたら 3 秒後に起こし直す。記録は logs/ に書き、画面には訳した行だけを出す
run_child() {
    local name=$1 log=$2
    shift 2
    : > "$log"
    while [ ! -e "$RUNDIR/stop" ]; do
        "$@" > >(while IFS= read -r l; do printf '%s\n' "$l" >> "$log"; if [ "$name" = camera ]; then conv_camera "$l"; else conv_audio "$l"; fi; done) 2>&1 &
        echo $! > "$RUNDIR/$name.pid"
        wait $!
        rm -f "$RUNDIR/$name.pid"
        [ -e "$RUNDIR/stop" ] && break
        if [ "$name" = audio ]; then out "$(M 'マイク：スマホとの接続が切れました。3 秒後に再接続します。' 'Microphone: disconnected from the phone. Reconnecting in 3 seconds.')"
        else out "$(M 'カメラ：停止しました。3 秒後に再開します。' 'Camera: stopped. Restarting in 3 seconds.')"; fi
        for _ in 1 2 3 4 5 6; do [ -e "$RUNDIR/stop" ] && break; sleep 0.5; done
    done
}

out ""
RUNNING=()
[ "$WANT_CAMERA" = 1 ] && RUNNING+=("$(M 'カメラ' 'camera')")
[ "$WANT_MIC" = 1 ] && RUNNING+=("$(M 'マイク' 'microphone')")
[ "$SPEAKER" = 1 ] && RUNNING+=("$(M 'スピーカー' 'speaker')")
SEP=$(M '・' ', ')
WHAT=$(printf "%s$SEP" "${RUNNING[@]}")
out "$(M '開始します：' 'Starting: ')${WHAT%"$SEP"}$(M '。GPU は使いません。' '. The GPU is not used.')" "$C_OK"
if [ "$WANT_CAMERA" = 1 ]; then
    A=(--addr "127.0.0.1:$CAM_PORT" --reconnect "${CAM_ARGS[@]}" --name "PocketGPU Android Camera")
    [ "$CAMERA" != auto ] && A+=(--camera "$CAMERA")
    [ "$QUALITY" != auto ] && A+=(--quality "$QUALITY")
    run_child camera "$LOGDIR/webcam_rx.txt" "$RX" "${A[@]}" &
fi
if [ "$WANT_MIC" = 1 ] || [ "$SPEAKER" = 1 ]; then
    A=(--connect "127.0.0.1:$AUD_PORT" --reconnect "${AUD_ARGS[@]}" --mic-name "PocketGPU Android Mic" --spk-name "PocketGPU Android Speaker")
    [ "$WANT_MIC" = 1 ] && A+=(--mic)
    [ "$SPEAKER" = 1 ] && [ -z "${PGPU_MICCAM_MICWAV:-}" ] && A+=(--speaker)
    run_child audio "$LOGDIR/ipaudio.txt" "$IPA" "${A[@]}" &
fi
if [ "$SECONDS_LIMIT" -le 0 ] && [ "$NOPROMPT" != 1 ]; then out "$(M '終了するには、このウィンドウで q キーを押してください。' 'To stop, press q in this window.')"; fi
T0=$(date +%s)
while :; do
    if [ "$SECONDS_LIMIT" -gt 0 ]; then
        [ $(($(date +%s) - T0)) -ge "$SECONDS_LIMIT" ] && break
        sleep 0.5
    elif [ "$NOPROMPT" = 1 ]; then
        sleep 1
    else
        k=
        read -rsn1 -t 1 k < /dev/tty 2> /dev/null
        rc=$?
        [ $rc -ne 0 ] && [ $rc -le 128 ] && sleep 1 # 端末が無い（すぐ失敗する）ときは空回りしない
        case "$k" in q | Q) break ;; esac
    fi
done
out ""
out "$(M 'カメラとマイクを停止しました。' 'Stopped the camera and the microphone.')"
cleanup
trap - EXIT
finish 0
