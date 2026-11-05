#!/bin/sh
# Build and run the offline tests of the Revelations 1 mod (native Windows, workspace Zig):  sh tests/build.sh   ->  tests/build/test_lobby1.exe (+ runs it)
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
mkdir -p build
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_lobby1.exe test_lobby1.c ../native/lobby1.c ../native/lobby1_text.c ../native/lobby1_input.c ../native/gmdtext.c
./build/test_lobby1.exe
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_padmerge.exe test_padmerge.c
./build/test_padmerge.exe
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_padmap.exe test_padmap.c
./build/test_padmap.exe
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_gametex.exe test_gametex.c ../native/gametex.c
ARC=""
for d in "${GAME_DIR:-}" ../../game ../../work/game; do
    [ -n "$d" ] && [ -f "$d/nativePC/archive/gui/resident.arc" ] && ARC="$d/nativePC/archive/gui/resident.arc" && break
done
if [ -n "$ARC" ]; then ./build/test_gametex.exe "$ARC" rev1; else echo "(no game folder found: only the inflate checks run; set GAME_DIR=<game folder>)"; ./build/test_gametex.exe; fi
