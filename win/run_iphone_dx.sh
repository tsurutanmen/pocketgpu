#!/bin/bash
# iPhone の実機で、10/6 夜に代役で通した物をまとめて試す。iPhone は受け手のアプリを開いたまま・自動ロックなし・USB でつなぐ。
#   win/run_iphone_dx.sh [版の名前]
#   1. 装置の拡張と、BC を読めるか  2. Vulkan の三角形と足し算  3. vkcube：JPEG の先送りあり・なし
#   4. DirectX 11 の三角形と立方体（BC1 も）  5. DirectX のサンプル（BC は GPU が読めなければドライバがほどく）
V=${1:-v0723}
VKB=/d/iphone-rpc/vkbuild
OUT=$VKB/iphone_runs/$V
WOUT=$(cygpath -w "$OUT")
mkdir -p "$OUT"
export IPHONE_VK=${IPHONE_VK_ADDR:-usb} # 代役で試すときは IPHONE_VK_ADDR=127.0.0.1:50070
# 窓はサブモニターに出す（メインは CEO が使う）
export IPHONE_VK_MONITOR=${IPHONE_VK_MONITOR:-sub}
# 速さを測るので、書き換えに合わせる出し方（FIFO）でも区切らずに全速で回す
export IPHONE_VK_VSYNC=0
VKCMD='D:/iphone-rpc/vkbuild/iphone-vk.cmd'
run() { timeout "$1" cmd.exe //c "$VKCMD" "${@:2}"; }
echo "結果は $OUT"

echo "== 0. USB の速さ：つなぎを 1・2・4 本同時に"
# usbmux はつなぎごとに受け取りの窓を持つらしいので、本数で足し算になるかを見る（先例 Backburner の調べから）
(cd /d/iphone-rpc/app/pc && PYTHONIOENCODING=utf-8 timeout 300 python usb_bw.py --mb 128) > "$OUT/usb_bw.txt" 2>&1
grep -a "MB/s" "$OUT/usb_bw.txt" | head -8

echo "== 1. 装置の拡張"
IPHONE_VK_LOG=1 run 120 "D:/iphone-rpc/vkbuild/extlist.exe" iPhone > "$OUT/extlist.txt" 2>&1
grep -a "受け手の GPU\|caps\|BC の\|: [0-9]*$" "$OUT/extlist.txt" | head -5

echo "== 2. Vulkan の三角形と足し算"
for t in tri tri13 vecadd; do
    (cd "$OUT" && IPHONE_VK_LOG=1 run 120 "D:/iphone-rpc/vkbuild/$t.exe") > "$OUT/$t.txt" 2>&1
    echo "$t：$(grep -a -v '^\[iphone-vk\]' "$OUT/$t.txt" | tail -1)"
    # 1 回目だけ GPU を失ったと出たことがある（10/7）。ドライバが何を見たかを出す
    grep -a "失敗を返した\|壊れていた\|切れた\|送れない" "$OUT/$t.txt" | head -3
done

echo "== 3. vkcube（300 コマ）JPEG の先送りあり・なし"
for p in 1 0; do
    for sz in "500 500" "1280 720"; do
        set -- $sz
        IPHONE_VK_JPEG_PIPE=$p IPHONE_VK_LOG=1 IPHONE_VK_STATS=1 \
            run 300 "D:/iphone-rpc/vkcube/vkcube.exe" --gpu_number 1 --c 300 --suppress_popups --width $1 --height $2 > "$OUT/vkcube_${1}_pipe$p.txt" 2>&1
        echo "vkcube ${1}x${2} 先送り $p：$(grep -a -o '[0-9.]* コマ/秒' "$OUT/vkcube_${1}_pipe$p.txt" | head -1)"
    done
done

echo "== 3b. 窓に出るまでの遅れ：60 コマ/秒で区切り、前のやり方（PEND=1）と新しいやり方（10/7 午後）"
for p in 1 0; do
    IPHONE_VK_MAX_FPS=60 IPHONE_VK_PEND=$p IPHONE_VK_LOG=1 \
        run 120 "D:/iphone-rpc/vkcube/vkcube.exe" --gpu_number 1 --c 300 --suppress_popups --present_mode 0 > "$OUT/latency_pend$p.txt" 2>&1
    echo "PEND=$p：$(grep -a '窓に出るまで\|切り替え' "$OUT/latency_pend$p.txt" | sed 's/\[iphone-vk\] //' | tr '\n' ' ')"
done

echo "== 4. DirectX 11（DXVK-macOS）"
D=$VKB/d3d11
(cd "$D" && DXVK_FILTER_DEVICE_NAME="iPhone-RPC" DXVK_LOG_LEVEL=info DXVK_LOG_PATH=none IPHONE_VK_LOG=1 \
    run 180 "D:/iphone-rpc/vkbuild/d3d11/d3d11tri.exe" "$WOUT\\d3d11tri.ppm") > "$OUT/d3d11tri.txt" 2>&1
echo "三角形：$(grep -a '検算\|失敗\|not supported' "$OUT/d3d11tri.txt" | head -3 | tr '\n' ' ')（代役とふつうの DirectX は 7a12ddcb）"
for m in plain bc1; do
    arg=""; [ $m = bc1 ] && arg=bc1
    (cd "$D" && DXVK_FILTER_DEVICE_NAME="iPhone-RPC" DXVK_LOG_LEVEL=info DXVK_LOG_PATH=none IPHONE_VK_LOG=1 IPHONE_VK_STATS=1 \
        run 300 "D:/iphone-rpc/vkbuild/d3d11/d3d11cube.exe" 300 "$WOUT\\d3d11cube_$m.ppm" $arg) > "$OUT/d3d11cube_$m.txt" 2>&1
    echo "立方体 $m：$(grep -a 'コマ/秒\|検算\|失敗' "$OUT/d3d11cube_$m.txt" | grep -av '^\[iphone-vk\]' | tr '\n' ' ')"
done
echo "（代役とふつうの DirectX は d760538a・BC1 はふつう 139f0d27・ドライバでほどくと b186cdaa）"
# 新しい DXVK 3.1.1：名乗りのスイッチを入れて同じ立方体
D3=$VKB/dxvk3
for m in plain bc1; do
    arg=""; [ $m = bc1 ] && arg=bc1
    (cd "$D3" && IPHONE_VK_DXVK3=1 DXVK_FILTER_DEVICE_NAME="iPhone-RPC" DXVK_LOG_LEVEL=info DXVK_LOG_PATH=none IPHONE_VK_LOG=1 \
        run 300 "D:/iphone-rpc/vkbuild/dxvk3/d3d11cube.exe" 300 "$WOUT\\dxvk3cube_$m.ppm" $arg) > "$OUT/d3d11dxvk3_$m.txt" 2>&1
    echo "DXVK 3 の立方体 $m：$(grep -a 'コマ/秒\|検算\|失敗' "$OUT/d3d11dxvk3_$m.txt" | grep -av '^\[iphone-vk\]' | tr '\n' ' ')"
done
# DXVK が「足りない」と言った機能を拾う
grep -a -h "not supported\|Failed\|err:" "$OUT"/d3d11*.txt | sort | uniq -c | head -10

echo "== 5. DirectX のサンプル"
bash /d/iphone-rpc/app/win/run_d3d11_samples.sh "$IPHONE_VK" EmptyProject11 SimpleSample11 BasicHLSL11 CascadedShadowMaps11 FluidCS11 HDRToneMappingCS11 SimpleBezier11 OIT11 2>&1 | tee "$OUT/samples.txt"
echo "== 6. DirectX のサンプル（新しい DXVK 3.1.1）"
DXVK_DIR=$VKB/dxvk3 IPHONE_VK_DXVK3=1 bash /d/iphone-rpc/app/win/run_d3d11_samples.sh "$IPHONE_VK" BasicHLSL11 CascadedShadowMaps11 FluidCS11 SimpleBezier11 2>&1 | tee "$OUT/samples_dxvk3.txt"
echo "== 7. 入門サンプル Tutorial01〜07（代役と iPhone の絵を比べる・代役の受け手が 50070 に要る）"
bash /d/iphone-rpc/app/win/run_d3d11_tutorials.sh 127.0.0.1:50070 "$IPHONE_VK" 2>&1 | tee "$OUT/tutorials.txt"
for f in tri tri13 d3d11tri d3d11cube_plain d3d11cube_bc1; do
    [ -f "$OUT/$f.ppm" ] && python -X utf8 /d/iphone-rpc/app/vk/tools/ppm2png.py "$OUT/$f.ppm" "$OUT/$f.png" > /dev/null
done
