# PocketGPU for Android（Linux）：台本の共通の部分。start-android.sh・stop-android.sh・check-android.sh・start-android-mic-camera.sh が . で読む。単独では動かさない。
# 読む側が先に決める物：KIT（キットのフォルダ）・LOGFILE（ログの置き場）・SELF（台本の名前。./start-android.sh のような形）
# 決めてから使う物：ADB（adb の場所）・ANDROID_SERIAL（使うスマホ）・PGPU_PORT（GPU の口。既定は 50300）
# 画面の文は M '日本語' 'English' の組で並べてある。言い回しはここで直す
#
# 試す人は使わない、試験用の差し替え：
#   ADB=パス                    adb の代わり（偽の adb で流れを試す）
#   PGPU_NOPROMPT=1             聞かずに「いいえ」で進み、最後に止まらない
#   PGPU_ANSWERS=y,n,...        PGPU_NOPROMPT のとき、はい・いいえの問いにこの順で答える
#   PGPU_ANDROID_FORM=new|old   カメラとマイクの USB の口の向け方を決める。new は localabstract（アプリ 0.1.2 から）・old は tcp（0.1.1）
#   PGPU_OS_RELEASE=パス         /etc/os-release の代わり
#   PGPU_LANG=ja|en             画面の言葉（無ければ LANG）

APP_PKG=com.tsurutanmen.pocketgpu
APP_ACT=$APP_PKG/.MainActivity
APP_MIN_NEW=0.1.2        # カメラとマイクの口が localabstract になった版
CAM_PORT=50064
AUD_PORT=50063
APP_GPU_PORT=50060       # アプリの中の GPU の受け手が端末で待つ TCP の口（0.1.3 以下。0.1.4 以上は 50060 が使えなければ 50061 から順に待つ）
APP_GPU_USB=pocketgpu-gpu # アプリの中の GPU の受け手が USB 用に待つ localabstract の名前（0.1.4 から）
APP_MIN_GPU_USB=0.1.4    # GPU の USB 用の口（localabstract）が付いた版
CAM_ABSTRACT=pocketgpu-webcam
AUD_ABSTRACT=pocketgpu-audio
DEV_DIR=/data/local/tmp  # キットの受け手を置く所
DEV_NAME=pgpu_server

ja=0
case "${PGPU_LANG:-${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}}" in ja*) ja=1 ;; esac
M() { if [ "$ja" = 1 ]; then printf '%s' "$1"; else printf '%s' "$2"; fi; }
say() { echo "$(M "$1" "$2")"; }

# ---- 画面とログ ----
LOG=()
if [ -t 1 ]; then C_OK=$'\e[32m'; C_WARN=$'\e[33m'; C_NG=$'\e[31m'; C_END=$'\e[0m'; else C_OK=; C_WARN=; C_NG=; C_END=; fi
out() { printf '%s%s%s\n' "${2:-}" "$1" "${2:+$C_END}"; LOG+=("$1"); }
step() { out ""; out "[$1/$TOTAL] $2"; }
ok() { out "  OK  $1" "$C_OK"; }
warn() { out "  !   $1" "$C_WARN"; }
ng() { out "  NG  $1" "$C_NG"; }
info() { out "      $1"; }
cmdline() { out "        $1"; }

