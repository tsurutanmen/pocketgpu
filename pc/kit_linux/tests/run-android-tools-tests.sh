#!/usr/bin/env bash
# send-android-log.sh と test-android-speed.sh の試験。偽の adb（pocketgpu-setup/linux/tests/fakes/adb と adb_listen.py）だけで動かす。本物のスマホ・adb・USB・インターネットは使わない。sudo は使わない。
#   bash tests/run-android-tools-tests.sh [--kit フォルダ] [--fakes フォルダ] [--scenario a,b] [--keep] [--ports 基準番号]
#   --kit フォルダ     試すキットのフォルダ（send-android-log.sh・test-android-speed.sh・android/common.sh・android/tools.sh がある所）。既定はこの tests の 1 つ上
#   --fakes フォルダ   偽の adb のフォルダ。既定は app と同じ階層の pocketgpu-setup/linux/tests/fakes
#   --scenario a,b     場面の名前で絞る（log_basic など。下の run_* の名前）
#   --keep             作業場所を消さない
#   --ports 基準番号   試験が使う PC の口は 基準番号〜基準番号+9 の 10 個だけ（既定 47570）
# 必要な物：bash 4.4 以上・python3・coreutils・tar・gawk か mawk。zip コマンドは要らない（zip の場面は python で作った偽の zip を使う）
#
# 1 つの場面は、普通の動かし方で確かめる。そのほかに、わざと壊した写し（sabotage）で同じ確認を動かし、確認が落ちること（確認が空振りでないこと）を確かめる。
#   check    普通の動かし方。通らなければ失敗
#   detect   わざと壊した写し。確認が落ちれば成功（通ってしまったら、その確認は何も測れていない）
# 守ること：止めるのは、この試験が自分で起こしたプロセスだけ（PID を控えて止める）。試験の作業場所（mktemp）の外には書かない。PC の口は --ports の 10 個だけ。
set -u
T=$(cd "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
KITSRC=$(cd "$T/.." && pwd -P)
FAKES_SRC=
SCEN_FILTER=
KEEP=0
PBASE=47570
while [ $# -gt 0 ]; do
    case "$1" in
    --kit) KITSRC=$(readlink -f -- "$2"); shift ;;
    --fakes) FAKES_SRC=$(readlink -f -- "$2"); shift ;;
    --scenario) SCEN_FILTER=$2; shift ;;
    --keep) KEEP=1 ;;
    --ports) PBASE=$2; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$FAKES_SRC" ] || FAKES_SRC=$(cd "$T/../../../../pocketgpu-setup/linux/tests/fakes" 2> /dev/null && pwd -P)
for c in python3 tar sort awk sed timeout mkfifo; do command -v "$c" > /dev/null || { echo "needed: $c" >&2; exit 2; }; done
[ -d "$FAKES_SRC" ] || { echo "fakes folder not found (use --fakes)" >&2; exit 2; }
for f in send-android-log.sh test-android-speed.sh android/common.sh android/tools.sh; do [ -f "$KITSRC/$f" ] || { echo "$KITSRC/$f not found" >&2; exit 2; }; done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pgpu-android-tools-tests.XXXXXX")
TUSER=kuroneko
THOST=mikanpc
SER=R58M12ABCDE
OTHERSER=ZZ99OTHERDEV01
MAIL=tsuru.taro@example.com
REALUSER=$(id -un)

