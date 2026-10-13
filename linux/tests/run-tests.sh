#!/usr/bin/env bash
# fix.sh（Linux）の試験。PC の中だけで、わざと壊したキットと偽の道具（usbmuxd・adb・pgpu-probe・vulkaninfo・pactl など）で場面を作る。
#   bash tests/run-tests.sh [--scenario a,b] [--keep] [--real-probe パス] [--android-src フォルダ]
#   --real-probe パス     本物の bin/pgpu-probe（キットの物）。iPhone の場面を、偽の usbmuxd（fakes/fake_usbmuxd.py）と本物の道具でも動かす
#   --android-src フォルダ  本物の start-android.sh と android/common.sh があるフォルダ。偽の adb の上で、fix.sh から本物を呼ぶ場面を足す
# 必要な物：bash 4.4 以上・python3・coreutils・iproute2 の ss。本物の iPhone・Android・usbmuxd・GPU・インターネットは使わない。sudo は使わない。
#
# 1 つの場面ごとに 3 通りで動かす（Windows の tests/run-tests.ps1 と同じ考え方）：
#   fix       直すことに y と答える。直ったこと（効果）を、画面の文ではなく、ファイル・プロセス・ポート・偽の道具の状態で確かめる
#   decline   直すことに n と答える。何も変わらないこと（効果が出ない・何も消えていない）を確かめる
#   sabotage  直す動作（fix.sh の act_*）だけを何もしない物に差し替えた fix.sh を、fix と同じ答えで動かす。効果の確認が落ちることを確かめる
#             （直す部分が本当に効いている証拠。fix.sh が「直った」と嘘を言わないことも、終わりのコードで確かめる）
# 案内だけの場面（static）は 1 回だけ動かして、出す案内と「sudo を実行しない」ことを確かめる。
# 守ること：止めるのは、この試験が自分で起こしたプロセスだけ（PID を控えて止める）。試験の作業場所（mktemp）の外には書かない。
set -u
T=$(cd "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
SRC_LINUX=$(cd "$T/.." && pwd -P)
SCEN_FILTER=
KEEP=0
REAL_PROBE=
ANDROID_SRC=
while [ $# -gt 0 ]; do
    case "$1" in
    --scenario) SCEN_FILTER=$2; shift ;;
    --keep) KEEP=1 ;;
    --real-probe) REAL_PROBE=$(readlink -f -- "$2"); shift ;;
    --android-src) ANDROID_SRC=$(readlink -f -- "$2"); shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
for c in python3 ss od sha256sum timeout setsid; do command -v "$c" > /dev/null || { echo "needed: $c" >&2; exit 2; }; done
WORK=$(mktemp -d "${TMPDIR:-/tmp}/pgpu-fix-tests.XXXXXX")
# 台本と偽の道具は、作業場所に写してから使う（Windows のドライブの上だと、実行の権限や行末が変わることがあるため）
mkdir -p "$WORK/src"
cp -r "$SRC_LINUX/fix.sh" "$T/fakes" "$WORK/src/"
find "$WORK/src" -type f \( -name '*.sh' -o -name '*.py' -o -name adb -o -name pgpu-probe \) -exec sed -i 's/\r$//' {} +
chmod +x "$WORK/src/fix.sh" "$WORK/src/fakes/"*
FIX="$WORK/src/fix.sh"
FAKES="$WORK/src/fakes"
# sabotage：act_* を全部「何もしないで成功を返す」物に差し替えた写し
SAB="$WORK/src/fix-sabotage.sh"
mapfile -t ACTS < <(grep -o '^act_[a-z0-9_]*()' "$FIX" | sed 's/()$//')
awk -v names="${ACTS[*]}" '{print} /^# ---- .*\(act_\*\) ----$/ {n = split(names, a, " "); for (i = 1; i <= n; i++) print a[i] "() { return 0; }"}' "$FIX" > "$SAB"
[ "$(grep -c '() { return 0; }' "$SAB")" -eq "${#ACTS[@]}" ] && [ "${#ACTS[@]}" -ge 8 ] || { echo "could not make the sabotage copy (${#ACTS[@]} act_ functions)" >&2; exit 2; }
TUSER=$(id -un)
THOST=$(cat /proc/sys/kernel/hostname 2> /dev/null || hostname)

# ---- 結果 ----
declare -A EFF_N EFF_BAD SAFE_BAD
SCENS_RUN=()
FAILS=()
check() { # check effect|safety "名前" コマンド...
    local kind=$1 name=$2 v=0
    shift 2
    if "$@" > /dev/null 2>&1; then v=1; fi
    local key="$SCEN|$MODE"
    if [ "$kind" = effect ]; then
        EFF_N[$key]=$((${EFF_N[$key]:-0} + 1))
        [ $v = 0 ] && EFF_BAD[$key]=$((${EFF_BAD[$key]:-0} + 1))
    else
        [ $v = 0 ] && SAFE_BAD[$key]=$((${SAFE_BAD[$key]:-0} + 1))
    fi
    printf '    %-7s %-6s %s %s\n' "$MODE" "$kind" "$([ $v = 1 ] && echo 'true ' || echo 'false')" "$name"
    case "$MODE:$kind:$v" in
    fix:*:0 | static:*:0 | *:safety:0) FAILS+=("$SCEN/$MODE: $name") ;;
    esac
}
not() { ! "$@"; }

