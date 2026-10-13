#!/usr/bin/env bash
# PocketGPU：iPhone を PC のマイクとウェブカメラとして使う（Linux）。iPhone の GPU は使わない。Windows のキットの Start-Mic-Camera.cmd（tools\miccam.ps1）と同じ順。
# 管理者の権限は要らない（v4l2loopback を入れるときだけ、出したコマンドを利用者が sudo で実行する）。何もインストールしない。
#   ./start-mic-camera.sh                          カメラとマイクを始める
#   ./start-mic-camera.sh --list                   USB でつながっている iPhone の一覧
#   ./start-mic-camera.sh --device 00008110-0012   iPhone が何台かあるとき、UDID（前の方 6 文字以上）か DeviceID で選ぶ
#   --no-mic・--no-camera・--speaker（PC の音を iPhone から出す。出力で「PocketGPU iPhone Speaker」を選ぶ）
#   --camera auto|back|front|ultrawide|tele・--quality auto|standard|high|max・--camera-out auto|pipewire|v4l2
# 動かす物：bin/webcam_rx（カメラ → v4l2loopback か PipeWire の仮想のカメラ）と bin/ipaudio（マイク → PipeWire か PulseAudio の仮想のマイク）。
#   どちらも、切れたら 3 秒後に起こし直す。q キー・Ctrl+C・このウィンドウを閉じると、両方を止める。
#   PulseAudio だけの PC（Ubuntu 22.04 など）では、pactl で仮想のマイク（null-sink と remap-source）を作り、止めるときに消す。
# 画面の文は下書き。M '日本語' 'English' の組で並べてあるので、言い回しはここで直す。
#
# 試験用の差し替え（試す人は使わない）：
#   PGPU_NOPROMPT=1             聞かずに進み、q キーを待たない
#   PGPU_MICCAM_SECONDS=N       N 秒で止める
#   PGPU_MICCAM_NOPROBE=1       手順 3 で iPhone のアプリの口をのぞかない（1 回で終わる試験の代役のため）
#   PGPU_MICCAM_MICWAV=パス     マイクを仮想のマイクでなく WAV に書く（音の仕組みの無い PC で試す）
#   PGPU_MICCAM_CAMOUT=…        カメラの出す先を webcam_rx の --out にそのまま渡す（none・y4m:パス）
#   PGPU_MICCAM_EXTRA_CAM=…・PGPU_MICCAM_EXTRA_AUDIO=…   webcam_rx・ipaudio に足す引数（--addr・--connect など）
#   PGPU_LANG=ja|en・IPHONE_VK_MUX=…（usbmuxd の口）
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"
set -u
self=$(readlink -f -- "${BASH_SOURCE[0]}")
KIT=$(dirname -- "$self")
PROBE="$KIT/bin/pgpu-probe"
RX="$KIT/bin/webcam_rx"
IPA="$KIT/bin/ipaudio"
LOGFILE="$KIT/pocketgpu-miccam-log.txt"
LOGDIR="$KIT/logs"
TOTAL=4
CAMERA_PORT=50064
AUDIO_PORT=50063

ja=0
case "${PGPU_LANG:-${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}}" in ja*) ja=1 ;; esac
M() { if [ "$ja" = 1 ]; then printf '%s' "$1"; else printf '%s' "$2"; fi; }

LOG=()
if [ -t 1 ]; then C_OK=$'\e[32m'; C_WARN=$'\e[33m'; C_NG=$'\e[31m'; C_END=$'\e[0m'; else C_OK=; C_WARN=; C_NG=; C_END=; fi
SCREEN=
# 画面に出す。子の記録を訳す糸（サブシェル）が出した行は、ログに入れるために SCREEN のファイルにも書く
out() {
    printf '%s%s%s\n' "${2:-}" "$1" "${2:+$C_END}"
    LOG+=("$1")
    if [ -n "$SCREEN" ] && [ "$BASH_SUBSHELL" -gt 0 ]; then printf '%s\n' "$1" >> "$SCREEN"; fi
}
step() { out ""; out "[$1/$TOTAL] $2"; }
ok() { out "  OK  $1" "$C_OK"; }
warn() { out "  !   $1" "$C_WARN"; }
ng() { out "  NG  $1" "$C_NG"; }
info() { out "      $1"; }
cmdline() { out "        $1"; }

