# Releasing

Players start the game with the "Resident Evil Revelations Co-op" shortcut, which runs `coop\coop-launcher.ps1` (`tools/launcher.ps1`). Before the game starts it reads
`https://api.github.com/repos/dolbaras/revelations-coop/releases/latest` (the repository name is a constant at the top of `tools/launcher.ps1` and in its URL check)
and installs that release when its tag is newer than `coop\version.txt`. Publishing a full release therefore updates every player who uses the shortcut the next time
they start the game, so a release must be finished and tested before it goes up. Nothing is published from this workspace without the owner.

What the launcher already out there relies on, so every later release keeps it (the same protocol as the Revelations 2, Jericho and TimeShift mods of this workspace):

- **Tag** `v<major>.<minor>.<patch>` (parsed as a .NET `[version]`), a full release: drafts and pre-releases are not "latest" and are never installed.
- **Asset** named `revelations-coop-<version>.zip` (the first asset matching `revelations-coop-*.zip`), attached to that release, so its URL is under
  `https://github.com/dolbaras/revelations-coop/releases/download/`. The launcher checks the download against the `sha256:` digest GitHub gives for the asset
  (`"digest": "sha256:<64 hex>"` in the release JSON). The 0.0.x launchers install without a check when an asset has no digest; **from 0.1.0 on an asset without a
  digest is not installed at all**. GitHub gave a digest for every asset of this repository (v0.0.1, v0.0.2, read 2026-11-09): always upload the release's files fresh,
  never re-attach an old asset.
- **`coop-installer.ps1` and `coop-texts.ps1` at the root of the zip** (as `tools/package.ps1` builds them), the installer taking
  `-Game <folder> -AutoClose -NoShortcut`: install into that folder with the window, close it by itself after a successful install, exit code 0 on success and
  non-zero on failure; `-NoShortcut` refreshes only the shortcuts the install record lists.
- **`files\coop\version.txt`** holding the version without the `v`.
- **`<game>\coop\install.txt`**, the install record the next installer reads: `version=`, then one line per `file=`, `backup=`, `chain=` (from 0.1.0: a copy made for the
  player's ASI loader, also listed as `file=`) and `shortcut=` (several shortcuts on one line are split after each `.lnk`, empty ones dropped). Older installers ignore `chain=`.
- The mod's DLL carries the words `RE Revelations co-op proxy` (the installer tells it from a dinput8.dll of someone else's by them): keep the log line (`native/proxy.c`;
  `tools/package.ps1` refuses a DLL without it).