# ---- 場面の用意 ----
SPAWNED=()
spawn() { # 自分で起こしたプロセス。PID を LASTPID と SPAWNED に
    "$@" < /dev/null > /dev/null 2>&1 &
    LASTPID=$!
    SPAWNED+=("$LASTPID")
}
alive() { [ -d "/proc/$1" ] && ! grep -q '^State:[[:space:]]*Z' "/proc/$1/status" 2> /dev/null; }
dead() { ! alive "$1"; }
cleanup_scene() {
    local p f
    for p in "${SPAWNED[@]}"; do alive "$p" && kill "$p" 2> /dev/null; done
    for f in "$FAKE_DIR"/adb/listen.*; do [ -f "$f" ] && { p=$(cat "$f"); alive "$p" && kill "$p" 2> /dev/null; }; done
    for p in "${SPAWNED[@]}"; do wait "$p" 2> /dev/null; done
    SPAWNED=()
}
USED_PORTS=" "
listening() {
    local hex
    hex=$(printf '%04X' "$1")
    awk -v p="$hex" '$4 == "0A" && toupper(substr($2, length($2) - 3)) == p {f = 1} END {exit !f}' /proc/net/tcp /proc/net/tcp6 2> /dev/null
}
free_port() {
    local p
    for _ in $(seq 1 400); do
        p=$((38100 + RANDOM % 9800))
        case "$USED_PORTS" in *" $p "*) continue ;; esac
        listening "$p" && continue
        USED_PORTS+="$p "
        echo "$p"
        return
    done
    echo "no free port" >&2
    exit 2
}
fake_elf() { # $1 = 1（32 ビット）か 2（64 ビット）、$2 = e_machine の 8 進（076 は x86_64・003 は i386・267 は aarch64）
    printf "\\177ELF\\00$1\\001\\001\\000\\000\\000\\000\\000\\000\\000\\000\\000\\003\\000\\$2\\000"
    head -c 44 /dev/zero
}
json_text() {
    printf '{\n    "file_format_version": "1.0.1",\n    "ICD": {\n        "library_path": "./libiphone_icd.so",\n        "library_arch": "%s",\n        "api_version": "1.2.0"\n    }\n}\n' "$1"
}
make_kit() {
    local K=$1
    mkdir -p "$K/bin" "$K/lib/x86_64" "$K/lib/i386" "$K/tools" "$K/android"
    fake_elf 2 076 > "$K/bin/pgpu-probe"
    fake_elf 2 076 > "$K/lib/x86_64/libiphone_icd.so"
    json_text 64 > "$K/lib/x86_64/iphone_icd.x86_64.json"
    fake_elf 1 003 > "$K/lib/i386/libiphone_icd.so"
    json_text 32 > "$K/lib/i386/iphone_icd.i386.json"
    fake_elf 2 076 > "$K/bin/webcam_rx"
    fake_elf 2 076 > "$K/bin/ipaudio"
    fake_elf 2 267 > "$K/android/server_aarch64"
    printf 'PK\003\004fake apk\n' > "$K/android/pocketgpu.apk"
    printf 'proto=3\napp=0.7.41\nplatform=linux-x86_64\n' > "$K/tools/kit.ini"
    cat > "$K/start-pocketgpu.sh" << 'EOF'
#!/usr/bin/env bash
# fake start-pocketgpu.sh for the tests
echo "start"
EOF
    cat > "$K/run-on-iphone-gpu.sh" << 'EOF'
#!/usr/bin/env bash
# fake run-on-iphone-gpu.sh for the tests: only --env, in the same form as the real one
KIT=$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")
J="$KIT/lib/x86_64/iphone_icd.x86_64.json:$KIT/lib/i386/iphone_icd.i386.json"
if [ "${1:-}" = --env ]; then
    printf 'export IPHONE_VK=%q\n' "${IPHONE_VK:-usb}"
    printf 'export VK_DRIVER_FILES=%q\n' "$J"
    printf 'export VK_ICD_FILENAMES=%q\n' "$J" # ICD-LINE
    exit 0
fi
exec "$@"
EOF
    cat > "$K/start-mic-camera.sh" << 'EOF'
#!/usr/bin/env bash
# fake start-mic-camera.sh for the tests: a child subshell like the real one, stops on TERM
( while :; do sleep 1; done ) &
child=$!
trap 'kill $child 2>/dev/null; exit 0' TERM INT HUP
wait
EOF
    cp "$K/start-mic-camera.sh" "$K/start-android-mic-camera.sh"
    cp "$FAKES/kit-start-android.sh" "$K/start-android.sh"
    printf '#!/usr/bin/env bash\necho stop\n' > "$K/stop-android.sh"
    printf '#!/usr/bin/env bash\necho check\n' > "$K/check-android.sh"
    cat > "$K/run-on-android-gpu.sh" << 'EOF'
#!/usr/bin/env bash
KIT=$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")
export IPHONE_VK="127.0.0.1:${PGPU_PORT:-50300}"
exec "$KIT/run-on-iphone-gpu.sh" "$@"
EOF
    printf '# fake common.sh\n: common\n' > "$K/android/common.sh"
    chmod 755 "$K"/*.sh "$K/bin/"*
}
link_tools() { local n; for n in "$@"; do ln -sf "$FAKES/small-tools.sh" "$BIN/$n"; done; }
new_scene() {
    S_DIR="$WORK/$SCEN-$MODE"
    rm -rf "$S_DIR"
    mkdir -p "$S_DIR"
    FAKE_DIR="$S_DIR/fake"
    KIT="$S_DIR/PocketGPU-linux-test"
    BIN="$S_DIR/bin"
    HOMED="$S_DIR/home"
    UDEV="$S_DIR/udev"
    SBIN="$S_DIR/sbin"
    SYSD="$S_DIR/sys"
    DEVD="$S_DIR/dev"
    XDG="$S_DIR/run"
    SYSUSB="$S_DIR/sysusb"
    OSREL="$S_DIR/os-release"
    mkdir -p "$FAKE_DIR/adb" "$FAKE_DIR/pactl" "$BIN" "$HOMED" "$UDEV" "$SBIN" "$SYSD" "$DEVD" "$XDG" "$SYSUSB"
    printf 'NAME="Ubuntu"\nPRETTY_NAME="Ubuntu 24.04 LTS (fake)"\nID=ubuntu\nID_LIKE=debian\n' > "$OSREL"
    make_kit "$KIT"
    ln -sf "$FAKES/adb" "$BIN/adb"
    link_tools usbmuxd idevicepair systemctl pactl id sudo modinfo vulkaninfo
    P=$(free_port)
    CAM=$(free_port)
    AUD=$(free_port)
    PROBE_FOR_TEST="$FAKES/pgpu-probe"
    EXTRA_ENV=()
    STOP_ALLOW=0
    ALLOWED_GONE=()
    TLANG=en
    ONLY_ARGS=()
}
snapshot() { (cd "$KIT" && find . -type f ! -path './fix-backup/*' ! -name 'pocketgpu-fix-*' | sort) > "$S_DIR/files-before.txt"; }
run_fix() { # run_fix 項目,項目 [引数...]
    local only=$1 fx=$FIX ans
    shift
    [ "$MODE" = sabotage ] && fx=$SAB
    case "$MODE" in fix | sabotage) ans=y,y,y,y,y,y,y,y,y,y ;; decline) ans=n,n,n,n,n,n,n,n,n,n ;; static) ans=${STATIC_ANS:-n,n,n,n} ;; esac
    [ -f "$S_DIR/files-before.txt" ] || snapshot
    OUTF="$S_DIR/out.txt"
    env -i HOME="$HOMED" USER="$TUSER" LOGNAME="$TUSER" PATH="$BIN:/usr/bin:/bin" LANG=C.UTF-8 \
        PGPU_LANG="$TLANG" PGPU_NOPROMPT=1 PGPU_ANSWERS="$ans" FAKE_DIR="$FAKE_DIR" \
        PGPU_TEST_PROBE="$PROBE_FOR_TEST" PGPU_UDEV_DIRS="$UDEV" PGPU_SBIN="$SBIN" PGPU_SYS="$SYSD" PGPU_DEV="$DEVD" \
        PGPU_OS_RELEASE="$OSREL" XDG_RUNTIME_DIR="$XDG" PGPU_TEST_STOP_ALLOW="$STOP_ALLOW" PGPU_SYS_USB="$SYSUSB" \
        PGPU_PORT="$P" PGPU_CAM_PORT="$CAM" PGPU_AUDIO_PORT="$AUD" "${EXTRA_ENV[@]}" \
        timeout 300 bash "$fx" --kit "$KIT" --only "$only" "$@" > "$OUTF" 2>&1
    RC=$?
    out_has() { grep -qF -- "$1" "$OUTF"; }
}
# 全部の場面で見る安全の確認：sudo を実行していない・何も消していない・まとめとログに秘密や個人の物が無い
leak_free() {
    local f s
    for f in "$KIT/pocketgpu-fix-summary.txt" "$KIT/pocketgpu-fix-log.txt"; do
        [ -f "$f" ] || continue
        for s in "$KIT" "$HOMED" "FAKESERIAL123" "00008110-001234567890ABCD" ${TOKEN_VALUE:+"$TOKEN_VALUE"}; do
            grep -qF -- "$s" "$f" && return 1
        done
        [ ${#TUSER} -ge 2 ] && grep -qF -- "$TUSER" "$f" && return 1
        [ ${#THOST} -ge 2 ] && grep -qF -- "$THOST" "$f" && return 1
    done
    return 0
}
nothing_deleted() {
    local gone g ok
    (cd "$KIT" && find . -type f ! -path './fix-backup/*' ! -name 'pocketgpu-fix-*' | sort) > "$S_DIR/files-after.txt"
    while IFS= read -r gone; do
        ok=0
        for g in "${ALLOWED_GONE[@]}"; do [ "$gone" = "./$g" ] && ok=1; done
        [ $ok = 1 ] || return 1
    done < <(comm -23 "$S_DIR/files-before.txt" "$S_DIR/files-after.txt")
}
common_checks() {
    check safety "sudo を実行していない" not test -e "$FAKE_DIR/sudo.log"
    check safety "キットのファイルを消していない" nothing_deleted
    check safety "まとめとログに、場所・ユーザー名・PC の名前・シリアル・UDID・合言葉が無い" leak_free
    check safety "bash のエラーが出ていない" not grep -qE 'unbound variable|syntax error|command not found|bad substitution|bad array subscript' "$OUTF"
    case "$MODE" in
    fix) check effect "終わりのコードが 0（直った）" test "$RC" = 0 ;;
    decline | sabotage) check safety "終わりのコードが 1（直ったと言わない）" test "$RC" = 1 ;;
    esac
}
sha() { sha256sum -- "$1" 2> /dev/null | awk '{print $1}'; }
no_cr() { ! grep -q $'\r' -- "$1"; }
json_good() { python3 -I -c 'import json,sys; j=json.load(open(sys.argv[1])); assert j["ICD"]["library_path"]=="./libiphone_icd.so"' "$1"; }
fwd_of() { awk -v p="tcp:$1" '$2 == p {print $3; exit}' "$FAKE_DIR/adb/forwards" 2> /dev/null; }
fwd_is() { [ "$(fwd_of "$1")" = "$2" ]; }
no_fwd() { [ -z "$(fwd_of "$1")" ]; }
adb_listens() { [ -f "$FAKE_DIR/adb/listen.$1" ] && alive "$(cat "$FAKE_DIR/adb/listen.$1")" && listening "$1"; }
backup_file() { ls -d "$KIT"/fix-backup/*/"$1" 2> /dev/null | head -n 1; }
no_backup() { [ ! -d "$KIT/fix-backup" ]; }
foreign_listener() { # adb ではない名前で、ポートを開けて待つ（python3）
    spawn python3 -c 'import socket,sys,time
s=socket.socket(); s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(4)
while True: time.sleep(1)' "$1"
    for _ in $(seq 1 60); do listening "$1" && break; sleep 0.05; done
}
holder() { # ファイルを読み込んだ（mmap した）まま待つ
    spawn python3 -c 'import mmap,sys,time
f=open(sys.argv[1],"rb"); m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
while True: time.sleep(1)' "$1"
    for _ in $(seq 1 60); do grep -qF "$1" "/proc/$LASTPID/maps" 2> /dev/null && break; sleep 0.05; done
}
android_ready() { # スマホ 1 台・キットの受け手が 1 つ・ポートはつながっている
    printf 'FAKESERIAL123\tdevice\n' > "$FAKE_DIR/adb/devices"
    echo 4242 > "$FAKE_DIR/adb/pids"
    sha "$KIT/android/server_aarch64" > "$FAKE_DIR/adb/hash"
}
reset_fwd() { cleanup_scene; rm -f "$FAKE_DIR"/adb/listen.*; : > "$FAKE_DIR/adb/forwards"; }
adb_fwd() { FAKE_DIR="$FAKE_DIR" ANDROID_SERIAL=FAKESERIAL123 "$FAKES/adb" forward "tcp:$1" "$2" > /dev/null; }

