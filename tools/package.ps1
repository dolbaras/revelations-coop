# Build the release archive dist\revelations-coop-<version>.zip:
#   Install Co-op.bat + coop-installer.ps1 + coop-texts.ps1   the installer (backs up what it replaces, uninstalls; 13 languages)
#   files\                                                   what goes into the game folder (dinput8.dll = the mod's proxy, coop\coop-launcher.ps1 behind the
#                                                            shortcut, coop\coop-installer.ps1 + coop-texts.ps1 for the uninstaller, coop\version.txt,
#                                                            coop\collect-logs.ps1 + coop\log-profiles\*.psd1 + coop\Collect logs.bat: the bug-report
#                                                            collector every co-op mod of the workspace shares, <workspace>\tools\log-collector)
#   README-COOP.txt, LICENSE.txt                           (docs\release-readme.txt with @VERSION@ filled in)
# Run native\build.sh first. The developer tools (console, test hooks) are inside the DLL but do nothing without their environment variables. The package sets no
# environment variable of its own and the shortcut's launcher sets none (REV1_ACHBLOCK and the other lab switches stay unset).
# Usage: package.ps1 -Version 0.1.0 [-Exe] [-Dist <folder>] [-Dll <dinput8.dll>] [-LogCollector <folder>] [-AnyDllVersion]
# -Dll: the DLL to ship (default native\build\dinput8.dll; `OUT=build-x/dinput8.dll sh native/build.sh` builds one elsewhere). Its R1_VERSION (native\r1.h, a string
# of its own in the DLL) must be -Version: coop\version.txt is what the launcher compares, the DLL's version is what the log says. -AnyDllVersion: skip that (tests,
# dry runs of a coming version).
# -LogCollector: the log collector's folder for a clone placed elsewhere (default <workspace>\tools\log-collector: the first tools\log-collector above the repository,
# three levels above mod\, four above a worktree in work\).
# -Exe also builds dist\revelations-coop-setup-<version>.exe: a single-file installer that carries this zip (an ADDITIONAL release asset; the zip stays the one the
# shortcut's launcher updates from: the launcher takes the first asset matching revelations-coop-*.zip). Made by <workspace>\tools\mkinstaller (-Mkinstaller <folder>
# for a clone placed elsewhere); the exe is unsigned. -Dist: write to that folder instead of dist\ (tests).
param([Parameter(Mandatory)] [string] $Version, [switch] $Exe, [string] $Mkinstaller = "", [string] $Dist = "", [string] $Dll = "", [string] $LogCollector = "",
      [switch] $AnyDllVersion)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
