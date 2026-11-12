#!/bin/sh
# Build the 32-bit dinput8.dll proxy for Revelations (rerev.exe) with the workspace Zig (no Visual Studio needed).
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
OUT="${OUT:-build/dinput8.dll}"   # OUT=somewhere/dinput8.dll builds elsewhere (a second agent may be using build/)
mkdir -p "$(dirname "$OUT")"
# the network link of the lobby (lobby_link.c) runs the coop session (coop_*.c) over ENET (the same sources as Rev2's build)
ENET="${ENET:-../../../../tools/enet/enet-1.3.18}"
"$ZIG" cc -target x86-windows-gnu -O2 -fno-omit-frame-pointer -shared -o "$OUT" \
  -DWIN32 -I"$ENET/include" proxy.c classes.c console.c testmode.c d3d.c inject.c mute.c steam.c lt.c capture.c split.c probe.c menu.c warp.c puppet_game.c puppet1.c overlay.c gametex.c lobby1.c lobby1_text.c lobby1_input.c gmdtext.c downed1.c give1.c scan1.c \
  lobby_link.c net1.c guards1.c guards1_game.c mapptn1.c mapptn1_game.c rolebarrier1.c rolebarrier1_game.c coopsave1_game.c world1.c world1_net.c world1_hooks.c world1_game.c sf1.c ach1_game.c coop_proto.c coop_net.c coop_session.c coop_steam.c coop_steam_real.c \
  "$ENET/callbacks.c" "$ENET/compress.c" "$ENET/host.c" "$ENET/list.c" "$ENET/packet.c" "$ENET/peer.c" "$ENET/protocol.c" "$ENET/win32.c" dinput8.def \
  -luser32 -lkernel32 -lgdi32 -lws2_32 -lole32 -luuid -lwinmm
