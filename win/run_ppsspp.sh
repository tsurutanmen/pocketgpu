#!/bin/bash
# PPSSPP 1.20.4（GPL-2.0・PSP の写し）を Vulkan で開き、決まった秒数だけ回す。ゲームは使わず、Vulkan で描く最初の画面を測る。
#   win/run_ppsspp.sh [受け手 または direct] [秒]
#   受け手を省くと usb（iPhone）。direct なら RTX に直接
P=/d/iphone-rpc/games/ppsspp
VK=${1:-usb}
SECS=${2:-20}
OUT=/d/iphone-rpc/vkbuild/game_runs
mkdir -p "$OUT"
LOG="$OUT/ppsspp_${VK//[:.]/_}_$(date +%m%d_%H%M%S).txt"
export IPHONE_VK_MONITOR=${IPHONE_VK_MONITOR:-sub}
export VK_LOADER_LAYERS_DISABLE='~implicit~'
cd "$P" || exit 1
ARGS=(--graphics=vulkan --windowed)
if [ "$VK" = direct ]; then
    ./PPSSPPWindows64.exe "${ARGS[@]}" > "$LOG" 2>&1 &
else
    IPHONE_VK=$VK IPHONE_VK_ONLY=1 IPHONE_VK_VSYNC=0 IPHONE_VK_LOG=1 IPHONE_VK_STATS=1 \
        cmd.exe //c ${IPHONE_VK_CMD:-D:/iphone-rpc/vkbuild/iphone-vk.cmd} "$(cygpath -w "$P/PPSSPPWindows64.exe")" "${ARGS[@]}" > "$LOG" 2>&1 &
fi
sleep "$SECS"
taskkill //IM PPSSPPWindows64.exe > /dev/null 2>&1
for i in $(seq 1 20); do tasklist | grep -qi PPSSPPWindows64.exe || break; sleep 0.5; done
taskkill //IM PPSSPPWindows64.exe //F > /dev/null 2>&1
wait
echo "$(grep -a -m1 "受け手の GPU" "$LOG" | sed 's/\[iphone-vk\] //')・$(grep -a "swapchain：.*コマ/秒" "$LOG" | tail -1 | sed 's/\[iphone-vk\] //')"
grep -a -i "失敗\|切れた\|作れない" "$LOG" | head -5
echo "記録：$LOG"
