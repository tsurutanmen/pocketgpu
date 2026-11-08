#!/bin/bash
# zig で組む（Visual Studio は要らない）。出す所は ../out
#   ./build.sh            全部
#   ./build.sh cam rx tool text
# camctl2：PC から操作する（webcam_rx の CTRL とパイプ・camctl_cli・偽の iPhone の CTRL）。出す所は camctl2/out
#   ./build.sh rx fake cli
# cam4k：camtest＝試験用の仮想カメラ（別の CLSID・名前・レジストリの置き場。本物に触らない）。bench＝色の変換の速さと正しさを測る
#   ./build.sh camtest bench
set -e
export PATH="/d/tools/zig-x86_64-windows-0.17.0:$PATH"
cd "$(dirname "$0")"
OUT=${OUT:-../out}
mkdir -p $OUT
CW="-O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function"
CXXW="$CW -fno-exceptions -fno-rtti -std=c++17"
WHAT=${*:-cam setup rx tool fake cli}
for w in $WHAT; do
  case $w in
  text) zig cc -target x86_64-windows-gnu -O2 gen_textmask.c -o $OUT/gen_textmask.exe -lgdi32 && $OUT/gen_textmask.exe > textmask.h ;;
  cam)
    # 32 ビット版は SSE2 を使う（vcam_convert.h）。x86-64 は SSE2 が必ずある
    zig c++ -target x86_64-windows-gnu $CXXW -shared dshow_cam.cpp dshow_cam.def -o $OUT/dshow_cam64.dll -lole32 -luuid -ladvapi32
    zig c++ -target x86-windows-gnu $CXXW -msse2 -shared dshow_cam.cpp dshow_cam.def -o $OUT/dshow_cam32.dll -lole32 -luuid -ladvapi32 ;;
  camtest)
    zig c++ -target x86_64-windows-gnu $CXXW -DVCAM_TEST -shared dshow_cam.cpp dshow_cam.def -o $OUT/dshow_cam4ktest64.dll -lole32 -luuid -ladvapi32
    zig c++ -target x86-windows-gnu $CXXW -msse2 -DVCAM_TEST -shared dshow_cam.cpp dshow_cam.def -o $OUT/dshow_cam4ktest32.dll -lole32 -luuid -ladvapi32 ;;
  bench)
    zig c++ -target x86_64-windows-gnu -O2 -std=c++17 vcam_bench.cpp -o $OUT/vcam_bench64.exe
    zig c++ -target x86-windows-gnu -msse2 -O2 -std=c++17 vcam_bench.cpp -o $OUT/vcam_bench32.exe ;;
  fake) zig cc -target x86_64-windows-gnu $CW fake_iphone.c -o $OUT/fake_iphone.exe -lws2_32 -lole32 -loleaut32 -luuid -lwinmm ;;
  # camctl2：古いアプリの真似（camset の偽の iPhone の写し。ポートだけ 50760〜50780）
  oldfake) zig cc -target x86_64-windows-gnu $CW fake_iphone_camset.c -o $OUT/fake_iphone_camset.exe -lws2_32 -lole32 -loleaut32 -luuid -lwinmm ;;
  cli) zig cc -target x86_64-windows-gnu $CW camctl_cli.c -o $OUT/camctl_cli.exe ;;
  setup) zig cc -target x86_64-windows-gnu $CW -municode camera_setup.c -o $OUT/camera_setup.exe -ladvapi32 ;;
  rx)   zig cc -target x86_64-windows-gnu $CW webcam_rx.c -o $OUT/webcam_rx.exe -lws2_32 -lole32 -loleaut32 -luuid -lwinmm -lgdi32 -luser32 -ladvapi32 ;;
  tool)
    for a in x86_64:64 x86:32; do
      t=${a%%:*}; b=${a##*:}
      zig c++ -target $t-windows-gnu $CXXW dscam_tool.cpp -o $OUT/dscam_tool$b.exe -lole32 -loleaut32 -luuid -ladvapi32 -lmfplat -lmf -lmfuuid
    done ;;
  esac
  echo "組んだ：$w"
done
