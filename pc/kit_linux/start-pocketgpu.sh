#!/usr/bin/env bash
# PocketGPU を始める（Linux）：この PC で iPhone の GPU を使うのに必要な物を順にチェックし、足りない物の直し方を出す。
# Windows のキットの Start-PocketGPU.cmd（tools\start.ps1）と同じ順。管理者の権限は要らない。何もインストールしない。
#   ./start-pocketgpu.sh
# チェックする順：この PC → usbmuxd と libimobiledevice → usbmuxd が動いているか → iPhone が USB で見えるか
#                → ペアリング → iPhone のアプリ → USB の速度 → Vulkan のローダーとドライバで iPhone の GPU が見えるか
# 画面の文は下書き。言い回しは M '日本語' 'English' の所で直す
#
# 試す人は使わない、試験用の差し替え：
#   IPHONE_VK_MUX=…             usbmuxd の口（ドライバと同じ名前。番号・/パス・@名前）
#   PGPU_OS_RELEASE=パス         /etc/os-release の代わり
#   PGPU_SYS_USB=フォルダ        /sys/bus/usb/devices の代わり（pgpu-probe usb）
#   PGPU_NOPROMPT=1             聞かずに「いいえ」で進み、最後に止まらない
#   PGPU_ANSWERS=y,n,...        PGPU_NOPROMPT のとき、はい・いいえの問いにこの順で答える
#   PGPU_LANG=ja|en             画面の言葉（無ければ LANG）
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"
set -u
self=$(readlink -f -- "${BASH_SOURCE[0]}")
KIT=$(dirname -- "$self")
PROBE="$KIT/bin/pgpu-probe"
RUN="$KIT/run-on-iphone-gpu.sh"
JSON="$KIT/lib/x86_64/iphone_icd.x86_64.json"
LOGFILE="$KIT/pocketgpu-start-log.txt"
TOTAL=8
ALLOK=1
DEV=
PROTO=3
APPVER=
if [ -r "$KIT/tools/kit.ini" ]; then
    while IFS='=' read -r k v; do
        v=${v%$'\r'}
        case "$k" in proto) PROTO=$v ;; app) APPVER=$v ;; esac
    done < "$KIT/tools/kit.ini"
fi

ja=0
case "${PGPU_LANG:-${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}}" in ja*) ja=1 ;; esac
M() { if [ "$ja" = 1 ]; then printf '%s' "$1"; else printf '%s' "$2"; fi; }

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

# ログをファイルに書く。利用者の名前・ホーム・PC の名前は伏せる
save_log() {
    local h u
    h=$(cat /proc/sys/kernel/hostname 2> /dev/null || hostname 2> /dev/null || true)
    u=${USER:-$(id -un 2> /dev/null)}
    printf '%s\n' "${LOG[@]}" | sed -e "s#${HOME:-/nonexistent}#~#g" \
        ${u:+-e "s#$u#(user)#g"} ${h:+-e "s#$h#(pc)#g"} > "$LOGFILE" 2> /dev/null
}
finish() {
    save_log
    out ""
    out "$(M 'このときのログ：' 'Log of this run: ')$LOGFILE"
    exit "$1"
}
stop_here() {
    out ""
    out "$(M 'ここで中断しました。問題を解消してから、もう一度 ./start-pocketgpu.sh を実行してください。' 'Stopped here. After fixing it, run ./start-pocketgpu.sh again.')" "$C_WARN"
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
# rpm-ostree の OS（Bazzite・Silverblue など）は dnf ではなく rpm-ostree で入れる
[ "$FAMILY" = fedora ] && [ -e /run/ostree-booted ] && FAMILY=ostree

# usbmuxd などを入れるコマンドを出す
install_hint() {
    case "$FAMILY" in
    steamos)
        info "$(M 'SteamOS はシステムが読み取り専用です。デスクトップモードの Konsole で次を実行してください。OS を更新すると削除されるので、更新のたびにもう一度実行してください。' 'SteamOS keeps the system read-only. Run these in Konsole in Desktop Mode. An OS update removes them, so do this again after each update.')"
        info "$(M 'sudo のパスワードを設定していない場合は、先に passwd を実行してください。' 'If you have not set a sudo password yet, run passwd first.')"
        cmdline "sudo steamos-readonly disable"
        cmdline "sudo pacman-key --init"
        cmdline "sudo pacman-key --populate archlinux holo"
        cmdline "sudo pacman -S --needed usbmuxd libimobiledevice"
        cmdline "sudo steamos-readonly enable"
        ;;
    arch) cmdline "sudo pacman -S --needed usbmuxd libimobiledevice vulkan-icd-loader" ;;
    debian) cmdline "sudo apt install usbmuxd libimobiledevice-utils libvulkan1" ;;
    fedora) cmdline "sudo dnf install usbmuxd libimobiledevice-utils vulkan-loader" ;;
    ostree)
        cmdline "rpm-ostree install usbmuxd libimobiledevice-utils"
        info "$(M 'そのあと PC を再起動してください。' 'Then restart the PC.')"
        ;;
    suse) cmdline "sudo zypper install usbmuxd imobiledevice-tools libvulkan1" ;;
    *) info "$(M 'パッケージマネージャで usbmuxd と libimobiledevice のツールと Vulkan のローダーをインストールしてください。' 'Install usbmuxd, the libimobiledevice tools and the Vulkan loader with your package manager.')" ;;
    esac
}

