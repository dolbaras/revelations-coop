# Offline tests of the installer, the uninstaller, the launcher's update and the texts (no game is started, no network, nothing outside a temp folder).
# A stand-in game folder (a tiny fake rerev.exe, -AnyExe) is made under %TEMP%; the supported exe's hash check is tested on a fake exe (refusal) and,
# when -RealGame <folder> is given, on that game's real rerev.exe READ-ONLY (never written).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tests\test_installer.ps1 [-RealGame <folder with the real rerev.exe>] [-Keep]
param([string] $RealGame = "", [switch] $Keep)
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
function Package($v) { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $pkgRoot "tools\package.ps1") -Version $v -Dist $dist | Out-Null; if ($LASTEXITCODE) { throw "package.ps1 failed" } }
Package "0.0.1"; Package "0.0.2"
$zip1 = Join-Path $dist "revelations-coop-0.0.1.zip"; $zip2 = Join-Path $dist "revelations-coop-0.0.2.zip"
Check ((Test-Path $zip1) -and (Test-Path $zip2)) "package.ps1 builds revelations-coop-<version>.zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$x1 = Join-Path $base "x1"; $x2 = Join-Path $base "x2"
[IO.Compression.ZipFile]::ExtractToDirectory($zip1, $x1); [IO.Compression.ZipFile]::ExtractToDirectory($zip2, $x2)
$names = (Get-ChildItem $x1 -File | ForEach-Object Name) -join ","
Check ($names -eq "coop-installer.ps1,coop-texts.ps1,Install Co-op.bat,LICENSE.txt,README-COOP.txt") "the zip root: $names"
$payload1 = @(Get-Content (Join-Path $x1 "files\coop\payload.txt"))
Check ((($payload1 -contains "dinput8.dll") -and ($payload1 -contains "coop\coop-launcher.ps1") -and ($payload1 -contains "coop\coop-installer.ps1") -and ($payload1 -contains "coop\coop-texts.ps1") -and ($payload1 -contains "coop\version.txt"))) "payload list names the DLL, launcher, installer, texts and version"
Check ((Get-Content (Join-Path $x2 "files\coop\version.txt") -TotalCount 1) -eq "0.0.2") "version.txt of the 0.0.2 package"

