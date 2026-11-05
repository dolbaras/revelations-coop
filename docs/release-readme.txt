Resident Evil Revelations Co-op
================================

Two players on one PC, split screen, for the story campaign of Resident Evil: Revelations (2013 PC, Steam build 13298487).

What it does
- The campaign's AI partner (Jessica, Parker, Jill, Chris, Keith, Quint depending on the part of the story) is taken over by a second player with a second
  gamepad. Each player has an own camera, an own HUD with the own ammunition and weapon, and an own pause / item menu.
- The screen is split side by side or top and bottom (your choice). Cutscenes, the pause menu and the game over screen use the whole screen.
- A lobby, opened by the new "Co-Op Play" row of the main menu (or F6, or the BACK button of a gamepad), sets: co-op on / off, which character player 1 plays, the layout of the screen, which gamepad is
  player 1's. The story is then started with Continue / Load Game / New Game as always; saves, checkpoints and achievements work as in the stock game.
  The lobby is in the game's 13 languages.
- Raid mode and online play are never touched by this mod.

Install
1. Run "Install Co-op.bat" (or the setup .exe). The window finds your Steam copy; otherwise choose the folder that contains rerev.exe.
2. The installer copies dinput8.dll and a coop folder into the game folder, keeps a dinput8.dll of someone else's in coop-backup, and makes the
   "Resident Evil Revelations Co-op" shortcut (desktop, Start menu). Start the game with that shortcut: it also installs a newer version of the mod from GitHub
   first, if there is one (no internet: the game starts as it is).
3. In the main menu choose "Co-Op Play" (the fourth entry) to open the co-op lobby; F6 (keyboard) or BACK (gamepad) open it too.

Remove: run "Uninstall Co-op.bat" in the game folder (or the installer again and choose "Remove co-op"). It deletes what it installed, puts the backed-up files back
and removes the shortcuts. Your saves are not touched; the lobby's settings file stays in %LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS (rev1_coop.ini, a few lines).

Needs: the Steam version of Resident Evil Revelations, build 13298487 (the installer checks rerev.exe and refuses any other one), Windows 10 or 11, PowerShell
5.1 (part of Windows), two gamepads for two players. The mod changes no game file other than adding dinput8.dll; it does not touch the Steam stub or Raid.

Limits (honest)
- The installer and the setup .exe are not code-signed: Windows SmartScreen may warn. The zip works the same way.
- Gamepad support in the lobby and in the game uses XInput (Xbox-compatible pads).
- Pad numbering gaps (a pad on XInput user 2 only), the exchange "player 1 uses gamepad 2" and rumble follow one rule; a gamepad that Windows shows twice (a Steam Input or remapper copy next to the physical pad) is recognised after a few seconds of play and counted as one gamepad. If two real pads are ever taken for one, start the game with the variable REV1_PADDEDUP=0.
- Network co-op is not part of this version.

Licence: MIT (LICENSE.txt). Not affiliated with Capcom.
