# Releasing

Players start the game with the "Resident Evil Revelations Co-op" shortcut, which runs `coop\coop-launcher.ps1` (`tools/launcher.ps1`). Before the game starts it reads
`https://api.github.com/repos/dolbaras/revelations-coop/releases/latest` (the repository name is a constant at the top of `tools/launcher.ps1` and in its URL check;
the owner creates the repository) and installs that release when its tag is newer than `coop\version.txt`. Publishing a release therefore updates every player who uses
the shortcut the next time they start the game, so a release must be finished and tested before it goes up. Nothing is published from this workspace without the owner.

What the launcher already out there relies on, so every later release keeps it (same protocol as the Jericho and TimeShift mods of this workspace):

- **Tag** `v<major>.<minor>.<patch>` (parsed as a .NET `[version]`), a full release: drafts and pre-releases are not "latest" and are never installed.
- **Asset** named `revelations-coop-<version>.zip` (the first asset matching `revelations-coop-*.zip`), attached to that release, so its URL is under
  `https://github.com/dolbaras/revelations-coop/releases/download/`. The launcher checks the download against the `sha256:` digest GitHub gives for the asset.
- **`coop-installer.ps1` and `coop-texts.ps1` at the root of the zip** (as `tools/package.ps1` builds them), the installer taking
  `-Game <folder> -AutoClose -NoShortcut`: install into that folder with the window, close it by itself after a successful install, exit code 0 on success and
  non-zero on failure; `-NoShortcut` refreshes only the shortcuts the install record lists.
- **`files\coop\version.txt`** holding the version without the `v`.
- **`<game>\coop\install.txt`**, the install record the next installer reads: `version=`, then one line per `file=`, `backup=` and `shortcut=` (several shortcuts on one
  line are split after each `.lnk`, empty ones dropped).
- The mod's DLL carries the words `RE Revelations co-op proxy` (the installer tells it from a dinput8.dll of someone else's by them): keep the log line.

A change to any of these needs a launcher that copes with both the old and the new form, shipped one release before the old form is dropped.

## Pre-releases (for testers; never installed by the shortcut)

- **Scheme:** pre-releases are versions `0.0.N` (`0.0.1` = "Pre-release 1", `0.0.2` = "Pre-release 2"), tag `v0.0.N`, created with `--prerelease`; the first full release is `0.1.0` or higher, so it sorts strictly above every
  pre-release and the launcher updates a pre-release install to it. GitHub's `releases/latest` never returns a pre-release (or a draft), so the shortcut's launcher never installs one by itself:
  testers install the zip / setup exe by hand. Never use `--latest` on a pre-release. Never label a pre-release with any "RC" wording.
- **Freeze:** everything committed (`git status` clean), `sh tests/build.sh`, `sh tests/build_menu.sh`, `powershell -File tests\test_installer.ps1` green, `sh native/build.sh`, then
  `powershell -File tools\package.ps1 -Version 0.0.2 -Exe -Dist ..\work\dist-pre2` (zip + setup exe; record both SHA-256 in the release notes).
