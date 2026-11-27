Resident Evil Revelations Co-op - version @VERSION@
================================================================

Two players in the story campaign of Resident Evil Revelations (2013 PC, Steam build 13298487; an older game build and a third build are supported too, see "Needs"): on one PC with split screen and two gamepads, or on two PCs over the network.

What it does
- The campaign's AI partner (Parker, Jessica, Chris, Keith, Quint depending on the part of the story) is taken over by a second player. Each player has an own camera, an own HUD
  with the own ammunition and weapon, and an own pause / item menu.
- One PC, split screen: the second player uses a second gamepad, or: player 1 plays with the keyboard and the mouse and player 2 with ONE gamepad (the lobby's "Gamepads" row, third
  value). The screen is split side by side or top and bottom (your choice); the HUD (minimap, ammunition, weapon box, text) keeps its proportions in each half, drawn in a centred 16:9 box (the same at 1920x1080). The game's item screen (BACK) of a player opens in his own half. Cutscenes, the pause menu and the game over
  screen use the whole screen. Split screen works in every part of the story with two playable characters.
- Network, two PCs: each player has the game on the own PC (both need the Steam copy and this mod) and plays the own character on the own screen. One person per PC; split screen
  and network are not used together. Network play follows the story section by section (every section with two playable characters, whichever pair the story gives: Jill and Parker, Chris and Jessica, Keith and Quint ...; arrival, characters and movement of each section were checked on two PCs, what was not is listed under "Limits"): the guest's game follows
  the host's New Game or Continue, both PCs wait for each other at every load and at the end of each cutscene, the sections and the episodes follow each other on both PCs (also from Episode 1 into Episode 2), and at the end of the
  campaign both PCs show the game's own end and each PC closes the co-op session after it. Enemies are run by the host's PC; doors, key items and item pickups are kept in step; a game over is shared and Continue takes both players to the same checkpoint.
  In one part of Episode 2 (the Guest Cabin) the story locks the partner away: there the second player plays HUNK, a character of the game, instead (over the network and in split screen).
- What a PC shows of the partner in a network game: his character walks, aims, shoots and changes weapons as he does, and plays his door-opening and pickup motions (his
  motions at levers, valve wheels, shelves and weapon boxes are passed the same way). A lever he pulls moves on your PC too. A locker, corpse box or safe he opens is shown
  opening on your screen (for you it stays closed with your own loot inside: open it yourself). The key items of every episode and the story flags that the game's objects
  set are passed to the other PC. Movies (the pre-rendered cutscenes) start together on both PCs ("Waiting for ... to finish loading..." while one PC waits for the other),
  and a skip needs both players: the first skip press shows "... wants to skip the cutscene" on the partner's screen, and when the partner skips too, the movie ends on both PCs.
- Downed / revive, in a network game and in split screen (a row of the lobby: off, 30, 60, 120 seconds or no limit; 60 by default and ON by default; in a network game the host's
  choice): a player whose health runs out lies down and can be helped up by the partner (stand next to him, hold A) before the time runs out; when both are down, it is game over for
  both. A line on the screen tells the player who lies "You are down. Your partner can help you up." and the other one "Your partner is down! Stand next to them and hold A to help.",
  with the time left (13 languages).
- Partner gate (split screen and network, local gamepads): when the two players get far apart (about 25 m) a line asks the one who runs away to wait for the partner; from about
  40 m the left stick of the one who walks AWAY is damped (never to zero, never a teleport or a kill; walking back or standing is not damped). In split screen, before a cutscene, a movie,
  a section change or a restart, a second player who is far away is put next to the first player. It can be switched off (rev1_coop.ini: leash=0).
- Split screen, the second player cannot fall out of the world: when his half has no ground (the game streams the level around the first player only) or he is far below the first
  player, he is put next to the first player after about two seconds.
- Split screen, the second player examines and uses things himself: the action prompts of objects (for example the drain cover in the galley of Episode 1) show in the half of the player
  who can use them, and he presses his own button; the first player is no longer forced to examine.
