#!/bin/bash
# vkQuake の timedemo（デモを全速で流してコマ数を測る）を動かす。データは無料の LibreQuake（BSD-3）。
#   win/run_vkquake.sh [受け手 または direct] [デモ] [幅] [高さ]
#   受け手を省くと usb（iPhone）。direct なら RTX に直接
# 結果が出たら止める（デモはそのあと繰り返し流れ続けるため）
G=/d/iphone-rpc/games/lq/lite
VK=${1:-usb}
DEMO=${2:-demo1}
W=${3:-1280}
H=${4:-720}
OUT=/d/iphone-rpc/vkbuild/game_runs
mkdir -p "$OUT"
LOG="$OUT/vkquake_${VK//[:.]/_}_${DEMO}_${W}x${H}_$(date +%m%d_%H%M%S).txt"
export IPHONE_VK_MONITOR=${IPHONE_VK_MONITOR:-sub}
cd "$G" || exit 1
# 結果は vkQuake の記録（-condebug）に書かれる。標準出力はファイルに向けると溜めたまま止められて出ない
QLOG="$APPDATA/vkQuake/qconsole.log"
rm -f "$QLOG"
ARGS=(-basedir . -condebug -window -width "$W" -height "$H" +timedemo "$DEMO")
if [ "$VK" = direct ]; then
    ./vkQuake.exe "${ARGS[@]}" > "$LOG" 2>&1 &
else
    # iPhone だけを見せる（vkQuake は最初の GPU を使う）。速さを測るので、画面の書き換えに合わせない
    IPHONE_VK=$VK IPHONE_VK_ONLY=1 IPHONE_VK_VSYNC=0 IPHONE_VK_LOG=1 IPHONE_VK_STATS=1 \
        cmd.exe //c D:/iphone-rpc/vkbuild/iphone-vk.cmd "$(cygpath -w "$G/vkQuake.exe")" "${ARGS[@]}" > "$LOG" 2>&1 &
fi
for i in $(seq 1 600); do
    grep -aq "frames .* seconds .* fps" "$QLOG" 2> /dev/null && break
    grep -aq "落ちた\|切れた" "$LOG" && break
    sleep 0.5
done
sleep 1
# ふつうに閉じる（ドライバの統計は閉じるときに出る）。閉じなければ止める
taskkill //IM vkQuake.exe > /dev/null 2>&1
for i in $(seq 1 20); do tasklist | grep -qi vkQuake.exe || break; sleep 0.5; done
taskkill //IM vkQuake.exe //F > /dev/null 2>&1
wait
cp -f "$QLOG" "${LOG%.txt}_qconsole.log" 2> /dev/null
echo "$(grep -a -m1 "^Device:" "$QLOG")・$(grep -a -m1 "frames .* seconds .* fps" "$QLOG" || echo "結果なし（$(grep -a "落ちた\|切れた\|ERROR\|Error" "$LOG" "$QLOG" 2> /dev/null | head -1)）")"
echo "記録：$LOG"