# The workspace's shared tools (<workspace>\tools\<name>): the first folder above the repository that has it (mod\ and work\wt-* both work).
function Workspace-Tool($name) {
    $d = $root
    for ($i = 0; $i -lt 6; $i++) {
        $d = Split-Path $d -Parent
        if (-not $d) { break }
        if (Test-Path -LiteralPath (Join-Path $d "tools\$name")) { return (Join-Path $d "tools\$name") }
    }
    Join-Path $root "..\..\..\tools\$name"
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "the version must be major.minor.patch (the launcher parses it as a .NET [version]): $Version" }
$dllFile = if ($Dll) { [IO.Path]::GetFullPath($Dll) } else { Join-Path $root "native\build\dinput8.dll" }
if (-not (Test-Path -LiteralPath $dllFile)) { throw "build $dllFile first (sh native/build.sh)" }
$dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dllFile))
# (the installer tells the mod's DLL from a dinput8.dll of someone else's by these words: a DLL without them would be taken for a foreign one)
if (-not $dllText.Contains("RE Revelations co-op proxy")) { throw "$dllFile is not the Revelations co-op proxy (its log line is missing)" }
# (R1_VERSION is a string of its own in the DLL: "\0<version>\0")
if (-not $AnyDllVersion -and -not $dllText.Contains("`0$Version`0")) { throw "$dllFile is not version $Version (set R1_VERSION in native\r1.h and build again; -AnyDllVersion for a dry run)" }
# (an absolute path: a relative -Dist made `$_.FullName.Substring($files.Length + 1)` below cut the payload list at the wrong place and the installer then reported its own files "missing", found 2026-11-04)
$dist = [IO.Path]::GetFullPath($(if ($Dist) { $Dist } else { Join-Path $root "dist" }))
New-Item -ItemType Directory -Force $dist | Out-Null
$stage = Join-Path $dist "revelations-coop-$Version"
$files = Join-Path $stage "files"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $files, (Join-Path $files "coop") | Out-Null
# The players' readme carries the version (@VERSION@ in docs\release-readme.txt), so it cannot go out with the number of an older release.
$readme = (Get-Content -LiteralPath (Join-Path $root "docs\release-readme.txt") -Raw).Replace("@VERSION@", $Version)
Copy-Item $dllFile (Join-Path $files "dinput8.dll")
Set-Content -Path (Join-Path $files "coop\version.txt") -Value $Version -Encoding ASCII
# The "Resident Evil Revelations Co-op" shortcut's launcher: updates the mod from the GitHub releases, then starts the game.
Copy-Item (Join-Path $PSScriptRoot "launcher.ps1") (Join-Path $files "coop\coop-launcher.ps1")
# The installer and its texts also go into the game folder (coop\): "Uninstall Co-op.bat" runs them.
Copy-Item (Join-Path $PSScriptRoot "installer.ps1") (Join-Path $files "coop\coop-installer.ps1")
Copy-Item (Join-Path $PSScriptRoot "coop-texts.ps1") (Join-Path $files "coop\coop-texts.ps1")
Set-Content -Path (Join-Path $files "coop\README.txt") -Value $readme -Encoding ASCII -NoNewline
# The bug-report collector, one for every co-op mod of the workspace (<workspace>\tools\log-collector: collect-logs.ps1 + a profile per game) and a double-clickable
# start for it: <game>\coop\Collect logs.bat -> one zip on the desktop. Every profile goes along (collect-logs.ps1 -All also takes the other co-op mods found on the
# PC); the .bat names this mod's.
$lc = if ($LogCollector) { $LogCollector } else { Workspace-Tool "log-collector" }
foreach ($p in (Join-Path $lc "collect-logs.ps1"), (Join-Path $lc "profiles\revelations1.psd1")) {
    if (-not (Test-Path -LiteralPath $p)) { throw "$p missing (the workspace's tools\log-collector; -LogCollector <folder>)" }
}
Copy-Item -LiteralPath (Join-Path $lc "collect-logs.ps1") (Join-Path $files "coop\collect-logs.ps1")
New-Item -ItemType Directory -Force (Join-Path $files "coop\log-profiles") | Out-Null
Get-ChildItem -LiteralPath (Join-Path $lc "profiles") -Filter "*.psd1" -File | ForEach-Object { Copy-Item -LiteralPath $_.FullName (Join-Path $files "coop\log-profiles\$($_.Name)") }
[IO.File]::WriteAllText((Join-Path $files "coop\Collect logs.bat"),
    "@powershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0collect-logs.ps1`" -Mod revelations1`r`n", (New-Object Text.ASCIIEncoding))
# Every file of the payload: the installer checks the folder it got against it before touching the game (a launcher started twice once deleted the extracted
# files under the first one's installer in another mod of this workspace, which then installed half of them and removed the rest as old).
$list = Get-ChildItem $files -Recurse -File | ForEach-Object { $_.FullName.Substring($files.Length + 1) }
Set-Content -Path (Join-Path $files "coop\payload.txt") -Value (@($list) + "coop\payload.txt") -Encoding ASCII
# Installer at the archive's root (the launcher runs <zip>\coop-installer.ps1 -Game <folder> -AutoClose -NoShortcut)
Copy-Item (Join-Path $PSScriptRoot "installer.ps1") (Join-Path $stage "coop-installer.ps1")
Copy-Item (Join-Path $PSScriptRoot "coop-texts.ps1") (Join-Path $stage "coop-texts.ps1")
Set-Content -Path (Join-Path $stage "Install Co-op.bat") -Encoding ASCII `
    -Value '@start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0coop-installer.ps1" %*'
Set-Content -Path (Join-Path $stage "README-COOP.txt") -Value $readme -Encoding ASCII -NoNewline
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")
$zip = Join-Path $dist "revelations-coop-$Version.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
# The archive is written by .NET's ZipArchive with entry names built here, every one with "/" (the zip format's separator). Compress-Archive of Windows PowerShell 5.1
# wrote "files\coop\..." (the Revelations 2 0.2.0 dry run of 2026-11-08 and the published Revelations 1 0.0.1 zip: `unzip` extracts them with "appears to use
# backslashes as path separators" and exit code 1; Expand-Archive, Explorer, the launcher and the setup stub took them). Files only, in name order; the folders follow from the names.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$base = (Get-Item -LiteralPath $stage).FullName.TrimEnd('\')
$fs = [IO.File]::Open($zip, [IO.FileMode]::CreateNew)
try {
    $za = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in @(Get-ChildItem -LiteralPath $base -Recurse -File | Sort-Object FullName)) {
            if (-not $f.FullName.StartsWith($base + '\')) { throw "zip: $($f.FullName) is not under $base" }
            $name = $f.FullName.Substring($base.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $name, [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $za.Dispose() }
} finally { $fs.Dispose() }
Remove-Item $stage -Recurse -Force
"packaged $zip"
if ($Exe) {
    $mk = if ($Mkinstaller) { $Mkinstaller } elseif (Test-Path -LiteralPath (Join-Path $root "tools\mkinstaller\mkinstaller.ps1")) { Join-Path $root "tools\mkinstaller" } else { Workspace-Tool "mkinstaller" }
    $mkScript = Join-Path $mk "mkinstaller.ps1"
    if (-not (Test-Path $mkScript)) { throw "tools\mkinstaller\mkinstaller.ps1 missing (the workspace's tools folder; -Mkinstaller <folder>)" }
    $exeOut = Join-Path $dist "revelations-coop-setup-$Version.exe"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $mkScript -Zip $zip -Out $exeOut -Product "Resident Evil Revelations Co-op" -Version $Version `
        -Description "Resident Evil Revelations Co-op installer" -TempPrefix "revelations-coop-setup" -Accent "c42830"
    if ($LASTEXITCODE -ne 0) { throw "mkinstaller.ps1 failed" }
}