NOPROMPT=${PGPU_NOPROMPT:-0}
SECONDS_LIMIT=${PGPU_MICCAM_SECONDS:-0}
RETRIES=0
ask_yes() {
    local a
    if [ "$NOPROMPT" = 1 ]; then out "      $1 -> $(M 'いいえ' 'no')"; return 1; fi
    read -r -p "      $1 [y/N] " a < /dev/tty || a=
    LOG+=("      $1 -> $a")
    case "$a" in y | Y | yes | はい) return 0 ;; *) return 1 ;; esac
}
ask_retry() {
    local a
    [ "$NOPROMPT" = 1 ] && return 1
    RETRIES=$((RETRIES + 1))
    [ $RETRIES -gt 50 ] && return 1
    read -r -p "      $(M '問題を解消したら Enter キーを押してください。もう一度チェックします。やめるときは q を入力して Enter キーを押してください。' 'Press Enter to check again after fixing it, or type q and Enter to stop.') " a < /dev/tty || return 1
    [ "$a" != q ]
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
    printf '%s\n' "${LOG[@]}" | sed -e "s#${HOME:-/nonexistent}#~#g" ${u:+-e "s#$u#(user)#g"} ${h:+-e "s#$h#(pc)#g"} > "$LOGFILE" 2> /dev/null
}
finish() {
    save_log
    out ""
    out "$(M 'このときのログ：' 'Log of this run: ')$LOGFILE"
    exit "$1"
}
stop_here() {
    out ""
    out "$(M 'ここで中断しました。問題を解消してから、もう一度 ./start-mic-camera.sh を実行してください。' 'Stopped here. After fixing it, run ./start-mic-camera.sh again.')" "$C_WARN"
    finish 1
}

# ---- 引数 ----
DEVICE=
LIST=0
WANT_CAMERA=1
WANT_MIC=1
SPEAKER=0
CAMERA=auto
QUALITY=auto
CAMOUT=auto
while [ $# -gt 0 ]; do
    case "$1" in
    --list) LIST=1 ;;
    --device) DEVICE=${2:-}; shift ;;
    --no-mic) WANT_MIC=0 ;;
    --no-camera) WANT_CAMERA=0 ;;
    --speaker) SPEAKER=1 ;;
    --camera) CAMERA=${2:-auto}; shift ;;
    --quality) QUALITY=${2:-auto}; shift ;;
    --camera-out) CAMOUT=${2:-auto}; shift ;;
    -h | --help) sed -n '2,10p' "$self" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) out "$(M '知らない引数です：' 'Unknown option: ')$1" "$C_NG"; exit 2 ;;
    esac
    shift
done

# ディストリビューションごとのインストールのコマンド（start-pocketgpu.sh と同じ見分け方）
OSR=${PGPU_OS_RELEASE:-/etc/os-release}
OS_ID=
OS_LIKE=
[ -r "$OSR" ] && OS_ID=$(. "$OSR" && printf '%s' "${ID:-}") && OS_LIKE=$(. "$OSR" && printf '%s' "${ID_LIKE:-}")
case " $OS_ID $OS_LIKE " in
*" steamos "*) FAMILY=steamos ;;
*" arch "* | *" cachyos "*) FAMILY=arch ;;
*" fedora "* | *" rhel "*) FAMILY=fedora ;;
*" debian "* | *" ubuntu "*) FAMILY=debian ;;
*" suse "* | *" opensuse "*) FAMILY=suse ;;
*) # ID_LIKE を書いていないディストリビューション（CachyOS など）は、パッケージマネージャで見分ける。PGPU_OS_RELEASE の試験では見ない
    FAMILY=other
    if [ -z "${PGPU_OS_RELEASE:-}" ]; then
        if command -v pacman > /dev/null 2>&1; then FAMILY=arch
        elif command -v apt-get > /dev/null 2>&1; then FAMILY=debian
        elif command -v dnf > /dev/null 2>&1; then FAMILY=fedora
        elif command -v zypper > /dev/null 2>&1; then FAMILY=suse; fi
    fi
    ;;
