Resident Evil Revelations Co-op - version @VERSION@
================================================================

Two players in the story campaign of Resident Evil Revelations (2013 PC, Steam build 13298487): on one PC with split screen and two gamepads, or on two PCs over the network.

What it does
- The campaign's AI partner (Parker, Jessica, Chris, Keith, Quint depending on the part of the story) is taken over by a second player. Each player has an own camera, an own HUD
  with the own ammunition and weapon, and an own pause / item menu.
- One PC, split screen: the second player uses a second gamepad. The screen is split side by side or top and bottom (your choice). Cutscenes, the pause menu and the game over
  screen use the whole screen. Split screen works in every part of the story with two playable characters.
- Network, two PCs: each player has the game on the own PC (both need the Steam copy and this mod) and plays the own character on the own screen. One person per PC; split screen
  and network are not used together. Network play follows the story section by section (every section with two playable characters, whichever pair the story gives: Jill and Parker, Chris and Jessica, Keith and Quint ...; arrival, characters and movement of each section were checked on two PCs, what was not is listed under "Limits"): the guest's game follows
  the host's New Game or Continue, both PCs wait for each other at every load and at the end of each cutscene, the sections and the episodes follow each other on both PCs (also from Episode 1 into Episode 2), and at the end of the
  campaign both PCs show the game's own end and each PC closes the co-op session after it. Enemies are run by the host's PC; doors, key items and item pickups are kept in step; a game over is shared and Continue takes both players to the same checkpoint.
  In one part of Episode 2 (the Guest Cabin) the story locks the partner away: there the second player plays HUNK, a character of the game, instead (over the network and in split screen).
- Downed / revive in a network game (the host's choice in the lobby: off, 30, 60, 120 seconds or no limit; 60 by default): a player whose health runs out lies down and can be
  helped up by the partner before the time runs out; when both are down, it is game over for both.
- Give to partner (as in Resident Evil 5): in the game's own item screen (BACK on a gamepad) a herb, a grenade or ammunition can be passed to the partner while the
  partner stands within about 3 m and is not down. The screen shows "Give to partner" on such an item; one herb or one grenade per press, half of that ammunition (rounded
  up; press again for more). The partner takes what fits; if the partner cannot hold any more, the item stays with you and the screen says so. On one PC and over the network,
  with a gamepad, the keyboard or the mouse.
- The Genesis scanner for both players: as soon as one player has the Genesis, the other gets one too (the story gives it to one character only). Over the network each
  player scans on the own PC, and an item a scan reveals shows on both PCs. In split screen the second player's scan uses the whole screen while he holds the scanner.
  Each player has an own analysis rate and own herb rewards; in part 2 of Episode 1 (the scanner tutorial) the rate is the team's, as the story counts it.
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
If you had an older version (0.0.1, 0.0.2 or 0.1.0): the shortcut updates it to this version by itself at the next start. A 0.1.0 game and a 0.2.0 game do not play together: the lobby refuses the other version and says so.

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
- Network play is for one person per PC. In the Guest Cabin part of Episode 2 the second player plays HUNK instead of the partner the story locks away: HUNK appears, walks and shoots there (on two PCs and in split screen) and the end of the part leads both PCs into Episode 3, but the part's own story (its scenes, doors and enemies) was not played through with HUNK; Split screen on one PC works in every part of the story with two playable characters.
- Not verified with two real PCs in this version: the last boss fight of the campaign played through (its end flow after it is verified), and the section 12-3, which could only be started directly (not by the story's own way); it follows the same rules.
- Genesis in split screen: while the second player scans, the screen shows his scanner view in full (the game has one scanner view, so one player scans at a time: the first to start, the other waits); his analysis rate is kept only while the game runs (it starts at 0 again after the game is restarted).
- Give to partner: an item that is on its way to the partner at the very moment a new section starts is not in that section's first checkpoint, so loading that checkpoint
  again (a game over before the next one) takes it back, like everything else since. Where the story changes the characters with the section (Episode 9 to 10), an item
  still on its way lands with the new character on that PC; it is not lost.
- Joining a running network game is not possible: if one player's game closes or the connection is lost, start a new co-op game from the lobby (Continue picks up the checkpoint).
- An enemy that chases the guest into another area is driven by the guest's PC. The host sees it asleep there, and if the host comes back before it dies,
  the guest's damage to it is lost (known since 0.1.0).
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

License: MIT (LICENSE.txt). ENet 1.3.18 (MIT, ENET-LICENSE.txt). Not affiliated with Capcom. No game files are shipped.