# pgpu-probe の 1 行目を返す（終了番号はそのまま）
probe() { "$PROBE" "$@" 2> /dev/null; }

out "$(M 'PocketGPU を開始します' 'Start PocketGPU')"
out "$(M 'iPhone の GPU を使うために必要な項目を、順にチェックします。管理者権限は必要ありません。' 'This checks, one by one, what is needed to use the iPhone GPU. No administrator rights are needed.')"
[ -f "$KIT/start-android.sh" ] && out "$(M 'Android のスマホを使う場合は、この台本ではなく ./start-android.sh を実行してください。' 'To use an Android phone, run ./start-android.sh instead of this script.')"

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
for f in "$PROBE" "$RUN" "$KIT/start-pocketgpu.sh"; do [ -x "$f" ] || chmod +x "$f" 2> /dev/null; done
"$PROBE" usb > /dev/null 2>&1
if [ $? -ge 126 ]; then
    ng "$(M 'bin/pgpu-probe を実行できません。' 'Cannot run bin/pgpu-probe.')"
    info "$(M 'キットを、実行が禁止されていない場所、たとえばホームフォルダに展開し直してください。' 'Extract the kit again in a place where programs may run, for example your home folder.')"
    stop_here
fi
if [ -n "${DISPLAY:-}" ]; then
    ok "$(M "X11 のディスプレイ $DISPLAY に、iPhone の GPU で描いたウィンドウを表示できます。" "Windows drawn by the iPhone GPU can be shown on the X11 display $DISPLAY.")"
    [ -n "${WAYLAND_DISPLAY:-}" ] && info "$(M 'Wayland のセッションでは、Xwayland 経由で表示します。' 'In a Wayland session they are shown through Xwayland.')"
elif [ -n "${WAYLAND_DISPLAY:-}" ]; then
    warn "$(M 'Wayland だけのセッションです。iPhone の GPU のウィンドウは X11 にだけ表示できるので、Xwayland が必要です。' 'This session is Wayland only. Windows from the iPhone GPU can only be shown on X11, so Xwayland is needed.')"
    ALLOK=0
else
    warn "$(M 'デスクトップが見つかりません。ウィンドウを表示しない計算だけのアプリは使用できます。' 'No desktop was found. Programs that only compute and show no window can still be used.')"
fi
WW=$(probe ww)
if [ "${WW%% *}" = "WW" ] && [ "${WW#WW OK}" != "$WW" ]; then
    ok "$(M 'メモリの書き込みを効率よく検出できます。' 'Memory writes can be tracked efficiently.')"
else
    warn "$(M 'このカーネルでは、メモリの書き込みを効率よく検出できません。動作はしますが、CPU の負荷が高くなります。カーネル 6.7 以降で改善します。' 'This kernel cannot track memory writes efficiently. It still works, but uses more CPU. Kernel 6.7 or later is better.')"
    LOG+=("      $WW")
