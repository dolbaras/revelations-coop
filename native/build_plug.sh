#!/bin/sh
# Build a hot-loadable plug-in: sh build_plug.sh [name] [source]  ->  build/<name>.dll (default r1plug_cap from plug_cap.c; plug_dev.c = capture engine + debug toolbox).
# Use a new name for every build while the old one may still be loaded.
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
NAME="${1:-r1plug_cap}"
SRC="${2:-plug_cap.c}"
mkdir -p build
"$ZIG" cc -target x86-windows-gnu -O2 -fno-omit-frame-pointer -shared -o "build/$NAME.dll" "$SRC" -luser32 -lkernel32 2>&1
