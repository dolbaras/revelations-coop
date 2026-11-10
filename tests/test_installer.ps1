# Offline tests of the installer, the uninstaller, the launcher's update and the texts (no game is started, no network, nothing outside a temp folder).
# A stand-in game folder (a tiny fake rerev.exe, -AnyExe) is made under %TEMP%; the supported exe's hash check is tested on a fake exe (refusal) and,
# when -RealGame <folder> is given, on a COPY of the real rerev.exe (the folder must be a throw-away one that holds only that copy: the test refuses a Steam
# library folder and anything named game; it never writes there, it copies the exe into its own temp folder).
# -OldZip <released zip>: also the update from that release (its own launcher) to a 0.1.0 package of this tree (section 4b): the published pre-release
#   work\dist-pre2c\revelations-coop-0.0.2.zip (= GitHub's v0.0.2 asset, sha256 f2139927...0192).
# -Online: also reads the real GitHub (read only) and downloads the published v0.0.2 zip once through this tree's launcher (section 4c).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tests\test_installer.ps1 [-RealGame <folder with a copy of the real rerev.exe>] [-OldZip <zip>] [-Online] [-Keep]
param([string] $RealGame = "", [switch] $Keep, [string] $OldZip = "", [switch] $Online)
if ($OldZip) { if (-not (Test-Path -LiteralPath $OldZip -PathType Leaf)) { throw "-OldZip: $OldZip not found" }; $OldZip = (Resolve-Path -LiteralPath $OldZip).Path }
if ($RealGame -and ($RealGame -match '\\steamapps\\' -or (Split-Path $RealGame -Leaf) -ieq "game" -or $RealGame -match 'RESIDENT EVIL REVELATIONS')) { throw "-RealGame must be a throw-away folder with a copy of rerev.exe, not a game install: $RealGame" }
$ErrorActionPreference = "Stop"
$env:PSModulePath = (($env:PSModulePath -split ";") | Where-Object { $_ -notlike "*PowerShell*" }) -join ";"   # Windows PowerShell 5.1 modules only
$root = Split-Path $PSScriptRoot -Parent
$tools = Join-Path $root "tools"
$base = Join-Path ([IO.Path]::GetTempPath()) ("re1-installer-test-" + $PID)
Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $base | Out-Null
$script:fails = 0; $script:checks = 0
function Check($cond, $name) { $script:checks++; if ($cond) { Write-Host "ok   $name" } else { Write-Host "FAIL $name"; $script:fails++ } }
function Sha($p) { $fs = [IO.File]::OpenRead($p); try { [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($fs)) -replace "-", "" } finally { $fs.Close() } }
function Snap($dir) {
    $t = @{}
    foreach ($f in Get-ChildItem -LiteralPath $dir -Recurse -File -Force) { $t[$f.FullName.Substring($dir.Length + 1)] = Sha $f.FullName }
    $t
}
function Same($a, $b) {
    if ($a.Count -ne $b.Count) { return $false }
    foreach ($k in $a.Keys) { if (-not $b.ContainsKey($k) -or $a[$k] -ne $b[$k]) { return $false } }
    $true
}
function Run-PS([string] $script, [string[]] $a) {
    $o = Join-Path $base "out.txt"; $e = Join-Path $base "err.txt"
    Remove-Item $o, $e -ErrorAction SilentlyContinue
    $args2 = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$script`"") + $a
    $p = Start-Process powershell -ArgumentList $args2 -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput $o -RedirectStandardError $e
    $script:exit = $p.ExitCode
    $script:out = (Get-Content -LiteralPath $o -Raw -ErrorAction SilentlyContinue) + (Get-Content -LiteralPath $e -Raw -ErrorAction SilentlyContinue)
}

# a DLL to package: the real build when there is one, else a stand-in with the words the installer looks for
$dll = Join-Path $root "native\build\dinput8.dll"
$fakeDll = Join-Path $base "built\native\build\dinput8.dll"
# the package script works from the repository layout: make a copy of the tools / docs around a stand-in build when the real DLL is missing
$pkgRoot = $root
if (-not (Test-Path $dll)) {
    $pkgRoot = Join-Path $base "repo"
    New-Item -ItemType Directory -Force (Join-Path $pkgRoot "native\build") | Out-Null
    Copy-Item $tools (Join-Path $pkgRoot "tools") -Recurse; Copy-Item (Join-Path $root "docs") (Join-Path $pkgRoot "docs") -Recurse; Copy-Item (Join-Path $root "LICENSE") $pkgRoot
    [IO.File]::WriteAllBytes((Join-Path $pkgRoot "native\build\dinput8.dll"), [Text.Encoding]::ASCII.GetBytes("MZ stand-in RE Revelations co-op proxy 0.0"))
}
$dist = Join-Path $base "dist"
# (the workspace's shared bug-report collector: the first tools\log-collector above the repository, three levels above mod\, four above a worktree in work\;
# passed explicitly, also for the stand-in copy under %TEMP%)
$logCollector = $null
for ($d = Split-Path $root -Parent; $d -and -not $logCollector; $d = Split-Path $d -Parent) { if (Test-Path -LiteralPath (Join-Path $d "tools\log-collector\collect-logs.ps1")) { $logCollector = Join-Path $d "tools\log-collector" } }
if (-not $logCollector) { throw "tools\log-collector not found above $root" }
# (-AnyDllVersion: the stand-in DLL, or the real build, is packaged under the test versions 0.0.1 / 0.0.2; the version check itself is tested below)
function Package($v, $out = $dist) { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $pkgRoot "tools\package.ps1") -Version $v -Dist $out -LogCollector $logCollector -AnyDllVersion | Out-Null; if ($LASTEXITCODE) { throw "package.ps1 failed" } }
Package "0.0.1"; Package "0.0.2"; Package "0.1.0"
# package.ps1 refuses a DLL whose R1_VERSION is not -Version (the DLL holds it as a string of its own: "\0<version>\0")
$vdll = Join-Path $base "vdll\dinput8.dll"; New-Item -ItemType Directory -Force (Split-Path $vdll) | Out-Null
[IO.File]::WriteAllBytes($vdll, [Text.Encoding]::ASCII.GetBytes("MZ stand-in RE Revelations co-op proxy %s loaded`0" + "0.0.3`0"))
$vdist = Join-Path $base "vdist"
Run-PS (Join-Path $pkgRoot "tools\package.ps1") @("-Version", "0.0.4", "-Dist", "`"$vdist`"", "-Dll", "`"$vdll`"", "-LogCollector", "`"$logCollector`"")
Check ($script:exit -ne 0 -and $script:out -match "is not version 0\.0\.4" -and -not (Test-Path (Join-Path $vdist "revelations-coop-0.0.4.zip"))) "package.ps1 refuses a DLL of another version (0.0.3 packaged as 0.0.4)"
Run-PS (Join-Path $pkgRoot "tools\package.ps1") @("-Version", "0.0.3", "-Dist", "`"$vdist`"", "-Dll", "`"$vdll`"", "-LogCollector", "`"$logCollector`"")
Check ($script:exit -eq 0 -and (Test-Path (Join-Path $vdist "revelations-coop-0.0.3.zip"))) "package.ps1 takes the DLL of the same version (0.0.3)"
$zip1 = Join-Path $dist "revelations-coop-0.0.1.zip"; $zip2 = Join-Path $dist "revelations-coop-0.0.2.zip"; $zip3 = Join-Path $dist "revelations-coop-0.1.0.zip"
Check ((Test-Path $zip1) -and (Test-Path $zip2)) "package.ps1 builds revelations-coop-<version>.zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
# (rel010) every entry name uses "/": Compress-Archive of Windows PowerShell 5.1 wrote "files\coop\..." (the published 0.0.1 zip), `unzip` then warned and exited 1
$za1 = [IO.Compression.ZipFile]::OpenRead($zip1)
try { $en = @($za1.Entries | ForEach-Object FullName) } finally { $za1.Dispose() }
$bs = @($en | Where-Object { $_.Contains('\') })
Check ($en.Count -ge 18 -and $bs.Count -eq 0 -and ($en -contains "files/coop/coop-launcher.ps1") -and ($en -contains "files/dinput8.dll")) "the zip's $($en.Count) entry names use '/' ($($bs.Count) with '\'$(if ($bs.Count) { ': ' + $bs[0] }))"
$x1 = Join-Path $base "x1"; $x2 = Join-Path $base "x2"; $x3 = Join-Path $base "x3"
[IO.Compression.ZipFile]::ExtractToDirectory($zip1, $x1); [IO.Compression.ZipFile]::ExtractToDirectory($zip2, $x2); [IO.Compression.ZipFile]::ExtractToDirectory($zip3, $x3)
$names = (Get-ChildItem $x1 -File | ForEach-Object Name) -join ","
Check ($names -eq "coop-installer.ps1,coop-texts.ps1,Install Co-op.bat,LICENSE.txt,README-COOP.txt") "the zip root: $names"
$payload1 = @(Get-Content (Join-Path $x1 "files\coop\payload.txt"))
Check ((($payload1 -contains "dinput8.dll") -and ($payload1 -contains "coop\coop-launcher.ps1") -and ($payload1 -contains "coop\coop-installer.ps1") -and ($payload1 -contains "coop\coop-texts.ps1") -and ($payload1 -contains "coop\version.txt"))) "payload list names the DLL, launcher, installer, texts and version"
Check ((($payload1 -contains "coop\collect-logs.ps1") -and ($payload1 -contains "coop\Collect logs.bat") -and ($payload1 -contains "coop\log-profiles\revelations1.psd1"))) "payload list names the log collector, its .bat and the profile"
Check ((Get-Content -LiteralPath (Join-Path $x1 "files\coop\Collect logs.bat") -Raw) -like '*collect-logs.ps1" -Mod revelations1*') "Collect logs.bat runs collect-logs.ps1 -Mod revelations1"
Check ((Get-Content (Join-Path $x2 "files\coop\version.txt") -TotalCount 1) -eq "0.0.2") "version.txt of the 0.0.2 package"

# 0. the 0.1.0 package: version, readme (names its own version, port and firewall, no list of known problems), the DLL's version, the release procedure
Check ((Get-Content (Join-Path $x3 "files\coop\version.txt") -TotalCount 1) -eq "0.1.0") "version.txt of the 0.1.0 package"
$rd1 = Get-Content (Join-Path $x1 "README-COOP.txt") -Raw; $rd3 = Get-Content (Join-Path $x3 "README-COOP.txt") -Raw
Check ($rd3 -match "version 0\.1\.0" -and $rd1 -match "version 0\.0\.1" -and $rd3 -notmatch "@VERSION@") "README-COOP.txt names the version of its own package (0.1.0 / 0.0.1), no placeholder left"
Check ((Get-Content (Join-Path $x3 "files\coop\README.txt") -Raw) -eq $rd3) "coop\README.txt is the same text as README-COOP.txt"
Check ($rd3 -match "28410" -and $rd3 -match "(?i)firewall" -and $rd3 -match "(?i)Episode 1") "the readme names UDP 28410, the firewall and the Episode 1 scope"
Check ($rd3 -notmatch "(?i)known problems" -and $rd3 -notmatch "(?i)pre-release (\d|for testing)|this pre-release|Pre-release \d" -and $rd3 -notmatch "(?i)release\scandidate|\bRC\b") "the readme has no 'known problems' list and no pre-release / RC wording"
$r1h = Get-Content (Join-Path $root "native\r1.h") -Raw
Check ($r1h -match '#define R1_VERSION "(\d+\.\d+\.\d+)"') "R1_VERSION is a plain major.minor.patch ($($Matches[1]))"
$relDoc = Get-Content (Join-Path $root "docs\releasing.md") -Raw
$fullPart = ($relDoc -split "(?m)^## Full release")[1]; if ($fullPart) { $fullPart = ($fullPart -split "(?m)^## ")[0] }
Check ($fullPart -match "--latest" -and $fullPart -notmatch "--prerelease\s+--title|create v0\.1\.0[^\r\n]*--prerelease" -and $fullPart -match "releases/latest" -and $fullPart -match "28410|test_update_real") "releasing.md: a 'Full release' section with --latest, no --prerelease on the create line, the releases/latest check"

# ---- the stand-in game
function New-Game($name, [switch] $WithForeignDll, [switch] $RealExe) {
    $g = Join-Path $base $name
    New-Item -ItemType Directory -Force (Join-Path $g "nativePC\msg") | Out-Null
    if ($RealExe) { Copy-Item (Join-Path $RealGame "rerev.exe") (Join-Path $g "rerev.exe") } else { [IO.File]::WriteAllText((Join-Path $g "rerev.exe"), "MZ fake game exe $name") }
    [IO.File]::WriteAllText((Join-Path $g "steam_api.dll"), "steam api")
    [IO.File]::WriteAllText((Join-Path $g "nativePC\msg\Title_eng.gmd"), "texts")
    if ($WithForeignDll) {
        [IO.File]::WriteAllText((Join-Path $g "dinput8.dll"), "ASI LOADER of somebody else")
        # (a loader the player renamed by hand for the mod's chain rule, and a mod's own folder: the installer must leave both alone)
        [IO.File]::WriteAllText((Join-Path $g "dinput8_real.dll"), "ASI LOADER renamed by the player")
        New-Item -ItemType Directory -Force (Join-Path $g "scripts") | Out-Null
        [IO.File]::WriteAllText((Join-Path $g "scripts\widescreen.asi"), "a plugin")
    }
    $g
}
$g = New-Game "RESIDENT EVIL REVELATIONS" -WithForeignDll
$links = (Join-Path $base "links one") + ";" + (Join-Path $base "links two")
$clean = Snap $g

# 1. refusal: not the supported exe (no -AnyExe): nothing may change
Run-PS (Join-Path $x1 "coop-installer.ps1") @("-Quiet", "-Game", "`"$g`"", "-Lang", "en")
Check ($script:exit -eq 1) "a fake exe is refused (exit $($script:exit))"
Check ($script:out -match "13298487") "the refusal names the supported build"
Check (Same $clean (Snap $g)) "nothing changed after the refusal"

# 2. install 0.0.1 over a game that has a foreign dinput8.dll
Run-PS (Join-Path $x1 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$g`"", "-ShortcutDir", "`"$links`"", "-Lang", "de")
Check ($script:exit -eq 0) "install 0.0.1: exit 0 ($($script:out.Trim()))"
Check ((Sha (Join-Path $g "dinput8.dll")) -eq (Sha (Join-Path $x1 "files\dinput8.dll"))) "dinput8.dll is the mod's"
Check ((Get-Content (Join-Path $g "coop-backup\dinput8.dll") -Raw) -eq "ASI LOADER of somebody else") "the foreign dinput8.dll is kept in coop-backup"
foreach ($f in "coop\coop-launcher.ps1", "coop\coop-installer.ps1", "coop\coop-texts.ps1", "coop\version.txt", "coop\README.txt", "coop\install.txt", "Uninstall Co-op.bat",
               "coop\collect-logs.ps1", "coop\Collect logs.bat", "coop\log-profiles\revelations1.psd1") {
    Check (Test-Path -LiteralPath (Join-Path $g $f)) "installed: $f"
}
$rec = Get-Content (Join-Path $g "coop\install.txt")
Check ($rec -contains "version=0.0.1") "record: version=0.0.1"
Check (($rec -contains "file=coop\collect-logs.ps1") -and ($rec -contains "file=coop\log-profiles\revelations1.psd1")) "record: the log collector's files (removed on uninstall)"
# the installed collector finds this game from its own folder and writes its zip (Documents, %LOCALAPPDATA%, Steam and the desktop under a temp folder)
Set-Content -LiteralPath (Join-Path $g "rev1_coop.log") -Value "proxy loaded from $g" -Encoding ASCII
$env:COLLECT_LOGS_TEST_HOME = Join-Path $base "collector home"; New-Item -ItemType Directory -Force (Join-Path $env:COLLECT_LOGS_TEST_HOME "Desktop") | Out-Null
Run-PS (Join-Path $g "coop\collect-logs.ps1") @("-NoPause")
$cz = @(Get-ChildItem -LiteralPath (Join-Path $env:COLLECT_LOGS_TEST_HOME "Desktop") -Filter "revelations-coop-report-*.zip")
Remove-Item Env:\COLLECT_LOGS_TEST_HOME
Check ($script:exit -eq 0 -and $cz.Count -eq 1) "the installed coop\collect-logs.ps1 writes revelations-coop-report-*.zip (exit $($script:exit))"
if ($cz.Count -eq 1) {
    $za = [IO.Compression.ZipFile]::OpenRead($cz[0].FullName)
    try { $zn = @($za.Entries | ForEach-Object FullName); $zr = (New-Object IO.StreamReader($za.GetEntry("logs/rev1_coop.log").Open())).ReadToEnd() } finally { $za.Dispose() }
    Check (($zn -contains "report.txt") -and ($zn -contains "coop/install.txt") -and $zr.Contains("<game>") -and -not $zr.Contains($g)) "the report zip: report.txt, the record, the log with the game folder replaced by <game>"
}
Remove-Item -LiteralPath (Join-Path $g "rev1_coop.log") -Force
Check ($rec -contains "backup=dinput8.dll") "record: backup=dinput8.dll"
Check (($rec | Where-Object { $_ -like "shortcut=*" }).Count -eq 2) "record: two shortcuts"
$shell = New-Object -ComObject WScript.Shell
foreach ($d in ($links -split ";")) {
    $l = Join-Path $d "Resident Evil Revelations Co-op.lnk"
    $ok = (Test-Path -LiteralPath $l) -and ($shell.CreateShortcut($l).Arguments -like "*coop-launcher.ps1*")
    Check $ok "shortcut in $(Split-Path $d -Leaf) runs the launcher"
}
$afterInstall = Snap $g
foreach ($k in $clean.Keys) { if ($k -ne "dinput8.dll") { Check ($afterInstall[$k] -eq $clean[$k]) "game file unchanged: $k" } }

# 3. reinstall the same version: the backup stays the foreign DLL (never our own DLL), no duplicates
Run-PS (Join-Path $x1 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$g`"", "-ShortcutDir", "`"$links`"", "-Lang", "en")
Check ($script:exit -eq 0) "reinstall: exit 0"
Check ((Get-Content (Join-Path $g "coop-backup\dinput8.dll") -Raw) -eq "ASI LOADER of somebody else") "reinstall keeps the foreign DLL as the backup"
$rec = Get-Content (Join-Path $g "coop\install.txt")
Check (@($rec | Where-Object { $_ -eq "file=dinput8.dll" }).Count -eq 1) "reinstall: dinput8.dll listed once"
Check (@($rec | Where-Object { $_ -eq "backup=dinput8.dll" }).Count -eq 1) "reinstall: one backup line"
# (that foreign dinput8.dll is no ASI loader: it stays off, nothing else is made, the result says so)
. (Join-Path $tools "coop-texts.ps1")
Check (-not (Test-Path -LiteralPath (Join-Path $g "winmm.dll")) -and -not ($rec | Where-Object { $_ -like "chain=*" })) "a dinput8.dll that is no ASI loader: no winmm.dll, no chain= line"
Check ($script:out.Contains($script:TEXTS["en"]["offnote"])) "the result says the other mod's dinput8.dll is kept and off while co-op is installed"

# 3b. an Ultimate ASI Loader as the foreign dinput8.dll (the Fusion Fix ships one): a copy runs on as winmm.dll, its settings file goes along as winmm.ini, the
#     player's plugin folder stays; reinstall and update keep the copies; uninstall puts the folder back byte for byte. Made-up PE files (never run): the installer
#     reads the export table only (IsUltimateASILoader + timeGetTime, 32-bit). A real loader may never be named dinput8_real.dll: under that name it closes the
#     game (tests\test_chain.c, native\chain_logic.h).
function New-FakePe($path, [string[]] $exports, [int] $machine = 0x14C, [string] $extra = "") {
    $b = New-Object byte[] 0xA00
    $w16 = { param($o, $v) $b[$o] = [byte]($v -band 0xFF); $b[$o + 1] = [byte](($v -shr 8) -band 0xFF) }
    $w32 = { param($o, [long] $v) & $w16 $o ($v -band 0xFFFF); & $w16 ($o + 2) (($v -shr 16) -band 0xFFFF) }
    $b[0] = 0x4D; $b[1] = 0x5A; & $w32 0x3C 0x80
    $b[0x80] = 0x50; $b[0x81] = 0x45
    $pe64 = $machine -eq 0x8664
    & $w16 0x84 $machine; & $w16 0x86 2; $optsize = $(if ($pe64) { 0xF0 } else { 0xE0 }); & $w16 0x94 $optsize; & $w16 0x96 0x2102
    $opt = 0x98; & $w16 $opt $(if ($pe64) { 0x20B } else { 0x10B })
    $dirs = $opt + $(if ($pe64) { 112 } else { 96 }); & $w32 ($dirs - 4) 16; & $w32 $dirs 0x2000; & $w32 ($dirs + 4) 0x100
    $sec = $opt + $optsize
    [Text.Encoding]::ASCII.GetBytes(".text").CopyTo($b, $sec); & $w32 ($sec + 8) 0x200; & $w32 ($sec + 12) 0x1000; & $w32 ($sec + 16) 0x200; & $w32 ($sec + 20) 0x400
    [Text.Encoding]::ASCII.GetBytes(".edata").CopyTo($b, $sec + 40); & $w32 ($sec + 48) 0x400; & $w32 ($sec + 52) 0x2000; & $w32 ($sec + 56) 0x400; & $w32 ($sec + 60) 0x600
    & $w32 (0x600 + 20) $exports.Count; & $w32 (0x600 + 24) $exports.Count; & $w32 (0x600 + 32) 0x2040
    $str = 0x100
    for ($i = 0; $i -lt $exports.Count; $i++) {
        & $w32 (0x640 + 4 * $i) (0x2000 + $str)
        [Text.Encoding]::ASCII.GetBytes($exports[$i]).CopyTo($b, 0x600 + $str); $str += $exports[$i].Length + 1
    }
    if ($extra) { [Text.Encoding]::ASCII.GetBytes($extra).CopyTo($b, 0x480) }
    [IO.File]::WriteAllBytes($path, $b)
}
$ualExports = @("DirectInput8Create", "DrawDibDraw", "IsUltimateASILoader", "timeBeginPeriod", "timeGetTime")
function New-UalGame($name, [int] $machine = 0x14C, [switch] $WithWinmm, [switch] $NoIni) {
    $gg = New-Game $name
    New-FakePe (Join-Path $gg "dinput8.dll") $ualExports $machine "Ultimate ASI Loader $name"
    if (-not $NoIni) { Set-Content -LiteralPath (Join-Path $gg "dinput8.ini") -Value "[GlobalSets]`r`nLoadPlugins=1" -Encoding ASCII }
    New-Item -ItemType Directory -Force (Join-Path $gg "scripts") | Out-Null
    New-FakePe (Join-Path $gg "scripts\Some.FusionFix.asi") @("InitializeASI") 0x14C "IsUltimateASILoader"
    Set-Content -LiteralPath (Join-Path $gg "scripts\Some.FusionFix.ini") -Value "[MAIN]`r`nSkipIntro = 1" -Encoding ASCII
    if ($WithWinmm) { Set-Content -LiteralPath (Join-Path $gg "winmm.dll") -Value "a winmm.dll of someone else's" -Encoding ASCII }
    $gg
}
$gu = New-UalGame "ual game"
$cleanU = Snap $gu
Run-PS (Join-Path $x1 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$gu`"", "-ShortcutDir", "`"$(Join-Path $base 'links ual')`"", "-Lang", "en")
$recU = @(Get-Content (Join-Path $gu "coop\install.txt"))
Check ($script:exit -eq 0 -and (Sha (Join-Path $gu "dinput8.dll")) -eq (Sha (Join-Path $x1 "files\dinput8.dll"))) "ASI loader: install 0.0.1 exit 0, dinput8.dll is the mod's ($($script:exit))"
Check ((Sha (Join-Path $gu "coop-backup\dinput8.dll")) -eq $cleanU["dinput8.dll"] -and (Sha (Join-Path $gu "winmm.dll")) -eq $cleanU["dinput8.dll"]) "ASI loader: kept in coop-backup AND running on as winmm.dll (the same bytes)"
Check ((Sha (Join-Path $gu "winmm.ini")) -eq $cleanU["dinput8.ini"]) "ASI loader: its settings file dinput8.ini goes along as winmm.ini"
Check (($recU -contains "chain=winmm.dll") -and ($recU -contains "chain=winmm.ini") -and ($recU -contains "file=winmm.dll") -and ($recU -contains "file=winmm.ini") -and ($recU -contains "backup=dinput8.dll")) "ASI loader: the record lists both copies (chain= and file=) and the backup"
Check ($script:out.Contains($script:TEXTS["en"]["asinote"]) -and -not $script:out.Contains($script:TEXTS["en"]["offnote"])) "ASI loader: the result says it runs on as winmm.dll"
$afterU = Snap $gu
Check (@($cleanU.Keys | Where-Object { $_ -ne "dinput8.dll" -and $afterU[$_] -ne $cleanU[$_] }).Count -eq 0) "ASI loader: every other game file unchanged (dinput8.ini, scripts\*.asi / .ini)"
# reinstall the same version and update to 0.0.2: the copies stay, listed once
Run-PS (Join-Path $x1 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$gu`"", "-ShortcutDir", "`"$(Join-Path $base 'links ual')`"")
Run-PS (Join-Path $x2 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$gu`"", "-ShortcutDir", "`"$(Join-Path $base 'links ual')`"")
$recU = @(Get-Content (Join-Path $gu "coop\install.txt"))
Check ($script:exit -eq 0 -and (Get-Content -LiteralPath (Join-Path $gu "coop\version.txt") -TotalCount 1) -eq "0.0.2" -and (Sha (Join-Path $gu "winmm.dll")) -eq $cleanU["dinput8.dll"] -and (Sha (Join-Path $gu "coop-backup\dinput8.dll")) -eq $cleanU["dinput8.dll"]) "ASI loader: reinstall + update to 0.0.2 keep the backup and the copy"
Check (@($recU | Where-Object { $_ -eq "chain=winmm.dll" }).Count -eq 1 -and @($recU | Where-Object { $_ -eq "file=winmm.dll" }).Count -eq 1 -and @($recU | Where-Object { $_ -eq "chain=winmm.ini" }).Count -eq 1) "ASI loader: after reinstall + update each copy is listed once"
Run-PS (Join-Path $gu "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$gu`"")
Check ($script:exit -eq 0 -and (Same $cleanU (Snap $gu))) "ASI loader: uninstall puts the folder back byte for byte (the loader as dinput8.dll, no winmm.dll / winmm.ini, scripts\ as it was)"
# the player replaced the copy after the install (his own winmm.dll now): it stays; the backup still comes back
Run-PS (Join-Path $x2 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$gu`"", "-ShortcutDir", "`"$(Join-Path $base 'links ual')`"")
Set-Content -LiteralPath (Join-Path $gu "winmm.dll") -Value "the player's own winmm.dll" -Encoding ASCII
Run-PS (Join-Path $gu "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$gu`"")
Check ($script:exit -eq 0 -and (Get-Content -LiteralPath (Join-Path $gu "winmm.dll") -Raw).Trim() -eq "the player's own winmm.dll" -and (Sha (Join-Path $gu "dinput8.dll")) -eq $cleanU["dinput8.dll"] -and
       -not (Test-Path -LiteralPath (Join-Path $gu "winmm.ini")) -and -not (Test-Path -LiteralPath (Join-Path $gu "coop-backup"))) "ASI loader: a winmm.dll the player changed is left in place by the uninstall, the loader comes back, winmm.ini goes"
# a winmm.dll of someone else's in the folder before the install: never touched, the loader stays off (and the result says so)
$gw = New-UalGame "ual with winmm" -WithWinmm
$cleanW = Snap $gw
Run-PS (Join-Path $x2 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$gw`"", "-ShortcutDir", "`"$(Join-Path $base 'links ualw')`"", "-Lang", "en")
$recW = @(Get-Content (Join-Path $gw "coop\install.txt"))
Check ($script:exit -eq 0 -and (Sha (Join-Path $gw "winmm.dll")) -eq $cleanW["winmm.dll"] -and -not (Test-Path -LiteralPath (Join-Path $gw "winmm.ini")) -and -not ($recW | Where-Object { $_ -like "chain=*" -or $_ -eq "file=winmm.dll" })) "ASI loader + a foreign winmm.dll: that winmm.dll untouched, no copies, nothing chained in the record"
Check ($script:out.Contains($script:TEXTS["en"]["offnote"])) "ASI loader + a foreign winmm.dll: the result says the loader is kept and off"
Run-PS (Join-Path $gw "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$gw`"")
Check ($script:exit -eq 0 -and (Same $cleanW (Snap $gw))) "ASI loader + a foreign winmm.dll: uninstall leaves the folder byte for byte"
# a 64-bit loader (it cannot stand in for the 32-bit game's winmm.dll) and a loader without dinput8.ini
$g64 = New-UalGame "ual 64" -Machine 0x8664
$clean64 = Snap $g64
Run-PS (Join-Path $x2 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$g64`"", "-ShortcutDir", "`"$(Join-Path $base 'links ual64')`"")
Check ($script:exit -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $g64 "winmm.dll"))) "a 64-bit ASI loader: no winmm.dll copy"
Run-PS (Join-Path $g64 "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$g64`"")
Check (Same $clean64 (Snap $g64)) "a 64-bit ASI loader: uninstall leaves the folder byte for byte"
$gn = New-UalGame "ual no ini" -NoIni
$cleanN = Snap $gn
Run-PS (Join-Path $x2 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$gn`"", "-ShortcutDir", "`"$(Join-Path $base 'links ualn')`"")
$recN = @(Get-Content (Join-Path $gn "coop\install.txt"))
Check ((Sha (Join-Path $gn "winmm.dll")) -eq $cleanN["dinput8.dll"] -and -not (Test-Path -LiteralPath (Join-Path $gn "winmm.ini")) -and ($recN -contains "chain=winmm.dll") -and -not ($recN -contains "chain=winmm.ini")) "an ASI loader without dinput8.ini: winmm.dll only"
Run-PS (Join-Path $gn "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$gn`"")
Check (Same $cleanN (Snap $gn)) "an ASI loader without dinput8.ini: uninstall leaves the folder byte for byte"
# a plugin that only NAMES the loader (FusionFix.asi asks GetProcAddress for IsUltimateASILoader) as the foreign dinput8.dll: not a loader
$gp = New-Game "named only"
New-FakePe (Join-Path $gp "dinput8.dll") @("DirectInput8Create", "timeGetTime") 0x14C "IsUltimateASILoader"
Run-PS (Join-Path $x2 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$gp`"", "-ShortcutDir", "`"$(Join-Path $base 'links named')`"")
Check ($script:exit -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $gp "winmm.dll"))) "a dinput8.dll that has the loader's name only as a string (not exported): no winmm.dll"
Run-PS (Join-Path $gp "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$gp`"")

# 3c. (ual-rev2, 2026-11-08) other mods' proxy DLLs next to the loader (d3d9 of DXVK / ReShade, dsound + its ini, version, xinput1_3, a plugins folder): never
#     touched by install, update or uninstall
function Sha0($p) { if (Test-Path -LiteralPath $p -PathType Leaf) { Sha $p } else { "(missing)" } }
function Snap-Diff($a, $b) {   # (not "Diff": an alias of Compare-Object in Windows PowerShell)
    @($a.Keys | Where-Object { -not $b.ContainsKey($_) -or $a[$_] -ne $b[$_] }) + @($b.Keys | Where-Object { -not $a.ContainsKey($_) }) | Sort-Object -Unique
}
$gx = New-UalGame "ual with proxies"
New-FakePe (Join-Path $gx "d3d9.dll") @("Direct3DCreate9", "Direct3DCreate9Ex") 0x14C "DXVK stand-in"
New-FakePe (Join-Path $gx "dsound.dll") @("DirectSoundCreate", "DirectSoundCreate8") 0x14C "DSOAL stand-in"
Set-Content -LiteralPath (Join-Path $gx "dsound.ini") -Value "[general]`r`nfrequency=48000" -Encoding ASCII
New-FakePe (Join-Path $gx "version.dll") @("GetFileVersionInfoA", "VerQueryValueA") 0x14C "version proxy stand-in"
New-FakePe (Join-Path $gx "xinput1_3.dll") @("XInputGetState", "XInputSetState") 0x14C "xinput proxy stand-in"
New-Item -ItemType Directory -Force (Join-Path $gx "plugins") | Out-Null
New-FakePe (Join-Path $gx "plugins\FpsLimit.asi") @("InitializeASI") 0x14C ""
$cleanX = Snap $gx
Run-PS (Join-Path $x1 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$gx`"", "-ShortcutDir", "`"$(Join-Path $base 'links ualx')`"", "-Lang", "en")
Run-PS (Join-Path $x2 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$gx`"", "-ShortcutDir", "`"$(Join-Path $base 'links ualx')`"", "-Lang", "en")
$afterX = Snap $gx
$chX = @(Snap-Diff $cleanX $afterX | Where-Object { $cleanX.ContainsKey($_) -and $_ -ne "dinput8.dll" })
Check ($script:exit -eq 0 -and (Sha0 (Join-Path $gx "winmm.dll")) -eq $cleanX["dinput8.dll"] -and $chX.Count -eq 0) "ASI loader + d3d9 / dsound (+ini) / version / xinput1_3 / plugins: the copy made, every other file unchanged after install + update ($($chX -join ', '))"
Run-PS (Join-Path $gx "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$gx`"")
$dX = @(Snap-Diff $cleanX (Snap $gx))
Check ($script:exit -eq 0 -and $dX.Count -eq 0) "ASI loader + other proxies: uninstall byte for byte ($($dX -join ', '))"

# 3d. (ual-rev2) another mod's dinput8.dll put OVER the co-op one after the install (the player installed an ASI mod later). Up to 0.2.0's first build the
#     update overwrote it without a copy and the uninstaller deleted it (in Revelations 1 up to 0.0.2).
#     a) uninstall then: it stays in place (the player's now); nothing else of co-op is left; the result says so
$go = New-Game "over coop a"
$cleanO = Snap $go
Run-PS (Join-Path $x1 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$go`"", "-ShortcutDir", "`"$(Join-Path $base 'links over')`"", "-Lang", "en")
New-FakePe (Join-Path $go "dinput8.dll") $ualExports 0x14C "a loader the player put over co-op"
$f1 = Sha (Join-Path $go "dinput8.dll")
Run-PS (Join-Path $go "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$go`"", "-Lang", "en")
$dO = @(Snap-Diff $cleanO (Snap $go))
Check ($script:exit -eq 0 -and (Sha0 (Join-Path $go "dinput8.dll")) -eq $f1) "over co-op's DLL, then uninstall: the other mod's dinput8.dll stays in place"
Check (($dO -join ",") -eq "dinput8.dll") "over co-op's DLL, then uninstall: nothing else of co-op is left ($($dO -join ', '))"
Check ($script:TEXTS["en"]["leftnote"] -and $script:out.Contains($script:TEXTS["en"]["leftnote"])) "over co-op's DLL, then uninstall: the result says it stays"
#     b) an update then (the shortcut's launcher found a newer release): kept in coop-backup as the original, co-op's DLL back, the loader runs on as
#        winmm.dll; the uninstall afterwards puts it back as dinput8.dll and leaves nothing of co-op
$go = New-Game "over coop b"
$cleanO = Snap $go
Run-PS (Join-Path $x1 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$go`"", "-ShortcutDir", "`"$(Join-Path $base 'links over')`"", "-Lang", "en")
New-FakePe (Join-Path $go "dinput8.dll") $ualExports 0x14C "a loader the player put over co-op"
Set-Content -LiteralPath (Join-Path $go "dinput8.ini") -Value "[GlobalSets]`r`nLoadPlugins=1" -Encoding ASCII
$f1 = Sha (Join-Path $go "dinput8.dll")
Run-PS (Join-Path $x2 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$go`"", "-NoShortcut", "-ShortcutDir", "`"$(Join-Path $base 'links over')`"", "-Lang", "en")
$recO = @(Get-Content (Join-Path $go "coop\install.txt"))
Check ($script:exit -eq 0 -and (Sha0 (Join-Path $go "coop-backup\dinput8.dll")) -eq $f1 -and ($recO -contains "backup=dinput8.dll")) "over co-op's DLL, then update: it is kept in coop-backup (not overwritten unseen)"
Check ((Sha0 (Join-Path $go "dinput8.dll")) -eq (Sha (Join-Path $x2 "files\dinput8.dll")) -and (Sha0 (Join-Path $go "winmm.dll")) -eq $f1 -and ($recO -contains "chain=winmm.dll")) "over co-op's DLL, then update: co-op's DLL back, the loader runs on as winmm.dll"
Check ($script:TEXTS["en"]["overcoop"] -and $script:out.Contains($script:TEXTS["en"]["overcoop"])) "over co-op's DLL, then update: the log says it is kept as the original"
Run-PS (Join-Path $go "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$go`"")
$dO = @(Snap-Diff $cleanO (Snap $go))
Check ($script:exit -eq 0 -and (Sha0 (Join-Path $go "dinput8.dll")) -eq $f1 -and -not (Test-Path -LiteralPath (Join-Path $go "winmm.dll")) -and (@($dO | Where-Object { $_ -notin "dinput8.dll", "dinput8.ini" }).Count -eq 0)) "over co-op's DLL, update, uninstall: the player's loader is back as dinput8.dll, nothing of co-op left ($($dO -join ', '))"
#     c) the same over a game that had a loader before co-op: the newer one wins, the copy of the older one is replaced by one of it
$go = New-UalGame "over coop c"
$cleanO = Snap $go
Run-PS (Join-Path $x1 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$go`"", "-ShortcutDir", "`"$(Join-Path $base 'links over')`"", "-Lang", "en")
New-FakePe (Join-Path $go "dinput8.dll") $ualExports 0x14C "a NEWER loader the player put over co-op"
$f2 = Sha (Join-Path $go "dinput8.dll")
Run-PS (Join-Path $x2 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$go`"", "-NoShortcut", "-ShortcutDir", "`"$(Join-Path $base 'links over')`"", "-Lang", "en")
Check ($script:exit -eq 0 -and (Sha0 (Join-Path $go "coop-backup\dinput8.dll")) -eq $f2 -and (Sha0 (Join-Path $go "winmm.dll")) -eq $f2 -and (Sha0 (Join-Path $go "dinput8.dll")) -eq (Sha (Join-Path $x2 "files\dinput8.dll"))) "a newer loader over co-op's DLL, then update: it is the original now and runs as winmm.dll"
Run-PS (Join-Path $go "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$go`"")
$dO = @(Snap-Diff $cleanO (Snap $go))
Check ($script:exit -eq 0 -and (Sha0 (Join-Path $go "dinput8.dll")) -eq $f2 -and -not (Test-Path -LiteralPath (Join-Path $go "winmm.dll")) -and ($dO -join ",") -eq "dinput8.dll") "a newer loader, update, uninstall: the newer loader stays as dinput8.dll, the rest as before co-op ($($dO -join ', '))"

# 4. the launcher's update from a local feed (no network). A small HTTP server on 127.0.0.1 answers by path as GitHub does or fails as a bad network does:
#    /ratelimit/ 403 with X-RateLimit-Remaining 0, a missing file 404, /stall-head/ never answers, /stall-body/ sends the headers and 64 bytes and stops,
#    /cut/ closes after half of the body, /redirect/ 302 to the file (github.com's download URL redirects to its asset host). Every failure keeps 0.0.1,
#    the game start line still comes, no update folder stays in %TEMP%.
$feedDir = Join-Path $base "feed"; New-Item -ItemType Directory -Force $feedDir | Out-Null
Copy-Item $zip2 $feedDir
$port = 20000 + ($PID % 20000)
$u = "http://127.0.0.1:$port"
$reqLog = Join-Path $base "requests.txt"
$serverScript = Join-Path $base "serve.ps1"
Set-Content -Path $serverScript -Encoding ASCII -Value @'
param($dir, $port, $reqLog)
$l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, [int]$port)
$l.Start()
$held = New-Object Collections.ArrayList
$enc = [Text.Encoding]::ASCII
function Send($s, [string] $head, [byte[]] $body, [int] $count) {
    $h = $enc.GetBytes($head + "Connection: close`r`n`r`n"); $s.Write($h, 0, $h.Length)
    if ($count -gt 0) { $s.Write($body, 0, $count) }
    $s.Flush()
}
while ($true) {
    $c = $l.AcceptTcpClient()
    $keep = $false
    try {
        $s = $c.GetStream(); $s.ReadTimeout = 5000
        $buf = New-Object byte[] 8192; $req = ""
        while ($req -notmatch "`r`n`r`n") { $n = $s.Read($buf, 0, $buf.Length); if ($n -le 0) { break }; $req += $enc.GetString($buf, 0, $n) }
        $path = ($req -split " ")[1]
        Add-Content -LiteralPath $reqLog -Value $path
        $name = ($path -split "/")[-1]
        $f = Join-Path $dir $name
        $b = if ($name -and (Test-Path -LiteralPath $f -PathType Leaf)) { [IO.File]::ReadAllBytes($f) } else { $null }
        $ok = "HTTP/1.1 200 OK`r`nContent-Type: application/octet-stream`r`nContent-Length: $(if ($b) { $b.Length } else { 0 })`r`n"
        if ($path -like "/stall-head/*") { $keep = $true }
        elseif ($path -like "/ratelimit/*") {
            $m = $enc.GetBytes('{"message":"API rate limit exceeded"}')
            Send $s "HTTP/1.1 403 rate limit exceeded`r`nX-RateLimit-Limit: 60`r`nX-RateLimit-Remaining: 0`r`nX-RateLimit-Reset: 2000000000`r`nContent-Length: $($m.Length)`r`n" $m $m.Length
        }
        elseif (-not $b) { Send $s "HTTP/1.1 404 Not Found`r`nContent-Length: 0`r`n" $null 0 }
        elseif ($path -like "/redirect/*") { Send $s "HTTP/1.1 302 Found`r`nLocation: http://127.0.0.1:$port/$name`r`nContent-Length: 0`r`n" $null 0 }
        elseif ($path -like "/cut/*") { Send $s $ok $b ([int][Math]::Floor($b.Length / 2)) }
        elseif ($path -like "/stall-body/*") { Send $s $ok $b ([Math]::Min(64, $b.Length)); $keep = $true }
        else { Send $s $ok $b $b.Length }
    } catch { }
    if ($keep) { [void]$held.Add($c) } else { $c.Close() }
}
'@
$srv = Start-Process powershell -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$serverScript`"", "`"$feedDir`"", $port, "`"$reqLog`"") -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
# A releases/latest answer in the real shape: tests\fixtures\github-release-v0.0.2.json is the v0.0.2 entry of api.github.com/repos/dolbaras/revelations-coop/releases
# (read 2026-11-09; author, uploader and body trimmed), given this test's tag, archive URL and digest ($null: none). -Bom writes a byte-order mark first.
$fixture = Get-Content -LiteralPath (Join-Path $PSScriptRoot "fixtures\github-release-v0.0.2.json") -Raw -Encoding UTF8
function Write-Feed($file, $url, $digest, [switch] $Pre, [switch] $Bom, $tag = "v0.0.2") {
    $r = $fixture | ConvertFrom-Json
    $r.tag_name = $tag; $r.name = "test $tag"; $r.prerelease = [bool]$Pre
    $a = @($r.assets | Where-Object { $_.name -like "*.zip" })[0]
    $a.name = "revelations-coop-$($tag.TrimStart('v')).zip"; $a.browser_download_url = $url; $a.digest = $digest
    [IO.File]::WriteAllText((Join-Path $feedDir $file), ($r | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($Bom.IsPresent)))
}
$launcher = Join-Path $g "coop\coop-launcher.ps1"
$llf = Join-Path $g "coop\launcher-log.txt"
function Temp-Left { @(Get-ChildItem -LiteralPath ([IO.Path]::GetTempPath()) -Directory -Filter "re1-coop-update-*" -ErrorAction SilentlyContinue).Count }
function Ver($dir = $g) { Get-Content -LiteralPath (Join-Path $dir "coop\version.txt") -TotalCount 1 }
# one launcher run as the shortcut makes it (minus the window and the game start): its new log lines, the paths it asked the server for, the time it took
function Launch($feed) {
    $n0 = @(Get-Content -LiteralPath $llf -ErrorAction SilentlyContinue).Count
    $r0 = @(Get-Content -LiteralPath $reqLog -ErrorAction SilentlyContinue).Count
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Run-PS $launcher @("-NoStart", "-NoWindow", "-Feed", $feed)
    $script:secs = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
    $script:llog = (@(Get-Content -LiteralPath $llf -Encoding UTF8 -ErrorAction SilentlyContinue) | Select-Object -Skip $n0) -join " | "
    $script:reqs = @(Get-Content -LiteralPath $reqLog -ErrorAction SilentlyContinue | Select-Object -Skip $r0)
}
$sha2 = Sha $zip2
$t0 = Temp-Left
$env:RE1_COOP_ANYEXE = "1"   # (the stand-in exe is not the supported one; the launcher starts the new installer without -AnyExe)
try {
    Launch "$u/ratelimit/latest.json"
    Check ((Ver) -eq "0.0.1" -and $script:exit -eq 0 -and $script:llog -match "rate limit is used up \(resets \d\d:\d\d\) \[HTTP 403\]" -and $script:llog -match "start: ") "launcher: GitHub's rate limit (403, X-RateLimit-Remaining 0) is named in the log and the game starts ($($script:llog))"
    Launch "$u/latest.json"
    Check ((Ver) -eq "0.0.1" -and $script:llog -match "\[HTTP 404\]" -and $script:llog -match "start: ") "launcher: no release (404): 0.0.1 stays, the game starts"
    Launch "$u/stall-head/latest.json"
    Check ((Ver) -eq "0.0.1" -and $script:llog -match "did not answer in 6 seconds" -and $script:secs -lt 11) "launcher: a feed that never answers is given up after 6 s ($($script:secs) s for the whole run)"
    Write-Feed "latest.json" "$u/revelations-coop-0.0.2.zip" "sha256:$sha2"
    Launch "$u/stall-body/latest.json"
    Check ((Ver) -eq "0.0.1" -and $script:llog -match "update: " -and $script:secs -lt 15) "launcher: a feed that stops after its headers is given up ($($script:secs) s for the whole run; $($script:llog))"
    Write-Feed "nodigest.json" "$u/revelations-coop-0.0.2.zip" $null
    Launch "$u/nodigest.json"
    Check ((Ver) -eq "0.0.1" -and $script:llog -match "refused: the release gives no usable checksum" -and -not ($script:reqs -match "\.zip")) "launcher: a release without a sha256 digest is not installed, not even downloaded"
    Write-Feed "pre.json" "$u/revelations-coop-0.0.2.zip" "sha256:$sha2" -Pre
    Launch "$u/pre.json"
    Check ((Ver) -eq "0.0.1" -and $script:llog -match "refused: the release is a pre-release" -and -not ($script:reqs -match "\.zip")) "launcher: a pre-release in the feed is not installed"
    Write-Feed "bad.json" "$u/revelations-coop-0.0.2.zip" ("sha256:" + "0" * 64)
    Launch "$u/bad.json"
    Check ((Ver) -eq "0.0.1" -and ($script:reqs -match "\.zip") -and $script:llog -match "update: " -and $script:llog -notmatch "checksum ok") "launcher: a wrong checksum is refused after the download, version stays 0.0.1"
    Write-Feed "cut.json" "$u/cut/revelations-coop-0.0.2.zip" "sha256:$sha2"
    Launch "$u/cut.json"
    Check ((Ver) -eq "0.0.1" -and $script:llog -match "update: " -and $script:llog -notmatch "checksum ok") "launcher: a download that breaks off is refused ($($script:llog))"
    Write-Feed "gone.json" "$u/revelations-coop-9.9.9.zip" "sha256:$sha2"
    Launch "$u/gone.json"
    Check ((Ver) -eq "0.0.1" -and $script:llog -match "\[HTTP 404\]") "launcher: a missing archive (404) is refused"
    Check ((Temp-Left) -eq $t0) "launcher: no update folder is left in %TEMP% after the failures"
    # this folder's game runs (a stand-in rerev.exe that waits): nothing is asked, nothing started. Skipped while any rerev runs on this PC (a lab instance must not
    # see a second one).
    if (@(Get-Process -Name rerev -ErrorAction SilentlyContinue).Count -eq 0) {
        $runDir = Join-Path $g "coop-test-running"; New-Item -ItemType Directory -Force $runDir | Out-Null
        $fakeExe = Join-Path $runDir "rerev.exe"
        Add-Type -OutputType ConsoleApplication -OutputAssembly $fakeExe -TypeDefinition 'public static class StandIn { public static void Main() { System.Threading.Thread.Sleep(20000); } }'
        $fp = Start-Process -FilePath $fakeExe -PassThru -WindowStyle Hidden
        try { Launch "$u/latest.json" } finally { Stop-Process -Id $fp.Id -Force -ErrorAction SilentlyContinue; $fp.WaitForExit(5000) | Out-Null }
        Remove-Item -LiteralPath $runDir -Recurse -Force
        Check ($script:exit -eq 0 -and $script:llog -match "the game runs already" -and $script:reqs.Count -eq 0 -and (Ver) -eq "0.0.1") "launcher: with this folder's game running it asks nothing and starts nothing ($($script:llog))"
    } else { Write-Host "skip  the game-running case: a rerev.exe runs on this PC" }
    # a second launcher (a double-clicked shortcut): the mutex is taken, it does nothing
    $mx = New-Object Threading.Mutex($false, "Local\Re1CoopLauncher")
    if ($mx.WaitOne(0)) {
        try { Launch "$u/latest.json" } finally { $mx.ReleaseMutex() }
        Check ($script:exit -eq 0 -and $script:llog -match "another launcher runs" -and $script:reqs.Count -eq 0 -and (Ver) -eq "0.0.1") "launcher: a second launcher does nothing"
    } else { Write-Host "skip  the second-launcher case: a launcher of this PC holds the mutex" }
    $mx.Dispose()
    # the update itself: a feed with a byte-order mark, the archive through a redirect
    Write-Feed "latest.json" "$u/redirect/revelations-coop-0.0.2.zip" "sha256:$sha2" -Bom
    Launch "$u/latest.json"
    Check ((Ver) -eq "0.0.2" -and $script:llog -match "checksum ok $sha2" -and $script:llog -match "downloaded \d+ bytes from http://127\.0\.0\.1" -and ($script:reqs -match "^/redirect/")) "launcher: updated to 0.0.2 through a redirect ($($script:llog))"
    Check ((Temp-Left) -eq $t0) "launcher: the update folder is removed after the update"
    Check ((Get-Content (Join-Path $g "coop\install.txt")) -contains "version=0.0.2") "record: version=0.0.2 after the update"
    Check ((Get-Content (Join-Path $g "coop-backup\dinput8.dll") -Raw) -eq "ASI LOADER of somebody else") "update keeps the foreign DLL as the backup"
    Check (@((Get-Content (Join-Path $g "coop\install.txt")) | Where-Object { $_ -like "shortcut=*" }).Count -eq 2) "update keeps the two shortcuts in the record"
    Check ((Test-Path (Join-Path ($links -split ";")[0] "Resident Evil Revelations Co-op.lnk"))) "update keeps the shortcut files"
    Launch "$u/latest.json"
    Check ($script:exit -eq 0 -and $script:llog -match "installed 0\.0\.2, latest v0\.0\.2" -and -not ($script:reqs -match "\.zip")) "launcher with nothing new: exit 0, nothing downloaded"

    # 4b. -OldZip <a released zip, e.g. work\dist-pre2c\revelations-coop-0.0.2.zip>: that release installed into its own stand-in game, then ITS launcher (the one the
    #     players have) updates it to a 0.1.0 package of this tree through the local feed; the record, the collector files and the uninstall of the result.
    if ($OldZip) {
        $ox = Join-Path $base "old"; [IO.Compression.ZipFile]::ExtractToDirectory($OldZip, $ox)
        $oldVer = Get-Content (Join-Path $ox "files\coop\version.txt") -TotalCount 1
        $go = New-Game "old release" -WithForeignDll
        $cleanO = Snap $go
        Run-PS (Join-Path $ox "coop-installer.ps1") @("-Quiet", "-Game", "`"$go`"", "-ShortcutDir", "`"$(Join-Path $base 'links old')`"")
        Check ($script:exit -eq 0 -and (Ver $go) -eq $oldVer) "old release $oldVer installed from $([IO.Path]::GetFileName($OldZip))"
        $distN = Join-Path $base "dist-next"; Package "0.1.0" $distN
        $zipN = Join-Path $distN "revelations-coop-0.1.0.zip"; Copy-Item $zipN $feedDir
        Write-Feed "next.json" "$u/redirect/revelations-coop-0.1.0.zip" ("sha256:" + (Sha $zipN)) -tag "v0.1.0"
        Run-PS (Join-Path $go "coop\coop-launcher.ps1") @("-NoStart", "-NoWindow", "-Feed", "$u/next.json")
        $orec = Get-Content (Join-Path $go "coop\install.txt")
        $olog = (Get-Content (Join-Path $go "coop\launcher-log.txt") -Encoding UTF8) -join " | "
        Check ((Ver $go) -eq "0.1.0" -and ($orec -contains "version=0.1.0") -and ($orec -contains "file=coop\collect-logs.ps1")) "the $oldVer launcher updates to 0.1.0: record and the collector files ($olog)"
        Check ((Sha (Join-Path $go "coop\coop-launcher.ps1")) -eq (Sha (Join-Path $tools "launcher.ps1"))) "after the $oldVer -> 0.1.0 update the game folder has this tree's launcher"
        Check ((Get-Content (Join-Path $go "coop-backup\dinput8.dll") -Raw) -eq "ASI LOADER of somebody else") "the $oldVer -> 0.1.0 update keeps the foreign DLL as the backup"
        Run-PS (Join-Path $go "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$go`"")
        Check ($script:exit -eq 0 -and (Same $cleanO (Snap $go))) "uninstall after the $oldVer -> 0.1.0 update: the game folder is the clean one"
        # a player of the old release with an Ultimate ASI Loader (kept off in coop-backup by that release): the update by the shortcut makes it run on as winmm.dll
        $goa = New-UalGame "old release asi"
        $cleanOA = Snap $goa
        Run-PS (Join-Path $ox "coop-installer.ps1") @("-Quiet", "-Game", "`"$goa`"", "-ShortcutDir", "`"$(Join-Path $base 'links old asi')`"")
        # (a release from 0.1.0 on makes the copy itself; the 0.0.x ones only kept the loader)
        $oldChains = [version]$oldVer -ge [version]"0.1.0"
        Check ($script:exit -eq 0 -and (Test-Path -LiteralPath (Join-Path $goa "winmm.dll")) -eq $oldChains -and (Sha (Join-Path $goa "coop-backup\dinput8.dll")) -eq $cleanOA["dinput8.dll"]) "old release $oldVer over an ASI loader: kept in coop-backup, winmm.dll made: $oldChains (as released)"
        Run-PS (Join-Path $goa "coop\coop-launcher.ps1") @("-NoStart", "-NoWindow", "-Feed", "$u/next.json")
        $oarec = @(Get-Content (Join-Path $goa "coop\install.txt"))
        Check ((Ver $goa) -eq "0.1.0" -and (Sha (Join-Path $goa "winmm.dll")) -eq $cleanOA["dinput8.dll"] -and ($oarec -contains "chain=winmm.dll") -and ($oarec -contains "file=winmm.dll")) "the $oldVer launcher's update to 0.1.0 makes the kept ASI loader run on as winmm.dll"
        Run-PS (Join-Path $goa "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$goa`"")
        Check ($script:exit -eq 0 -and (Same $cleanOA (Snap $goa))) "uninstall after that update: the folder is byte for byte the one with the loader as dinput8.dll"
    }

    # 4c. -Online (the real GitHub, read only: four API reads and ONE download of the published v0.0.2 zip, ~315 KB): releases/latest of this repository (404 while
    #     only pre-releases exist, else the protocol's shape) and of the two sister mods with full releases (jericho-coop, timeshift-coop: same protocol); then this
    #     tree's launcher, fed by a local copy of the real v0.0.2 entry marked as a full release, installs the REAL asset over a stand-in 0.0.1: TLS 1.2,
    #     github.com's redirect to its asset host, GitHub's digest.
    if ($Online) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        function Get-Api($path) {
            $rq = [Net.HttpWebRequest]::Create("https://api.github.com/repos/dolbaras/$path"); $rq.UserAgent = "re1-coop-test"; $rq.Accept = "application/vnd.github+json"; $rq.Timeout = 15000
            try { $rs = $rq.GetResponse(); try { return @{ code = 200; json = (New-Object IO.StreamReader($rs.GetResponseStream())).ReadToEnd() } } finally { $rs.Close() } }
            catch { $we = $_.Exception; while ($we -and -not ($we -is [Net.WebException])) { $we = $we.InnerException }; if ($we -and $we.Response) { return @{ code = [int]$we.Response.StatusCode; json = "" } }; throw }
        }
        function Shape-Ok($rel, $repo) {
            $z = @($rel.assets | Where-Object { $_.name -like "$repo-*.zip" })[0]
            ($rel.tag_name -match '^v\d+\.\d+\.\d+$') -and -not $rel.prerelease -and -not $rel.draft -and $z -and ("$($z.digest)" -match '^sha256:[0-9a-f]{64}$') -and
                ("$($z.browser_download_url)" -eq "https://github.com/dolbaras/$repo/releases/download/$($rel.tag_name)/$($z.name)") -and ($z.state -eq "uploaded")
        }
        $mine = Get-Api "revelations-coop/releases/latest"
        if ($mine.code -eq 404) { Check $true "online: revelations-coop releases/latest answers 404 (no full release yet: the shortcut installs nothing, as intended)" }
        else { Check ($mine.code -eq 200 -and (Shape-Ok ($mine.json | ConvertFrom-Json) "revelations-coop")) "online: revelations-coop releases/latest has the protocol's shape" }
        foreach ($sis in "jericho-coop", "timeshift-coop") {
            $r = Get-Api "$sis/releases/latest"
            Check ($r.code -eq 200 -and (Shape-Ok ($r.json | ConvertFrom-Json) $sis)) "online: $sis releases/latest is a full release with a sha256 digest on its zip"
        }
        $t = Get-Api "revelations-coop/releases/tags/v0.0.2"
        if ($t.code -eq 200) {
            $rel = $t.json | ConvertFrom-Json; $rel.prerelease = $false
            [IO.File]::WriteAllText((Join-Path $feedDir "real.json"), ($rel | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
            $gr = New-Game "online"
            $cleanR = Snap $gr
            $distR = Join-Path $base "dist-001"; Package "0.0.1" $distR
            $xr = Join-Path $base "x001"; [IO.Compression.ZipFile]::ExtractToDirectory((Join-Path $distR "revelations-coop-0.0.1.zip"), $xr)
            Run-PS (Join-Path $xr "coop-installer.ps1") @("-Quiet", "-Game", "`"$gr`"", "-ShortcutDir", "`"$(Join-Path $base 'links online')`"")
            Run-PS (Join-Path $gr "coop\coop-launcher.ps1") @("-NoStart", "-NoWindow", "-Feed", "$u/real.json")
            $rl = (Get-Content -LiteralPath (Join-Path $gr "coop\launcher-log.txt") -Encoding UTF8 -ErrorAction SilentlyContinue) -join " | "
            $want = "$(@($rel.assets | Where-Object { $_.name -like 'revelations-coop-*.zip' })[0].digest)".Substring(7).ToUpper()
            Check ((Ver $gr) -eq "0.0.2" -and $rl -match "checksum ok $want" -and $rl -match "downloaded \d+ bytes from https://(?!github\.com)") "online: the real v0.0.2 zip came over TLS through github.com's redirect, GitHub's digest matched, installed ($rl)"
            Run-PS (Join-Path $gr "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$gr`"")
            Check ($script:exit -eq 0 -and (Same $cleanR (Snap $gr))) "online: uninstall of the real v0.0.2 leaves the clean folder"
        } else { Check $false "online: the v0.0.2 release is readable (HTTP $($t.code))" }
    }
} finally {
    if ($srv -and -not $srv.HasExited) { Stop-Process -Id $srv.Id -Force }
    Remove-Item Env:\RE1_COOP_ANYEXE -ErrorAction SilentlyContinue
}

# 5. uninstall (the installed copy, as "Uninstall Co-op.bat" runs it): the game is as before
Run-PS (Join-Path $g "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$g`"")
Check ($script:exit -eq 0) "uninstall: exit 0"
Check (Same $clean (Snap $g)) "after the uninstall the game folder is byte for byte the clean one (foreign dinput8.dll back)"
foreach ($d in ($links -split ";")) { Check (-not (Test-Path -LiteralPath (Join-Path $d "Resident Evil Revelations Co-op.lnk"))) "shortcut removed from $(Split-Path $d -Leaf)" }
Run-PS (Join-Path $x2 "coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$g`"")
Check ($script:exit -eq 1) "uninstall of a game without the mod fails with exit 1"

# 6. a game without any dinput8.dll: install and uninstall leave nothing
$g2 = New-Game "game two"
$clean2 = Snap $g2
Run-PS (Join-Path $x2 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$g2`"", "-ShortcutDir", "`"$(Join-Path $base 'links three')`"")
Check (($script:exit -eq 0) -and -not (Test-Path (Join-Path $g2 "coop-backup"))) "install into a game without dinput8.dll: no backup folder"
# (what game starts with the mod leave next to rerev.exe: the log, older logs, an empty screenshot folder)
foreach ($n in "rev1_coop.log", "rev1_coop.1.log", "rev1_coop.4.log") { Set-Content -LiteralPath (Join-Path $g2 $n) -Value "log" }
New-Item -ItemType Directory -Force (Join-Path $g2 "rev1_coop\shots") | Out-Null
Run-PS (Join-Path $g2 "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$g2`"")
Check (($script:exit -eq 0) -and (Same $clean2 (Snap $g2))) "uninstall: the game folder is the clean one again (the mod's logs and the empty rev1_coop\ gone too)"

# 7. a game folder with spaces and an apostrophe, found by the Find-Games candidates is out of scope here; the path itself must work
$g3 = New-Game "Resident Evil's Revelations (test)"
Run-PS (Join-Path $x1 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$g3`"", "-ShortcutDir", "`"$(Join-Path $base 'links four')`"")
Check ($script:exit -eq 0) "install into a folder with spaces and an apostrophe"
# (a player's screenshot and a file of someone else's that only looks like the log stay)
New-Item -ItemType Directory -Force (Join-Path $g3 "rev1_coop\shots") | Out-Null
Set-Content -LiteralPath (Join-Path $g3 "rev1_coop\shots\shot0001_012.bmp") -Value "bmp"
Set-Content -LiteralPath (Join-Path $g3 "rev1_coop.notes.log") -Value "mine"
Set-Content -LiteralPath (Join-Path $g3 "rev1_coop.log") -Value "log"
Run-PS (Join-Path $g3 "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$g3`"")
Check ($script:exit -eq 0) "uninstall from it"
Check ((Test-Path -LiteralPath (Join-Path $g3 "rev1_coop\shots\shot0001_012.bmp")) -and (Test-Path -LiteralPath (Join-Path $g3 "rev1_coop.notes.log")) -and
       -not (Test-Path -LiteralPath (Join-Path $g3 "rev1_coop.log"))) "uninstall keeps a screenshot and rev1_coop.notes.log, removes rev1_coop.log"

# 8. the real exe's hash is accepted (read only), a changed exe is refused
if ($RealGame) {
    $exe = Join-Path $RealGame "rerev.exe"
    Check ((Sha $exe) -eq "1573B79EB921B1E7F571E6DEB71C39961C7F1940FAD6D19283FC3AEA7FCA48AA") "the real rerev.exe has the supported hash (read only: $exe)"
    # a stand-in folder holding a COPY of the real exe: the hash check passes without -AnyExe, the icon is made from the exe's own icon, the shortcut uses it
    $g4 = Join-Path $base "real exe copy"
    New-Item -ItemType Directory -Force $g4 | Out-Null
    Copy-Item $exe (Join-Path $g4 "rerev.exe")
    Run-PS (Join-Path $x1 "coop-installer.ps1") @("-Quiet", "-Game", "`"$g4`"", "-ShortcutDir", "`"$(Join-Path $base 'links five')`"")
    Check ($script:exit -eq 0) "the real exe passes the hash check without -AnyExe (exit $($script:exit))"
    $ico = Join-Path $g4 "coop\coop.ico"
    Check ((Test-Path $ico) -and (Get-Item $ico).Length -gt 1000) "coop.ico made from the game's icon ($((Get-Item $ico -ErrorAction SilentlyContinue).Length) bytes)"
    $sc = $shell.CreateShortcut((Join-Path $base "links five\Resident Evil Revelations Co-op.lnk"))
    Check ($sc.IconLocation -like "*coop.ico*") "the shortcut uses coop.ico"
    Run-PS (Join-Path $g4 "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$g4`"")
    Check (($script:exit -eq 0) -and (@(Get-ChildItem $g4 -Force).Count -eq 1)) "uninstall leaves only rerev.exe"
}

# 8b. the window's code without showing it (RE1_COOP_UI_SELFTEST): page 1 for every language over six folder states
$ui0 = Join-Path $base "ui empty"; New-Item -ItemType Directory -Force $ui0 | Out-Null                         # a folder without rerev.exe
$ui1 = New-Game "ui fake exe"                                                                                      # a fake exe: refused
# (with -RealGame the three "good" folders hold a copy of the real exe and pass the hash check; without it RE1_COOP_ANYEXE stands in and the fake-exe state is skipped)
$real = [bool]$RealGame
$ui2 = if ($real) { New-Game "ui good" -RealExe } else { New-Game "ui good" }                                    # accepted and not installed
$ui3 = if ($real) { New-Game "ui installed" -RealExe } else { New-Game "ui installed" }
if (-not $real) { $env:RE1_COOP_ANYEXE = "1" }
Run-PS (Join-Path $x2 "coop-installer.ps1") @("-Quiet", "-Game", "`"$ui3`"", "-ShortcutDir", "`"$(Join-Path $base 'links six')`"")      # installed 0.0.2: the same version as this installer ("reinstall")
$ui4 = if ($real) { New-Game "ui older" -RealExe } else { New-Game "ui older" }
Run-PS (Join-Path $x1 "coop-installer.ps1") @("-Quiet", "-Game", "`"$ui4`"", "-ShortcutDir", "`"$(Join-Path $base 'links seven')`"")      # installed 0.0.1 (the installer under test has 0.0.2: "update")
$env:RE1_COOP_UI_SELFTEST = (@("", $ui0, $ui1, $ui2, $ui3, $ui4) -join ";")
$o = Join-Path $base "ui-out.txt"
$p = Start-Process powershell -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$(Join-Path $x2 'coop-installer.ps1')`"") -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput $o
Remove-Item Env:\RE1_COOP_UI_SELFTEST, Env:\RE1_COOP_ANYEXE -ErrorAction SilentlyContinue
$uiLines = @(Get-Content -LiteralPath $o -Encoding UTF8 | Where-Object { $_ -like "UI|*" })
Check ($p.ExitCode -eq 0 -and $uiLines.Count -eq 13 * 6) "the window code runs for 13 languages x 6 folder states ($($uiLines.Count) lines, exit $($p.ExitCode))"
. (Join-Path $tools "coop-texts.ps1")
$rawKeys = @($uiLines | Where-Object { $segs = @($_.Split("|") + ($_ -split ' // ')) | ForEach-Object { $_.Trim() }; @($segs | Where-Object { @($script:TEXTS["en"].Keys) -ccontains $_ }).Count -gt 0 })
Check ($rawKeys.Count -eq 0) "no raw text key shows in the window ($($rawKeys.Count))"
$st = @{}
foreach ($l in $uiLines) { $f = $l.Split("|"); $st["$($f[1])|$([IO.Path]::GetFileName($f[2]))"] = $f }
foreach ($lg in "en", "ru", "ja", "nl", "zh-tw", "pt-br") {
    Check ($st["$lg|ui good"][6] -eq "True" -and $st["$lg|ui good"][5] -eq "0") "ui $lg : a good folder enables Install, no Remove"
    if ($real) { Check ($st["$lg|ui fake exe"][6] -eq "False") "ui $lg : a fake exe disables Install" }
    Check ($st["$lg|ui empty"][6] -eq "False") "ui $lg : a folder without rerev.exe disables Install"
    Check ($st["$lg|ui installed"][5] -eq "1" -and $st["$lg|ui installed"][4] -ne $st["$lg|ui good"][4]) "ui $lg : an installed folder offers Remove and a different button (Reinstall)"
    Check ($st["$lg|ui older"][4] -ne $st["$lg|ui installed"][4] -and $st["$lg|ui older"][4] -ne $st["$lg|ui good"][4]) "ui $lg : an older install offers Update"
}
Check ($st["en|ui good"][4] -eq "Install" -and $st["en|ui installed"][4] -eq "Reinstall" -and $st["en|ui older"][4] -eq "Update") "ui en : Install / Reinstall / Update"
Check ($st["ja|ui good"][3] -match "rerev|Resident Evil") "ui ja : the info text is in Japanese and names the game"
foreach ($lg in $script:LANGS.Keys) { Check ($st["$lg|ui good"][3] -match "28410") "ui $lg : the window text names the network port 28410" }

# 9. texts: 13 languages, the same keys and placeholders as English, nothing empty, translated
. (Join-Path $tools "coop-texts.ps1")
$en = $script:TEXTS["en"]
function Ph($x) { (([regex]::Matches($x, "\{\d\}") | ForEach-Object { $_.Value } | Sort-Object) -join "") }
Check ($script:TEXTS.Count -eq 13 -and $script:LANGS.Count -eq 13) "13 languages"
foreach ($lg in $script:LANGS.Keys) {
    $t = $script:TEXTS[$lg]
    $missing = @($en.Keys | Where-Object { -not $t.ContainsKey($_) -or -not $t[$_] })
    $extra = @($t.Keys | Where-Object { -not $en.ContainsKey($_) })
    Check ($missing.Count -eq 0 -and $extra.Count -eq 0) "texts $lg : same $($en.Count) keys (missing: $($missing -join ','), extra: $($extra -join ','))"
    $bad = @($en.Keys | Where-Object { (Ph $t[$_]) -ne (Ph $en[$_]) })
    Check ($bad.Count -eq 0) "texts $lg : placeholders as in English ($($bad -join ','))"
    if ($lg -ne "en") {
        $same = @($en.Keys | Where-Object { $t[$_] -eq $en[$_] -and $_ -notin "found", "lnkdesc", "err" })
        Check ($same.Count -eq 0) "texts $lg : translated ($($same -join ','))"
    }
}
# the result page's text with the note about another mod's dinput8.dll fits its label (540 x 160 at 96 DPI, Segoe UI 10.5; measured without a window)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$resFont = New-Object Drawing.Font("Segoe UI", 10.5)
$tall = @()
foreach ($lg in $script:LANGS.Keys) {
    foreach ($k in "asinote", "offnote") {
        $txt = ($script:TEXTS[$lg]["done"] + "||||" + $script:TEXTS[$lg][$k]).Replace("||", "`n")
        $hgt = [Windows.Forms.TextRenderer]::MeasureText($txt, $resFont, (New-Object Drawing.Size(540, 2000)), [Windows.Forms.TextFormatFlags]::WordBreak).Height
        if ($hgt -gt 160) { $tall += "$lg/$k $hgt px" }
    }
    # (ual-rev2) the removal's result with the note about another mod's dinput8.dll left in place
    $txt = ($script:TEXTS[$lg]["gone"] + "||||" + $script:TEXTS[$lg]["leftnote"]).Replace("||", "`n")
    $hgt = [Windows.Forms.TextRenderer]::MeasureText($txt, $resFont, (New-Object Drawing.Size(540, 2000)), [Windows.Forms.TextFormatFlags]::WordBreak).Height
    if ($hgt -gt 160) { $tall += "$lg/leftnote $hgt px" }
}
Check ($tall.Count -eq 0) "the result text with the dinput8.dll note fits its 160-px label in every language ($($tall -join ', '))"
foreach ($pair in @(@("ru-RU", "ru"), @("de-DE", "de"), @("pt-BR", "pt-br"), @("pt-PT", "pt"), @("zh-CN", "zh-cn"), @("zh-Hans-CN", "zh-cn"), @("zh-TW", "zh-tw"), @("zh-HK", "zh-tw"), @("ja-JP", "ja"),
                    @("nl-BE", "nl"), @("pl-PL", "pl"), @("es-MX", "es"), @("it-IT", "it"), @("fr-CA", "fr"), @("en-GB", "en"), @("sv-SE", "en"), @("ptB", "pt-br"), @("chT", "zh-tw"), @("jpn", "ja"))) {
    Check ((Pick-Lang $pair[0]) -eq $pair[1]) "Pick-Lang $($pair[0]) -> $($pair[1])"
}

# 10. what the package must not carry or set (offline checks of the shipped scripts and of the shortcut)
$shipped = @("coop-installer.ps1", "coop-texts.ps1") | ForEach-Object { Get-Content -LiteralPath (Join-Path $x2 $_) -Raw -Encoding UTF8 }
$shipped += Get-Content -LiteralPath (Join-Path $x2 "files\coop\coop-launcher.ps1") -Raw -Encoding UTF8
$labVars = @($shipped | ForEach-Object { [regex]::Matches($_, '\$env:(REV1_[A-Z0-9_]+)\s*=') | ForEach-Object { $_.Groups[1].Value } })
Check ($labVars.Count -eq 0) "no shipped script sets a REV1_* variable ($($labVars -join ','))"
Check (@($shipped | Where-Object { $_ -match 'REV1_ACHBLOCK|REV1_CONSOLE|REV1_ANYEXE|REV1_WORLD' }).Count -eq 0) "no shipped script names a lab switch (REV1_ACHBLOCK, REV1_CONSOLE, REV1_ANYEXE, REV1_WORLD)"
$launcherText = Get-Content -LiteralPath (Join-Path $x2 "files\coop\coop-launcher.ps1") -Raw -Encoding UTF8
Check ($launcherText.Contains("api.github.com/repos/dolbaras/revelations-coop/releases/latest") -and $launcherText.Contains("revelations-coop-*.zip") -and
       $launcherText.Contains("https://github.com/dolbaras/revelations-coop/releases/download/*")) "the launcher's feed: dolbaras/revelations-coop, asset revelations-coop-*.zip, download URL checked"
# the exe's SHA-256 in the installer is the one of the mod's own gate (native\r1.h R1_EXE_SHA256; the DLL's exe tag in the network hello is its first 8 bytes)
$r1h = Join-Path $root "native\r1.h"
if (Test-Path -LiteralPath $r1h) {
    $m = [regex]::Match((Get-Content -LiteralPath $r1h -Raw), '#define R1_EXE_SHA256 "([0-9a-f]{64})"')
    $ins = [regex]::Match((Get-Content -LiteralPath (Join-Path $x2 "coop-installer.ps1") -Raw -Encoding UTF8), '\$script:EXE_SHA = "([0-9A-F]{64})"')
    Check ($m.Success -and $ins.Success -and $m.Groups[1].Value.ToUpper() -eq $ins.Groups[1].Value) "the installer's exe hash is the mod's own gate (r1.h R1_EXE_SHA256)"
}

# 11. the launcher starts the game through Steam (steam://rungameid/222480) for a Steam folder, by the exe otherwise (no game is started: -NoStart)
$steamRoot = Join-Path $base "steam lib\steamapps"
$gs = Join-Path $steamRoot "common\RESIDENT EVIL REVELATIONS"
New-Item -ItemType Directory -Force $gs | Out-Null
[IO.File]::WriteAllText((Join-Path $gs "rerev.exe"), "MZ fake game exe steam")
[IO.File]::WriteAllText((Join-Path $steamRoot "appmanifest_222480.acf"), "`"AppState`"`n{`n`t`"appid`"`t`t`"222480`"`n`t`"installdir`"`t`t`"RESIDENT EVIL REVELATIONS`"`n}`n")
$env:RE1_COOP_ANYEXE = "1"
Run-PS (Join-Path $x2 "coop-installer.ps1") @("-Quiet", "-Game", "`"$gs`"", "-ShortcutDir", "`"$(Join-Path $base 'links steam')`"")
Remove-Item Env:\RE1_COOP_ANYEXE -ErrorAction SilentlyContinue
Check ($script:exit -eq 0) "install into a Steam library folder"
$lnkArgs = $shell.CreateShortcut((Join-Path $base "links steam\Resident Evil Revelations Co-op.lnk")).Arguments
Check ($lnkArgs -like "*coop-launcher.ps1*" -and $lnkArgs -notmatch "REV1_|rerev\.exe") "the shortcut runs the launcher only (no exe, no variable)"
Run-PS (Join-Path $gs "coop\coop-launcher.ps1") @("-NoUpdate", "-NoStart")
$slog = Get-Content -LiteralPath (Join-Path $gs "coop\launcher-log.txt") -Raw
Check ($slog -match "start: steam://rungameid/222480") "the launcher starts a Steam copy through Steam (rungameid/222480)"
$g5 = New-Game "plain copy"
$env:RE1_COOP_ANYEXE = "1"
Run-PS (Join-Path $x2 "coop-installer.ps1") @("-Quiet", "-Game", "`"$g5`"", "-ShortcutDir", "`"$(Join-Path $base 'links plain')`"")
Remove-Item Env:\RE1_COOP_ANYEXE -ErrorAction SilentlyContinue
Run-PS (Join-Path $g5 "coop\coop-launcher.ps1") @("-NoUpdate", "-NoStart")
Check ((Get-Content -LiteralPath (Join-Path $g5 "coop\launcher-log.txt") -Raw) -match "start: rerev\.exe") "a copy outside a Steam library: the launcher starts rerev.exe itself"
Run-PS (Join-Path $g5 "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$g5`"")
Run-PS (Join-Path $gs "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$gs`"")
Check ((@(Get-ChildItem $gs -Force | ForEach-Object Name) -join ",") -eq "rerev.exe") "uninstall from the Steam-library folder leaves only rerev.exe"

# 12. the player's own files are never touched: the renamed loader and the plugin folder next to a foreign dinput8.dll (the game 'g' above, checked by the snapshot)
Check ((Test-Path (Join-Path $g "dinput8_real.dll")) -and (Test-Path (Join-Path $g "scripts\widescreen.asi"))) "dinput8_real.dll and scripts\ of the player survive install, update and uninstall"

# 13. no Revelations 2 leftovers in any text (the game is "Resident Evil Revelations", the exe rerev.exe, the build 13298487)
foreach ($lg in $script:LANGS.Keys) {
    $joined = (@($script:TEXTS[$lg].Values) -join " ")
    $old = [regex]::Matches($joined, 'Revelations[ -]?2|rerev2|3658995|287290').Count
    Check ($old -eq 0) "texts $lg : names Revelations, rerev.exe and build 13298487 only"
    Check ($joined.Contains("13298487")) "texts $lg : the bad-exe text names the supported build"
}

# 14. the launcher's update window carries the network line (both ways, the port, the firewall); every language has it
$lsrc = Get-Content (Join-Path $tools "launcher.ps1") -Raw
Check ($lsrc -match 'Lt "net"') "launcher.ps1 shows the network line (l_net) in its update window"
foreach ($lg in $script:LANGS.Keys) {
    Check (($script:TEXTS[$lg]["l_net"] -match "28410") -and ($script:TEXTS[$lg]["net"] -match "28410") -and ($script:TEXTS[$lg]["net"] -match "Steam")) "texts $lg : net / l_net name UDP port 28410 and the Steam way"
}

if (-not $Keep) { Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue }
"$($script:checks - $script:fails) of $($script:checks) checks passed" + $(if ($Keep) { " (kept: $base)" } else { "" })
exit $(if ($script:fails) { 1 } else { 0 })
