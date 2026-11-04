#!/bin/sh
# Build and run the offline tests of the Revelations 1 mod (native Windows, workspace Zig):  sh tests/build.sh   ->  tests/build/test_lobby1.exe (+ runs it)
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
mkdir -p build
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_lobby1.exe test_lobby1.c ../native/lobby1.c ../native/lobby1_text.c ../native/lobby1_input.c ../native/gmdtext.c
./build/test_lobby1.exe