# =====================================================================
# 場面。sc_<名前> が 1 つの場面。MODE は fix・decline・sabotage・static
# =====================================================================

# ---- files：行末が CRLF になった台本 ----
sc_files_crlf() {
    new_scene
    sed -i 's/$/\r/' "$KIT/start-pocketgpu.sh" "$KIT/run-on-iphone-gpu.sh"
    local before
    before=$(sha "$KIT/run-on-iphone-gpu.sh")
    run_fix files
    check effect "start-pocketgpu.sh の行末が LF になった" no_cr "$KIT/start-pocketgpu.sh"
    check effect "run-on-iphone-gpu.sh の行末が LF になった" no_cr "$KIT/run-on-iphone-gpu.sh"
    check effect "直したあとも実行できる" test -x "$KIT/run-on-iphone-gpu.sh"
    [ "$MODE" = fix ] && check safety "前の物（CRLF のまま）が fix-backup に残っている" test "$(sha "$(backup_file run-on-iphone-gpu.sh)")" = "$before"
    [ "$MODE" = decline ] && check safety "断ったので中身は同じ・fix-backup も作らない" test "$(sha "$KIT/run-on-iphone-gpu.sh")" = "$before" -a ! -d "$KIT/fix-backup"
    common_checks
}
# ---- files：実行の権限が無い ----
sc_files_exec() {
    new_scene
    chmod -x "$KIT/run-on-iphone-gpu.sh" "$KIT/bin/webcam_rx"
    run_fix files
    check effect "run-on-iphone-gpu.sh を実行できる" test -x "$KIT/run-on-iphone-gpu.sh"
    check effect "bin/webcam_rx を実行できる" test -x "$KIT/bin/webcam_rx"
    [ "$MODE" = fix ] && check safety "前の権限を fix-backup に書いた" grep -q 'run-on-iphone-gpu.sh' "$(ls "$KIT"/fix-backup/*/permissions-before.txt 2> /dev/null | head -n 1)"
    [ "$MODE" = decline ] && check safety "断ったので権限は無いまま" test ! -x "$KIT/run-on-iphone-gpu.sh"
    common_checks
}
# ---- files：ドライバの json が壊れた ----
sc_files_json() {
    new_scene
    printf '{ "file_format_version": "1.0.1", "ICD": { "library_path": "./libiphone_icd.so", "library_arch": "64", "api_version": "1.2.0" \n' > "$KIT/lib/x86_64/iphone_icd.x86_64.json"
    local before
    before=$(sha "$KIT/lib/x86_64/iphone_icd.x86_64.json")
    run_fix files
    check effect "json が読めて、横の .so を指す" json_good "$KIT/lib/x86_64/iphone_icd.x86_64.json"
    [ "$MODE" = fix ] && check safety "壊れた前の json が fix-backup に残っている" test "$(sha "$(backup_file lib/x86_64/iphone_icd.x86_64.json)")" = "$before"
    [ "$MODE" = decline ] && check safety "断ったので json は同じ" test "$(sha "$KIT/lib/x86_64/iphone_icd.x86_64.json")" = "$before"
    common_checks
}
# ---- files：ドライバの .so が無い（このツールでは直せない） ----
sc_files_missing() {
    new_scene
    rm "$KIT/lib/x86_64/libiphone_icd.so"
    local before
    before=$(sha "$KIT/lib/x86_64/iphone_icd.x86_64.json")
    snapshot
    STATIC_ANS=y,y,y,y
    run_fix files
    check effect "終わりのコードが 1" test "$RC" = 1
    check effect "展開し直す案内（tar xzf）を出した" out_has "tar xzf"
    check effect "無いファイルの名前を出した" out_has "lib/x86_64/libiphone_icd.so"
    check safety "y と答えても、.so が無いので json は作り直さない" test "$(sha "$KIT/lib/x86_64/iphone_icd.x86_64.json")" = "$before"
    common_checks
}
# ---- tools：受け渡し役が動いていないのに、合言葉のファイルが残っている ----
sc_tools_token() {
    new_scene
    TOKEN_VALUE=$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')
    (umask 077 && printf '%s\n' "$TOKEN_VALUE" > "$KIT/tools/broker-token")
    ALLOWED_GONE=(tools/broker-token)
    run_fix tools
    check effect "キットの tools/broker-token が無くなった" test ! -e "$KIT/tools/broker-token"
    [ "$MODE" = fix ] && check safety "合言葉のファイルは消さずに fix-backup に移した（中身も同じ）" grep -qF "$TOKEN_VALUE" "$(backup_file tools/broker-token)"
    [ "$MODE" = decline ] && check safety "断ったのでファイルはそのまま" grep -qF "$TOKEN_VALUE" "$KIT/tools/broker-token"
    check safety "合言葉を画面に出していない" not grep -qF "$TOKEN_VALUE" "$OUTF"
    common_checks
    TOKEN_VALUE=
}
# ---- tools：合言葉の合わない、古い受け渡し役が残っている ----
sc_tools_old_broker() {
    new_scene
    local old_tok other other_dir
    old_tok=$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')
    TOKEN_VALUE=$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')
    printf '%s\n' "$TOKEN_VALUE" > "$KIT/tools/broker-token"
    ALLOWED_GONE=(tools/broker-token)
    spawn env FAKE_DIR="$FAKE_DIR" PGPU_BROKER_TOKEN="$old_tok" bash "$PROBE_FOR_TEST" broker
    local old=$LASTPID
    # 別のフォルダのキットの受け渡し役（止めてはいけない物）
    other_dir="$S_DIR/other-kit/bin"
    mkdir -p "$other_dir" "$S_DIR/other-fake"
    cp "$PROBE_FOR_TEST" "$other_dir/pgpu-probe"
    spawn env FAKE_DIR="$S_DIR/other-fake" PGPU_BROKER_TOKEN="$old_tok" bash "$other_dir/pgpu-probe" broker
    other=$LASTPID
    for _ in $(seq 1 40); do [ -s "$FAKE_DIR/brokers" ] && break; sleep 0.05; done
    STOP_ALLOW="$old,$other"
    run_fix tools
    check effect "古い受け渡し役が止まった" dead "$old"
    check effect "古い合言葉のファイルがキットから無くなった" test ! -e "$KIT/tools/broker-token"
    check safety "別のフォルダのキットの受け渡し役は動いたまま" alive "$other"
    [ "$MODE" = decline ] && check safety "断ったので古い受け渡し役も動いたまま" alive "$old"
    common_checks
    TOKEN_VALUE=
}
# ---- tools：start-mic-camera.sh が二重に動いている ----
sc_tools_dup_miccam() {
    new_scene
    spawn bash "$KIT/start-mic-camera.sh"
    local older=$LASTPID
    sleep 1.1
    spawn bash "$KIT/start-mic-camera.sh"
    local newer=$LASTPID
    sleep 0.3
    STOP_ALLOW="$older,$newer"
    run_fix tools
    check effect "古い方が止まった" dead "$older"
    check safety "新しい方は動いたまま" alive "$newer"
    [ "$MODE" = decline ] && check safety "断ったので古い方も動いたまま" alive "$older"
    common_checks
}
# ---- iphone：ペアリングされていない ----
sc_iphone_pair() {
    new_scene
    touch "$FAKE_DIR/trust"
    real_probe_scene
    run_fix iphone
    check effect "ペアリングされた" test -e "$FAKE_DIR/paired"
    [ "$MODE" = decline ] && check safety "断ったので idevicepair を実行していない" not grep -q '^idevicepair pair' "$FAKE_DIR/tools.log"
    check safety "UDID を画面に出していない" not grep -qF "00008110-001234567890ABCD" "$OUTF"
    common_checks
}
# ---- iphone：ほかのプログラムが iPhone の GPU を握っている ----
sc_iphone_holder() {
    new_scene
    touch "$FAKE_DIR/paired"
    holder "$KIT/lib/x86_64/libiphone_icd.so"
    local h=$LASTPID
    echo "$h" > "$FAKE_DIR/holders"
    cp "$KIT/lib/x86_64/libiphone_icd.so" "$S_DIR/other-libiphone_icd.so"
    holder "$S_DIR/other-libiphone_icd.so"
    local u=$LASTPID
    STOP_ALLOW="$h,$u"
    real_probe_scene
    run_fix iphone
    check effect "握っていたプログラムが止まった" dead "$h"
    check effect "iPhone のアプリが応答した" out_has "The iPhone app answered"
    check safety "キットの外の .so を読み込んだプログラムは動いたまま" alive "$u"
    [ "$MODE" = decline ] && check safety "断ったので握っていたプログラムも動いたまま" alive "$h"
    common_checks
}
# ---- android：スマホが無いのに adb forward が残っている ----
sc_android_leftover() {
    new_scene
    printf 'FAKESERIAL123\tdevice\n' > "$FAKE_DIR/adb/devices"
    adb_fwd "$P" "tcp:$P"
    adb_fwd "$CAM" "localabstract:pocketgpu-webcam"
    adb_fwd "$AUD" "localabstract:pocketgpu-audio"
    : > "$FAKE_DIR/adb/devices"
    local other
    other=$(free_port)
    foreign_listener "$other"
    local u=$LASTPID
    run_fix android
    check effect "GPU のポートの forward が外れた" no_fwd "$P"
    check effect "カメラのポートの forward が外れた" no_fwd "$CAM"
    check effect "マイクのポートの forward が外れた" no_fwd "$AUD"
    check effect "GPU のポートが閉じた" not listening "$P"
    check safety "関係の無いポートのプログラムは動いたまま" alive "$u"
    common_checks
}
# ---- android：adb を起動し直したあとで、forward はあるがポートが開いていない ----
sc_android_stale() {
    new_scene
    android_ready
    echo "FAKESERIAL123 tcp:$P tcp:$P" > "$FAKE_DIR/adb/forwards"
    run_fix android
    check effect "GPU のポートの forward がある" fwd_is "$P" "tcp:$P"
    check effect "GPU のポートを adb が開けている" adb_listens "$P"
    common_checks
}
# ---- android：GPU のポートを、adb ではないプログラムが使っている ----
sc_android_conflict() {
    new_scene
    android_ready
    foreign_listener "$P"
    local f=$LASTPID
    STOP_ALLOW="$f"
    run_fix android
    check effect "ポートを使っていたプログラムが止まった" dead "$f"
    check effect "GPU のポートを adb が開けている" adb_listens "$P"
    check effect "forward の向け先が tcp:$P" fwd_is "$P" "tcp:$P"
    [ "$MODE" = decline ] && check safety "断ったのでプログラムは動いたまま" alive "$f"
    common_checks
}
# ---- android：受け手が二重で、古い ----
sc_android_oldrecv() {
    new_scene
    android_ready
    echo "1111 2222" > "$FAKE_DIR/adb/pids"
    echo "0000000000000000000000000000000000000000000000000000000000000000" > "$FAKE_DIR/adb/hash" # pgpu-allow-secret（試験の偽の値）
    adb_fwd "$P" "tcp:$P"
    run_fix android
    check effect "スマホの受け手が 1 つになった" test "$(wc -w < "$FAKE_DIR/adb/pids")" = 1
    check effect "スマホの受け手がキットの物になった" test "$(cat "$FAKE_DIR/adb/hash")" = "$(sha "$KIT/android/server_aarch64")"
    check effect "GPU のポートを adb が開けている" adb_listens "$P"
    [ "$MODE" = decline ] && check safety "断ったので start-android.sh を呼んでいない（push していない）" not grep -q '^-s FAKESERIAL123 push' "$FAKE_DIR/adb/calls.log"
    common_checks
}
# ---- android：アプリ 0.1.2 なのに、カメラとマイクが古い向け方 ----
sc_android_appform_new() {
    new_scene
    android_ready
    echo "0.1.2" > "$FAKE_DIR/adb/app"
    adb_fwd "$P" "tcp:$P"
    adb_fwd "$CAM" "tcp:50064"
    adb_fwd "$AUD" "tcp:50063"
    run_fix android
    check effect "カメラが localabstract:pocketgpu-webcam" fwd_is "$CAM" "localabstract:pocketgpu-webcam"
    check effect "マイクが localabstract:pocketgpu-audio" fwd_is "$AUD" "localabstract:pocketgpu-audio"
    check effect "カメラのポートを adb が開けている" adb_listens "$CAM"
    check safety "GPU のポートには触っていない" fwd_is "$P" "tcp:$P"
    common_checks
}
# ---- android：アプリ 0.1.1 なのに、カメラとマイクが新しい向け方 ----
sc_android_appform_old() {
    new_scene
    android_ready
    echo "0.1.1" > "$FAKE_DIR/adb/app"
    adb_fwd "$P" "tcp:$P"
    adb_fwd "$CAM" "localabstract:pocketgpu-webcam"
    adb_fwd "$AUD" "localabstract:pocketgpu-audio"
    run_fix android
    check effect "カメラが tcp:50064" fwd_is "$CAM" "tcp:50064"
    check effect "マイクが tcp:50063" fwd_is "$AUD" "tcp:50063"
    common_checks
}
# ---- android：アプリ 0.1.4 の受け手を使っていて、forward は残っているが PC のポートが開いていない ----
sc_android_app014_stale() {
    new_scene
    android_ready
    : > "$FAKE_DIR/adb/pids"
    echo "0.1.4" > "$FAKE_DIR/adb/app"
    echo "FAKESERIAL123 tcp:$P localabstract:pocketgpu-gpu" > "$FAKE_DIR/adb/forwards"
    run_fix android
    check effect "GPU のポートの向け先が localabstract:pocketgpu-gpu のまま" fwd_is "$P" "localabstract:pocketgpu-gpu"
    check effect "GPU のポートを adb が開けている" adb_listens "$P"
    check safety "キットの受け手を入れていない（push していない）" not grep -q 'push' "$FAKE_DIR/adb/calls.log"
    common_checks
}