NOPROMPT=${PGPU_NOPROMPT:-0}
IFS=',' read -r -a ANSWERS <<< "${PGPU_ANSWERS:-}"
RETRIES=0
ask_yes() {
    local a
    if [ "$NOPROMPT" = 1 ]; then
        a=${ANSWERS[0]:-n}
        ANSWERS=("${ANSWERS[@]:1}")
        out "      $1 -> $([ "$a" = y ] && M 'はい' 'yes' || M 'いいえ' 'no')"
        [ "$a" = y ]
        return
    fi
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

# ログをファイルに書く。利用者の名前・ホーム・PC の名前・スマホのシリアルは伏せる
save_log() {
    local h u
    h=$(cat /proc/sys/kernel/hostname 2> /dev/null || hostname 2> /dev/null || true)
    u=${USER:-$(id -un 2> /dev/null)}
    printf '%s\n' "${LOG[@]}" | sed -e "s#${HOME:-/nonexistent}#~#g" \
        ${u:+-e "s#$u#(user)#g"} ${h:+-e "s#$h#(pc)#g"} ${ANDROID_SERIAL:+-e "s#$ANDROID_SERIAL#(serial)#g"} > "$LOGFILE" 2> /dev/null
}
finish() {
    save_log
    out ""
    out "$(M 'このときのログ：' 'Log of this run: ')$LOGFILE"
    exit "$1"
}
stop_here() {
    out ""
    out "$(M "ここで中断しました。問題を解消してから、もう一度 $SELF を実行してください。" "Stopped here. After fixing it, run $SELF again.")" "$C_WARN"
    out "$(M '解決しないときは、下のログファイルを Discord で送ってください。' 'If you are stuck, send the log file below to us on Discord.')"
    finish 1
}

# ---- この PC の種類 ----
OSR=${PGPU_OS_RELEASE:-/etc/os-release}
OS_ID=
OS_LIKE=
OS_NAME=
if [ -r "$OSR" ]; then
    OS_ID=$(. "$OSR" && printf '%s' "${ID:-}")
    OS_LIKE=$(. "$OSR" && printf '%s' "${ID_LIKE:-}")
    OS_NAME=$(. "$OSR" && printf '%s' "${PRETTY_NAME:-${NAME:-}}")
fi
family() {
    case " $OS_ID $OS_LIKE " in
    *" steamos "*) echo steamos ;;
    *" arch "* | *" cachyos "*) echo arch ;;
    *" fedora "* | *" rhel "*) echo fedora ;;
    *" debian "* | *" ubuntu "*) echo debian ;;
    *" suse "* | *" opensuse "*) echo suse ;;
    *)
        # ID_LIKE を書いていないディストリビューション（CachyOS など）は、パッケージマネージャで見分ける。PGPU_OS_RELEASE の試験では見ない
        if [ -n "${PGPU_OS_RELEASE:-}" ]; then echo other
        elif command -v pacman > /dev/null 2>&1; then echo arch
        elif command -v apt-get > /dev/null 2>&1; then echo debian
        elif command -v dnf > /dev/null 2>&1; then echo fedora
        elif command -v zypper > /dev/null 2>&1; then echo suse
        else echo other; fi
        ;;
    esac
}
FAMILY=$(family)
[ "$FAMILY" = fedora ] && [ -e /run/ostree-booted ] && FAMILY=ostree

# adb を入れるコマンドを出す
adb_install_hint() {
    case "$FAMILY" in
    steamos)
        info "$(M 'SteamOS はシステムが読み取り専用です。デスクトップモードの Konsole で次を実行してください。OS を更新すると削除されるので、更新のたびにもう一度実行してください。' 'SteamOS keeps the system read-only. Run these in Konsole in Desktop Mode. An OS update removes them, so do this again after each update.')"
        cmdline "sudo steamos-readonly disable"
        cmdline "sudo pacman-key --init"
        cmdline "sudo pacman-key --populate archlinux holo"
        cmdline "sudo pacman -S android-tools"
        cmdline "sudo steamos-readonly enable"
        ;;
    arch) cmdline "sudo pacman -S android-tools" ;;
    debian) cmdline "sudo apt install adb" ;;
    fedora) cmdline "sudo dnf install android-tools" ;;
    ostree)
        cmdline "rpm-ostree install android-tools"
        info "$(M 'そのあと PC を再起動してください。' 'Then restart the PC.')"
        ;;
    suse) cmdline "sudo zypper install android-tools" ;;
    *) info "$(M 'パッケージマネージャで adb が入っているパッケージ、たいていは android-tools か android-tools-adb をインストールしてください。' 'Install the package that has adb with your package manager. It is usually called android-tools or android-tools-adb.')" ;;
    esac
    info "$(M 'パッケージを使わない場合は、Google の SDK Platform-Tools for Linux を展開して、platform-tools フォルダをこのキットのフォルダの中に置いてください。' 'Without a package, download SDK Platform-Tools for Linux from Google, unzip it, and put the platform-tools folder inside this kit folder.')"
    info "https://developer.android.com/tools/releases/platform-tools"
    info "$(M '別の場所に置いたときは、export ADB=/場所/platform-tools/adb を実行してから、もう一度実行してください。' 'If you put it somewhere else, run export ADB=/path/platform-tools/adb and run this again.')"
}