- Split screen: the co-op save is kept on while the title's Continue or New Game loads (the first Continue after starting the game could load the solo save's area).
- Split screen: the second HUD set is taken down before a section change or a restart (players reported a crash after the first B.O.W. kill and at the end of a section: the same cause;
  reproduced and gone in our restart test, see Limits). A short scripted scene that repeats (the ship's shake) or a short door animation should no longer take the whole screen (checked by
  offline tests only: we could not make the loop happen in the lab).
- Give to partner (as in Resident Evil 5): in the game's own item screen (BACK on a gamepad) a herb, a grenade or ammunition can be passed to the partner while the
  partner stands within about 3 m and is not down. The screen shows "Give to partner" on such an item; one herb or one grenade per press, half of that ammunition (rounded
  up; press again for more). The partner takes what fits; if the partner cannot hold any more, the item stays with you and the screen says so. On one PC and over the network,
  with a gamepad, the keyboard or the mouse.
- The Genesis scanner for both players: as soon as one player has the Genesis, the other gets one too (the story gives it to one character only). Over the network each
  player scans on the own PC, and an item a scan reveals shows on both PCs. In split screen the second player's scan opens in his own half (the other half stays as it is).
  Each player has an own analysis rate and own herb rewards; in part 2 of Episode 1 (the scanner tutorial) the rate is the team's, as the story counts it.
- A lobby, opened by the "Co-Op Play" row of the main menu (or F6, or the BACK button of a gamepad), sets: co-op on / off, which character player 1 plays, the layout of the
  screen, which gamepad is player 1's (or "keyboard + mouse and one gamepad"), the Downed / revive rule, and the network: "Host" (you start the game, the other player joins) or "Join", the connection ("Steam friends" or "Direct IP") and "Start".
  The lobby is in the game's 13 languages.
  While co-op is on, the game's PAUSE menu has a "Co-Op Play" row (it takes the place of "Restart", which moves into the page): a page in the game's own options style (second
  player on / off, screen layout top and bottom / side by side, input device: 2 gamepads / gamepads swapped / keyboard + gamepad, the Downed / revive rule, Restart, in a network
  game the host's rules, leave / end co-op) and the mod's name and version. The lobby (the title menu's row, F6, BACK) is as before. The story is started from the game's own menus as always; a co-op game keeps its own save (see "Saves").
- When the host leaves or the connection is lost, the guest reads it on screen and goes back to the title; when the guest leaves, the host plays on with the game's AI partner.
  Both PCs need the same version of the mod: another version is refused and the lobby says so.
- Raid mode and the game's own online play are never touched by this mod.

New in 0.2.7 (the details and what was checked are in the release notes on GitHub)
- Split screen with the keyboard and the mouse for player 1 and ONE gamepad for player 2: the gamepad no longer drives player 1 as well (a stale pad assignment, for example after
  the lobby's "gamepads swapped", made the pad move the keyboard's player); the second player's stick has the gamepad's normal aim speed (it ran several times too fast next to a
  mouse player) and follows the Options sensitivity; the second player's ammunition is used up like the first player's (his reserve never dropped before).
- The second player in Episode 1: when the story locks him in place while it "waits" (the galley sink part of section 1-3: "he starts a scan animation and cannot move") he is released;
  the story character who stands at the Genesis hand-out of 1-2 now blocks him as well as the first player (he walked through); at the start of Episode 1 (the dark opening corridor)
  his half is lit by his own torch beam. At the start of chapter 2 (the B.O.W.s) he can go down to the beach himself (when he jumps first the screen stays split, no whole-screen top-down view), and a human who was left behind is put next to the other one only after 45 seconds.
- The second player's rifle scope (Jessica's sniper rifle in the snow chapter): the scope picture, zoom and hidden character show in his half; the first player's aim is not disturbed.
- Split screen at 1920x1080 and above: the HUD, the scanner overlay and the item screen are no longer stretched; the item screen and the second player's Genesis scanner open in the
  half of the player who uses them; a prompt of the second player (a ledge, a jump down) shows in his half only.
- Documents (the Genesis manual and other pages the game shows) and the close examination of an object (a crate, a story scene that zooms on something) now appear in the half of the player
  who opens them, with that player's gamepad turning the pages; the other half goes on as it is (before, a document was always in the first player's half and the examine camera and text covered both
  halves). The second player's Genesis scanner is steadier when it is used again and again (the exchange no longer flaps at its start and the camera no longer points at the other player for a
  moment).
