#!/usr/bin/env bash
# PocketGPU を直す（Linux）：start-pocketgpu.sh のチェックと同じ所を見て、直せる物は 1 つずつ聞いてから直し、直せない物は何をすればよいかを出す。
# Windows のキットの Fix-PocketGPU.cmd（tools\fix.ps1）の Linux 版。bash 4.4 以上だけで動く。管理者の権限は要らない。
# 画面の文は下書き。M '日本語' 'English' の組で並べてあるので、言い回しはここで直す。
#
# 決まり：
#   - 自分では何も消さない。止める・入れ替える・つなぎ直す前に、必ず y/N で聞く。y と答えた物だけを直す
#   - 入れ替える前の物は <キット>/fix-backup/<日時>/ に残す（行末を直した .sh・作り直した json・古い合言葉のファイル・権限の記録）
#   - sudo の要る物（パッケージ・サービス・グループ・カーネルのモジュール）は自分では実行しない。ディストリごとのコマンドを出すだけ
#   - 止めるのは、このユーザーのプロセスだけ
#   - 結果は pocketgpu-fix-log.txt と pocketgpu-fix-summary.txt（Discord に貼れる短い文）に書く。
#     合言葉・ユーザー名・PC の名前・フォルダの場所・スマホのシリアル・iPhone の UDID は載せない
#
#   ./fix.sh                       順にチェックして、直せる物は聞いてから直す
#   ./fix.sh --check-only          チェックだけ。何も変えない（何を直せるかだけ出す）
#   ./fix.sh --only usbmuxd,gpu    その項目だけ。pc・files・loader・usbmuxd・udev・tools・iphone・android・media・gpu
#   ./fix.sh --kit フォルダ         直すキットのフォルダ。無ければ、このファイルのフォルダか 1 つ上（無ければホームの近くを探す）
#   ./fix.sh --no-summary          最後のまとめを出さない
#   終わりのコード：0 問題なし（または直った）・1 直らなかった物がある・2 キットのフォルダが見つからない・3 使い方の誤り
#
# 試す人は使わない、試験用の差し替え（start-pocketgpu.sh と同じ物に加えて）：
#   PGPU_NOPROMPT=1・PGPU_ANSWERS=y,n,...・PGPU_LANG=ja|en・PGPU_OS_RELEASE=パス・PGPU_SYS_USB=フォルダ・IPHONE_VK_MUX=…
#   PGPU_TEST_PROBE=パス          bin/pgpu-probe の代わりに動かす物（偽の道具）
#   PGPU_UDEV_DIRS=a:b            udev の規則を探すフォルダ（既定は /etc/udev/rules.d など 4 つ）
#   PGPU_SBIN=a:b                 PATH の外で usbmuxd を探すフォルダ（既定は /usr/sbin:/usr/bin:/sbin）
#   PGPU_SYS=フォルダ・PGPU_DEV=フォルダ   /sys と /dev の代わり（v4l2loopback を見る所）
#   PGPU_SEARCH_ROOTS=a:b         キットを探すフォルダ
#   PGPU_TEST_STOP_ALLOW=PID,PID  止めてよいプロセスを、この PID だけにする（試験が、自分の物以外を止めないための保険）
#   ADB=パス・ANDROID_SERIAL・PGPU_PORT・PGPU_CAM_PORT・PGPU_AUDIO_PORT   Android の adb・使うスマホ・PC 側のポート（既定は 50300・50064・50063）
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"
if [ "${BASH_VERSINFO[0]}" -lt 4 ] || { [ "${BASH_VERSINFO[0]}" -eq 4 ] && [ "${BASH_VERSINFO[1]}" -lt 4 ]; }; then
    echo "bash 4.4 or newer is needed. / bash 4.4 以上が必要です。" >&2
    exit 3
fi
set -u
FIX_VERSION=1
self=$(readlink -f -- "${BASH_SOURCE[0]}")
HERE=$(dirname -- "$self")
TOTAL=10

# ---- Android の台本の名前（別の係が作っているキットの台本）。名前が変わったら、ここだけを直す ----
AND_START=start-android.sh               # 受け手をスマホに入れて起動し、adb forward でつなぐ（--app・--serial を受ける）
AND_STOP=stop-android.sh                 # 受け手を止めて、ポートを閉じる
AND_CHECK=check-android.sh               # チェックだけ
AND_RUN=run-on-android-gpu.sh            # 1 つのアプリをスマホの GPU で起動する（--env で設定を出す）
AND_MICCAM=start-android-mic-camera.sh   # スマホをマイクとカメラにする
AND_COMMON=android/common.sh             # 上の台本が . で読む共通の部分
AND_SERVER=android/server_aarch64        # スマホで動く受け手
AND_APK=android/pocketgpu.apk            # Android のアプリ
AND_PKG=com.tsurutanmen.pocketgpu        # アプリのパッケージ名
AND_DEV_DIR=/data/local/tmp              # スマホの中の、受け手を置く所
AND_DEV_NAME=pgpu_server                 # スマホの中の、受け手の名前
AND_APP_GPU_PORT=50060                   # アプリの中の GPU の受け手が、スマホで待つ TCP のポート（0.1.3 以下。0.1.4 以上は 50060 が使えなければ 50061 から順に待つ）
AND_APP_GPU_USB=localabstract:pocketgpu-gpu # アプリの中の GPU の受け手が、USB 用に待つ口（0.1.4 から）
AND_APP_MIN_GPU_USB=0.1.4                # GPU の USB 用の口（localabstract）が付いたアプリの版
AND_APP_MIN_NEW=0.1.2                    # カメラとマイクのポートが localabstract に変わったアプリの版
AND_CAM_NEW=localabstract:pocketgpu-webcam
AND_CAM_OLD=tcp:50064
AND_AUD_NEW=localabstract:pocketgpu-audio
AND_AUD_OLD=tcp:50063

# ---- 言葉 ----
ja=0
case "${PGPU_LANG:-${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}}" in ja*) ja=1 ;; esac
M() { if [ "$ja" = 1 ]; then printf '%s' "$1"; else printf '%s' "$2"; fi; }

# ---- 引数 ----
CHECK_ONLY=0
NO_SUMMARY=0
ONLY=()
KIT_ARG=
usage() {
    M '使い方：./fix.sh [--check-only] [--only 項目,項目] [--kit フォルダ] [--no-summary]' 'Usage: ./fix.sh [--check-only] [--only item,item] [--kit folder] [--no-summary]'
    echo
    M '項目：pc・files・loader・usbmuxd・udev・tools・iphone・android・media・gpu' 'Items: pc, files, loader, usbmuxd, udev, tools, iphone, android, media, gpu'
    echo
}
while [ $# -gt 0 ]; do
    case "$1" in
    --check-only | -n) CHECK_ONLY=1 ;;
    --only) IFS=',' read -r -a ONLY <<< "${2:-}"; shift ;;
    --only=*) IFS=',' read -r -a ONLY <<< "${1#--only=}" ;;
    --kit | --kit-dir) KIT_ARG=${2:-}; shift ;;
    --kit=*) KIT_ARG=${1#--kit=} ;;
    --no-summary) NO_SUMMARY=1 ;;
    -h | --help) usage; exit 0 ;;
    *) usage >&2; exit 3 ;;
    esac
    shift
done
want() {
    local x
    [ ${#ONLY[@]} -eq 0 ] && return 0
    for x in "${ONLY[@]}"; do [ "$x" = "$1" ] && return 0; done
    return 1
}

# ---- 画面とログ ----
LOG=()
if [ -t 1 ]; then C_OK=$'\e[32m'; C_WARN=$'\e[33m'; C_NG=$'\e[31m'; C_HEAD=$'\e[36m'; C_END=$'\e[0m'; else C_OK=; C_WARN=; C_NG=; C_HEAD=; C_END=; fi
out() { printf '%s%s%s\n' "${2:-}" "$1" "${2:+$C_END}"; LOG+=("$1"); }
step() { out ""; out "[$1/$TOTAL] $2"; }
ok() { out "  OK  $1" "$C_OK"; }
warn() { out "  !   $1" "$C_WARN"; }
ng() { out "  NG  $1" "$C_NG"; }
info() { out "      $1"; }
cmdline() { out "        $1"; }
logonly() { LOG+=("      $1"); }

NOPROMPT=${PGPU_NOPROMPT:-0}
ANSWERS=()
IFS=',' read -r -a ANSWERS <<< "${PGPU_ANSWERS:-}"
# はい・いいえを聞く。端末が無いときは「いいえ」
ask_yes() {
    local a=
    if [ "$NOPROMPT" = 1 ]; then
        a=${ANSWERS[0]:-n}
        [ ${#ANSWERS[@]} -gt 0 ] && ANSWERS=("${ANSWERS[@]:1}")
        out "      $1 -> $([ "$a" = y ] && M 'はい' 'yes' || M 'いいえ' 'no')"
        [ "$a" = y ]
        return
    fi
    if { : < /dev/tty; } 2> /dev/null; then
        read -r -p "      $1 [y/N] " a < /dev/tty || a=
    else
        out "      $1 -> $(M '端末が無いので「いいえ」' 'no terminal, so no')"
        return 1
    fi
    LOG+=("      $1 -> $a")
    case "$a" in y | Y | yes | YES | はい) return 0 ;; *) return 1 ;; esac
}
# 直してよいかを聞く。--check-only のときは聞かずに「いいえ」
offer() {
    if [ "$CHECK_ONLY" = 1 ]; then
        out "      $1 -> $(M '--check-only なので聞きません' '--check-only: not asking')"
        return 1
    fi
    ask_yes "$1"
}

# ---- 結果 ----
# 状態：ok 問題なし・fixed 直した・unfixed 直らなかった（または直せない）・warn 気をつけたい・skip 対象外
R_ID=()
R_STATE=()
R_TITLE=()
R_NOTE=()
add_result() { R_ID+=("$1"); R_STATE+=("$2"); R_TITLE+=("$3"); R_NOTE+=("${4:-}"); }
result_of() {
    local i
    for i in "${!R_ID[@]}"; do [ "${R_ID[$i]}" = "$1" ] && { printf '%s' "${R_STATE[$i]}"; return 0; }; done
    printf ''
}
join_by() { local s=$1 r= x; shift; for x in "$@"; do r="${r:+$r$s}$x"; done; printf '%s' "$r"; }

# ---- この PC の種類と、足りない物の入れ方 ----
OSR=${PGPU_OS_RELEASE:-/etc/os-release}
OS_ID=
OS_LIKE=
OS_NAME=
if [ -r "$OSR" ]; then
    OS_ID=$(. "$OSR" && printf '%s' "${ID:-}")
    OS_LIKE=$(. "$OSR" && printf '%s' "${ID_LIKE:-}")
    OS_NAME=$(. "$OSR" && printf '%s' "${PRETTY_NAME:-${NAME:-}}")
fi
case " $OS_ID $OS_LIKE " in
*" steamos "*) FAMILY=steamos ;;
*" arch "*) FAMILY=arch ;;
*" fedora "* | *" rhel "*) FAMILY=fedora ;;
*" debian "* | *" ubuntu "*) FAMILY=debian ;;
*" suse "* | *" opensuse "*) FAMILY=suse ;;
*) FAMILY=other ;;
esac
[ "$FAMILY" = fedora ] && [ -e /run/ostree-booted ] && FAMILY=ostree

# パッケージの名前。$1 = 何か、$2 = 家族。無ければ空
pkg_names() {
    case "$1:$2" in
    usbmuxd:arch | usbmuxd:steamos) echo "usbmuxd libimobiledevice" ;;
    usbmuxd:debian | usbmuxd:fedora | usbmuxd:ostree) echo "usbmuxd libimobiledevice-utils" ;;
    usbmuxd:suse) echo "usbmuxd imobiledevice-tools" ;;
    idevice:arch | idevice:steamos) echo "libimobiledevice" ;;
    idevice:debian | idevice:fedora | idevice:ostree) echo "libimobiledevice-utils" ;;
    idevice:suse) echo "imobiledevice-tools" ;;
    vulkan:arch | vulkan:steamos) echo "vulkan-icd-loader" ;;
    vulkan:debian | vulkan:suse) echo "libvulkan1" ;;
    vulkan:fedora | vulkan:ostree) echo "vulkan-loader" ;;
    vulkan32:arch | vulkan32:steamos) echo "lib32-vulkan-icd-loader" ;;
    vulkan32:debian) echo "libvulkan1:i386" ;;
    vulkan32:fedora | vulkan32:ostree) echo "vulkan-loader.i686" ;;
    vulkan32:suse) echo "libvulkan1-32bit" ;;
    adb:arch | adb:steamos) echo "android-tools" ;;
    adb:debian) echo "adb" ;;
    adb:fedora | adb:ostree | adb:suse) echo "android-tools" ;;
    adbudev:arch | adbudev:steamos) echo "android-udev" ;;
    adbudev:debian) echo "android-sdk-platform-tools-common" ;;
    adbudev:fedora | adbudev:ostree) echo "android-tools" ;;
    adbudev:suse) echo "android-udev-rules" ;;
    pipewire:arch | pipewire:steamos | pipewire:debian) echo "pipewire pipewire-pulse wireplumber" ;;
    pipewire:fedora | pipewire:ostree | pipewire:suse) echo "pipewire pipewire-pulseaudio wireplumber" ;;
    pactl:arch | pactl:steamos) echo "libpulse" ;;
    pactl:debian | pactl:fedora | pactl:ostree | pactl:suse) echo "pulseaudio-utils" ;;
    v4l2:arch) if [ "$OS_ID" = cachyos ]; then echo "v4l2loopback-dkms linux-cachyos-headers"; else echo "v4l2loopback-dkms linux-headers"; fi ;;
    v4l2:debian) echo "v4l2loopback-dkms" ;;
    v4l2:fedora | v4l2:ostree) echo "v4l2loopback" ;;
    v4l2:suse) echo "v4l2loopback-kmp-default" ;;
    ffmpeg:arch | ffmpeg:steamos) echo "ffmpeg" ;;
    ffmpeg:debian) echo "libavcodec-extra" ;;
    ffmpeg:fedora | ffmpeg:ostree) echo "libavcodec-free" ;;
    ffmpeg:suse) echo "libavcodec61" ;;
    *) echo "" ;;
    esac
}
# 入れ方を出す。sudo の要る物は出すだけで、ここでは実行しない
install_hint() {
    local what=$1 p
    p=$(pkg_names "$what" "$FAMILY")
    if [ -z "$p" ]; then
        info "$(M "パッケージマネージャで次の物をインストールしてください：$2" "Install this with your package manager: $2")"
        return
    fi
    info "$(M '次のコマンドでインストールしてください。このツールは sudo を実行しません：' 'Install it with the command below. This tool does not run sudo:')"
    case "$FAMILY" in
    arch) cmdline "sudo pacman -S --needed $p" ;;
    debian)
        [ "$what" = vulkan32 ] && cmdline "sudo dpkg --add-architecture i386 && sudo apt update"
        cmdline "sudo apt install $p"
        ;;
    fedora)
        cmdline "sudo dnf install $p"
        [ "$what" = v4l2 ] && info "$(M 'v4l2loopback は RPM Fusion にあります。' 'v4l2loopback is in RPM Fusion.')"
        ;;
    ostree)
        cmdline "rpm-ostree install $p"
        info "$(M 'そのあと PC を再起動してください。' 'Then restart the PC.')"
        ;;
    suse) cmdline "sudo zypper install $p" ;;
    steamos)
        if [ "$what" = v4l2 ]; then
            info "$(M 'SteamOS では v4l2loopback を使えません。PipeWire のカメラを使います。' 'SteamOS cannot use v4l2loopback. The PipeWire camera is used.')"
            return
        fi
        info "$(M 'SteamOS はシステムが読み取り専用です。このツールは案内だけを出します。入れるときは、デスクトップモードの Konsole で次を実行してください。OS を更新すると消えるので、更新のたびにもう一度実行してください。' 'SteamOS keeps the system read-only, so this tool only shows the steps. To install, run these in Konsole in Desktop Mode. An OS update removes them, so do this again after each update.')"
        cmdline "sudo steamos-readonly disable"
        cmdline "sudo pacman-key --init"
        cmdline "sudo pacman-key --populate archlinux holo"
        cmdline "sudo pacman -S --needed $p"
        cmdline "sudo steamos-readonly enable"
        ;;
    esac
}