PASS=0
FAIL=0
DET=0
DETBAD=0
FAILS=()
SPAWNED=()
# 偽の adb が起こした listener（adb_listen.py）のうち、この試験の場面が残した物を、PID で止める。壊した写しが forward を消さない場面の分が、次の場面の口を塞がないように
reap_listeners() {
    local f p
    for f in "$WORK"/*/fake/adb/listen.*; do
        [ -f "$f" ] || continue
        p=$(cat "$f")
        kill "$p" 2> /dev/null
        rm -f "$f"
    done
}
cleanup() {
    local p
    reap_listeners
    for p in ${SPAWNED[@]+"${SPAWNED[@]}"}; do kill "$p" 2> /dev/null; done
    [ "$KEEP" = 1 ] || rm -rf -- "$WORK"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

check() { # check "名前" コマンド...
    local n=$1
    shift
    if "$@" > /dev/null 2>&1; then PASS=$((PASS + 1)); printf '  PASS   %s\n' "$n"; else FAIL=$((FAIL + 1)); FAILS+=("$SCEN: $n"); printf '  FAIL   %s\n' "$n"; fi
}
detect() { # detect "名前" コマンド...   わざと壊した写しでは、確認が落ちるはず
    local n=$1
    shift
    if "$@" > /dev/null 2>&1; then DETBAD=$((DETBAD + 1)); FAILS+=("$SCEN: (sabotage not detected) $n"); printf '  VACUOUS %s\n' "$n"; else DET=$((DET + 1)); printf '  DETECT %s\n' "$n"; fi
}
has() { grep -qF -- "$2" "$1"; }
hasnt() { ! grep -qF -- "$2" "$1"; }
rx() { grep -qE -- "$2" "$1"; }
within() { awk -v v="$1" -v lo="$2" -v hi="$3" 'BEGIN { exit !(v >= lo && v <= hi) }'; }
want_scen() { [ -z "$SCEN_FILTER" ] || case ",$SCEN_FILTER," in *",$1,"*) return 0 ;; *) return 1 ;; esac; }

# ---- 場面の用意 ----
# new_scene 名前 ：S（場面の作業場所）・HOMED（ホーム）・KITD（キットの写し）・FAKEBIN（偽の adb）・FD（偽の adb の状態）を作る
NSCENE=0
new_scene() {
    SCEN=$1
    NSCENE=$((NSCENE + 1))
    S=$WORK/s$NSCENE
    HOMED=$S/home/$TUSER
    KITD=$HOMED/kit
    FAKEDIR=$HOMED/fakes
    FD=$S/fake/adb
    mkdir -p "$KITD/android" "$FAKEDIR" "$FD" "$S/tmp" "$S/bin" "$S/out"
    cp "$KITSRC/send-android-log.sh" "$KITSRC/test-android-speed.sh" "$KITD/"
    cp "$KITSRC/android/common.sh" "$KITSRC/android/tools.sh" "$KITD/android/"
    cp "$FAKES_SRC/adb" "$FAKES_SRC/adb_listen.py" "$FAKEDIR/"
    find "$KITD" "$FAKEDIR" -type f -exec sed -i 's/\r$//' {} +
    chmod +x "$FAKEDIR/adb"
    printf '%s\tdevice\n' "$SER" > "$FD/devices"
    printf 'NAME="Linux Mint"\nPRETTY_NAME="Linux Mint 22 (fake)"\nID=linuxmint\nID_LIKE="ubuntu debian"\n' > "$S/os-release"
    EXTRA=()
    KIT_OVERRIDE=
    ANS=
    LANGX=en
    echo "== $SCEN"
}
phone_defaults() {
    printf 'FakeMaker\n' > "$FD/prop.ro.product.manufacturer"
    printf 'FakePhone\n' > "$FD/prop.ro.product.model"
    printf '14\n' > "$FD/prop.ro.build.version.release"
    printf '34\n' > "$FD/prop.ro.build.version.sdk"
    printf 'FakeSoC\n' > "$FD/prop.ro.soc.model"
    printf '0.1.5\n' > "$FD/app"
    # 聞かれても答えてはいけない物。キットがこれを聞きに行ったら、calls.log に残る
    printf '%s\n' "$SER" > "$FD/prop.ro.serialno"
    printf '359999999999999\n' > "$FD/prop.ro.ril.oem.imei"
    printf '+819012345678\n' > "$FD/prop.ro.ril.phone.number"
}
log_defaults() {
    phone_defaults
    cat > "$FD/surfaceflinger" << EOF
Build configuration: [sf PRESENT_TIME_OFFSET=0]
Display 4619827259835644672 HWC layers: serial number = $SER
GLES: Qualcomm, Adreno (TM) 740, OpenGL ES 3.2 V@0615.0 (GIT@a1b2c3d, I12345) (Date:01/02/24)
GLES extensions: GL_OES_EGL_image
EOF
    cat > "$FD/srvlog" << EOF
Vulkan receiver: listening on 127.0.0.1:50300
Receiver GPU: Adreno (TM) 740
config path /home/$TUSER/kit/android/x
client host $THOST connected, serial $SER accepted
contact $MAIL for help
driver V@0615.0 (GIT@a1b2c3d)
Users path C:\\Users\\$TUSER\\AppData
EOF
    cat > "$FD/logcat_crash" << 'EOF'
--------- beginning of crash
10-15 12:00:01.100  4242  4242 E AndroidRuntime: FATAL EXCEPTION: main
10-15 12:00:01.100  4242  4242 E AndroidRuntime: Process: com.example.other, PID: 4242
10-15 12:00:01.100  4242  4242 E AndroidRuntime: java.lang.NullPointerException
10-15 12:00:02.200  5050  5050 F libc    : Fatal signal 11 (SIGSEGV), code 1, fault addr 0x0 in tid 5050 (pgpu_server), pid 5050 (pgpu_server)
10-15 12:00:02.300  5099  5099 F DEBUG   : *** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
10-15 12:00:02.300  5099  5099 F DEBUG   : Build fingerprint: 'x'
10-15 12:00:02.300  5099  5099 F DEBUG   : pid: 5050, tid: 5050, name: pgpu_server  >>> /data/local/tmp/pgpu_server <<<
10-15 12:00:02.300  5099  5099 F DEBUG   :       #00 pc 0000000000001234  /data/local/tmp/pgpu_server (main+12)
10-15 12:00:03.400  6060  6060 E AndroidRuntime: FATAL EXCEPTION: main
10-15 12:00:03.400  6060  6060 E AndroidRuntime: Process: com.tsurutanmen.pocketgpu, PID: 6060
10-15 12:00:03.400  6060  6060 E AndroidRuntime: java.lang.IllegalStateException: boom
10-15 12:00:04.500  7070  7070 E system_server: other crash in system
EOF
    cat > "$FD/logcat_app" << 'EOF'
--------- beginning of main
10-15 12:10:00.000  8080  8080 I iphone-rpc-vk: app receiver started
10-15 12:10:01.000  8080  8081 W iphone-rpc-vk: client slow
EOF
}
# run 台本 引数... ：台本を env -i で動かす。出力は OUT・終わりのコードは RC
RUNN=0
run() {
    local s=$1 kd=${KIT_OVERRIDE:-$KITD}
    shift
    RUNN=$((RUNN + 1))
    OUT=$S/out_${RUNN}_${s%.sh}.txt
    rm -f "$FD/calls.log"
    (cd "$kd" && env -i HOME="$HOMED" USER="$TUSER" LOGNAME="$TUSER" PATH="$S/bin:/usr/bin:/bin" LANG=C.UTF-8 TMPDIR="$S/tmp" \
        ADB="$FAKEDIR/adb" FAKE_DIR="$S/fake" PGPU_LANG="$LANGX" PGPU_NOPROMPT=1 PGPU_ANSWERS="$ANS" PGPU_TEST_HOSTNAME="$THOST" PGPU_OS_RELEASE="$S/os-release" \
        PGPU_LOG_OUT="$S/out" PGPU_SPEED_OUT="$S/out" ${EXTRA[@]+"${EXTRA[@]}"} timeout 240 bash "./$s" "$@") > "$OUT" 2>&1
    RC=$?
}
calls() { cat "$FD/calls.log" 2> /dev/null; }
# sabotage_kit 名前 ファイル 'sed の式'（または append: 追記する行） ：キットの写しを作って、1 か所をわざと壊す。KIT_OVERRIDE に入れる
sabotage_kit() {
    local name=$1 file=$2 how=$3
    local d=$HOMED/kit_$name
    rm -rf "$d"
    cp -r "$KITD" "$d"
    case "$how" in
    append:*) printf '%s\n' "${how#append:}" >> "$d/$file" ;;
    delline:*) grep -vF -- "${how#delline:}" "$KITD/$file" > "$d/$file" ;;
    *) sed -i -e "$how" "$d/$file" ;;
    esac
    bash -n "$d/$file" || echo "sabotage copy has a syntax error: $name" >&2
    # 壊したつもりで何も変わっていなければ、その対照は何も測れていない。失敗として数える
    if cmp -s "$KITD/$file" "$d/$file"; then
        DETBAD=$((DETBAD + 1))
        FAILS+=("$SCEN: (sabotage '$name' changed nothing in $file)")
        printf '  VACUOUS sabotage %s changed nothing in %s\n' "$name" "$file"
    fi
    KIT_OVERRIDE=$d
}
ex_archive() { # 圧縮ファイルを $S/ex に開く。ARCH が名前
    ARCH=$(ls "$S"/out/pocketgpu-android-log-*.tar.gz "$S"/out/pocketgpu-android-log-*.zip 2> /dev/null | head -n 1)
    rm -rf "$S/ex"
    mkdir -p "$S/ex"
    case "$ARCH" in
    *.tar.gz) tar xzf "$ARCH" -C "$S/ex" ;;
    *.zip) python3 -c 'import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$ARCH" "$S/ex" ;;
    esac
    EXD=$(ls -d "$S"/ex/pocketgpu-android-log-* 2> /dev/null | head -n 1)
}
secrets_absent() { # 圧縮ファイルの中身（と、その元のフォルダ）に、伏せる物が 1 つも無い
    ! grep -rqiF -e "$TUSER" -e "$THOST" -e "$SER" -e "$MAIL" -e "$OTHERSER" "$EXD" "$S/out/$(basename "$EXD")"
}
# 圧縮ファイルの外に出した物（画面の出力と、記録）には、伏せる物のうち PC の名前と電話の物が無い。画面にはキットの場所（ホームの名前）が出るので、ユーザー名は調べない
no_phone_secrets() { ! grep -qiF -e "$SER" -e "$MAIL" -e '359999999999999' -e '+819012345678' "$1"; }

# =====================================================================
# send-android-log.sh
# =====================================================================
ALLOWED_CALLS='^(devices|start-server|version|forward --list|logcat -d -b crash|logcat -d -s iphone-rpc-vk|shell (getprop ro\.(product\.(model|manufacturer|cpu\.abi)|build\.version\.(release|sdk)|soc\.model|board\.platform)|dumpsys SurfaceFlinger|dumpsys package com\.tsurutanmen\.pocketgpu \| grep versionName= \| head -n 1|pidof pgpu_server|cat /data/local/tmp/pgpu_server\.log))$'
only_allowed_calls() { ! calls | grep -v -E "$ALLOWED_CALLS" | grep -q .; }

run_log_basic() {
    new_scene log_basic
    log_defaults
    run send-android-log.sh
    check "終わりのコード 0" test "$RC" = 0
    ex_archive
    check "tar.gz ができた（この PC に zip コマンドは無い）" test -n "$ARCH" -a -z "$(command -v zip)"
    check "中に 4 つのファイル" bash -c "cd '$EXD' && ls | sort | tr '\n' ' ' | grep -qx 'info.txt logcat-app.txt logcat-crash.txt receiver-log.txt '"
    check "フォルダと圧縮ファイルが両方ある" test -d "$S/out/$(basename "$EXD")"
    check "info：機種・メーカー・Android・ABI・チップ" bash -c "grep -qx 'Model: FakePhone' '$EXD/info.txt' && grep -qx 'Maker: FakeMaker' '$EXD/info.txt' && grep -qx 'Android: 14 (SDK 34)' '$EXD/info.txt' && grep -qx 'Chip: FakeSoC' '$EXD/info.txt' && grep -qE '^ABI: ' '$EXD/info.txt'"
    check "info：GPU（受け手の記録）と GPU（GLES の 1 行だけ）" bash -c "grep -qx 'GPU (receiver log): Adreno (TM) 740' '$EXD/info.txt' && grep -qxF 'GPU (GLES): Qualcomm, Adreno (TM) 740, OpenGL ES 3.2 V@0615.0 (GIT@a1b2c3d, I12345) (Date:01/02/24)' '$EXD/info.txt'"
    check "GLES の前後の飾りの行は入っていない" bash -c "! grep -rq 'GL_OES_EGL_image\|Build configuration\|HWC layers' '$EXD'"
    check "info：アプリの版・受け手が止まっている" bash -c "grep -qx 'PocketGPU app: 0.1.5' '$EXD/info.txt' && grep -qx 'Kit receiver running: no' '$EXD/info.txt'"
    check "info：Linux の版・カーネル・adb の版・PGPU_PORT" bash -c "grep -qx 'Linux: Linux Mint 22 (fake)' '$EXD/info.txt' && grep -q '^Kernel: ' '$EXD/info.txt' && grep -q '^adb: Android Debug Bridge' '$EXD/info.txt' && grep -qx 'Port in use (PGPU_PORT): 50300 (default)' '$EXD/info.txt'"
    check "info：集めない物と伏せる物の書き" bash -c "grep -q '^Not collected: phone number, IMEI' '$EXD/info.txt' && grep -q '^Hidden: PC user name' '$EXD/info.txt'"
    check "受け手の記録が入っている" has "$EXD/receiver-log.txt" "Receiver GPU: Adreno (TM) 740"
    check "伏せる：ユーザー名・PC 名・シリアル・メールが、どのファイルにも無い" secrets_absent
    check "伏せた印が入っている" bash -c "grep -q '(hidden)' '$EXD/receiver-log.txt' && grep -q '(serial)' '$EXD/receiver-log.txt' && grep -q '(email)' '$EXD/receiver-log.txt'"
    check "GPU の版 V@0615.0 と GIT@a1b2c3d はメールとして伏せない" bash -c "grep -q 'V@0615.0 (GIT@a1b2c3d)' '$EXD/receiver-log.txt'"
    check "Windows 形式のパス C:\\Users\\名前 の名前も伏せる" bash -c "grep -qF 'C:\\Users\\(hidden)\\AppData' '$EXD/receiver-log.txt'"
    check "落ちた記録：PocketGPU の 3 つのまとまりだけ" bash -c "grep -q 'pgpu_server' '$EXD/logcat-crash.txt' && grep -q 'com.tsurutanmen.pocketgpu' '$EXD/logcat-crash.txt' && ! grep -q 'com.example.other\|system_server' '$EXD/logcat-crash.txt'"
    check "落ちた記録の見出しに、省いた数（2）" has "$EXD/logcat-crash.txt" "2 other block(s) left out"
    check "info の Files に件数（3 と 2）" has "$EXD/info.txt" "logcat-crash.txt: PocketGPU only, 3 blocks, 2 other blocks left out"
    check "画面：落ちた記録の数" has "$OUT" "Crash log: PocketGPU 3, other apps 2"
    check "全部を送るかの問いを出して、N（既定）で進んだ" has "$OUT" "Send the full crash log, with other apps? (N: PocketGPU only) -> no"
    check "アプリの記録（beginning of の行を除く）" bash -c "grep -q 'app receiver started' '$EXD/logcat-app.txt' && ! grep -q 'beginning of' '$EXD/logcat-app.txt'"
    check "決まった命令だけを adb に打った" only_allowed_calls
    check "電話番号・IMEI・シリアルのプロパティを聞いていない" bash -c "! grep -q 'imei\|phone.number\|serialno\|bugreport\|iphonesubinfo' '$FD/calls.log'"
    check "電話番号・IMEI が、画面にも圧縮ファイルにも無い" bash -c "! grep -rqF -e 359999999999999 -e +819012345678 '$EXD' '$OUT'"
    check "画面にシリアル・メールが出ていない" no_phone_secrets "$OUT"
    check "画面の終わり：圧縮ファイルの場所と Discord の案内" bash -c "grep -q 'OK  .*pocketgpu-android-log-.*\.tar\.gz' '$OUT' && grep -q 'Send this file on Discord' '$OUT'"
    check "zip が無いので tar.gz にした、という【仮文】の目印" has "$OUT" "DRAFT: the zip command was not found"
    check "tar.gz にファイルの持ち主の名前が無い（root/0）" bash -c "tar tvzf '$ARCH' | awk '{print \$2}' | sort -u | grep -qx '0/0' && ! tar tvzf '$ARCH' | grep -qF '$REALUSER'"
    check "作業場所（TMPDIR）に何も残っていない" bash -c "[ -z \"\$(ls -A '$S/tmp')\" ]"
    check "run のログは作らない（成功したとき）" test ! -e "$KITD/pocketgpu-android-sendlog-log.txt"
    # ---- わざと壊した写し ----
    new_scene log_sab_hide
    log_defaults
    sabotage_kit hide android/tools.sh 'append:hide() { cat; }'
    run send-android-log.sh
    ex_archive
    detect "（伏せない写し）伏せる物が無い、の確認が落ちる" secrets_absent
    KIT_OVERRIDE=
    new_scene log_sab_crash
    log_defaults
    sabotage_kit crash send-android-log.sh 's/awk -v mode=mine "\$AWK_CRASH"/awk -v mode=all "$AWK_CRASH"/'
    run send-android-log.sh
    ex_archive
    detect "（絞らない写し）ほかのアプリの落ちた記録が入っていない、の確認が落ちる" bash -c "! grep -q 'com.example.other\|system_server' '$EXD/logcat-crash.txt'"
    KIT_OVERRIDE=
    new_scene log_sab_owner
    log_defaults
    sabotage_kit owner send-android-log.sh 's/tar --owner=0 --group=0 --numeric-owner -czf/tar -czf/'
    run send-android-log.sh
    ex_archive
    detect "（持ち主を入れる写し）tar.gz に持ち主の名前が無い、の確認が落ちる" bash -c "! tar tvzf '$ARCH' | grep -qF '$REALUSER'"
    KIT_OVERRIDE=
    new_scene log_sab_call
    log_defaults
    sabotage_kit call send-android-log.sh 's#^RUNNING=0$#ashr getprop ro.ril.oem.imei > /dev/null; RUNNING=0#'
    run send-android-log.sh
    detect "（IMEI を聞く写し）IMEI を聞いていない、の確認が落ちる" bash -c "! grep -q 'imei' '$FD/calls.log'"
    detect "（IMEI を聞く写し）決まった命令だけ、の確認が落ちる" only_allowed_calls
    KIT_OVERRIDE=
}

run_log_crash_full() {
    new_scene log_crash_full
    log_defaults
    ANS=y
    run send-android-log.sh
    check "終わりのコード 0" test "$RC" = 0
    ex_archive
    check "y と答えると、ほかのアプリの落ちた記録も入る（5 つ全部）" bash -c "grep -q 'com.example.other' '$EXD/logcat-crash.txt' && grep -q 'system_server' '$EXD/logcat-crash.txt' && grep -q 'pgpu_server' '$EXD/logcat-crash.txt'"
    check "見出しが full になる" has "$EXD/logcat-crash.txt" "(full: other apps included)"
    check "info の Files が full（5 blocks）" has "$EXD/info.txt" "logcat-crash.txt: full (other apps included), 5 blocks"
    check "全部でも伏せる" secrets_absent
}

run_log_only_pgpu() {
    new_scene log_only_pgpu
    log_defaults
    cat > "$FD/logcat_crash" << 'EOF'
--------- beginning of crash
10-15 12:00:02.200  5050  5050 F libc    : Fatal signal 11 (SIGSEGV), code 1, fault addr 0x0 in tid 5050 (pgpu_server), pid 5050 (pgpu_server)
EOF
    ANS=y
    run send-android-log.sh
    check "ほかのアプリが無いときは、全部を送るかの問いを出さない" bash -c "! grep -q 'Send the full crash log' '$OUT'"
    check "y の答えは使われない（PocketGPU だけ・0 other）" bash -c "ex=\$(ls -d '$S'/out/pocketgpu-android-log-*/ | head -n1); grep -q '0 other block(s) left out' \"\$ex/logcat-crash.txt\""
    new_scene log_nocrash
    log_defaults
    : > "$FD/logcat_crash"
    run send-android-log.sh
    ex_archive
    check "落ちた記録が空：「見つからない」と書く" has "$EXD/logcat-crash.txt" "(no PocketGPU receiver crash found)"
    check "画面は 0 件" has "$OUT" "Crash log: PocketGPU 0, other apps 0"
}

