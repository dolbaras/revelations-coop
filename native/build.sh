#!/bin/sh
# Build the 32-bit dinput8.dll proxy for Revelations (rerev.exe) with the workspace Zig (no Visual Studio needed).
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
mkdir -p build
"$ZIG" cc -target x86-windows-gnu -O2 -fno-omit-frame-pointer -shared -o build/dinput8.dll \
  -DWIN32 proxy.c classes.c console.c testmode.c d3d.c inject.c mute.c steam.c lt.c capture.c split.c probe.c menu.c warp.c overlay.c lobby1.c lobby1_text.c lobby1_input.c gmdtext.c dinput8.def \
  -luser32 -lkernel32 -lgdi32 -lws2_32 -lole32 -luuid -lwinmm