fi
[ "$FAMILY" = steamos ] && info "$(M 'SteamOS を検出しました。このチェックはデスクトップモードで実行してください。' 'SteamOS detected. Run this check in Desktop Mode.')"

# ==== 2. usbmuxd と libimobiledevice ====
while :; do
    step 2 "$(M 'usbmuxd と libimobiledevice' 'usbmuxd and libimobiledevice')"
    HAVE_MUXD=$(command -v usbmuxd || ls /usr/sbin/usbmuxd /usr/bin/usbmuxd /sbin/usbmuxd 2> /dev/null | head -1)
    HAVE_PAIR=$(command -v idevicepair || true)
    if [ -n "$HAVE_MUXD" ] || probe mux > /dev/null; then
        ok "$(M 'usbmuxd がインストールされています。' 'usbmuxd is installed.')"
        if [ -n "$HAVE_PAIR" ]; then
            ok "$(M 'libimobiledevice のツールがインストールされています。' 'The libimobiledevice tools are installed.')"
        else
            warn "$(M 'libimobiledevice のツール idevicepair がありません。ペアリングに必要になることがあります。' 'The libimobiledevice tool idevicepair is missing. It may be needed for pairing.')"
            install_hint
        fi
        break
    fi
    ng "$(M 'usbmuxd がインストールされていません。Linux が USB で iPhone と通信するために必要です。' 'usbmuxd is not installed. Linux needs it to talk to the iPhone over USB.')"
    install_hint
    ask_retry || stop_here
done

# ==== 3. usbmuxd が動いているか ====
while :; do
    step 3 "$(M 'usbmuxd の実行' 'usbmuxd running')"
    MUX=$(probe mux)
    if [ "${MUX#MUX OK}" != "$MUX" ]; then
        ok "$(M 'usbmuxd が実行されています：' 'usbmuxd is running: ')${MUX#MUX OK }"
        break
    fi
    LOG+=("      $MUX")
    USB=$(probe usb | tail -1)
    ng "$(M 'usbmuxd に接続できません。' 'Cannot connect to usbmuxd.')"
    case "$MUX" in
    *denied)
        info "$(M 'usbmuxd のソケットへのアクセスが拒否されました。PC を再起動してください。' 'Access to the usbmuxd socket was denied. Restart the PC.')"
        ;;
    *)
        if [ "${USB#USB }" = 0 ]; then
            info "$(M 'iPhone を USB ケーブルで接続して、ロックを解除してください。多くのディストリビューションでは、iPhone を接続したときに usbmuxd が起動します。' 'Plug the iPhone in with USB and unlock it. On most distributions usbmuxd starts when an iPhone is plugged in.')"
        else
            info "$(M 'iPhone は USB で接続されていますが、usbmuxd が起動していません。次のコマンドで起動してください：' 'The iPhone is plugged in, but usbmuxd is not running. Start it with:')"
            cmdline "sudo systemctl restart usbmuxd"
            info "$(M 'それでも起動しないときは、ケーブルを抜いて接続し直してください。' 'If it still does not start, unplug the cable and plug it in again.')"
        fi
        ;;
    esac
    ask_retry || stop_here
done

