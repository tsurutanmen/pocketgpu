#!/bin/bash
# DirectX 11 の入門サンプル Tutorial01〜07 を、ドライバ越しに 2 つの受け手で動かし、60 コマ目の絵を比べる。
# Tutorial は DXUT ではないので、絵はドライバの IPHONE_VK_DUMP で撮り、決めた時間で止める。
#   win/run_d3d11_tutorials.sh [基準の受け手] [比べる受け手]
#   既定は代役の 127.0.0.1:50070 と、iPhone のふりの 127.0.0.1:50072。iPhone で試すなら 2 つ目を usb に
S=/d/iphone-rpc/vkbuild/d3d11samples/Direct3D11Tutorials
DX=${DXVK_DIR:-/d/iphone-rpc/vkbuild/d3d11}
VKCMD=${VKCMD:-${IPHONE_VK_CMD:-D:/iphone-rpc/vkbuild/iphone-vk.cmd}}
REF=${1:-127.0.0.1:50070}
CMP=${2:-127.0.0.1:50072}
OUT=/d/iphone-rpc/vkbuild/d3d11samples_runs/tut_$(date +%m%d_%H%M)
mkdir -p "$OUT" "$S/x64/DXVK"
cp -f "$S"/x64/Release/*.exe "$S/x64/DXVK/" && cp -f "$DX/d3d11.dll" "$DX/dxgi.dll" "$S/x64/DXVK/"
export IPHONE_VK_MONITOR=${IPHONE_VK_MONITOR:-sub}
echo "基準 $REF・比べる $CMP・結果は $OUT"
for n in 1 2 3 4 5 6 7; do
    name=Tutorial0$n
    dir="$S/$name"
    [ -d "$dir" ] || dir="$S/Tutorial02" # Tutorial01 はシェーダーの物が無い
    for tag in ref cmp; do
        vk=$REF; [ $tag = cmp ] && vk=$CMP
        rm -f "$OUT/${name}_$tag.ppm"
        # 入門サンプルは自分では終わらないので、絵を書いたら止める
        (cd "$dir" && IPHONE_VK=$vk IPHONE_VK_LOG=1 IPHONE_VK_DUMP="60:$(cygpath -w "$OUT")\\${name}_$tag.ppm" DXVK_FILTER_DEVICE_NAME="iPhone" DXVK_LOG_LEVEL=none \
            cmd.exe //c "$VKCMD" "$(cygpath -m "$S/x64/DXVK/$name.exe")" > "$OUT/${name}_$tag.log" 2>&1) &
        for i in $(seq 1 60); do [ -s "$OUT/${name}_$tag.ppm" ] && break; sleep 0.5; done
        sleep 0.5
        taskkill //IM "$name.exe" //F > /dev/null 2>&1
        wait
    done
    if [ -s "$OUT/${name}_ref.ppm" ] && [ -s "$OUT/${name}_cmp.ppm" ]; then
        echo "$name：$(PYTHONIOENCODING=utf-8 python "$(dirname "$0")/../vk/tools/imgdiff.py" "$OUT/${name}_ref.ppm" "$OUT/${name}_cmp.ppm" | sed 's/^[^：]*：//')"
    else
        echo "$name：絵が無い（基準 $( [ -s "$OUT/${name}_ref.ppm" ] && echo あり || echo なし)・比べる $( [ -s "$OUT/${name}_cmp.ppm" ] && echo あり || echo なし)）"
        grep -a "作れない\|失敗\|まだ中継" "$OUT/${name}_cmp.log" | sort | uniq -c | head -3
    fi
done