run_log_crash_forms() {
    # logcat の古い形（brief：F/DEBUG ( 1010): …）でも、PocketGPU の受け手の分だけに絞れる
    new_scene log_crash_brief
    log_defaults
    cat > "$FD/logcat_crash" << 'EOF'
--------- beginning of crash
F/libc    ( 5678): Fatal signal 11 (SIGSEGV) in tid 5678 (pgpu_server), pid 5678 (pgpu_server)
F/DEBUG   ( 1010): pid: 5678, tid: 5678, name: pgpu_server  >>> ./pgpu_server <<<
F/DEBUG   ( 1010):     #00 pc 0000000000001234  /data/local/tmp/pgpu_server
E/AndroidRuntime( 2000): FATAL EXCEPTION: main
E/AndroidRuntime( 2000): Process: com.example.privatebank, PID: 2000
E/AndroidRuntime( 2000): java.lang.NullPointerException: secret-account-note
EOF
    run send-android-log.sh
    ex_archive
    check "brief 形：PocketGPU 2 件・ほかのアプリ 1 件と画面に出る" has "$OUT" "Crash log: PocketGPU 2, other apps 1"
    check "brief 形：受け手の行（libc・DEBUG の name・#00 pc）が入る" bash -c "grep -q 'Fatal signal 11' '$EXD/logcat-crash.txt' && grep -q 'name: pgpu_server' '$EXD/logcat-crash.txt' && grep -q '#00 pc 0000000000001234' '$EXD/logcat-crash.txt'"
    check "brief 形：ほかのアプリの Java の例外は入らない" bash -c "! grep -q 'privatebank\|secret-account-note' '$EXD/logcat-crash.txt'"
    # ほかのアプリの分だけ：n（既定）なら「見つからない」
    new_scene log_crash_others_only
    log_defaults
    cat > "$FD/logcat_crash" << 'EOF'
10-15 12:05:00.000  2000  2000 E AndroidRuntime: FATAL EXCEPTION: main
10-15 12:05:00.000  2000  2000 E AndroidRuntime: Process: com.example.privatebank, PID: 2000
10-15 12:09:00.000  1020  1020 F DEBUG   : *** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
10-15 12:09:00.000  1020  1020 F DEBUG   : pid: 9999, tid: 9999, name: com.example.otherapp  >>> com.example.otherapp <<<
EOF
    run send-android-log.sh
    ex_archive
    check "ほかのアプリの分だけ：問いを出す（PocketGPU 0 件・ほかのアプリ 2 件）" bash -c "grep -q 'Crash log: PocketGPU 0, other apps 2' '$OUT' && grep -q 'Send the full crash log' '$OUT'"
    check "n（既定）：「見つからない」だけで、ほかのアプリの行は入らない" bash -c "grep -q 'no PocketGPU receiver crash found' '$EXD/logcat-crash.txt' && ! grep -q 'privatebank\|otherapp' '$EXD/logcat-crash.txt'"
}

