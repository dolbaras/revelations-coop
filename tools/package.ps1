# Build the release archive dist\revelations-coop-<version>.zip:
#   Install Co-op.bat + coop-installer.ps1 + coop-texts.ps1   the installer (backs up what it replaces, uninstalls; 13 languages)
#   files\                                                   what goes into the game folder (dinput8.dll = the mod's proxy, coop\coop-launcher.ps1 behind the
#                                                            shortcut, coop\coop-installer.ps1 + coop-texts.ps1 for the uninstaller, coop\version.txt)
#   README-COOP.txt, LICENSE.txt
# Run native\build.sh first. The developer tools (warp, console, test hooks) are inside the DLL but do nothing without their environment variables / console.
# Usage: package.ps1 -Version 0.1.0 [-Exe] [-Dist <folder>]
# -Exe also builds dist\revelations-coop-setup-<version>.exe: a single-file installer that carries this zip (an ADDITIONAL release asset; the zip stays the one the
# shortcut's launcher updates from: the launcher takes the first asset matching revelations-coop-*.zip). Made by <workspace>\tools\mkinstaller (-Mkinstaller <folder>
# for a clone placed elsewhere); the exe is unsigned. -Dist: write to that folder instead of dist\ (tests).
param([Parameter(Mandatory)] [string] $Version, [switch] $Exe, [string] $Mkinstaller = "", [string] $Dist = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "the version must be major.minor.patch (the launcher parses it as a .NET [version]): $Version" }
$dll = Join-Path $root "native\build\dinput8.dll"
if (-not (Test-Path $dll)) { throw "build native\build\dinput8.dll first (sh native/build.sh)" }
$dist = if ($Dist) { $Dist } else { Join-Path $root "dist" }
New-Item -ItemType Directory -Force $dist | Out-Null
$stage = Join-Path $dist "revelations-coop-$Version"
$files = Join-Path $stage "files"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $files, (Join-Path $files "coop") | Out-Null
Copy-Item $dll (Join-Path $files "dinput8.dll")
Set-Content -Path (Join-Path $files "coop\version.txt") -Value $Version -Encoding ASCII
# The "Resident Evil Revelations Co-op" shortcut's launcher: updates the mod from the GitHub releases, then starts the game.
Copy-Item (Join-Path $PSScriptRoot "launcher.ps1") (Join-Path $files "coop\coop-launcher.ps1")
# The installer and its texts also go into the game folder (coop\): "Uninstall Co-op.bat" runs them.
Copy-Item (Join-Path $PSScriptRoot "installer.ps1") (Join-Path $files "coop\coop-installer.ps1")
Copy-Item (Join-Path $PSScriptRoot "coop-texts.ps1") (Join-Path $files "coop\coop-texts.ps1")
Copy-Item (Join-Path $root "docs\release-readme.txt") (Join-Path $files "coop\README.txt")
# Every file of the payload: the installer checks the folder it got against it before touching the game (a launcher started twice once deleted the extracted
# files under the first one's installer in another mod of this workspace, which then installed half of them and removed the rest as old).
$list = Get-ChildItem $files -Recurse -File | ForEach-Object { $_.FullName.Substring($files.Length + 1) }
Set-Content -Path (Join-Path $files "coop\payload.txt") -Value (@($list) + "coop\payload.txt") -Encoding ASCII
# Installer at the archive's root (the launcher runs <zip>\coop-installer.ps1 -Game <folder> -AutoClose -NoShortcut)
Copy-Item (Join-Path $PSScriptRoot "installer.ps1") (Join-Path $stage "coop-installer.ps1")
Copy-Item (Join-Path $PSScriptRoot "coop-texts.ps1") (Join-Path $stage "coop-texts.ps1")
Set-Content -Path (Join-Path $stage "Install Co-op.bat") -Encoding ASCII `
    -Value '@start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0coop-installer.ps1" %*'
Copy-Item (Join-Path $root "docs\release-readme.txt") (Join-Path $stage "README-COOP.txt")
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")
$zip = Join-Path $dist "revelations-coop-$Version.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Remove-Item $stage -Recurse -Force
"packaged $zip"
if ($Exe) {
    $mk = if ($Mkinstaller) { $Mkinstaller } elseif (Test-Path -LiteralPath (Join-Path $root "tools\mkinstaller\mkinstaller.ps1")) { Join-Path $root "tools\mkinstaller" } else { Join-Path $root "..\..\..\tools\mkinstaller" }
    $mkScript = Join-Path $mk "mkinstaller.ps1"
    if (-not (Test-Path $mkScript)) { throw "tools\mkinstaller\mkinstaller.ps1 missing (the workspace's tools folder; -Mkinstaller <folder>)" }
    $exeOut = Join-Path $dist "revelations-coop-setup-$Version.exe"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $mkScript -Zip $zip -Out $exeOut -Product "Resident Evil Revelations Co-op" -Version $Version `
        -Description "Resident Evil Revelations Co-op installer" -TempPrefix "revelations-coop-setup" -Accent "c42830"
    if ($LASTEXITCODE -ne 0) { throw "mkinstaller.ps1 failed" }
}
