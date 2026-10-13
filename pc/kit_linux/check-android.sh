#!/usr/bin/env bash
# PocketGPU for Android（Linux）：Android のスマホの GPU を使うのに必要な物を順にチェックし、足りない物の直し方を出す。Windows のキットの android\check_android.cmd と同じ目的。
# スマホにもこの PC にも何も変えない（adb で読むだけ）。管理者の権限は要らない。何もインストールしない。
#   ./check-android.sh
#   ./check-android.sh --serial 番号    スマホが何台かあるとき、adb devices のシリアルで選ぶ
# チェックする順：この PC → adb → スマホ → スマホの受け手とこの PC の口 → PocketGPU のアプリ → Vulkan のローダーとドライバでスマホの GPU が見えるか
# start-pocketgpu.sh は iPhone 用。Android のスマホは、先に ./start-android.sh を実行してから、この台本を実行する。
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"
set -u
self=$(readlink -f -- "${BASH_SOURCE[0]}")
KIT=$(dirname -- "$self")
SELF=./check-android.sh
LOGFILE="$KIT/pocketgpu-android-check-log.txt"
PROBE="$KIT/bin/pgpu-probe"
RUN="$KIT/run-on-android-gpu.sh"
TOTAL=6
# shellcheck source=android/common.sh
. "$KIT/android/common.sh"
usage() {
    say '使い方：./check-android.sh [--serial シリアル]' 'Usage: ./check-android.sh [--serial SERIAL]'
    say '  Android のスマホの GPU を使うのに必要な項目を順にチェックします。何も変更しません。先に ./start-android.sh を実行してください。' '  Checks, one by one, what is needed to use the Android phone GPU. It changes nothing. Run ./start-android.sh first.'
    say '  --serial   スマホが何台かあるとき、adb devices のシリアルで選びます' '  --serial   choose a phone by its serial from adb devices when more than one is connected'
}
while [ $# -gt 0 ]; do
    case "$1" in
    --serial) export ANDROID_SERIAL=${2:-}; shift ;;
    -h | --help) usage; exit 0 ;;
    *) say "知らない引数です：$1" "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

out "$(M 'PocketGPU for Android をチェックします' 'Check PocketGPU for Android')"
out "$(M 'Android のスマホの GPU を使うために必要な項目を、順にチェックします。何も変更しません。' 'This checks, one by one, what is needed to use the Android phone GPU. It changes nothing.')"

# ==== 1. この PC ====
step 1 "$(M 'この PC' 'This PC')"
ARCH=$(uname -m)
KREL=$(uname -r)
if [ "$ARCH" != x86_64 ]; then
    ng "$(M "このキットは x86_64 の PC 用です。この PC は $ARCH です。" "This kit is for x86_64 PCs. This PC is $ARCH.")"
    stop_here
fi
GLIBC=$(getconf GNU_LIBC_VERSION 2> /dev/null | awk '{print $2}')
ok "$(M "${OS_NAME:-Linux}・カーネル $KREL・glibc ${GLIBC:-?}・x86_64" "${OS_NAME:-Linux}, kernel $KREL, glibc ${GLIBC:-?}, x86_64")"
LOG+=("      family=$FAMILY")
for f in "$PROBE" "$RUN" "$KIT/run-on-iphone-gpu.sh"; do [ -x "$f" ] || chmod +x "$f" 2> /dev/null; done
"$PROBE" usb > /dev/null 2>&1
if [ $? -ge 126 ]; then
    ng "$(M 'bin/pgpu-probe を実行できません。' 'Cannot run bin/pgpu-probe.')"
    info "$(M 'キットを、実行が禁止されていない場所、たとえばホームフォルダに展開し直してください。' 'Extract the kit again in a place where programs may run, for example your home folder.')"
    stop_here
fi
if [ -n "${DISPLAY:-}" ]; then
    ok "$(M "X11 のディスプレイ $DISPLAY に、スマホの GPU で描いたウィンドウを表示できます。" "Windows drawn by the phone GPU can be shown on the X11 display $DISPLAY.")"
    [ -n "${WAYLAND_DISPLAY:-}" ] && info "$(M 'Wayland のセッションでは、Xwayland 経由で表示します。' 'In a Wayland session they are shown through Xwayland.')"
elif [ -n "${WAYLAND_DISPLAY:-}" ]; then
    warn "$(M 'Wayland だけのセッションです。スマホの GPU のウィンドウは X11 にだけ表示できるので、Xwayland が必要です。' 'This session is Wayland only. Windows from the phone GPU can only be shown on X11, so Xwayland is needed.')"
else
    warn "$(M 'デスクトップが見つかりません。ウィンドウを表示しない計算だけのアプリは使用できます。' 'No desktop was found. Programs that only compute and show no window can still be used.')"
