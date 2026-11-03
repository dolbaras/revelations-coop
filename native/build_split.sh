#!/bin/sh
# Build the hot-loadable split-screen plug-in: sh build_split.sh [name]  ->  build/<name>.dll (default r1split). Use a new name for every build while the old one may still be loaded.
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
NAME="${1:-r1split}"
mkdir -p build
"$ZIG" cc -target x86-windows-gnu -O2 -fno-omit-frame-pointer -shared -o "build/$NAME.dll" plug_split.c -luser32 -lkernel32 2>&1
