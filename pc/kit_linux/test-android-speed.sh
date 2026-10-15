#!/usr/bin/env bash
# PocketGPU for Android（Linux）：USB の速さを測って、Discord に貼れる txt を 1 つ作る。Windows のキットの Test-Android-Speed.cmd（tools\android-speed.ps1）と同じ測り方・同じ出力の形。
# スマホには何も入れない・root も要らない。スマホに最初から入っている nc（toybox）を、数十秒だけ使う。この PC には何もインストールしない。管理者の権限も要らない。
#   ./test-android-speed.sh
#   ./test-android-speed.sh --serial 番号    スマホが何台かあるとき、adb devices のシリアルで選ぶ
# 約 30 秒かかる。キットのフォルダ（書けなければホーム）に txt を作る。
#
# 測る物（adb で、下の決まった命令だけを打つ。getprop の全部・dumpsys の全部は使わない）
#   スマホの名前・Android の版・チップ    adb shell getprop ro.product.model / ro.product.marketname / ro.product.manufacturer / ro.build.version.release
#                                        / ro.build.version.sdk / ro.product.cpu.abi / ro.soc.model / ro.board.platform
#   USB の速さ（スマホ側）                  adb shell cat /sys/class/udc/*/current_speed（maximum_speed・state も）。読めなければ「不明」
#   USB の速さ（PC 側）                    /sys/bus/usb/devices/*/speed。シリアルが adb のシリアルと同じ USB 機器の物。5000 以上が USB 3・480 が USB 2。見つからなければ「不明」
#                                        （Windows 版は口の名前からの推定。こちらは PC とスマホが取り決めた速さをそのまま読む）
#   PC→スマホ・スマホ→PC の MB/s・往復 ms  adb forward tcp:口 tcp:口 を通して、nc とつなぐ（口は 47600 から順に、使用中でない物）
#       PC→スマホ  adb shell "nc -l -s 127.0.0.1 -p 口 > /dev/null"      PC が送った量が全部届いて、スマホの nc が終わるまでの時間
#       スマホ→PC  adb shell "head -c バイト数 /dev/zero | nc -l -s 127.0.0.1 -p 口"      PC が読み終わるまでの時間
#       往復      adb shell "nc -l -s 127.0.0.1 -p 口 -e cat"（返ってこなければ mkfifo の形）。1 バイトを送って 1 バイトが返るまで
#     -s 127.0.0.1 が断られる nc のときは、-s 無し（スマホのどの口でも待つ）でもう一度。そのときはファイルにそう書く
#   PC→スマホの時間は、PC が送り終わって接続を閉じてから、スマホの adb shell が終わるまでを含む（bash は接続の片側だけを閉じられないため）。
#     流す量が少ない（遅い）ときは、Windows 版より少し遅く出る
#   adb forward は終わりに消す。前からある adb forward（受け手の口）には触らない
#
# 集めない物：電話番号・IMEI・Google アカウント・端末のシリアル番号
# 伏せる物：PC のユーザー名・PC 名・ホームのフォルダ名・adb のシリアル番号・メールアドレス（「(hidden)」「(serial)」「(email)」。send-android-log.sh と同じ）
#
# 終わりのコード：0 測れた・1 adb が無い/スマホが無い/許可がまだ（案内を出して止まる）・2 txt を作れなかった・3 txt は作ったが、MB/s が測れなかった（nc が使えないなど）
# 画面の文は下書き。言い回しは android/common.sh の M と、ここの M '日本語' 'English' の所で直す。説明の文章は 1 か所だけ（USB 2 のとき）で、【仮文】の印を付けた
#
# 試す人は使わない、試験用の差し替え（android/common.sh の物に加えて）：
#   PGPU_SPEED_OUT=フォルダ        txt をキットのフォルダの代わりにここへ作る
#   PGPU_SPEED_PORT=番号           口を探し始める番号（既定 47600）
#   PGPU_SPEED_MB=数               1 回に流す量（MB）を決め打ちにする（既定は、最初の 4 MB の速さから 8〜192 MB に決める）
#   PGPU_SPEED_PINGS=数            往復を測る回数（既定 200）
#   PGPU_SPEED_ENDWAIT_MS=数       PC→スマホで、スマホの nc が終わるのを待つ長さ（ミリ秒。既定 15000）
#   PGPU_SYS_USB=フォルダ          /sys/bus/usb/devices の代わり（start-pocketgpu.sh と同じ）
#   PGPU_TEST_HOSTNAME=名前        PC 名の代わり
#   PGPU_TEST_TCP_PROBE=パス       /dev/tcp が使えるかを見るときに開く物の代わり（/dev/tcpx/127.0.0.1/9 にすると、使えない bash のふり）
#   PGPU_TEST_CLIP=パス           クリップボードの代わりに、コピーするはずだった物をこのファイルに書く
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"
set -u
self=$(readlink -f -- "${BASH_SOURCE[0]}")
KIT=$(dirname -- "$self")
SELF=./test-android-speed.sh
LOGFILE="$KIT/pocketgpu-android-speedtest-log.txt"
TOTAL=5
# shellcheck source=android/common.sh
. "$KIT/android/common.sh"
usage() {
    say '使い方：./test-android-speed.sh [--serial シリアル]' 'Usage: ./test-android-speed.sh [--serial SERIAL]'
    say '  USB の速さ（PC→スマホ・スマホ→PC の MB/s と、往復の ms）を測って、Discord に貼れる txt を作ります。約 30 秒かかります。' '  Measures the USB speed (MB/s in both directions and the round trip in ms) and writes a text file to paste on Discord. It takes about 30 seconds.'
    say '  スマホには何もインストールしません。root も要りません。スマホに入っている nc を数十秒だけ使います' '  Nothing is installed on the phone and no root is needed. It uses the nc command that Android already has, for a few seconds.'
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
# 切れた接続に書いても、この台本が黙って終わらないようにする（書くのが失敗するだけにする）
trap '' PIPE
make_work speed || {
    echo "ERROR: could not make a temporary folder (TMPDIR=${TMPDIR:-/tmp})" >&2
    exit 2
}
FWD_PORT=0
PH_PID=
FORCEKILLED=0
BINDOPT='-s 127.0.0.1 '
BINDFELL=0
# 終わるとき（途中で止めたときも）：自分が起こした adb shell だけを PID で止め、自分が作った adb forward を消し、作業場所を消す
cleanup() {
    exec 3>&- 4>&-
    if [ -n "$PH_PID" ] && kill -0 "$PH_PID" 2> /dev/null; then kill "$PH_PID" 2> /dev/null; fi
    remove_forward
    cleanup_work
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# ---- 小さな道具 ----
NOW=0
now() { # NOW に今の時刻（マイクロ秒）
    if [ -n "${EPOCHREALTIME:-}" ]; then NOW=${EPOCHREALTIME/[.,]/}; else NOW=$(date +%s%6N); fi
}
msleep() { read -r -t "$1" _ <> "$WORK/sleep.fifo" || true; } # 秒（0.005 など）。fork しない
mkfifo "$WORK/sleep.fifo" 2> /dev/null
first_line() { awk 'NF { gsub(/^[ \t]+|[ \t]+$/, ""); gsub(/[ \t]+/, " "); print; exit }' <<< "$1"; }
fmt1() { awk -v x="$1" 'BEGIN { printf "%.1f", x }'; }
fmtms() { awk -v x="$1" 'BEGIN { x = x / 1000; if (x < 10) printf "%.2f", x; else printf "%.1f", x }'; } # マイクロ秒を ms に
median() { printf '%s\n' "$@" | sort -n | awk '{ a[NR] = $1 } END { if (NR) print a[int((NR - 1) / 2) + 1]; else print 0 }'; }
# MB/s ：バイト数とマイクロ秒から
mbps() { awk -v b="$1" -v us="$2" 'BEGIN { if (us <= 0) us = 1; printf "%.4f", (b / 1048576) / (us / 1000000) }'; }

remove_forward() {
    if [ "$FWD_PORT" != 0 ]; then
        "$ADB" forward --remove "tcp:$FWD_PORT" > /dev/null 2>&1 < /dev/null
        FWD_PORT=0
    fi
}
# この PC の口が待ち状態か
pc_listening() {
    local hex
    hex=$(printf '%04X' "$1")
    awk -v p="$hex" '$4 == "0A" && toupper(substr($2, length($2) - 3)) == p { f = 1 } END { exit !f }' /proc/net/tcp /proc/net/tcp6 2> /dev/null
}
# スマホの口が待ち状態か（/proc/net/tcp と tcp6 の 0A）。0 待っている・1 待っていない・2 読めない
phone_listening() {
    local hex
    hex=$(printf '%04X' "$1")
    adbf "$WORK/tcp.txt" 15 shell cat /proc/net/tcp /proc/net/tcp6
    { [ "$ADB_RC" = 0 ] && grep -q -E '^[[:space:]]*sl[[:space:]]' "$WORK/tcp.txt"; } || return 2
    awk -v p="$hex" '$4 == "0A" && toupper(substr($2, length($2) - 3)) == p { f = 1 } END { exit !f }' "$WORK/tcp.txt"
}
# 空いている口を選んで adb forward tcp:口 tcp:口 を作る。この PC の使用中・前からある adb forward・スマホ側の使用中を避ける。作れたら FWD_PORT に入れる
new_forward() {
    local start=47600 i p fw rc
    case "${PGPU_SPEED_PORT:-}" in '' | *[!0-9]* | ?????[0-9]*) ;; *) start=$PGPU_SPEED_PORT ;; esac
    adbv fw 30 forward --list
    for ((i = 0; i < 40; i++)); do
        p=$((start + i))
        [ "$p" -le 65535 ] || break
        grep -q -E "tcp:$p([^0-9]|\$)" <<< "$fw" && continue
        pc_listening "$p" && continue
        phone_listening "$p"
        rc=$?
        [ "$rc" = 0 ] && continue
        if "$ADB" forward "tcp:$p" "tcp:$p" > /dev/null 2>&1 < /dev/null; then
            FWD_PORT=$p
            return 0
        fi
    done
    return 1
}

