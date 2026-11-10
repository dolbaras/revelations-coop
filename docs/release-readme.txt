Resident Evil Revelations Co-op - version @VERSION@
================================================================

Two players in the story campaign of Resident Evil: Revelations (2013 PC, Steam build 13298487): on one PC with split screen and two gamepads, or on two PCs over the network.

What it does
- The campaign's AI partner (Parker, Jessica, Chris, Keith, Quint depending on the part of the story) is taken over by a second player. Each player has an own camera, an own HUD
  with the own ammunition and weapon, and an own pause / item menu.
- One PC, split screen: the second player uses a second gamepad. The screen is split side by side or top and bottom (your choice). Cutscenes, the pause menu and the game over
  screen use the whole screen. Split screen works in the whole campaign.
- Network, two PCs: each player has the game on the own PC (both need the Steam copy and this mod) and plays the own character on the own screen. One person per PC; split screen
  and network are not used together. Network play covers Episode 1 (Jill and Parker): the guest's game follows the host's New Game or Continue, both PCs wait for each other at every
  load and at the end of each cutscene, the sections of Episode 1 follow each other on both PCs, and at the end of Episode 1 both PCs show the game's own end and the co-op
  session closes. Enemies are run by the host's PC; doors, key items and item pickups are kept in step; a game over is shared and Continue takes both players to the same checkpoint.
  Where the story has one character alone, the other player watches that character until the two meet again.
