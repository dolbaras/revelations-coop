# Update test against the REAL GitHub, without publishing anything: the shortcut's launcher updates a stand-in game folder that holds the published pre-release 0.0.1 to the
# published pre-release 0.0.2, through the release's own JSON (releases/tags/v0.0.2), the real download of the real asset, the real sha256: digest GitHub gives for it and the real
# installer of that release. Network use: reads the GitHub API and downloads release assets (GET only; nothing is written to GitHub, no token is used).
# Everything else happens in a temp folder (never the repository, never a real game); the stand-in "rerev.exe" is a few bytes and is never started (-NoStart).
# Two launchers are tried, each on its own fresh 0.0.1 install: the one that 0.0.1 installed (what players really have) and the one of this tree (tools\launcher.ps1).
# From 0.1.0 on the launcher refuses a pre-release or a draft in its feed (releases/latest never returns one): the tree's launcher is shown refusing the real
# releases/tags/v0.0.2 feed (a pre-release), then it reads the same real entry marked as a full release from a small server on 127.0.0.1 (the asset URL stays GitHub's:
# the real download through github.com's redirect, GitHub's digest, the release's own installer).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tests\test_update_real.ps1 [-Report <markdown file>] [-From v0.0.1] [-To v0.0.2] [-Keep]
param([string] $Report = "", [string] $From = "v0.0.1", [string] $To = "v0.0.2", [switch] $Keep)
$ErrorActionPreference = "Stop"
$env:PSModulePath = (($env:PSModulePath -split ";") | Where-Object { $_ -notlike "*PowerShell*" }) -join ";"   # Windows PowerShell 5.1 modules only (as the launcher does)
$root = Split-Path $PSScriptRoot -Parent
$tools = Join-Path $root "tools"
$repo = "dolbaras/revelations-coop"
$base = Join-Path ([IO.Path]::GetTempPath()) ("re1-update-real-" + $PID)
Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $base | Out-Null
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$script:fails = 0; $script:checks = 0; $script:rows = @()
function Check($cond, $name, $detail = "") {
    $script:checks++
    if ($cond) { Write-Host "ok   $name"; $script:rows += "| ok | $name | $detail |" } else { Write-Host "FAIL $name $detail"; $script:fails++; $script:rows += "| FAIL | $name | $detail |" }
}
function Sha($p) { $fs = [IO.File]::OpenRead($p); try { [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($fs)) -replace "-", "" } finally { $fs.Close() } }
function Get-Text($url) {
    $req = [Net.HttpWebRequest]::Create($url); $req.UserAgent = "re1-coop-update-test"; $req.Accept = "application/vnd.github+json"; $req.Timeout = 30000
    $resp = $req.GetResponse(); try { (New-Object IO.StreamReader($resp.GetResponseStream())).ReadToEnd() } finally { $resp.Close() }
}
function Download($url, $to) {
    $req = [Net.HttpWebRequest]::Create($url); $req.UserAgent = "re1-coop-update-test"; $req.Timeout = 60000; $req.ReadWriteTimeout = 60000
    $resp = $req.GetResponse(); $in = $resp.GetResponseStream(); $out = [IO.File]::Create($to)
    try { $in.CopyTo($out) } finally { $out.Close(); $in.Close(); $resp.Close() }
}
function Run-PS([string] $script, [string[]] $a) {
    $o = Join-Path $base "out.txt"; $e = Join-Path $base "err.txt"
    Remove-Item $o, $e -ErrorAction SilentlyContinue
    $p = Start-Process powershell -ArgumentList (@("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$script`"") + $a) -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput $o -RedirectStandardError $e
    $script:exit = $p.ExitCode
    $script:out = "" + (Get-Content -LiteralPath $o -Raw -ErrorAction SilentlyContinue) + (Get-Content -LiteralPath $e -Raw -ErrorAction SilentlyContinue)
}
Add-Type -AssemblyName System.IO.Compression.FileSystem

# --- the published releases (the feed the launcher will read is the second one)
$feedFrom = "https://api.github.com/repos/$repo/releases/tags/$From"
$feedTo = "https://api.github.com/repos/$repo/releases/tags/$To"
$relFrom = (Get-Text $feedFrom) | ConvertFrom-Json
$relTo = (Get-Text $feedTo) | ConvertFrom-Json
$aFrom = @($relFrom.assets | Where-Object { $_.name -like "revelations-coop-*.zip" })[0]
$aTo = @($relTo.assets | Where-Object { $_.name -like "revelations-coop-*.zip" })[0]
Check ($aFrom -and $aTo) "both releases have a revelations-coop-*.zip asset ($($relFrom.tag_name), $($relTo.tag_name))"
Check ("$($aTo.digest)" -match '^sha256:[0-9a-f]{64}$') "GitHub gives a sha256 digest for $($aTo.name)" "$($aTo.digest)"
if ($script:fails) { throw "the releases are not as expected" }
$dl = Join-Path $base "download"; New-Item -ItemType Directory -Force $dl | Out-Null
$zipFrom = Join-Path $dl $aFrom.name; $zipTo = Join-Path $dl $aTo.name
Download $aFrom.browser_download_url $zipFrom
Download $aTo.browser_download_url $zipTo
Check ((Sha $zipFrom) -eq ("$($aFrom.digest)".Substring(7)).ToUpper()) "the downloaded $From zip matches its digest"
Check ((Sha $zipTo) -eq ("$($aTo.digest)".Substring(7)).ToUpper()) "the downloaded $To zip matches its digest"
$xFrom = Join-Path $base "xfrom"; $xTo = Join-Path $base "xto"
[IO.Compression.ZipFile]::ExtractToDirectory($zipFrom, $xFrom); [IO.Compression.ZipFile]::ExtractToDirectory($zipTo, $xTo)
$verFrom = $From.TrimStart("v"); $verTo = $To.TrimStart("v")
# the same entry of $To as a full release, served from 127.0.0.1 for the launcher of this tree (it refuses a pre-release feed)
$srvDir = Join-Path $base "feed"; New-Item -ItemType Directory -Force $srvDir | Out-Null
$relFull = (Get-Text $feedTo) | ConvertFrom-Json; $relFull.prerelease = $false
[IO.File]::WriteAllText((Join-Path $srvDir "full.json"), ($relFull | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
$port = 20000 + (($PID + 7) % 20000)
$srvScript = Join-Path $base "serve.ps1"
Set-Content -Path $srvScript -Encoding ASCII -Value @'
param($dir, $port)
$l = New-Object System.Net.HttpListener
$l.Prefixes.Add("http://127.0.0.1:$port/")
$l.Start()
while ($l.IsListening) {
    $c = $l.GetContext()
    $f = Join-Path $dir ($c.Request.Url.AbsolutePath.TrimStart('/'))
    if (Test-Path -LiteralPath $f -PathType Leaf) { $b = [IO.File]::ReadAllBytes($f); $c.Response.ContentLength64 = $b.Length; $c.Response.OutputStream.Write($b, 0, $b.Length) }
    else { $c.Response.StatusCode = 404 }
    $c.Response.Close()
}
'@
$srv = Start-Process powershell -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$srvScript`"", "`"$srvDir`"", $port) -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
$feedFull = "http://127.0.0.1:$port/full.json"

function New-Game($name) {
    $g = Join-Path $base $name
    New-Item -ItemType Directory -Force (Join-Path $g "nativePC\msg") | Out-Null
    [IO.File]::WriteAllText((Join-Path $g "rerev.exe"), "MZ stand-in game exe, never started")
    [IO.File]::WriteAllText((Join-Path $g "steam_api.dll"), "steam api")
    [IO.File]::WriteAllText((Join-Path $g "nativePC\msg\Title_eng.gmd"), "texts")
    [IO.File]::WriteAllText((Join-Path $g "dinput8.dll"), "ASI LOADER of somebody else")
    $g
}
function Scenario($label, $useTreeLauncher) {
    Write-Host "--- $label"
    $slug = ($label -replace '[^a-z0-9]+', '-')
    $g = New-Game ("game " + $slug)
    $links = (Join-Path $base ("links one " + $slug)) + ";" + (Join-Path $base ("links two " + $slug))
    $env:RE1_COOP_ANYEXE = "1"      # (the stand-in exe is not the supported build; the launcher starts the new installer without -AnyExe)
    try {
        Run-PS (Join-Path $xFrom "coop-installer.ps1") @("-Quiet", "-AnyExe", "-Game", "`"$g`"", "-ShortcutDir", "`"$links`"", "-Lang", "en")
        Check (($script:exit -eq 0) -and ((Get-Content (Join-Path $g "coop\version.txt") -TotalCount 1) -eq $verFrom)) "$label : the published $From installs into the stand-in game (exit $($script:exit))"
        $launcher = Join-Path $g "coop\coop-launcher.ps1"
        if ($useTreeLauncher) {
            Copy-Item (Join-Path $tools "launcher.ps1") $launcher -Force
            Copy-Item (Join-Path $tools "coop-texts.ps1") (Join-Path $g "coop\coop-texts.ps1") -Force
        }
        $dllBefore = Sha (Join-Path $g "dinput8.dll")
        # a feed nobody can call newer: the launcher must do nothing
        Run-PS $launcher @("-NoStart", "-NoWindow", "-Feed", $feedFrom)
        Check (($script:exit -eq 0) -and ((Get-Content (Join-Path $g "coop\version.txt") -TotalCount 1) -eq $verFrom) -and ((Sha (Join-Path $g "dinput8.dll")) -eq $dllBefore)) "$label : the feed of $From (nothing newer) changes nothing"
        # the default feed (no -Feed): releases/latest never returns a pre-release, so while there is no full release the shortcut's launcher must install nothing (404 -> the game just starts)
        $latest404 = $false
        try { $null = Get-Text "https://api.github.com/repos/$repo/releases/latest" } catch { $latest404 = ("$($_.Exception.Message)" -match "404") }
        if ($latest404) {
            Remove-Item Env:\RE1_COOP_FEED -ErrorAction SilentlyContinue
            Run-PS $launcher @("-NoStart", "-NoWindow")
            $log0 = Get-Content (Join-Path $g "coop\launcher-log.txt") -Raw
            Check (($script:exit -eq 0) -and ((Get-Content (Join-Path $g "coop\version.txt") -TotalCount 1) -eq $verFrom) -and ($log0 -match "update: .*404")) "$label : the default feed (releases/latest = 404, only pre-releases exist) installs nothing and the launcher still exits 0"
        } else { Write-Host "skip $label : releases/latest returns a release now; the default-feed check is for the time before the first full release"; $script:rows += "| skip | $label : default feed check | a full release exists |" }
        # the real update (the tree's launcher: first the refusal of the pre-release feed, then the same entry as a full release from 127.0.0.1)
        $feedUse = $feedTo
        if ($useTreeLauncher -and $relTo.prerelease) {
            $n0 = @(Get-Content (Join-Path $g "coop\launcher-log.txt") -ErrorAction SilentlyContinue).Count
            Run-PS $launcher @("-NoStart", "-NoWindow", "-Feed", $feedTo)
            $logR = (@(Get-Content (Join-Path $g "coop\launcher-log.txt") -ErrorAction SilentlyContinue) | Select-Object -Skip $n0) -join " | "
            Check (($script:exit -eq 0) -and ($logR -match "refused: the release is a pre-release") -and ($logR -notmatch "downloaded|checksum ok") -and ((Get-Content (Join-Path $g "coop\version.txt") -TotalCount 1) -eq $verFrom)) "$label : the real $To feed (a pre-release) is refused, nothing downloaded, $verFrom stays" "$logR"
            $feedUse = $feedFull
        }
        Run-PS $launcher @("-NoStart", "-NoWindow", "-Feed", $feedUse)
        $log = Get-Content (Join-Path $g "coop\launcher-log.txt") -Raw
        Check ($script:exit -eq 0) "$label : launcher exit 0 ($($script:out.Trim()))"
        Check ($log -match "installed $([regex]::Escape($verFrom)), latest $([regex]::Escape($To))") "$label : the launcher saw $verFrom installed and $To in the feed"
        Check ("$($aTo.browser_download_url)" -like "https://github.com/dolbaras/revelations-coop/releases/download/*") "$label : the asset URL passes the launcher's own-repository check" "$($aTo.browser_download_url)"
        $want = ("$($aTo.digest)".Substring(7)).ToUpper()
        Check ($log -match "checksum ok $want") "$label : the launcher checked the real digest of the downloaded asset" "sha256 $want"
        if ($useTreeLauncher) { Check ($log -match "downloaded \d+ bytes from https://(?!github\.com)") "$label : the asset came from GitHub through its redirect to the asset host" "$(([regex]::Match($log, 'downloaded [^\r\n]*')).Value)" }
        Check ($log -match "installer exit 0") "$label : the downloaded installer ran and exited 0"
        Check ((Get-Content (Join-Path $g "coop\version.txt") -TotalCount 1) -eq $verTo) "$label : coop\version.txt is $verTo now"
        $rec = @(Get-Content (Join-Path $g "coop\install.txt"))
        Check ($rec -contains "version=$verTo") "$label : the install record says version=$verTo"
        Check ((Sha (Join-Path $g "dinput8.dll")) -eq (Sha (Join-Path $xTo "files\dinput8.dll"))) "$label : dinput8.dll is the DLL inside the $To archive"
        Check ((Get-Content (Join-Path $g "coop-backup\dinput8.dll") -Raw) -eq "ASI LOADER of somebody else") "$label : the foreign dinput8.dll is still the backup"
        Check ((@($rec | Where-Object { $_ -like "shortcut=*" }).Count -eq 2) -and (Test-Path -LiteralPath (Join-Path ($links -split ";")[1] "Resident Evil Revelations Co-op.lnk"))) "$label : the two shortcuts stay in the record and on disk"
        Check ((Sha (Join-Path $g "coop\coop-installer.ps1")) -eq (Sha (Join-Path $xTo "files\coop\coop-installer.ps1"))) "$label : the installed installer is the one of $To"
        # the next start finds nothing newer
        Run-PS (Join-Path $g "coop\coop-launcher.ps1") @("-NoStart", "-NoWindow", "-Feed", $feedUse)
        Check (($script:exit -eq 0) -and ((Get-Content (Join-Path $g "coop\version.txt") -TotalCount 1) -eq $verTo)) "$label : a second start with the same feed installs nothing"
        Run-PS (Join-Path $g "coop\coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$g`"")
        Check (($script:exit -eq 0) -and ((Get-Content (Join-Path $g "dinput8.dll") -Raw) -eq "ASI LOADER of somebody else") -and -not (Test-Path (Join-Path $g "coop"))) "$label : uninstall puts the foreign dinput8.dll back and removes coop\"
    } finally { Remove-Item Env:\RE1_COOP_ANYEXE -ErrorAction SilentlyContinue }
}
try {
    Scenario "launcher installed by $From" $false
    Scenario "launcher of this tree" $true
} finally { if ($srv -and -not $srv.HasExited) { Stop-Process -Id $srv.Id -Force } }

if ($Report) {
    $head = @("# Update test against the real GitHub", "", "Run: $(Get-Date -Format 'yyyy-MM-dd HH:mm') (script tests\test_update_real.ps1), feed $feedTo (the tree's launcher: refused as a pre-release, then the same entry as a full release from 127.0.0.1), $($script:checks - $script:fails) of $($script:checks) checks passed.", "",
              "| result | check | detail |", "|---|---|---|")
    Set-Content -LiteralPath $Report -Value ($head + $script:rows) -Encoding UTF8
}
if (-not $Keep) { Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue }
"$($script:checks - $script:fails) of $($script:checks) checks passed" + $(if ($Keep) { " (kept: $base)" } else { "" })
exit $(if ($script:fails) { 1 } else { 0 })
