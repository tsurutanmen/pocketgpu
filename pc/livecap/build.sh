#!/bin/sh
# livecap.exe を zig cc で組む。Git Bash で：sh build.sh
#   -mcpu=baseline：配る先の古い CPU でも動くように（zig cc の既定は、組んだ PC の CPU の命令を使う）
#   -Os -s：小さく。要る DLL は Windows に入っている物だけ（tests/pe_imports.py で確かめる）
set -e
cd "$(dirname "$0")"
export PATH="/d/tools/zig-x86_64-windows-0.17.0:$PATH"
zig cc -target x86_64-windows-gnu -mcpu=baseline -Os -s -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    livecap.c -o livecap.exe -lole32 -luuid -lws2_32 -lavrt -lgdi32 -luser32 -lshell32
rm -f livecap.pdb
ls -l livecap.exe