# ---- 版の比べ方 ----
ver_ge() { [ -n "$1" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -n 1)" = "$2" ]; }

# ---- キットのフォルダ ----
looks_like_kit() {
    local n
    for n in run-on-iphone-gpu.sh start-pocketgpu.sh bin/pgpu-probe tools/kit.ini lib/x86_64/iphone_icd.x86_64.json "$AND_START"; do
        [ -e "$1/$n" ] && return 0
    done
    return 1
}
find_kit() {
    local r d e IFS=':' cands=()
    local roots=${PGPU_SEARCH_ROOTS:-$HERE/..:${HOME:-/nonexistent}:${HOME:-/nonexistent}/Downloads:${HOME:-/nonexistent}/Desktop:${HOME:-/nonexistent}/ダウンロード:${HOME:-/nonexistent}/デスクトップ}
    for r in $roots; do
        [ -d "$r" ] || continue
        for d in "$r"/PocketGPU*/ "$r"/*/PocketGPU*/; do
            [ -d "$d" ] || continue
            d=$(readlink -f -- "$d")
            looks_like_kit "$d" && cands+=("$d")
        done
    done
    [ ${#cands[@]} -eq 0 ] && return 0
    printf '%s\n' "${cands[@]}" | sort -u
}
KIT=
if [ -n "$KIT_ARG" ]; then
    if [ ! -d "$KIT_ARG" ]; then
        out "$(M 'そのフォルダがありません。' 'That folder does not exist.')"
        exit 2
    fi
    KIT=$(readlink -f -- "$KIT_ARG")
elif looks_like_kit "$HERE"; then
    KIT=$HERE
elif looks_like_kit "$HERE/.."; then
    KIT=$(readlink -f -- "$HERE/..")
else
    mapfile -t CANDS < <(find_kit)
    if [ ${#CANDS[@]} -eq 1 ]; then
        out "$(M 'キットのフォルダが見つかりました。' 'Found a kit folder.')"
        ask_yes "$(M "このフォルダのキットを直しますか？ ${CANDS[0]}" "Fix the kit in this folder? ${CANDS[0]}")" && KIT=${CANDS[0]}
    elif [ ${#CANDS[@]} -gt 1 ]; then
        out "$(M 'キットのフォルダが複数見つかりました。直すフォルダを --kit で指定してください。' 'Found more than one kit folder. Choose one with --kit.')"
        for c in "${CANDS[@]}"; do info "$c"; done
    fi
    if [ -z "$KIT" ]; then
        out "$(M 'キットのフォルダが見つかりません。./fix.sh --kit "キットのフォルダ" の形で、もう一度実行してください。' 'The kit folder was not found. Run this again as: ./fix.sh --kit "path to the kit folder"')"
        exit 2
    fi
fi
PROBE=${PGPU_TEST_PROBE:-$KIT/bin/pgpu-probe}
RUN="$KIT/run-on-iphone-gpu.sh"
TOKEN_FILE="$KIT/tools/broker-token"
PROTO=3
APPVER=
PLATFORM=
if [ -r "$KIT/tools/kit.ini" ]; then
    while IFS='=' read -r k v; do
        v=${v%$'\r'}
        case "$k" in proto) PROTO=$v ;; app) APPVER=$v ;; platform) PLATFORM=$v ;; esac
    done < "$KIT/tools/kit.ini"
fi
MYUID=$(id -u)
STAMP=$(date +%Y%m%d-%H%M%S)
BACKUP=
MASKS=()   # まとめとログで伏せる物（スマホのシリアルなど）

# ---- 入れ替える前の物を残す ----
backup_dir() {
    if [ -z "$BACKUP" ]; then
        BACKUP="$KIT/fix-backup/$STAMP"
        (umask 077 && mkdir -p -- "$BACKUP") || { BACKUP=; return 1; }
    fi
}
# キットの中の相対の場所を、同じ形で fix-backup/<日時>/ に写す
backup_copy() {
    backup_dir || return 1
    mkdir -p -- "$BACKUP/$(dirname -- "$1")" && cp -p -- "$KIT/$1" "$BACKUP/$1"
}

# ==== 直す動作（act_*）。状態を変えるのはここだけ。試験の sabotage は、下の印のあとで全部を何もしない物に差し替える ====
act_chmod_x() { chmod +x -- "$1"; }
act_to_lf() {
    local t="$1.pgpu-fix-tmp"
    tr -d '\r' < "$1" > "$t" || { rm -f -- "$t"; return 1; }
    chmod --reference="$1" -- "$t" 2> /dev/null
    mv -f -- "$t" "$1"
}
act_write_file() { printf '%s' "$2" > "$1"; }
act_move() { mkdir -p -- "$(dirname -- "$2")" && mv -- "$1" "$2"; }
act_signal() { kill "-$2" "$1" 2> /dev/null; }
act_idevicepair() { timeout 60 idevicepair pair 2>&1 < /dev/null; }
act_adb() { "$ADB" "$@" < /dev/null 2>&1; }
act_run_android_start() { (cd "$KIT" && ADB=$ADB ANDROID_SERIAL=$1 PGPU_PORT=$2 PGPU_NOPROMPT=1 PGPU_ANSWERS= timeout 120 bash "$KIT/$AND_START" "${@:3}" < /dev/null 2>&1); }
act_pactl_unload() { pactl unload-module "$1" 2>&1; }
# ---- ここまで直す動作 (act_*) ----

# ---- プロセス ----
proc_uid() { awk '/^Uid:/ {print $2; exit}' "/proc/$1/status" 2> /dev/null; }
proc_stat_after() { # /proc/PID/stat の、名前より後ろ（3 番目の値から）
    local s
    s=$(cat "/proc/$1/stat" 2> /dev/null) || return 1
    printf '%s' "${s##*) }"
}
proc_ppid() { local a; a=$(proc_stat_after "$1") || return 1; set -- $a; printf '%s' "$2"; }
proc_state() { local a; a=$(proc_stat_after "$1") || return 1; set -- $a; printf '%s' "$1"; }
proc_start() { local a; a=$(proc_stat_after "$1") || return 1; set -- $a; printf '%s' "${20}"; }
proc_alive() { [ -d "/proc/$1" ] && [ "$(proc_state "$1")" != Z ] && [ -n "$(proc_state "$1")" ]; }
proc_comm() { cat "/proc/$1/comm" 2> /dev/null || echo '?'; }
# 自分と、自分を起こした親たち（止めない）
SELF_PIDS=" $$ "
_p=$$
for _i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    _p=$(proc_ppid "$_p") || break
    [ -z "$_p" ] || [ "$_p" -le 1 ] && break
    SELF_PIDS+="$_p "
done
proc_abs() { # 引数の場所を、そのプロセスの今のフォルダから絶対の場所にする
    local a=$2 c
    case "$a" in /*) ;; *)
        c=$(readlink "/proc/$1/cwd" 2> /dev/null) || return 1
        a="$c/$a"
        ;;
    esac
    readlink -f -- "$a" 2> /dev/null || printf '%s' "$a"
}
# このユーザーの、このキットの物のプロセス。1 行に「PID 役 開始 親」。役：
#   probe-broker（--broker の受け渡し役）・probe-broker-quiet（起動ごとの受け渡し役）・broker-helper（run-on-iphone-gpu.sh --broker）
#   webcam_rx・ipaudio・miccam（start-mic-camera.sh）・andmiccam（Android のマイクとカメラ）
kit_procs() {
    local d pid args a i role path pr2
    pr2=$(readlink -f -- "$PROBE" 2> /dev/null || printf '%s' "$PROBE")
    for d in /proc/[0-9]*; do
        pid=${d#/proc/}
        case "$SELF_PIDS" in *" $pid "*) continue ;; esac
        [ "$(proc_uid "$pid")" = "$MYUID" ] || continue
        args=()
        mapfile -d '' -t args 2> /dev/null < "$d/cmdline" || continue
        [ ${#args[@]} -gt 0 ] || continue
        role=
        for i in 0 1; do
            a=${args[$i]:-}
            [ -n "$a" ] || continue
            case "$a" in -*) continue ;; esac
            path=$(proc_abs "$pid" "$a") || continue
            case "$path" in
            "$KIT/bin/pgpu-probe" | "$pr2")
                if [ "${args[$((i + 1))]:-}" = broker ]; then
                    if [ "${args[$((i + 2))]:-}" = --quiet ]; then role=probe-broker-quiet; else role=probe-broker; fi
                fi
                ;;
            "$KIT/run-on-iphone-gpu.sh") [ "${args[$((i + 1))]:-}" = --broker ] && role=broker-helper ;;
            "$KIT/bin/webcam_rx") role=webcam_rx ;;
            "$KIT/bin/ipaudio") role=ipaudio ;;
            "$KIT/start-mic-camera.sh") role=miccam ;;
            "$KIT/$AND_MICCAM") role=andmiccam ;;
            esac
            [ -n "$role" ] && break
        done
        [ -n "$role" ] || continue
        printf '%s %s %s %s\n' "$pid" "$role" "$(proc_start "$pid")" "$(proc_ppid "$pid")"
    done
}
# 止める。TERM で頼み、5 秒で終わらなければ KILL。聞いたあとでだけ呼ぶ。このユーザーのプロセスだけ
stop_pid() {
    local pid=$1 i
    proc_alive "$pid" || return 0
    if [ -n "${PGPU_TEST_STOP_ALLOW:-}" ]; then
        case ",$PGPU_TEST_STOP_ALLOW," in *",$pid,"*) ;; *)
            warn "PGPU_TEST_STOP_ALLOW: refused to stop PID $pid"
            return 1
            ;;
        esac
    fi
    if [ "$(proc_uid "$pid")" != "$MYUID" ]; then
        warn "$(M "PID $pid は別のユーザーのプロセスなので止めません。" "PID $pid belongs to another user, so it is not stopped.")"
        return 1
    fi
    act_signal "$pid" TERM
    for i in $(seq 1 50); do proc_alive "$pid" || return 0; sleep 0.1; done
    act_signal "$pid" KILL
    for i in $(seq 1 20); do proc_alive "$pid" || return 0; sleep 0.1; done
    warn "$(M "PID $pid を止められませんでした。" "Could not stop PID $pid.")"
    return 1
}
proc_label() { printf '%s (PID %s)' "$(proc_comm "$1")" "$1"; }

# ---- ポート ----
port_listening() {
    local hex
    hex=$(printf '%04X' "$1")
    awk -v p="$hex" '$4 == "0A" && toupper(substr($2, length($2) - 3)) == p {f = 1} END {exit !f}' /proc/net/tcp /proc/net/tcp6 2> /dev/null
}
# ポートで待っているプロセス。1 行に「名前 PID」。ss が無いか、別のユーザーの物は出ない
port_owners() {
    command -v ss > /dev/null 2>&1 || return 0
    ss -H -ltnp "sport = :$1" 2> /dev/null | grep -o '("[^"]*",pid=[0-9]*' | sed 's/("\([^"]*\)",pid=\([0-9]*\)/\1 \2/' | sort -u
}

# ---- ファイルの中身 ----
# ELF の種類：「64 x86_64」「32 i386」「64 aarch64」など。ELF でなければ空
elf_kind() {
    local b cls mach
    b=$(od -An -tx1 -N20 -- "$1" 2> /dev/null | tr -s ' \n' ' ')
    set -- $b
    [ $# -ge 20 ] || return 0
    [ "$1$2$3$4" = 7f454c46 ] || return 0
    case "$5" in 01) cls=32 ;; 02) cls=64 ;; *) cls=? ;; esac
    case "${20}${19}" in 003e) mach=x86_64 ;; 0003) mach=i386 ;; 00b7) mach=aarch64 ;; 0028) mach=arm ;; *) mach="0x${20}${19}" ;; esac
    printf '%s %s' "$cls" "$mach"
}
has_crlf() { grep -q $'\r' -- "$1" 2> /dev/null; }
# ドライバの json を読む。正しければ空、だめなら理由（badjson・badlib・wrongarch）
json_problem() {
    local f=$1 want_arch=$2 dir lib arch
    dir=$(dirname -- "$f")
    if command -v python3 > /dev/null 2>&1; then
        local r
        r=$(python3 -I -c 'import json,sys
try:
    j=json.load(open(sys.argv[1],encoding="utf-8")); i=j["ICD"]; print(i["library_path"]); print(i.get("library_arch",""))
except Exception: sys.exit(1)' "$f" 2> /dev/null) || { echo badjson; return; }
        lib=$(printf '%s\n' "$r" | sed -n 1p)
        arch=$(printf '%s\n' "$r" | sed -n 2p)
    else
        grep -q '"ICD"' "$f" && grep -q '"file_format_version"' "$f" || { echo badjson; return; }
        lib=$(sed -n 's/.*"library_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$f" | head -n 1)
        arch=$(sed -n 's/.*"library_arch"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$f" | head -n 1)
        [ -n "$lib" ] || { echo badjson; return; }
    fi
    [ -n "$arch" ] && [ "$arch" != "$want_arch" ] && { echo wrongarch; return; }
    case "$lib" in /*) ;; *) lib="$dir/$lib" ;; esac
    [ -f "$lib" ] || { echo badlib; return; }
}
# 作り直す json（キットを作る make_linux_kit.py と同じ形）。$1 = 64 か 32、$2 = 前の json の api_version（無ければ 1.2.0）
json_text() {
    printf '{\n    "file_format_version": "1.0.1",\n    "ICD": {\n        "library_path": "./libiphone_icd.so",\n        "library_arch": "%s",\n        "api_version": "%s"\n    }\n}\n' "$1" "${2:-1.2.0}"
}

probe() { "$PROBE" "$@" 2> /dev/null; }
probe_runs() {
    [ -e "$PROBE" ] || return 1
    "$PROBE" usb > /dev/null 2>&1
    [ $? -lt 126 ]
}

out "$(M 'PocketGPU を直します：Linux' 'Fix PocketGPU (Linux)')"
out "$(M 'start-pocketgpu.sh と同じ所をチェックして、直せる物は 1 つずつ聞いてから直します。何も消しません。sudo も実行しません。' 'This checks the same things as start-pocketgpu.sh and fixes what it can, asking before each fix. Nothing is deleted, and sudo is never run.')"
[ "$CHECK_ONLY" = 1 ] && out "$(M '--check-only：チェックだけです。何も変更しません。' '--check-only: checking only. Nothing is changed.')" "$C_HEAD"

CTX_MUX_OK=0
CTX_DEV=
CTX_ANDROID_PORT_OK=0
CTX_LOADER=
CTX_DESKTOP=none
ADB=${ADB:-}
PGPU_PORT=${PGPU_PORT:-50300}
case "$PGPU_PORT" in '' | *[!0-9]*) PGPU_PORT=50300 ;; esac
CAM_PORT=${PGPU_CAM_PORT:-50064}
AUD_PORT=${PGPU_AUDIO_PORT:-50063}
has_android_files() { [ -e "$KIT/$AND_START" ] || [ -d "$KIT/android" ]; }
has_media_files() { [ -e "$KIT/start-mic-camera.sh" ] || [ -e "$KIT/bin/webcam_rx" ] || [ -e "$KIT/bin/ipaudio" ] || [ -e "$KIT/$AND_MICCAM" ]; }

# ==== 1. この PC ====
if want pc; then
    step 1 "$(M 'この PC' 'This PC')"
    t=$(M 'この PC' 'This PC')
    st=ok
    notes=()
    ARCH=$(uname -m)
    KREL=$(uname -r)
    GLIBC=$(getconf GNU_LIBC_VERSION 2> /dev/null | awk '{print $2}')
    if [ "$ARCH" != x86_64 ]; then
        ng "$(M "このキットは x86_64 の PC 用です。この PC は $ARCH です。" "This kit is for x86_64 PCs. This PC is $ARCH.")"
        st=unfixed
        notes+=("arch $ARCH")
    else
        ok "$(M "${OS_NAME:-Linux}・カーネル $KREL・glibc ${GLIBC:-?}・x86_64" "${OS_NAME:-Linux}, kernel $KREL, glibc ${GLIBC:-?}, x86_64")"
    fi
    logonly "family=$FAMILY"
    if [ -n "${GLIBC:-}" ] && ! ver_ge "$GLIBC" 2.31; then
        ng "$(M "glibc $GLIBC は古すぎます。2.31 以上（Ubuntu 20.04 以降など）が必要です。" "glibc $GLIBC is too old. 2.31 or newer (Ubuntu 20.04 or later, for example) is needed.")"
        st=unfixed
        notes+=("glibc $GLIBC")
    fi
    if [ "$MYUID" = 0 ]; then
        ng "$(M 'root か sudo で実行しています。ゲームを起動するのと同じ、ふつうのユーザーで実行してください。root で直すと、ファイルの持ち主が root になって、あとで使えなくなります。' 'This runs as root (sudo). Run it as the normal user who starts the games. Fixing as root makes root own the files, and then they cannot be used.')"
        info "$(M 'sudo を付けずに、もう一度 ./fix.sh を実行してください。' 'Run ./fix.sh again without sudo.')"
        st=unfixed
        notes+=("$(M 'root で実行していた' 'ran as root')")
    fi
    OPTS=$(findmnt -no OPTIONS -T "$KIT" 2> /dev/null || true)
    case ",$OPTS," in *,noexec,*)
        ng "$(M 'キットのフォルダが、プログラムの実行が禁止された noexec の場所にあります。' 'The kit folder is on a drive where programs may not run (noexec).')"
        info "$(M 'キットを、ホームフォルダなど、ふつうの場所に展開し直してください。このツールは移動しません。' 'Extract the kit again in a normal place such as your home folder. This tool does not move it.')"
        st=unfixed
        notes+=("noexec")
        ;;
    esac
    if [ -n "${DISPLAY:-}" ]; then
        CTX_DESKTOP=X11
        [ -n "${WAYLAND_DISPLAY:-}" ] && CTX_DESKTOP=Wayland+Xwayland
        ok "$(M "デスクトップ：$CTX_DESKTOP" "Desktop: $CTX_DESKTOP")"
    elif [ -n "${WAYLAND_DISPLAY:-}" ]; then
        CTX_DESKTOP=Wayland
        warn "$(M 'Wayland だけのセッションです。GPU のウィンドウは X11 にだけ表示できるので、Xwayland が必要です。' 'This session is Wayland only. GPU windows can only be shown on X11, so Xwayland is needed.')"
        [ "$st" = ok ] && st=warn
        notes+=("$(M 'Xwayland が無い' 'no Xwayland')")
    else
        info "$(M 'デスクトップが見つかりません。ウィンドウを出さない計算だけのアプリは使えます。' 'No desktop was found. Programs that only compute and show no window can still be used.')"
    fi
    if probe_runs; then
        WW=$(probe ww)
        logonly "$WW"
        [ "${WW#WW OK}" = "$WW" ] && info "$(M 'このカーネルでは、メモリの書き込みを効率よく検出できません。動作はしますが、CPU の負荷が高くなります。カーネル 6.7 以降で改善します。' 'This kernel cannot track memory writes efficiently. It still works, but uses more CPU. Kernel 6.7 or later is better.')"
    fi
    [ "$FAMILY" = steamos ] && info "$(M 'SteamOS です。このツールはデスクトップモードで実行してください。' 'SteamOS detected. Run this tool in Desktop Mode.')"
    add_result pc "$st" "$t" "$(join_by ', ' "${notes[@]}")"
fi

# ==== 2. キットのファイル ====
# 1 つ = 「場所|種類」。種類：elf64・elf32・elfarm64・json64・json32・sh（実行する台本）・shlib（. で読む台本）・ini・apk
required_files() {
    printf '%s\n' "bin/pgpu-probe|elf64" "lib/x86_64/libiphone_icd.so|elf64" "lib/x86_64/iphone_icd.x86_64.json|json64" \
        "start-pocketgpu.sh|sh" "run-on-iphone-gpu.sh|sh" "tools/kit.ini|ini"
    if [ -d "$KIT/lib/i386" ]; then printf '%s\n' "lib/i386/libiphone_icd.so|elf32" "lib/i386/iphone_icd.i386.json|json32"; fi
    if [ -e "$KIT/start-mic-camera.sh" ] || [ -e "$KIT/bin/webcam_rx" ] || [ -e "$KIT/bin/ipaudio" ]; then
        printf '%s\n' "start-mic-camera.sh|sh" "bin/webcam_rx|elf64" "bin/ipaudio|elf64"
    fi
    if has_android_files; then
        printf '%s\n' "$AND_START|sh" "$AND_STOP|sh" "$AND_CHECK|sh" "$AND_RUN|sh" "$AND_COMMON|shlib" "$AND_SERVER|elfarm64"
        [ -e "$KIT/$AND_MICCAM" ] && printf '%s\n' "$AND_MICCAM|sh"
        [ -e "$KIT/$AND_APK" ] && printf '%s\n' "$AND_APK|apk"
    fi
    return 0
}
# 欠け・壊れの一覧。1 行に「場所|理由」。理由：missing・empty・notelf・wrongarch・badjson・badlib・crlf・noexec・parse・notapk
check_files() {
    local line p k f e jp
    while IFS= read -r line; do
        p=${line%%|*}
        k=${line#*|}
        f="$KIT/$p"
        if [ ! -f "$f" ]; then echo "$p|missing"; continue; fi
        if [ ! -s "$f" ]; then echo "$p|empty"; continue; fi
        case "$k" in
        elf64 | elf32 | elfarm64)
            e=$(elf_kind "$f")
            if [ -z "$e" ]; then echo "$p|notelf"
            elif [ "$k" = elf64 ] && [ "$e" != "64 x86_64" ]; then echo "$p|wrongarch"
            elif [ "$k" = elf32 ] && [ "$e" != "32 i386" ]; then echo "$p|wrongarch"
            elif [ "$k" = elfarm64 ] && [ "$e" != "64 aarch64" ]; then echo "$p|wrongarch"
            elif [ "${p#bin/}" != "$p" ] && [ ! -x "$f" ]; then echo "$p|noexec"
            fi
            ;;
        json64 | json32)
            jp=$(json_problem "$f" "${k#json}")
            [ -n "$jp" ] && echo "$p|$jp"
            ;;
        sh | shlib)
            if has_crlf "$f"; then echo "$p|crlf"
            elif ! bash -n "$f" 2> /dev/null; then echo "$p|parse"
            fi
            [ "$k" = sh ] && [ ! -x "$f" ] && echo "$p|noexec"
            ;;
        apk) [ "$(head -c 2 "$f" 2> /dev/null)" = PK ] || echo "$p|notapk" ;;
        esac
    done < <(required_files)
    return 0
}
why_text() {
    case "$1" in
    missing) M '無い' 'missing' ;;
    empty) M '中身が空' 'empty' ;;
    notelf) M '壊れている。Linux のプログラムとして読めない' 'damaged (not a Linux program file)' ;;
    wrongarch) M '種類が違う。64・32 ビットや CPU が合わない' 'wrong kind (64/32-bit or CPU does not match)' ;;
    badjson) M '壊れている。json として読めない' 'damaged (not valid json)' ;;
    badlib) M 'json が指すドライバのファイルが無い' 'the driver file named in the json is missing' ;;
    crlf) M '行末が Windows の形の CRLF になっている' 'has Windows line endings (CRLF)' ;;
    noexec) M '実行の権限が無い' 'not executable' ;;
    parse) M '台本として読めない' 'cannot be read as a script' ;;
    notapk) M 'アプリのファイルとして読めない' 'not an app file' ;;
    *) printf '%s' "$1" ;;
    esac
}
if want files; then
    step 2 "$(M 'キットのファイル' 'Kit files')"
    t=$(M 'キットのファイル' 'Kit files')
    mapfile -t BAD < <(check_files)
    if [ ${#BAD[@]} -eq 0 ]; then
        ok "$(M '必要なファイルがそろっていて、壊れていません。' 'All needed files are here and not damaged.')"
        add_result files ok "$t"
    else
        ng "$(M "欠けている・壊れているファイルが ${#BAD[@]} 個あります。" "${#BAD[@]} file(s) are missing or damaged.")"
        for b in "${BAD[@]}"; do info "${b%%|*}  ->  $(why_text "${b#*|}")"; done
        CRLF=()
        NOEX=()
        JSONFIX=()
        for b in "${BAD[@]}"; do
            case "${b#*|}" in
            crlf) CRLF+=("${b%%|*}") ;;
            noexec) NOEX+=("${b%%|*}") ;;
            badjson | wrongarch | badlib)
                case "${b%%|*}" in *.json)
                    # 横の .so が正しいときだけ、json を作り直せば直る
                    so="$(dirname -- "${b%%|*}")/libiphone_icd.so"
                    want_kind="64 x86_64"
                    case "${b%%|*}" in *i386*) want_kind="32 i386" ;; esac
                    [ "$(elf_kind "$KIT/$so")" = "$want_kind" ] && JSONFIX+=("${b%%|*}")
                    ;;
                esac
                ;;
            esac
        done
        acts=()
        if [ ${#CRLF[@]} -gt 0 ]; then
            info "$(M '直し方：行末を Linux の形の LF に直します。Windows を通してコピーしたり、Windows のエディタで開いて保存したりすると、この形になります。前の物は fix-backup/ に残します。' 'Fix: change the line endings to the Linux form (LF). This happens when the kit is copied through Windows or saved in a Windows editor. The old files are kept in fix-backup/.')"
            if offer "$(M "行末を直しますか？ $(join_by ', ' "${CRLF[@]}")" "Fix the line endings? $(join_by ', ' "${CRLF[@]}")")"; then
                for p in "${CRLF[@]}"; do
                    if backup_copy "$p" && act_to_lf "$KIT/$p"; then acts+=("lf $p"); else warn "$(M "直せませんでした：$p" "Could not fix: $p")"; fi
                done
            fi
        fi
        if [ ${#NOEX[@]} -gt 0 ]; then
            info "$(M '直し方：chmod +x で実行の権限を付けます。zip で展開したり、Windows のドライブを通したりすると、権限が無くなります。前の権限は fix-backup/ に書いて残します。' 'Fix: make them executable (chmod +x). Permissions are lost when the kit is unpacked from a zip or copied through a Windows drive. The old permissions are written down in fix-backup/.')"
            if offer "$(M "実行の権限を付けますか？ $(join_by ', ' "${NOEX[@]}")" "Make them executable? $(join_by ', ' "${NOEX[@]}")")"; then
                if backup_dir; then
                    for p in "${NOEX[@]}"; do
                        stat -c '%a %n' -- "$KIT/$p" 2> /dev/null | sed "s#$KIT/##" >> "$BACKUP/permissions-before.txt"
                        if act_chmod_x "$KIT/$p"; then acts+=("chmod $p"); else warn "$(M "直せませんでした：$p" "Could not fix: $p")"; fi
                    done
                fi
            fi
        fi
        if [ ${#JSONFIX[@]} -gt 0 ]; then
            info "$(M '直し方：ドライバの json を、キットと同じ形で作り直します。前の物は fix-backup/ に残します。' 'Fix: write the driver json again in the same form as the kit. The old file is kept in fix-backup/.')"
            if offer "$(M "json を作り直しますか？ $(join_by ', ' "${JSONFIX[@]}")" "Write the json again? $(join_by ', ' "${JSONFIX[@]}")")"; then
                for p in "${JSONFIX[@]}"; do
                    arch=64
                    case "$p" in *i386*) arch=32 ;; esac
                    api=$(sed -n 's/.*"api_version"[[:space:]]*:[[:space:]]*"\([0-9.]*\)".*/\1/p' "$KIT/$p" 2> /dev/null | head -n 1)
                    if backup_copy "$p" && act_write_file "$KIT/$p" "$(json_text "$arch" "$api")"; then acts+=("json $p"); else warn "$(M "直せませんでした：$p" "Could not fix: $p")"; fi
                done
            fi
        fi
        mapfile -t BAD2 < <(check_files)
        if [ ${#BAD2[@]} -eq 0 ]; then
            ok "$(M '直りました。' 'Fixed.')"
            [ -n "$BACKUP" ] && info "$(M '前の物：fix-backup/' 'Old files: fix-backup/')$STAMP"
            add_result files fixed "$t" "$(join_by ', ' "${acts[@]}")"
        else
            [ ${#acts[@]} -gt 0 ] && ng "$(M 'まだ直っていないファイルがあります：' 'Still not fixed:')"
            for b in "${BAD2[@]}"; do [ ${#acts[@]} -gt 0 ] && info "${b%%|*}  ->  $(why_text "${b#*|}")"; done
            hard=0
            for b in "${BAD2[@]}"; do case "${b#*|}" in missing | empty | notelf | wrongarch | parse | notapk | badlib) hard=1 ;; esac; done
            if [ "$hard" = 1 ]; then
                info "$(M '欠けている・壊れているファイルは、このツールでは取り直せません。配られた tar.gz から、キットを展開し直してください：' 'This tool cannot get missing or damaged files again. Extract the kit again from the tar.gz you were given:')"
                cmdline "tar xzf PocketGPU-linux-*.tar.gz"
                info "$(M 'tar.gz が無いときは、Discord の #get-kit で /kit と入力して手順を見てください。' 'If you do not have the tar.gz, type /kit in #get-kit on Discord and follow the steps.')"
            fi
            nm=()
            for b in "${BAD2[@]}"; do nm+=("${b%%|*} (${b#*|})"); done
            add_result files unfixed "$t" "$(join_by ', ' "${nm[@]}")"
        fi
    fi
fi

# ==== 3. Vulkan のローダー ====
loader_version() { # 1 行目に版、2 行目にどこから読んだか。分からなければ空
    local l v f
    if probe_runs; then
        l=$(probe loader)
        logonly "$l"
        if [ "${l#LOADER OK}" != "$l" ]; then printf '%s\nprobe\n' "$(printf '%s' "$l" | awk '{print $3}')"; return; fi
        [ "$l" = "LOADER NG missing" ] && return
    fi
    if command -v vulkaninfo > /dev/null 2>&1; then
        v=$(timeout 20 vulkaninfo --summary 2> /dev/null | sed -n 's/.*Vulkan Instance Version:[[:space:]]*\([0-9.]*\).*/\1/p' | head -n 1)
        [ -n "$v" ] && { printf '%s\nvulkaninfo\n' "$v"; return; }
    fi
    for f in /usr/lib/x86_64-linux-gnu/libvulkan.so.1 /usr/lib64/libvulkan.so.1 /usr/lib/libvulkan.so.1; do
        [ -e "$f" ] || continue
        [ "$(elf_kind "$(readlink -f "$f")")" = "64 x86_64" ] || continue
        v=$(basename -- "$(readlink -f -- "$f")" | sed -n 's/^libvulkan\.so\.\([0-9]*\.[0-9]*\.[0-9]*\)$/\1/p')
        printf '%s\nfile\n' "${v:-?}"
        return
    done
}
has_vulkan32() {
    local f
    for f in /usr/lib32/libvulkan.so.1 /usr/lib/i386-linux-gnu/libvulkan.so.1 /usr/lib/libvulkan.so.1; do
        [ -e "$f" ] && [ "$(elf_kind "$(readlink -f -- "$f")")" = "32 i386" ] && return 0
    done
    return 1
}
if want loader; then
    step 3 "$(M 'Vulkan のローダー' 'Vulkan loader')"
    t=$(M 'Vulkan のローダー' 'Vulkan loader')
    st=ok
    notes=()
    mapfile -t LVR < <(loader_version)
    LV=${LVR[0]:-}
    CTX_LOADER=$LV
    if [ -z "$LV" ]; then
        ng "$(M 'Vulkan のローダー libvulkan.so.1 がありません。' 'The Vulkan loader libvulkan.so.1 is missing.')"
        install_hint vulkan "Vulkan loader"
        st=unfixed
        notes+=("$(M 'libvulkan.so.1 が無い' 'libvulkan.so.1 missing')")
    else
        ok "$(M "Vulkan のローダー $LV" "Vulkan loader $LV")"
        if [ "$LV" != "?" ] && ! ver_ge "$LV" 1.3.207; then
            warn "$(M "ローダー $LV は 1.3.207 より前なので、VK_DRIVER_FILES も VK_ADD_DRIVER_FILES も読みません。VK_ICD_FILENAMES だけが効きます。" "Loader $LV is older than 1.3.207, so it reads neither VK_DRIVER_FILES nor VK_ADD_DRIVER_FILES. Only VK_ICD_FILENAMES works.")"
            st=warn
            notes+=("loader $LV < 1.3.207")
            if [ -f "$RUN" ]; then
                ENVOUT=$(IPHONE_VK_ONLY=1 bash "$RUN" --env 2> /dev/null)
                if printf '%s\n' "$ENVOUT" | grep -q '^export VK_ICD_FILENAMES='; then
                    ok "$(M 'run-on-iphone-gpu.sh は VK_ICD_FILENAMES も渡すので、このローダーでも動きます。' 'run-on-iphone-gpu.sh also sets VK_ICD_FILENAMES, so it works with this loader.')"
                else
                    ng "$(M 'run-on-iphone-gpu.sh が VK_ICD_FILENAMES を渡しません。古いキットです。新しいキットを展開し直してください。' 'run-on-iphone-gpu.sh does not set VK_ICD_FILENAMES. This is an old kit. Extract a new kit.')"
                    st=unfixed
                    notes+=("$(M '古い起動の台本' 'old launcher')")
                fi
            fi
            if [ "${IPHONE_VK_ONLY:-1}" = 0 ]; then
                warn "$(M 'PC の GPU と一緒に見せる設定 IPHONE_VK_ONLY=0 は、このローダーでは使えません。設定を外してください。' 'IPHONE_VK_ONLY=0 (show the PC GPUs too) does not work with this loader. Remove that setting.')"
            fi
            info "$(M 'ローダーは、ディストリの更新で新しくなります。' 'The loader becomes newer when you update your distribution.')"
        elif [ "$LV" != "?" ] && ! ver_ge "$LV" 1.3.234; then
            info "$(M "ローダー $LV は VK_DRIVER_FILES を読みます（標準の設定で動きます）。VK_ADD_DRIVER_FILES は 1.3.234 からなので、IPHONE_VK_ONLY=0（PC の GPU と一緒に見せる設定）は使えません。" "Loader $LV reads VK_DRIVER_FILES, so the default setting works. VK_ADD_DRIVER_FILES needs 1.3.234, so IPHONE_VK_ONLY=0 (show the PC GPUs too) does not work.")"
            if [ "${IPHONE_VK_ONLY:-1}" = 0 ]; then
                warn "$(M 'いま IPHONE_VK_ONLY=0 が設定されています。このローダーでは GPU が見えません。設定を外してください。' 'IPHONE_VK_ONLY=0 is set now. With this loader the GPU is not seen. Remove that setting.')"
                st=warn
                notes+=("IPHONE_VK_ONLY=0 with loader $LV")
            fi
        fi
        # 利用者の環境変数が、ドライバを隠していないか（シェルの設定ファイルは変えない。案内だけ）
        for v in VK_LOADER_DRIVERS_DISABLE VK_LOADER_DRIVERS_SELECT; do
            val=${!v:-}
            [ -n "$val" ] || continue
            hide=0
            case "$v:$val" in
            VK_LOADER_DRIVERS_DISABLE:*iphone* | VK_LOADER_DRIVERS_DISABLE:"*" | VK_LOADER_DRIVERS_DISABLE:*"~all~"*) hide=1 ;;
            VK_LOADER_DRIVERS_SELECT:*iphone* | VK_LOADER_DRIVERS_SELECT:"*") ;;
            VK_LOADER_DRIVERS_SELECT:*) hide=1 ;;
            esac
            if [ "$hide" = 1 ]; then
                warn "$(M "環境変数 $v=$val が、iPhone のドライバを隠します。" "The environment variable $v=$val hides the iPhone driver.")"
                info "$(M '~/.profile・~/.bashrc・/etc/environment・~/.config/environment.d/ のどこかで設定されています。その行を消すか、先頭に # を付けてから、ログインし直してください。このツールは設定ファイルを変えません。' 'It is set in ~/.profile, ~/.bashrc, /etc/environment or ~/.config/environment.d/. Remove that line or put # in front of it, then log in again. This tool does not change those files.')"
                [ "$st" = ok ] && st=warn
                notes+=("$v")
            fi
        done
        if [ -d "$KIT/lib/i386" ] && ! has_vulkan32; then
            warn "$(M '32 ビットの Vulkan のローダーがありません。32 ビットのゲームでは GPU が見えません。64 ビットのゲームは動きます。' 'The 32-bit Vulkan loader is missing. 32-bit games will not see the GPU. 64-bit games work.')"
            install_hint vulkan32 "32-bit Vulkan loader"
            [ "$st" = ok ] && st=warn
            notes+=("$(M '32 ビットのローダーが無い' 'no 32-bit loader')")
        fi
    fi
    add_result loader "$st" "$t" "$(join_by ', ' ${LV:+"loader $LV"} "${notes[@]}")"
fi

# ==== 4. usbmuxd ====
find_usbmuxd() {
    local d IFS=':'
    command -v usbmuxd 2> /dev/null && return
    for d in ${PGPU_SBIN:-/usr/sbin:/usr/bin:/sbin}; do [ -x "$d/usbmuxd" ] && { echo "$d/usbmuxd"; return; }; done
    return 0
}
if want usbmuxd; then
    step 4 "$(M 'usbmuxd と libimobiledevice' 'usbmuxd and libimobiledevice')"
    t=$(M 'usbmuxd' 'usbmuxd')
    st=ok
    notes=()
    HAVE_MUXD=$(find_usbmuxd)
    if [ -z "$HAVE_MUXD" ]; then
        ng "$(M 'usbmuxd がインストールされていません。Linux が USB で iPhone と通信するために必要です。' 'usbmuxd is not installed. Linux needs it to talk to the iPhone over USB.')"
        install_hint usbmuxd "usbmuxd libimobiledevice"
        st=unfixed
        notes+=("$(M 'usbmuxd が入っていない' 'usbmuxd not installed')")
    else
        ok "$(M 'usbmuxd がインストールされています。' 'usbmuxd is installed.')"
        if command -v idevicepair > /dev/null 2>&1; then
            ok "$(M 'libimobiledevice のツール idevicepair があります。' 'The libimobiledevice tool idevicepair is installed.')"
        else
            warn "$(M 'libimobiledevice のツール idevicepair がありません。ペアリングに必要になることがあります。' 'The libimobiledevice tool idevicepair is missing. It may be needed for pairing.')"
            install_hint idevice "libimobiledevice tools"
            st=warn
            notes+=("$(M 'idevicepair が無い' 'no idevicepair')")
        fi
    fi
    for v in IPHONE_VK_MUX USBMUXD_SOCKET_ADDRESS; do
        [ -n "${!v:-}" ] || continue
        warn "$(M "環境変数 $v が設定されています（${!v}）。usbmuxd の場所が変わります。わざと設定したのでなければ、外してください。" "The environment variable $v is set (${!v}). It changes where usbmuxd is looked for. Unless you set it on purpose, remove it.")"
        [ "$st" = ok ] && st=warn
        notes+=("$v")
    done
    if probe_runs; then
        MUX=$(probe mux)
        logonly "$MUX"
        USBN=$(probe usb | awk '$1 == "USB" {print $2; exit}')
        if [ "${MUX#MUX OK}" != "$MUX" ]; then
            ok "$(M 'usbmuxd が動いています。' 'usbmuxd is running.')"
            CTX_MUX_OK=1
        else
            why=${MUX##* }
            where=$(printf '%s' "$MUX" | awk '{print $3}')
            case "$why" in
            refused)
                ng "$(M "usbmuxd のソケット（$where）は残っていますが、usbmuxd が答えません。usbmuxd が止まったあとの、古いソケットが残っています。" "The usbmuxd socket ($where) exists, but usbmuxd does not answer. An old socket remains after usbmuxd stopped.")"
                info "$(M '次のコマンドで usbmuxd を起動し直すと、ソケットも作り直されます。このツールは sudo を実行しません：' 'Restarting usbmuxd with the command below also makes the socket again. This tool does not run sudo:')"
                cmdline "sudo systemctl restart usbmuxd"
                info "$(M 'systemd の無い PC では、iPhone のケーブルを抜いて接続し直すか、PC を再起動してください。' 'On a PC without systemd, unplug and plug in the iPhone cable again, or restart the PC.')"
                st=unfixed
                notes+=("$(M '古いソケット' 'stale socket')")
                ;;
            denied)
                ng "$(M "usbmuxd のソケット（$where）に接続する権限がありません。" "No permission to connect to the usbmuxd socket ($where).")"
                info "$(M "ソケットの権限：$(stat -c '%A %U:%G' -- "$where" 2> /dev/null || echo '?')" "Socket permissions: $(stat -c '%A %U:%G' -- "$where" 2> /dev/null || echo '?')")"
                info "$(M '次のコマンドで usbmuxd を起動し直すか、PC を再起動してください：' 'Restart usbmuxd with the command below, or restart the PC:')"
                cmdline "sudo systemctl restart usbmuxd"
                st=unfixed
                notes+=("$(M 'ソケットの権限' 'socket permission')")
                ;;
            *)
                if [ -z "$HAVE_MUXD" ]; then
                    :
                elif [ "${USBN:-0}" = 0 ]; then
                    info "$(M 'usbmuxd は止まっています。iPhone を USB でつなぐと、多くのディストリでは自動で起動します。いまは iPhone がつながっていないので、問題ではありません。' 'usbmuxd is stopped. On most distributions it starts by itself when an iPhone is plugged in. No iPhone is plugged in now, so this is fine.')"
                    [ "$st" = ok ] && st=warn
                    notes+=("$(M 'iPhone が USB に無い' 'no iPhone on USB')")
                else
                    ng "$(M 'iPhone は USB でつながっていますが、usbmuxd が起動していません。' 'The iPhone is plugged in, but usbmuxd is not running.')"
                    if command -v systemctl > /dev/null 2>&1; then
                        logonly "systemctl is-active usbmuxd: $(systemctl is-active usbmuxd 2> /dev/null)"
                    fi
                    info "$(M '次のコマンドで起動してください。このツールは sudo を実行しません：' 'Start it with the command below. This tool does not run sudo:')"
                    cmdline "sudo systemctl restart usbmuxd"
                    info "$(M 'つなぐたびに起動しないときは、udev の規則が無いかもしれません。手順 5 を見てください。' 'If it does not start each time you plug in, the udev rule may be missing. See step 5.')"
                    st=unfixed
                    notes+=("$(M 'usbmuxd が起動していない' 'usbmuxd not running')")
                fi
                ;;
            esac
        fi
    else
        info "$(M 'bin/pgpu-probe を実行できないので、usbmuxd が動いているかはチェックできません。手順 2 を見てください。' 'bin/pgpu-probe cannot run, so whether usbmuxd runs cannot be checked. See step 2.')"
    fi
    add_result usbmuxd "$st" "$t" "$(join_by ', ' "${notes[@]}")"
fi
[ "$CTX_MUX_OK" = 0 ] && ! want usbmuxd && probe_runs && [ "$(probe mux | cut -c1-6)" = "MUX OK" ] && CTX_MUX_OK=1

# ==== 5. udev の規則とグループ ====
udev_files() { # 規則のファイルの一覧（同じ名前は前のフォルダが勝つ）
    local d f n seen=" " IFS=':'
    for d in ${PGPU_UDEV_DIRS:-/etc/udev/rules.d:/run/udev/rules.d:/usr/lib/udev/rules.d:/lib/udev/rules.d}; do
        [ -d "$d" ] || continue
        for f in "$d"/*.rules; do
            [ -f "$f" ] || continue
            n=$(basename -- "$f")
            case "$seen" in *" $n "*) continue ;; esac
            seen+="$n "
            echo "$f"
        done
    done
    return 0
}
in_groups() { case " $2 " in *" $1 "*) return 0 ;; esac; return 1; }
USERNAME_=$(id -un 2> /dev/null || echo "${USER:-}")
GROUPS_NOW=$(id -nG 2> /dev/null)
GROUPS_DB=$(id -nG "$USERNAME_" 2> /dev/null)
# グループの状態を見て、要るなら案内を出す。$1 = グループ。0：入っている・1：DB にはあるがまだログインし直していない・2：入っていない
group_state() {
    if in_groups "$1" "$GROUPS_NOW"; then return 0; fi
    if in_groups "$1" "$GROUPS_DB"; then return 1; fi
    return 2
}
group_advice() {
    case "$2" in
    1) info "$(M "グループ $1 には入っていますが、まだこのログインに効いていません。ログアウトしてログインし直すか、PC を再起動してください。" "You are in the group $1, but this login does not have it yet. Log out and log in again, or restart the PC.")" ;;
    2)
        info "$(M "グループ $1 に入る必要があります。次のコマンドを実行してから、ログアウトしてログインし直してください。このツールは sudo を実行しません：" "You need to join the group $1. Run the command below, then log out and log in again. This tool does not run sudo:")"
        getent group "$1" > /dev/null 2>&1 || cmdline "sudo groupadd $1"
        cmdline "sudo usermod -aG $1 \"\$USER\""
        ;;
    esac
}
android_wanted() {
    has_android_files || return 1
    [ -n "$ADB" ] && [ -x "$ADB" ] && return 0
    command -v adb > /dev/null 2>&1
}
if want udev; then
    step 5 "$(M 'udev の規則とグループ' 'udev rules and groups')"
    t=$(M 'udev の規則とグループ' 'udev rules and groups')
    st=ok
    notes=()
    mapfile -t RULES < <(udev_files)
    logonly "udev rules: ${#RULES[@]}"
    if [ -n "$(find_usbmuxd)" ]; then
        MUXRULE=
        for f in "${RULES[@]}"; do
            case "$(basename -- "$f")" in *usbmuxd*) MUXRULE=$f; break ;; esac
            grep -q 'usbmuxd' -- "$f" 2> /dev/null && { MUXRULE=$f; break; }
        done
        if [ -n "$MUXRULE" ]; then
            ok "$(M "usbmuxd の udev の規則があります：$(basename -- "$MUXRULE")" "The usbmuxd udev rule is here: $(basename -- "$MUXRULE")")"
        else
            warn "$(M 'usbmuxd の udev の規則が見つかりません。iPhone をつないでも、usbmuxd が自動で起動しないことがあります。' 'The usbmuxd udev rule was not found. usbmuxd may not start by itself when the iPhone is plugged in.')"
            info "$(M 'usbmuxd を入れ直すと、規則も入ります：' 'Reinstalling usbmuxd also installs the rule:')"
            install_hint usbmuxd "usbmuxd"
            st=warn
            notes+=("$(M 'usbmuxd の規則が無い' 'no usbmuxd rule')")
        fi
    fi
    if android_wanted; then
        ANDRULES=()
        for f in "${RULES[@]}"; do
            case "$(basename -- "$f")" in *android*) ANDRULES+=("$f"); continue ;; esac
            grep -qi 'idVendor}=="18d1"\|adb' -- "$f" 2> /dev/null && ANDRULES+=("$f")
        done
        if [ ${#ANDRULES[@]} -eq 0 ]; then
            warn "$(M 'Android の udev の規則が見つかりません。adb devices で「no permissions」と出るときは、これが原因です。' 'No Android udev rule was found. If adb devices says "no permissions", this is why.')"
            install_hint adbudev "Android udev rules"
            st=warn
            notes+=("$(M 'Android の規則が無い' 'no Android rule')")
        else
            ok "$(M "Android の udev の規則があります：$(basename -- "${ANDRULES[0]}")" "An Android udev rule is here: $(basename -- "${ANDRULES[0]}")")"
            if grep -q 'uaccess' -- "${ANDRULES[@]}" 2> /dev/null; then
                info "$(M '規則は uaccess で、ログインしているユーザーに権限を渡します。グループは要りません。' 'The rule uses uaccess, which gives access to the logged-in user. No group is needed.')"
            else
                for g in $(grep -ho 'GROUP="[^"]*"' -- "${ANDRULES[@]}" 2> /dev/null | sed 's/GROUP="\(.*\)"/\1/' | sort -u); do
                    group_state "$g"
                    gs=$?
                    if [ $gs = 0 ]; then
                        ok "$(M "グループ $g に入っています。" "You are in the group $g.")"
                    else
                        warn "$(M "Android の規則は、グループ $g のユーザーにだけ権限を渡します。" "The Android rule gives access only to users in the group $g.")"
                        group_advice "$g" $gs
                        st=warn
                        notes+=("$(M "グループ $g" "group $g")")
                    fi
                done
            fi
        fi
    fi
    [ "$st" = ok ] && [ ${#notes[@]} -eq 0 ] && ok "$(M '権限の問題は見つかりませんでした。' 'No permission problem was found.')"
    add_result udev "$st" "$t" "$(join_by ', ' "${notes[@]}")"
fi

# ==== 6. 動いている PocketGPU の道具 ====
if want tools; then
    step 6 "$(M '動いている PocketGPU の道具' 'Running PocketGPU tools')"
    t=$(M '動いている道具' 'Running tools')
    acts=()
    left=()
    found=0
    mapfile -t PROCS < <(kit_procs)
    logonly "kit procs: ${#PROCS[@]}"
    role_of() { local l; for l in "${PROCS[@]}"; do [ "${l%% *}" = "$1" ] && { set -- $l; printf '%s' "$2"; return; }; done; }
    # 1) マイクとカメラの台本が二重に動いている。台本の中の子（サブシェル）は数えない
    for r in miccam andmiccam; do
        tops=()
        for l in "${PROCS[@]}"; do
            set -- $l
            [ "$2" = "$r" ] || continue
            [ "$(role_of "$4")" = "$r" ] && continue
            tops+=("$3 $1")
        done
        [ ${#tops[@]} -gt 1 ] || continue
        found=1
        mapfile -t tops < <(printf '%s\n' "${tops[@]}" | sort -n -r)
        name=start-mic-camera.sh
        [ "$r" = andmiccam ] && name=$AND_MICCAM
        ng "$(M "$name が ${#tops[@]} 個、二重に動いています。同じスマホを取り合って、カメラとマイクがつながらなくなります。" "$name is running ${#tops[@]} times. They fight over the same phone, and the camera and mic fail.")"
        newest=${tops[0]#* }
        info "$(M "直し方：一番新しい PID $newest を残して、古い物を止めます。止めると、その台本が作った仮想のマイクも片づきます。" "Fix: keep the newest, PID $newest, and stop the older ones. Stopping one also removes the virtual mic it made.")"
        for x in "${tops[@]:1}"; do
            pid=${x#* }
            if offer "$(M "古い $name（PID $pid）を止めますか？" "Stop the older $name (PID $pid)?")"; then
                if stop_pid "$pid"; then ok "$(M "止めました：PID $pid" "Stopped: PID $pid")"; acts+=("stopped $name"); else left+=("$name PID $pid"); fi
            else left+=("$name PID $pid"); fi
        done
    done
    # 2) 親の台本が無いのに残っているカメラとマイクの道具（台本を強く止めたあとなど）
    for l in "${PROCS[@]}"; do
        set -- $l
        case "$2" in webcam_rx | ipaudio) ;; *) continue ;; esac
        pr=$(role_of "$4")
        case "$pr" in miccam | andmiccam) continue ;; esac
        found=1
        ng "$(M "bin/$2（PID $1）が、start-mic-camera.sh の外で残っています。" "bin/$2 (PID $1) remains without start-mic-camera.sh.")"
        if offer "$(M "残っている bin/$2（PID $1）を止めますか？" "Stop the leftover bin/$2 (PID $1)?")"; then
            if stop_pid "$1"; then ok "$(M "止めました：PID $1" "Stopped: PID $1")"; acts+=("stopped leftover $2"); else left+=("$2 PID $1"); fi
        else left+=("$2 PID $1"); fi
    done
    # 3) Flatpak の Steam 用の受け渡し役（run-on-iphone-gpu.sh --broker）と、その合言葉のファイル
    HELPERS=()
    BROKERS=()
    for l in "${PROCS[@]}"; do
        set -- $l
        case "$2" in
        broker-helper) [ "$(role_of "$4")" = broker-helper ] || HELPERS+=("$3 $1") ;;
        probe-broker) BROKERS+=("$1:$4") ;;
        esac
    done
    if [ ${#HELPERS[@]} -gt 1 ]; then
        found=1
        mapfile -t HELPERS < <(printf '%s\n' "${HELPERS[@]}" | sort -n -r)
        ng "$(M "run-on-iphone-gpu.sh --broker が ${#HELPERS[@]} 個、二重に動いています。" "run-on-iphone-gpu.sh --broker is running ${#HELPERS[@]} times.")"
        info "$(M "直し方：一番新しい PID ${HELPERS[0]#* } を残して、古い物を止めます。" "Fix: keep the newest, PID ${HELPERS[0]#* }, and stop the older ones.")"
        for x in "${HELPERS[@]:1}"; do
            pid=${x#* }
            if offer "$(M "古い受け渡し役（PID $pid）を止めますか？" "Stop the older USB helper (PID $pid)?")"; then
                if stop_pid "$pid"; then acts+=("stopped duplicate broker"); else left+=("broker PID $pid"); fi
            else left+=("broker PID $pid"); fi
        done
    fi
    BC=
    if [ -f "$TOKEN_FILE" ] && probe_runs; then
        # 合言葉そのものは画面にもログにも出さない
        BC=$(env -u PGPU_BROKER PGPU_BROKER_TOKEN="$(head -c 32 "$TOKEN_FILE")" "$PROBE" broker-check 2> /dev/null | awk '{print $1, $2}')
        logonly "token file: $BC"
        case "$BC" in
        "BROKER OK") ok "$(M 'Flatpak の Steam 用の受け渡し役が動いていて、合言葉も合っています。' 'The USB helper for Flatpak Steam is running and its secret matches.')" ;;
        "BROKER NG")
            found=1
            ng "$(M '受け渡し役は動いていますが、tools/broker-token の合言葉と合いません。前の受け渡し役が残っています。' 'A USB helper is running, but the secret in tools/broker-token does not match. An old helper remains.')"
            if [ ${#BROKERS[@]} -eq 0 ]; then
                info "$(M 'このキットの受け渡し役のプロセスが見つかりません。別のフォルダのキットが動いているかもしれません。そのウィンドウを閉じてください。' 'No helper process of this kit was found. A kit in another folder may be running. Close that window.')"
                left+=("$(M '別のキットの受け渡し役' 'helper of another kit')")
            fi
            for x in "${BROKERS[@]}"; do
                pid=${x%%:*}
                pp=${x#*:}
                tgt=$pid
                [ "$(role_of "$pp")" = broker-helper ] && tgt=$pp
                info "$(M "直し方：古い受け渡し役 $(proc_label "$tgt") を止めます。そのあと、もう一度 ./run-on-iphone-gpu.sh --broker を実行してください。" "Fix: stop the old helper $(proc_label "$tgt"). Then run ./run-on-iphone-gpu.sh --broker again.")"
                if offer "$(M "古い受け渡し役（PID $tgt）を止めますか？" "Stop the old USB helper (PID $tgt)?")"; then
                    if stop_pid "$tgt"; then acts+=("stopped old broker"); [ "$tgt" != "$pid" ] && stop_pid "$pid" > /dev/null; else left+=("broker PID $tgt"); fi
                else left+=("broker PID $tgt"); fi
            done
            ;;
        esac
    fi
    # 合言葉のファイルが、もう動いていない受け渡し役の物（受け渡し役を強く止めたあとに残る）
    if [ -f "$TOKEN_FILE" ] && probe_runs; then
        BC2=$(env -u PGPU_BROKER PGPU_BROKER_TOKEN="$(head -c 32 "$TOKEN_FILE")" "$PROBE" broker-check 2> /dev/null | awk '{print $1, $2}')
        if [ "$BC2" = "BROKER NONE" ]; then
            found=1
            ng "$(M 'tools/broker-token が残っていますが、受け渡し役は動いていません。Flatpak の Steam のゲームが、古い合言葉で接続しようとして失敗します。' 'tools/broker-token remains, but no USB helper is running. Games in Flatpak Steam try the old secret and fail.')"
            info "$(M '直し方：古い合言葉のファイルを fix-backup/ に移します。消しません。Flatpak の Steam を使うときは、もう一度 ./run-on-iphone-gpu.sh --broker を実行してください。' 'Fix: move the old secret file into fix-backup/. It is not deleted. To use Flatpak Steam, run ./run-on-iphone-gpu.sh --broker again.')"
            if offer "$(M '古い合言葉のファイルを fix-backup/ に移しますか？' 'Move the old secret file into fix-backup/?')"; then
                if backup_dir && act_move "$TOKEN_FILE" "$BACKUP/tools/broker-token"; then acts+=("moved stale broker-token"); else left+=("broker-token"); fi
            else left+=("broker-token"); fi
        fi
    fi
    if [ ! -f "$TOKEN_FILE" ] && [ ${#BROKERS[@]} -gt 0 ] && [ -z "$BC" ]; then
        info "$(M '受け渡し役は動いていますが、tools/broker-token がありません。Flatpak の Steam から使えないときは、そのウィンドウを閉じて、もう一度 ./run-on-iphone-gpu.sh --broker を実行してください。' 'A USB helper is running, but tools/broker-token is missing. If Flatpak Steam cannot use it, close that window and run ./run-on-iphone-gpu.sh --broker again.')"
    fi
    # 確かめ直す
    if [ "$found" = 0 ]; then
        ok "$(M '二重に動いている物や、残っている物はありません。' 'Nothing is running twice, and nothing is left over.')"
        add_result tools ok "$t"
    else
        remain=0
        mapfile -t PROCS < <(kit_procs)
        for r in miccam andmiccam broker-helper; do
            n=0
            for l in "${PROCS[@]}"; do set -- $l; [ "$2" = "$r" ] && [ "$(role_of "$4")" != "$r" ] && n=$((n + 1)); done
            [ $n -gt 1 ] && remain=1
        done
        for l in "${PROCS[@]}"; do
            set -- $l
            case "$2" in webcam_rx | ipaudio) case "$(role_of "$4")" in miccam | andmiccam) ;; *) remain=1 ;; esac ;; esac
        done
        if [ -f "$TOKEN_FILE" ] && probe_runs; then
            b=$(env -u PGPU_BROKER PGPU_BROKER_TOKEN="$(head -c 32 "$TOKEN_FILE")" "$PROBE" broker-check 2> /dev/null | awk '{print $1, $2}')
            [ "$b" != "BROKER OK" ] && remain=1
        fi
        if [ "$remain" = 0 ]; then
            ok "$(M '直りました。' 'Fixed.')"
            add_result tools fixed "$t" "$(join_by ', ' "${acts[@]}")"
        else
            add_result tools unfixed "$t" "$(join_by ', ' "${left[@]:-$(M '残っている' 'still there')}")"
        fi
    fi
fi

# ==== 7. iPhone の USB・ペアリング・アプリ ====
# ドライバ（libiphone_icd.so）を読み込んでいるプロセス。1 行に「PID」
driver_holders() {
    local d pid libs=() l
    for l in "$KIT/lib/x86_64/libiphone_icd.so" "$KIT/lib/i386/libiphone_icd.so"; do [ -e "$l" ] && libs+=("$(readlink -f -- "$l")"); done
    [ ${#libs[@]} -gt 0 ] || return 0
    for d in /proc/[0-9]*; do
        pid=${d#/proc/}
        case "$SELF_PIDS" in *" $pid "*) continue ;; esac
        [ -r "$d/maps" ] || continue
        for l in "${libs[@]}"; do
            if grep -qF -- " $l" "$d/maps" 2> /dev/null; then echo "$pid"; break; fi
        done
    done
    return 0
}
mask_udid() { sed -E 's/[0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}/(udid)/g; s/[0-9A-Fa-f]{40}/(udid)/g'; }
if want iphone; then
    step 7 "$(M 'iPhone の USB・ペアリング・アプリ' 'iPhone USB, pairing and app')"
    t=$(M 'iPhone' 'iPhone')
    if ! probe_runs; then
        info "$(M 'bin/pgpu-probe を実行できないので、チェックできません。手順 2 を見てください。' 'bin/pgpu-probe cannot run, so this cannot be checked. See step 2.')"
        add_result iphone skip "$t" "$(M 'pgpu-probe が動かない' 'pgpu-probe cannot run')"
    elif [ "$CTX_MUX_OK" = 0 ] && [ "$(probe mux | cut -c1-6)" != "MUX OK" ]; then
        USBN=$(probe usb | awk '$1 == "USB" {print $2; exit}')
        if [ "${USBN:-0}" = 0 ]; then
            info "$(M 'iPhone が USB でつながっていません。iPhone を使わないなら問題ありません。' 'No iPhone is plugged in. That is fine if you do not use an iPhone.')"
            add_result iphone skip "$t" "$(M 'iPhone が USB に無い' 'no iPhone on USB')"
        else
            warn "$(M 'usbmuxd が動いていないので、チェックできません。手順 4 を先に直してください。' 'usbmuxd is not running, so this cannot be checked. Fix step 4 first.')"
            add_result iphone skip "$t" "$(M 'usbmuxd が動いていない' 'usbmuxd not running')"
        fi
    else
        DEVS=$(probe devices)
        USBDEV=$(printf '%s\n' "$DEVS" | awk '$1 == "DEV" && $3 == "USB" {print; exit}')
        if [ -z "$USBDEV" ]; then
            ng "$(M 'iPhone が USB で認識されていません。' 'The iPhone is not seen over USB.')"
            printf '%s\n' "$DEVS" | grep -q ' Network ' && info "$(M 'Wi-Fi 経由でだけ認識されています。USB ケーブルでつないでください。' 'It is only seen over Wi-Fi. Connect it with a USB cable.')"
            if [ "$(probe usb | awk '$1 == "USB" {print $2; exit}')" != 0 ]; then
                info "$(M 'Linux は iPhone を認識していますが、usbmuxd はまだ認識していません。iPhone のロックを解除して、「このコンピュータを信頼しますか？」と出たら「信頼」をタップしてください。' 'Linux sees the iPhone, but usbmuxd does not yet. Unlock the iPhone and tap Trust if it asks "Trust This Computer?".')"
            else
                info "$(M 'iPhone を USB ケーブルでつないで、ロックを解除してください。充電専用のケーブルではつながりません。' 'Plug the iPhone in with USB and unlock it. A charge-only cable does not work.')"
            fi
            add_result iphone unfixed "$t" "$(M 'USB で認識されていない（案内を出した）' 'not seen over USB (instructions shown)')"
        else
            CTX_DEV=$(printf '%s' "$USBDEV" | awk '{print $2}')
            ok "$(M 'iPhone が USB で認識されています。' 'The iPhone is connected over USB.')"
            [ "$(printf '%s\n' "$DEVS" | awk '$1 == "DEV" && $3 == "USB"' | wc -l)" -gt 1 ] && warn "$(M 'Apple 製のデバイスが 2 台以上つながっています。最初の 1 台を使います。' 'More than one Apple device is connected. The first one is used.')"
            st=
            note=
            acts=()
            # ペアリング
            PAIR=$(probe pair "$CTX_DEV")
            logonly "$PAIR"
            if [ "$PAIR" = "PAIR OK" ]; then
                ok "$(M 'この PC と iPhone はペアリングされています。' 'This PC and the iPhone are paired.')"
            else
                ng "$(M 'この PC と iPhone はまだペアリングされていません。' 'This PC and the iPhone are not paired yet.')"
                info "$(M '直し方：idevicepair pair を実行します。iPhone のロックを解除しておき、「このコンピュータを信頼しますか？」と出たら「信頼」をタップして、パスコードを入力してください。' 'Fix: run idevicepair pair. Keep the iPhone unlocked. When it asks "Trust This Computer?", tap Trust and enter the passcode.')"
                if ! command -v idevicepair > /dev/null 2>&1; then
                    install_hint idevice "libimobiledevice tools"
                elif offer "$(M 'いま idevicepair pair を実行しますか？' 'Run idevicepair pair now?')"; then
                    while IFS= read -r l; do [ -n "$l" ] && info "$l"; done < <(act_idevicepair | mask_udid | tail -n 5)
                    for i in 1 2 3 4 5; do
                        [ "$(probe pair "$CTX_DEV")" = "PAIR OK" ] && break
                        sleep 1
                    done
                fi
                if [ "$(probe pair "$CTX_DEV")" = "PAIR OK" ]; then
                    ok "$(M 'ペアリングできました。' 'Paired.')"
                    acts+=("paired")
                else
                    st=unfixed
                    note=$(M 'ペアリングされていない' 'not paired')
                fi
            fi
            # アプリが答えるか
            if [ -z "$st" ]; then
                H=$(probe hello "$CTX_DEV" "$PROTO" 3000)
                logonly "$H"
                case "$H" in
                "HELLO OK"*)
                    ok "$(M 'iPhone のアプリが応答しました：' 'The iPhone app answered: ')${H#*name=}"
                    note=${H#*name=}
                    ;;
                "HELLO NG timeout"*)
                    ng "$(M 'iPhone のアプリは起動していますが、返事がありません。ほかのプログラムが iPhone の GPU を使っているか、前の接続が残っています。受け手は一度に 1 つのプログラムの相手しかしません。' 'The iPhone app is open but does not answer. Another program is using the iPhone GPU (the receiver serves one program at a time), or an old connection remains.')"
                    mapfile -t HOLD < <(driver_holders)
                    if [ ${#HOLD[@]} -eq 0 ]; then
                        info "$(M 'iPhone の GPU を使っている、この PC のプログラムは見つかりませんでした。iPhone のアプリを上にスワイプして一度終了し、開き直してください。' 'No program on this PC is using the iPhone GPU. Close the iPhone app (swipe it away) and open it again.')"
                        info "$(M 'Flatpak の Steam のゲームは、ここからは見えません。Steam でゲームを終了してください。' 'Games in Flatpak Steam cannot be seen from here. Quit the game in Steam.')"
                        st=unfixed
                        note=$(M '受け手が応答しない（PC に握っているプログラムは無い。アプリを開き直す案内を出した）' 'receiver does not answer (no program on the PC holds it; told to reopen the app)')
                    else
                        info "$(M 'iPhone のドライバ libiphone_icd.so を読み込んでいるプログラム：' 'Programs that loaded the iPhone driver (libiphone_icd.so):')"
                        for p in "${HOLD[@]}"; do info "$(proc_label "$p")"; done
                        info "$(M '直し方：使っていない物を止めると、iPhone の GPU が空きます。ゲームの途中なら、止めずに自分で閉じてください。' 'Fix: stopping the ones you no longer use frees the iPhone GPU. If you are in a game, close it yourself instead.')"
                        stopped=()
                        for p in "${HOLD[@]}"; do
                            if offer "$(M "$(proc_label "$p") を止めますか？ 保存していない内容は失われます。" "Stop $(proc_label "$p")? Unsaved work is lost.")"; then
                                stop_pid "$p" && { ok "$(M "止めました：PID $p" "Stopped: PID $p")"; stopped+=("$(proc_comm "$p") PID $p"); }
                            fi
                        done
                        H2=
                        if [ ${#stopped[@]} -gt 0 ]; then
                            for i in 1 2 3 4 5 6 7 8; do
                                H2=$(probe hello "$CTX_DEV" "$PROTO" 2000)
                                [ "${H2#HELLO OK}" != "$H2" ] && break
                                sleep 1
                            done
                        fi
                        if [ "${H2#HELLO OK}" != "$H2" ] && [ -n "$H2" ]; then
                            ok "$(M 'iPhone のアプリが応答しました：' 'The iPhone app answered: ')${H2#*name=}"
                            acts+=("stopped the holder")
                        else
                            st=unfixed
                            if [ ${#stopped[@]} -gt 0 ]; then note=$(M '止めても応答しない。iPhone のアプリを開き直す' 'still no answer after stopping; reopen the iPhone app')
                            else note=$(M '握っているプログラムを止めなかった' 'the holder was not stopped'); fi
                        fi
                    fi
                    ;;
                "HELLO NG result -9"*)
                    ng "$(M 'iPhone のアプリと、このキットの版が合いません。' 'The iPhone app and this kit are on different versions.')"
                    info "$(M "このキットの RpcServer-${APPVER:-*}.ipa を入れ直してください。このツールは iPhone にアプリを入れません。" "Install RpcServer-${APPVER:-*}.ipa from this kit again. This tool does not install apps on the iPhone.")"
                    st=unfixed
                    note=$(M "アプリとキットの版が違う（キット ${APPVER:-?}）" "app and kit versions differ (kit ${APPVER:-?})")
                    ;;
                "HELLO NG result"*)
                    ng "$(M 'iPhone のアプリが拒否しました。コード ' 'The iPhone app refused. Code ')${H#HELLO NG result }"
                    st=unfixed
                    note="refused ${H#HELLO NG result }"
                    ;;
                *)
                    ng "$(M 'iPhone のアプリが応答しません。' 'The iPhone app does not answer.')"
                    info "$(M 'iPhone で PocketGPU のアプリを開いて、前面に表示したまま、画面をつけておいてください。「開始」ボタンがあれば押してください。' 'Open the PocketGPU app on the iPhone and keep it in front with the screen on. Tap Start if the app shows it.')"
                    info "$(M '無料の Apple ID で入れたアプリは 7 日で開けなくなります。そのときは入れ直してください。' 'With a free Apple ID the app stops opening after 7 days. Install it again then.')"
                    st=unfixed
                    note=$(M 'アプリが応答しない（開く・入れ直す案内を出した）' 'app does not answer (instructions shown)')
                    ;;
                esac
            fi
            if [ -z "$st" ]; then
                if [ ${#acts[@]} -gt 0 ]; then add_result iphone fixed "$t" "$(join_by ', ' "${acts[@]}")"; else add_result iphone ok "$t" "$note"; fi
            else
                add_result iphone unfixed "$t" "$note"
            fi
        fi
    fi
fi

# ==== 8. Android ====
find_adb() {
    local c
    if [ -n "$ADB" ]; then [ -x "$ADB" ] && return 0; ADB_BAD=1; return 1; fi
    ADB=$(command -v adb 2> /dev/null || true)
    [ -n "$ADB" ] && return 0
    for c in "$KIT/platform-tools/adb" "$KIT/../platform-tools/adb" "${HOME:-/nonexistent}/platform-tools/adb" "${HOME:-/nonexistent}/Android/Sdk/platform-tools/adb" \
        "${HOME:-/nonexistent}/Downloads/platform-tools/adb" "${ANDROID_HOME:-/nonexistent}/platform-tools/adb" "${ANDROID_SDK_ROOT:-/nonexistent}/platform-tools/adb" \
        /opt/android-sdk/platform-tools/adb /opt/platform-tools/adb /usr/lib/android-sdk/platform-tools/adb; do
        [ -x "$c" ] && { ADB=$c; return 0; }
    done
    ADB=
    return 1
}
ADB_BAD=0
adbq() { "$ADB" "$@" < /dev/null 2> /dev/null | tr -d '\r'; }
# 端末の一覧を読む：A_READY・A_UNAUTH・A_OFFLINE・A_NOPERM
read_devices() {
    local line serial rest
    A_READY=() A_UNAUTH=() A_OFFLINE=() A_NOPERM=()
    while IFS= read -r line; do
        case "$line" in "" | "List of devices"* | "* "*) continue ;; esac
        serial=${line%%[[:space:]]*}
        rest=${line#"$serial"}
        rest=${rest#"${rest%%[![:space:]]*}"}
        MASKS+=("$serial")
        case "$rest" in
        device | device[[:space:]]*) A_READY+=("$serial") ;;
        unauthorized*) A_UNAUTH+=("$serial") ;;
        offline*) A_OFFLINE+=("$serial") ;;
        "no permissions"*) A_NOPERM+=("$serial") ;;
        esac
    done < <(adbq devices)
}
# adb forward の一覧：FW[PC のポート]=向け先
declare -A FW
read_forwards() {
    local s l d
    FW=()
    while read -r s l d; do
        case "$l" in tcp:*) FW[${l#tcp:}]=$d ;; esac
    done < <(adbq forward --list)
}
# 今の状態の問題を数える。$1 = 1 なら画面に出さない。PROBLEMS に名前：
#   GPU の口   norecv dup oldrecv portconflict fwdmissing fwdwrong fwdstale
#   アプリの口 appleft appfwdwrong appfwdhalf appfwdstale appportconflict
android_problems() {
    local quiet=$1 sa=(-s "$SERIAL") n pids hash want t owners foreign name port new old
    say() { [ "$quiet" = 1 ] || "$@"; }
    PROBLEMS=()
    CONFLICT=()
    APPFIX=()
    APPLEFT=()
    APPCONFLICT=()
    read_forwards
    pids=$(adbq "${sa[@]}" shell pidof "$AND_DEV_NAME")
    n=$(printf '%s' "$pids" | wc -w)
    hash=$(adbq "${sa[@]}" shell sha256sum "$AND_DEV_DIR/$AND_DEV_NAME" | awk '{print $1}')
    APP_INSTALLED=0
    APP_VER=
    adbq "${sa[@]}" shell pm path "$AND_PKG" | grep -q '^package:' && APP_INSTALLED=1
    [ "$APP_INSTALLED" = 1 ] && APP_VER=$(adbq "${sa[@]}" shell dumpsys package "$AND_PKG" | sed -n 's/^[[:space:]]*versionName=//p' | head -n 1 | tr -d ' ')
    APP_FORM=unknown
    if [ -n "$APP_VER" ]; then if ver_ge "$APP_VER" "$AND_APP_MIN_NEW"; then APP_FORM=new; else APP_FORM=old; fi; fi
    APPRECV=0
    if [ "$n" = 0 ] && [ "$APP_INSTALLED" = 1 ]; then
        if [ "${FW[$PGPU_PORT]:-}" = "tcp:$AND_APP_GPU_PORT" ]; then APPRECV=1
        elif [ "${FW[$PGPU_PORT]:-}" = "$AND_APP_GPU_USB" ] && { [ -z "$APP_VER" ] || ver_ge "$APP_VER" "$AND_APP_MIN_GPU_USB"; }; then APPRECV=1; fi
    fi
    if [ "$n" = 0 ]; then
        if [ "$APPRECV" = 1 ]; then
            if [ "${FW[$PGPU_PORT]}" = "tcp:$AND_APP_GPU_PORT" ]; then say ok "$(M "スマホでは、PocketGPU アプリの中の受け手（スマホのポート $AND_APP_GPU_PORT）を使っています。" "The phone uses the receiver inside the PocketGPU app (port $AND_APP_GPU_PORT on the phone).")"
            else say ok "$(M "スマホでは、PocketGPU アプリの中の受け手（USB 用の口 $AND_APP_GPU_USB）を使っています。" "The phone uses the receiver inside the PocketGPU app (USB socket $AND_APP_GPU_USB).")"; fi
        else PROBLEMS+=(norecv); say ng "$(M "スマホで受け手（$AND_DEV_NAME）が動いていません。" "The receiver ($AND_DEV_NAME) is not running on the phone.")"; fi
    elif [ "$n" -gt 1 ]; then
        PROBLEMS+=(dup)
        say ng "$(M "スマホで受け手（$AND_DEV_NAME）が $n 個、二重に動いています。" "The receiver ($AND_DEV_NAME) runs $n times on the phone.")"
    else
        say ok "$(M 'スマホで受け手が 1 個、動いています。' 'One receiver runs on the phone.')"
    fi
    if [ "$n" -gt 0 ] && [ -n "$hash" ] && [ -n "$KIT_SRV_HASH" ] && [ "$hash" != "$KIT_SRV_HASH" ]; then
        PROBLEMS+=(oldrecv)
        say ng "$(M "スマホで動いている受け手が、このキットの受け手（$AND_SERVER）と違います。古い受け手が残っています。" "The receiver on the phone differs from the one in this kit ($AND_SERVER). An old receiver remains.")"
    fi
    owners=$(port_owners "$PGPU_PORT")
    foreign=$(printf '%s\n' "$owners" | awk 'NF == 2 && $1 !~ /^adb/')
    if [ -n "$foreign" ]; then
        PROBLEMS+=(portconflict)
        while read -r name port; do CONFLICT+=("$port"); done <<< "$foreign"
        say ng "$(M "この PC のポート $PGPU_PORT を、adb ではないプログラムが使っています：" "Port $PGPU_PORT on this PC is used by a program that is not adb:")"
        while read -r name port; do say info "$name (PID $port)"; done <<< "$foreign"
    elif [ -z "${FW[$PGPU_PORT]:-}" ]; then
        PROBLEMS+=(fwdmissing)
        say ng "$(M "ポート $PGPU_PORT の adb forward がありません。" "There is no adb forward for port $PGPU_PORT.")"
    elif [ "${FW[$PGPU_PORT]}" != "tcp:$PGPU_PORT" ] && [ "$APPRECV" = 0 ]; then
        PROBLEMS+=(fwdwrong)
        say ng "$(M "ポート $PGPU_PORT の adb forward が、別の所（${FW[$PGPU_PORT]}）を指しています。" "The adb forward for port $PGPU_PORT points somewhere else (${FW[$PGPU_PORT]}).")"
    elif ! port_listening "$PGPU_PORT"; then
        PROBLEMS+=(fwdstale)
        say ng "$(M "adb forward は残っていますが、この PC のポート $PGPU_PORT が開いていません（adb を起動し直したあとなど）。" "The adb forward is listed, but port $PGPU_PORT on this PC is not open (for example after adb restarted).")"
    else
        say ok "$(M "この PC のポート $PGPU_PORT は、adb forward でスマホにつながっています。" "Port $PGPU_PORT on this PC is connected to the phone by adb forward.")"
    fi
    # アプリのカメラとマイクのポート
    local specs=("webcam $CAM_PORT $AND_CAM_NEW $AND_CAM_OLD" "audio $AUD_PORT $AND_AUD_NEW $AND_AUD_OLD") sp present=0
    for sp in "${specs[@]}"; do set -- $sp; [ -n "${FW[$2]:-}" ] && present=1; done
    if [ "$APP_INSTALLED" = 0 ]; then
        for sp in "${specs[@]}"; do
            set -- $sp
            t=${FW[$2]:-}
            { [ "$t" = "$3" ] || [ "$t" = "$4" ]; } && APPLEFT+=("$2")
        done
        if [ ${#APPLEFT[@]} -gt 0 ]; then
            PROBLEMS+=(appleft)
            say ng "$(M "PocketGPU アプリが入っていないのに、ポート ${APPLEFT[*]} の adb forward が残っています。" "The PocketGPU app is not on the phone, but the adb forward for port(s) ${APPLEFT[*]} remains.")"
        fi
    elif [ "$present" = 0 ]; then
        say info "$(M "PocketGPU アプリ（${APP_VER:-版は不明}）：カメラとマイクのポートは、まだ開いていません。使うときは ./$AND_MICCAM を実行してください。" "PocketGPU app (${APP_VER:-version unknown}): the camera and mic ports are not open yet. To use them, run ./$AND_MICCAM.")"
    else
        for sp in "${specs[@]}"; do
            set -- $sp
            name=$1 port=$2 new=$3 old=$4
            t=${FW[$port]:-}
            want=$new
            [ "$APP_FORM" = old ] && want=$old
            owners=$(port_owners "$port")
            foreign=$(printf '%s\n' "$owners" | awk 'NF == 2 && $1 !~ /^adb/')
            if [ -n "$foreign" ]; then
                PROBLEMS+=(appportconflict)
                APPFIX+=("$sp")
                while read -r a b; do APPCONFLICT+=("$b:$port"); done <<< "$foreign"
                say ng "$(M "この PC のポート $port を、adb ではないプログラムが使っています：" "Port $port on this PC is used by a program that is not adb:")"
                while read -r a b; do say info "$a (PID $b)"; done <<< "$foreign"
            elif [ -z "$t" ]; then
                PROBLEMS+=(appfwdhalf)
                APPFIX+=("$sp")
                say ng "$(M "$name のポート $port の adb forward がありません。カメラとマイクの組のもう片方はつながっています。" "There is no adb forward for the $name port $port, but the other one of the camera and mic pair is connected.")"
            elif [ "$t" != "$new" ] && [ "$t" != "$old" ]; then
                PROBLEMS+=(appfwdwrong)
                APPFIX+=("$sp")
                say ng "$(M "$name のポート $port の adb forward が、別の所（$t）を指しています。" "The adb forward for the $name port $port points somewhere else ($t).")"
            elif [ "$APP_FORM" != unknown ] && [ "$t" != "$want" ]; then
                PROBLEMS+=(appfwdwrong)
                APPFIX+=("$sp")
                if [ "$APP_FORM" = new ]; then say ng "$(M "$name のポート $port が古い向け方（$t）です。アプリ $APP_VER は、設定で「古い PC のツールも受け付ける」をオンにしたときしか、古い向け方を受けません。" "The $name port $port uses the old form ($t). App $APP_VER takes the old form only when \"Also accept old PC tools\" is on in its settings.")"
                else say ng "$(M "$name のポート $port が新しい向け方（$t）です。アプリ $APP_VER は、この向け方を受けません。" "The $name port $port uses the new form ($t). App $APP_VER does not take this form.")"; fi
            elif ! port_listening "$port"; then
                PROBLEMS+=(appfwdstale)
                APPFIX+=("$sp")
                say ng "$(M "adb forward は残っていますが、この PC のポート $port が開いていません。" "The adb forward is listed, but port $port on this PC is not open.")"
            else
                say ok "$(M "$name のポート $port：adb forward $t（アプリ ${APP_VER:-?}）" "The $name port $port: adb forward $t (app ${APP_VER:-?})")"
            fi
        done
    fi
}
has_problem() { local x; for x in "${PROBLEMS[@]}"; do for y in "$@"; do [ "$x" = "$y" ] && return 0; done; done; return 1; }
if want android; then
    step 8 "$(M 'Android の受け手とアプリのポート' 'Android receiver and app ports')"
    t=$(M 'Android' 'Android')
    if ! has_android_files; then
        info "$(M 'このキットには Android 用のファイルがありません。Android を使わないなら問題ありません。' 'This kit has no Android files. That is fine if you do not use Android.')"
        add_result android skip "$t" "$(M 'Android 用のファイルが無い' 'no Android files')"
    elif ! find_adb; then
        if [ "$ADB_BAD" = 1 ]; then
            warn "$(M "環境変数 ADB が設定されていますが、そのファイルは実行できません：$ADB" "ADB is set, but that file cannot be run: $ADB")"
        else
            info "$(M 'adb が見つかりません。Android を使わないなら問題ありません。使うときは、次のどちらかで入れてください。' 'adb was not found. That is fine if you do not use Android. If you do, get it in one of these ways.')"
            install_hint adb "adb"
            info "$(M 'パッケージを使わない場合は、Google の SDK Platform-Tools for Linux を展開して、platform-tools フォルダをキットのフォルダの中に置いてください。' 'Without a package, unzip SDK Platform-Tools for Linux from Google and put the platform-tools folder inside the kit folder.')"
        fi
        add_result android skip "$t" "$(M 'adb が無い（Android 未使用）' 'no adb (Android not used)')"
    else
        info "$(M "adb：$ADB" "adb: $ADB")"
        "$ADB" start-server > /dev/null 2>&1 < /dev/null
        read_devices
        read_forwards
        SERIAL=${ANDROID_SERIAL:-}
        [ -z "$SERIAL" ] && [ ${#A_READY[@]} -eq 1 ] && SERIAL=${A_READY[0]}
        LEFT=()
        for p in "$PGPU_PORT" "$CAM_PORT" "$AUD_PORT"; do [ -n "${FW[$p]:-}" ] && LEFT+=("$p"); done
        acts=()
        if [ ${#A_READY[@]} -eq 0 ]; then
            if [ ${#A_UNAUTH[@]} -gt 0 ]; then
                ng "$(M 'スマホはつながっていますが、この PC がまだ許可されていません。ロックを解除して、「USB デバッグを許可しますか？」で「許可」をタップしてください。' 'The phone is connected, but this PC is not allowed yet. Unlock it and tap Allow on "Allow USB debugging?".')"
                add_result android unfixed "$t" "$(M '許可されていない（案内を出した）' 'not allowed (instructions shown)')"
            elif [ ${#A_NOPERM[@]} -gt 0 ] || [ ${#A_OFFLINE[@]} -gt 0 ]; then
                if [ ${#A_NOPERM[@]} -gt 0 ]; then
                    ng "$(M 'このユーザーに、スマホを使う権限がありません。adb devices が「no permissions」と出ています。' 'This user may not use the phone (adb devices says "no permissions").')"
                    info "$(M 'udev の規則やグループを直したあとでも、前から動いている adb のサーバーは古い権限のままです。手順 5 も見てください。' 'Even after fixing the udev rules or groups, an adb server that was already running keeps the old permissions. See step 5 too.')"
                else
                    ng "$(M 'スマホが offline と表示されています。' 'The phone is shown as offline.')"
                fi
                info "$(M '直し方：adb kill-server と adb start-server で、adb のサーバーを起動し直します。今ある adb forward は外れます。' 'Fix: restart the adb server (adb kill-server and adb start-server). The current adb forwards are dropped.')"
                if offer "$(M 'adb のサーバーを起動し直しますか？' 'Restart the adb server?')"; then
                    act_adb kill-server > /dev/null
                    act_adb start-server > /dev/null
                    sleep 1
                fi
                read_devices
                if [ ${#A_NOPERM[@]} -eq 0 ] && [ ${#A_OFFLINE[@]} -eq 0 ] && [ ${#A_READY[@]} -gt 0 ]; then
                    ok "$(M "直りました。スマホが使えるようになりました。続けて ./$AND_START を実行してください。" "Fixed. The phone can be used now. Next, run ./$AND_START.")"
                    add_result android fixed "$t" "$(M 'adb のサーバーを起動し直した' 'restarted the adb server')"
                else
                    [ ${#A_NOPERM[@]} -gt 0 ] && install_hint adbudev "Android udev rules"
                    info "$(M 'ケーブルを抜いて接続し直してください。' 'Unplug the cable and plug it in again.')"
                    add_result android unfixed "$t" "$( [ ${#A_NOPERM[@]} -gt 0 ] && M 'no permissions のまま' 'still no permissions' || M 'offline のまま' 'still offline')"
                fi
            elif [ ${#LEFT[@]} -gt 0 ]; then
                ng "$(M "スマホはつながっていないのに、ポート ${LEFT[*]} の adb forward が残っています。" "No phone is connected, but the adb forward for port(s) ${LEFT[*]} remains.")"
                info "$(M '直し方：adb forward --remove で、残っている adb forward を外します。' 'Fix: remove the leftover adb forward (adb forward --remove).')"
                if offer "$(M '残っている adb forward を外しますか？' 'Remove the leftover adb forward?')"; then
                    for p in "${LEFT[@]}"; do act_adb forward --remove "tcp:$p" > /dev/null; done
                fi
                read_forwards
                still=()
                for p in "${LEFT[@]}"; do [ -n "${FW[$p]:-}" ] && still+=("$p"); done
                if [ ${#still[@]} -eq 0 ]; then
                    ok "$(M '外しました。' 'Removed.')"
                    add_result android fixed "$t" "$(M '残っていた adb forward を外した' 'removed leftover adb forwards')"
                else
                    add_result android unfixed "$t" "$(M "古い adb forward が残っている：${still[*]}" "leftover adb forward remains: ${still[*]}")"
                fi
            else
                info "$(M 'Android のスマホはつながっていません。Android を使わないなら問題ありません。' 'No Android phone is connected. That is fine if you do not use Android.')"
                add_result android skip "$t" "$(M 'スマホなし（Android 未使用）' 'no phone (Android not used)')"
            fi
        elif [ -z "$SERIAL" ]; then
            ng "$(M "スマホが 2 台以上つながっています。使う 1 台を ANDROID_SERIAL で決めてから、もう一度実行してください（adb devices でシリアルが分かります）。" "More than one phone is connected. Set ANDROID_SERIAL to the one you use and run this again (adb devices shows the serials).")"
            add_result android unfixed "$t" "$(M 'スマホが複数。ANDROID_SERIAL が要る' 'several phones; ANDROID_SERIAL is needed')"
        else
            MASKS+=("$SERIAL")
            KIT_SRV_HASH=
            [ -f "$KIT/$AND_SERVER" ] && KIT_SRV_HASH=$(sha256sum -- "$KIT/$AND_SERVER" | awk '{print $1}')
            android_problems 0
            P1=("${PROBLEMS[@]}")
            if [ ${#P1[@]} -eq 0 ]; then
                CTX_ANDROID_PORT_OK=1
                add_result android ok "$t" "$( [ "$APPRECV" = 1 ] && echo 'app receiver' || echo 'kit receiver')"
            else
                # 1) ポートを使っている、adb ではないプログラムをどける
                for p in "${CONFLICT[@]}" "${APPCONFLICT[@]%%:*}"; do
                    [ -n "$p" ] || continue
                    if offer "$(M "$(proc_label "$p") を止めますか？ そのポートを adb が使えるようになります。" "Stop $(proc_label "$p")? Then adb can use that port.")"; then
                        stop_pid "$p" && { ok "$(M "止めました：PID $p" "Stopped: PID $p")"; acts+=("stopped $(proc_comm "$p")"); }
                    fi
                done
                # 2) 受け手を入れ直す（無い・二重・古い）
                if has_problem norecv dup oldrecv; then
                    if [ -f "$KIT/$AND_START" ]; then
                        info "$(M "直し方：./$AND_START と同じ手順です。スマホの古い受け手を止めて、キットの受け手をコピーして起動し、adb forward をつなぎ直します（スマホには何もインストールしません）。" "Fix: the same steps as ./$AND_START. It stops the old receiver on the phone, copies the kit receiver, starts it and connects adb forward again. Nothing is installed on the phone.")"
                        if offer "$(M '受け手を入れ直して、つなぎ直しますか？' 'Restart the receiver and reconnect?')"; then
                            R=$(act_run_android_start "$SERIAL" "$PGPU_PORT" --serial "$SERIAL")
                            rc=$?
                            while IFS= read -r l; do [ -n "$l" ] && info "$l"; done < <(printf '%s\n' "$R" | grep -v '^[[:space:]]*$' | tail -n 4)
                            if [ $rc -ne 0 ]; then ng "$(M "./$AND_START が失敗しました。上の表示を見てください。" "./$AND_START failed. See the lines above.")"; else acts+=("restarted the receiver"); fi
                        fi
                    else
                        info "$(M "キットに ./$AND_START がありません（手順 2）。" "The kit has no ./$AND_START. See step 2.")"
                    fi
                elif has_problem fwdmissing fwdwrong fwdstale portconflict; then
                    # 3) adb forward だけやり直す
                    dest="tcp:$PGPU_PORT"
                    if [ "$APPRECV" = 1 ]; then
                        if ver_ge "$APP_VER" "$AND_APP_MIN_GPU_USB"; then dest=$AND_APP_GPU_USB; else dest="tcp:$AND_APP_GPU_PORT"; fi
                    fi
                    info "$(M "直し方：ポート $PGPU_PORT の adb forward を外して、つなぎ直します（$dest）。つながっていた接続は一度切れます。" "Fix: remove the adb forward for port $PGPU_PORT and connect it again ($dest). Existing connections are dropped once.")"
                    if offer "$(M 'adb forward をやり直しますか？' 'Redo the adb forward?')"; then
                        act_adb -s "$SERIAL" forward --remove "tcp:$PGPU_PORT" > /dev/null
                        r=$(act_adb -s "$SERIAL" forward "tcp:$PGPU_PORT" "$dest") || ng "$(M "adb forward に失敗しました：$r" "adb forward failed: $r")"
                        acts+=("redid adb forward")
                    fi
                fi
                # 4) アプリが無いのに残っているカメラとマイクのポート
                if has_problem appleft; then
                    info "$(M '直し方：adb forward --remove で、残っている adb forward を外します。' 'Fix: remove the leftover adb forward (adb forward --remove).')"
                    if offer "$(M 'PocketGPU アプリのポートの、残っている adb forward を外しますか？' 'Remove the leftover adb forward for the PocketGPU app ports?')"; then
                        for p in "${APPLEFT[@]}"; do act_adb -s "$SERIAL" forward --remove "tcp:$p" > /dev/null; done
                        acts+=("removed leftover app forwards")
                    fi
                fi
                # 5) カメラとマイクのポートを、アプリの版に合う向け方でつなぎ直す
                if has_problem appfwdwrong appfwdhalf appfwdstale appportconflict; then
                    if [ "$APP_FORM" = old ]; then fw=$(M 'アプリ 0.1.1 以前の古い向け方 tcp' 'the old form (app 0.1.1 and older, tcp)'); else fw=$(M 'アプリ 0.1.2 以上の新しい向け方 localabstract' 'the new form (app 0.1.2 and newer, localabstract)'); fi
                    info "$(M "直し方：カメラとマイクの adb forward を、$fw でつなぎ直します。アプリは操作しません。" "Fix: connect the camera and mic adb forward again in $fw. The app itself is not touched.")"
                    if offer "$(M 'カメラとマイクの adb forward をやり直しますか？' 'Redo the camera and mic adb forward?')"; then
                        for sp in "${APPFIX[@]}"; do
                            set -- $sp
                            dest=$3
                            [ "$APP_FORM" = old ] && dest=$4
                            act_adb -s "$SERIAL" forward --remove "tcp:$2" > /dev/null
                            r=$(act_adb -s "$SERIAL" forward "tcp:$2" "$dest") || ng "$(M "adb forward に失敗しました：$r" "adb forward failed: $r")"
                            acts+=("redid $1 forward $dest")
                        done
                    fi
                fi
                # 確かめ直す
                android_problems 1
                if [ ${#PROBLEMS[@]} -eq 0 ]; then
                    ok "$(M '直りました。' 'Fixed.')"
                    CTX_ANDROID_PORT_OK=1
                    add_result android fixed "$t" "$(join_by ', ' "${P1[@]}") ($(join_by ', ' "${acts[@]}"))"
                else
                    ng "$(M "まだ直っていません：$(join_by ', ' "${PROBLEMS[@]}")" "Not fixed yet: $(join_by ', ' "${PROBLEMS[@]}")")"
                    info "$(M "スマホのロックを解除して画面をつけたまま、./$AND_STOP のあとに ./$AND_START を実行してみてください。カメラとマイクを使っているなら、./$AND_MICCAM を止めてから、もう一度実行してください。" "With the phone unlocked and the screen on, run ./$AND_STOP and then ./$AND_START. If you use the camera and mic, stop ./$AND_MICCAM and run it again.")"
                    add_result android unfixed "$t" "$(M '直らなかった：' 'not fixed: ')$(join_by ', ' "${PROBLEMS[@]}")"
                fi
            fi
        fi
    fi
fi

# ==== 9. カメラとマイク ====
SYSR=${PGPU_SYS:-/sys}
DEVR=${PGPU_DEV:-/dev}
miccam_running() {
    local l
    while IFS= read -r l; do set -- $l; case "$2" in miccam | andmiccam) return 0 ;; esac; done < <(kit_procs)
    return 1
}
if want media; then
    step 9 "$(M 'カメラとマイク' 'Camera and microphone')"
    t=$(M 'カメラとマイク' 'Camera and mic')
    if ! has_media_files; then
        info "$(M 'このキットには、カメラとマイクの道具がありません。' 'This kit has no camera and mic tools.')"
        add_result media skip "$t" "$(M 'カメラとマイクの道具が無い' 'no camera and mic tools')"
    else
        st=ok
        notes=()
        acts=()
        # 音：PipeWire か PulseAudio
        SERVER=
        command -v pactl > /dev/null 2>&1 && SERVER=$(LC_ALL=C timeout 10 pactl info 2> /dev/null | sed -n 's/^Server Name: //p')
        PWSOCK=${XDG_RUNTIME_DIR:-/run/user/$MYUID}/pipewire-0
        if [ "${SERVER#*PipeWire}" != "$SERVER" ]; then
            ok "$(M "音：PipeWire（$SERVER）" "Sound: PipeWire ($SERVER)")"
        elif [ -n "$SERVER" ]; then
            ok "$(M "音：PulseAudio（$SERVER）。仮想のマイクは pactl で作ります。" "Sound: PulseAudio ($SERVER). The virtual mic is made with pactl.")"
        elif [ -S "$PWSOCK" ]; then
            ok "$(M '音：PipeWire。pactl は無いが、PipeWire は動いている' 'Sound: PipeWire (no pactl, but PipeWire is running)')"
        else
            ng "$(M 'PipeWire も PulseAudio も見つかりません。マイクとスピーカーは使えません。' 'Neither PipeWire nor PulseAudio was found. The mic and speaker cannot be used.')"
            install_hint pipewire "PipeWire"
            st=unfixed
            notes+=("$(M '音の仕組みが無い' 'no sound server')")
        fi
        # 前の start-mic-camera.sh が残した仮想のマイク・スピーカー（台本を強く止めたあとに残る）
        if [ -n "$SERVER" ]; then
            mapfile -t MODS < <(LC_ALL=C pactl list short modules 2> /dev/null | awk -F'\t' '$3 ~ /pocketgpu_/ {print $1}')
            if [ ${#MODS[@]} -gt 0 ]; then
                if miccam_running; then
                    info "$(M "PocketGPU の仮想のマイク・スピーカーが ${#MODS[@]} 個あります（start-mic-camera.sh が使っています）。" "There are ${#MODS[@]} PocketGPU virtual mic/speaker modules (start-mic-camera.sh is using them).")"
                else
                    ng "$(M "start-mic-camera.sh が動いていないのに、PocketGPU の仮想のマイク・スピーカーが ${#MODS[@]} 個残っています。通話アプリで選ぶと、音が出ません。" "start-mic-camera.sh is not running, but ${#MODS[@]} PocketGPU virtual mic/speaker modules remain. Picking them in a call app gives no sound.")"
                    info "$(M '直し方：残っている PocketGPU のモジュールだけを pactl unload-module で外します。ほかのモジュールには触りません。' 'Fix: unload only the leftover PocketGPU modules (pactl unload-module). Other modules are not touched.')"
                    if offer "$(M "残っている仮想のマイク・スピーカー（モジュール ${MODS[*]}）を外しますか？" "Unload the leftover virtual mic/speaker (modules ${MODS[*]})?")"; then
                        for m in "${MODS[@]}"; do act_pactl_unload "$m" > /dev/null; done
                    fi
                    mapfile -t MODS2 < <(LC_ALL=C pactl list short modules 2> /dev/null | awk -F'\t' '$3 ~ /pocketgpu_/ {print $1}')
                    if [ ${#MODS2[@]} -eq 0 ]; then
                        ok "$(M '外しました。' 'Unloaded.')"
                        acts+=("unloaded ${#MODS[@]} leftover modules")
                    else
                        st=unfixed
                        notes+=("$(M '残った仮想のマイク' 'leftover virtual mic')")
                    fi
                fi
            fi
        fi
        # カメラ：v4l2loopback
        if [ -d "$SYSR/module/v4l2loopback" ]; then
            VDEVS=()
            for d in "$SYSR"/devices/virtual/video4linux/video*; do [ -d "$d" ] && VDEVS+=("$(basename -- "$d")"); done
            ok "$(M "カメラ：v4l2loopback が読み込まれています（${VDEVS[*]:-デバイスなし}）。" "Camera: v4l2loopback is loaded (${VDEVS[*]:-no device}).")"
            ec=$(cat "$SYSR/module/v4l2loopback/parameters/exclusive_caps" 2> /dev/null || true)
            if [ -n "$ec" ] && [ "${ec:0:1}" != Y ] && [ "${ec:0:1}" != 1 ]; then
                warn "$(M 'v4l2loopback が exclusive_caps=1 なしで読み込まれています。Chrome・Zoom・Discord がカメラとして選べません。次のコマンドで読み込み直してください。このツールは sudo を実行しません：' 'v4l2loopback was loaded without exclusive_caps=1. Chrome, Zoom and Discord cannot pick it as a camera. Load it again with the commands below. This tool does not run sudo:')"
                cmdline "sudo modprobe -r v4l2loopback"
                cmdline "sudo modprobe v4l2loopback exclusive_caps=1 card_label=\"PocketGPU iPhone Camera\""
                [ "$st" = ok ] && st=warn
                notes+=("exclusive_caps")
            fi
            for v in "${VDEVS[@]}"; do
                f="$DEVR/$v"
                [ -e "$f" ] || continue
                if [ ! -r "$f" ] || [ ! -w "$f" ]; then
                    g=$(stat -c '%G' -- "$f" 2> /dev/null)
                    warn "$(M "$f を使う権限がありません（グループ ${g:-?}）。" "No permission to use $f (group ${g:-?}).")"
                    if [ -n "$g" ] && [ "$g" != root ]; then
                        group_state "$g"
                        group_advice "$g" $?
                    fi
                    [ "$st" = ok ] && st=warn
                    notes+=("$(M "$v の権限" "$v permission")")
                fi
            done
        elif [ "$FAMILY" = steamos ]; then
            info "$(M 'カメラ：SteamOS では v4l2loopback を使えません。OBS などで選べる PipeWire のカメラを使います。' 'Camera: SteamOS cannot use v4l2loopback. The PipeWire camera (OBS and others can pick it) is used.')"
        else
            if [ -n "$(modinfo -n v4l2loopback 2> /dev/null || /sbin/modinfo -n v4l2loopback 2> /dev/null)" ]; then
                warn "$(M 'カメラ：v4l2loopback は入っていますが、読み込まれていません。Zoom・Discord・ブラウザで選べるようにするには、次を実行してください。このツールは sudo を実行しません：' 'Camera: v4l2loopback is installed but not loaded. So that Zoom, Discord and browsers can pick the camera, run this. This tool does not run sudo:')"
                cmdline "sudo modprobe v4l2loopback exclusive_caps=1 card_label=\"PocketGPU iPhone Camera\""
                info "$(M 'PC を再起動すると外れます。そのときは、もう一度実行してください。' 'It is gone after a restart. Then run it again.')"
            else
                warn "$(M 'カメラ：v4l2loopback がありません。PipeWire のカメラだけになります。OBS など一部のアプリでしか選べません。' 'Camera: v4l2loopback is missing. Only the PipeWire camera is available, which only some apps such as OBS can pick.')"
                install_hint v4l2 "v4l2loopback"
            fi
            [ "$st" = ok ] && st=warn
            notes+=("$(M 'v4l2loopback が読み込まれていない' 'v4l2loopback not loaded')")
        fi
        # カメラの映像を読む FFmpeg（libavcodec）
        LDC=$(command -v ldconfig 2> /dev/null || echo /sbin/ldconfig)
        if "$LDC" -p 2> /dev/null | grep -q 'libavcodec\.so\.[0-9]'; then
            ok "$(M 'FFmpeg の libavcodec があります。' 'FFmpeg (libavcodec) is installed.')"
        elif [ -x "$LDC" ] || command -v ldconfig > /dev/null 2>&1; then
            ng "$(M 'カメラの映像を表示するのに要る FFmpeg の libavcodec がありません。' 'FFmpeg (libavcodec), needed to show the camera picture, is missing.')"
            install_hint ffmpeg "FFmpeg (libavcodec)"
            [ "$st" = ok ] && st=warn
            notes+=("$(M 'libavcodec が無い' 'no libavcodec')")
        fi
        if [ "$st" = ok ] && [ ${#acts[@]} -gt 0 ]; then st=fixed; fi
        add_result media "$st" "$t" "$(join_by ', ' "${acts[@]}" "${notes[@]}")"
    fi
fi

# ==== 10. ドライバ経由で GPU が見えるか ====
PC_VENDORS='vendor 0x(8086|10de|1002|1414|10005),'
vk_check() { # $1 = 起動の台本。GPU の行を VK_GPUS に、PC の GPU だけの行を VK_PC に、終わりのコードを VK_RC に
    local out
    out=$(
        eval "$(bash "$1" --env 2> /dev/null)"
        export VK_LOADER_LAYERS_DISABLE='~implicit~'
        timeout 30 "$PROBE" vklist 2>&1
    )
    VK_RC=$?
    VK_OUT=$out
    VK_GPUS=$(printf '%s\n' "$out" | grep -E '^GPU[0-9]+: ' | grep -v -E "$PC_VENDORS" || true)
    VK_PC=$(printf '%s\n' "$out" | grep -E '^GPU[0-9]+: ' | grep -E "$PC_VENDORS" || true)
}
if want gpu; then
    step 10 "$(M 'ドライバ経由で GPU が見えるか' 'Phone GPU through the driver')"
    t=$(M 'スマホの GPU' 'Phone GPU')
    if ! probe_runs || [ ! -f "$RUN" ]; then
        info "$(M 'bin/pgpu-probe か run-on-iphone-gpu.sh が使えないので、チェックできません。手順 2 を見てください。' 'bin/pgpu-probe or run-on-iphone-gpu.sh cannot be used, so this cannot be checked. See step 2.')"
        add_result gpu skip "$t" "$(M 'キットのファイル' 'kit files')"
    else
        targets=()
        is=$(result_of iphone)
        as=$(result_of android)
        { [ "$is" = ok ] || [ "$is" = fixed ]; } && targets+=("iphone")
        { [ "$as" = ok ] || [ "$as" = fixed ] || [ "$CTX_ANDROID_PORT_OK" = 1 ]; } && [ -f "$KIT/$AND_RUN" ] && targets+=("android")
        [ ${#targets[@]} -eq 0 ] && targets=("iphone")
        seen=()
        for tg in "${targets[@]}"; do
            if [ "$tg" = android ]; then vk_check "$KIT/$AND_RUN"; else vk_check "$RUN"; fi
            logonly "vklist $tg rc=$VK_RC"
            if [ -n "$VK_GPUS" ]; then
                while IFS= read -r g; do
                    g=${g#GPU*: }
                    g=${g%%  (vendor*}
                    ok "$(M 'ドライバ経由で見えます：' 'Seen through the driver: ')$g"
                    seen+=("$g")
                done <<< "$VK_GPUS"
            fi
        done
        if [ ${#seen[@]} -gt 0 ]; then
            add_result gpu ok "$t" "$(join_by '; ' "${seen[@]}")"
        else
            ng "$(M 'ドライバ経由でスマホの GPU が見えません。' 'The driver does not show the phone GPU.')"
            reasons=()
            if [ "$VK_RC" = 124 ]; then
                info "$(M '30 秒以内に終わりませんでした。スマホのアプリを閉じて、開き直してください。' 'It did not finish in 30 seconds. Close and reopen the app on the phone.')"
                reasons+=("$(M '30 秒以内に終わらない' 'did not finish in 30 seconds')")
            elif [ -n "$VK_PC" ]; then
                info "$(M 'PC の GPU だけが見えます：' 'Only the PC GPUs are seen:')"
                while IFS= read -r l; do info "${l#GPU*: }"; done <<< "$VK_PC"
            else
                while IFS= read -r l; do info "$l"; done < <(printf '%s\n' "$VK_OUT" | grep -v '^$' | tail -n 4)
            fi
            for i in "${!R_ID[@]}"; do
                case "${R_ID[$i]}" in files | loader | usbmuxd | iphone | android | udev | tools)
                    if [ "${R_STATE[$i]}" = unfixed ]; then
                        info "$(M "この項目が直っていません：${R_TITLE[$i]}。先にそれを直すと、見えるようになることがあります。" "This item is not fixed: ${R_TITLE[$i]}. Fixing it first may make the GPU visible.")"
                        reasons+=("$(M '前の項目が直っていない：' 'an earlier item is not fixed: ')${R_TITLE[$i]}")
                    fi
                    ;;
                esac
            done
            if [ ${#reasons[@]} -eq 0 ]; then
                info "$(M '原因が分かりませんでした。start-pocketgpu.sh の結果と pocketgpu-fix-log.txt を Discord で送ってください。' 'The cause is unknown. Send the result of start-pocketgpu.sh and pocketgpu-fix-log.txt to us on Discord.')"
                reasons+=("$(M '原因不明' 'cause unknown')")
            fi
            add_result gpu unfixed "$t" "$(join_by ', ' "${reasons[@]}")"
        fi
    fi
fi

# ==== まとめ ====
# 載せてはいけない物を伏せる：キットの場所・ホーム・ユーザー名・PC の名前・スマホのシリアル・iPhone の UDID
HOSTN=$(cat /proc/sys/kernel/hostname 2> /dev/null || hostname 2> /dev/null || true)
mask_line() {
    local s=$1 m
    s=${s//"$KIT"/<kit>}
    [ -n "${HOME:-}" ] && [ "$HOME" != / ] && s=${s//"$HOME"/\~}
    for m in "${MASKS[@]}"; do [ ${#m} -ge 4 ] && s=${s//"$m"/(serial)}; done
    [ ${#USERNAME_} -ge 2 ] && s=${s//"$USERNAME_"/(user)}
    [ ${#HOSTN} -ge 2 ] && s=${s//"$HOSTN"/(pc)}
    printf '%s\n' "$s" | mask_udid
}
name_of() {
    case "$1" in ok) M '問題なし' 'OK' ;; fixed) M '直った' 'fixed' ;; unfixed) M '直らなかった' 'not fixed' ;; warn) M '気をつけたい' 'note' ;; skip) M '対象外' 'skipped' ;; esac
}
c_ok=0 c_fixed=0 c_unfixed=0 c_warn=0 c_skip=0
for s in "${R_STATE[@]}"; do eval "c_$s=\$((c_$s + 1))"; done
SUM=()
SUM+=("PocketGPU Fix (Linux) $(date +%Y-%m-%d) (fix v$FIX_VERSION$([ "$CHECK_ONLY" = 1 ] && echo ', check only'))")
SUM+=("PC: ${OS_NAME:-Linux} / $FAMILY / kernel $(uname -r) / $(uname -m) / desktop $CTX_DESKTOP / Vulkan loader ${CTX_LOADER:--} / usbmuxd $([ -n "$(find_usbmuxd)" ] && echo yes || echo no) / adb $([ -n "$ADB" ] && echo yes || echo no)")
SUM+=("Kit: app ${APPVER:-?} / proto $PROTO / ${PLATFORM:-linux} / android $(has_android_files && echo yes || echo no)")
SUM+=("$(M "結果：直った $c_fixed / 直らなかった $c_unfixed / 気をつけたい $c_warn / 問題なし $c_ok / 対象外 $c_skip" "Result: fixed $c_fixed / not fixed $c_unfixed / note $c_warn / OK $c_ok / skipped $c_skip")")
okl=()
for i in "${!R_ID[@]}"; do
    case "${R_STATE[$i]}" in
    ok | skip) okl+=("${R_TITLE[$i]}") ;;
    *) SUM+=("[$(name_of "${R_STATE[$i]}")] ${R_TITLE[$i]}${R_NOTE[$i]:+ - ${R_NOTE[$i]}}") ;;
    esac
done
[ ${#okl[@]} -gt 0 ] && SUM+=("$(M '問題なし・対象外：' 'OK / skipped: ')$(join_by ', ' "${okl[@]}")")
SUMTEXT=$(for l in "${SUM[@]}"; do mask_line "$l"; done)

if [ "$NO_SUMMARY" = 0 ]; then
    out ""
    out "$(M '==== まとめ：Discord に貼れます ====' '==== Summary (you can paste this on Discord) ====')" "$C_HEAD"
    while IFS= read -r l; do out "$l"; done <<< "$SUMTEXT"
    out "$(M '=======================================' '==============================================')" "$C_HEAD"
    out "$(M 'この文には、合言葉・ユーザー名・PC の名前・フォルダの場所・シリアルは入っていません。' 'This text has no secret, user name, PC name, folder path or serial number.')"
    if printf '%s\n' "$SUMTEXT" > "$KIT/pocketgpu-fix-summary.txt" 2> /dev/null; then
        out "$(M 'ファイルにも書きました：' 'Also saved to: ')pocketgpu-fix-summary.txt"
    fi
    CLIP=
    command -v wl-copy > /dev/null 2>&1 && [ -n "${WAYLAND_DISPLAY:-}" ] && CLIP="wl-copy"
    [ -z "$CLIP" ] && command -v xclip > /dev/null 2>&1 && [ -n "${DISPLAY:-}" ] && CLIP="xclip -selection clipboard"
    [ -z "$CLIP" ] && command -v xsel > /dev/null 2>&1 && [ -n "${DISPLAY:-}" ] && CLIP="xsel --clipboard --input"
    if [ -n "$CLIP" ] && ask_yes "$(M 'この文をクリップボードにコピーしますか？' 'Copy this text to the clipboard?')"; then
        printf '%s' "$SUMTEXT" | $CLIP && ok "$(M 'コピーしました。' 'Copied.')"
    fi
fi
LOGPATH="$KIT/pocketgpu-fix-log.txt"
for l in "${LOG[@]}"; do mask_line "$l"; done > "$LOGPATH" 2> /dev/null
out ""
out "$(M 'ログ：' 'Log: ')pocketgpu-fix-log.txt"
[ "$c_unfixed" -gt 0 ] && exit 1
exit 0