- Downed / revive in a network game (the host's choice in the lobby: off, 30, 60, 120 seconds or no limit; 60 by default): a player whose health runs out lies down and can be
  helped up by the partner before the time runs out; when both are down, it is game over for both.
- A lobby, opened by the "Co-Op Play" row of the main menu (or F6, or the BACK button of a gamepad), sets: co-op on / off, which character player 1 plays, the layout of the
  screen, which gamepad is player 1's, and the network: "Host" (you start the game, the other player joins) or "Join", the connection ("Steam friends" or "Direct IP") and "Start".
  The lobby is in the game's 13 languages. The story is started from the game's own menus as always; a co-op game keeps its own save (see "Saves").
- When the host leaves or the connection is lost, the guest reads it on screen and goes back to the title; when the guest leaves, the host plays on with the game's AI partner.
  Both PCs need the same version of the mod: another version is refused and the lobby says so.
- Raid mode and the game's own online play are never touched by this mod.

Install
1. Run "Install Co-op.bat" (or the setup .exe). The window finds your Steam copy; otherwise choose the folder that contains rerev.exe. For network play do it on BOTH PCs.
2. The installer copies dinput8.dll and a coop folder into the game folder, keeps a dinput8.dll of someone else's in coop-backup, and makes the
   "Resident Evil Revelations Co-op" shortcut (desktop, Start menu). Start the game with that shortcut (it starts it through Steam): it also installs a newer version of the
   mod from GitHub first, if there is one (no internet: the game starts as it is). A new version is installed only when its download matches the checksum GitHub gives for it.
3. In the main menu choose "Co-Op Play" (the fourth entry) to open the co-op lobby; F6 (keyboard) or BACK (gamepad) open it too.
If you had a pre-release (0.0.1 or 0.0.2): the shortcut updates it to this version by itself at the next start.

Network play: Steam friends or IP address
- "Steam friends": the host starts hosting, the friend joins from the Steam friends list ("Join Game") or from the host's invite. No address and no port forwarding are needed.
- "Direct IP": the host's game listens on UDP port 28410. On the same home network nothing else is needed; over the internet the host forwards UDP 28410 to the PC on the router.
  Windows Firewall asks the first time the game listens: allow Resident Evil Revelations (on a home network: "Private networks"; over the internet also "Public"). The player who
  joins needs no open port: they type the host's address in the lobby (the "Host address" row). The host sees the own address in the "Your address" row.
- Both PCs need the same version of the mod (the shortcut updates it at every start) and the same Steam build of the game.

Saves
- A co-op game has its OWN save, separate from your solo campaign: a co-op game, on two PCs or in split screen on one PC, never writes your normal save
  (DATA0.DAT in Steam Cloud). Each PC keeps its co-op save in %LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS\rev1_coop_save\COOP0.DAT (a local file, not in
  Steam Cloud; the first co-op checkpoint starts it from a copy of your solo save's settings and unlocks) and next to it COOP0.INF (which character this PC played).
- While co-op is on (a network session, or split screen switched on in the lobby) the title menu's Continue continues the co-op save; without a co-op save it shows
  New Game only. With co-op off your solo Continue loads your solo progress as before.
- In a network game Continue keeps the characters the co-op save was played with: if the room gave the host the other character, the host's Continue puts the
  save's characters back on both PCs and both screens say so. New Game keeps the characters chosen in the room. The guest's game follows the host's checkpoint
  and takes its own weapons from its own co-op save.
- To start the co-op campaign over, delete the rev1_coop_save folder (with the game closed).

Achievements
- Steam achievements are not blocked in your game: what you earn in a co-op game is really unlocked on your Steam account. Each PC credits only what its own player did
  (the partner's kills, pickups and actions count on the partner's PC, not on yours).

Remove: run "Uninstall Co-op.bat" in the game folder (or the installer again and choose "Remove co-op"). It deletes what it installed (also the mod's log files next to rerev.exe),
puts the backed-up files back and removes the shortcuts. Your saves are not touched (the co-op save folder rev1_coop_save stays too); the lobby's settings file stays in %LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS
(rev1_coop.ini, a few lines); screenshots you took with the mod stay in rev1_coop\shots.

Needs: the Steam version of Resident Evil Revelations, build 13298487 (the installer checks rerev.exe and refuses any other one), Windows 10 or 11, PowerShell
5.1 (part of Windows), two gamepads for two players on one PC. The mod changes no game file other than adding dinput8.dll; it does not touch the Steam stub or Raid.

Limits
- Network play is for Episode 1 with one person per PC (the lobby offers it for Episode 1 only); split screen on one PC works in the whole campaign.
- Joining a running network game is not possible: if one player's game closes or the connection is lost, start a new co-op game from the lobby (Continue picks up the checkpoint).
- While co-op is on, the Campaign page's save panel (play time, chapter) can still show your solo save's values; Continue loads the co-op save.
- In a network game the partner's character on your screen can hold other guns than the partner really carries (the DLC pistols are never shown on it); what the partner
  shoots and hits is always done with the partner's own weapon on the partner's PC.
- Other mods that use dinput8.dll (ASI loaders, fixes): the installer keeps your file in coop-backup and puts the mod's dinput8.dll in its place. If your file is an Ultimate ASI
  Loader (the loader of most ASI mods), the installer also puts a copy of it in the game folder as winmm.dll, so the plugins in your scripts folder keep working. Any other
  dinput8.dll is off until you remove the co-op mod, which puts your files back as they were. Do not rename an ASI loader to dinput8_real.dll: the mod does not load a loader
  under that name. A dinput8.dll another mod puts over the co-op one later is kept too: the next install or update of the co-op mod keeps it as your original, removing the co-op
  mod leaves it in place. Other DLLs such mods use (d3d9.dll, dsound.dll, version.dll, xinput1_3.dll) are not touched.
- The installer and the setup .exe are not code-signed: Windows SmartScreen may warn. The zip works the same way.
- Gamepad support in the lobby and in the game uses XInput (Xbox-compatible pads).
- Pad numbering gaps (a pad on XInput user 2 only), the exchange "player 1 uses gamepad 2" and rumble follow one rule; a gamepad that Windows shows twice (a Steam Input or
  remapper copy next to the physical pad) is recognised after a few seconds of play and counted as one gamepad. If two real pads are ever taken for one, start the game with the
  variable REV1_PADDEDUP=0.

Reporting a bug (please attach)
- The zip that coop\Collect logs.bat in the game folder puts on your desktop (double-click it right after the problem, before you start the game again; the game keeps only the
  last five starts' logs). It holds rev1_coop.log (next to rerev.exe; the last runs are rev1_coop.1.log ... rev1_coop.4.log; a crash is written there with the line
  "AccessViolation"), rev1_coop.ini and the game's config.ini from %LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS (your lobby and display settings), the install record and
  logs, the mod's version, hashes of rerev.exe and of the mod's DLL, Windows, graphics card and driver, and the names (not the contents) of the files in the mod's folder. It
  removes your Windows user name, computer name, folder paths, player names, Steam names and IDs, e-mail and MAC addresses and public IP addresses, and uploads nothing; open
  the zip to check it before you attach it.
- For a network game: the zip of both PCs.
- A short description: which part of the story, which gamepads (Xbox / other / Steam Input), what you pressed.
- Issues: https://github.com/dolbaras/revelations-coop/issues

Licence: MIT (LICENSE.txt). Not affiliated with Capcom. No game files are shipped.
