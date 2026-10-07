#!/bin/bash
# Godot 4 の公式の見本を、ドライバ越しに Vulkan で動かす。毎秒のコマ数を Godot に出させる。
#   win/run_godot.sh [受け手 または direct] [見本のフォルダ] [描き方 mobile か forward_plus] [回す回数]
#   受け手を省くと usb（iPhone）。direct なら RTX に直接
#   見本のフォルダは D:/iphone-rpc/games/godot/demos/ の下（例 3d_squash_the_creeps/squash_the_creeps）。先に --headless --import が要る
G=/d/iphone-rpc/games/godot
VK=${1:-usb}
DEMO=${2:-3d_squash_the_creeps/squash_the_creeps}
METHOD=${3:-mobile}
ITER=${4:-1200}
OUT=/d/iphone-rpc/vkbuild/game_runs
mkdir -p "$OUT"
TAGN="godot_${VK//[:.]/_}_$(basename "$DEMO")_${METHOD}_$(date +%m%d_%H%M%S)"
LOG="$OUT/$TAGN.txt"
export IPHONE_VK_MONITOR=${IPHONE_VK_MONITOR:-sub}
export VK_LOADER_LAYERS_DISABLE='~implicit~'
cd "$G" || exit 1
EXE="$(cygpath -w "$G/Godot_v4.7.2-stable_win64_console.exe")"
# --quit-after は回す回数（物理の 1 回ごと）で終わる。--print-fps で毎秒のコマ数が出る
ARGS=(--path "$(cygpath -w "$G/demos/$DEMO")" --rendering-driver vulkan --rendering-method "$METHOD" --resolution 1280x720 --print-fps --quit-after "$ITER")
if [ "$VK" = direct ]; then
    timeout 300 "$G/Godot_v4.7.2-stable_win64_console.exe" "${ARGS[@]}" > "$LOG" 2>&1
else
    IPHONE_VK=$VK IPHONE_VK_ONLY=1 IPHONE_VK_VSYNC=0 IPHONE_VK_LOG=1 IPHONE_VK_STATS=1 \
        timeout 300 cmd.exe //c D:/iphone-rpc/vkbuild/iphone-vk.cmd "$EXE" "${ARGS[@]}" > "$LOG" 2>&1
fi
echo "終わり方 $?・$(grep -a -m1 "Vulkan.*- \|Forward\|Mobile" "$LOG" | sed 's/^ *//' | cut -c1-120)"
grep -a "Project FPS" "$LOG" | tail -5 | tr '\n' ' '; echo
grep -a -i "error\|失敗\|切れた" "$LOG" | grep -v "^\[iphone-vk\] まだ無い入口\|入口が無い" | head -5
echo "記録：$LOG"
