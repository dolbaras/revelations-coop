#!/bin/sh
# Build and run the offline tests of the Revelations 1 mod (native Windows, workspace Zig):  sh tests/build.sh   ->  tests/build/test_lobby1.exe (+ runs it)
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
mkdir -p build
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_lobby1.exe test_lobby1.c ../native/lobby1.c ../native/lobby1_text.c ../native/lobby1_input.c ../native/gmdtext.c
./build/test_lobby1.exe
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_downed.exe test_downed.c
./build/test_downed.exe
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_downed_pair.exe test_downed_pair.c
./build/test_downed_pair.exe
# the network side of the downed rule (downed1_net.h: one machine per PC, events over a line with latency / loss, puppet pose actions, coupled game over)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_downed_net.exe test_downed_net.c
./build/test_downed_net.exe
# LAB: packcopy and its undo (packcopy1.h: the receiver's words and the weapon owners back before any death / Continue; a replaced unit is never written; the undo
# by itself before a reload through rolebarrier_on_restart, checked with the glue's bag check rb_bag_shared)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_packcopy1.exe test_packcopy1.c ../native/rolebarrier1.c
./build/test_packcopy1.exe
# pickups whose world item leaves the list after AddItem returned (pickpend1.h, wp9 live: the herb 8/12): watched, sent once when gone; + the glue wiring
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_pickpend1.exe test_pickpend1.c
./build/test_pickpend1.exe
# give1: the RE5-style "give to partner" from the item screen (give1.h: cells, reach, downed partner, split plan, the network GIVE / ACK / CANCEL on a delayed line with
# timeouts, repeats, reloads and section changes; the 13-language texts and the game's glyphs; the stock bytes of give1.c in the image; the glue wiring)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_give1.exe test_give1.c
./build/test_give1.exe
# scan1: the Genesis for the second player (scan1.h: who gets one, the split exchange of the second human, his own rate and herb latch, the hidden-item REVEAL
# on a delayed line with loads; the stock bytes, vtable slots and rets of scan1.c in the image; the wiring in probe.c / split.c / story1_game.inc)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_scan1.exe test_scan1.c
./build/test_scan1.exe
# the downed pose (downed1_pose.h: the Raid agony subset driven offline; plans against a fake engine + the build facts in the image), 64- and 32-bit
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_downed_pose.exe test_downed_pose.c
./build/test_downed_pose.exe
"$ZIG" cc -target x86-windows-gnu -O1 -g -Wall -o build/test_downed_pose_x86.exe test_downed_pose.c
./build/test_downed_pose_x86.exe
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_padmerge.exe test_padmerge.c
./build/test_padmerge.exe
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_padmap.exe test_padmap.c
./build/test_padmap.exe
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_devreset.exe test_devreset.c
./build/test_devreset.exe
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_ach1.exe test_ach1.c
./build/test_ach1.exe
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_gametex.exe test_gametex.c ../native/gametex.c
ARC=""
for d in "${GAME_DIR:-}" ../../game ../../work/game; do
    [ -n "$d" ] && [ -f "$d/nativePC/archive/gui/resident.arc" ] && ARC="$d/nativePC/archive/gui/resident.arc" && break
done
if [ -n "$ARC" ]; then ./build/test_gametex.exe "$ARC" rev1; else echo "(no game folder found: only the inflate checks run; set GAME_DIR=<game folder>)"; ./build/test_gametex.exe; fi
# the overlay's device Reset path (release + rebuild, 3 rounds; the shared Japanese face once crashed the live game with a double free): needs the game's archive for the real faces
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/overlay_preview.exe overlay_preview.c ../native/overlay.c ../native/gametex.c ../native/lobby1.c ../native/lobby1_text.c ../native/gmdtext.c -lgdi32 -luser32 -lm
if [ -n "$ARC" ]; then ./build/overlay_preview.exe build/pv_out 1280 720 "" all "$ARC" 2>/dev/null | grep -q "reset cycle: 3 x release + render ok" && echo "overlay reset cycle ok" || { echo "FAIL: overlay reset cycle"; exit 1; }
else ./build/overlay_preview.exe build/pv_out 1280 720 2>/dev/null | grep -q "reset cycle" && echo "overlay reset cycle ok (no game archive: fallback faces)" || { echo "FAIL: overlay reset cycle"; exit 1; }; fi
# the network link of the lobby (lobby_link.c) over the coop session and ENET, on the in-process "sim" transport: the state the model holds through Start / Disconnect / ready
ENET="${ENET:-../../../../tools/enet/enet-1.3.18}"
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -DWIN32 -I"$ENET/include" -o build/test_lobbylink.exe test_lobbylink.c ../native/lobby_link.c ../native/lobby1.c ../native/lobby1_text.c ../native/lobby1_input.c ../native/gmdtext.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c \
  "$ENET/callbacks.c" "$ENET/compress.c" "$ENET/host.c" "$ENET/list.c" "$ENET/packet.c" "$ENET/peer.c" "$ENET/protocol.c" "$ENET/win32.c" -lws2_32 -lwinmm