# ==== 4. iPhone が USB で見えるか ====
while :; do
    step 4 "$(M 'iPhone が USB で認識されるか' 'iPhone over USB')"
    DEVS=$(probe devices)
    USBDEV=$(printf '%s\n' "$DEVS" | awk '$1 == "DEV" && $3 == "USB" {print; exit}')
    if [ -n "$USBDEV" ]; then
        DEV=$(printf '%s' "$USBDEV" | awk '{print $2}')
        ok "$(M 'iPhone が USB で認識されています。' 'The iPhone is connected over USB.')"
        [ "$(printf '%s\n' "$DEVS" | awk '$1 == "DEV" && $3 == "USB"' | wc -l)" -gt 1 ] &&
            warn "$(M 'Apple 製のデバイスが 2 台以上接続されています。最初の 1 台を使用します。' 'More than one Apple device is connected. The first one is used.')"
        LOG+=("      $USBDEV")
        SPEED=$(probe usb | awk '$1 == "USBDEV" {print $3; exit}')
        case "$SPEED" in
        480) info "$(M 'USB 2 で接続されています。USB 3 に対応した USB-C ケーブルと PC の USB 3 ポートを使うと高速になります。' 'Connected at USB 2 speed. A USB-C cable that supports USB 3, in a USB 3 port, is faster.')" ;;
        5000 | 10000 | 20000) info "$(M "USB 3 で接続されています（${SPEED} Mbps）。" "Connected at USB 3 speed, ${SPEED} Mbps.")" ;;
        esac
        break
    fi
    ng "$(M 'iPhone が USB で認識されていません。' 'The iPhone is not seen over USB.')"
    [ -f "$KIT/start-android.sh" ] && info "$(M 'Android のスマホを使うときは、./start-android.sh を実行してください。' 'For an Android phone, run ./start-android.sh.')"
    printf '%s\n' "$DEVS" | grep -q ' Network ' && info "$(M 'Wi-Fi 経由でのみ認識されています。USB ケーブルで接続してください。' 'It is only seen over Wi-Fi. Connect it with a USB cable.')"
    if [ "$(probe usb | tail -1)" != "USB 0" ]; then
        info "$(M 'Linux は iPhone を認識していますが、usbmuxd はまだ認識していません。' 'Linux sees the iPhone, but usbmuxd does not see it yet.')"
        info "$(M 'iPhone のロックを解除してください。「このコンピュータを信頼しますか？」と表示されたら「信頼」をタップしてください。' 'Unlock the iPhone. If it asks "Trust This Computer?", tap Trust.')"
    else
        info "$(M 'iPhone を USB ケーブルで接続して、ロックを解除してください。充電専用のケーブルでは接続できません。' 'Plug the iPhone in with USB and unlock it. A charge-only cable does not work.')"
    fi
    ask_retry || stop_here
done

# ==== 5. ペアリング ====
while :; do
    step 5 "$(M 'ペアリング' 'Pairing')"
    PAIR=$(probe pair "$DEV")
    if [ "$PAIR" = "PAIR OK" ]; then
        ok "$(M 'この PC と iPhone はペアリングされています。' 'This PC and the iPhone are paired.')"
        break
    fi
    LOG+=("      $PAIR")
    ng "$(M 'この PC と iPhone はまだペアリングされていません。' 'This PC and the iPhone are not paired yet.')"
    info "$(M 'iPhone のロックを解除して、次のコマンドを実行してください。iPhone に「このコンピュータを信頼しますか？」と表示されたら「信頼」をタップして、パスコードを入力してください。' 'Unlock the iPhone and run the command below. When the iPhone asks "Trust This Computer?", tap Trust and enter the passcode.')"
    cmdline "idevicepair pair"
    if [ -n "$(command -v idevicepair)" ] && ask_yes "$(M 'いま idevicepair pair を実行しますか？' 'Run idevicepair pair now?')"; then
        while IFS= read -r l; do info "$l"; done < <(idevicepair pair 2>&1)
        continue
    fi
    [ -z "$(command -v idevicepair)" ] && install_hint
    ask_retry || stop_here
done

