#!/bin/bash
# SuperTuxKart 1.5（GPL-3.0）を Vulkan で、決まった秒数だけ自動で走らせる（--profile-time）。終わると平均のコマ数を書く。
#   win/run_stk.sh [受け手 または direct] [コース] [秒] [カートの数]
#   受け手を省くと usb（iPhone）。direct なら RTX に直接
S=/d/iphone-rpc/games/stk/SuperTuxKart-1.5-win/stk-code/build-x86_64/bin
VK=${1:-usb}
TRACK=${2:-lighthouse}
SECS=${3:-20}
KARTS=${4:-4}
OUT=/d/iphone-rpc/vkbuild/game_runs
mkdir -p "$OUT"
LOG="$OUT/stk_${VK//[:.]/_}_${TRACK}_$(date +%m%d_%H%M%S).txt"
export IPHONE_VK_MONITOR=${IPHONE_VK_MONITOR:-sub}
export VK_LOADER_LAYERS_DISABLE='~implicit~'
cd "$S" || exit 1
ARGS=(--render-driver=vulkan --windowed --screensize=1280x720 --track="$TRACK" --numkarts="$KARTS" --profile-time="$SECS")
if [ "$VK" = direct ]; then
    timeout 300 ./supertuxkart.exe "${ARGS[@]}" > "$LOG" 2>&1
else
    IPHONE_VK=$VK IPHONE_VK_ONLY=1 IPHONE_VK_VSYNC=0 IPHONE_VK_LOG=1 IPHONE_VK_STATS=1 \
        timeout 300 cmd.exe //c D:/iphone-rpc/vkbuild/iphone-vk.cmd "$(cygpath -w "$S/supertuxkart.exe")" "${ARGS[@]}" > "$LOG" 2>&1
fi
echo "終わり方 $?・$(grep -a -m1 "Vulkan renderer" "$LOG" | sed 's/.*renderer: //')・$(grep -a "Average FPS" "$LOG" | sed 's/.*profile: //')"
grep -a -i "\[error\|\[fatal\|失敗\|切れた" "$LOG" | grep -v "まだ無い入口\|入口が無い" | head -5
echo "記録：$LOG"
