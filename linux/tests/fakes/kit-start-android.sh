#!/usr/bin/env bash
# 試験用の偽の start-android.sh（試験のキットに入れる）。本物と同じ順に、adb で受け手を止めて・置いて・起動して・adb forward をつなぐ。
# fix.sh が渡す物：ADB・ANDROID_SERIAL・PGPU_PORT・PGPU_NOPROMPT=1・引数 --serial シリアル（と、アプリの受け手なら --app）
set -u
KIT=$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")
A=${ADB:-adb}
P=${PGPU_PORT:-50300}
S=${ANDROID_SERIAL:-}
while [ $# -gt 0 ]; do case "$1" in --serial) S=$2; shift ;; esac; shift; done
"$A" -s "$S" shell "kill \$(pidof pgpu_server) 2>/dev/null; rm -f /data/local/tmp/pgpu_server; true" > /dev/null || exit 1
"$A" -s "$S" push "$KIT/android/server_aarch64" /data/local/tmp/pgpu_server > /dev/null || exit 1
"$A" -s "$S" shell "cd /data/local/tmp && chmod 755 pgpu_server && (nohup ./pgpu_server 127.0.0.1 $P > pgpu_server.log 2>&1 &)" > /dev/null
[ -n "$("$A" -s "$S" shell pidof pgpu_server)" ] || { echo "receiver did not start"; exit 1; }
"$A" -s "$S" forward --remove "tcp:$P" > /dev/null 2>&1
"$A" -s "$S" forward "tcp:$P" "tcp:$P" || exit 1
echo "Ready (fake start-android.sh)."