# ---- android：no permissions（udev を直したあとで、adb のサーバーが古い権限のまま） ----
sc_android_noperm() {
    new_scene
    printf 'FAKESERIAL123\tno permissions (user in plugdev group; are your udev rules wrong?)\n' > "$FAKE_DIR/adb/devices"
    touch "$FAKE_DIR/adb/noperm_fix"
    run_fix android
    check effect "adb devices で device になった" grep -q $'\tdevice$' "$FAKE_DIR/adb/devices"
    [ "$MODE" = decline ] && check safety "断ったので kill-server していない" not grep -q '^kill-server' "$FAKE_DIR/adb/calls.log"
    common_checks
}
# ---- android：本物の start-android.sh を、偽の adb の上で fix.sh から呼ぶ（--android-src のときだけ） ----
sc_android_real_start() {
    new_scene
    cp "$ANDROID_SRC/start-android.sh" "$KIT/start-android.sh"
    cp "$ANDROID_SRC/android/common.sh" "$KIT/android/common.sh"
    sed -i 's/\r$//' "$KIT/start-android.sh" "$KIT/android/common.sh"
    chmod 755 "$KIT/start-android.sh"
    snapshot
    android_ready
    : > "$FAKE_DIR/adb/pids"
    run_fix android
    check effect "スマホの受け手が 1 つ動いている" test "$(wc -w < "$FAKE_DIR/adb/pids")" = 1
    check effect "GPU のポートを adb が開けている" adb_listens "$P"
    check effect "forward の向け先が tcp:$P" fwd_is "$P" "tcp:$P"
    common_checks
}
# ---- media：前の start-mic-camera.sh が残した仮想のマイク ----
sc_media_modules() {
    new_scene
    echo "PulseAudio (on PipeWire 1.0.5)" > "$FAKE_DIR/pactl/server"
    printf '7\tmodule-alsa-card\tdevice_id="0"\n21\tmodule-null-sink\tsink_name=pocketgpu_mic_sink sink_properties=device.description=PocketGPU_iPhone_Mic_internal\n22\tmodule-remap-source\tmaster=pocketgpu_mic_sink.monitor source_name=pocketgpu_iphone_mic\n' > "$FAKE_DIR/pactl/modules"
    run_fix media
    check effect "pocketgpu_ のモジュールが外れた" not grep -q pocketgpu_ "$FAKE_DIR/pactl/modules"
    check safety "ほかのモジュール（7）は残っている" grep -q $'^7\t' "$FAKE_DIR/pactl/modules"
    # この場面は、ほかに直せない注意（v4l2loopback が無いなど）があるので、終わりのコードは 0（注意だけ）
    common_checks
}