# ==== 6. iPhone のアプリが答えるか ====
while :; do
    step 6 "$(M 'iPhone のアプリ' 'iPhone app')"
    H=$(probe hello "$DEV" "$PROTO" 3000)
    if [ "${H#HELLO OK}" != "$H" ]; then
        NAME=${H#*name=}
        MB=$(printf '%s' "$H" | sed -n 's/.*mem_mb=\([0-9]*\).*/\1/p')
        MEM=
        [ "${MB:-0}" -gt 0 ] && MEM=$(M "・使用可能なメモリ $(awk "BEGIN{printf \"%.1f\", $MB/1024}") GB" ", memory $(awk "BEGIN{printf \"%.1f\", $MB/1024}") GB")
        ok "$(M 'iPhone のアプリが応答しました：' 'The iPhone app answered: ')$NAME$MEM"
        break
    fi
    LOG+=("      $H")
    case "$H" in
    "HELLO NG timeout"*)
        ng "$(M 'iPhone のアプリは起動していますが、他のアプリが iPhone の GPU を使用しています。' 'The iPhone app is open, but another program is using the iPhone GPU.')"
        info "$(M 'iPhone の GPU は一度に 1 つのアプリしか使用できません。そのアプリを終了するか、iPhone のアプリを終了してから起動し直してください。' 'Only one program at a time can use the iPhone GPU. Close that program, or close and reopen the iPhone app.')"
        ;;
    "HELLO NG result -9"*)
        ng "$(M 'iPhone のアプリとこのキットのバージョンが一致しません。' 'The iPhone app does not match this kit.')"
        info "$(M "このフォルダの RpcServer-${APPVER:-*}.ipa をインストールし直してください。" "Install RpcServer-${APPVER:-*}.ipa from this folder again.")"
        ;;
    "HELLO NG result"*)
        ng "$(M 'iPhone のアプリが拒否しました。コード ' 'The iPhone app refused. Code ')${H#HELLO NG result }"
        ;;
    *)
        ng "$(M 'iPhone のアプリが応答しません。' 'The iPhone app does not answer.')"
        info "$(M 'iPhone で PocketGPU のアプリを起動して、前面に表示したまま、画面をオンにしておいてください。バックグラウンドになると GPU を使用できません。' 'Open the PocketGPU app on the iPhone. Keep it in front with the screen on. In the background it cannot use the GPU.')"
        info "$(M 'アプリに「開始」ボタンが表示されている場合は、タップしてください。' 'If the app shows a Start button, tap it.')"
        info "$(M 'まだインストールしていない場合は、このフォルダの RpcServer の .ipa をインストールしてください。手順は README.txt に書いてあります。' 'If it is not installed yet, install the RpcServer .ipa from this folder. See README.txt.')"
        info "$(M '無料の Apple ID でインストールしたアプリは、7 日で起動できなくなります。その場合は、インストールし直してください。' 'With a free Apple ID the app stops opening after 7 days. Install it again then.')"
        ;;
    esac
    ask_retry || stop_here
done

# ==== 7. USB の速度 ====
while :; do
    step 7 "$(M 'USB の速度' 'USB speed')"
    info "$(M '数秒かかります。' 'This takes a few seconds.')"
    BW=$(probe bw "$DEV" 32)
    if [ "${BW#BW OK}" = "$BW" ]; then
        LOG+=("      $BW")
        if [ "$BW" = "BW NG dropped" ]; then
            warn "$(M '測定中に接続が切れました。次へ進みます。' 'The connection dropped while measuring. Going on.')"
        else
            warn "$(M '測定できませんでした。アプリのバージョンが古い可能性があります。次へ進みます。' 'Could not measure. The app may be old. Going on.')"
        fi
        break
    fi
    DOWN=$(printf '%s' "$BW" | sed -n 's/.*down=\([0-9]*\).*/\1/p')
    UP=$(printf '%s' "$BW" | sed -n 's/.*up=\([0-9]*\).*/\1/p')
    RTT=$(printf '%s' "$BW" | sed -n 's/.*rtt_ms=\([0-9.]*\).*/\1/p')
    LINE=$(M "iPhone→PC ${DOWN} MB/s・PC→iPhone ${UP} MB/s・往復時間 ${RTT} ms" "iPhone->PC ${DOWN} MB/s, PC->iPhone ${UP} MB/s, round trip ${RTT} ms")
    if [ "${DOWN:-0}" -lt 45 ] || [ "${UP:-0}" -lt 35 ]; then
        warn "$LINE"
        info "$(M 'USB 2 の速度です。負荷の高いゲームは遅くなります。' 'This is USB 2 speed. Heavy games will be slow.')"
        info "$(M 'USB 3 に対応した USB-C ケーブルを使い、PC の USB-C ポートまたは USB 3 ポートに接続してください。iPhone に付属のケーブルは USB 2 です。' 'Use a USB-C cable that supports USB 3, and plug it into a USB-C or USB 3 port on the PC. The cable that came with the iPhone is USB 2.')"
        info "$(M 'USB 3 で高速になるのは、iPhone 15 Pro 以降の Pro モデルだけです。' 'Only iPhone 15 Pro and later Pro models get faster with USB 3.')"
        [ "$NOPROMPT" = 1 ] && break
        read -r -p "      $(M 'ケーブルかポートを変更したら、Enter キーを押すと再測定します。このまま進むときは n を入力して Enter キーを押してください。' 'After changing the cable or port, press Enter to measure again. To go on as is, type n and Enter.') " a < /dev/tty || break
        LOG+=("      -> $a")
        RETRIES=$((RETRIES + 1))
        { [ "$a" = n ] || [ $RETRIES -gt 50 ]; } && break
        continue
    fi
    ok "$LINE"
    [ "${DOWN:-0}" -lt 100 ] && info "$(M '使用できる速度です。USB 3 のケーブルとポートを使うと、さらに高速になることがあります。' 'This speed works. A USB 3 cable and port may be faster.')"
    break