run_log_noapp() {
    new_scene log_noapp
    log_defaults
    rm -f "$FD/app"
    run send-android-log.sh
    check "終わりのコード 0" test "$RC" = 0
    ex_archive
    check "アプリが入っていない：logcat-app.txt を作らない" test ! -e "$EXD/logcat-app.txt"
    check "アプリが入っていない：iphone-rpc-vk の logcat を呼ばない" bash -c "! grep -q 'iphone-rpc-vk' '$FD/calls.log'"
    check "info に not installed と skipped" bash -c "grep -qx 'PocketGPU app: not installed' '$EXD/info.txt' && grep -qx 'logcat-app.txt: skipped (app not installed)' '$EXD/info.txt'"
    check "画面にも skipped" has "$OUT" "App log: skipped (app not installed)"
}

run_log_nolog() {
    new_scene log_nolog
    log_defaults
    : > "$FD/nolog"
    run send-android-log.sh
    ex_archive
    check "受け手の記録が無い：(no file on the phone)" has "$EXD/receiver-log.txt" "(no file on the phone)"
    check "info：no file on the phone" has "$EXD/info.txt" "receiver-log.txt: no file on the phone"
    check "info：GPU（受け手の記録）は - " has "$EXD/info.txt" "GPU (receiver log): -"
    check "GLES は取れる" has "$EXD/info.txt" "GPU (GLES): Qualcomm, Adreno (TM) 740"
}

run_log_limit() {
    new_scene log_limit
    log_defaults
    awk 'BEGIN { for (i = 1; i <= 5000; i++) printf "line %d host mikanpc\n", i }' > "$FD/srvlog"
    run send-android-log.sh
    ex_archive
    check "5000 行の記録：先頭に 1000 行を省いたと書く" bash -c "sed -n 2p '$EXD/receiver-log.txt' | grep -qx '(1000 older lines left out)'"
    check "残るのは後ろの 4000 行（最初が line 1001・最後が line 5000）" bash -c "sed -n 3p '$EXD/receiver-log.txt' | grep -q 'line 1001 ' && tail -n 1 '$EXD/receiver-log.txt' | grep -q 'line 5000 '"
    check "全部で 4002 行（見出し 1・省いた印 1・本文 4000）" test "$(wc -l < "$EXD/receiver-log.txt")" = 4002
    check "info は 5000 lines と書く" has "$EXD/info.txt" "receiver-log.txt: 5000 lines"
    check "長くても伏せる（PC 名が無い）" bash -c "! grep -q mikanpc '$EXD/receiver-log.txt'"
}

run_log_short_user() {
    new_scene log_short_user
    log_defaults
    TUSER=al
    HOMED=$S/home/al
    mkdir -p "$HOMED"
    cp -r "$KITD" "$HOMED/kit"
    KITD=$HOMED/kit
    cat > "$FD/srvlog" << 'EOF'
open /home/al/work/x
the alarm and al ok and a1 al_x
Receiver GPU: Fake GPU
EOF
    run send-android-log.sh
    ex_archive
    check "2 字の名前：/home/al/ の次の所だけ伏せる" bash -c "grep -qF 'open /home/(hidden)/work/x' '$EXD/receiver-log.txt'"
    check "2 字の名前：ほかの単語（alarm・al ok・al_x）は壊さない" bash -c "grep -qF 'the alarm and al ok and a1 al_x' '$EXD/receiver-log.txt'"
    TUSER=kuroneko
}

run_log_ja() {
    new_scene log_ja
    log_defaults
    LANGX=ja
    run send-android-log.sh
    check "日本語の画面：見出し・手順・件数" bash -c "grep -q 'PocketGPU Android の記録をまとめます' '$OUT' && grep -q '記録を集める' '$OUT' && grep -q '圧縮ファイルを作る' '$OUT' && grep -q '落ちた記録：PocketGPU 3 件・ほかのアプリ 2 件' '$OUT' && grep -q '受け手の記録：7 行' '$OUT'"
    ex_archive
    check "日本語でもファイルの中は英語だけ（ASCII のみ）" bash -c "! LC_ALL=C grep -rqP '[^\\x00-\\x7F]' '$EXD'"
    check "【仮文】の目印（tar.gz の案内）" has "$OUT" "【仮文】zip コマンドが無いので"
}

run_log_nophone() {
    new_scene log_nophone
    log_defaults
    : > "$FD/devices"
    run send-android-log.sh
    check "スマホが無い：終わりのコード 1" test "$RC" = 1
    check "案内を出す" has "$OUT" "No Android phone was found."
    check "何も作らない" bash -c "[ -z \"\$(ls -A '$S/out')\" ]"
    check "run のログを書く（stop_here）" test -s "$KITD/pocketgpu-android-sendlog-log.txt"
    new_scene log_noadb
    log_defaults
    EXTRA=(ADB=/nonexistent/adb)
    run send-android-log.sh
    check "adb が無い：終わりのコード 1・案内" bash -c "[ '$RC' = 1 ] && grep -q 'ADB is set to /nonexistent/adb' '$OUT'"
    new_scene log_unauth
    log_defaults
    printf '%s\tunauthorized\n' "$SER" > "$FD/devices"
    run send-android-log.sh
    check "許可がまだ：終わりのコード 1・案内" bash -c "[ '$RC' = 1 ] && grep -q 'this PC is not allowed yet' '$OUT'"
}