- **Publish checklist (the lead, after the owner's word):**
  1. `git status` clean in the mod repository; the commit to tag is the one the package was built from (`git log -1`); no personal paths / names / e-mail in the tree
     (search the tree for drive-letter paths and e-mail addresses: nothing may be found).
  2. `git push origin main`
  3. `git tag -a v0.0.2 -m "Pre-release 2"` then `git push origin v0.0.2` (commit identity: the noreply one of the repository owner).
  4. `gh release create v0.0.2 --repo dolbaras/revelations-coop --prerelease --title "Pre-release 2 (0.0.2)" --notes-file <relnotes body> <dist>\revelations-coop-0.0.2.zip <dist>\revelations-coop-setup-0.0.2.exe`
  5. Check: `gh release view v0.0.2 --repo dolbaras/revelations-coop --json isPrerelease,tagName,assets` (isPrerelease true, both assets there, the zip's digest equals the recorded SHA-256);
     `gh api repos/dolbaras/revelations-coop/releases/latest` must NOT return v0.0.2 (404 while there is no full release).
  6. Tell the testers: install `revelations-coop-setup-0.0.2.exe` or run `Install Co-op.bat` from the zip; report with `rev1_coop.log` (see the players' readme).

## Building

```
sh native/build.sh                                    # native\build\dinput8.dll (32-bit, Zig)
powershell -File tools\package.ps1 -Version 0.1.0     # dist\revelations-coop-0.1.0.zip
powershell -File tools\package.ps1 -Version 0.1.0 -Exe   # + dist\revelations-coop-setup-0.1.0.exe (workspace tools\mkinstaller; unsigned)
```

The zip holds `Install Co-op.bat`, `coop-installer.ps1`, `coop-texts.ps1` (the 13 languages of the game; shared by the installer and the launcher), `README-COOP.txt`,
`LICENSE.txt` and `files\` (`dinput8.dll`, `coop\coop-launcher.ps1`, `coop\coop-installer.ps1`, `coop\coop-texts.ps1`, `coop\README.txt`, `coop\version.txt`,
`coop\payload.txt` = the list of every payload file that the installer checks before it touches the game).

## What the installer does

- Finds the game: its own folder, the Steam uninstall key of app 222480, every Steam library (`libraryfolders.vdf` and the `appmanifest_222480.acf` installdir), common
  folders, or the player picks it. **It checks that `rerev.exe` is the Steam build 13298487** (SHA-256 `1573b79e...48aa`) and refuses any other exe (the mod's gate would keep
  it off anyway); `-AnyExe` or `RE1_COOP_ANYEXE=1` skip the check (tests only).
- Refuses while a `rerev.exe` runs from that folder. A folder Windows protects is installed by an elevated copy of the installer (Windows asks the player).
- Keeps a `dinput8.dll` of someone else's (an ASI loader, a graphics fix) in `<game>\coop-backup`, once; its own DLL of an older version is never kept as the original.
- Copies the payload, writes `coop\install.txt` (`coop\version.txt` last: the launcher reads it, an update that broke off is tried again at the next start), the
  desktop and Start menu shortcuts (the game's own icon with a co-op badge, `coop\coop.ico`), and `Uninstall Co-op.bat` at the game folder's root.
- Removal (`Uninstall Co-op.bat`, or the installer again with "Remove co-op"): deletes what the record lists, puts the backed-up files back, removes the shortcuts, `coop`
  and `coop-backup`. The game's saves and `rev1_coop.ini` (the lobby's settings, `%LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS`) are not touched.
- Texts in 13 languages (en ru de fr it es pt pt-br nl pl ja zh-cn zh-tw; the game's own set), chosen by Windows' language, `-Lang` or the list in the window.

## Tests

`powershell -File tests\test_installer.ps1 [-RealGame <folder with the real rerev.exe>]` (everything in a temp folder, no game is started, no network): the zip's layout and
payload list; a fake exe is refused without a trace; install over a game with a foreign dinput8.dll (backup, record, two shortcuts, every other game file untouched);
reinstall (the backup stays the foreign DLL); the launcher's update from a local feed served on 127.0.0.1 (a wrong checksum refused, the right one installed, backup and
shortcuts kept); uninstall (the game folder is byte for byte the clean one); a game without dinput8.dll; a path with spaces and an apostrophe; with `-RealGame` a COPY of
the real exe passes the hash check and its icon becomes the shortcut's icon; the 13 texts (same keys, same placeholders, translated) and the language choice. The installer's WINDOW is
checked by `lab\inst_window_test.ps1` of the lab (install / uninstall through the zip installer and through the setup exe, on a stand-in folder and on a game test copy, windows on a
second monitor, pictures of the windows themselves): test-only variables `RE1_COOP_UI_POS=x,y` (window at that position WITHOUT taking the focus), `RE1_COOP_UI_SHOTS=<folder>`,
`RE1_COOP_UI_LANGS=en,ru`, `RE1_COOP_UI_TAG`, `RE1_COOP_UI_AUTO=install|remove` (clicks the button, only for the folder in `RE1_COOP_UI_ONLY`; a player never sets them).

An update test against the real GitHub is done with `coop-launcher.ps1 -Feed http://127.0.0.1:<port>/latest.json -NoStart` as the test does, or `RE1_COOP_FEED` for a start
through the shortcut. New shortcuts go to `-ShortcutDir <folder>` in tests, never to the desktop.