# ---- adb を探す。見つかったら ADB に入れて 0。無ければ 1 ----
android_find_adb() {
    local c
    if [ -n "${ADB:-}" ]; then
        [ -x "$ADB" ] && return 0
        ADB_BAD=1
        return 1
    fi
    ADB=$(command -v adb 2> /dev/null || true)
    [ -n "$ADB" ] && return 0
    for c in "$KIT/platform-tools/adb" "$KIT/../platform-tools/adb" "${HOME:-/nonexistent}/platform-tools/adb" "${HOME:-/nonexistent}/Android/Sdk/platform-tools/adb" \
        "${HOME:-/nonexistent}/Downloads/platform-tools/adb" "${ANDROID_HOME:-/nonexistent}/platform-tools/adb" "${ANDROID_SDK_ROOT:-/nonexistent}/platform-tools/adb" \
        /opt/android-sdk/platform-tools/adb /opt/platform-tools/adb /usr/lib/android-sdk/platform-tools/adb; do
        if [ -x "$c" ]; then
            ADB=$c
            return 0
        fi
    done
    ADB=
    return 1
}
ADB_BAD=0
android_adb_missing() {
    if [ "$ADB_BAD" = 1 ]; then
        ng "$(M "環境変数 ADB に $ADB が設定されていますが、そのファイルは実行できません。" "ADB is set to $ADB, but that file cannot be run.")"
        return
    fi
    ng "$(M 'adb が見つかりません。PC からスマホを操作するために必要です。' 'adb was not found. It is needed to control the phone from this PC.')"
    adb_install_hint
}

# スマホに命令する。端末の標準入力を食べないよう /dev/null を渡す
ash() { "$ADB" shell "$@" < /dev/null; }
# 端末の返事の \r を消す
ashr() { ash "$@" 2> /dev/null | tr -d '\r'; }