esac
hint_ffmpeg() {
    case "$FAMILY" in
    debian) cmdline "sudo apt install libavcodec-extra" ;;
    fedora) cmdline "sudo dnf install libavcodec-free" ;;
    arch | steamos) cmdline "sudo pacman -S --needed ffmpeg" ;;
    suse) cmdline "sudo zypper install libavcodec61" ;;
    *) info "$(M 'パッケージマネージャで FFmpeg（libavcodec）をインストールしてください。' 'Install FFmpeg (libavcodec) with your package manager.')" ;;
    esac
}
hint_v4l2() {
    info "$(M 'Zoom・Discord・ブラウザなど、ふつうのカメラとして選べるようにするには v4l2loopback が必要です。一度だけ次を実行してください：' 'So that Zoom, Discord, browsers and others can pick it as a normal camera, v4l2loopback is needed. Run this once:')"
    case "$FAMILY" in
    debian) cmdline "sudo apt install v4l2loopback-dkms" ;;
    fedora) cmdline "sudo dnf install v4l2loopback   # RPM Fusion" ;;
    arch)
        # headers は動いているカーネルに合わせる（CachyOS は linux-cachyos-headers・LTS は linux-lts-headers）。わからなければ linux-headers
        local kpkg
        kpkg=$(pacman -Qqo "/usr/lib/modules/$(uname -r)/vmlinuz" 2> /dev/null | head -1)
        cmdline "sudo pacman -S --needed v4l2loopback-dkms ${kpkg:-linux}-headers"
        ;;
    steamos) info "$(M 'SteamOS では v4l2loopback を使えません。PipeWire のカメラを使います。' 'SteamOS cannot use v4l2loopback. The PipeWire camera is used.')"; return ;;
    *) info "$(M 'パッケージマネージャで v4l2loopback をインストールしてください。' 'Install v4l2loopback with your package manager.')" ;;
    esac
    cmdline "sudo modprobe v4l2loopback exclusive_caps=1 card_label=\"PocketGPU iPhone Camera\""
    info "$(M 'PC を再起動すると消えます。そのときは modprobe の行をもう一度実行してください。' 'It is gone after a restart. Then run the modprobe line again.')"
    case "$FAMILY" in debian | fedora) info "$(M 'セキュアブートが有効な PC では、インストールの途中で MOK のパスワードを決めて、再起動の画面で登録してください。登録しないと modprobe が「Key was rejected by service」で失敗します。' 'With Secure Boot on, set a MOK password during the install and enroll it on the screen after a restart. Without it, modprobe fails with "Key was rejected by service".')" ;; esac
}

out "$(M 'iPhone をマイクとカメラとして使います' 'Use the iPhone as a microphone and a camera')"
out "$(M 'iPhone の GPU は使いません。管理者権限は必要ありません。' 'The iPhone GPU is not used. No administrator rights are needed.')"
[ -f "$KIT/start-android-mic-camera.sh" ] && out "$(M 'Android のスマホを使う場合は、この台本ではなく ./start-android-mic-camera.sh を実行してください。' 'To use an Android phone, run ./start-android-mic-camera.sh instead of this script.')"
if [ "$WANT_CAMERA" = 0 ] && [ "$WANT_MIC" = 0 ] && [ "$SPEAKER" = 0 ]; then
    ng "$(M 'カメラもマイクもスピーカーも使わない設定になっています。' 'Camera, microphone, and speaker are all turned off.')"
    finish 2
fi
for f in "$PROBE" "$RX" "$IPA"; do
    [ -x "$f" ] || chmod +x "$f" 2> /dev/null
    if [ ! -x "$f" ]; then
        ng "$(M "$f がありません。キットをすべて展開し直してください。" "$f is missing. Extract the whole kit again.")"
        stop_here
    fi
done

# ==== 1. usbmuxd ====
while :; do
    step 1 "$(M 'usbmuxd の実行' 'usbmuxd running')"
    MUX=$("$PROBE" mux 2> /dev/null)
    if [ "${MUX#MUX OK}" != "$MUX" ]; then ok "$(M 'usbmuxd が実行されています。' 'usbmuxd is running.')"; break; fi
    LOG+=("      $MUX")
    ng "$(M 'usbmuxd に接続できません。' 'Cannot connect to usbmuxd.')"
    info "$(M 'iPhone を USB ケーブルで接続して、ロックを解除してください。usbmuxd がインストールされていないときは、./start-pocketgpu.sh で手順を確認できます。' 'Plug the iPhone in with USB and unlock it. If usbmuxd is not installed, ./start-pocketgpu.sh shows how.')"
    ask_retry || stop_here
done