done

# ==== 8. Vulkan のローダーとドライバ ====
step 8 "$(M 'ドライバ経由で iPhone の GPU を認識できるか' 'iPhone GPU through the driver')"
LD=$(probe loader)
if [ "${LD#LOADER OK}" = "$LD" ]; then
    ng "$(M 'Vulkan のローダー libvulkan.so.1 がありません。' 'The Vulkan loader libvulkan.so.1 is missing.')"
    install_hint
    stop_here
fi
LV=$(printf '%s' "$LD" | awk '{print $3}')
ok "$(M "Vulkan のローダー $LV" "Vulkan loader $LV")"
LOG+=("      $LD")
# VK_DRIVER_FILES と VK_ADD_DRIVER_FILES は 1.3.207 から。それより古いと VK_ICD_FILENAMES だけが効く（iPhone の GPU だけを見せる標準の設定では問題ない）
if [ "$(printf '%s\n1.3.207\n' "$LV" | sort -V | head -1)" != 1.3.207 ]; then
    warn "$(M 'Vulkan のローダーが古いため、IPHONE_VK_ONLY=0 で PC の GPU と一緒に表示する設定は使用できません。標準の設定では問題ありません。' 'The Vulkan loader is old, so IPHONE_VK_ONLY=0, which shows the PC GPUs too, does not work. The default setting is fine.')"
fi
VK=$(
    eval "$("$RUN" --env)"
    export VK_LOADER_LAYERS_DISABLE='~implicit~' # チェックのときだけ、ほかの層（Steam のオーバーレイなど）を読まない
    timeout 30 "$PROBE" vklist 2>&1
)
RC=$?
# PC の GPU の vendor（Intel 0x8086・NVIDIA 0x10de・AMD 0x1002・Microsoft 0x1414・Mesa の CPU 0x10005）は数えない。
# 古いローダーで環境変数が効かず、PC の GPU だけが見えたときに「見えた」としないため
GPUS=$(printf '%s\n' "$VK" | grep -E '^GPU[0-9]+: ' | grep -v -E 'vendor 0x(8086|10de|1002|1414|10005),' || true)
PCONLY=$(printf '%s\n' "$VK" | grep -E '^GPU[0-9]+: ' | grep -E 'vendor 0x(8086|10de|1002|1414|10005),' || true)
if [ -z "$GPUS" ] && [ -n "$PCONLY" ]; then
    ng "$(M 'PC の GPU だけが見えて、iPhone の GPU が見えません。' 'Only the PC GPU is seen, not the iPhone GPU.')"
    while IFS= read -r l; do info "${l#GPU*: }"; done <<< "$PCONLY"
    stop_here
