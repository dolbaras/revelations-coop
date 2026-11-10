#!/bin/sh
# Build and run the offline harness of the title menu glue (32-bit, like the game): sh tests/build_menu.sh
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
mkdir -p build
ENET="${ENET:-../../../../tools/enet/enet-1.3.18}"   # the menu glue links the network link of the lobby (lobby_link.c -> coop session -> ENET)
"$ZIG" cc -target x86-windows-gnu -O1 -g -Wall -Wno-unused-function -DWIN32 -I"$ENET/include" -o build/test_menu.exe test_menu.c ../native/menu.c ../native/lobby1.c ../native/lobby1_text.c ../native/lobby1_input.c ../native/gmdtext.c \
  ../native/lobby_link.c ../native/net1.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_session.c ../native/coop_steam.c ../native/coop_steam_real.c \
  "$ENET/callbacks.c" "$ENET/compress.c" "$ENET/host.c" "$ENET/list.c" "$ENET/packet.c" "$ENET/peer.c" "$ENET/protocol.c" "$ENET/win32.c" -luser32 -lkernel32 -lws2_32 -lwinmm
./build/test_menu.exe
