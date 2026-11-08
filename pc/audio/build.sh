#!/bin/bash
# zig で組む（Visual Studio は要らない）。出す所は ../out
#   ./build.sh            全部
#   ./build.sh ipaudio fake
set -e
export PATH="/d/tools/zig-x86_64-windows-0.17.0:$PATH"
cd "$(dirname "$0")"
OUT=${OUT:-../out}
mkdir -p $OUT
CW="-O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers"
WHAT=${*:-ipaudio fake}
for w in $WHAT; do
  case $w in
  ipaudio) zig cc -target x86_64-windows-gnu $CW ipaudio.c -o $OUT/ipaudio.exe -lole32 -luuid -lws2_32 -lavrt -loleaut32 -luser32 -lgdi32 -lshell32 ;;
  fake)    zig cc -target x86_64-windows-gnu $CW fake_iphone.c -o $OUT/fake_iphone.exe -lws2_32 -lwinmm -lole32 -luuid -lavrt -loleaut32 ;;
  esac
  echo "組んだ：$w"
done