fi

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
        ok "$(M "スマホが USB で認識されています：${ANDROID_MODEL:-?}・Android ${ANDROID_REL:-?}" "The phone is connected over USB: ${ANDROID_MODEL:-?}, Android ${ANDROID_REL:-?}")"
        [ "${ANDROID_SDK:-30}" -lt 30 ] 2> /dev/null && warn "$(M 'Android 11 より前のスマホです。動かない可能性があります。' 'This phone is older than Android 11. It may not work.')"
        case "$ANDROID_ABI" in
        arm64-v8a) ;;
        x86_64) info "$(M 'x86_64 のスマホです。エミュレーターとして扱います。' 'This is an x86_64 phone. It is treated as an emulator.')" ;;
        *) ng "$(M "このスマホの CPU には対応していません。対応しているのは 64 ビット ARM のスマホです。このスマホの値：${ANDROID_ABI:-?}" "This phone's CPU is not supported. Only 64-bit ARM phones are supported. This phone says: ${ANDROID_ABI:-?}")"; stop_here ;;
        esac
        break
    fi
    android_device_problem
    ask_retry || stop_here
done

# ==== 4. スマホの受け手とこの PC の口 ====
while :; do
    step 4 "$(M 'スマホの受け手と、この PC の口' 'The receiver on the phone, and the port on this PC')"
    KITPID=$(ashr "pidof $DEV_NAME")
    APPV=
    android_app_installed && APPV=$(android_app_version)
    android_snapshot
    EXPECT=
    if [ -n "$KITPID" ]; then
        ok "$(M "キットの受け手がスマホで動いています。口：127.0.0.1:$PGPU_PORT" "The kit receiver is running on the phone. Port: 127.0.0.1:$PGPU_PORT")"
        EXPECT="tcp:$PGPU_PORT"
    elif { [ -z "$APPV" ] || version_ge "$APPV" "$APP_MIN_GPU_USB"; } && snap_unix "$APP_GPU_USB"; then
        ok "$(M "PocketGPU のアプリの中の受け手が動いています。USB 用の口：localabstract:$APP_GPU_USB" "The receiver in the PocketGPU app is running. USB socket: localabstract:$APP_GPU_USB")"
        EXPECT="localabstract:$APP_GPU_USB"
    elif snap_tcp "$APP_GPU_PORT"; then
        ok "$(M "PocketGPU のアプリの中の受け手が動いています。口：127.0.0.1:$APP_GPU_PORT" "The receiver in the PocketGPU app is running. Port: 127.0.0.1:$APP_GPU_PORT")"
        EXPECT="tcp:$APP_GPU_PORT"
    else
        ng "$(M 'スマホで受け手が動いていません。' 'No receiver is running on the phone.')"
        info "$(M '次のコマンドで、受け手をスマホに入れて起動してください：' 'Put the receiver on the phone and start it with:')"
        cmdline "./start-android.sh"
        ask_retry || stop_here
        continue
    fi
    FWD=$("$ADB" forward --list 2> /dev/null < /dev/null | tr -d '\r' | awk -v s="$ANDROID_SERIAL" -v l="tcp:$PGPU_PORT" '$1 == s && $2 == l {print $3; exit}')
    if [ -z "$FWD" ]; then
        ng "$(M "この PC の $PGPU_PORT 番がスマホにつながっていません。" "Port $PGPU_PORT on this PC is not connected to the phone.")"
        info "$(M '次のコマンドで、つなぎ直してください：' 'Connect it again with:')"
        cmdline "./start-android.sh"
        ask_retry || stop_here
        continue
    fi
    if [ "$FWD" != "$EXPECT" ]; then
        warn "$(M "この PC の $PGPU_PORT 番は、スマホの ${FWD} につながっていますが、動いている受け手は ${EXPECT} です。" "Port $PGPU_PORT on this PC is connected to ${FWD} on the phone, but the receiver that is running uses ${EXPECT}.")"
        info "$(M 'もう一度 ./start-android.sh を実行してください。アプリの受け手を使うときは --app を付けます。' 'Run ./start-android.sh again. To use the receiver in the app, add --app.')"
    else
        ok "$(M "この PC の $PGPU_PORT 番がスマホの受け手につながっています。" "Port $PGPU_PORT on this PC is connected to the receiver on the phone.")"
    fi
    break
done

# ==== 5. PocketGPU のアプリ ====
step 5 "$(M 'PocketGPU のアプリ' 'PocketGPU app')"
if android_app_installed; then
    APPVER=$(android_app_version)
    ok "$(M "PocketGPU のアプリが入っています：${APPVER:-版は不明}" "The PocketGPU app is installed: ${APPVER:-unknown version}")"
    android_snapshot
    if snap_unix "$CAM_ABSTRACT" || snap_tcp "$CAM_PORT"; then info "$(M 'ウェブカメラがオンになっています。' 'The webcam is on.')"; else info "$(M 'ウェブカメラはオフです。' 'The webcam is off.')"; fi
    if snap_unix "$AUD_ABSTRACT" || snap_tcp "$AUD_PORT"; then info "$(M 'スピーカーとマイクがオンになっています。' 'The speaker and mic are on.')"; else info "$(M 'スピーカーとマイクはオフです。' 'The speaker and mic are off.')"; fi
    if [ -n "$APPVER" ] && ! version_ge "$APPVER" "$APP_MIN_NEW"; then
        warn "$(M "アプリが古い版です。カメラとマイクを USB で使う口の形が違うため、./start-android-mic-camera.sh は古い形に合わせます。新しい版は android/pocketgpu.apk です。" "The app is an old version. It uses the old form of the camera and mic ports, so ./start-android-mic-camera.sh adapts to it. The new version is android/pocketgpu.apk.")"
    fi
