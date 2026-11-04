# Resident Evil Revelations co-op (work in progress)

Network co-op and split screen for the story campaign of Resident Evil: Revelations (2013 PC, Steam build 13298487, `rerev.exe`, 32-bit).
Status: **developer build, not released.** The proxy gates on the exe SHA-256 (other builds: it only forwards `DirectInput8Create`), works on the live game
and never touches Raid (every module stops and removes its hooks while a Raid scene / session is active). Nothing here modifies game files or the Steam stub.

What works on the real game (test copy, main PC):

- **Second human on one PC** (`native/capture.c`, console `cap ...`): the AI partner of the campaign is taken over by a second pad (aim, fire, reload, melee, weapon change,
  menus), by a remote input record (network layer API) or by a script; the own slot decides who the engine creates as the human.
- **Split screen** (`native/split.c`, console `split ...`): two local humans, each with an own camera, own HUD (ammo / weapon / reticle of the own character), the second
  human's pause and item menu on his own pad, cutscenes / pause / game over in whole view, checkpoint reloads and the game's own Exit survive. Side by side or top / bottom.
  The engine already renders several viewports natively; the module switches viewport 2 on with the second camera manager and builds a second cockpit unit set with the engine's own
  calls. API for a lobby: `capture_bind`, `capture_set_own`, `split_set_layout`, `split_enable` (see `split.h`).
- `native/steam.c`: logs the game's Steam calls; `REV1_ACHBLOCK=1` (set by the lab scripts) blocks achievement / stat writes in test runs.

Layout: `native/` proxy (C, 32-bit, built with Zig: `sh native/build.sh`; hot-loadable experiment plug-ins `plug_*.c`: `build_plug.sh`, `build_split.sh`), `tools/r1console.py` console client.

Variables: `REV1_LOG=<file>`, `REV1_CONSOLE=<port>` (UDP on 127.0.0.1), `REV1_ANYEXE=1` (lab only), `REV1_SPLIT=v|h` (start with the split screen requested, side by side / top-bottom),
test-window variables of the lab (`REV1_WINPOS`, `REV1_WINSIZE`, `REV1_BACKGROUND`, `REV1_MUTE`, `REV1_ADAPTER`).
Console words (UDP): `split status | on [v|h] | off | lay v|h | hud | menu | view | probe`, `splitkill` (emergency off from any thread), `cap status | bind | own | src | ...`.
Licence: MIT.
