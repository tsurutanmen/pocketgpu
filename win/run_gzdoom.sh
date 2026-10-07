#!/bin/bash
# GZDoom（Vulkan で描く）と Freedoom（修正 BSD）を、ステージを開いて決まった秒数だけ回す。
#   win/run_gzdoom.sh [受け手 または direct] [ステージ] [秒] [幅] [高さ]
#   受け手を省くと usb（iPhone）。direct なら RTX に直接
# GZDoom は Freedoom に入っているふつうの Doom のデモを再生できないので、timedemo は使えない（タイトル画面を回り続ける）。
# 代わりにステージ MAP01 を開いて回し、ドライバが数えたコマ数で比べる（direct はドライバを通らないので、GZDoom の記録だけ）
G=/d/iphone-rpc/games/gzdoom
VK=${1:-usb}
MAP=${2:-MAP01}
SECS=${3:-30}
W=${4:-1280}
H=${5:-720}
OUT=/d/iphone-rpc/vkbuild/game_runs
mkdir -p "$OUT"
TAGN="gzdoom_${VK//[:.]/_}_${MAP}_${W}x${H}_$(date +%m%d_%H%M%S)"
LOG="$OUT/$TAGN.txt"
GLOG="$OUT/${TAGN}_log.txt"
export IPHONE_VK_MONITOR=${IPHONE_VK_MONITOR:-sub}
# Vulkan の暗黙のレイヤー（Meta の VR などが入れる物）は読み込まない。RTX 直とドライバ越しを同じ条件にする
export VK_LOADER_LAYERS_DISABLE='~implicit~'
cd "$G" || exit 1
cp -f ../fd/freedoom-0.13.0/freedoom2.wad . 2> /dev/null || cp -f ../fd/*/freedoom2.wad . 2> /dev/null
# vid_preferbackend 1 が Vulkan。コマ数の上限と垂直同期を外し、全速で描かせる
ARGS=(-iwad freedoom2.wad -width "$W" -height "$H" +vid_fullscreen 0 +vid_preferbackend 1 +vid_maxfps 0 +vid_vsync 0
    +map "$MAP" +logfile "$(cygpath -w "$GLOG")" -nosound)
if [ "$VK" = direct ]; then
    ./gzdoom.exe "${ARGS[@]}" > "$LOG" 2>&1 &
else
    IPHONE_VK=$VK IPHONE_VK_ONLY=1 IPHONE_VK_VSYNC=0 IPHONE_VK_LOG=1 IPHONE_VK_STATS=1 \
        cmd.exe //c D:/iphone-rpc/vkbuild/iphone-vk.cmd "$(cygpath -w "$G/gzdoom.exe")" "${ARGS[@]}" > "$LOG" 2>&1 &
fi
sleep "$SECS"
# 窓を閉じて終わらせる（ドライバが最後にコマ数を書く）。終わらなければ止める
taskkill //IM gzdoom.exe > /dev/null 2>&1
for i in $(seq 1 20); do tasklist | grep -qi gzdoom.exe || break; sleep 0.5; done
taskkill //IM gzdoom.exe //F > /dev/null 2>&1
wait
echo "$(grep -a -m1 "Vulkan device:" "$GLOG" 2> /dev/null)・$(grep -a "swapchain：" "$LOG" | tail -1)"
grep -a -i "error\|failed\|失敗\|切れた" "$LOG" "$GLOG" 2> /dev/null | head -5
echo "記録：$GLOG"
