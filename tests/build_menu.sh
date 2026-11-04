#!/bin/sh
# Build and run the offline harness of the title menu glue (32-bit, like the game): sh tests/build_menu.sh
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
mkdir -p build
"$ZIG" cc -target x86-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_menu.exe test_menu.c ../native/menu.c ../native/lobby1.c ../native/lobby1_text.c ../native/lobby1_input.c ../native/gmdtext.c -luser32 -lkernel32
./build/test_menu.exe