else
    info "$(M 'PocketGPU のアプリは入っていません。GPU だけなら要りません。カメラとマイクを使うときは、./start-android-mic-camera.sh が入れ方を案内します。' 'The PocketGPU app is not installed. It is not needed for the GPU alone. To use the camera and microphone, ./start-android-mic-camera.sh shows how to install it.')"
fi

# ==== 6. Vulkan のローダーとドライバ ====
step 6 "$(M 'ドライバ経由でスマホの GPU を認識できるか' 'Phone GPU through the driver')"
LD=$("$PROBE" loader 2> /dev/null)
if [ "${LD#LOADER OK}" = "$LD" ]; then
    ng "$(M 'Vulkan のローダー libvulkan.so.1 がありません。' 'The Vulkan loader libvulkan.so.1 is missing.')"
    case "$FAMILY" in
    arch | steamos) cmdline "sudo pacman -S --needed vulkan-icd-loader" ;;
    debian) cmdline "sudo apt install libvulkan1" ;;
    fedora) cmdline "sudo dnf install vulkan-loader" ;;
    suse) cmdline "sudo zypper install libvulkan1" ;;
    *) info "$(M 'パッケージマネージャで Vulkan のローダーをインストールしてください。' 'Install the Vulkan loader with your package manager.')" ;;
    esac
    stop_here
fi
LV=$(printf '%s' "$LD" | awk '{print $3}')
ok "$(M "Vulkan のローダー $LV" "Vulkan loader $LV")"
LOG+=("      $LD")
VK=$(
    eval "$("$RUN" --env)"
    export VK_LOADER_LAYERS_DISABLE='~implicit~' # チェックのときだけ、ほかの層（Steam のオーバーレイなど）を読まない
    timeout 30 "$PROBE" vklist 2>&1
)
RC=$?
# PC の GPU の vendor（Intel 0x8086・NVIDIA 0x10de・AMD 0x1002・Microsoft 0x1414・Mesa の CPU 0x10005）は数えない
GPUS=$(printf '%s\n' "$VK" | grep -E '^GPU[0-9]+: ' | grep -v -E 'vendor 0x(8086|10de|1002|1414|10005),' || true)
PCONLY=$(printf '%s\n' "$VK" | grep -E '^GPU[0-9]+: ' | grep -E 'vendor 0x(8086|10de|1002|1414|10005),' || true)
if [ -z "$GPUS" ] && [ -n "$PCONLY" ]; then
    ng "$(M 'PC の GPU だけが見えて、スマホの GPU が見えません。' 'Only the PC GPU is seen, not the phone GPU.')"
    while IFS= read -r l; do info "${l#GPU*: }"; done <<< "$PCONLY"
    stop_here
fi
if [ -n "$GPUS" ]; then
    while IFS= read -r g; do
        g=${g#GPU*: }
        ok "$(M 'ドライバ経由で認識できます：' 'Seen through the driver: ')${g%%  (vendor*}"
    done <<< "$GPUS"
else
    ng "$(M 'ドライバ経由でスマホの GPU を認識できません。' 'The driver does not see the phone GPU.')"
    if [ $RC = 124 ]; then
        info "$(M '30 秒以内に完了しませんでした。./stop-android.sh を実行してから、./start-android.sh をもう一度実行してください。' 'It did not finish in 30 seconds. Run ./stop-android.sh and then ./start-android.sh again.')"
    else
        while IFS= read -r l; do info "$l"; done < <(printf '%s\n' "$VK" | grep -v '^$' | tail -5)
        info "$(M 'スマホのロックを解除して、画面をオンにしてから、もう一度実行してください。' 'Unlock the phone, turn the screen on, and run this again.')"
    fi
    stop_here
fi

# ==== 使い方 ====
out ""
out "$(M '準備ができました。' 'Ready.')" "$C_OK"
out ""
out "$(M "・ターミナルで次のように実行します：" "- In a terminal, run:")"
out "    \"$RUN\" vkcube"
out "$(M "・Steam のゲームは、プロパティの「起動オプション」に次の 1 行を入力します。" "- For a Steam game, put this line in Properties > Launch Options:")"
out "    \"$RUN\" %command%"
out "$(M '・使用中は、スマホのロックを解除したまま、画面をオンにしておいてください。' '- While you use it, keep the phone unlocked with the screen on.')"
finish 0