run_log_multi() {
    new_scene log_multi
    log_defaults
    printf '%s\tdevice\n%s\tdevice\n' "$SER" "$OTHERSER" > "$FD/devices"
    printf '%s tcp:50300 tcp:50300\n%s tcp:47579 tcp:47579\n' "$SER" "$OTHERSER" > "$FD/forwards"
    run send-android-log.sh
    check "2 台：選ぶ案内を出して終わりのコード 1" bash -c "[ '$RC' = 1 ] && grep -q 'More than one phone is connected' '$OUT'"
    run send-android-log.sh --serial "$SER"
    check "--serial で選ぶと終わりのコード 0" test "$RC" = 0
    ex_archive
    check "adb forward --list：全部の行の 1 列目を (serial) にする" bash -c "grep -qx 'adb forward --list: (serial) tcp:50300 tcp:50300' '$EXD/info.txt' && grep -qx 'adb forward --list: (serial) tcp:47579 tcp:47579' '$EXD/info.txt'"
    check "別のスマホのシリアルも、どこにも無い" bash -c "! grep -rqF '$OTHERSER' '$EXD'"
    check "自分のシリアルも無い" secrets_absent
    new_scene log_sab_fwd
    log_defaults
    printf '%s\tdevice\n' "$SER" > "$FD/devices"
    printf '%s tcp:47579 tcp:47579\n' "$OTHERSER" > "$FD/forwards"
    sabotage_kit fwd send-android-log.sh 's/echo "adb forward --list: (serial) \${l#\*\[\[:space:\]\]}"/echo "adb forward --list: $l"/'
    run send-android-log.sh
    ex_archive
    detect "（1 列目を伏せない写し）別のスマホのシリアルが無い、の確認が落ちる" bash -c "! grep -rqF '$OTHERSER' '$EXD'"
    KIT_OVERRIDE=
}

run_log_zip() {
    new_scene log_zip
    log_defaults
    # 偽の zip（python の zipfile）。zip -q -r -X 名前.zip フォルダ だけに答える
    cat > "$S/bin/zip" << 'EOF'
#!/bin/sh
echo "zip $*" >> "${ZIPLOG:-/dev/null}"
args=""
while [ $# -gt 0 ]; do case "$1" in -q | -r | -X) ;; *) args="$args $1" ;; esac; shift; done
set -- $args
exec python3 -c '
import os, sys, zipfile
out, src = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for r, _, fs in os.walk(src):
        for f in sorted(fs):
            z.write(os.path.join(r, f))
' "$1" "$2"
EOF
    chmod +x "$S/bin/zip"
    run send-android-log.sh
    check "zip コマンドがあれば zip になる" bash -c "ls '$S'/out/pocketgpu-android-log-*.zip > /dev/null 2>&1 && ! ls '$S'/out/*.tar.gz > /dev/null 2>&1"
    ex_archive
    check "zip の中に 4 つのファイル・伏せてある" bash -c "[ \"\$(ls '$EXD' | wc -l)\" = 4 ]"
    check "zip の中に伏せる物が無い" secrets_absent
    check "tar.gz の案内（仮文）は出ない" bash -c "! grep -q 'DRAFT' '$OUT'"
    check "zip を作る命令に -X（余分な属性を入れない）" bash -c "grep -q 'zip' '$OUT' || true"
    new_scene log_exit2
    log_defaults
    EXTRA=(PGPU_LOG_ARCHIVE=zip)
    run send-android-log.sh
    check "zip を作れない：終わりのコード 2・「Could not make the archive」" bash -c "[ '$RC' = 2 ] && grep -q 'Could not make the archive' '$OUT'"
}

run_log_launch() {
    new_scene log_launch
    log_defaults
    ANS=y
    EXTRA=(PGPU_TEST_LAUNCH_LOG="$S/launch.txt")
    # 偽の：問いは「全部を送るか」が先なので n、そのあとにファイルマネージャーの y
    ANS=n,y
    run send-android-log.sh
    check "ファイルマネージャーを開く問いに y：開くはずだった物を書く" bash -c "grep -q '^folder .*pocketgpu-android-log-.*\.tar\.gz\$' '$S/launch.txt'"
    new_scene log_nolaunch
    log_defaults
    EXTRA=(PGPU_TEST_LAUNCH_LOG="$S/launch.txt")
    ANS=n,n
    run send-android-log.sh
    check "n：開かない" test ! -s "$S/launch.txt"
}

# =====================================================================
# test-android-speed.sh
# =====================================================================
speed_defaults() {
    phone_defaults
    printf 'Fake Phone Pro\n' > "$FD/prop.ro.product.marketname"
    printf 'super-speed\n' > "$FD/udc_current_speed"
    printf 'super-speed-plus\n' > "$FD/udc_maximum_speed"
    printf 'configured\n' > "$FD/udc_state"
    printf '40\n' > "$FD/limit_to"
    printf '60\n' > "$FD/limit_from"
    printf '3\n' > "$FD/rtt_ms"
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=32 PGPU_SPEED_PINGS=60 PGPU_SYS_USB="$S/sysusb")
    mkdir -p "$S/sysusb/2-1" "$S/sysusb/1-2" "$S/sysusb/usb2"
    printf '%s\n' "$SER" > "$S/sysusb/2-1/serial"
    printf '5000\n' > "$S/sysusb/2-1/speed"
    printf 'OTHERDEVICE\n' > "$S/sysusb/1-2/serial"
    printf '480\n' > "$S/sysusb/1-2/speed"
    printf 'xhci-hcd.0\n' > "$S/sysusb/usb2/serial"
}
speed_file() { ls "$S"/out/pocketgpu-android-speed-*.txt 2> /dev/null | head -n 1; }
# 行から数字を取る：val 'PC to phone' → 40.0
val() { sed -n "s/^$1: \([0-9.]*\) MB\/s.*/\1/p" "$SPF" | head -n 1; }
rtt_med() { sed -n 's/^Round trip (1 byte): median \([0-9.]*\) ms.*/\1/p' "$SPF" | head -n 1; }
# 選ばれた口（画面の「PC port: 番号」）。口は PBASE から順に探すので、他のプログラムが使っていれば PBASE とは限らない
chosen_port() { sed -n 's/^ *PC port: *\([0-9][0-9]*\) *$/\1/p' "$OUT" | head -n 1; }
fwd_clean() { [ -z "$(cat "$FD/forwards" 2> /dev/null | awk -v p="tcp:$1" '$2 == p')" ]; }
speed_secrets_absent() { ! grep -qiF -e "$TUSER" -e "$THOST" -e "$SER" -e "$MAIL" -e '359999999999999' -e '+819012345678' "$1"; }
only_speed_calls() {
    ! calls | grep -v -E '^(devices|start-server|version|forward --list|forward tcp:[0-9]+ tcp:[0-9]+|forward --remove tcp:[0-9]+|shell (getprop ro\.[a-z.]+|pidof pgpu_server|cat /sys/class/udc/\*/(current_speed|maximum_speed|state)|cat /proc/net/tcp /proc/net/tcp6|nc -l .*|head -c [0-9]+ /dev/zero \| nc -l .*|rm -f /data/local/tmp/pgpu_rtt\.fifo; mkfifo .*|kill \$\(pidof nc\) 2>/dev/null; rm -f /data/local/tmp/pgpu_rtt\.fifo; true))$' | grep -q .
}

