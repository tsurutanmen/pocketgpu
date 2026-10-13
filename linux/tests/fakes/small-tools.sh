#!/usr/bin/env bash
# 試験用の小さな偽の道具を 1 つにまとめた物。呼ばれた名前（$0 の名前）で振る舞いを変える。run-tests.sh が、名前ごとに場面の bin/ へリンクを張る。
#   usbmuxd       あるだけ（「入っている」のふり）。動かすと何もせずに終わる
#   idevicepair   pair：$FAKE_DIR/trust があれば $FAKE_DIR/paired を作る（iPhone で「信頼」を押したふり）。UDID の形の文字を出す
#   systemctl     is-active：inactive を返す。ほかは何もしない
#   vulkaninfo    --summary：$FAKE_DIR/vulkaninfo の版を出す（無ければ失敗）
#   pactl         $FAKE_DIR/pactl/server（サーバーの名前）と $FAKE_DIR/pactl/modules（list short modules の行）
#   id            -nG：$FAKE_DIR/groups_now・-nG ユーザー：$FAKE_DIR/groups_db。ほかは本物の id
#   sudo          呼ばれたら $FAKE_DIR/sudo.log に書いて失敗する（fix.sh は sudo を実行しないはず）
#   modinfo       $FAKE_DIR/modinfo があれば、その中身を出す
set -u
D=${FAKE_DIR:?}
name=$(basename -- "$0")
printf '%s %s\n' "$name" "$*" >> "$D/tools.log"
case "$name" in
usbmuxd) exit 0 ;;
idevicepair)
    if [ "${1:-}" = pair ]; then
        if [ -f "$D/trust" ]; then
            touch "$D/paired"
            echo "SUCCESS: Paired with device 00008110-001234567890ABCD"
            exit 0
        fi
        echo "ERROR: Please accept the trust dialog on the screen of device 00008110-001234567890ABCD, then attempt to pair again."
        exit 1
    fi
    exit 0
    ;;
systemctl) [ "${1:-}" = is-active ] && { echo inactive; exit 3; }; exit 0 ;;
vulkaninfo)
    [ -f "$D/vulkaninfo" ] || exit 1
    echo "Vulkan Instance Version: $(cat "$D/vulkaninfo")"
    ;;
pactl)
    P=$D/pactl
    [ -f "$P/server" ] || { echo "Connection failure: Connection refused" >&2; exit 1; }
    case "${1:-}" in
    info) echo "Server Name: $(cat "$P/server")" ;;
    list) [ -f "$P/modules" ] && cat "$P/modules" ;;
    unload-module)
        awk -F'\t' -v m="${2:-}" '$1 != m' "$P/modules" > "$P/modules.tmp" && mv "$P/modules.tmp" "$P/modules"
        ;;
    load-module) echo 99 ;;
    esac
    ;;
id)
    if [ "${1:-}" = -nG ] && [ $# -eq 1 ] && [ -f "$D/groups_now" ]; then cat "$D/groups_now"; exit 0; fi
    if [ "${1:-}" = -nG ] && [ $# -eq 2 ] && [ -f "$D/groups_db" ]; then cat "$D/groups_db"; exit 0; fi
    exec /usr/bin/id "$@"
    ;;
sudo) echo "$*" >> "$D/sudo.log"; exit 1 ;;
modinfo) [ -f "$D/modinfo" ] && { cat "$D/modinfo"; exit 0; }; exit 1 ;;
esac
exit 0