# ---- スマホで動かす 1 つの adb shell ----
# 標準入力は開けたままにする（nc の標準入力が、始めから終わりになって nc が先に終わらないように）。名前付きパイプを使い、閉じるのは stop_phone
PH_N=0
PH_IN=
PH_OUT=
PH_ERR=
PH_TEXT=
start_phone() { # $1 = スマホで動かす命令
    PH_N=$((PH_N + 1))
    PH_IN=$WORK/ph$PH_N.in
    PH_OUT=$WORK/ph$PH_N.out
    PH_ERR=$WORK/ph$PH_N.err
    mkfifo "$PH_IN" 2> /dev/null || return 1
    exec 4<> "$PH_IN"
    "$ADB" shell "$1" < "$PH_IN" > "$PH_OUT" 2> "$PH_ERR" 4>&- &
    PH_PID=$!
    return 0
}
phone_alive() {
    local line
    [ -n "$PH_PID" ] || return 1
    [ -r "/proc/$PH_PID/stat" ] || return 1
    read -r line < "/proc/$PH_PID/stat" 2> /dev/null || return 1
    line=${line##*) }
    [ "${line%% *}" != Z ] && [ "${line%% *}" != X ]
}
# 終わらせる。自分が起こした adb shell だけを PID で止める。出力の文字は PH_TEXT
stop_phone() {
    local i
    exec 4>&-
    for ((i = 0; i < 60; i++)); do
        phone_alive || break
        msleep 0.05
    done
    if phone_alive; then
        kill "$PH_PID" 2> /dev/null
        FORCEKILLED=1
    fi
    [ -n "$PH_PID" ] && wait "$PH_PID" 2> /dev/null
    PH_TEXT=$(cat "$PH_ERR" "$PH_OUT" 2> /dev/null | tr -d '\r')
    rm -f -- "$PH_IN"
    PH_PID=
}
# nc が待ち始めるのを待つ。WPS に listening | exited（nc が始まらずに終わった）| unknown（/proc が読めない。少し待って進む）| timeout
WPS=
wait_phone_server() { # $1 = 口
    local t0 unknown=0 rc
    now
    t0=$NOW
    while :; do
        now
        if [ $((NOW - t0)) -ge 8000000 ]; then WPS=timeout; return; fi
        if ! phone_alive; then WPS=exited; return; fi
        phone_listening "$1"
        rc=$?
        if [ "$rc" = 0 ]; then WPS=listening; return; fi
        if [ "$rc" = 2 ]; then
            unknown=$((unknown + 1))
            if [ "$unknown" -ge 2 ]; then
                msleep 0.8
                if phone_alive; then WPS=unknown; else WPS=exited; fi
                return
            fi
        fi
        msleep 0.15
    done
}
# この PC の口に TCP でつなぐ。つながったら fd 3 に開く
connect_tcp() {
    { exec 3<> "/dev/tcp/127.0.0.1/$1"; } 2> /dev/null
}

# ---- 流すデータ ----
# 4 MB の乱数の塊を繰り返して流す（cat に塊の名前を並べる。1 つの cat なので、止まったら timeout で止められる）
PAYLOAD=()
make_payload() { # $1 = バイト数
    local full rem i
    if [ ! -s "$WORK/buf4" ]; then head -c 4194304 /dev/urandom > "$WORK/buf4"; fi
    full=$(($1 / 4194304))
    rem=$(($1 % 4194304))
    PAYLOAD=()
    for ((i = 0; i < full; i++)); do PAYLOAD+=("$WORK/buf4"); done
    if [ "$rem" -gt 0 ]; then
        head -c "$rem" "$WORK/buf4" > "$WORK/bufr"
        PAYLOAD+=("$WORK/bufr")
    fi
}
tmo() { local t=$1; shift; if [ -n "$HAVE_TIMEOUT" ]; then timeout "$t" "$@"; else "$@"; fi; }

# ---- 1 回の転送 ----
# 結果：R_OK（1 で測れた）・R_MBPS・R_US・R_END（closed | sent）・R_WHY（server：nc が始まらない | connect | io）・R_DETAIL
R_OK=0
R_MBPS=
R_US=0
R_END=
R_WHY=
R_DETAIL=
r_fail() { R_OK=0; R_WHY=$1; R_DETAIL=$2; }
# 仕上げ：つないだ口を閉じて、スマホの adb shell を終わらせる
r_end() { exec 3>&-; stop_phone; }
# nc を起こして、待ち始めて、つなぐまで。うまくいけば 0。だめなら R_WHY を決めて 1
r_open() { # $1 = スマホの命令 $2 = 口
    R_OK=0
    if ! start_phone "$1"; then r_fail server 'could not start adb'; return 1; fi
    wait_phone_server "$2"
    if [ "$WPS" = exited ] || [ "$WPS" = timeout ]; then
        stop_phone
        r_fail server "$(first_line "$PH_TEXT")"
        return 1
    fi
    if ! connect_tcp "$2"; then r_end; r_fail connect 'connect failed'; return 1; fi
    return 0
}
measure_to() { # PC→スマホ。$1 = 口 $2 = バイト数
    local bytes=$2 us0 usent us1 end=sent deadline
    r_open "nc -l ${BINDOPT}-p $1 > /dev/null" "$1" || return 0
    make_payload "$bytes"
    now
    us0=$NOW
    if ! tmo 120 cat "${PAYLOAD[@]}" >&3 2> /dev/null; then
        r_end
        r_fail io 'send stopped'
        return 0
    fi
    now
    usent=$NOW
    exec 3>&-
    # スマホの nc が全部受けて終わると、adb shell も終わる。そこまでを時間にする。終わるのが見えなければ送り終わりまで（実際より速く出るので、ファイルにそう書く）
    R_US=$((usent - us0))
    deadline=$((usent + ${PGPU_SPEED_ENDWAIT_MS:-15000} * 1000))
    case "${PGPU_SPEED_ENDWAIT_MS:-15000}" in '' | *[!0-9]*) deadline=$((usent + 15000000)) ;; esac
    while :; do
        if ! phone_alive; then
            now
            us1=$NOW
            end=closed
            R_US=$((us1 - us0))
            break
        fi
        now
        [ "$NOW" -ge "$deadline" ] && break
        msleep 0.005
    done
    [ "$R_US" -le 0 ] && R_US=1
    r_end
    R_OK=1
    R_END=$end
    R_MBPS=$(mbps "$bytes" "$R_US")
}
measure_from() { # スマホ→PC。$1 = 口 $2 = バイト数
    local bytes=$2 us0 us1 got
    r_open "head -c $bytes /dev/zero | nc -l ${BINDOPT}-p $1" "$1" || return 0
    now
    us0=$NOW
    got=$(tmo 120 head -c "$bytes" <&3 2> /dev/null | wc -c)
    now
    us1=$NOW
    r_end
    if [ "${got:-0}" -lt "$bytes" ]; then
        r_fail io "received $((got / 1048576)) of $((bytes / 1048576)) MB"
        return 0
    fi
    R_US=$((us1 - us0))
    [ "$R_US" -le 0 ] && R_US=1
    R_OK=1
    R_MBPS=$(mbps "$bytes" "$R_US")
}
# 往復：1 バイトを送って 1 バイトが返るまで。$1 = 口 $2 = e（nc -e cat）か fifo $3 = 回数
RTT_MIN=
RTT_MED=
RTT_P95=
RTT_MAX=
RTT_N=0
RTT_MODE=
measure_rtt() {
    local port=$1 mode=$2 pings=$3 cmd fifo=/data/local/tmp/pgpu_rtt.fifo i warm=20 t0 t1 tstart ch rc
    local -a us=()
    if [ "$mode" = e ]; then cmd="nc -l ${BINDOPT}-p $port -e cat"
    else cmd="rm -f $fifo; mkfifo $fifo && cat $fifo | nc -l ${BINDOPT}-p $port > $fifo; rm -f $fifo"; fi
    r_open "$cmd" "$port" || return 0
    now
    tstart=$NOW
    for ((i = 0; i < warm + pings; i++)); do
        now
        t0=$NOW
        if ! printf 'U' >&3 2> /dev/null; then r_end; r_fail io 'no echo from the phone'; return 0; fi
        read -r -n 1 -t 4 -u 3 ch
        rc=$?
        if [ "$rc" -gt 128 ]; then r_end; r_fail io 'no echo from the phone'; return 0; fi
        if [ "$rc" != 0 ]; then r_end; r_fail io 'connection closed'; return 0; fi
        now
        t1=$NOW
        [ "$i" -ge "$warm" ] && us+=($((t1 - t0)))
        # 1 回が遅いスマホでも 12 秒で切り上げる（20 回は測る）
        if [ $((t1 - tstart)) -gt 12000000 ] && [ "${#us[@]}" -ge 20 ]; then break; fi
    done
    r_end
    RTT_N=${#us[@]}
    read -r RTT_MIN RTT_MED RTT_P95 RTT_MAX < <(printf '%s\n' "${us[@]}" | sort -n | awk '{ a[NR] = $1 } END { n = NR; c = int((n * 95 + 99) / 100); if (c > n) c = n; print a[1], a[int((n - 1) / 2) + 1], a[c], a[n] }')
    RTT_MODE=$mode
    R_OK=1
}
# 1 つの向きを、runs 回測る。bind の形が断られたら -s 無しで 1 度やり直す（最初の 1 回だけ）。結果：DIR_VALUES（MB/s の並び）・DIR_END_SENT（1 なら閉じるのが見えなかった回がある）・DIR_FAIL_WHY・DIR_FAIL_DETAIL
DIR_VALUES=()
DIR_END_SENT=0
DIR_FAIL_WHY=
DIR_FAIL_DETAIL=
measure_direction() { # $1 = to か from $2 = 口 $3 = バイト数 $4 = 回数
    local i fn=measure_from
    [ "$1" = to ] && fn=measure_to
    DIR_VALUES=()
    DIR_END_SENT=0
    DIR_FAIL_WHY=
    DIR_FAIL_DETAIL=
    for ((i = 0; i < $4; i++)); do
        $fn "$2" "$3"
        if [ "$R_OK" != 1 ] && [ "$R_WHY" = server ] && [ -n "$BINDOPT" ] && [ "$BINDFELL" = 0 ]; then
            BINDOPT=
            BINDFELL=1
            $fn "$2" "$3"
        fi
        if [ "$R_OK" != 1 ]; then DIR_FAIL_WHY=$R_WHY; DIR_FAIL_DETAIL=$R_DETAIL; break; fi
        DIR_VALUES+=("$R_MBPS")
        [ "$1" = to ] && [ "$R_END" = sent ] && DIR_END_SENT=1
    done
}

# ---- USB の速さ ----
# スマホ側：/sys/class/udc/*/current_speed は、スマホの USB コントローラーが PC と取り決めた速さ。読めないスマホ（SELinux・古い Android）もある
udc_value() {
    adbf "$WORK/udc.txt" 15 shell cat "/sys/class/udc/*/$1"
    grep -v -i -E 'No such file|Permission denied|not found|^cat:|inaccessible' "$WORK/udc.txt" | awk 'NF { gsub(/^[ \t]+|[ \t]+$/, ""); printf "%s%s", (n++ ? "," : ""), $0 }'
}
speed_label() {
    case "$(printf '%s' "$1" | tr 'A-Z' 'a-z')" in
    super-speed-plus) printf '%s' 'super-speed-plus (10 Gbps, USB 3.1 or newer)' ;;
    super-speed) printf '%s' 'super-speed (5 Gbps, USB 3)' ;;
    high-speed) printf '%s' 'high-speed (480 Mbps, USB 2)' ;;
    full-speed) printf '%s' 'full-speed (12 Mbps)' ;;
    low-speed) printf '%s' 'low-speed (1.5 Mbps)' ;;
    *) printf '%s' "$1" ;;
    esac
}
# 'USB 3' | 'USB 2' | 'USB 1' | ''（分からない）
speed_class() {
    case "$(printf '%s' "$1" | tr 'A-Z' 'a-z')" in
    *super-speed*) printf '%s' 'USB 3' ;;
    *high-speed*) printf '%s' 'USB 2' ;;
    *full-speed* | *low-speed*) printf '%s' 'USB 1' ;;
    esac
}
# PC 側：シリアルが adb のシリアルと同じ USB 機器の speed（Mbps）を読む。PC_LEVEL（'USB 3' | 'USB 2' | 'USB 1' | ''）と PC_TEXT
PC_LEVEL=
PC_TEXT=unknown
get_pc_usb() {
    local base=${PGPU_SYS_USB:-/sys/bus/usb/devices} d s want sp
    local -a hits=()
    PC_LEVEL=
    PC_TEXT='unknown'
    if [ -z "${ANDROID_SERIAL:-}" ]; then PC_TEXT='unknown (no serial)'; return 0; fi
    case "$ANDROID_SERIAL" in *:*) PC_TEXT='unknown (the phone is not connected over USB)'; return 0 ;; esac
    want=$(printf '%s' "$ANDROID_SERIAL" | tr 'A-Z' 'a-z')
    for d in "$base"/*; do
        [ -r "$d/serial" ] || continue
        s=$(tr -d '\r\n ' < "$d/serial" 2> /dev/null | tr 'A-Z' 'a-z')
        [ "$s" = "$want" ] && hits+=("$d")
    done
    if [ "${#hits[@]}" -eq 0 ]; then PC_TEXT="unknown (phone not found in $base)"; return 0; fi
    if [ "${#hits[@]}" -gt 1 ]; then PC_TEXT='unknown (more than one USB device has the serial)'; return 0; fi
    sp=$(tr -d '\r\n ' < "${hits[0]}/speed" 2> /dev/null)
    case "$sp" in
    '' | *[!0-9.]*) PC_TEXT='unknown (cannot read the speed)'; return 0 ;;
    esac
    # 5000 以上が SuperSpeed、480 が High-Speed、12 と 1.5 が Full・Low
    if awk -v v="$sp" 'BEGIN { exit !(v >= 5000) }'; then PC_LEVEL='USB 3'; PC_TEXT="$sp Mbps, SuperSpeed (USB 3), read from ${hits[0]}/speed"
    elif awk -v v="$sp" 'BEGIN { exit !(v >= 480) }'; then PC_LEVEL='USB 2'; PC_TEXT="$sp Mbps, High-Speed (USB 2), read from ${hits[0]}/speed"
    else PC_LEVEL='USB 1'; PC_TEXT="$sp Mbps, Full/Low-Speed (USB 1), read from ${hits[0]}/speed"; fi
}
clip_tool() { # コピーに使う道具。無ければ 1
    if [ -n "${PGPU_TEST_CLIP:-}" ]; then CLIP=test; return 0; fi
    if [ -n "${WAYLAND_DISPLAY:-}" ] && command -v wl-copy > /dev/null 2>&1; then CLIP=wl-copy; return 0; fi
    if [ -n "${DISPLAY:-}" ] && command -v xclip > /dev/null 2>&1; then CLIP=xclip; return 0; fi
    if [ -n "${DISPLAY:-}" ] && command -v xsel > /dev/null 2>&1; then CLIP=xsel; return 0; fi
    return 1
}
clip_copy() { # $1 = ファイル
    case "$CLIP" in
    test) cp -- "$1" "$PGPU_TEST_CLIP" ;;
    wl-copy) wl-copy < "$1" > /dev/null 2>&1 ;;
    xclip) xclip -selection clipboard < "$1" > /dev/null 2>&1 ;;
    xsel) xsel --clipboard --input < "$1" > /dev/null 2>&1 ;;
    esac
}

# =====================================================================
# 流れ
# =====================================================================
out "$(M 'PocketGPU Android の速さを測ります' 'Measure the PocketGPU Android speed')"

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
# bash の /dev/tcp（ネットワークへのリダイレクト）が使えるか。一部の bash には組み込まれていない。使えなければ「そんなファイルは無い」と答え、使えれば（口が閉じているので）「接続を断られた」と答える
if (exec 3<> "${PGPU_TEST_TCP_PROBE:-/dev/tcp/127.0.0.1/9}") 2>&1 | grep -q 'No such file or directory'; then
    ng "$(M 'この PC の bash では /dev/tcp が使えません' 'The bash on this PC cannot open /dev/tcp')"
    # 【仮文】鶴田さんが書き直す
    info "$(M '【仮文】/dev/tcp が使える bash（たいていの Linux の bash）で実行してください' 'DRAFT: run this with a bash that has /dev/tcp (most Linux distributions have one)')"
    stop_here
fi

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
MARKET=$(prop ro.product.marketname)
MAKER=$(prop ro.product.manufacturer)
RELEASE=$ANDROID_REL
SDK=$ANDROID_SDK
ABI=$ANDROID_ABI
SOC=$(prop ro.soc.model)
[ -n "$SOC" ] || SOC=$(prop ro.board.platform)
PHONE_NAME=$(printf '%s %s' "$MAKER" "$MODEL" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
if [ -n "$MARKET" ]; then
    case "$(printf '%s' "$PHONE_NAME" | tr 'A-Z' 'a-z')" in *"$(printf '%s' "$MARKET" | tr 'A-Z' 'a-z')"*) ;; *) PHONE_NAME="$PHONE_NAME ($MARKET)" ;; esac
fi
[ -n "$PHONE_NAME" ] || PHONE_NAME=unknown
RUNNING=0
[ -n "$(ashr "pidof $DEV_NAME")" ] && RUNNING=1
ok "$(M "使用するスマホ：${MODEL:-?}・Android ${RELEASE:-?}" "Phone to use: ${MODEL:-?}, Android ${RELEASE:-?}")"
info "$(M '機種：' 'Model: ')$PHONE_NAME"
info "Android: ${RELEASE:--}${SDK:+ (SDK $SDK)}"
info "$(M 'チップ：' 'Chip: ')${SOC:--}"
info "$(M '受け手：' 'Receiver: ')$([ "$RUNNING" = 1 ] && M '動いています' 'running' || M '止まっています' 'not running')"

# ==== 3. USB ====
step 3 "$(M 'USB の接続の速さ' 'USB link speed')"
UDC_CUR=$(udc_value current_speed)
UDC_MAX=$(udc_value maximum_speed)
UDC_STATE=$(udc_value state)
PHONE_USB_TEXT=unknown
[ -n "$UDC_CUR" ] && PHONE_USB_TEXT=$(speed_label "$UDC_CUR")
PHONE_CLASS=$(speed_class "$UDC_CUR")
get_pc_usb
LINK_CLASS=
LINK_BY=
if [ -n "$PHONE_CLASS" ]; then LINK_CLASS=$PHONE_CLASS; LINK_BY=phone
elif [ -n "$PC_LEVEL" ]; then LINK_CLASS=$PC_LEVEL; LINK_BY=PC; fi
info "$(M '端末側：' 'Phone side: ')$([ -n "$UDC_CUR" ] && echo "$PHONE_USB_TEXT" || M '不明' 'unknown')"
[ -n "$UDC_MAX" ] && info "$(M '端末の上限：' 'Phone maximum: ')$(speed_label "$UDC_MAX")"
info "$(M 'PC 側：' 'PC side: ')$PC_TEXT"
info "$(M '接続：' 'Link: ')$([ -n "$LINK_CLASS" ] && echo "$LINK_CLASS (from $LINK_BY)" || M '不明' 'unknown')"
if [ "$PHONE_CLASS" = 'USB 2' ] || { [ -z "$PHONE_CLASS" ] && [ "$PC_LEVEL" = 'USB 2' ]; }; then
    # 【仮文】鶴田さんが書き直す
    warn "$(M '【仮文】USB 3 のポートと USB 3 のケーブルでつなぎ直すと速くなるかもしれません' 'DRAFT: it may be faster with a USB 3 port and a USB 3 cable')"
fi

# ==== 4. 速さ ====
step 4 "$(M 'adb forward の速さ' 'Speed through adb forward')"
TO_VALUES=()
FROM_VALUES=()
TO_FAIL_WHY=
TO_FAIL_DETAIL=
FROM_FAIL_WHY=
FROM_FAIL_DETAIL=
TO_SENT=0
RTT_OK=0
RTT_FAIL=
NC_PROBLEM=
MB_EACH=0
RUNS=0
PORT=0
if ! new_forward; then
    NC_PROBLEM='could not open an adb forward port'
    ng "$(M 'adb forward の口を作れませんでした' 'Could not open an adb forward port')"
else
    PORT=$FWD_PORT
    info "$(M 'PC の口：' 'PC port: ')$PORT"
    # 最初の 4 MB で速さを見て、1 回に流す量を決める（約 2 秒分。8〜192 MB）
    measure_to "$PORT" 4194304
    if [ "$R_OK" != 1 ] && [ "$R_WHY" = server ] && [ -n "$BINDOPT" ] && [ "$BINDFELL" = 0 ]; then
        BINDOPT=
        BINDFELL=1
        measure_to "$PORT" 4194304
    fi
    if [ "$R_OK" != 1 ]; then
        if [ -n "$R_DETAIL" ]; then NC_PROBLEM="nc on the phone did not work: $R_DETAIL"; else NC_PROBLEM="nc on the phone did not work ($R_WHY)"; fi
        ng "$(M 'このスマホの nc が使えませんでした：' 'nc on the phone did not work: ')"
        info "${R_DETAIL:-$R_WHY}"
    else
        PROBE_MBPS=$R_MBPS
        MB_EACH=$(awk -v m="$PROBE_MBPS" 'BEGIN { v = int(m * 2 + 0.5); if (v < 8) v = 8; if (v > 192) v = 192; print v }')
        case "${PGPU_SPEED_MB:-}" in '' | *[!0-9]* | ?????*) ;; 0*) ;; *) MB_EACH=$PGPU_SPEED_MB ;; esac
        RUNS=3
        awk -v m="$PROBE_MBPS" 'BEGIN { exit !(m < 4) }' && RUNS=1
        BYTES=$((MB_EACH * 1048576))
        measure_direction to "$PORT" "$BYTES" "$RUNS"
        TO_VALUES=(${DIR_VALUES[@]+"${DIR_VALUES[@]}"})
        TO_SENT=$DIR_END_SENT
        TO_FAIL_WHY=$DIR_FAIL_WHY
        TO_FAIL_DETAIL=$DIR_FAIL_DETAIL
        if [ "${#TO_VALUES[@]}" -gt 0 ]; then ok "$(M 'PC → 端末：' 'PC to phone: ')$(fmt1 "$(median "${TO_VALUES[@]}")") MB/s"
        else ng "$(M 'PC → 端末：' 'PC to phone: ')${TO_FAIL_DETAIL:-$TO_FAIL_WHY}"; fi
        measure_direction from "$PORT" "$BYTES" "$RUNS"
        FROM_VALUES=(${DIR_VALUES[@]+"${DIR_VALUES[@]}"})
        FROM_FAIL_WHY=$DIR_FAIL_WHY
        FROM_FAIL_DETAIL=$DIR_FAIL_DETAIL
        if [ "${#FROM_VALUES[@]}" -gt 0 ]; then ok "$(M '端末 → PC：' 'Phone to PC: ')$(fmt1 "$(median "${FROM_VALUES[@]}")") MB/s"
        else ng "$(M '端末 → PC：' 'Phone to PC: ')${FROM_FAIL_DETAIL:-$FROM_FAIL_WHY}"; fi
        PINGS=200
        case "${PGPU_SPEED_PINGS:-}" in '' | *[!0-9]* | ?????*) ;; *) [ "$PGPU_SPEED_PINGS" -ge 5 ] && PINGS=$PGPU_SPEED_PINGS ;; esac
        measure_rtt "$PORT" e "$PINGS"
        if [ "$R_OK" != 1 ]; then measure_rtt "$PORT" fifo "$PINGS"; fi
        if [ "$R_OK" = 1 ]; then
            RTT_OK=1
            ok "$(M '往復：' 'Round trip: ')$(fmtms "$RTT_MED") ms"
        else
            RTT_FAIL=${R_DETAIL:-$R_WHY}
            ng "$(M '往復：' 'Round trip: ')$RTT_FAIL"
        fi
    fi
fi
remove_forward
if [ "$FORCEKILLED" = 1 ]; then
    # adb shell が止まらなかったので PID で止めた。スマホの nc が残っていたら、このコマンドで消えるまでの短い間だけ口が残る（スマホを再起動しても消える）
    ash "kill \$(pidof nc) 2>/dev/null; rm -f /data/local/tmp/pgpu_rtt.fifo; true" > /dev/null 2>&1
fi

# ==== 5. txt ====
step 5 "$(M 'txt を作る' 'Write the text file')"
adbv ADBVER 30 version
{
    echo 'PocketGPU Android speed test'
    echo "Created: $(date '+%Y-%m-%d %H:%M')"
    echo
    echo '[Phone]'
    echo "Name: $PHONE_NAME"
    echo "Android: ${RELEASE:--}${SDK:+ (SDK $SDK)}"
    echo "Chip: ${SOC:--}"
    echo "ABI: ${ABI:--}"
    echo "Kit receiver running: $([ "$RUNNING" = 1 ] && echo yes || echo no)"
    echo
    echo '[USB]'
    echo "Phone side (current): $PHONE_USB_TEXT"
    echo "Phone side (maximum): $([ -n "$UDC_MAX" ] && speed_label "$UDC_MAX" || echo unknown)"
    [ -n "$UDC_STATE" ] && echo "Phone USB controller state: $UDC_STATE"
    echo "PC side: $PC_TEXT"
    echo "Link: $([ -n "$LINK_CLASS" ] && echo "$LINK_CLASS (from $LINK_BY)" || echo unknown)"
    echo
    echo '[adb forward]'
    if [ "$PORT" = 0 ] || [ -n "$NC_PROBLEM" ]; then
        echo "Not measured: $NC_PROBLEM"
    else
        echo "Method: adb forward tcp:$PORT to nc on the phone ($([ "$BINDFELL" = 1 ] && echo 'nc listens on all addresses, -s was refused' || echo 'nc listens on 127.0.0.1'))"
        echo "Data: $MB_EACH MB per run, $RUNS run(s) per direction. 1 MB = 1048576 bytes"
        for dir in to from; do
            if [ "$dir" = to ]; then L='PC to phone'; V=(${TO_VALUES[@]+"${TO_VALUES[@]}"}); FW=$TO_FAIL_WHY; FD=$TO_FAIL_DETAIL
            else L='Phone to PC'; V=(${FROM_VALUES[@]+"${FROM_VALUES[@]}"}); FW=$FROM_FAIL_WHY; FD=$FROM_FAIL_DETAIL; fi
            if [ "${#V[@]}" -gt 0 ]; then
                line="$L: $(fmt1 "$(median "${V[@]}")") MB/s (median; runs $(for x in "${V[@]}"; do printf '%s, ' "$(fmt1 "$x")"; done | sed 's/, $//'))"
                [ "$dir" = to ] && [ "$TO_SENT" = 1 ] && line="$line [end of transfer not seen on the phone side]"
                echo "$line"
            else
                echo "$L: not measured (${FD:-$FW})"
            fi
        done
        if [ "$RTT_OK" = 1 ]; then
            echo "Round trip (1 byte): median $(fmtms "$RTT_MED") ms, min $(fmtms "$RTT_MIN"), p95 $(fmtms "$RTT_P95"), max $(fmtms "$RTT_MAX") ($RTT_N pings, $([ "$RTT_MODE" = e ] && echo 'nc -e cat' || echo fifo))"
        else
            echo "Round trip: not measured (${RTT_FAIL:-no answer})"
        fi
    fi
    echo
    echo '[PC]'
    echo "Linux: ${OS_NAME:-unknown}"
    echo "Kernel: $(uname -r 2> /dev/null || echo '?')"
    while IFS= read -r l; do [ -n "${l// /}" ] && [ "${l#Installed as}" = "$l" ] && echo "adb: ${l#"${l%%[![:space:]]*}"}"; done <<< "$ADBVER"
    echo
    echo 'Not collected: phone number, IMEI, accounts, phone serial number. Hidden: PC user name, PC name, adb serial number, e-mail addresses.'
} > "$WORK/speed.raw"
hide < "$WORK/speed.raw" > "$WORK/speed.txt"

ROOT=$(get_out_root "${PGPU_SPEED_OUT:-}")
STAMP=$(date '+%Y%m%d-%H%M%S')
NAME=pocketgpu-android-speed-$STAMP
FILE=$ROOT/$NAME.txt
n=1
while [ -e "$FILE" ]; do
    n=$((n + 1))
    FILE=$ROOT/$NAME-$n.txt
done
if ! cp -- "$WORK/speed.txt" "$FILE" 2> /dev/null || [ ! -s "$FILE" ]; then
    ng "$(M 'txt を作れませんでした' 'Could not write the text file')"
    exit 2
fi

out ""
while IFS= read -r l; do out "$l"; done < "$WORK/speed.txt"
out ""
ok "$FILE"
info "$(M 'この txt を Discord に貼ってください' 'Paste this text on Discord')"
info "$(M '含まない：電話番号・IMEI・アカウント・シリアル番号・PC のユーザー名・PC 名' 'Not included: phone number, IMEI, accounts, serial number, PC user name, PC name')"
if clip_tool && ask_yes "$(M 'クリップボードにコピーしますか？' 'Copy it to the clipboard?')"; then
    if clip_copy "$FILE"; then ok "$(M 'コピーしました' 'Copied')"; else warn "$(M 'コピーできませんでした' 'Could not copy')"; fi
fi
MEASURED=0
{ [ "${#TO_VALUES[@]}" -gt 0 ] || [ "${#FROM_VALUES[@]}" -gt 0 ]; } && MEASURED=1
[ "$MEASURED" = 1 ] && exit 0
exit 3