- A Co-Op page in the pause menu in the game's own options style (see above), both screen layouts selectable there, and the mod's name, version and GitHub address on the title screen
  and on that page.

Install
1. Run "Install Co-op.bat" (or the setup .exe). The window finds your Steam copy; otherwise choose the folder that contains rerev.exe. For network play do it on BOTH PCs.
2. The installer copies dinput8.dll and a coop folder into the game folder, keeps a dinput8.dll of someone else's in coop-backup, and makes the
   "Resident Evil Revelations Co-op" shortcut (desktop, Start menu). Start the game with that shortcut (it starts it through Steam): it also installs a newer version of the
   mod from GitHub first, if there is one (no internet: the game starts as it is). A new version is installed only when its download matches the checksum GitHub gives for it.
3. In the main menu choose "Co-Op Play" (the fourth entry) to open the co-op lobby; F6 (keyboard) or BACK (gamepad) open it too.
If you had an older version (0.0.1, 0.0.2, 0.1.0, 0.2.0, 0.2.1, 0.2.2, 0.2.3, 0.2.4, 0.2.5 or 0.2.6): the shortcut updates it to this version by itself at the next start. Different versions do not play together (for example 0.2.6 and 0.2.7): the lobby refuses the other version and says so.

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
  and takes its own weapons from its own co-op save. At the first level after New Game or Continue, a guest's character that has no gun or no knife there
  starts with what the host's game gives that character: its guns, the knife, spare ammunition and herbs.
- To start the co-op campaign over, delete the rev1_coop_save folder (with the game closed).

Achievements
- Steam achievements are not blocked in your game: what you earn in a co-op game is really unlocked on your Steam account. Each PC credits only what its own player did
  (the partner's kills, pickups and actions count on the partner's PC, not on yours).

Remove: run "Uninstall Co-op.bat" in the game folder (or the installer again and choose "Remove co-op"). It deletes what it installed (also the mod's log files next to rerev.exe),
puts the backed-up files back and removes the shortcuts. Your saves are not touched (the co-op save folder rev1_coop_save stays too); the lobby's settings file stays in %LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS
(rev1_coop.ini, a few lines); screenshots you took with the mod stay in rev1_coop\shots.

Needs: Resident Evil Revelations for PC: tested and supported is the Steam build 13298487; also made for an older build (PE timestamp 2019-02-23, a copy with the Steam wrapper
removed, rerev.exe SHA-256 BA1F0168...4F88), for which the installer puts a DLL made for it; and a third build (rerev.exe SHA-256 B90005AF...58EA, seen in a player's logs) that has the
Steam build's code layout (the installer puts the same DLL as for the Steam build; every patch checks the game's bytes and stays off on a mismatch). A licensed copy is recommended; on other versions stable work is not guaranteed.
Any other rerev.exe: the installer shows its SHA-256 and size and asks whether to install anyway on an untested version (see "Other game versions" under Limits). Windows 10 or 11, PowerShell
5.1 (part of Windows), two gamepads (or a keyboard and a mouse plus one gamepad) for two players on one PC. The mod changes no game file other than adding dinput8.dll; it does not touch the Steam stub, steam_api.dll, any Steam emulator's files or Raid, and it never works around a copy protection. If Steam updates the game to a build the mod does not know, the mod does not recognise that rerev.exe (co-op stays off, the game runs as before) until a mod version for that build exists; the installer then asks again before it installs.

Limits
- Other game versions (0.2.7): A licensed copy is recommended; on other versions stable work is not guaranteed. The third build (B90005AF...58EA) was NOT available to us: it is accepted because a player's log showed the same addresses as the Steam build, not because we ran it; its crashes in the log (the end of a section, a stray address when hosting through Steam) were not reproduced on it. The installer recognises the Steam build 13298487 and the older build
  (2019-02-23, unwrapped) by the SHA-256 of rerev.exe and installs the DLL made for each. The older build's DLL is the same mod with the game's addresses moved to that build (every patch checks the game's
  code bytes first and switches itself off, with a line in rev1_coop.log, when they are not the expected ones). What was run on the older build and what was not is in the release notes
  ("Checked" and "Not verified"); the lobby's Steam friends and invitations are not available on a copy that has a Steam emulator instead of Steam: use "Direct IP" (UDP 28410). The older build
  is not large-address-aware (2 GB): long split-screen sessions are not verified. On the older build the second player's shots in split screen follow his own view and damaged and killed an enemy in our last test
  (one earlier test run of the same kind, with too few lucky hits, showed no damage: the test aims by steering and hits only when the line is within about 20 units of the enemy; it is only a few minutes of play, not a long fight). A network game between the two builds
  (Steam build hosts, older build joins by IP) was checked for walking, aiming, the reload, the knife and the weapon change of a keyboard / mouse guest, and the guest's aimed shots on the older build damaged an enemy there. For any other rerev.exe the installer asks before it installs (the answer is kept in coop\anyexe.txt for that
  exe only, and "Remove co-op" deletes it); the mod then runs with the Steam build's DLL and every patch checks the game's bytes; whatever does not match stays off. A network game needs the
  same mod version on both PCs; the supported and the older build can join each other (they send the same build tag) - see the notes for what was verified.
