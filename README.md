# Resident Evil Revelations co-op (work in progress, stage 1 skeleton)

Network co-op and split screen for the story campaign of Resident Evil: Revelations (2013 PC, Steam build 13298487, `rerev.exe`, 32-bit).
Status: **skeleton only, never run**. The proxy gates on the exe SHA-256, forwards `DirectInput8Create`, writes a log and offers a read-only UDP
test console with the class-name walker. Nothing here modifies the game files or the Steam stub, nothing hooks game code yet.

- `native/` `dinput8.dll` proxy (C, 32-bit, built with Zig: `sh native/build.sh`): `proxy.c` log, SHA-256 gate, crash log, import patching,
  `classes.c` MT Framework class-name walker (vtable[4] -> `mov eax,DTI; ret` -> DTI +4 name, +16 parent, +24 size in BYTES), `console.c` UDP console.
- `tools/r1console.py` console client.

Variables: `REV1_LOG=<file>` (default `rev1_coop.log` next to the exe), `REV1_CONSOLE=<port>` (UDP on 127.0.0.1), `REV1_ANYEXE=1` (lab only).
Note: .text of `rerev.exe` is plain on disk, but the Steam stub (`.bind`) runs first; the mod never touches Raid.
Licence: MIT.