# ---- 案内だけの場面（static） ----
sc_udev_group() {
    new_scene
    echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", MODE="0660", GROUP="plugdev"' > "$UDEV/51-android.rules"
    echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="05ac", RUN+="/usr/sbin/usbmuxd -u"' > "$UDEV/39-usbmuxd.rules"
    echo "$TUSER" > "$FAKE_DIR/groups_now"
    echo "$TUSER" > "$FAKE_DIR/groups_db"
    STATIC_ANS=y,y,y,y
    run_fix udev
    check effect "グループに入るコマンドを出した" out_has 'sudo usermod -aG plugdev'
    check effect "usbmuxd の規則は見つけた" out_has '39-usbmuxd.rules'
    check effect "終わりのコードは 0（注意だけ）" test "$RC" = 0
    echo "$TUSER plugdev" > "$FAKE_DIR/groups_db"
    run_fix udev
    check effect "DB にはあるときは、ログインし直す案内" out_has 'log in again'
    rm -f "$UDEV/51-android.rules"
    echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", TAG+="uaccess"' > "$UDEV/70-android-tools.rules"
    run_fix udev
    check effect "uaccess の規則なら、グループは要らない" out_has 'No group is needed'
    common_checks
}
sc_usbmuxd_missing() {
    new_scene
    rm -f "$BIN/usbmuxd" "$BIN/idevicepair"
    echo missing > "$FAKE_DIR/mux"
    local os want
    for os in "cachyos|ID=cachyos\nID_LIKE=arch|sudo pacman -S --needed usbmuxd libimobiledevice" \
        "mint|ID=linuxmint\nID_LIKE=\"ubuntu debian\"|sudo apt install usbmuxd libimobiledevice-utils" \
        "fedora|ID=fedora|sudo dnf install usbmuxd libimobiledevice-utils" \
        "steamos|ID=steamos\nID_LIKE=arch|sudo steamos-readonly disable"; do
        IFS='|' read -r name body want <<< "$os"
        printf "NAME=$name\n$body\n" > "$OSREL"
        run_fix usbmuxd
        check effect "$name：入れ方を出した（$want）" out_has "$want"
        check effect "$name：終わりのコードが 1" test "$RC" = 1
    done
    TLANG=ja
    run_fix usbmuxd
    check effect "日本語でも出せる（usbmuxd がインストールされていません）" out_has "usbmuxd がインストールされていません"
    common_checks
}
sc_usbmuxd_stale() {
    new_scene
    if [ "${REAL_RUN:-0}" = 1 ]; then
        cp "$REAL_PROBE" "$KIT/bin/pgpu-probe"
        PROBE_FOR_TEST=
        python3 "$FAKES/fake_usbmuxd.py" --stale "$FAKE_DIR/usbmuxd.sock"
        EXTRA_ENV=(IPHONE_VK_MUX="$FAKE_DIR/usbmuxd.sock")
        snapshot
    else
        echo refused > "$FAKE_DIR/mux"
    fi
    run_fix usbmuxd
    check effect "古いソケットと言った" out_has "An old socket remains"
    check effect "起動し直すコマンドを出した" out_has "sudo systemctl restart usbmuxd"
    check effect "終わりのコードが 1" test "$RC" = 1
    common_checks
}
sc_loader() {
    new_scene
    echo "LOADER OK 1.3.204 /usr/lib/libvulkan.so.1" > "$FAKE_DIR/loader"
    run_fix loader
    check effect "1.3.204：VK_ICD_FILENAMES だけが効くと言った" out_has "Only VK_ICD_FILENAMES works"
    check effect "1.3.204：起動の台本は対応済み" out_has "also sets VK_ICD_FILENAMES"
    check effect "1.3.204：終わりのコードは 0（注意だけ）" test "$RC" = 0
    sed -i '/ICD-LINE/d' "$KIT/run-on-iphone-gpu.sh"
    run_fix loader
    check effect "古い起動の台本だと NG" out_has "does not set VK_ICD_FILENAMES"
    check effect "古い起動の台本だと終わりのコードが 1" test "$RC" = 1
    echo "LOADER OK 1.3.220 /usr/lib/libvulkan.so.1" > "$FAKE_DIR/loader"
    EXTRA_ENV=(IPHONE_VK_ONLY=0)
    run_fix loader
    check effect "1.3.220 で IPHONE_VK_ONLY=0：使えないと言った" out_has "With this loader the GPU is not seen"
    EXTRA_ENV=(VK_LOADER_DRIVERS_DISABLE='*iphone*')
    echo "LOADER OK 1.3.280 /usr/lib/libvulkan.so.1" > "$FAKE_DIR/loader"
    run_fix loader
    check effect "VK_LOADER_DRIVERS_DISABLE がドライバを隠すと言った" out_has "hides the iPhone driver"
    EXTRA_ENV=()
    echo "LOADER NG missing" > "$FAKE_DIR/loader"
    run_fix loader
    check effect "ローダーが無い：Ubuntu の入れ方（libvulkan1）" out_has "sudo apt install libvulkan1"
    PROBE_FOR_TEST="$S_DIR/no-such-probe"
    echo "1.3.250" > "$FAKE_DIR/vulkaninfo"
    run_fix loader
    check effect "pgpu-probe が動かないときは vulkaninfo で版を読む" out_has "Vulkan loader 1.3.250"
    common_checks
}
sc_check_only() {
    new_scene
    sed -i 's/$/\r/' "$KIT/run-on-iphone-gpu.sh"
    local before
    before=$(sha "$KIT/run-on-iphone-gpu.sh")
    TOKEN_VALUE=$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')
    printf '%s\n' "$TOKEN_VALUE" > "$KIT/tools/broker-token"
    STATIC_ANS=y,y,y,y,y,y,y,y
    run_fix files,tools --check-only
    check effect "--check-only：y を用意しても台本は同じ" test "$(sha "$KIT/run-on-iphone-gpu.sh")" = "$before"
    check effect "--check-only：合言葉のファイルも動かさない" test -f "$KIT/tools/broker-token"
    check effect "--check-only：fix-backup を作らない" no_backup
    check effect "--check-only：問題があるので終わりのコードは 1" test "$RC" = 1
    common_checks
    TOKEN_VALUE=
}
sc_gpu_ok() {
    new_scene
    touch "$FAKE_DIR/paired"
    real_probe_scene
    run_fix usbmuxd,iphone,gpu
    check effect "iPhone の GPU が見えた" out_has "Seen through the driver: Apple A17 Pro GPU (fake)"
    check effect "終わりのコードは 0" test "$RC" = 0
    check effect "まとめのファイルを書いた" test -s "$KIT/pocketgpu-fix-summary.txt"
    common_checks
}
# アプリの受け手：0.1.4 は USB 用の口（localabstract）・0.1.3 以下は tcp:50060。どちらも「問題なし」と読む
sc_android_app_receiver() {
    new_scene
    android_ready
    : > "$FAKE_DIR/adb/pids"
    echo "0.1.4" > "$FAKE_DIR/adb/app"
    adb_fwd "$P" "localabstract:pocketgpu-gpu"
    run_fix android
    check effect "0.1.4：アプリの受け手を使っていると言った" out_has "USB socket localabstract:pocketgpu-gpu"
    check effect "0.1.4：終わりのコードは 0" test "$RC" = 0
    reset_fwd
    echo "0.1.3" > "$FAKE_DIR/adb/app"
    adb_fwd "$P" "tcp:50060"
    run_fix android
    check effect "0.1.3：アプリの受け手（ポート 50060）を使っていると言った" out_has "receiver inside the PocketGPU app (port 50060"
    check effect "0.1.3：終わりのコードは 0" test "$RC" = 0
    reset_fwd
    echo "0.1.3" > "$FAKE_DIR/adb/app"
    adb_fwd "$P" "localabstract:pocketgpu-gpu"
    run_fix android
    check effect "0.1.3 なのに USB 用の口の forward は、受け手が無いものとして問題にする（終わりのコード 1）" test "$RC" = 1
    common_checks
}
sc_android_gpu_ok() {
    new_scene
    android_ready
    adb_fwd "$P" "tcp:$P"
    run_fix android,gpu
    check effect "スマホの GPU が見えた" out_has "Seen through the driver: PocketGPU phone"
    check effect "終わりのコードは 0" test "$RC" = 0
    common_checks
}
# 全部の項目を、問題の無い偽のキットで通して、bash のエラーが出ないこと（英語と日本語）
sc_full_smoke() {
    new_scene
    touch "$FAKE_DIR/paired"
    android_ready
    adb_fwd "$P" "tcp:$P"
    echo "PulseAudio (on PipeWire 1.0.5)" > "$FAKE_DIR/pactl/server"
    run_fix pc,files,loader,usbmuxd,udev,tools,iphone,android,media,gpu
    check effect "全部の項目：英語で最後まで動いた（まとめを出した）" out_has "==== Summary"
    check effect "全部の項目：GPU が見えた" out_has "Seen through the driver"
    check safety "全部の項目：英語で bash のエラーが無い" not grep -qE 'unbound variable|syntax error|command not found|bad substitution|bad array subscript' "$OUTF"
    TLANG=ja
    run_fix pc,files,loader,usbmuxd,udev,tools,iphone,android,media,gpu
    check effect "全部の項目：日本語で最後まで動いた" out_has "==== まとめ"
    common_checks
}
# 本物の pgpu-probe で、ファイルのチェック（本物の ELF を 64 ビットの x86_64 と読めるか）
sc_files_real() {
    new_scene
    cp "$REAL_PROBE" "$KIT/bin/pgpu-probe"
    chmod 755 "$KIT/bin/pgpu-probe"
    snapshot
    run_fix files
    check effect "本物の pgpu-probe を正しいファイルと読んだ" out_has "All needed files are here"
    check effect "終わりのコードは 0" test "$RC" = 0
    common_checks
}
# 本物の pgpu-probe があるときは、iPhone の場面を偽の usbmuxd と本物の道具で動かす
real_probe_scene() {
    [ -n "$REAL_PROBE" ] && [ "${REAL_RUN:-0}" = 1 ] || return 0
    cp "$REAL_PROBE" "$KIT/bin/pgpu-probe"
    chmod 755 "$KIT/bin/pgpu-probe"
    PROBE_FOR_TEST=
    mkdir -p "$SYSUSB/1-1"
    echo 05ac > "$SYSUSB/1-1/idVendor"
    echo 12a8 > "$SYSUSB/1-1/idProduct"
    echo 480 > "$SYSUSB/1-1/speed"
    spawn python3 "$FAKES/fake_usbmuxd.py" --socket "$FAKE_DIR/usbmuxd.sock" --state "$FAKE_DIR"
    for _ in $(seq 1 60); do [ -S "$FAKE_DIR/usbmuxd.sock" ] && break; sleep 0.05; done
    EXTRA_ENV=(IPHONE_VK_MUX="$FAKE_DIR/usbmuxd.sock")
    snapshot
}

