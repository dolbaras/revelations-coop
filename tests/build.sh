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
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_devreset.exe test_devreset.c
./build/test_devreset.exe
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_gametex.exe test_gametex.c ../native/gametex.c
ARC=""
for d in "${GAME_DIR:-}" ../../game ../../work/game; do
    [ -n "$d" ] && [ -f "$d/nativePC/archive/gui/resident.arc" ] && ARC="$d/nativePC/archive/gui/resident.arc" && break
done
if [ -n "$ARC" ]; then ./build/test_gametex.exe "$ARC" rev1; else echo "(no game folder found: only the inflate checks run; set GAME_DIR=<game folder>)"; ./build/test_gametex.exe; fi
# the overlay's device Reset path (release + rebuild, 3 rounds; the shared Japanese face once crashed the live game with a double free): needs the game's archive for the real faces
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/overlay_preview.exe overlay_preview.c ../native/overlay.c ../native/gametex.c ../native/lobby1.c ../native/lobby1_text.c -lgdi32 -luser32 -lm
if [ -n "$ARC" ]; then ./build/overlay_preview.exe build/pv_out 1280 720 "" all "$ARC" 2>/dev/null | grep -q "reset cycle: 3 x release + render ok" && echo "overlay reset cycle ok" || { echo "FAIL: overlay reset cycle"; exit 1; }
else ./build/overlay_preview.exe build/pv_out 1280 720 2>/dev/null | grep -q "reset cycle" && echo "overlay reset cycle ok (no game archive: fallback faces)" || { echo "FAIL: overlay reset cycle"; exit 1; }; fi