# ---- the stand-in game
function New-Game($name, [switch] $WithForeignDll, [switch] $RealExe) {
    $g = Join-Path $base $name
    New-Item -ItemType Directory -Force (Join-Path $g "nativePC\msg") | Out-Null
    if ($RealExe) { Copy-Item (Join-Path $RealGame "rerev.exe") (Join-Path $g "rerev.exe") } else { [IO.File]::WriteAllText((Join-Path $g "rerev.exe"), "MZ fake game exe $name") }
    [IO.File]::WriteAllText((Join-Path $g "steam_api.dll"), "steam api")
    [IO.File]::WriteAllText((Join-Path $g "nativePC\msg\Title_eng.gmd"), "texts")
    if ($WithForeignDll) { [IO.File]::WriteAllText((Join-Path $g "dinput8.dll"), "ASI LOADER of somebody else") }
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
foreach ($f in "coop\coop-launcher.ps1", "coop\coop-installer.ps1", "coop\coop-texts.ps1", "coop\version.txt", "coop\README.txt", "coop\install.txt", "Uninstall Co-op.bat") {
    Check (Test-Path -LiteralPath (Join-Path $g $f)) "installed: $f"
}
$rec = Get-Content (Join-Path $g "coop\install.txt")
Check ($rec -contains "version=0.0.1") "record: version=0.0.1"
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
Run-PS (Join-Path $x1 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$g`"", "-ShortcutDir", "`"$links`"")
Check ($script:exit -eq 0) "reinstall: exit 0"
Check ((Get-Content (Join-Path $g "coop-backup\dinput8.dll") -Raw) -eq "ASI LOADER of somebody else") "reinstall keeps the foreign DLL as the backup"
$rec = Get-Content (Join-Path $g "coop\install.txt")
Check (@($rec | Where-Object { $_ -eq "file=dinput8.dll" }).Count -eq 1) "reinstall: dinput8.dll listed once"
Check (@($rec | Where-Object { $_ -eq "backup=dinput8.dll" }).Count -eq 1) "reinstall: one backup line"

# 4. the launcher updates 0.0.1 -> 0.0.2 from a local feed (no network); a wrong checksum is refused
$feedDir = Join-Path $base "feed"; New-Item -ItemType Directory -Force $feedDir | Out-Null
Copy-Item $zip2 $feedDir
$port = 20000 + ($PID % 20000)
$listenerScript = Join-Path $base "serve.ps1"
Set-Content -Path $listenerScript -Value @'
param($dir, $port)
$l = New-Object System.Net.HttpListener
$l.Prefixes.Add("http://127.0.0.1:$port/")
$l.Start()
while ($l.IsListening) {
    $c = $l.GetContext()
    $f = Join-Path $dir ($c.Request.Url.AbsolutePath.TrimStart('/'))
    if (Test-Path -LiteralPath $f -PathType Leaf) {
        $b = [IO.File]::ReadAllBytes($f); $c.Response.ContentLength64 = $b.Length; $c.Response.OutputStream.Write($b, 0, $b.Length)
    } else { $c.Response.StatusCode = 404 }
    $c.Response.Close()
}
'@
$srv = Start-Process powershell -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$listenerScript`"", "`"$feedDir`"", $port) -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
function Write-Feed($digest) {
    $a = @{ name = "revelations-coop-0.0.2.zip"; browser_download_url = "http://127.0.0.1:$port/revelations-coop-0.0.2.zip" }
    if ($digest) { $a.digest = "sha256:" + $digest }
    @{ tag_name = "v0.0.2"; assets = @($a) } | ConvertTo-Json -Depth 5 | Set-Content -Path (Join-Path $feedDir "latest.json") -Encoding UTF8
}
$env:RE1_COOP_ANYEXE = "1"   # (the stand-in exe is not the supported one; the launcher starts the new installer without -AnyExe)
try {
    $launcher = Join-Path $g "coop\coop-launcher.ps1"
    Write-Feed ("0" * 64)
    Run-PS $launcher @("-NoStart", "-NoWindow", "-Feed", "http://127.0.0.1:$port/latest.json")
    Check ((Get-Content (Join-Path $g "coop\version.txt") -TotalCount 1) -eq "0.0.1") "launcher: a wrong checksum is refused, version stays 0.0.1"
    Write-Feed (Sha $zip2)
    Run-PS $launcher @("-NoStart", "-NoWindow", "-Feed", "http://127.0.0.1:$port/latest.json")
    $log = Get-Content (Join-Path $g "coop\launcher-log.txt") -Raw
    Check ((Get-Content (Join-Path $g "coop\version.txt") -TotalCount 1) -eq "0.0.2") "launcher: updated to 0.0.2 ($($log.Split([char]10)[-3].Trim()))"
    Check ((Get-Content (Join-Path $g "coop\install.txt")) -contains "version=0.0.2") "record: version=0.0.2 after the update"
    Check ((Get-Content (Join-Path $g "coop-backup\dinput8.dll") -Raw) -eq "ASI LOADER of somebody else") "update keeps the foreign DLL as the backup"
    Check (@((Get-Content (Join-Path $g "coop\install.txt")) | Where-Object { $_ -like "shortcut=*" }).Count -eq 2) "update keeps the two shortcuts in the record"
    Check ((Test-Path (Join-Path ($links -split ";")[0] "Resident Evil Revelations Co-op.lnk"))) "update keeps the shortcut files"
    Run-PS $launcher @("-NoStart", "-NoWindow", "-Feed", "http://127.0.0.1:$port/latest.json")
    Check ($script:exit -eq 0) "launcher with nothing new: exit 0"
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
Run-PS (Join-Path $g2 "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$g2`"")
Check (($script:exit -eq 0) -and (Same $clean2 (Snap $g2))) "uninstall: the game folder is the clean one again"

# 7. a game folder with spaces and an apostrophe, found by the Find-Games candidates is out of scope here; the path itself must work
$g3 = New-Game "Resident Evil's Revelations (test)"
Run-PS (Join-Path $x1 "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$g3`"", "-ShortcutDir", "`"$(Join-Path $base 'links four')`"")
Check ($script:exit -eq 0) "install into a folder with spaces and an apostrophe"
Run-PS (Join-Path $g3 "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$g3`"")
Check ($script:exit -eq 0) "uninstall from it"

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
foreach ($lg in "en", "ru", "ja", "zh-tw", "pt-br") {
    Check ($st["$lg|ui good"][6] -eq "True" -and $st["$lg|ui good"][5] -eq "0") "ui $lg : a good folder enables Install, no Remove"
    if ($real) { Check ($st["$lg|ui fake exe"][6] -eq "False") "ui $lg : a fake exe disables Install" }
    Check ($st["$lg|ui empty"][6] -eq "False") "ui $lg : a folder without rerev.exe disables Install"
    Check ($st["$lg|ui installed"][5] -eq "1" -and $st["$lg|ui installed"][4] -ne $st["$lg|ui good"][4]) "ui $lg : an installed folder offers Remove and a different button (Reinstall)"
    Check ($st["$lg|ui older"][4] -ne $st["$lg|ui installed"][4] -and $st["$lg|ui older"][4] -ne $st["$lg|ui good"][4]) "ui $lg : an older install offers Update"
}
Check ($st["en|ui good"][4] -eq "Install" -and $st["en|ui installed"][4] -eq "Reinstall" -and $st["en|ui older"][4] -eq "Update") "ui en : Install / Reinstall / Update"
Check ($st["ja|ui good"][3] -match "rerev|Resident Evil") "ui ja : the info text is in Japanese and names the game"

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
foreach ($pair in @(@("ru-RU", "ru"), @("de-DE", "de"), @("pt-BR", "pt-br"), @("pt-PT", "pt"), @("zh-CN", "zh-cn"), @("zh-Hans-CN", "zh-cn"), @("zh-TW", "zh-tw"), @("zh-HK", "zh-tw"), @("ja-JP", "ja"),
                    @("nl-BE", "nl"), @("pl-PL", "pl"), @("es-MX", "es"), @("it-IT", "it"), @("fr-CA", "fr"), @("en-GB", "en"), @("sv-SE", "en"), @("ptB", "pt-br"), @("chT", "zh-tw"), @("jpn", "ja"))) {
    Check ((Pick-Lang $pair[0]) -eq $pair[1]) "Pick-Lang $($pair[0]) -> $($pair[1])"
}

if (-not $Keep) { Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue }
"$($script:checks - $script:fails) of $($script:checks) checks passed" + $(if ($Keep) { " (kept: $base)" } else { "" })
exit $(if ($script:fails) { 1 } else { 0 })
