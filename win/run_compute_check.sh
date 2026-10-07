#!/bin/bash
# 読み戻しを変えたあとに、計算の答えが崩れていないかだけを手早く見る（通しの試験の 2・5b と、8 の計算の場面）。
#   IPHONE_VK=127.0.0.1:50072 win/run_compute_check.sh [名前]
V=${1:-compute_$(date +%m%d_%H%M)}
VKB=/d/iphone-rpc/vkbuild
OUT=$VKB/iphone_runs/$V
mkdir -p "$OUT/feat_native" "$OUT/feat_dev" "$VKB/d3d11native"
export IPHONE_VK=${IPHONE_VK:-usb}
export IPHONE_VK_MONITOR=${IPHONE_VK_MONITOR:-sub}
VKCMD="${IPHONE_VK_CMD:-D:/iphone-rpc/vkbuild/iphone-vk.cmd}"
run() { timeout "$1" cmd.exe //c "$VKCMD" "${@:2}"; }
echo "受け手 $IPHONE_VK・結果は $OUT"
for t in vecadd pushdesc copydesc copyimg; do
    (cd "$OUT" && run 120 "D:/iphone-rpc/vkbuild/$t.exe") > "$OUT/$t.txt" 2>&1
    echo "$t：$(grep -a -v '^\[iphone-vk\]' "$OUT/$t.txt" | tail -1)"
done
BC=$VKB/d3d11samples/BasicCompute11
for dx in d3d11 dxvk3; do
    mkdir -p "$BC/x64/$dx" && cp -f "$BC/x64/Release/BasicCompute11.exe" "$BC/x64/$dx/" && cp -f "$VKB/$dx/d3d11.dll" "$VKB/$dx/dxgi.dll" "$BC/x64/$dx/"
    flag=""; [ $dx = dxvk3 ] && flag=1
    (cd "$BC" && IPHONE_VK_DXVK3=$flag IPHONE_VK_ONLY=1 DXVK_LOG_LEVEL=none \
        run 120 "$(cygpath -m "$BC/x64/$dx/BasicCompute11.exe")") > "$OUT/basiccompute_$dx.txt" 2>&1
    echo "BasicCompute11 $dx：$(grep -a 'Verifying' "$OUT/basiccompute_$dx.txt" | tr '\n' ' ')"
done
FT=$VKB/d3d11/feat.exe
cp -f "$FT" "$VKB/d3d11native/"
for sc in cs_uav cs_atomic cs_shared cs_struct cs_append so indirect query deferred; do
    (cd "$VKB/d3d11native" && timeout 60 ./feat.exe "$sc" "$(cygpath -w "$OUT/feat_native/$sc.ppm")") > "$OUT/feat_native/$sc.txt" 2>&1
    (cd "$VKB/d3d11" && DXVK_FILTER_DEVICE_NAME="iPhone-RPC" DXVK_LOG_LEVEL=none \
        run 120 "$(cygpath -m "$FT")" "$sc" "$(cygpath -w "$OUT/feat_dev/$sc.ppm")") > "$OUT/feat_dev/$sc.txt" 2>&1
    echo "$sc：$(PYTHONIOENCODING=utf-8 python /d/iphone-rpc/app/vk/tools/imgdiff.py "$OUT/feat_native/$sc.ppm" "$OUT/feat_dev/$sc.ppm" 2>&1 | sed 's/^[^：]*：//')"
done
