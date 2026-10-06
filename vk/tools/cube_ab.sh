#!/bin/bash
# vkcube を「違いの読み出しあり・なし」で交互に回して、コマ/秒と運んだ量を比べる
#   使い方：cube_ab.sh 受け手の番地 回数 [出力先]   例：cube_ab.sh 127.0.0.1:50071 2
set -u
ADDR=${1:-127.0.0.1:50071}
REP=${2:-1}
OUT=${3:-/d/iphone-rpc/vkbuild/delta_runs}
mkdir -p "$OUT"
for r in $(seq 1 "$REP"); do
    for size in 500x500 1280x720; do
        for d in 0 1; do
            w=${size%x*}; h=${size#*x}
            f="$OUT/ab_${size}_d${d}_$r.txt"
            IPHONE_VK=$ADDR IPHONE_VK_LOG=1 IPHONE_VK_STATS=1 IPHONE_VK_DELTA=$d timeout 300 cmd.exe //c "D:\\iphone-rpc\\vkbuild\\iphone-vk.cmd" \
                "D:\\iphone-rpc\\vkcube\\vkcube.exe" --gpu_number 1 --c 300 --suppress_popups --width "$w" --height "$h" > "$f" 2>&1
            fps=$(grep -a -o "[0-9.]* コマ/秒" "$f" | tail -1)
            got=$(grep -a -o "受け取ったバイト [0-9]*" "$f" | grep -o "[0-9]*")
            rb=$(grep -a -o "引き取り [0-9.]* ms" "$f")
            printf "%-9s 違い%s  %-14s %6s MB/コマ  %s\n" "$size" "$( [ $d = 1 ] && echo あり || echo なし )" "$fps" \
                "$(python -c "print(f'{${got:-0}/300/1e6:.2f}')")" "$rb"
        done
    done
done