- The older game build (2019-02-23) and 0.2.7: its DLL is made from the same sources with the new addresses of this version found by comparing the two builds' code; the additions of 0.2.6 (own prompts, gate, fall rescue, HUD fit, Downed / revive in split screen, keyboard + one gamepad) and of 0.2.7 (the pause menu page, the title label, the second player's scope, torch, collision, descent and unlock, the HUD at 1080p, the item screen and scanner in the player's half) were NOT run on that build. Every patch checks the game's bytes first and stays off on a mismatch.
- Split screen flashlight of the second player: in dark zones where the game switches its own torch on (for example the start of Episode 1) the second player's half now has its own beam from his own character, lit by the second player's view, and the first player's beam stays in the first half. Checked in the lab on the dark start of Episode 1 at 1280x720, top/bottom and side by side (pictures and half brightness with the beam on / off / moved away; the glow around the first player's body and other zones with the torch were not examined; REV1_FLASH2=0 switches the second beam off). The torch is automatic in the game (no button), so there is nothing to toggle.
- Split screen, story characters: at the Genesis hand-out of Episode 1-2 the story sets a "not pushed" collision for the partner (it is meant for the computer partner), so the second player walked through the character who stands there while the first player was blocked. The second player now gets the first player's collision values (every frame, not while either is dead), so a story character blocks both. Checked in the lab (a human put inside that character's body: pushed out with the fix, left inside without it) with the values the story sets, side by side and top/bottom; the real hand-out scene itself was not played, the values were set the way the story sets them. REV1 console `cap collmirror 0` is the stock behaviour for tests.
- Partner gate: the distances (25 m hint, 40 m damping) are a first choice. Keyboard and mouse players get the hint only (the game fills their input itself). In a network game the remote partner is damped by his own PC only, and nobody is put next to the other. The line, the damping (about a quarter of the speed when walking away, no change when walking towards the partner) and the move before a scene were checked in the lab with two virtual gamepads, top/bottom and side by side; a section change in real play was not driven to the end, and no real pair of players has used it yet.
- Split screen HUD fit: checked in both layouts and both halves at 1280x720 (the screen of our test PC) and, for the HUD, the item screen and the scanner, at 1920x1080; 2560x1440, ultrawide and other shapes were not seen; the HUD is smaller than the half's corners on purpose. The text of item messages, help and subtitles was not seen in the split (it uses the same fit).
- Split screen, own prompts: the second player's own examine prompt and press were proven for the drain cover of Episode 1 only (the fix is general for story prompts of that kind; in the final 0.2.6 build the lab route that reaches that spot loads the checkpoint through the new Continue rule and then neither player got the prompt, so the proof was repeated with that rule switched off in the lab: the scene started by his press) the galley prompts "10b" / "10c", other parts of the story, a padlock door and the "missing opening events" a player reported (Episode 1) were not examined. The crash at the end of a section (second HUD set) was reproduced with a restart in the lab and is not seen after the change; the real end of a section during play, a "New Game" crash after "Saving game" and a crash at a stray address when hosting through Steam were not reproduced.
- Not verified in 0.2.7, hardware: every split-screen test of this version used virtual gamepads (ViGEm) and keys injected into the game, not a real gamepad and a real keyboard / mouse: the
  fix of "the gamepad drives player 1 too", the aim speed of the second player's stick, the pause page's mouse and the keyboard path were not tried by a person. Resolutions: 1280x720 and,
  for the HUD fit, the item screen, the scanner and the pause page, 1920x1080; the torch, the descent, the collision and the scope were seen at 1280x720 only.