fi
if [ -n "$GPUS" ]; then
    while IFS= read -r g; do
        g=${g#GPU*: }
        ok "$(M 'ドライバ経由で認識できます：' 'Seen through the driver: ')${g%%  (vendor*}"
    done <<< "$GPUS"
else
    ng "$(M 'ドライバ経由で iPhone の GPU を認識できません。' 'The driver does not see the iPhone GPU.')"
    if [ $RC = 124 ]; then
        info "$(M '30 秒以内に完了しませんでした。iPhone のアプリを終了してから、起動し直してください。' 'It did not finish in 30 seconds. Close and reopen the iPhone app.')"
    else
        while IFS= read -r l; do info "$l"; done < <(printf '%s\n' "$VK" | grep -v '^$' | tail -5)
    fi
    stop_here
fi

# ==== 使い方 ====
STEAMLINE="\"$RUN\" %command%"
out ""
out "$(M '準備ができました。' 'Ready.')" "$C_OK"
out ""
out "$(M 'iPhone の GPU で起動するには' 'To run something on the iPhone GPU')"
out "$(M '・ターミナルで次のように実行します：' '- In a terminal, run:')"
out "    \"$RUN\" vkcube"
out "$(M '・Steam のゲームは、ゲームのプロパティの「起動オプション」に次の 1 行を入力します。Proton のゲームもこのままで動作します。' "- For a Steam game, put this line in the game's Properties > Launch Options. Proton games work the same way:")"
out "    $STEAMLINE"
if command -v flatpak > /dev/null 2>&1 && flatpak info com.valvesoftware.Steam > /dev/null 2>&1; then
    out "$(M '・Flatpak 版の Steam がインストールされています。Flatpak 版で使うには、次の 1 回だけの設定が必要です：' '- The Flatpak Steam is installed. To use it there, set this once:')"
    out "    flatpak override --user --filesystem=\"$KIT\":ro com.valvesoftware.Steam"
    out "$(M '  さらに、ゲームを起動する前に別のターミナルで次を実行して、開いたままにしてください：' '  Also, before starting the game, run this in another terminal and keep it open:')"
    out "    \"$RUN\" --broker"
fi
if [ -e /snap/steam/current ]; then
    out "$(M '・Snap 版の Steam がインストールされています。Snap 版では、ゲームから iPhone の USB に接続できない可能性が高く、まだ確認していません。Valve の .deb か、ディストリビューションのパッケージの Steam を使ってください。' '- The Snap Steam is installed. From the Snap Steam, games can most likely not reach the iPhone over USB. This has not been checked yet. Use the .deb from Valve or the Steam package of your distribution.')"
fi
[ "$FAMILY" = steamos ] && out "$(M '・ゲームモードでも動作する見込みですが、まだ確認していません。まずデスクトップモードで試してください。' '- Game Mode is expected to work but has not been checked yet. Try Desktop Mode first.')"
out "$(M '・アンチチートを導入しているゲームでは使用しないでください。アカウントが停止されることがあります。' '- Do not use games with anti-cheat. Your account may be banned.')"
out "$(M '・使用中は、iPhone のアプリを前面に表示したまま、画面をオンにしておいてください。' '- While you use it, keep the iPhone app in front with the screen on.')"
if [ -x "$KIT/start-mic-camera.sh" ] || [ -f "$KIT/start-mic-camera.sh" ]; then
    out "$(M '・iPhone をマイクとカメラとして使うには、./start-mic-camera.sh を実行します。GPU を使っていても、使っていなくても使えます。' '- To use the iPhone as a microphone and a camera, run ./start-mic-camera.sh. It works with or without the GPU.')"
fi
out ""
CLIP=
command -v wl-copy > /dev/null 2>&1 && [ -n "${WAYLAND_DISPLAY:-}" ] && CLIP="wl-copy"
[ -z "$CLIP" ] && command -v xclip > /dev/null 2>&1 && [ -n "${DISPLAY:-}" ] && CLIP="xclip -selection clipboard"
[ -z "$CLIP" ] && command -v xsel > /dev/null 2>&1 && [ -n "${DISPLAY:-}" ] && CLIP="xsel --clipboard --input"
if [ -n "$CLIP" ] && ask_yes "$(M 'Steam の起動オプションの 1 行をコピーしますか？' 'Copy the Steam launch option line?')"; then
    printf '%s' "$STEAMLINE" | $CLIP && ok "$(M 'コピーしました。' 'Copied.')"
fi
[ "$ALLOK" = 1 ] && finish 0
finish 2
