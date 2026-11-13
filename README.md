# Resident Evil Revelations Co-op

[![Latest release](https://img.shields.io/github/v/release/dolbaras/revelations-coop?label=latest%20release)](https://github.com/dolbaras/revelations-coop/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

Two-player co-op for the story campaign of **Resident Evil Revelations** (2013, PC, Steam build 13298487): split screen on one PC with two gamepads, or network co-op on two PCs (one person each, Steam friends or IP address). The campaign's AI partner is played by the second player, with an own camera, HUD and pause / item menu.

## Status

**Version 0.2.1** ([release notes](https://github.com/dolbaras/revelations-coop/releases/latest)): network co-op follows the story section by section, from Episode 1 to the end of the campaign, with whichever pair the story gives (Jill and Parker, Chris and Jessica, Keith and Quint ...); where the story has only one playable character, the second player watches that character until the two meet again. Split screen on one PC works in every part of the story with two playable characters. 0.2.1 fixes network play: an enemy that chased the guest into another area now keeps the guest's damage when the host gets there; the partner's character holds exactly the partner's guns; the guest's character starts the session's first level with its knife, spare ammunition and herbs; the guest no longer crashes when the first level begins after a long wait; an enemy no longer vanishes from the guest's screen while it is still alive for the host; the Campaign page shows the co-op save while co-op is on. Different versions do not play together: the lobby refuses the other version and says so.

How it is tested: on the author's test PCs, two copies of the game (one per PC) in one co-op session; the arrival, the characters and the movement of each network section were checked on two PCs; parts of the routes were driven by test aids (teleports, scripted walking), not by people. This version was not played by people before its release, and not yet tried between two different networks (Steam relay): please report what you find. Both players need the same version of the mod.

## What works

✅ checked in the game on test PCs · 🟨 partly checked (the row says what is missing) · ⬜ not tested · — not in this version

| Area | Status |
|---|---|
| "Co-Op Play" row in the main menu (the fourth entry, in the game's own style; F6 or a gamepad's BACK open it too) and the co-op lobby: co-op on / off, which character player 1 plays, screen layout, player 1's gamepad, Host / Join, Steam friends or Direct IP | ✅ the mouse on the new row is not checked |
| Split screen on one PC with two gamepads: own camera, HUD (ammunition, weapon, reticle) and pause / item menu for each player; side by side or top / bottom; cutscenes, pause and game over in whole view; every part of the story with two playable characters | ✅ |
| Network co-op, Episode 1 (Jill and Parker), both role orders: lobby -> New Game -> 1-1 -> 1-2 -> 1-3 -> the end of the episode on both PCs; game over and Continue shared | 🟨 the gameplay of 1-2 itself was skipped by a test command, not played in a co-op session |
| Network co-op, Episode 2 to the end of the campaign, section by section: arrival, characters and movement of each section on two PCs | 🟨 not verified: the last boss fight played through; section 12-3 was only started directly |
| Join through Steam friends ("Join Game" in the friends list, the host's invite; no port forwarding) or by IP address (UDP 28410); another mod version or game build is refused with a reason | ✅ on two PCs of one network |
| The host runs the enemies, the guest's shots count; doors, key items, pickups and story flags kept in step; both PCs wait for each other at every load and cutscene end | ✅ |
| Downed / revive in a network game (the host's choice: off, 30, 60, 120 seconds or no limit; 60 by default) | ✅ |
| The Genesis scanner for the second player too: both get it when one has it; network: each scans on the own PC, an item a scan reveals shows on both PCs; split screen: the second player's scan in full screen (one scanner at a time); own analysis rate and herbs per player (part 2 of Episode 1: the team's rate) | 🟨 network checked (both get it, the team rate in 1-2, own rates after it, a revealed item on the other PC); split screen: the full-screen view, the wait while the first player scans, the own rate and herb checked (the rate set by a test command); a real scan of a target by the second player in split screen not reached; the revealed item on the other PC checked with a test command |
| Give to partner (as in Resident Evil 5): in the game's item screen (BACK) a herb or a grenade (one per press) or half of an ammunition type goes to the partner standing within about 3 m who is not down; the partner takes what fits, the rest stays with you | ✅ split screen and network; keyboard and mouse checked in a network game; the game itself was run in Russian only |
| A co-op save of its own: a co-op game never writes your solo save (DATA0.DAT); Continue keeps the save's characters on both PCs | ✅ |
| The partner on the minimap and on the map screen (network and each half of the split screen) | ✅ |
| Steam achievements: not blocked; each PC credits only what its own player did | ✅ |
| Other mods: an Ultimate ASI Loader in `dinput8.dll` keeps running as `winmm.dll`; another mod's `dinput8.dll` put over co-op's later is kept as your original | ✅ |
| Installer, uninstaller and the self-updating "Resident Evil Revelations Co-op" shortcut (SHA-256 checked) | ✅ |
| Gamepads: compact numbering, "player 1 uses gamepad 2", a pad that Windows shows twice counted once | ✅ |
| Play between two different networks (Steam relay) | ⬜ |
| Played through by people (this version was released without a play session by people) | ⬜ |
| HUNK as the second player in the Guest Cabin part of Episode 2, where the story locks the partner away (network and split screen) | 🟨 appears, walks and shoots on two PCs and in split screen; the end of the part leads both PCs into Episode 3 (reached with test help); the part's own story not played through with HUNK |
| Raid mode and the game's own online play | — never touched |

## Download and install

1. Download from [Releases](https://github.com/dolbaras/revelations-coop/releases/latest): `revelations-coop-setup-<version>.exe` (one file, nothing to extract) or `revelations-coop-<version>.zip` (extract it anywhere and run `Install Co-op.bat`; the same installer). For network play install it on BOTH PCs.
2. The installer finds your Steam copy (or asks for the folder that holds `rerev.exe`), keeps a `dinput8.dll` of someone else's in `coop-backup`, adds the mod and makes the "Resident Evil Revelations Co-op" shortcut (desktop, Start menu). It is in the game's 13 languages.
3. Start the game with that shortcut (it starts the game through Steam): before the game starts it installs a newer release from GitHub by itself (the download is checked against the SHA-256 GitHub gives for it); offline the game starts as it is. In the main menu choose **Co-Op Play**.

The setup exe is not code-signed, so Windows SmartScreen may warn ("More info" -> "Run anyway"); the zip with `Install Co-op.bat` does the same. To remove the mod run `Uninstall Co-op.bat` in the game folder (or the installer again, "Remove co-op"): it puts the backed-up files back; your saves are not touched. Details, network ports and the limits are in `README-COOP.txt` inside the zip (source `docs/release-readme.txt`).

## Requirements

- The Steam version of Resident Evil Revelations, build 13298487 (the installer checks `rerev.exe` and refuses any other one). The mod changes no game file other than adding `dinput8.dll`; it does not touch the Steam stub.
- Windows 10 or 11, PowerShell 5.1 (part of Windows).
- Split screen: two XInput (Xbox-compatible) gamepads.
- Network by IP address: the host's game listens on UDP 28410 (allow the game in Windows Firewall; over the internet the host forwards UDP 28410). Steam friends need no port.

## How to play

- Main menu -> **Co-Op Play** (or F6 / a gamepad's BACK) opens the lobby: switch co-op on, choose which character player 1 plays (player 2 takes the other), the layout and player 1's gamepad.
- **Split screen:** with co-op on, start the story with the game's own New Game, Continue or Load Game; the second player uses the second gamepad.
- **Network:** one player chooses "Host", the other "Join" through "Steam friends" ("Join Game" in the Steam friends list or the host's invite) or "Direct IP" (the "Host address" row); when both are ready, the host starts the story with the game's own New Game or Continue and the guest's game follows.
- While co-op is on, the title's Continue continues the co-op save (`%LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS\rev1_coop_save`); with co-op off your solo Continue loads your solo progress as before.

### Controls

| Key / pad | What it does |
|---|---|
| F6 / gamepad BACK | opens the co-op lobby (besides the Co-Op Play row) |
| Second gamepad | the second player in split screen |

## Languages

The lobby, the installer and the shortcut's messages are in the game's 13 languages; character names and the menu words come from the game's own message files.

## Known limits / not verified

- Network play is for one person per PC; split screen and network are not used together. In the Guest Cabin part of Episode 2 the second player plays HUNK instead of the partner the story locks away: HUNK appears, walks and shoots there (on two PCs and in split screen) and the end of the part leads both PCs into Episode 3, but the part's own story (its scenes, doors and enemies) was not played through with HUNK.
- Not verified with two real PCs in this version: the last boss fight of the campaign played through (its end flow after it is verified), and the section 12-3, which could only be started directly.
- Genesis in split screen: while the second player scans, the screen shows his scanner view in full (the game has one scanner view, so one player scans at a time: the first to start, the other waits); his analysis rate is kept only while the game runs (it starts at 0 again after the game is restarted).
- Give to partner: an item that is on its way to the partner at the very moment a new section starts is not in that section's first checkpoint, so loading that checkpoint again (a game over before the next one) takes it back, like everything else since. Where the story changes the characters with the section (Episode 9 to 10), an item still on its way lands with the new character on that PC; it is not lost.
- Joining a running network game is not possible: if one player's game closes or the connection is lost, start a new co-op game from the lobby (Continue picks up the checkpoint).
- At the first level of a network session the host's character on the guest's screen carries no knife, so a knife attack of the host is not shown there (it counts on the host's PC as usual).
- The installer and the setup exe are not code-signed (see above).

## Reporting a bug

Open an [issue](https://github.com/dolbaras/revelations-coop/issues) with a short description (which part of the story, what you did, what happened, about when, which gamepads) and the zip that `coop\Collect logs.bat` in the game folder puts on your desktop (for a network game: of BOTH PCs; double-click it right after the problem, before you start the game again). It holds the mod's logs, settings, the install record and a system summary, with your user, computer and player names, Steam names and IDs, paths, e-mail, MAC and public IP addresses removed. Nothing is uploaded; open the zip to check it before you attach it.

## For developers

- `native/` — the proxy `dinput8.dll` (C, 32-bit, built with Zig: `sh native/build.sh`; the ENet 1.3.18 sources are downloaded separately). It gates on the exe's SHA-256 (another build: it only forwards `DirectInput8Create`), works on the live game and never touches Raid (every module stops and removes its hooks while a Raid scene or session is active).
- `tools/` — installer, launcher, texts in 13 languages, packaging, the test console client `r1console.py`; `tests/` — offline tests (`sh tests/build.sh`, `tests/test_installer.ps1`), a headless bot (`tests/coop_bot.c`).
- `docs/` — `releasing.md` (the release protocol the self-update relies on), `downed-revive.md`, `development.md` (test variables, console words, module notes).
- Version scheme: pre-releases 0.0.1 and 0.0.2 (marked "pre-release", never installed by the shortcut), full releases from 0.1.0; the shortcut follows only `releases/latest`.

## Credits and third-party

- ENet 1.3.18 (MIT) for the network transport; notice in `docs/enet-license.txt`, shipped as `ENET-LICENSE.txt` in the zip.
- Ultimate ASI Loader (ThirteenAG): kept running next to the mod as `winmm.dll` when you had it; not shipped.

## License

MIT (see `LICENSE`). No game files are in this repository or in the releases; you need your own Steam copy of the game. Resident Evil Revelations is a trademark of Capcom; this project is not affiliated with Capcom.

## По-русски

Resident Evil Revelations Co-op — кооператив вдвоём для сюжетной кампании Resident Evil Revelations (2013, PC, Steam): разделённый экран на одном ПК с двумя геймпадами или игра по сети на двух ПК (друзья Steam или IP-адрес). Скачайте `revelations-coop-setup-<версия>.exe` или zip со страницы [выпусков](https://github.com/dolbaras/revelations-coop/releases/latest) и запустите установщик (для игры по сети — на обоих ПК); дальше запускайте игру ярлыком «Resident Evil Revelations Co-op»: он сам ставит новые версии. В главном меню — пункт **Co-Op Play**. Предметы можно передавать партнёру, как в Resident Evil 5: в экране предметов игры (кнопка BACK) на траве, гранате или патронах появляется «Отдать партнёру», если партнёр рядом (до 3 м) и не лежит без сил. Сканер Genesis есть у обоих игроков: на разделённом экране сканирование второго игрока идёт на весь экран. Описание каждого выпуска есть и на русском.