# ---- 動かす ----
FIX_SCENES=(files_crlf files_exec files_json tools_token tools_old_broker tools_dup_miccam iphone_pair iphone_holder
    android_leftover android_stale android_conflict android_oldrecv android_appform_new android_appform_old android_app014_stale android_noperm media_modules)
STATIC_SCENES=(files_missing udev_group usbmuxd_missing usbmuxd_stale loader check_only gpu_ok android_gpu_ok android_app_receiver full_smoke)
[ -n "$ANDROID_SRC" ] && FIX_SCENES+=(android_real_start)
wanted() { [ -z "$SCEN_FILTER" ] && return 0; case ",$SCEN_FILTER," in *",$1,"*) return 0 ;; esac; return 1; }
echo "work: $WORK"
echo "fix.sh act_ functions (sabotaged): ${ACTS[*]}"
run_set() {
    for SCEN in "${FIX_SCENES[@]}"; do
        wanted "$SCEN" || continue
        echo "== $SCEN$([ "$REAL_RUN" = 1 ] && echo ' (real pgpu-probe)')"
        SCENS_RUN+=("$SCEN")
        for MODE in fix decline sabotage; do
            STATIC_ANS=
            "sc_$SCEN"
            cleanup_scene
        done
    done
    for SCEN in "${STATIC_SCENES[@]}"; do
        wanted "$SCEN" || continue
        echo "== $SCEN (static)$([ "$REAL_RUN" = 1 ] && echo ' (real pgpu-probe)')"
        MODE=static
        STATIC_ANS=
        "sc_$SCEN"
        cleanup_scene
    done
}
REAL_RUN=0
run_set
if [ -n "$REAL_PROBE" ]; then
    # 本物の道具でもう一度：iPhone の場面と、usbmuxd の古いソケットと、ファイルのチェック（本物の ELF）
    REAL_RUN=1
    FIX_SCENES=(iphone_pair iphone_holder)
    STATIC_SCENES=(files_real usbmuxd_stale)
    run_set
