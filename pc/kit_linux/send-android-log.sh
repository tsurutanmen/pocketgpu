#!/usr/bin/env bash
# PocketGPU for Android（Linux）：Android の記録を 1 つの圧縮ファイルにまとめる。Windows のキットの Send-Android-Log.cmd（tools\android-log.ps1）と同じ物を集めて、同じ物を伏せる。
# スマホにもこの PC にも何も変えない（adb で読むだけ）。管理者の権限は要らない。何もインストールしない。
#   ./send-android-log.sh
#   ./send-android-log.sh --serial 番号    スマホが何台かあるとき、adb devices のシリアルで選ぶ
# 圧縮ファイルは zip コマンドがあれば zip、無ければ tar.gz。キットのフォルダ（書けなければホーム）に、フォルダと一緒に作る。
#
# 集める物（adb で、下の決まった命令だけを打つ。getprop の全部・dumpsys の全部・bugreport は使わない）
#   receiver-log.txt   adb shell cat /data/local/tmp/pgpu_server.log                     キットの受け手の記録
#   logcat-crash.txt   adb logcat -d -b crash                                            Android の落ちた記録。既定は PocketGPU の受け手の行だけ
#   logcat-app.txt     adb logcat -d -s iphone-rpc-vk                                    アプリの受け手の記録（アプリが入っているときだけ）
#   info.txt           機種・Android の版・チップ・GPU の名前・アプリの版・キットの版・Linux の版・PC 側の口・adb forward の一覧
#     機種など：adb shell getprop ro.product.model / ro.product.manufacturer / ro.build.version.release / ro.build.version.sdk / ro.product.cpu.abi / ro.soc.model / ro.board.platform
#     GPU：受け手の記録の「Receiver GPU:」の行と、adb shell dumpsys SurfaceFlinger の「GLES:」の行（この 1 行だけを残す）
#     アプリの版：adb shell dumpsys package com.tsurutanmen.pocketgpu の versionName（この 1 行だけを残す）
# 集めない物：電話番号・IMEI・Google アカウント・端末のシリアル番号・キットの印・ほかのアプリの記録（全部を送ると答えたときの落ちた記録を除く）
# 伏せる物：PC のユーザー名・PC 名・ホームのフォルダ名・adb のシリアル番号・メールアドレス。「(hidden)」「(serial)」「(email)」に直す（全部のファイルに最後にまとめて）。
#   Windows 版との違い：root と localhost は伏せない（何も隠れないので）。adb forward --list の 1 列目（どのスマホのシリアルでも）は必ず (serial) にする
#   tar.gz には、ファイルの持ち主の名前を入れない（--owner=0 --group=0 --numeric-owner）
#
# 落ちた記録に PocketGPU 以外のアプリの行があるときだけ、全部を送るか y/N で聞く（既定 N = PocketGPU の受け手の行だけ）
# 終わりのコード：0 圧縮ファイルを作った・1 adb が無い/スマホが無い/許可がまだ（案内を出して止まる）・2 圧縮ファイルを作れなかった
# 画面の文は下書き。言い回しは android/common.sh の M と、ここの M '日本語' 'English' の所で直す
#
# 試す人は使わない、試験用の差し替え（android/common.sh の物に加えて）：
#   PGPU_LOG_OUT=フォルダ          フォルダと圧縮ファイルをキットのフォルダの代わりにここへ作る
#   PGPU_LOG_ARCHIVE=zip|tar       zip か tar.gz かを決める（既定は、zip コマンドがあれば zip）
#   PGPU_TEST_HOSTNAME=名前        PC 名の代わり
#   PGPU_TEST_LAUNCH_LOG=パス      ファイルマネージャーを開く代わりに、開くはずだった物をこのファイルに書く
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"
set -u
export LC_NUMERIC=C
self=$(readlink -f -- "${BASH_SOURCE[0]}")
KIT=$(dirname -- "$self")
SELF=./send-android-log.sh
LOGFILE="$KIT/pocketgpu-android-sendlog-log.txt"
TOTAL=4
PORT_WAS_SET=${PGPU_PORT+yes}
MAXLINES=4000
# shellcheck source=android/common.sh
. "$KIT/android/common.sh"
usage() {
    say '使い方：./send-android-log.sh [--serial シリアル]' 'Usage: ./send-android-log.sh [--serial SERIAL]'
    say '  Android の記録（受け手の記録・落ちた記録・アプリの記録・機種などの情報）を 1 つの圧縮ファイルにまとめます。何も変更しません。' '  Collects the Android logs (receiver log, crash log, app log, phone information) into one archive. It changes nothing.'
    say '  電話番号・IMEI・アカウント・シリアル番号・PC のユーザー名・PC 名は含みません' '  The phone number, IMEI, accounts, serial number, PC user name and PC name are not included.'
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

# ---- 作業場所と共通の道具 ----
# shellcheck source=android/tools.sh
. "$KIT/android/tools.sh"
make_work sendlog || {
    echo "ERROR: could not make a temporary folder (TMPDIR=${TMPDIR:-/tmp})" >&2
    exit 2
}
trap cleanup_work EXIT
trap 'exit 130' INT TERM

# ---- 小さな道具 ----
# 長い記録は後ろの MAXLINES 行だけ残す。省いたときは先頭にそう書く
limit_lines() {
    awk -v max="$MAXLINES" '{ a[NR] = $0 } END { n = NR; while (n > 0 && a[n] ~ /^[ \t]*$/) n--; s = 1; if (n > max) { s = n - max + 1; print "(" (s - 1) " older lines left out)" } for (i = s; i <= n; i++) print a[i] }'
}
format_lines() {
    if [ "$ja" = 1 ]; then printf '%s 行' "$1"; elif [ "$1" = 1 ]; then printf '1 line'; else printf '%s lines' "$1"; fi
}
# write_log 名前 ：標準入力を伏せて、出す所のフォルダ $DIR に書く
write_log() { hide | awk '{ print }' > "$DIR/$1"; }

# ---- 落ちた記録を、PocketGPU の受け手の分だけに絞る ----
# logcat の行は、同じ（PID・タグ）が続く間を 1 つのまとまりにする。「*** *** ***」の行と「FATAL EXCEPTION」の行は、新しいまとまりの始まり。
# まとまりの中のどこかに pgpu_server・libpgpu_server.so・PocketGPU のアプリ名があれば、そのまとまりを残す
# mode：count（「全部 PocketGPU」の 2 つの数を出す）・mine（PocketGPU のまとまりだけ）・all（全部）
AWK_CRASH=$(
    cat << 'AWKEOF'
function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
function linekey(l,   head, rest, f, pid, i, tag, s, p, r) {
    if (match(l, /^[0-9][0-9]-[0-9][0-9][ \t]+[0-9][0-9]:[0-9][0-9]:[0-9][0-9]\.[0-9]+[ \t]+[0-9]+[ \t]+[0-9]+[ \t]+[VDIWEFS][ \t]+/)) {
        head = substr(l, 1, RLENGTH)
        rest = substr(l, RLENGTH + 1)
        split(head, f, /[ \t]+/)
        pid = f[3]
        for (i = 2; i <= length(rest); i++) {
            if (substr(rest, i, 1) != ":") continue
            if (i == length(rest) || substr(rest, i + 1, 1) ~ /[ \t]/) {
                tag = trim(substr(rest, 1, i - 1))
                if (tag != "") return pid "|" tag
            }
        }
        return ""
    }
    if (match(l, /^[VDIWEFS]\/[^(]+\([ \t]*[0-9]+[ \t]*\)[ \t]*:/)) {
        s = substr(l, 3, RLENGTH - 2)
        p = index(s, "(")
        tag = trim(substr(s, 1, p - 1))
        r = substr(s, p + 1)
        sub(/\).*/, "", r)
        pid = trim(r)
        return pid "|" tag
    }
    return ""
}
function flush(   i) {
    if (have && nonblank) {
        total++
        if (isme) mine++
        if (mode == "all" || (mode == "mine" && isme)) for (i = 1; i <= bn; i++) print buf[i]
    }
    bn = 0; nonblank = 0; isme = 0
}
{
    line = $0
    lo = tolower(line)
    if (lo ~ /^-----+[ \t]*beginning of/) next
    key = linekey(line)
    banner = (index(line, "*** *** ***") > 0) || (index(lo, "fatal exception") > 0)
    if (!have || banner || (key != "" && curkey != "" && key != curkey)) {
        flush()
        have = 1
        curkey = key
    } else if (key != "" && curkey == "") curkey = key
    buf[++bn] = line
    if (line !~ /^[ \t]*$/) nonblank = 1
    if (lo ~ /pgpu_server|libpgpu_server\.so|com\.tsurutanmen\.pocketgpu/) isme = 1
}
END {
    flush()
    if (mode == "count") print total + 0, mine + 0
}
AWKEOF
)

# ---- 圧縮ファイル・ファイルマネージャー ----
# make_archive 出す所 名前 ：ARCHIVE に作ったファイルを入れる。作れなければ 1
make_archive() {
    local root=$1 name=$2 fmt=${PGPU_LOG_ARCHIVE:-}
    if [ -z "$fmt" ]; then command -v zip > /dev/null 2>&1 && fmt=zip || fmt=tar; fi
    ARCHIVE=
    if [ "$fmt" = zip ]; then
        (cd -- "$root" && zip -q -r -X "$name.zip" "$name") > /dev/null 2>&1 || return 1
        ARCHIVE="$root/$name.zip"
    else
        # 持ち主の名前（ユーザー名）を入れない。使えない tar のときは付けずに作る
        (cd -- "$root" && tar --owner=0 --group=0 --numeric-owner -czf "$name.tar.gz" "$name") > /dev/null 2>&1 \
            || (cd -- "$root" && rm -f -- "$name.tar.gz" && tar -czf "$name.tar.gz" "$name") > /dev/null 2>&1 || return 1
        ARCHIVE="$root/$name.tar.gz"
    fi
    [ -s "$ARCHIVE" ]
}
launch_folder() { # $1 = 圧縮ファイル
    if [ -n "${PGPU_TEST_LAUNCH_LOG:-}" ]; then
        printf 'folder %s\n' "$1" >> "$PGPU_TEST_LAUNCH_LOG"
        return 0
    fi
    xdg-open "$(dirname -- "$1")" > /dev/null 2>&1 < /dev/null &
}
can_launch() { [ -n "${PGPU_TEST_LAUNCH_LOG:-}" ] || { command -v xdg-open > /dev/null 2>&1 && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; }; }

# =====================================================================
# 流れ
# =====================================================================
out "$(M 'PocketGPU Android の記録をまとめます' 'Collect the PocketGPU Android log')"
out "$(M 'Android の記録を 1 つの圧縮ファイルにまとめます。何も変更しません。管理者権限は必要ありません。' 'This puts the Android logs into one archive. It changes nothing. No administrator rights are needed.')"

# ==== 1. adb ====
while :; do
    step 1 adb
    if android_find_adb; then
        ok "$(M "adb が見つかりました：$ADB" "adb was found: $ADB")"
        break
    fi
    android_adb_missing
    ask_retry || stop_here
done

# ==== 2. スマホ ====
while :; do
    step 2 "$(M 'スマートフォンをチェック' 'Check the phone')"
    if android_select_device; then
        android_phone_info
        break
    fi
    android_device_problem
    ask_retry || stop_here
done
build_hide
MODEL=$ANDROID_MODEL
MAKER=$(prop ro.product.manufacturer)
RELEASE=$ANDROID_REL
SDK=$ANDROID_SDK
ABI=$ANDROID_ABI
SOC=$(prop ro.soc.model)
[ -n "$SOC" ] || SOC=$(prop ro.board.platform)

GLES=
adbf "$WORK/sf.txt" 60 shell dumpsys SurfaceFlinger
GLES=$(sed -n 's/^[[:space:]]*GLES:[[:space:]]*\(.*[^[:space:]]\)[[:space:]]*$/\1/p' "$WORK/sf.txt" | head -n 1)
APPVER=$(android_app_version)
RUNNING=0
[ -n "$(ashr "pidof $DEV_NAME")" ] && RUNNING=1

adbf "$WORK/recv.raw" 60 shell cat "$DEV_DIR/$DEV_NAME.log"
RECV_MISSING=0
{ [ "$ADB_RC" != 0 ] || grep -q 'No such file or directory' "$WORK/recv.raw"; } && RECV_MISSING=1
RECV_LINES=0
RECV_GPU=
if [ "$RECV_MISSING" = 0 ]; then
    trim_tail < "$WORK/recv.raw" > "$WORK/recv.txt"
    RECV_LINES=$(line_count "$WORK/recv.txt")
    RECV_GPU=$(sed -n -E 's/.*(Receiver GPU|受け手の GPU):[[:space:]]*(.*[^[:space:]])[[:space:]]*$/\2/p' "$WORK/recv.txt" | head -n 1)
fi

ok "$(M "使用するスマホ：${MODEL:-?}・Android ${RELEASE:-?}" "Phone to use: ${MODEL:-?}, Android ${RELEASE:-?}")"
info "$(M '機種：' 'Model: ')${MODEL:--}"
info "Android: ${RELEASE:--}${SDK:+ (SDK $SDK)}"
info "GPU: ${RECV_GPU:-${GLES:--}}"
info "$(M 'アプリ：' 'App: ')${APPVER:-$(M '入っていません' 'not installed')}"
info "$(M '受け手：' 'Receiver: ')$([ "$RUNNING" = 1 ] && M '動いています' 'running' || M '止まっています' 'not running')"

# ==== 3. 記録 ====
step 3 "$(M '記録を集める' 'Collect the logs')"
if [ "$RECV_MISSING" = 1 ]; then
    info "$(M '受け手の記録：' 'Receiver log: ')$(M 'ファイルがありません' 'no file')"
else
    info "$(M '受け手の記録：' 'Receiver log: ')$(format_lines "$RECV_LINES")"
fi

adbf "$WORK/crash.raw" 60 logcat -d -b crash
read -r CRASH_ALL CRASH_MINE < <(awk -v mode=count "$AWK_CRASH" "$WORK/crash.raw")
OTHERS=$((CRASH_ALL - CRASH_MINE))
info "$(M '落ちた記録：PocketGPU ' 'Crash log: PocketGPU ')$CRASH_MINE$(M " 件・ほかのアプリ $OTHERS 件" ", other apps $OTHERS")"
SEND_FULL=0
if [ "$OTHERS" -gt 0 ]; then
    ask_yes "$(M '落ちた記録を全部送りますか？ほかのアプリの行も入ります（N：PocketGPU だけ）' 'Send the full crash log, with other apps? (N: PocketGPU only)')" && SEND_FULL=1
fi
if [ "$SEND_FULL" = 1 ]; then awk -v mode=all "$AWK_CRASH" "$WORK/crash.raw" > "$WORK/crash.sel"; else awk -v mode=mine "$AWK_CRASH" "$WORK/crash.raw" > "$WORK/crash.sel"; fi

APP_CHECKED=0
APP_LINES=0
if [ -n "$APPVER" ]; then
    adbf "$WORK/app.raw" 60 logcat -d -s iphone-rpc-vk
    APP_CHECKED=1
    grep -v -i -E '^-{5,}[[:space:]]*beginning of' "$WORK/app.raw" | trim_tail > "$WORK/app.txt"
    APP_LINES=$(line_count "$WORK/app.txt")
    info "$(M 'アプリの記録：' 'App log: ')$(format_lines "$APP_LINES")"
else
    info "$(M 'アプリの記録：' 'App log: ')$(M 'アプリが入っていないので飛ばします' 'skipped (app not installed)')"
fi

adbv FWLIST 30 forward --list
adbv ADBVER 30 version

# ==== 4. 圧縮ファイル ====
step 4 "$(M '圧縮ファイルを作る' 'Make the archive')"
ROOT=$(get_out_root "${PGPU_LOG_OUT:-}")
STAMP=$(date '+%Y%m%d-%H%M%S')
NAME=pocketgpu-android-log-$STAMP
DIR=$ROOT/$NAME
n=1
while [ -e "$DIR" ] || [ -e "$DIR.zip" ] || [ -e "$DIR.tar.gz" ]; do
    n=$((n + 1))
    NAME=pocketgpu-android-log-$STAMP-$n
    DIR=$ROOT/$NAME
done

{
    echo 'PocketGPU Android log'
    echo "Created: $(date '+%Y-%m-%d %H:%M')"
    echo
    echo '[Kit]'
    echo "Folder: $KIT"
    if [ -r "$KIT/tools/kit.ini" ]; then
        KI=$(tr -d '\r' < "$KIT/tools/kit.ini" | grep -E '^(proto|app|platform)=' || true)
        if [ -n "$KI" ]; then while IFS= read -r l; do echo "tools/kit.ini: $l"; done <<< "$KI"; else echo 'tools/kit.ini: (empty)'; fi
    else
        echo 'tools/kit.ini: (no tools/kit.ini)'
    fi
    echo
    echo '[PC]'
    echo "Linux: ${OS_NAME:-unknown}"
    echo "Family: $FAMILY"
    echo "Kernel: $(uname -r 2> /dev/null || echo '?')"
    echo "glibc: $(getconf GNU_LIBC_VERSION 2> /dev/null | awk '{print $2}')"
    echo "Architecture: $(uname -m 2> /dev/null || echo '?')"
    echo "Session: display=$([ -n "${DISPLAY:-}" ] && echo yes || echo no) wayland=$([ -n "${WAYLAND_DISPLAY:-}" ] && echo yes || echo no)"
    while IFS= read -r l; do [ -n "${l// /}" ] && echo "adb: ${l#"${l%%[![:space:]]*}"}"; done <<< "$ADBVER"
    echo "Port in use (PGPU_PORT): $PGPU_PORT$([ -n "$PORT_WAS_SET" ] || echo ' (default)')"
    if [ -n "$(printf '%s' "$FWLIST" | tr -d '[:space:]')" ]; then
        # 1 列目はスマホのシリアル。どのスマホの物でも隠す
        while IFS= read -r l; do
            [ -n "${l// /}" ] || continue
            echo "adb forward --list: (serial) ${l#*[[:space:]]}"
        done <<< "$FWLIST"
    else
        echo 'adb forward --list: (none)'
    fi
    echo
    echo '[Phone]'
    echo "Model: $MODEL"
    echo "Maker: $MAKER"
    echo "Android: $RELEASE (SDK $SDK)"
    echo "ABI: $ABI"
    echo "Chip: $SOC"
    echo "GPU (receiver log): ${RECV_GPU:--}"
    echo "GPU (GLES): ${GLES:--}"
    echo "PocketGPU app: ${APPVER:-not installed}"
    echo "Kit receiver running: $([ "$RUNNING" = 1 ] && echo yes || echo no)"
    echo
    echo '[Files]'
    echo "receiver-log.txt: $([ "$RECV_MISSING" = 1 ] && echo 'no file on the phone' || echo "$RECV_LINES lines")"
    if [ "$SEND_FULL" = 1 ]; then echo "logcat-crash.txt: full (other apps included), $CRASH_ALL blocks"; else echo "logcat-crash.txt: PocketGPU only, $CRASH_MINE blocks, $OTHERS other blocks left out"; fi
    echo "logcat-app.txt: $([ "$APP_CHECKED" = 1 ] && echo "$APP_LINES lines" || echo 'skipped (app not installed)')"
    echo
    echo 'Not collected: phone number, IMEI, accounts, phone serial number, kit mark.'
    echo 'Hidden: PC user name, PC name, adb serial number, e-mail addresses.'
} > "$WORK/info.txt"

FAILED=0
{
    mkdir -p -- "$DIR" \
        && write_log info.txt < "$WORK/info.txt" \
        && {
            echo "# adb shell cat $DEV_DIR/$DEV_NAME.log"
            if [ "$RECV_MISSING" = 1 ]; then echo '(no file on the phone)'; else limit_lines < "$WORK/recv.txt"; fi
        } | write_log receiver-log.txt \
        && {
            if [ "$SEND_FULL" = 1 ]; then echo '# adb logcat -d -b crash (full: other apps included)'; else echo "# adb logcat -d -b crash (PocketGPU receiver only: $OTHERS other block(s) left out)"; fi
            if [ -s "$WORK/crash.sel" ]; then limit_lines < "$WORK/crash.sel"; else echo '(no PocketGPU receiver crash found)'; fi
        } | write_log logcat-crash.txt \
        && {
            if [ "$APP_CHECKED" = 1 ]; then
                {
                    echo '# adb logcat -d -s iphone-rpc-vk'
                    if [ "$APP_LINES" -gt 0 ]; then limit_lines < "$WORK/app.txt"; else echo '(empty)'; fi
                } | write_log logcat-app.txt
            fi
        } \
        && make_archive "$ROOT" "$NAME"
} || FAILED=1
if [ "$FAILED" = 1 ] || [ -z "${ARCHIVE:-}" ] || [ ! -s "$ARCHIVE" ]; then
    ng "$(M '圧縮ファイルを作れませんでした' 'Could not make the archive')"
    exit 2
fi

out ""
ok "$ARCHIVE"
case "$ARCHIVE" in *.zip) ;; *) info "$(M '【仮文】zip コマンドが無いので、tar.gz にしました' 'DRAFT: the zip command was not found, so this is a tar.gz')" ;; esac
info "$(M 'この圧縮ファイルを Discord に送ってください' 'Send this file on Discord')"
info "$(M '含まない：電話番号・IMEI・アカウント・PC のユーザー名・PC 名' 'Not included: phone number, IMEI, accounts, PC user name, PC name')"
if can_launch && ask_yes "$(M 'ファイルマネージャーでこの場所を開きますか？' 'Open this place in the file manager?')"; then launch_folder "$ARCHIVE"; fi
exit 0