A change to any of these needs a launcher that copes with both the old and the new form, shipped one release before the old form is dropped. The update from 0.0.x to
0.1.0 is made by the 0.0.x launcher (the one on the player's PC): `tests\test_installer.ps1 -OldZip` runs exactly that with the published v0.0.2 zip.

## What the launcher does when something goes wrong

The game always starts (through Steam for a Steam copy), nothing half-installed stays behind, and `coop\launcher-log.txt` says why. Each line but "no network" is a
case of `tests\test_installer.ps1` (section 4), run against a small HTTP server on 127.0.0.1 (the same launcher code as the Revelations 2 mod's, 2026-11-06):

| Case | Behaviour (log line) |
|---|---|
| GitHub's rate limit (60 unauthenticated requests an hour; 403 or 429 with `X-RateLimit-Remaining: 0`) | `update: GitHub's rate limit is used up (resets HH:mm) [HTTP 403]: no update check this time` |
| No full release yet / missing file | `update: not found [HTTP 404] ...` |
| No network, no name lookup | `update: no network (<status>): ...` |
| The feed does not answer | `update: the feed did not answer in 6 seconds` (`GetResponseAsync` + `Wait(6000)`) |
| The feed sends its headers and stops | `update: no network (Timeout): ...` (6 s read timeout; at most 10 s and 1 MB in all) |
| A feed with a byte-order mark | read normally |
| A pre-release or draft in the feed | `refused: the release is a pre-release or a draft`, nothing downloaded |
| No `sha256:` digest on the asset | `refused: the release gives no usable checksum`, nothing downloaded |
| Wrong checksum | message box (the window is up), version stays |
| The download breaks off | `update: the download broke off (<got> of <total> bytes)` |
| The archive is missing (404) | `update: not found [HTTP 404] ...` |
| This folder's `rerev.exe` runs | `the game runs already`, nothing asked |
| A second launcher (a double-clicked shortcut) | `another launcher runs` (mutex `Local\Re1CoopLauncher`) |
| The update | `downloaded <n> bytes from <scheme://host after the redirect>`, `checksum ok <sha256>`, `installer exit 0`; the temp folder is removed after a success and after every failure |

## Building

```
sh native/build.sh                                      # native\build\dinput8.dll (32-bit, Zig)
OUT=build-x/dinput8.dll sh native/build.sh              # the same into native\build-x\ (another agent may use native\build)
powershell -File tools\package.ps1 -Version 0.1.0       # dist\revelations-coop-0.1.0.zip
powershell -File tools\package.ps1 -Version 0.1.0 -Exe  # + dist\revelations-coop-setup-0.1.0.exe (workspace tools\mkinstaller; unsigned)
powershell -File tools\package.ps1 -Version 0.1.0 -Exe -Dll native\build-x\dinput8.dll -Dist <folder>   # another DLL, another output folder
```

In a worktree (`work\wt-*`) set `ZIG=<workspace>/tools/zig/zig-x86_64-windows-0.16.0/zig.exe` and `ENET=<workspace>/tools/enet/enet-1.3.18` for `native/build.sh` (its defaults
are relative to `mod\`); `package.ps1` finds `<workspace>\tools\log-collector` and `tools\mkinstaller` from `mod\` and from a worktree (the first `tools\<name>` above the
repository). **`package.ps1` refuses a DLL whose `R1_VERSION` is not `-Version`** (the DLL holds the version as a string of its own; `-AnyDllVersion` for tests and dry runs of
a coming version): `coop\version.txt` is what the launcher compares, the DLL's version is what the log, the lobby and the network hello say (another mod version is refused).
The zip is written by .NET's `ZipArchive` with every entry name using "/" (Compress-Archive of Windows PowerShell 5.1 wrote `files\coop\...`, as in the published 0.0.1 zip:
`unzip` then warns "appears to use backslashes as path separators" and exits 1).

**The release DLL is built from the exported public tree, not from `mod\`** (`lab/export_public.sh` of the lab, outside this repository; below). The lab branches carry `notes/`
(the agents' resume files and lab notes) and their history; neither goes public: the public repository gets one new commit per release with the exported tree.

The zip holds `Install Co-op.bat`, `coop-installer.ps1`, `coop-texts.ps1` (the 13 languages of the game; shared by the installer and the launcher), `README-COOP.txt`,
`LICENSE.txt` and `files\` (`dinput8.dll`, `coop\coop-launcher.ps1`, `coop\coop-installer.ps1`, `coop\coop-texts.ps1`, `coop\README.txt`, `coop\version.txt`,
`coop\collect-logs.ps1` + `coop\log-profiles\*.psd1` + `coop\Collect logs.bat` (the bug-report collector, below), `coop\payload.txt` = the list of every payload file
that the installer checks before it touches the game). `README-COOP.txt` and `coop\README.txt` are `docs\release-readme.txt` with `@VERSION@` filled in. The package sets no
environment variable and the shortcut's launcher sets none: `REV1_ACHBLOCK` (Steam unlocks blocked) is a lab default only, so a player's game passes every achievement call
through to Steam.

## Bug reports: Collect logs.bat

`package.ps1` takes the bug-report collector every co-op mod of the workspace shares from `<workspace>\tools\log-collector` (`collect-logs.ps1` and one profile per
game, all of them, so that `-All` also takes the other co-op mods found on a PC; `-LogCollector <folder>` for a clone placed elsewhere) and writes
`coop\Collect logs.bat` (`collect-logs.ps1 -Mod revelations1`). A double-click puts `revelations-coop-report-<date>-<time>.zip` on the desktop: `report.txt` (mod
version, SHA-256 of `rerev.exe`, `dinput8.dll` and a chained `dinput8_real.dll`, install record summary, Windows and its UI language, graphics adapters and drivers,
memory, CPU, display modes), `logs\rev1_coop*.log`, `coop\install.txt`, `install-log.txt`, `version.txt`, `launcher-log.txt`, `settings\rev1_coop.ini` and the
game's `config.ini` (`%LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS`), `temp\re1-coop-install.log`, `files.txt` (names and sizes of `coop\` and of the game folder's
top level). Cleaned out: user, computer and domain names, the profile, Documents and game paths, any `<drive>:\Users\<x>`, `name=` of the ini, the Steam names the log quotes
(own and the partner's: `you are <id> "<name>"`, `linked with`, the session lines) and Steam's account names, Steam IDs, public IPv4 / IPv6 addresses (LAN kept),
e-mails, MACs; nothing is uploaded. The uninstaller removes it with `coop\`. Tests: `tests\test_installer.ps1` (the payload has it, it is installed and in the record,
the installed script writes its zip, uninstall removes it) and the shared `tools\log-collector\tests\test-collect.ps1`.

## What the installer does

- Finds the game: its own folder, the Steam uninstall key of app 222480, every Steam library (`libraryfolders.vdf` and the `appmanifest_222480.acf` installdir), common
  folders, or the player picks it. **It checks that `rerev.exe` is the Steam build 13298487** (SHA-256 `1573b79e...48aa`, the same constant as `R1_EXE_SHA256` in
  `native/r1.h`, whose first 8 bytes the DLL sends as the exe tag of the network hello) and refuses any other exe in the player's language; `-AnyExe` or
  `RE1_COOP_ANYEXE=1` skip the check (tests only).
- Refuses while a `rerev.exe` runs from that folder. A folder Windows protects is installed by an elevated copy of the installer (Windows asks the player).
- Keeps a `dinput8.dll` of someone else's (an ASI loader, a graphics fix) in `<game>\coop-backup`, once; its own DLL of an older version is never kept as the original. A
  `dinput8_real.dll` the player placed (the mod's chain rule) and every other file are left alone.
- **When that `dinput8.dll` is an Ultimate ASI Loader** (32-bit, exports `IsUltimateASILoader` and winmm's `timeGetTime`) and the game folder has no `winmm.dll`, a copy of it
  is made as `winmm.dll`, and the loader's `dinput8.ini` (if any) as `winmm.ini`: a name the loader serves and `rerev.exe` imports (`WINMM.dll`, after `DINPUT8.dll`; the
  mod's DLL imports winmm too), so Windows loads it from the game folder at the start and it loads its plugins (`scripts\*.asi`) as before, next to the mod's `dinput8.dll`.
  The record lists the copies as `chain=` and `file=` lines (written before the copies are made); reinstall and update keep them, the uninstaller removes a copy only while
  it still equals its source and then puts the loader back as `dinput8.dll`. Not `dinput8_real.dll`: the loader knows itself only by its file name and under that name shows
  "This library isn't supported." and closes the game (its source: `proxy::Load` of v10.0.2, `LoadOriginalLibrary` of v7.8.0); the proxy never loads such a loader, nor a
  copy of the mod, as `dinput8_real.dll` (`native/chain_logic.h`, log line `chain: dinput8_real.dll is an Ultimate ASI Loader: not loaded ...`). Any other
  `dinput8.dll`, or a loader next to a `winmm.dll` of someone else's, stays off while co-op is installed; the result page says which of the two happened.
- **A `dinput8.dll` another mod put over the co-op one after the install** (it is not the mod's DLL by `DLL_MARK` while the record lists `dinput8.dll` as the mod's): an
  install or update (also the launcher's) keeps it in `coop-backup\dinput8.dll` as the original (it replaces an older one there; copies made from the older one are removed
  and made again from it, the record is written at once) and puts the co-op DLL back; the uninstaller leaves it in place and does not put the older original over it
  (`overcoop`, `leftin`, `leftnote` in 13 languages). Before (0.0.1, 0.0.2): the update overwrote it without a copy, the uninstaller deleted it.
- Other mods' proxy DLLs (`d3d9.dll` of DXVK / ReShade, `dsound.dll` + ini, `version.dll`, `xinput1_3.dll`) and plugin folders are not in the payload: never touched.
- Copies the payload, writes `coop\install.txt` (`coop\version.txt` last: the launcher reads it, an update that broke off is tried again at the next start), the
  desktop and Start menu shortcuts (the game's own icon with a co-op badge, `coop\coop.ico`), and `Uninstall Co-op.bat` at the game folder's root.
- The shortcut starts the game through Steam (`steam://rungameid/222480`, found from the library's `appmanifest_*.acf`; a copy outside a Steam library starts `rerev.exe`).
- Removal (`Uninstall Co-op.bat`, or the installer again with "Remove co-op"): deletes what the record lists, puts the backed-up files back, removes the shortcuts, `coop`
  and `coop-backup`, the mod's logs next to `rerev.exe` (`rev1_coop.log`, `rev1_coop.N.log`) and `rev1_coop\shots` + `rev1_coop\` when they are empty (screenshots stay; from
  0.1.0 the DLL makes these folders at the first screenshot, before it made them at every game start). The game's saves, the co-op save `rev1_coop_save\COOP0.DAT` and `rev1_coop.ini` (the lobby's settings,
  `%LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS`) are not touched.
- Texts in 13 languages (en ru de fr it es pt pt-br nl pl ja zh-cn zh-tw; the game's own set), chosen by Windows' language, `-Lang` or the list in the window. The first page
  names where co-op starts (the "Co-Op Play" row), what is covered (split screen and network: the campaign), both network ways (Steam friends; IP address with UDP
  28410 and the firewall) and the co-op save (round 15: a network game keeps its own save on each PC, the solo save is not changed); the launcher's update window carries the network line.

## Other mods' dinput8.dll: the DLL side

Ultimate ASI Loader v10.0.2 (`source/loader/startup/startup.cpp` HookEntryPoints / RestoreSlots, read for the Revelations 2 mod on 2026-11-08) patches the exe's kernel32
slots of about 30 functions in its DllMain, records the value it saw and writes every one back at the exe's first such call, before it loads its plugins. A loader as
`winmm.dll` always initialises before the mod (the mod's DLL imports winmm). Of the mod's import hooks the loader can write back only kernel32 ones that `rerev.exe` imports:
`GetProcAddress`, `LoadLibraryA`, `LoadLibraryW` (the `dyn:` log lines of `native/testmode.c`, log-only). Every hook the co-op needs stays: `ExitProcess`,
`d3d9!Direct3DCreate9`, the XInput glue (`XINPUT1_3` #2-#5), the `steam_api` hooks. `tests/ual_test.sh` (in `tests/build.sh`) shows it without the game: a stand-in loader
written from that source as `winmm.dll`, a stand-in exe with the hooked imports, the mod's DLL with `REV1_ANYEXE=1`: 3 slots written back, every product hook in place; and
the guard: the stand-in loader as `dinput8_real.dll` is not loaded. Not covered: a loader the player names `xinput1_3.dll` (it initialises after the mod and would rewrite the
XInput ordinal slots, i.e. the pad glue; the installer never makes that name). Not seen live with a real loader next to Revelations 1 (no ASI mod for this game is known;
the Revelations 2 mod ran the Fusion Fix's loader live this way on 2026-11-07).

## Tests

`powershell -File tests\test_installer.ps1 [-RealGame <folder with a COPY of the real rerev.exe>] [-OldZip <released zip>] [-Online] [-Keep]` (everything in a temp folder, no game is
started; without `-Online` no network but 127.0.0.1). The same cases as the Revelations 2 mod's test plus this mod's (2026-11-09: 266 checks with `-OldZip`, see "Release status"):
- the package: the zip's layout, every entry name with "/", the payload list, the log collector in it, the DLL version gate (another version refused, the same one taken); the
  0.1.0 package's readme (its own version, UDP 28410, the firewall, the whole campaign, no "known problems" list and no pre-release wording), `R1_VERSION`, the "Full release" section here;
- the installer: a fake exe is refused without a trace; install over a game with a foreign dinput8.dll (backup, record, two shortcuts, every other game file untouched, a
  `dinput8_real.dll` and a plugin folder of the player survive, the result says the foreign DLL is off); reinstall; uninstall (byte for byte); a game without dinput8.dll (the
  mod's logs and an empty `rev1_coop\` go too); a path with spaces and an apostrophe (a screenshot and a look-alike log stay); the installed log collector writes its zip;
- an Ultimate ASI Loader as the foreign dinput8.dll (made-up PE files, never run): the `winmm.dll` / `winmm.ini` copies, the record, the result text, reinstall + update, uninstall
  byte for byte; a `winmm.dll` the player changed, a foreign `winmm.dll`, a 64-bit loader, a loader without `dinput8.ini`, a DLL that only names the loader; other mods' proxies
  untouched; another mod's dinput8.dll put over co-op's (uninstall leaves it, an update keeps it as the original, the same over an older loader); the result texts fit their label;
- the launcher (section 4): every case of the table above, the feeds made from `tests\fixtures\github-release-v0.0.2.json` (the real v0.0.2 entry of the releases API, author /
  uploader / body trimmed), the update through a redirect, backup and shortcuts kept; the start line (`steam://rungameid/222480` for a Steam library folder, `rerev.exe` otherwise;
  `-NoStart`). The game-running case is skipped while any `rerev` runs on the PC (a lab instance must not see a second one);
- `-OldZip work\dist-pre2c\revelations-coop-0.0.2.zip` (the published v0.0.2 asset, sha256 `f2139927...0192`): that release installed, ITS launcher updates it to a 0.1.0 package of
  this tree (record, collector files, this tree's launcher, backup kept, uninstall clean); the same over an ASI loader (the update makes the `winmm.dll` copy);
- `-Online` (read only, plus ONE download of the published v0.0.2 zip, 315 KB): `releases/latest` of this repository (404 while only pre-releases exist, else the protocol's
  shape), of `jericho-coop` and `timeshift-coop`; this tree's launcher, fed a local copy of the real v0.0.2 entry marked as a full release, installs the real asset over a
  stand-in 0.0.1 (TLS, github.com's redirect, GitHub's digest), uninstall clean;
- no shipped script sets or names a lab variable; the feed constants; the installer's exe hash equals the mod's own gate; the 13 texts (same keys, same placeholders, translated, no
  Revelations 2 name or build, the network lines) and the language choice.

`powershell -File tests\test_update_real.ps1 [-Report <file>]` is the update test against the real GitHub, without publishing anything: it reads the GitHub API and the assets of the
published pre-releases only (nothing is written to GitHub), installs the published `v0.0.1` into a stand-in game folder under `%TEMP%` (never the repository, never a real game), and
runs `coop-launcher.ps1 -Feed https://api.github.com/repos/dolbaras/revelations-coop/releases/tags/v0.0.2 -NoStart -NoWindow`: once with the launcher that 0.0.1 installed (what
players really have) and once with the launcher of this tree. It needs internet; the unauthenticated GitHub API allows 60 requests an hour per address.

The installer's WINDOW is checked by `lab\inst_window_test.ps1` of the lab (pictures of the window itself, on a second monitor): test-only variables `RE1_COOP_UI_POS=x,y` (window at
that position WITHOUT taking the focus), `RE1_COOP_UI_SHOTS=<folder>`, `RE1_COOP_UI_LANGS=en,ru`, `RE1_COOP_UI_TAG`, `RE1_COOP_UI_AUTO=install|remove` (clicks the button, only for
the folder in `RE1_COOP_UI_ONLY`; a player never sets them). `coop-launcher.ps1 -Feed <url> -NoStart` (or `RE1_COOP_FEED=<url>` for a start through the shortcut) points the
launcher at another feed; with a feed of its own the launcher skips the download-URL check. New shortcuts go to `-ShortcutDir <folder>` in tests, never to the desktop.

## Versions and pre-releases

The launcher installs only `releases/latest` of the repository, and only when its tag parses as a .NET `[version]` that is HIGHER than `coop\version.txt` of the installed copy.
GitHub never reports a pre-release as "latest", so a pre-release is never installed by the shortcut: a tester installs the zip or the setup exe by hand.

| What | Version / tag | Notes |
|---|---|---|
| Pre-release 1 | `0.0.1`, tag `v0.0.1`, GitHub "pre-release" | published 2026-11-05 |
| Pre-release 2 | `0.0.2`, tag `v0.0.2`, GitHub "pre-release" | published 2026-11-06 (`R1_VERSION` was `0.0.2-pre2`) |
| First full release | `0.1.0` (tag `v0.1.0`, NOT a pre-release) | higher than any `0.0.N`, so every tester who installed a pre-release is updated by the shortcut at the next start |
| Second full release | `0.2.0` (tag `v0.2.0`, NOT a pre-release) | published 2026-11-12 from public commit `58a0d02`: network co-op over all episodes (no setting), give to partner, the Genesis for the second player, HUNK in 2-2, the split aim fix; `R1_VERSION` in `native/r1.h` is `0.2.0` |
| Fix release | `0.2.1` (tag `v0.2.1`, NOT a pre-release, Latest) | published 2026-11-13 from public commit `de5e123`: the zone hand-over of an enemy between the PCs, the partner's real guns on the puppet, the guest's start kit, the start of the shared world with the own level, the save panel, the guns of the host's save for an unarmed guest, the fall-out guard of enemy copies (`rguard`); `R1_VERSION` in `native/r1.h` is `0.2.1` |

Rules: never publish a full release with a version below `0.1.0`; never mark `v0.1.0` or higher as a pre-release while it should reach players; no "RC" wording anywhere (title,
notes, tag message, README). Both players need the same package: the network hello carries the mod version (`R1_VERSION`) and refuses another one with a reason; a release that
changes a message layout also bumps `CP_PROTO_VERSION` (`native/coop_proto.h`).

## 0.2.1 (fix release; published 2026-11-13)

- **Published:** tag `v0.2.1` (full release, Latest) on public commit `de5e123`, 2026-11-13. Assets: `revelations-coop-0.2.1.zip` sha256 `ccec18ef...`, `revelations-coop-setup-0.2.1.exe` sha256 `7bc68917...`, `dinput8.dll` in the zip `8d01f1d3...` (the DLL of the final regression).
- **Checks after the publication (2026-11-13):**
  - The release is neither a draft nor a pre-release, and each asset's `digest` equals the local SHA-256. `releases/latest` = `v0.2.1`.
  - `tests\test_installer.ps1 -Online`: 286 of 286.
  - The real update through the real feed (no `-Feed`) on a stand-in folder: the published 0.2.0 launcher logged `installed 0.2.0, latest v0.2.1`, `downloaded 738699 bytes`, `checksum ok CCEC18EF...`, `installer exit 0`; `version.txt` read 0.2.1 and the installed `dinput8.dll` was `8d01f1d3...`. A second start installed nothing. The uninstall put the stand-in back byte for byte.
- **Number:** `0.2.1`, fixes of the network game only. `R1_VERSION` = `0.2.1`; 0.2.0 and 0.2.1 refuse each other in a game by the hello's version string, with the reason. `CP_PROTO_VERSION` stays `3`.
- **Before publishing:** `lab\publish-0.2.1-checklist.md` (the final regression on the release DLL, slot by slot, each with its positive markers) and the owner's word.

## 0.2.0 (all episodes; published 2026-11-12)

- **Published:** tag `v0.2.0` (full release, Latest) on public commit `58a0d02`, 2026-11-12. Assets: `revelations-coop-0.2.0.zip` sha256 `4f11e223...`, `revelations-coop-setup-0.2.0.exe` sha256 `a29132ba...`, `dinput8.dll` in the zip `95653832...` (the DLL of the final regression).
- **Checks after the publication (2026-11-12):**
  - The release is neither a draft nor a pre-release, and each asset's `digest` equals the local SHA-256. `releases/latest` = `v0.2.0`.
  - `tests\test_installer.ps1 -OldZip work\dist-010-r17\revelations-coop-0.1.0.zip -Online`: 294 of 294.
  - The real update through the real feed (no `-Feed`) on a stand-in folder: the published 0.1.0 launcher logged `installed 0.1.0, latest v0.2.0`, `downloaded 728322 bytes`, `checksum ok 4F11E223...`, `installer exit 0`, and `version.txt` read 0.2.0. A second start installed nothing. The uninstall put the stand-in back byte for byte.

- **Number:** `0.2.0` (the first release that changes what the network game does between the sections; 0.1.x would hide that). `R1_VERSION` = `0.2.0`; the package, tag and `coop\version.txt` all say `0.2.0`.
- **How it is built in:** `native/r1.h` `#define R1_VERSION "0.2.0"`; `package.ps1 -Version 0.2.0` refuses a DLL that does not hold the string `0.2.0` (the DLL holds it as a string of its own between two zero bytes), `README-COOP.txt` and `coop\version.txt` get it from `-Version`.
- **0.1.0 -> 0.2.0 by the shortcut:** the 0.1.0 launcher (the one on the player's PC, `coop\coop-launcher.ps1`) asks `releases/latest` at the start of the game, sees a tag that parses as a higher .NET `[version]`, downloads the zip, checks it against the digest of the release JSON and runs the new installer (the protocol above: record, backup, chain).
  `tests\test_installer.ps1 -OldZip work\dist-010-r17\revelations-coop-0.1.0.zip` (the PUBLISHED v0.1.0 asset, sha256 starts `17f3348f...`, the digest recorded below) runs exactly that: ITS launcher updates a stand-in game folder to the 0.2.0 package of this tree through the local feed (268 of 268, 2026-11-11).
- **0.1.0 meets 0.2.0 in a game:** the hello carries the mod version string (`coop_session.c`: `mod_mismatch`, refusal `CP_REF_MODVER`): the host refuses the other version with the text `co-op mod X on the host, Y here: both need the same version (the Co-op shortcut updates it)` (the lobby shows it in the game's language), in both directions, before any section message is sent. So the 0.1.0 guest
  that rejects a SECTION outside Episode 1 (`ep1_only`) never gets one: nothing desyncs, a player simply updates. `CP_PROTO_VERSION` stays `3`: no message layout changed (RBM_SECTION / RBM_EPDONE are the 0.1.0 layouts; only which sections are allowed changed).
- **Before publishing:** the checklist of the full release below (the 0.2.0 version of it: `lab\publish-0.2.0-checklist.md`), `notes\allep-live.md` (verdict table), `lab\release-notes-0.2.0-draft.txt` (what is verified, what is not, what to check by hand) and the owner's word.

## Full release (every player's shortcut installs it)

A full release is what `releases/latest` returns, so the launcher of every installed 0.0.x copy updates to it at the next start. Create it only when the whole release checklist of
the plan is closed and the owner has given the word ("публикуй"); it cannot be quietly taken back once players have installed it.

Tools: `gh` is the portable one at `<workspace>\tools\gh\bin\gh.exe` with `$env:GH_CONFIG_DIR=<workspace>\tools\gh\config` (PowerShell); identity: the noreply address of the
owner's GitHub account. `<dist>` is the output folder (`work\dist-010`).

1. **Freeze:** the release branch merged, `git -C mod status --short` empty, `R1_VERSION` in `native/r1.h` is `0.1.0`; `sh tests/build.sh`, `sh tests/build_menu.sh` and
   `powershell -File tests\test_installer.ps1 -OldZip work\dist-pre2c\revelations-coop-0.0.2.zip -Online` green on that commit; `tests\test_update_real.ps1` green.
2. **Export dry run and the release DLL:** `DRYRUN=1 PUB=<scratch folder> DLL=<dist>\dinput8.dll ZIG=<zig.exe> ENET=<enet folder> TESTS=1 sh lab/export_public.sh` (exports the
   committed HEAD of `mod/`, `SRC=work/wt-<x>` for a worktree; drops `notes/`; names the lab's static-query folder without its drive path; greps for private paths, names, account
   ids, SteamIDs, any e-mail address and any "RC" wording; builds and tests the public tree; keeps its DLL).
3. `powershell -File mod\tools\package.ps1 -Version 0.1.0 -Exe -Dll <dist>\dinput8.dll -Dist <dist>` (refuses the DLL if its `R1_VERSION` is not 0.1.0). Record the SHA-256 of the
   zip, the setup exe and the DLL. Unzip the zip into a scratch folder (`unzip -t` exit 0) and grep it, the exe and the DLL with the export's pattern (only the generic
   `Users\<name>` of the collector's comments may match).
4. Install the zip (setup exe) on BOTH test PCs for the acceptance runs (outside this file: `notes\release-0.1.0-plan.md` section 6).
5. **The public commit:** `git clone https://github.com/dolbaras/revelations-coop games\re-revelations\pub` once; `sh lab/export_public.sh` (working tree in `pub\`), review
   `git -C pub diff --stat`; `git -C pub add -A`, `git -C pub commit -m "Resident Evil Revelations Co-op 0.1.0"` (with the Co-Authored-By trailer), then push `pub` main
   (`git -C pub -c credential.helper= -c "credential.helper=!'<gh.exe>' auth git-credential" push origin main`). Never push a lab branch: its history holds `notes/`.
6. `gh release create v0.1.0 --repo dolbaras/revelations-coop --target main --latest --title "Resident Evil Revelations Co-op 0.1.0" --notes-file <relnotes body> <dist>\revelations-coop-0.1.0.zip <dist>\revelations-coop-setup-0.1.0.exe`
   (no `--prerelease`, no `--draft`; the tag is made by the command on the pushed main; the notes carry the three SHA-256 and the honest scope: network co-op for Episode 1, one
   person per PC; split screen on one PC in the whole campaign).
7. Check at once: `gh release view v0.1.0 --repo dolbaras/revelations-coop --json isPrerelease,isDraft,tagName,assets` (both false, both assets there, the zip's and the exe's
   digest equal the recorded SHA-256) **and** `gh api repos/dolbaras/revelations-coop/releases/latest --jq .tag_name` prints `v0.1.0` (before the release it is a 404);
   `tests\test_installer.ps1 -Online` passes its shape check against it. Do not tell anybody before both are right.
8. Check the update on the N100 (plan H24): a test copy with the pre-release 0.0.2 installed, start the shortcut (the owner's scheduled task), see the update window, the new
   `coop\version.txt` (0.1.0) and the lobby; a second start installs nothing.
9. Tell the players. The v0.0.x pre-releases stay as they are.

**If a check fails after publishing:** take the release away at once so the shortcut stops offering it: `gh release delete v0.1.0 --repo dolbaras/revelations-coop --yes --cleanup-tag`
(or `gh release edit v0.1.0 --prerelease` first if it must stay visible), confirm that `releases/latest` no longer returns it, fix everything the failure showed, and release again
under the same version only if no player could have installed it, else under the next one.

## Pre-releases (history; for testers, never installed by the shortcut)

Pre-releases were published from `mod\` main directly (`git push origin main`, `git tag -a v0.0.N`, `gh release create v0.0.N --prerelease --title "Pre-release N (0.0.N)" ...`;
`releases/latest` stays 404). A further pre-release before 0.1.0 is not planned; if one is ever needed it uses the export of step 5 above and `--prerelease`, never `--latest`.

## Release status

Public packages so far: **pre-release 1 (0.0.1)** and **pre-release 2 (0.0.2)**, both GitHub pre-releases, and the first full release **0.1.0** (tag `v0.1.0`, `--latest`,
published 2026-11-10 from the public commit `6394f13`, an export of rev1-int `3cfa42e`; zip `17f3348f...`, setup exe `453c35dd...`, `dinput8.dll` `486dc173...`).
Checked after the publication (2026-11-10, throw-away copies, no window): `tests\test_installer.ps1 -Online` 264 of 264; the 0.0.2 launcher of a copy with 0.0.2 installed,
through the real feed (`-NoStart -NoWindow`, no `-Feed`): `installed 0.0.2, latest v0.1.0`, `checksum ok 17F3348F...8B71`, `installer exit 0`, `version.txt` 0.1.0, one shortcut,
the uninstall byte for byte back to the copy before 0.0.2; the setup exe from the release page with `-Quiet`: exit 0, the launcher then says `installed 0.1.0, latest v0.1.0`.
Not checked: the player's window path (progress window, installer window), a start through Steam, SmartScreen on a browser download.

Verified offline for 0.1.0 (2026-11-09, no game started, no window shown):
- `tests\test_installer.ps1 -OldZip work\dist-pre2c\revelations-coop-0.0.2.zip -Online`: 271 of 271 (the game-running case skipped: a lab game ran). The same test on the
  0.0.2-era installer / launcher / texts stops at the first ASI-loader case (no `winmm.dll` copy).
- `tests\test_update_real.ps1`: 38 of 38 against the real GitHub (read only): the 0.0.1 launcher updates to the published v0.0.2 through `releases/tags/v0.0.2`; this tree's
  launcher refuses that feed (a pre-release) and installs the same real entry marked as a full release (`downloaded 315009 bytes from https://release-assets.githubusercontent.com`,
  `checksum ok F2139927...0192`).
- `tests/build.sh` and `tests/build_menu.sh` green, `test_chain` 19 of 19, `ual_test` 21 of 21.
- The export dry run (`lab/export_public.sh`, `DRYRUN=1 TESTS=1`): the public tree has no `notes/`, no private path, name, account or SteamID, no e-mail; it builds and its tests pass;
  the package built from its DLL has 18 entries, all with "/", `unzip -t` exit 0, and the stub's self-test passes. The SHA-256 of the zip, the setup exe and the DLL are recorded
  when the release package is built (step 3 above); builds are not byte-reproducible (PE time stamp, debug id).

Not verified live by these tests: the installer's window with the new texts (13 languages), a real Ultimate ASI Loader next to Revelations 1, the shortcut starting the game through
Steam, the first real `releases/latest` update (only the publication shows it; step 8).
