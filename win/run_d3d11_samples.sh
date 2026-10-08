#!/bin/bash
# DirectX 11 のサンプル（DirectX SDK のサンプル集・CI の d3d11-samples-win で組む）を 3 通りで動かして、
# 最初のコマの絵と、決めたコマ数を出すのにかかった時間を比べる。
#   native：ふつうの DirectX
#   dxvk  ：DXVK-macOS で Vulkan に直し、RTX で直接描く
#   icd   ：DXVK-macOS で Vulkan に直し、iPhone のドライバ越しに描く（IPHONE_VK の受け手）
#   win/run_d3d11_samples.sh [IPHONE_VK の値] [サンプル名 ...]
# 絵は DXUT の -screenshotexit で撮る（最初のコマ・-constantframetime で動きを決める）。
# DXUT でない Tutorial は、ドライバの IPHONE_VK_DUMP で 60 コマ目を撮る（icd だけ）
S=/d/iphone-rpc/vkbuild/d3d11samples
DX=${DXVK_DIR:-/d/iphone-rpc/vkbuild/d3d11} # 新しい DXVK で試すときは DXVK_DIR=/d/iphone-rpc/vkbuild/dxvk3 と IPHONE_VK_DXVK3=1
VKCMD="${IPHONE_VK_CMD:-D:/iphone-rpc/vkbuild/iphone-vk.cmd}"
OUT=/d/iphone-rpc/vkbuild/d3d11samples_runs/$(date +%m%d_%H%M)
VK=${1:-127.0.0.1:50070}
shift
NAMES=("$@")
[ ${#NAMES[@]} -eq 0 ] && NAMES=(EmptyProject11 SimpleSample11 BasicHLSL11 BasicCompute11 CascadedShadowMaps11 VarianceShadows11
    FluidCS11 NBodyGravityCS11 HDRToneMappingCS11 SimpleBezier11 DynamicShaderLinkage11 MultithreadedRendering11 OIT11)
FRAMES=${FRAMES:-300}
mkdir -p "$OUT"
# 窓はサブモニターに出す（左にある・X=-1920）。ドライバ越しの窓は IPHONE_VK_MONITOR=sub でも動く
ARGS="-windowed -width:640 -height:480 -forcevsync:0 -noerrormsgboxes -nostats -constantframetime:0.0166 -startx:${SUB_X:--1800} -starty:${SUB_Y:-100}"
export IPHONE_VK_MONITOR=${IPHONE_VK_MONITOR:-sub}
echo "受け手 $VK・結果は $OUT"

run() { # 名前 形 フォルダ 記録の印 追加の引数
    local name=$1 mode=$2 dir=$3 tag=$4 extra=$5
    local exe="$dir/$name.exe" t0 t1
    t0=$(date +%s.%N)
    if [ "$mode" = icd ]; then
        (cd "$dir" && IPHONE_VK=$VK IPHONE_VK_LOG=1 DXVK_FILTER_DEVICE_NAME="iPhone-RPC" DXVK_LOG_LEVEL=none \
            timeout 180 cmd.exe //c "$VKCMD" "$(cygpath -m "$exe")" $ARGS "$extra") > "$OUT/${name}_${mode}_$tag.log" 2>&1
    elif [ "$mode" = dxvk ]; then
        (cd "$dir" && DXVK_FILTER_DEVICE_NAME="NVIDIA GeForce RTX 3060" DXVK_LOG_LEVEL=none timeout 180 /d/iphone-rpc/vkbuild/subwin.exe "./$name.exe" $ARGS "$extra") > /dev/null 2>&1
    else
        (cd "$dir" && timeout 180 /d/iphone-rpc/vkbuild/subwin.exe "./$name.exe" $ARGS "$extra") > /dev/null 2>&1
    fi
    local rc=$?
    t1=$(date +%s.%N)
    taskkill //IM "$name.exe" //F > /dev/null 2>&1
    echo "$rc $(awk "BEGIN{print $t1 - $t0}")"
}

for name in "${NAMES[@]}"; do
    exe=$(ls $S/*/x64/Release/$name.exe 2>/dev/null | head -1)
    [ -z "$exe" ] && { echo "$name：見つからない"; continue; }
    rel=$(dirname "$exe")
    # DXVK の DLL を置く場所。素材の探し方が変わらないように、同じ深さに作る
    dxd=$(dirname "$rel")/DXVK
    mkdir -p "$dxd"
    cp -f "$exe" "$dxd/" && cp -f $DX/d3d11.dll $DX/dxgi.dll "$dxd/"
    line="$name"
    for mode in native dxvk icd; do
        d=$rel
        [ "$mode" != native ] && d=$dxd
        shot=$(cygpath -w "$OUT/${name}_$mode")
        r1=$(run "$name" "$mode" "$d" shot "-screenshotexit:$shot")
        r2=$(run "$name" "$mode" "$d" perf "-quitafterframe:$FRAMES")
        line="$line | $mode 絵:${r1%% *} 時間:$(printf %.1f "${r2#* }")s"
    done
    echo "$line"
    PYTHONIOENCODING=utf-8 python "$(dirname "$0")/../vk/tools/imgdiff.py" "$OUT/${name}_native.bmp" "$OUT/${name}_dxvk.bmp" "$OUT/${name}_icd.bmp" 2>&1 | sed 's/^/    /'
    grep -a "swapchain：" "$OUT/${name}_icd_perf.log" 2>/dev/null | tail -1 | sed 's/^/    /'
    grep -a "まだ中継していない\|この種類の descriptor\|まだ扱わない\|落ちた" "$OUT/${name}_icd_"*.log 2>/dev/null | sort | uniq -c | head -5 | sed 's/^/    /'
done