- Not verified in 0.2.7, network: the network play of this version was not run on two PCs (the network code changed in two places only: the host's Downed / revive rule from the pause
  page reaches the guest, and the session record can change while playing; the pause page's host / guest rows - kick, end co-op, leave - were run as model tests, not live).
- Not verified in 0.2.7, documents, examine and the scanner: the document in the second player's half was proven with the exchange forced by a console command (the automatic start by a real pickup of a
  document by the second player was not reached in the lab); the examine camera was proven in the examiner's half in both layouts, but the examine TEXT only side by side (not top/bottom), and the
  first player's own press of the examine prompt did not start it in the lab (it was started with the second player's pad and a forced camera); the quick repeated use of the second player's scanner
  (a tester's report: his half without the scope picture, the camera at the chest) was NOT seen in the lab even in 128 presses: two defects of the exchange that could cause it were found and fixed;
  the first player's mouse freezing while the second player scans (a tester's report) could not be reproduced (the lab's mouse does not turn the camera at all); the black bars of the scanner view
  are the game's own look (the first player's scan has them too).
- Not verified in 0.2.7, the story: the second player's release in the galley part of section 1-3 was proven in a forced lab route (the story's flags set by the game's own call; the
  second player walked 25 units before and 219 units after), not in the player's exact spot (the scene "13_050_0" with Chris tied to a chair) and not by a real play-through; the repeat
  guard for a short scene that comes back (it no longer takes the whole screen a third time within 90 seconds) was tested offline only. The collision of the story character at the Genesis hand-out
  of 1-2 was proven with the values the story sets, put in by us, not by playing the hand-out scene. The torch: only the dark start of Episode 1 was searched for a zone where the game
  switches its torch on (the other section starts are lit); the glow around the first player's body is not copied, and the partner's beam is not drawn on the other PC. The second player's
  rifle scope: the PSG1 of section 2-1 (the other rifles use the same code); the scope while a pause menu, a scanner or a cutscene starts at the same moment was not provoked.
