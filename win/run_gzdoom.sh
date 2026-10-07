#!/bin/bash
# GZDoom（Vulkan で描く）と Freedoom（修正 BSD）の timedemo を動かす。
#   win/run_gzdoom.sh [受け手 または direct] [デモ] [幅] [高さ]
#   受け手を省くと usb（iPhone）。direct なら RTX に直接
G=/d/iphone-rpc/games/gzdoom
VK=${1:-usb}
DEMO=${2:-demo1}
W=${3:-1280}
H=${4:-720}
OUT=/d/iphone-rpc/vkbuild/game_runs
mkdir -p "$OUT"
TAGN="gzdoom_${VK//[:.]/_}_${DEMO}_${W}x${H}_$(date +%m%d_%H%M%S)"
LOG="$OUT/$TAGN.txt"
GLOG="$OUT/${TAGN}_log.txt"
export IPHONE_VK_MONITOR=${IPHONE_VK_MONITOR:-sub}
# Vulkan の暗黙のレイヤー（Meta の VR などが入れる物）は読み込まない。RTX 直とドライバ越しを同じ条件にする
export VK_LOADER_LAYERS_DISABLE='~implicit~'
cd "$G" || exit 1
cp -f ../fd/freedoom-0.13.0/freedoom2.wad . 2> /dev/null || cp -f ../fd/*/freedoom2.wad . 2> /dev/null
# vid_preferbackend 1 が Vulkan。timedemo はデモが終わると時間を書いて閉じる
ARGS=(-iwad freedoom2.wad -width "$W" -height "$H" +vid_fullscreen 0 +vid_preferbackend 1 +logfile "$(cygpath -w "$GLOG")" -timedemo "$DEMO" -nosound)
if [ "$VK" = direct ]; then
    timeout 300 ./gzdoom.exe "${ARGS[@]}" > "$LOG" 2>&1
else
    IPHONE_VK=$VK IPHONE_VK_ONLY=1 IPHONE_VK_VSYNC=0 IPHONE_VK_LOG=1 IPHONE_VK_STATS=1 \
        timeout 300 cmd.exe //c D:/iphone-rpc/vkbuild/iphone-vk.cmd "$(cygpath -w "$G/gzdoom.exe")" "${ARGS[@]}" > "$LOG" 2>&1
fi
taskkill //IM gzdoom.exe //F > /dev/null 2>&1
echo "$(grep -a -m1 -i "Vulkan device:\|^GPU\|Device:" "$GLOG" 2> /dev/null)・$(grep -a -i "gametics\|timed\|fps" "$GLOG" 2> /dev/null | tail -1 || echo 結果なし)"
echo "記録：$GLOG"