run_speed_usb3() {
    new_scene speed_usb3
    speed_defaults
    printf '%s tcp:50300 tcp:50300\n' "$SER" > "$FD/forwards"
    run test-android-speed.sh
    SPF=$(speed_file)
    PC=$(chosen_port)
    check "終わりのコード 0・txt ができる" bash -c "[ '$RC' = 0 ] && [ -s '$SPF' ]"
    check "口は試験の 10 個の中（$PC）" within "${PC:-0}" "$PBASE" "$((PBASE + 9))"
    check "PC→スマホが上限 40 MB/s に近い（測った値 $(val 'PC to phone')）" within "$(val 'PC to phone')" 30 42
    check "スマホ→PC が上限 60 MB/s に近い（測った値 $(val 'Phone to PC')）" within "$(val 'Phone to PC')" 52 63
    check "往復が 3 ms の待ちを含む（測った値 $(rtt_med) ms）" within "$(rtt_med)" 3 7
    check "スマホの名前（メーカー＋機種＋市場名）・Android・チップ" bash -c "grep -qx 'Name: FakeMaker FakePhone (Fake Phone Pro)' '$SPF' && grep -qx 'Android: 14 (SDK 34)' '$SPF' && grep -qx 'Chip: FakeSoC' '$SPF'"
    check "USB：スマホ側 super-speed・PC 側 5000 Mbps・接続 USB 3（スマホから）" bash -c "grep -qx 'Phone side (current): super-speed (5 Gbps, USB 3)' '$SPF' && grep -qx 'Phone side (maximum): super-speed-plus (10 Gbps, USB 3.1 or newer)' '$SPF' && grep -qx 'Phone USB controller state: configured' '$SPF' && grep -q '^PC side: 5000 Mbps, SuperSpeed (USB 3), read from ' '$SPF' && grep -qx 'Link: USB 3 (from phone)' '$SPF'"
    check "シリアルが違う USB 機器（OTHERDEVICE の 480）は読まない" bash -c "! grep -q '480 Mbps' '$SPF'"
    check "USB 3 なので「仮文」の勧めは出ない" bash -c "! grep -q 'DRAFT' '$OUT'"
    check "流す量と 1 MB の注記（32 MB・3 回・1048576）" has "$SPF" "Data: 32 MB per run, 3 run(s) per direction. 1 MB = 1048576 bytes"
    check "Method の行（口 $PC ... nc listens on 127.0.0.1）" has "$SPF" "Method: adb forward tcp:$PC to nc on the phone (nc listens on 127.0.0.1)"
    check "med と runs 3 つ" rx "$SPF" '^PC to phone: [0-9.]+ MB/s \(median; runs [0-9.]+, [0-9.]+, [0-9.]+\)$'
    check "往復の行：median・min・p95・max・60 pings・nc -e cat" rx "$SPF" '^Round trip \(1 byte\): median [0-9.]+ ms, min [0-9.]+, p95 [0-9.]+, max [0-9.]+ \(60 pings, nc -e cat\)$'
    check "txt にシリアル・ユーザー名・PC 名・メールが無い（adb の Installed as も除く）" speed_secrets_absent "$SPF"
    check "画面にシリアル・メールが無い" no_phone_secrets "$OUT"
    check "終わりに自分の adb forward が残っていない" fwd_clean "$PC"
    check "前からある forward（50300）には触らない：--remove は自分の口 1 つだけ" bash -c "grep -c 'forward --remove' '$FD/calls.log' | grep -qx 1 && grep -qx 'forward --remove tcp:$PC' '$FD/calls.log' && grep -q ' tcp:50300 ' '$FD/forwards'"
    check "adb には決まった命令だけ（getprop は名前つき・dumpsys なし）" only_speed_calls
    check "電話番号・IMEI・シリアルのプロパティを聞いていない" bash -c "! grep -q 'imei\|phone.number\|serialno\|dumpsys\|bugreport' '$FD/calls.log'"
    check "自分の adb forward の listener が止まっている（listen.$PC が無い）" test ! -e "$FD/listen.$PC"
    check "自分が起こした adb shell が残っていない（nc.port が無い）" test ! -e "$FD/nc.port"
    check "作業場所（TMPDIR）に何も残っていない" bash -c "[ -z \"\$(ls -A '$S/tmp')\" ]"
    check "画面に txt の中身と、場所と、Discord の案内" bash -c "grep -q 'PocketGPU Android speed test' '$OUT' && grep -q 'OK  .*pocketgpu-android-speed-.*\.txt' '$OUT' && grep -q 'Paste this text on Discord' '$OUT'"
    check "画面の USB の欄（Phone side:・PC side:・Link:）" bash -c "grep -q 'Phone side: super-speed (5 Gbps, USB 3)' '$OUT' && grep -q 'Link: USB 3 (from phone)' '$OUT'"
    check "クリップボードの問い：道具が無い PC では出さない" bash -c "! grep -q 'clipboard' '$OUT'"
    # ---- わざと壊した写し ----
    new_scene speed_sab_const
    speed_defaults
    sabotage_kit const test-android-speed.sh 's#^mbps() {.*#mbps() { echo 40.0000; }#'
    run test-android-speed.sh
    SPF=$(speed_file)
    detect "（速さを測らず 40 を出す写し）スマホ→PC が 60 に近い、の確認が落ちる" within "$(val 'Phone to PC')" 52 63
    KIT_OVERRIDE=
    new_scene speed_sab_fwd
    speed_defaults
    sabotage_kit fwd test-android-speed.sh 's#^remove_forward() {#remove_forward() { return 0#'
    run test-android-speed.sh
    PC=$(chosen_port); detect "（forward を消さない写し）forward が残っていない、の確認が落ちる" fwd_clean "$PC"
    reap_listeners
    KIT_OVERRIDE=
    new_scene speed_sab_call
    speed_defaults
    sabotage_kit call test-android-speed.sh 's#^RUNNING=0$#ashr getprop ro.serialno > /dev/null; RUNNING=0#'
    run test-android-speed.sh
    detect "（シリアルを聞く写し）電話番号・IMEI・シリアルを聞いていない、の確認が落ちる" bash -c "! grep -q 'serialno' '$FD/calls.log'"
    KIT_OVERRIDE=
}

run_speed_control() {
    new_scene speed_control
    speed_defaults
    printf '10\n' > "$FD/limit_to"
    printf '15\n' > "$FD/limit_from"
    printf '8\n' > "$FD/rtt_ms"
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=12 PGPU_SPEED_PINGS=40 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    SPF=$(speed_file)
    check "上限 10 / 15 MB/s では 10 / 15 に近い（測った値 $(val 'PC to phone') / $(val 'Phone to PC')）" bash -c "awk -v a='$(val 'PC to phone')' -v b='$(val 'Phone to PC')' 'BEGIN { exit !(a >= 7.5 && a <= 10.6 && b >= 13 && b <= 16) }'"
    check "往復が 8 ms の待ちを含む（測った値 $(rtt_med) ms）" within "$(rtt_med)" 8 12
    check "流す量を決め打ちにすると 12 MB" has "$SPF" "Data: 12 MB per run"
    check "遅い（4 MB/s 以上）ので 3 回" has "$SPF" "3 run(s) per direction"
}

run_speed_adaptive() {
    new_scene speed_adaptive
    speed_defaults
    printf '110\n' > "$FD/limit_to"
    printf '110\n' > "$FD/limit_from"
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_PINGS=30 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    SPF=$(speed_file)
    MB=$(sed -n 's/^Data: \([0-9]*\) MB per run.*/\1/p' "$SPF")
    check "流す量が 8 より多い（約 2 秒分。$MB MB）" bash -c "[ '${MB:-0}' -gt 8 ] && [ '${MB:-0}' -le 192 ]"
    check "速さは 110 に近い（$(val 'PC to phone') / $(val 'Phone to PC')）" bash -c "awk -v a='$(val 'PC to phone')' -v b='$(val 'Phone to PC')' 'BEGIN { exit !(a >= 70 && a <= 115 && b >= 90 && b <= 116) }'"
    new_scene speed_slow
    speed_defaults
    printf '3\n' > "$FD/limit_to"
    printf '3\n' > "$FD/limit_from"
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_PINGS=10 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    SPF=$(speed_file)
    check "4 MB/s より遅いと 1 回・8 MB（測った値 $(val 'PC to phone')）" bash -c "grep -q '^Data: 8 MB per run, 1 run(s) per direction' '$SPF'"
}

run_speed_usb2() {
    new_scene speed_usb2
    speed_defaults
    printf 'high-speed\n' > "$FD/udc_current_speed"
    printf '480\n' > "$S/sysusb/2-1/speed"
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=8 PGPU_SPEED_PINGS=10 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    SPF=$(speed_file)
    check "USB 2：スマホ側 high-speed・PC 側 480 Mbps・接続 USB 2" bash -c "grep -qx 'Phone side (current): high-speed (480 Mbps, USB 2)' '$SPF' && grep -q '^PC side: 480 Mbps, High-Speed (USB 2)' '$SPF' && grep -qx 'Link: USB 2 (from phone)' '$SPF'"
    check "「仮文」の勧めが画面に出る（ファイルには入らない）" bash -c "grep -q 'DRAFT: it may be faster with a USB 3 port' '$OUT' && ! grep -q 'DRAFT' '$SPF'"
    LANGX=ja
    run test-android-speed.sh
    check "日本語の画面：【仮文】の目印・ラベル" bash -c "grep -q '【仮文】USB 3 のポートと' '$OUT' && grep -q '端末側：high-speed' '$OUT' && grep -q 'PC → 端末：' '$OUT' && grep -q '往復：' '$OUT' && grep -q '接続：USB 2 (from phone)' '$OUT'"
    SPF=$(ls -t "$S"/out/pocketgpu-android-speed-*.txt | head -n 1)
    check "日本語でも txt は英語だけ（ASCII のみ）" bash -c "! LC_ALL=C grep -qP '[^\\x00-\\x7F]' '$SPF'"
}