# ---- 使うスマホを決める ----
# A_STATUS：ok・none・unauth・offline・noperm・many・badserial。ok のとき ANDROID_SERIAL を決めて export する
A_READY=()
A_UNAUTH=()
A_OFFLINE=()
A_NOPERM=()
A_STATUS=none
in_list() {
    local x=$1
    shift
    local y
    for y in "$@"; do [ "$x" = "$y" ] && return 0; done
    return 1
}
android_select_device() {
    local list line serial rest want=${ANDROID_SERIAL:-}
    A_READY=() A_UNAUTH=() A_OFFLINE=() A_NOPERM=()
    "$ADB" start-server > /dev/null 2>&1 < /dev/null
    list=$("$ADB" devices 2> /dev/null < /dev/null | tr -d '\r')
    while IFS= read -r line; do
        case "$line" in "" | "List of devices"* | "* "*) continue ;; esac
        serial=${line%%[[:space:]]*}
        rest=${line#"$serial"}
        rest=${rest#"${rest%%[![:space:]]*}"}
        case "$rest" in
        device | device[[:space:]]*) A_READY+=("$serial") ;;
        unauthorized*) A_UNAUTH+=("$serial") ;;
        offline*) A_OFFLINE+=("$serial") ;;
        "no permissions"*) A_NOPERM+=("$serial") ;;
        esac
    done <<< "$list"
    LOG+=("      adb: ready ${#A_READY[@]}, unauthorized ${#A_UNAUTH[@]}, offline ${#A_OFFLINE[@]}, no permissions ${#A_NOPERM[@]}")
    if [ -n "$want" ]; then
        if in_list "$want" "${A_READY[@]}"; then A_STATUS=ok
        elif in_list "$want" "${A_UNAUTH[@]}"; then A_STATUS=unauth
        elif in_list "$want" "${A_OFFLINE[@]}"; then A_STATUS=offline
        elif in_list "$want" "${A_NOPERM[@]}"; then A_STATUS=noperm
        elif [ $((${#A_READY[@]} + ${#A_UNAUTH[@]} + ${#A_OFFLINE[@]} + ${#A_NOPERM[@]})) -eq 0 ]; then A_STATUS=none
        else A_STATUS=badserial; fi
    elif [ ${#A_READY[@]} -eq 1 ]; then
        ANDROID_SERIAL=${A_READY[0]}
        A_STATUS=ok
    elif [ ${#A_READY[@]} -gt 1 ]; then A_STATUS=many
    elif [ ${#A_UNAUTH[@]} -gt 0 ]; then A_STATUS=unauth
    elif [ ${#A_NOPERM[@]} -gt 0 ]; then A_STATUS=noperm
    elif [ ${#A_OFFLINE[@]} -gt 0 ]; then A_STATUS=offline
    else A_STATUS=none; fi
    [ "$A_STATUS" = ok ] && export ANDROID_SERIAL
    [ "$A_STATUS" = ok ]
}
# つながっているスマホの一覧。複数台のときの案内に使う
android_list_ready() {
    local s m
    for s in "${A_READY[@]}"; do
        m=$("$ADB" -s "$s" shell getprop ro.product.model < /dev/null 2> /dev/null | tr -d '\r')
        info "  $s  ${m:-?}"
    done
}
android_device_problem() {
    case "$A_STATUS" in
    none)
        ng "$(M 'Android のスマホが見つかりません。' 'No Android phone was found.')"
        info "$(M 'スマホを USB ケーブルでこの PC に接続してください。充電専用のケーブルでは接続できません。' 'Plug the phone into this PC with a USB cable. A charge-only cable does not work.')"
        info "$(M 'スマホで USB デバッグをオンにしてください。「設定」>「デバイス情報」の「ビルド番号」を 7 回タップすると、「設定」>「システム」に「開発者向けオプション」が表示されます。その中の「USB デバッグ」をオンにします。' 'Turn on USB debugging on the phone. In Settings > About phone, tap Build number 7 times. Then Developer options appears in Settings > System. Turn on USB debugging there.')"
        info "$(M 'スマホに「USB デバッグを許可しますか？」と表示されたら「許可」をタップしてください。' 'If the phone asks "Allow USB debugging?", tap Allow.')"
        info "$(M 'adb が何を見ているかは、次のコマンドで確認できます：' 'You can see what adb sees with:')"
        cmdline "$ADB devices"
        ;;
    unauth)
        ng "$(M 'スマホはつながっていますが、この PC がまだ許可されていません。' 'The phone is connected, but this PC is not allowed yet.')"
        info "$(M 'スマホのロックを解除して、「USB デバッグを許可しますか？」で「許可」をタップしてください。' 'Unlock the phone and tap Allow on the question "Allow USB debugging?".')"
        info "$(M '表示されないときは、ケーブルを抜いて接続し直してください。それでも表示されないときは、開発者向けオプションの「USB デバッグの許可を取り消す」をタップしてから、接続し直してください。' 'If the question does not appear, unplug the cable and plug it in again. If it still does not appear, tap "Revoke USB debugging authorizations" in Developer options and plug it in again.')"
        ;;
    offline)
        ng "$(M 'スマホが offline と表示されています。' 'The phone is shown as offline.')"
        info "$(M 'スマホのロックを解除して、ケーブルを抜いて接続し直してください。直らないときは、次のコマンドで adb を再起動してください：' 'Unlock the phone and plug the cable in again. If that does not help, restart adb with:')"
        cmdline "$ADB kill-server"
        ;;
    noperm)
        ng "$(M 'この PC のユーザーに、スマホを使う権限がありません。' 'This user on this PC is not allowed to use the phone.')"
        info "$(M 'ケーブルを抜いて接続し直してから、次のコマンドで adb を再起動してください：' 'Plug the cable in again, then restart adb with:')"
        cmdline "$ADB kill-server"
        info "$(M 'それでも直らないときは、udev のルールが入っていません。次のパッケージを入れてから、ケーブルを接続し直してください：' 'If that does not help, the udev rules are missing. Install this package and plug the cable in again:')"
        case "$FAMILY" in
        arch | steamos) cmdline "sudo pacman -S android-udev" ;;
        debian) cmdline "sudo apt install android-sdk-platform-tools-common" ;;
        *) info "$(M 'パッケージマネージャで、adb の udev ルールが入っているパッケージをインストールしてください。' 'Install the package that has the adb udev rules with your package manager.')" ;;
        esac
        ;;
    many)
        ng "$(M 'スマホが 2 台以上つながっています。使うスマホを選んでください。' 'More than one phone is connected. Choose the one to use.')"
        android_list_ready
        info "$(M 'シリアルを指定して、もう一度実行してください：' 'Give the serial and run this again:')"
        cmdline "$SELF --serial ${A_READY[0]}"
        info "$(M '別のスマホを抜いて、もう一度実行することもできます。' 'You can also unplug the other phones and run this again.')"
        ;;
    badserial)
        ng "$(M "指定されたスマホ ${ANDROID_SERIAL:-} が見つかりません。つながっているスマホ：" "The phone ${ANDROID_SERIAL:-} was not found. Connected phones:")"
        android_list_ready
        ;;
    esac
}

# 使うスマホの情報。ANDROID_MODEL・ANDROID_REL・ANDROID_SDK・ANDROID_ABI を決める
android_phone_info() {
    ANDROID_MODEL=$(ashr getprop ro.product.model)
    ANDROID_REL=$(ashr getprop ro.build.version.release)
    ANDROID_SDK=$(ashr getprop ro.build.version.sdk)
    ANDROID_ABI=$(ashr getprop ro.product.cpu.abi)
    LOG+=("      phone: ${ANDROID_MODEL:-?}, Android ${ANDROID_REL:-?}, sdk ${ANDROID_SDK:-?}, abi ${ANDROID_ABI:-?}")
}

# ---- スマホの中の PocketGPU のアプリ ----
android_app_installed() { ashr pm path "$APP_PKG" | grep -q '^package:'; }
android_app_version() {
    ashr "dumpsys package $APP_PKG | grep versionName= | head -n 1" | sed -n 's/.*versionName=//p' | head -n 1 | tr -d ' '
}
version_ge() { # $1 が $2 以上なら 0
    [ -n "$1" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -n 1)" = "$2" ]
}
APK="$KIT/android/pocketgpu.apk"
# アプリが無いとき：入れ方を出す。聞いて、はいなら adb install。入ったら 0
android_install_app() {
    if [ ! -r "$APK" ]; then
        ng "$(M 'スマホに PocketGPU のアプリが入っていません。このキットにはアプリのファイルがありません。キットをすべて展開し直してください。' 'The PocketGPU app is not installed on the phone, and this kit has no app file. Extract the whole kit again.')"
        return 1
    fi
    ng "$(M 'スマホに PocketGPU のアプリが入っていません。' 'The PocketGPU app is not installed on the phone.')"
    info "$(M 'このキットの android/pocketgpu.apk を入れます。次のコマンドと同じです：' 'This installs android/pocketgpu.apk from this kit. It is the same as:')"
    cmdline "$ADB install -r \"$APK\""
    if ask_yes "$(M 'いまスマホに入れますか？' 'Install it on the phone now?')"; then
        local r
        r=$("$ADB" install -r "$APK" 2>&1 < /dev/null | tr -d '\r')
        while IFS= read -r l; do [ -n "$l" ] && info "$l"; done <<< "$r"
        if printf '%s\n' "$r" | grep -q '^Success'; then
            info "$(M 'スマホに Google Play プロテクトの確認が出たときは「インストールする」をタップしてください。' 'If the phone shows a Google Play Protect question, tap Install anyway.')"
            return 0
        fi
        if printf '%s\n' "$r" | grep -q 'UPDATE_INCOMPATIBLE'; then
            info "$(M '別の署名のアプリが入っています。次のコマンドで削除してから、もう一度実行してください：' 'An app with a different signature is installed. Remove it with the command below and run this again:')"
            cmdline "$ADB uninstall $APP_PKG"
        fi
    fi
    return 1
}

# 端末で待っている口の一覧を 1 回で読む。ここから unix の抽象名と TCP の LISTEN を調べる
PROCSNAP=
android_snapshot() {
    PROCSNAP=$(ashr 'echo ==unix; cat /proc/net/unix; echo ==tcp; cat /proc/net/tcp /proc/net/tcp6')
}
snap_unix() { awk -v n="@$1" '/^==tcp/ {exit} $NF == n {f = 1} END {exit !f}' <<< "$PROCSNAP"; }
snap_tcp() {
    local hex
    hex=$(printf '%04X' "$1")
    awk -v p="$hex" '/^==tcp/ {t = 1; next} t && $4 == "0A" && toupper(substr($2, length($2) - 3)) == p {f = 1} END {exit !f}' <<< "$PROCSNAP"
}
# forward を張る。$1 = new か old。$2 = cam か aud。失敗したら FWERR に adb の言葉を入れて 1
FWERR=
fw_add() {
    local port target
    case "$2" in cam) port=$CAM_PORT; target=$CAM_ABSTRACT ;; *) port=$AUD_PORT; target=$AUD_ABSTRACT ;; esac
    if [ "$1" = new ]; then target="localabstract:$target"; else target="tcp:$port"; fi
    FWERR=$("$ADB" forward "tcp:$port" "$target" 2>&1 < /dev/null)
    local rc=$?
    FWERR=$(printf '%s' "$FWERR" | tr -d '\r')
    LOG+=("      adb forward tcp:$port $target -> $rc")
    return $rc
}
# この PC の口 $1 が、使うスマホ（ANDROID_SERIAL）とは別のスマホにつながっていれば 0。adb forward は同じ口を黙って付け替えるので、複数台のときに先に確かめる
android_port_other_phone() {
    local s p
    while read -r s p _; do
        [ "$p" = "tcp:$1" ] && [ "$s" != "${ANDROID_SERIAL:-}" ] && return 0
    done < <("$ADB" forward --list 2> /dev/null < /dev/null | tr -d '\r')
    return 1
}
fw_remove() { "$ADB" forward --remove "tcp:$1" > /dev/null 2>&1 < /dev/null; }

# PGPU_PORT：GPU の口
PGPU_PORT=${PGPU_PORT:-50300}
case "$PGPU_PORT" in '' | *[!0-9]*) PGPU_PORT=50300 ;; esac