./build/test_lobbylink.exe
# the network core (net1.c: the console `net` command, exe tag, roles, settings, automatic start, load barrier with a bad line) on the "sim" transport
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -DWIN32 -I"$ENET/include" -o build/test_netcore.exe test_netcore.c ../native/net1.c ../native/lobby_link.c ../native/lobby1.c ../native/lobby1_text.c ../native/lobby1_input.c ../native/gmdtext.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c \
  "$ENET/callbacks.c" "$ENET/compress.c" "$ENET/host.c" "$ENET/list.c" "$ENET/packet.c" "$ENET/peer.c" "$ENET/protocol.c" "$ENET/win32.c" -lws2_32 -lwinmm
./build/test_netcore.exe
# the network guards (guards1.c: byte checks before the exe+1396 and pickup patches, the pickup stub, refusal reasons; fake memory with the real bytes of the build)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_guards1.exe test_guards1.c ../native/guards1.c
./build/test_guards1.exe
# the same stub executed as 32-bit code (x86 target; the game is a 32-bit process): thiscall, ret 4, ecx kept
"$ZIG" cc -target x86-windows-gnu -O0 -g -Wall -o build/test_guards1_x86.exe test_guards1_x86.c ../native/guards1.c
./build/test_guards1_x86.exe
# the co-op partner on the HUD minimap (mapptn1.c: the stock decision fails the co-op requirement, the patched one passes; byte checks, the stub's targets,
# apply / revert / refusals on the real bytes, the split's minimap cell swap, the build facts in the image), then the stub EXECUTED as 32-bit code
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_mapptn1.exe test_mapptn1.c ../native/mapptn1.c
./build/test_mapptn1.exe
"$ZIG" cc -target x86-windows-gnu -O0 -g -Wall -o build/test_mapptn1_x86.exe test_mapptn1_x86.c ../native/mapptn1.c
./build/test_mapptn1_x86.exe
# the engine hooks of the world (world1_hooks.c: table of sites with the bytes of the build, x86 length decoder, trampolines executed as 32-bit code, apply / revert / refusals on fake memory, sArrange list readers)
"$ZIG" cc -target x86-windows-gnu -O0 -g -Wall -Wno-unused-function -o build/test_world1_game.exe test_world1_game.c ../native/world1_hooks.c ../native/guards1.c
./build/test_world1_game.exe
# the shared world model of Episode 1 (world1.c: enemies by (block, index), HIT / DEATH / DESPAWN, kill credit by slot, PICKUP / DOOR / FLAG, epochs) on a simulated lossy line
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_world1.exe test_world1.c ../native/world1.c ../native/coop_proto.c
./build/test_world1.exe
# the Episode 1 story mirror (sf1.c: flags E1, Genesis rate E2, set pieces E6): the policy alone, then two PCs (real world1 models + sf1 + mock engines with the 1-2 scan
# dispatcher and the 1-1 chain) on a lossy line, either human scanning
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_sf1.exe test_sf1.c ../native/sf1.c ../native/world1.c ../native/coop_proto.c
./build/test_sf1.exe
# the 1-3 solo part (solo1.h, ep1-solo E3): the story view of FsmSetEnablePartner, the watch camera, the scene flags, the puppet hold and the downed presence on a model
# of two PCs' engines (both role choices); the model before the fix must fail the same story
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_solo1.exe test_solo1.c
./build/test_solo1.exe
# round 9, puppet authority (zone1.h, zone1_game.inc): the story's zones count the partner only by his PC's report; two PCs in the l8d situation (his
# puppet in cc_must00[9], his real human outside: no END; he walks in: the END once on both), both role orders; the model before the fix fires the END
# by the puppet; the lease; the image's FSM call sites and the glue wiring
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_zone1.exe test_zone1.c -lm
./build/test_zone1.exe
# rev1 live 11 (local1.h): the enemies the host does not simulate: the l10e timeline (R7) and the lead order on a model of both PCs; the model before the fix
# (every enemy a replica, nothing reported) leaves the host's records of the key zombies alive (its story waits at Rachel n7)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_local1.exe test_local1.c
./build/test_local1.exe
# rev1 live 12 (stage1.h, task 5): the watching PC streams the watched human's stage parts (CP_W1_STAGE); the model before the fix leaves it in the void
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_stage1.exe test_stage1.c
./build/test_stage1.exe
# rev1 live 13 (watch1.h, task 3): the watching PC shows the enemies around the watched human (reverse replicas from the client's records, un-hidden after the zone
# pass, the minimap scope); the model before the fix draws no enemy on the watcher
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_watch1.exe test_watch1.c
./build/test_watch1.exe
# rev1 live 17 (zkeep1.h rule 7): a replica the host simulates stays visible and hittable on the client across the client's own zone passes (m16a1 / m16a3:
# the guest's human crossed a zone border mid-fight, its engine slept the host's Ooze and every shot went through); the model before the fix must fail
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_zkeep1.exe test_zkeep1.c
./build/test_zkeep1.exe
# fix021 (0.2.1): an enemy the host sleeps while the guest drives it continues from the guest's state when the host wakes it (watch1.h 2z / 3z / 6, local1.h
# 4a / 5a), on a two-PC model; the 0.2.0 rules must fail it (MODEL BEFORE THE FIX FAILS lines)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_handover1.exe test_handover1.c
./build/test_handover1.exe
# allep step 4: the fall-out rescue of the own human (native/fallres1.h)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_fallres1.exe test_fallres1.c
./build/test_fallres1.exe
# fix021 candidate (rguard1.h): an enemy copy (kind 3) never dies of this PC's fall-out (f21s5 07:11:36: the guest's replicas over floor it never loaded); the model
# before the fix must fail it (MODEL BEFORE THE FIX FAILS line)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_rguard1.exe test_rguard1.c
./build/test_rguard1.exe
# allep step 4: Hunk as the second human of the solo part (native/hunk1.h)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_hunk1.exe test_hunk1.c
./build/test_hunk1.exe
# rev1 live 14 (hitx1.h + local1.h's dead guard): the client's kills die by the game's own death on both PCs (the real HitInfo through the Raid applier on the
# host, the replica's own update on the client), local1 never takes a replica the host's DEATH came for, the finish of a dead enemy no death state reached;
# on a model of the engine's hit queue / death request / DeadState chain: the model before the fix must fail (the m13f defect)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_hitx1.exe test_hitx1.c ../native/world1_hooks.c ../native/guards1.c
./build/test_hitx1.exe
# rev1 live 14 (watchcam1.h, task 2): the watching PC's camera = the watched human's own view (the active PC's gameplay camera, smoothed, written after manager
# 1's stock update); the model before the fix never turns with the watched player
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_watchcam1.exe test_watchcam1.c
./build/test_watchcam1.exe
# rev1 live 14 (mot1.h, task 2): a replica enemy plays its owner's motions (layer ids sent on change, ordered per key and layer, a new unit sends again); the
# model before the fix keeps the spawn motion
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_mot1.exe test_mot1.c
./build/test_mot1.exe
# rev1 live 14 (bag1.h, task 3): each puppet carries and holds its owner's current gun (the archive loaded the stock way first); the m14a bags before the fix
# show different guns on the two PCs
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_bag1.exe test_bag1.c
./build/test_bag1.exe
# fix021 (0.2.1): the world of a level instance waits for this PC's own level (a barrier opened by its timeout: the 1-2 start crash of fw1 23:04 / 23:53)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_wgate1.exe test_wgate1.c
./build/test_wgate1.exe
# rev1 live 15 (coopsave1.h, task 5): the co-op game's own save; the solo DATA0.DAT never written by a co-op game (the model before the fix overwrites it)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_coopsave1.exe test_coopsave1.c
./build/test_coopsave1.exe
# E11 (grab1.h, ep1-solo): the own human held by an enemy sends CP_SF_HELD, the partner's real receiver (puppet1.c) holds the puppet through the bite and runs
# it again afterwards; the model before the fix (no HELD) lets the break-free waggle walk the puppet
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_grab1.exe test_grab1.c ../native/puppet1.c ../native/coop_proto.c -lm
./build/test_grab1.exe
# E8 (prompt1.h, ep1-probe): a story prompt pressed by one human counts on both PCs (two PCs, the local-pad action command manager, a prompt script like
# 11_10b_0; the model before the fix leaves the other copy waiting); plus the glue wiring
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_prompt1.exe test_prompt1.c
./build/test_prompt1.exe
# the world model over the real session code (world1_net.c: CP_EVENT / CP_WORLD envelopes, host relay, epoch of the level instance, load barrier) on the sim transport with loss
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -DWIN32 -I"$ENET/include" -o build/test_world1_net.exe test_world1_net.c ../native/world1_net.c ../native/world1.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c   "$ENET/callbacks.c" "$ENET/compress.c" "$ENET/host.c" "$ENET/list.c" "$ENET/packet.c" "$ENET/peer.c" "$ENET/protocol.c" "$ENET/win32.c" -lws2_32 -lwinmm
./build/test_world1_net.exe
# the role / barrier / restart logic (rolebarrier1.c: roles, hook byte checks, rendezvous, load barrier, Retry / Continue triple) on fake memory and a simulated line; then the detour stubs run as 32-bit code
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_rolebarrier1.exe test_rolebarrier1.c ../native/rolebarrier1.c
./build/test_rolebarrier1.exe
"$ZIG" cc -target x86-windows-gnu -O0 -g -Wall -o build/test_rolebarrier1_x86.exe test_rolebarrier1_x86.c ../native/rolebarrier1.c
./build/test_rolebarrier1_x86.exe
# continue1 (notes/continue1.md): the Continue after a shared game over: the host's checkpoint record with RESTART (the guest's movie decision), the level start
# rendezvous and the shield after a restart, the follow through the guest's own stock Continue, Continue chosen on the guest (CONTREQ); the models before the fixes fail
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_continue1.exe test_continue1.c ../native/rolebarrier1.c
./build/test_continue1.exe
# the same logic over the real link code (lobby_link.c / coop_session.c on the sim transport): roles, START, load barrier, event rendezvous, host triple
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -DWIN32 -I"$ENET/include" -o build/test_rolebarrier1_net.exe test_rolebarrier1_net.c ../native/rolebarrier1.c ../native/lobby_link.c ../native/lobby1.c ../native/lobby1_text.c ../native/lobby1_input.c ../native/gmdtext.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c \
  "$ENET/callbacks.c" "$ENET/compress.c" "$ENET/host.c" "$ENET/list.c" "$ENET/packet.c" "$ENET/peer.c" "$ENET/protocol.c" "$ENET/win32.c" -lws2_32 -lwinmm