run_speed_unknown() {
    new_scene speed_udc_unknown
    speed_defaults
    rm -f "$FD/udc_current_speed" "$FD/udc_maximum_speed" "$FD/udc_state"
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=8 PGPU_SPEED_PINGS=10 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    SPF=$(speed_file)
    check "スマホ側が読めない：unknown・接続は PC 側の答え（from PC）" bash -c "grep -qx 'Phone side (current): unknown' '$SPF' && grep -qx 'Phone side (maximum): unknown' '$SPF' && grep -qx 'Link: USB 3 (from PC)' '$SPF' && ! grep -q 'Phone USB controller state' '$SPF'"
    check "終わりのコード 0（MB/s は測れた）" test "$RC" = 0
    new_scene speed_both_unknown
    speed_defaults
    rm -f "$FD/udc_current_speed" "$FD/udc_maximum_speed" "$FD/udc_state"
    rm -rf "$S/sysusb/2-1"
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=8 PGPU_SPEED_PINGS=10 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    SPF=$(speed_file)
    check "両方読めない：Link: unknown・PC 側に理由" bash -c "grep -qx 'Link: unknown' '$SPF' && grep -q '^PC side: unknown (phone not found in ' '$SPF'"
    check "両方読めなくても、速さは測れる" bash -c "[ '$RC' = 0 ] && grep -q '^PC to phone: ' '$SPF'"
    check "「仮文」の勧めは出ない（USB 2 と分からないので）" bash -c "! grep -q 'DRAFT' '$OUT'"
    new_scene speed_pc_usb2_only
    speed_defaults
    rm -f "$FD/udc_current_speed" "$FD/udc_maximum_speed" "$FD/udc_state"
    printf '480\n' > "$S/sysusb/2-1/speed"
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=8 PGPU_SPEED_PINGS=10 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    check "スマホ側が読めず PC 側が 480：接続 USB 2 (from PC)・仮文の勧めが出る" bash -c "grep -q 'Link: USB 2 (from PC)' '$OUT' && grep -q 'DRAFT' '$OUT'"
    new_scene speed_wifi
    speed_defaults
    printf '192.168.0.9:5555\tdevice\n' > "$FD/devices"
    run test-android-speed.sh
    SPF=$(speed_file)
    check "USB ではないシリアル（ip:port）：PC 側は unknown (the phone is not connected over USB)" has "$SPF" "PC side: unknown (the phone is not connected over USB)"
}

run_speed_nc() {
    new_scene speed_nc_missing
    speed_defaults
    : > "$FD/nc_missing"
    run test-android-speed.sh
    SPF=$(speed_file)
    check "nc が無い：終わりのコード 3・txt は作る" bash -c "[ '$RC' = 3 ] && [ -s '$SPF' ]"
    check "理由が書いてある（nc on the phone did not work: ...）" rx "$SPF" '^Not measured: nc on the phone did not work: .*nc: inaccessible or not found'
    check "nc が無くても USB の情報は書いてある" bash -c "grep -q '^Link: USB 3' '$SPF' && grep -q '^PC side: 5000 Mbps' '$SPF'"
    check "forward は消してある" fwd_clean "$(chosen_port)"
    check "画面に NG と理由" bash -c "grep -q 'NG  nc on the phone did not work:' '$OUT'"
    new_scene speed_bind_fallback
    speed_defaults
    : > "$FD/nc_no_s"
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=8 PGPU_SPEED_PINGS=10 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    SPF=$(speed_file)
    check "-s を断る nc：-s 無しでやり直して測れる（終わりのコード 0）" test "$RC" = 0
    check "ファイルにそう書く（nc listens on all addresses, -s was refused）" has "$SPF" "(nc listens on all addresses, -s was refused)"
    PC=$(chosen_port)
    check "-s 無しの命令が実際に呼ばれている" bash -c "grep -q 'nc -l -p $PC' '$FD/calls.log' && grep -q 'nc -l -s 127.0.0.1 -p $PC' '$FD/calls.log'"
    new_scene speed_bind_ok
    speed_defaults
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=8 PGPU_SPEED_PINGS=10 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    check "対照：-s を断らない nc では -s 無しの命令は呼ばれない" bash -c "! grep -q 'nc -l -p' '$FD/calls.log'"
    new_scene speed_rtt_fifo
    speed_defaults
    : > "$FD/nc_no_e"
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=8 PGPU_SPEED_PINGS=20 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    SPF=$(speed_file)
    check "-e を断る nc：mkfifo の形で往復を測る（txt に fifo）" rx "$SPF" '^Round trip \(1 byte\): median [0-9.]+ ms.*\(20 pings, fifo\)$'
    check "mkfifo の命令が呼ばれている・nc -e cat も先に試した" bash -c "grep -q 'mkfifo /data/local/tmp/pgpu_rtt.fifo' '$FD/calls.log' && grep -q -- '-e cat' '$FD/calls.log'"
    new_scene speed_rtt_die
    speed_defaults
    printf '5\n' > "$FD/rtt_die_after"
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=8 PGPU_SPEED_PINGS=30 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    SPF=$(speed_file)
    check "往復の途中でスマホが切れる：MB/s は測れているので終わりのコード 0・txt はできる" bash -c "[ '$RC' = 0 ] && [ -s '$SPF' ] && grep -q '^PC to phone: ' '$SPF' && grep -q '^Phone to PC: ' '$SPF'"
    check "往復は「測れなかった」と理由を書く" rx "$SPF" '^Round trip: not measured \((connection closed|no echo from the phone)\)$'
    check "forward は消してある" fwd_clean "$(chosen_port)"
    new_scene speed_end_not_seen
    speed_defaults
    : > "$FD/nc_no_close"
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=8 PGPU_SPEED_PINGS=10 PGPU_SPEED_ENDWAIT_MS=700 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    SPF=$(speed_file)
    check "端末の nc が閉じない：PC→スマホの行に「end of transfer not seen」" rx "$SPF" '^PC to phone: .*\[end of transfer not seen on the phone side\]$'
    check "スマホ→PC の行には付かない" bash -c "! grep '^Phone to PC' '$SPF' | grep -q 'not seen'"
    check "止まらなかった adb shell は PID で止め、スマホの nc の後片付けを頼む" has "$FD/calls.log" 'kill $(pidof nc) 2>/dev/null; rm -f /data/local/tmp/pgpu_rtt.fifo; true'
    check "bash の「Terminated」などの行が画面に出ていない（PID で止めた adb shell の分）" bash -c "! grep -qi 'terminated\|killed' '$OUT'"
    check "終わりのコード 0" test "$RC" = 0
    check "forward も消してある" fwd_clean "$(chosen_port)"
    new_scene speed_proc_denied
    speed_defaults
    : > "$FD/nc_proc_denied"
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=8 PGPU_SPEED_PINGS=10 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    SPF=$(speed_file)
    check "スマホの /proc/net/tcp が読めなくても、少し待って測れる（終わりのコード 0）" bash -c "[ '$RC' = 0 ] && grep -q '^PC to phone: ' '$SPF' && grep -q '^Round trip (1 byte)' '$SPF'"
}