- Not covered in 0.2.7: the tilt of the second player's head while he shoots (a tester's report, cosmetic, not looked at); a tester's online problems with a game copy that has a Steam emulator
  (hosting by IP or the emulator's online, the invitation closing the game) and with Radmin VPN need his logs (the zip of "Collect logs.bat"): please send them, with the game build.
- Network play is for one person per PC. In the Guest Cabin part of Episode 2 the second player plays HUNK instead of the partner the story locks away: HUNK appears, walks and shoots there (on two PCs and in split screen) and the end of the part leads both PCs into Episode 3, but the part's own story (its scenes, doors and enemies) was not played through with HUNK; Split screen on one PC works in every part of the story with two playable characters.
- Not verified with two real PCs in this version: the last boss fight of the campaign played through (its end flow after it is verified), and the section 12-3, which could only be started directly (not by the story's own way); it follows the same rules.
- Not verified with two real PCs, the network additions of 0.2.2: a lever pulled by the partner moving on the other PC (the parts of the levels with levers load only by the
  story, which our tests could not drive there; checked by the offline tests only), using a key on the host's PC that the guest picked up (the key's story flag reaching the
  host was checked, in Episode 12), a weapon box shown opening on the other PC (our test could not open the box), the on-screen line after a lone skip press (the vote itself
  was checked), a skip pressed on a gamepad (our tests pressed keys of the keyboard; the game's own skip test reads both), and the partner's motions at levers, valve wheels,
  shelves and weapon boxes (his door and pickup motions were checked).
- Genesis in split screen: the second player's scanner opens in his own half and the other half stays normal (checked at 1280x720 and 1920x1080, both layouts); the game has one scanner view, so one player scans at a time: the first to start, the other waits; his analysis rate is kept only while the game runs (it starts at 0 again after the game is restarted). A real scan of a target by him in split screen was not reached.
- Give to partner: an item that is on its way to the partner at the very moment a new section starts is not in that section's first checkpoint, so loading that checkpoint
  again (a game over before the next one) takes it back, like everything else since. Where the story changes the characters with the section (Episode 9 to 10), an item
  still on its way lands with the new character on that PC; it is not lost.
- Split screen, doors and items: a door or an item is used by the player who stands nearest to it (the other player cannot use it while he is not the nearer one; a door that is opening stays with
  the player who started it). The prompt of a door or an item (the green button) is drawn in both halves of the screen wherever the door or item is visible, also for the first player.
  A door needs its opening animation loaded, which the game does around the first player: if the second player opens a door that is far from the first player, the door closes again after
  6 seconds and the second player is free; try again when the first player is nearer (in our measurements a door was ready when the first player stood about 200 units or nearer for a few
  seconds, and then stayed ready).
- Split screen, other things the action button does (doors other than the two we tried (Episodes 3 and 9), crates, wall scanners and hand prints, elevator switches, weapon boxes, ladders): they use the same rule as the doors (the nearest player, his own
  action button), but they were not seen working for the second player in our tests (the crate did not answer the first player either; the others could not be reached by the test controls); if one of them
  does not work for the second player, please report it.
- Split screen and cutscenes: a movie (the pre-rendered cutscenes) uses the whole screen, also a movie that starts in the middle of play; an in-engine cutscene
  uses the whole screen while the game drives the camera for it. The game's short scripted moments during play (lines of dialogue, a character who waits, a shake of
  the camera) keep the split screen. The short loading screen before a movie in the middle of play (a few seconds) is shown in the first player's half.
  A tutorial page of the game (for example the Genesis manual at the start of Episode 1-2) is shown in the first player's half; the first player turns its pages.
- Network play with the keyboard and the mouse: the partner sees your character walk, aim, shoot, reload, swing the knife and change the weapon as motions. Turning on the spot with
  the mouse while aiming and the quick turn are not shown as motions (his character turns only when it walks).
- Joining a running network game is not possible: if one player's game closes or the connection is lost, start a new co-op game from the lobby (Continue picks up the checkpoint).
- At the first level of a network session the host's character on the guest's screen carries no knife, so a knife attack of the host is not shown there (it counts on the host's PC as usual).
- Network, the world objects the partner uses: valve wheels (Episodes 5, 6 and 12) and the shelves you push (Episode 2) move only on the PC of the player who uses them (his
  motion is shown on his character and the story flag is passed on: the game moves the player who turns or pushes, so the other PC cannot replay it); each player rides an
  elevator himself (a ride loads areas); ducts, vent covers, wire-puzzle covers, sideboards, TVs, toilets, shutters and two-wing doors are not shown on the other PC; a crate
  breaks on each PC by the hits that PC sees (not checked). A locker, corpse box, safe or weapon box the partner opened looks open on your screen but is still closed for you
  (your own loot, as before). On ladders, climbs and jumps the partner's character is moved along by the position correction, without the climbing motion.
- Network, in-engine cutscenes (the ones that are not movies): each PC starts them by its own story and both PCs leave each scene together (the end is waited for); the game
  has no skip for them, and nothing corrects a drift inside a scene. After a movie both PCs start the next scene within a few hundredths of a second; where a section begins
  without a movie, its first scene can start about a second apart on the two PCs (a few seconds in the worst cases we measured).
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
