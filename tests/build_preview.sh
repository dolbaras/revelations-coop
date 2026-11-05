#!/bin/sh
# Build the overlay preview tool (GDI only, no game): sh tests/build_preview.sh ; tests/build/overlay_preview.exe <out dir> [w h]
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
mkdir -p build
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/overlay_preview.exe overlay_preview.c ../native/overlay.c ../native/lobby1.c ../native/lobby1_text.c -lgdi32 -luser32 -lm
