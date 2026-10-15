# PocketGPU for Android（Linux）：send-android-log.sh と test-android-speed.sh が . で読む、共通の小さな道具。単独では動かさない。
# android/common.sh を読んだあとに読む。読む側が先に決める物：KIT（キットのフォルダ）
# 入れる物：作業場所 WORK（make_work）・adb を時間つきで動かす adbf と adbv・伏せる hide（build_hide のあとで使う）・出す所 get_out_root
# 伏せる物（Windows のキットの tools\android-log.ps1 の Hide-Private と同じ）：PC のユーザー名・ホームのフォルダ名・PC 名・adb のシリアル番号・メールアドレス。
#   「(hidden)」「(serial)」「(email)」に直す。3 字以上の名前は、前後が英数字でなければどこでも。短い名前は、/home/ か /Users/ の次の場所だけ
#   （短い名前を全部直すと、記録の文字が壊れるため）。root と localhost は伏せない（何も隠れないので）

export LC_NUMERIC=C

# ---- 作業場所 ----
# make_work 名前 ：mktemp -d の場所を WORK に入れる。作れなければ 1。消すのは、この場所だけ
WORK=
make_work() {
    WORK=$(mktemp -d "${TMPDIR:-/tmp}/pgpu-$1.XXXXXX" 2> /dev/null) || WORK=
    [ -n "$WORK" ]
}
cleanup_work() { [ -n "$WORK" ] && rm -rf -- "$WORK"; }

# ---- adb ----
HAVE_TIMEOUT=
command -v timeout > /dev/null 2>&1 && HAVE_TIMEOUT=1
# adbf ファイル 秒 引数... ：adb の出力（\r と NUL を除く）をファイルに書く。adb の終わりのコードは ADB_RC。標準入力は閉じる
ADB_RC=0
adbf() {
    local f=$1 t=$2
    shift 2
    if [ -n "$HAVE_TIMEOUT" ]; then timeout "$t" "$ADB" "$@" < /dev/null 2> /dev/null | tr -d '\r\0' > "$f"; else "$ADB" "$@" < /dev/null 2> /dev/null | tr -d '\r\0' > "$f"; fi
    ADB_RC=${PIPESTATUS[0]}
}
# adbv 変数 秒 引数... ：同じ物を変数に入れる
adbv() {
    local __var=$1
    shift
    adbf "$WORK/adb.out" "$@"
    printf -v "$__var" '%s' "$(cat "$WORK/adb.out")"
}
# prop 名前 ：getprop の答えの最初の行
prop() { ashr getprop "$1" | awk 'NF {print; exit}' | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'; }
# 後ろの空の行を捨てる
trim_tail() { awk '{ a[NR] = $0 } END { n = NR; while (n > 0 && a[n] ~ /^[ \t]*$/) n--; for (i = 1; i <= n; i++) print a[i] }'; }
line_count() { awk 'END { print NR + 0 }' "$1"; }

# ---- 伏せる ----
sed_esc() { printf '%s' "$1" | sed -e 's/[][\.*^$/|(){}+?#]/\\&/g'; }
HIDE_NAMES=()
HIDE_ARGS=()
add_hide_name() {
    local n=$1 lc x
    case "$n" in '' | *[![:space:]]*) ;; *) return 0 ;; esac
    [ -n "$n" ] || return 0
    lc=$(printf '%s' "$n" | tr 'A-Z' 'a-z')
    case "$lc" in root | localhost | localhost.localdomain | hidden | serial | email | '(none)') return 0 ;; esac
    for x in ${HIDE_NAMES[@]+"${HIDE_NAMES[@]}"}; do [ "$x" = "$lc" ] && return 0; done
    HIDE_NAMES+=("$lc")
}
# build_hide ：スマホのシリアル（ANDROID_SERIAL）が決まったあとに 1 回呼ぶ
build_hide() {
    local h u n e
    h=${PGPU_TEST_HOSTNAME:-$(cat /proc/sys/kernel/hostname 2> /dev/null || hostname 2> /dev/null || true)}
    u=${USER:-$(id -un 2> /dev/null || true)}
    add_hide_name "$u"
    add_hide_name "${LOGNAME:-}"
    add_hide_name "$(id -un 2> /dev/null || true)"
    add_hide_name "$h"
    add_hide_name "${h%%.*}"
    add_hide_name "$(basename -- "${HOME:-/}")"
    for n in ${HIDE_NAMES[@]+"${HIDE_NAMES[@]}"}; do
        e=$(sed_esc "$n")
        HIDE_ARGS+=(-e 's#([/\\])(Users|home)([/\\])'"$e"'([^A-Za-z0-9]|$)#\1\2\3(hidden)\4#Ig')
        if [ "${#n}" -ge 3 ]; then
            # 続けて並んだ 2 つ目は、1 つ目が前後の 1 字を使ってしまうので、2 回かける
            HIDE_ARGS+=(-e 's#(^|[^A-Za-z0-9])'"$e"'([^A-Za-z0-9]|$)#\1(hidden)\2#Ig')
            HIDE_ARGS+=(-e 's#(^|[^A-Za-z0-9])'"$e"'([^A-Za-z0-9]|$)#\1(hidden)\2#Ig')
        fi
    done
    if [ -n "${ANDROID_SERIAL:-}" ] && [ "${#ANDROID_SERIAL}" -ge 4 ]; then
        e=$(sed_esc "$ANDROID_SERIAL")
        HIDE_ARGS+=(-e 's#(^|[^A-Za-z0-9])'"$e"'([^A-Za-z0-9]|$)#\1(serial)\2#Ig')
        HIDE_ARGS+=(-e 's#(^|[^A-Za-z0-9])'"$e"'([^A-Za-z0-9]|$)#\1(serial)\2#Ig')
    fi
    # 最後の部分が英字 2 字以上のときだけ（GPU の版の V@0615.0 や GIT@abc は、メールアドレスではないので残す）
    HIDE_ARGS+=(-e 's#[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.[A-Za-z][A-Za-z]+([^A-Za-z0-9]|$)#(email)\2#g')
}
hide() { sed -E "${HIDE_ARGS[@]}"; }

# ---- 出す所 ----
# get_out_root 上書きの場所 ：上書き・キットのフォルダ・ホーム・TMPDIR の順に、書ける所を探す
get_out_root() {
    local c probe
    for c in "${1:-}" "$KIT" "${HOME:-}" "${TMPDIR:-/tmp}"; do
        [ -n "$c" ] || continue
        mkdir -p -- "$c" 2> /dev/null || continue
        probe="$c/.pgpu-write-test-$$"
        if : > "$probe" 2> /dev/null; then
            rm -f -- "$probe"
            printf '%s' "$c"
            return 0
        fi
    done
    printf '%s' "$KIT"
}
