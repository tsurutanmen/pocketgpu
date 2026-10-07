#!/bin/bash
# Veloren（GPL-3.0・ボクセルの RPG・wgpu）を Vulkan で開き、決まった秒数だけ回す。最初のメニュー画面を測る。
#   win/run_veloren.sh [受け手 または direct] [秒]
#   受け手を省くと usb（iPhone）。direct なら RTX に直接
V=/d/iphone-rpc/games/veloren
VK=${1:-usb}
SECS=${2:-30}
OUT=/d/iphone-rpc/vkbuild/game_runs
mkdir -p "$OUT"
LOG="$OUT/veloren_${VK//[:.]/_}_$(date +%m%d_%H%M%S).txt"
export IPHONE_VK_MONITOR=${IPHONE_VK_MONITOR:-sub}
export VK_LOADER_LAYERS_DISABLE='~implicit~'
# wgpu に Vulkan を使わせる
export WGPU_BACKEND=vulkan
export RUST_LOG=${RUST_LOG:-info}
cd "$V" || exit 1
if [ "$VK" = direct ]; then
    ./veloren-voxygen.exe > "$LOG" 2>&1 &
else
    IPHONE_VK=$VK IPHONE_VK_ONLY=1 IPHONE_VK_VSYNC=0 IPHONE_VK_LOG=1 IPHONE_VK_STATS=1 \
        cmd.exe //c ${IPHONE_VK_CMD:-D:/iphone-rpc/vkbuild/iphone-vk.cmd} "$(cygpath -w "$V/veloren-voxygen.exe")" > "$LOG" 2>&1 &
fi
sleep "$SECS"
taskkill //IM veloren-voxygen.exe > /dev/null 2>&1
for i in $(seq 1 20); do tasklist | grep -qi veloren-voxygen.exe || break; sleep 0.5; done
taskkill //IM veloren-voxygen.exe //F > /dev/null 2>&1
wait
echo "$(grep -a -m1 "受け手の GPU" "$LOG" | sed 's/\[iphone-vk\] //')・$(grep -a "swapchain：.*コマ/秒" "$LOG" | tail -1 | sed 's/\[iphone-vk\] //')"
grep -a -i "adapter\|backend\|panic\|error\|失敗\|切れた" "$LOG" | grep -v "まだ無い入口\|入口が無い" | head -6 | cut -c1-200
echo "記録：$LOG"