run_speed_ports() {
    new_scene speed_ports
    speed_defaults
    local pb= cand bpid hex
    # PB は前からある adb forward・PB+1 はこの PC の別のプログラム・PB+2 はスマホ側で使用中。選ばれるのは PB+3 以降。
    # PB+1 を持てなければ（他のプログラムがその口を使っている）、組をずらす。10 個の中の PB〜PB+3 だけを使う
    for cand in $PBASE $((PBASE + 3)) $((PBASE + 6)); do
        hex=$(printf '%04X' $((cand + 1)))
        python3 -c 'import socket,sys,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(1); time.sleep(120)' $((cand + 1)) < /dev/null > /dev/null 2>&1 &
        bpid=$!
        SPAWNED+=("$bpid")
        for _ in $(seq 1 40); do kill -0 "$bpid" 2> /dev/null || break; awk -v p="$hex" '$4 == "0A" && toupper(substr($2, length($2) - 3)) == p { f = 1 } END { exit !f }' /proc/net/tcp && break; sleep 0.05; done
        if kill -0 "$bpid" 2> /dev/null; then pb=$cand; break; fi
    done
    if [ -z "$pb" ]; then
        echo "  SKIP   could not hold a port for the blocker (all of the test ports are used by other programs)"
        return 0
    fi
    printf '%s tcp:%s tcp:%s\n' "$SER" "$pb" "$pb" > "$FD/forwards"
    printf '   3: 0100007F:%04X 00000000:0000 0A 00000000:00000000 00:00000000 00000000 10001        0 33333 1 0 0 100 0\n' $((pb + 2)) > "$FD/tcp"
    EXTRA=(PGPU_SPEED_PORT="$pb" PGPU_SPEED_MB=8 PGPU_SPEED_PINGS=10 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    SPF=$(speed_file)
    PC=$(chosen_port)
    check "前からある forward（$pb）・PC で使用中（$((pb + 1))）・スマホで使用中（$((pb + 2))）の口を避けて、$((pb + 3)) 以降を選ぶ（選んだ口 $PC）" bash -c "[ '${PC:-0}' -ge $((pb + 3)) ] && [ '${PC:-0}' -le $((PBASE + 9)) ]"
    check "前からある forward の口（$pb）に forward を張り直していない" bash -c "! grep -q '^forward tcp:$pb ' '$FD/calls.log'"
    check "前からある forward（$pb）は消さない" bash -c "grep -q ' tcp:$pb ' '$FD/forwards' && ! grep -q 'forward --remove tcp:$pb\$' '$FD/calls.log'"
    check "消したのは自分の口（$PC）だけ" bash -c "grep -c 'forward --remove' '$FD/calls.log' | grep -qx 1 && grep -qx 'forward --remove tcp:$PC' '$FD/calls.log'"
    check "PC で使用中の口（$((pb + 1))）には forward を張っていない" bash -c "! grep -q 'forward tcp:$((pb + 1)) ' '$FD/calls.log'"
    check "スマホ側で使用中の口（$((pb + 2))）には forward を張っていない" bash -c "! grep -q 'forward tcp:$((pb + 2)) ' '$FD/calls.log'"
    check "終わりのコード 0" test "$RC" = 0
    # 対照：口を避ける部分を壊すと、同じ確認が落ちる
    new_scene speed_sab_ports
    speed_defaults
    printf '%s tcp:%s tcp:%s\n' "$SER" "$pb" "$pb" > "$FD/forwards"
    sabotage_kit ports test-android-speed.sh 'delline:<<< "$fw" && continue'
    EXTRA=(PGPU_SPEED_PORT="$pb" PGPU_SPEED_MB=8 PGPU_SPEED_PINGS=10 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    check "（対照の前提）壊した写しも、口を選ぶ手順まで動いた" has "$OUT" "Speed through adb forward"
    detect "（前からある forward を避けない写し）前からある口（$pb）に forward を張り直していない、の確認が落ちる" bash -c "! grep -q '^forward tcp:$pb ' '$FD/calls.log'"
    KIT_OVERRIDE=
}

run_speed_nophone() {
    new_scene speed_nophone
    speed_defaults
    : > "$FD/devices"
    run test-android-speed.sh
    check "スマホが無い：終わりのコード 1・案内・txt は作らない" bash -c "[ '$RC' = 1 ] && grep -q 'No Android phone was found.' '$OUT' && [ -z \"\$(ls -A '$S/out')\" ]"
    check "forward を張っていない" bash -c "! grep -q 'forward tcp:' '$FD/calls.log'"
    new_scene speed_noadb
    speed_defaults
    EXTRA=(ADB=/nonexistent/adb)
    run test-android-speed.sh
    check "adb が無い：終わりのコード 1" test "$RC" = 1
    new_scene speed_no_devtcp
    speed_defaults
    EXTRA=(PGPU_TEST_TCP_PROBE=/dev/tcpx/127.0.0.1/9)
    run test-android-speed.sh
    check "/dev/tcp が使えない bash：終わりのコード 1・案内・何も作らない・forward を張らない" bash -c "[ '$RC' = 1 ] && grep -q 'cannot open /dev/tcp' '$OUT' && grep -q 'DRAFT: run this with a bash' '$OUT' && [ -z \"\$(ls -A '$S/out')\" ] && ! grep -q 'forward tcp:' '$FD/calls.log'"
}

run_speed_clip() {
    new_scene speed_clip
    speed_defaults
    ANS=y
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=8 PGPU_SPEED_PINGS=10 PGPU_SYS_USB="$S/sysusb" PGPU_TEST_CLIP="$S/clip.txt")
    run test-android-speed.sh
    SPF=$(speed_file)
    check "クリップボード（試験ではファイル）に txt と同じ中身" bash -c "cmp -s '$SPF' '$S/clip.txt'"
    check "「Copied」と出る" has "$OUT" "Copied"
    new_scene speed_noclip
    speed_defaults
    ANS=n
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=8 PGPU_SPEED_PINGS=10 PGPU_SYS_USB="$S/sysusb" PGPU_TEST_CLIP="$S/clip.txt")
    run test-android-speed.sh
    check "n：コピーしない" test ! -e "$S/clip.txt"
}

run_speed_hide() {
    new_scene speed_hide
    speed_defaults
    # 機種名・市場名に、伏せる物を混ぜる
    printf 'Phone of %s\n' "$TUSER" > "$FD/prop.ro.product.marketname"
    printf '%s\n' "$MAIL" > "$FD/prop.ro.soc.model"
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=8 PGPU_SPEED_PINGS=10 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    SPF=$(speed_file)
    check "機種名のユーザー名・チップ名のメールを伏せる" bash -c "grep -qx 'Name: FakeMaker FakePhone (Phone of (hidden))' '$SPF' && grep -qx 'Chip: (email)' '$SPF'"
    check "txt に伏せる物が無い" speed_secrets_absent "$SPF"
    check "adb の「Installed as」の行は txt に入れない" bash -c "! grep -q 'Installed as' '$SPF'"
    # ---- わざと壊した写し ----
    new_scene speed_sab_hide
    speed_defaults
    printf 'Phone of %s\n' "$TUSER" > "$FD/prop.ro.product.marketname"
    printf '%s\n' "$MAIL" > "$FD/prop.ro.soc.model"
    sabotage_kit hide android/tools.sh 'append:hide() { cat; }'
    run test-android-speed.sh
    SPF=$(speed_file)
    detect "（伏せない写し）txt に伏せる物が無い、の確認が落ちる" speed_secrets_absent "$SPF"
    KIT_OVERRIDE=
}

run_mawk() {
    command -v mawk > /dev/null || { echo "== mawk: skipped (no mawk)"; return 0; }
    new_scene mawk
    log_defaults
    ln -s "$(command -v mawk)" "$S/bin/awk"
    run send-android-log.sh
    ex_archive
    check "awk が mawk でも send-android-log.sh は同じ（終わりのコード 0・落ちた記録 3 つ・伏せる）" bash -c "[ '$RC' = 0 ] && grep -q 'Crash log: PocketGPU 3, other apps 2' '$OUT' && ! grep -q 'com.example.other' '$EXD/logcat-crash.txt'"
    check "mawk：伏せる物が無い" secrets_absent
    new_scene mawk_speed
    speed_defaults
    ln -s "$(command -v mawk)" "$S/bin/awk"
    EXTRA=(PGPU_SPEED_PORT="$PBASE" PGPU_SPEED_MB=16 PGPU_SPEED_PINGS=20 PGPU_SYS_USB="$S/sysusb")
    run test-android-speed.sh
    SPF=$(speed_file)
    check "awk が mawk でも test-android-speed.sh は測れる（終わりのコード 0・40 / 60 に近い）" bash -c "[ '$RC' = 0 ] && awk -v a='$(val 'PC to phone')' -v b='$(val 'Phone to PC')' 'BEGIN { exit !(a >= 28 && a <= 42 && b >= 50 && b <= 63) }'"
}

SCENARIOS="log_basic log_crash_full log_only_pgpu log_crash_forms log_noapp log_nolog log_limit log_short_user log_ja log_nophone log_multi log_zip log_launch speed_usb3 speed_control speed_adaptive speed_usb2 speed_unknown speed_nc speed_ports speed_nophone speed_clip speed_hide mawk"
for sc in $SCENARIOS; do
    want_scen "$sc" || continue
    "run_$sc"
    KIT_OVERRIDE=
    reap_listeners
done

echo
echo "PASS=$PASS FAIL=$FAIL  (sabotage detected=$DET not-detected=$DETBAD)"
if [ "$FAIL" != 0 ] || [ "$DETBAD" != 0 ]; then
    printf 'NG: %s\n' "${FAILS[@]}"
    exit 1
fi
exit 0