# ==== 2. iPhone ====
while :; do
    step 2 "iPhone"
    mapfile -t PHONES < <("$RX" --list 2> /dev/null | awk -F'\t' '$2 == "USB" {print $1 "\t" $4}')
    if [ "$LIST" = 1 ]; then
        [ ${#PHONES[@]} = 0 ] && info "$(M 'USB で接続されている iPhone はありません。' 'No iPhone is connected over USB.')"
        for p in "${PHONES[@]}"; do info "UDID ${p#*$'\t'}・DeviceID ${p%%$'\t'*}"; done
        finish 0
    fi
    if [ ${#PHONES[@]} = 0 ]; then
        ng "$(M 'USB で接続されている iPhone が見つかりません。' 'No iPhone is connected over USB.')"
        info "$(M 'iPhone を USB ケーブルで接続して、ロックを解除してください。「このコンピュータを信頼しますか？」と表示されたら「信頼」をタップしてください。' 'Plug the iPhone in with USB and unlock it. If it asks "Trust This Computer?", tap Trust.')"
        ask_retry || stop_here
        continue
    fi
    PICK=
    if [ -n "$DEVICE" ]; then
        want=$(printf '%s' "$DEVICE" | tr -d '-' | tr 'a-z' 'A-Z')
        for p in "${PHONES[@]}"; do
            id=${p%%$'\t'*}
            u=$(printf '%s' "${p#*$'\t'}" | tr -d '-' | tr 'a-z' 'A-Z')
            if [ "$DEVICE" = "$id" ] || { [ ${#want} -ge 6 ] && [ "${u#"$want"}" != "$u" ]; }; then PICK=$p; break; fi
        done
        if [ -z "$PICK" ]; then
            ng "$(M "指定された iPhone「$DEVICE」が見つかりません。接続されている iPhone：" "The iPhone \"$DEVICE\" was not found. Connected iPhones:")"
            for p in "${PHONES[@]}"; do info "UDID ${p#*$'\t'}・DeviceID ${p%%$'\t'*}"; done
            ask_retry || stop_here
            continue
        fi
    else
        PICK=${PHONES[0]}
        [ ${#PHONES[@]} -gt 1 ] && warn "$(M "${#PHONES[@]} 台の iPhone が接続されています。1 番目の iPhone を使います。選ぶときは --device に UDID を指定してください。" "${#PHONES[@]} iPhones are connected. Using the first one. To choose, pass its UDID with --device.")"
    fi
    DEV_ID=${PICK%%$'\t'*}
    DEV_UDID=${PICK#*$'\t'}
    ok "$(M "使用する iPhone：UDID ${DEV_UDID:0:13}…・DeviceID $DEV_ID" "iPhone to use: UDID ${DEV_UDID:0:13}…, DeviceID $DEV_ID")"
    break
done
DEVARG=$DEV_UDID
[ -z "$DEVARG" ] || [ "$DEVARG" = "?" ] && DEVARG=$DEV_ID

# ==== 3. iPhone のアプリ ====
step 3 "$(M 'iPhone のアプリ' 'iPhone app')"
if [ "${PGPU_MICCAM_NOPROBE:-0}" != 1 ]; then
    if [ "$WANT_CAMERA" = 1 ]; then
        if "$PROBE" port "$DEV_ID" $CAMERA_PORT > /dev/null 2>&1; then ok "$(M 'iPhone のアプリでウェブカメラがオンになっています。' 'The webcam is on in the iPhone app.')"
        else warn "$(M 'iPhone のアプリで「ウェブカメラとして使う」をオンにしてください。オンになると自動で接続します。' 'Turn on "Use this iPhone as a webcam" in the iPhone app. It connects automatically when it is on.')"; fi
    fi
    if [ "$WANT_MIC" = 1 ] || [ "$SPEAKER" = 1 ]; then
        if "$PROBE" port "$DEV_ID" $AUDIO_PORT > /dev/null 2>&1; then ok "$(M 'iPhone のアプリでスピーカーとマイクがオンになっています。' 'Speaker and mic are on in the iPhone app.')"
        else warn "$(M 'iPhone のアプリで「PC のスピーカーとマイクとして使う」をオンにしてください。オンになると自動で接続します。' 'Turn on "Use as PC speaker and mic" in the iPhone app. It connects automatically when it is on.')"; fi
    fi
fi
info "$(M 'GPU を使わない場合は、iPhone のアプリで「GPU を使わずにマイクとカメラだけ使う」をオンにしてください。' 'If you do not use the GPU, turn on "Use only the mic and camera, not the GPU" in the iPhone app.')"

# ==== 4. PC のカメラとマイク ====
step 4 "$(M 'PC のカメラとマイク' 'Camera and microphone on this PC')"
CAM_ARGS=()
if [ "$WANT_CAMERA" = 1 ]; then
    CHK=$("$RX" --check 2> /dev/null)
    LOG+=("      $(printf '%s' "$CHK" | tr '\n' ' ')")
    if ! printf '%s\n' "$CHK" | grep -q '^AVCODEC OK'; then
        ng "$(M 'カメラの映像を表示するには FFmpeg（libavcodec）が必要です。次のコマンドでインストールしてください。今回はカメラを使わずに続けます。' 'FFmpeg (libavcodec) is needed for the camera picture. Install it with the command below. This time it continues without the camera.')"
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
            ok "$(M "カメラ：仮想カメラ $V4L に表示します。Zoom・Discord・OBS・ブラウザなどで「PocketGPU iPhone Camera」などの名前で選べます。" "Camera: shown on the virtual camera $V4L. Zoom, Discord, OBS, browsers and others can pick it.")"
        elif [ "$PWV" = OK ] && [ "$CAMOUT" != v4l2 ]; then
            CAM_ARGS=(--out pipewire)
            ok "$(M 'カメラ：PipeWire の仮想カメラ「PocketGPU iPhone Camera」に表示します。' 'Camera: shown on the PipeWire camera "PocketGPU iPhone Camera".')"
            warn "$(M 'PipeWire のカメラを選べるのは、OBS（PipeWire のビデオキャプチャデバイス）や、PipeWire のカメラに対応したブラウザなどだけです。' 'Only apps that support PipeWire cameras, such as OBS (Video Capture Device, PipeWire) and some browsers, can pick it.')"
            hint_v4l2
        else
            ng "$(M 'カメラを表示する仮想カメラがありません（v4l2loopback も PipeWire も見つかりません）。今回はカメラを使わずに続けます。' 'There is no virtual camera to show the picture on: neither v4l2loopback nor PipeWire was found. This time it continues without the camera.')"
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
            [ "$WANT_MIC" = 1 ] && ok "$(M 'マイク：PipeWire に仮想マイク「PocketGPU iPhone Mic」を作ります。通話アプリのマイクで選んでください。' 'Microphone: a virtual microphone "PocketGPU iPhone Mic" is made in PipeWire. Pick it as the microphone in your call app.')"
            [ "$SPEAKER" = 1 ] && ok "$(M 'スピーカー：出力の一覧に「PocketGPU iPhone Speaker」を作ります。音を iPhone から出すには、これを選んでください。' 'Speaker: "PocketGPU iPhone Speaker" is added to the outputs. Pick it to play sound on the iPhone.')"
        elif printf '%s\n' "$ACHK" | grep -q '^PULSE OK' && [ -n "$SERVER" ]; then
            AUD_ARGS=(--backend pulse)
            if [ "$WANT_MIC" = 1 ]; then
                m1=$(pactl load-module module-null-sink sink_name=pocketgpu_mic_sink sink_properties=device.description=PocketGPU_iPhone_Mic_internal 2> /dev/null)
                m2=$(pactl load-module module-remap-source master=pocketgpu_mic_sink.monitor source_name=pocketgpu_iphone_mic source_properties=device.description=PocketGPU_iPhone_Mic 2> /dev/null)
                [ -n "$m1" ] && PA_MODULES+=("$m1")
                [ -n "$m2" ] && PA_MODULES+=("$m2")
                if [ -n "$m1" ] && [ -n "$m2" ]; then
                    AUD_ARGS+=(--pulse-mic-sink pocketgpu_mic_sink)
                    ok "$(M 'マイク：PulseAudio に仮想マイク「PocketGPU_iPhone_Mic」を作りました。通話アプリのマイクで選んでください。止めると消えます。' 'Microphone: made the virtual microphone "PocketGPU_iPhone_Mic" in PulseAudio. Pick it in your call app. It is removed when you stop.')"
                else
                    ng "$(M 'PulseAudio に仮想マイクを作れませんでした。今回はマイクを使わずに続けます。' 'Could not make a virtual microphone in PulseAudio. This time it continues without the microphone.')"
                    WANT_MIC=0
                fi
            fi
            if [ "$SPEAKER" = 1 ]; then
                m3=$(pactl load-module module-null-sink sink_name=pocketgpu_iphone_speaker sink_properties=device.description=PocketGPU_iPhone_Speaker 2> /dev/null)
                if [ -n "$m3" ]; then
                    PA_MODULES+=("$m3")
                    AUD_ARGS+=(--pulse-spk-source pocketgpu_iphone_speaker.monitor)
                    ok "$(M 'スピーカー：出力の一覧に「PocketGPU_iPhone_Speaker」を作りました。音を iPhone から出すには、これを選んでください。' 'Speaker: added "PocketGPU_iPhone_Speaker" to the outputs. Pick it to play sound on the iPhone.')"
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
if [ "$WANT_CAMERA" = 0 ] && [ "$WANT_MIC" = 0 ] && [ "$SPEAKER" = 0 ]; then stop_here; fi

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
    *"iPhone を待っています"* | *"つなげない"*) out "  $(M 'カメラ：iPhone の接続を待っています。iPhone のアプリで「ウェブカメラとして使う」をオンにして、アプリを前面に表示してください。' 'Camera: waiting for the iPhone. In the iPhone app, turn on "Use this iPhone as a webcam" and keep the app in front.')" ;;
    *"iPhone との接続が切れた"*) out "  $(M 'カメラ：iPhone との接続が切れました。再接続を待っています。' 'Camera: the iPhone disconnected. Waiting to reconnect.')" ;;
    *"iPhone が断った（status 1）"*) out "  $(M 'カメラ：iPhone のカメラへのアクセスが許可されていません。iPhone の設定アプリで許可してください。' 'Camera: camera access is not allowed. Allow it in the iPhone Settings app.')" ;;
    *"iPhone が断った（status 3）"*) out "  $(M 'カメラ：iPhone のアプリを前面に表示してください。iOS はバックグラウンドのアプリにカメラを使わせません。' 'Camera: bring the iPhone app to the front. iOS does not let apps in the background use the camera.')" ;;
    *"iPhone が断った"*) out "  $(M 'カメラ：iPhone が開始できませんでした。' 'Camera: the iPhone could not start.') ${l##*（}" ;;
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
        out "  $(M "マイク：接続中です。iPhone のマイクを $(awk "BEGIN{printf \"%.1f\", ${k:-48000}/1000}") kHz で取り込んでいます。" "Microphone: connected. Recording the iPhone microphone at $(awk "BEGIN{printf \"%.1f\", ${k:-48000}/1000}") kHz.")" ;;
    "つながった。iPhone："*) out "  $(M 'スピーカー：接続中です。' 'Speaker: connected.')" ;;
    *"マイクはアプリを前に"*) out "  $(M 'マイク：iPhone のアプリを前面に表示すると開始します。' 'Microphone: it starts when the iPhone app is in front.')" ;;
    "iPhone のマイクが開始されました。"*) out "  $(M 'マイク：開始しました。' 'Microphone: started.')" ;;
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
        if [ "$name" = audio ]; then out "$(M 'マイク：iPhone との接続が切れました。3 秒後に再接続します。' 'Microphone: disconnected from the iPhone. Reconnecting in 3 seconds.')"
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
LIST=$(printf "%s$SEP" "${RUNNING[@]}")
out "$(M '開始します：' 'Starting: ')${LIST%"$SEP"}$(M '。GPU は使いません。' '. The GPU is not used.')" "$C_OK"
MUXARGS=()
[ -n "${IPHONE_VK_MUX:-}" ] && MUXARGS=(--mux "$IPHONE_VK_MUX")
# shellcheck disable=SC2206
EXTRA_CAM=(${PGPU_MICCAM_EXTRA_CAM:-})
# shellcheck disable=SC2206
EXTRA_AUD=(${PGPU_MICCAM_EXTRA_AUDIO:-})
if [ "$WANT_CAMERA" = 1 ]; then
    A=("${MUXARGS[@]}" --reconnect "${CAM_ARGS[@]}")
    if [ ${#EXTRA_CAM[@]} -gt 0 ]; then A+=("${EXTRA_CAM[@]}"); else A=(--usb --device "$DEVARG" "${A[@]}"); fi
    [ "$CAMERA" != auto ] && A+=(--camera "$CAMERA")
    [ "$QUALITY" != auto ] && A+=(--quality "$QUALITY")
    run_child camera "$LOGDIR/webcam_rx.txt" "$RX" "${A[@]}" &
fi
if [ "$WANT_MIC" = 1 ] || [ "$SPEAKER" = 1 ]; then
    A=("${MUXARGS[@]}" --reconnect "${AUD_ARGS[@]}")
    if [ ${#EXTRA_AUD[@]} -gt 0 ]; then A+=("${EXTRA_AUD[@]}"); else A=(--usb --device "$DEVARG" "${A[@]}"); fi
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