fi

# ---- まとめ ----
echo
echo "==== 対照の効き目（decline・sabotage で、効果の確認が 1 つ以上落ちたか）===="
bad_ctl=0
declare -A SEEN
for SCEN in "${SCENS_RUN[@]}"; do
    [ -n "${SEEN[$SCEN]:-}" ] && continue
    SEEN[$SCEN]=1
    for MODE in decline sabotage; do
        k="$SCEN|$MODE"
        if [ "${EFF_BAD[$k]:-0}" -ge 1 ]; then echo "  ok    $SCEN/$MODE（効果の確認 ${EFF_N[$k]:-0} 個のうち ${EFF_BAD[$k]} 個が落ちた）"
        else echo "  NG    $SCEN/$MODE：効果の確認が 1 つも落ちなかった（試験が何も測っていない）"; bad_ctl=$((bad_ctl + 1)); fi
    done
done
echo
if [ ${#FAILS[@]} -eq 0 ] && [ $bad_ctl = 0 ]; then
    echo "PASS"
    rc=0
else
    echo "FAIL"
    for f in "${FAILS[@]}"; do echo "  $f"; done
    rc=1
fi
if [ "$KEEP" = 0 ] && [ $rc = 0 ]; then rm -rf "$WORK"; else echo "work kept: $WORK"; fi
exit $rc