./build/test_rolebarrier1_net.exe
# the headless bot (real ENet): the plain protocol scenario and, with --lk, the lobby -> start -> load barrier scenario of the network core (run_net_tests.py, run_lk_bot.py)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -DWIN32 -I"$ENET/include" -o build/coop_bot.exe coop_bot.c ../native/lobby_link.c ../native/lobby1.c ../native/lobby1_text.c ../native/lobby1_input.c ../native/gmdtext.c ../native/puppet1.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c \
  "$ENET/callbacks.c" "$ENET/compress.c" "$ENET/host.c" "$ENET/list.c" "$ENET/packet.c" "$ENET/peer.c" "$ENET/protocol.c" "$ENET/win32.c" -lws2_32 -lwinmm
# the puppet logic (puppet1.c: pad sender / receiver, holds, dead-man, position correction) and the wire protocol with CP_PAD; no game, no network
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/test_puppet1.exe test_puppet1.c ../native/puppet1.c ../native/coop_proto.c -lm
./build/test_puppet1.exe
# the second human's aim gate (aimgate1.h: sGamePad+0x883 is the OWN human's "may aim"; a model of the frame, stock vs gate, and the build facts in the image)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_aimgate1.exe test_aimgate1.c
./build/test_aimgate1.exe
# the narrow keyboard-mode fix (kbskip1.h: the partner's pad kept through FUN_0088CB10, the keyboard still on the current slot; a model of the engine's pad frame + the build facts in the image), 64- and 32-bit
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_kbskip1.exe test_kbskip1.c
./build/test_kbskip1.exe
"$ZIG" cc -target x86-windows-gnu -O0 -g -Wall -o build/test_kbskip1_x86.exe test_kbskip1.c
./build/test_kbskip1_x86.exe
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_proto.exe test_proto.c ../native/coop_proto.c -lm
./build/test_proto.exe > build/test_proto.out
tail -1 build/test_proto.out
# the session and the Steam transport tests (the session code over real ENet on loopback; the Steam backend on the mock)
for t in test_session test_steam; do
  X=""; [ "$t" = test_steam ] && X="steam_mock.c"
  "$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -DWIN32 -I"$ENET/include" -o build/$t.exe $t.c $X ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c \
    "$ENET/callbacks.c" "$ENET/compress.c" "$ENET/host.c" "$ENET/list.c" "$ENET/packet.c" "$ENET/peer.c" "$ENET/protocol.c" "$ENET/win32.c" -lws2_32 -lwinmm
  ./build/$t.exe > build/$t.out
  tail -1 build/$t.out
done
# 0.1.0 (rel010, from the Revelations 2 mod) the chain rule (native/chain_logic.h): an Ultimate ASI Loader or a copy of the mod as dinput8_real.dll is never loaded, a plain wrapper is.
# Real files when present: the system's 32-bit dinput8.dll (load), the mod's own build (copy of the mod), UAL_FILES="<dll>;<dll>" (loaders, not in the repository)
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -o build/test_chain.exe test_chain.c
set -- ./build/test_chain.exe
for f in /c/Windows/SysWOW64/dinput8.dll; do [ -f "$f" ] && set -- "$@" --load "$f"; done
for f in ../native/build/dinput8.dll "${OUT_DLL:-}"; do [ -n "$f" ] && [ -f "$f" ] && set -- "$@" --coop "$f"; done
OLDIFS=$IFS; IFS=';'; for f in ${UAL_FILES:-}; do [ -f "$f" ] && set -- "$@" --ual "$f"; done; IFS=$OLDIFS
if "$@" > build/test_chain.txt; then tail -2 build/test_chain.txt; else cat build/test_chain.txt; echo "FAIL: test_chain"; exit 1; fi
# (rel010) an Ultimate ASI Loader-like winmm.dll initialised before the mod (the mod imports winmm: the installer's copy of a player's loader runs so):
# it may write back only the mod's log-only kernel32 hooks it wraps itself (GetProcAddress, LoadLibraryA / W); every hook the co-op needs stays (tests/ual_test.sh, 32-bit stand-ins).
# The mod's DLL: ../native/build/dinput8.dll or OUT_DLL
UDLL=""; for f in "${OUT_DLL:-}" ../native/build/dinput8.dll; do [ -z "$UDLL" ] && [ -n "$f" ] && [ -f "$f" ] && UDLL="$f"; done
if [ -n "$UDLL" ]; then MOD_DLL="$UDLL" ZIG="$ZIG" sh ual_test.sh > build/ual_test.txt || { cat build/ual_test.txt; echo "FAIL: ual_test"; exit 1; }; tail -1 build/ual_test.txt
else echo "(ual_test: no native/build/dinput8.dll; sh native/build.sh first, or OUT_DLL=<file>)"; fi
