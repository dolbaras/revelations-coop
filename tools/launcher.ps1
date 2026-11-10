# Resident Evil Revelations Co-op launcher (shipped as <game>\coop\coop-launcher.ps1; the installer's desktop shortcut runs it). Before the game starts it asks
# GitHub for the latest release of the mod; when that is newer than the installed one, it downloads the release's archive (checked against the SHA-256 GitHub gives
# for it; a release that gives none is NOT installed), runs that version's installer for this game folder (its window shows the steps and closes by itself) and then
# starts the game. Offline, on any error (rate limit, 404, cut download, wrong checksum), or when the check takes longer than 6 seconds, the game simply starts as it is.
# The protocol every release keeps is in docs/releasing.md.
# Usage: coop-launcher.ps1 [-NoUpdate] [-NoStart] [-Feed <url of a releases/latest JSON>] [-NoWindow]
# -NoWindow (tests): no progress window, the installer runs with -Quiet.
param([switch] $NoUpdate, [switch] $NoStart, [string] $Feed = "", [switch] $NoWindow)
$ErrorActionPreference = "Stop"
$coop = $PSScriptRoot
$game = Split-Path $coop -Parent
$feedUrl = if ($Feed) { $Feed } elseif ($env:RE1_COOP_FEED) { $env:RE1_COOP_FEED } else {
    "https://api.github.com/repos/dolbaras/revelations-coop/releases/latest" }
$log = Join-Path $coop "launcher-log.txt"
# Started from PowerShell 7, Windows PowerShell inherits its module path and some of its own
# commands fail to load: the installer started from here gets Windows PowerShell's own.
$env:PSModulePath = (@($env:PSModulePath -split ';') | Where-Object { $_ -and $_ -notmatch '\\PowerShell\\' }) -join ';'
function Log($s) { try { Add-Content -LiteralPath $log -Encoding UTF8 -Value ("{0} {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $s) } catch { } }

. (Join-Path $coop "coop-texts.ps1")
$script:lang = Pick-Lang ""
$script:TITLE = "Resident Evil Revelations Co-op"
function Lt($k) { L ("l_" + $k) }

# Steam's copy: its app id from the library's appmanifest_<id>.acf whose installdir is this folder.
function Steam-AppId {
    if ($game -notmatch '^(.*\\steamapps)\\common\\([^\\]+)$') { return $null }
    $apps, $dir = $Matches[1], $Matches[2]
    foreach ($acf in Get-ChildItem -LiteralPath $apps -Filter "appmanifest_*.acf" -ErrorAction SilentlyContinue) {
        $text = Get-Content -LiteralPath $acf.FullName -Raw -ErrorAction SilentlyContinue
        if ($text -match ('"installdir"\s+"' + [regex]::Escape($dir) + '"') -and $text -match '"appid"\s+"(\d+)"') { return $Matches[1] }
    }
    $null
}

function Start-Game {
    # Steam's copy through Steam (its overlay, its own start); any other one by its exe.
    $app = Steam-AppId
    $how = if ($app) { "steam://rungameid/$app" } else { "rerev.exe" }
    Log ("start: $how" + $(if ($NoStart) { " (-NoStart)" } else { "" }))
    if ($NoStart) { return }
    if ($app) { Start-Process "steam://rungameid/$app" }
    else { Start-Process -FilePath (Join-Path $game "rerev.exe") -WorkingDirectory $game }
}

function Version-Of($s) {
    $s = ("$s".Trim()) -replace '^[vV]', ''
    try { return [version]$s } catch { return $null }
}

# A small window: one line and a bar.
$form = $null
function Show-Window($text) {
    if ($NoWindow) { return }
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $script:form = New-Object Windows.Forms.Form
    $form.Text = $script:TITLE; $form.FormBorderStyle = "FixedSingle"; $form.MaximizeBox = $false; $form.MinimizeBox = $false
    $form.StartPosition = "CenterScreen"; $form.ClientSize = New-Object Drawing.Size(460, 120)
    $form.BackColor = [Drawing.Color]::FromArgb(24, 24, 28); $form.ForeColor = [Drawing.Color]::FromArgb(232, 232, 236)
    $form.Font = New-Object Drawing.Font("Segoe UI", 10)
    $script:lbl = New-Object Windows.Forms.Label
    $lbl.Location = New-Object Drawing.Point(18, 16); $lbl.Size = New-Object Drawing.Size(424, 62); $lbl.Text = $text
    $form.Controls.Add($lbl)
    $bar = New-Object Windows.Forms.Panel
    $bar.Location = New-Object Drawing.Point(18, 90); $bar.Size = New-Object Drawing.Size(424, 10)
    $bar.BackColor = [Drawing.Color]::FromArgb(34, 34, 40)
    $script:fill = New-Object Windows.Forms.Panel
    $fill.Size = New-Object Drawing.Size(0, 10); $fill.BackColor = [Drawing.Color]::FromArgb(196, 40, 48)
    $bar.Controls.Add($fill); $form.Controls.Add($bar)
    try { $form.Icon = New-Object Drawing.Icon((Join-Path $coop "coop.ico"), 32, 32) } catch { }
    # a dark title bar like the installer's (Windows 10 2004+ / 11)
    $form.Add_HandleCreated({
        try {
            Add-Type -Namespace CoopLauncher -Name Dwm -MemberDefinition '[DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr h, int a, ref int v, int s);'
            $on = 1; [CoopLauncher.Dwm]::DwmSetWindowAttribute($this.Handle, 20, [ref]$on, 4) | Out-Null
        } catch { }
    })
    $form.Show()
    $form.Refresh()
    [Windows.Forms.Application]::DoEvents()
}
function Set-Progress($pct, $text) {
    if (-not $form) { return }
    if ($text) { $lbl.Text = $text }
    $fill.Width = [int](424 * [Math]::Max(0, [Math]::Min(100, $pct)) / 100)
    [Windows.Forms.Application]::DoEvents()
}

# What a failed update check was, in one line for the log (and the message after a failed download): GitHub's rate limit (60 requests an hour without a login:
# 403 or 429 with X-RateLimit-Remaining 0), no full release yet (404), another HTTP status, no network, or anything else. The game starts in every case.
function Explain-Error($ex) {
    $e = $ex
    while ($e.InnerException -and -not ($e -is [Net.WebException])) { $e = $e.InnerException }
    if ($e -is [Net.WebException] -and $e.Response) {
        $code = [int]$e.Response.StatusCode
        $left = $e.Response.Headers["X-RateLimit-Remaining"]; $reset = $e.Response.Headers["X-RateLimit-Reset"]
        if (($code -eq 403 -or $code -eq 429) -and ($left -eq "0" -or $code -eq 429)) {
            $when = ""
            if ($reset -match '^\d+$') { $when = " (resets " + [DateTimeOffset]::FromUnixTimeSeconds([long]$reset).LocalDateTime.ToString("HH:mm") + ")" }
            return "GitHub's rate limit is used up${when} [HTTP $code]: no update check this time"
        }
        if ($code -eq 404) { return "not found [HTTP 404] (no full release yet, or a missing file)" }
        return "GitHub answered [HTTP $code]"
    }
    if ($e -is [Net.WebException]) { return "no network ($($e.Status)): " + $e.Message }
    $ex.Message
}

# The feed (a small JSON): 6 seconds to answer, 10 seconds and 1 MB in all. (Async + Wait: the request's own Timeout does not cover an asynchronous call, and its
# synchronous form leaves the name lookup out (Microsoft's documentation of HttpWebRequest.Timeout); measured 2026-11-06 under Windows PowerShell 5.1:
# GetResponseAsync returns at once, a failed lookup or a dead address stays inside the task, and Wait(6000) ends it.)
function Read-Feed($url, $cur) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $req = [Net.HttpWebRequest]::Create($url)
    $req.UserAgent = "re1-coop-launcher/$cur"; $req.Accept = "application/vnd.github+json"; $req.Timeout = 6000; $req.ReadWriteTimeout = 6000
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $task = $req.GetResponseAsync()
    try {
        if (-not $task.Wait(6000)) { $req.Abort(); throw "the feed did not answer in 6 seconds" }
    } catch [AggregateException] { throw $_.Exception.GetBaseException() }
    $resp = $task.Result
    try {
        $in = $resp.GetResponseStream(); $ms = New-Object IO.MemoryStream; $buf = New-Object byte[] 8192
        while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
            $ms.Write($buf, 0, $n)
            if ($sw.ElapsedMilliseconds -gt 10000 -or $ms.Length -gt 1048576) { $req.Abort(); throw "the feed is too slow or too big" }
        }
        # (a reader that drops a byte-order mark: ConvertFrom-Json of Windows PowerShell refuses one, "Invalid JSON primitive")
        $ms.Position = 0
        return (New-Object IO.StreamReader($ms, [Text.Encoding]::UTF8, $true)).ReadToEnd()
    } finally { $resp.Close() }
}

function Try-Update {
    $cur = Version-Of (Get-Content -LiteralPath (Join-Path $coop "version.txt") -TotalCount 1 -ErrorAction SilentlyContinue)
    if (-not $cur) { Log "no installed version"; return }
    $rel = (Read-Feed $feedUrl $cur) | ConvertFrom-Json
    $new = Version-Of $rel.tag_name
    Log "installed $cur, latest $($rel.tag_name)"
    if (-not $new -or $new -le $cur) { return }
    # (releases/latest never answers with these; a feed that does is wrong)
    if ($rel.prerelease -or $rel.draft) { Log "refused: the release is a pre-release or a draft"; return }
    $asset = @($rel.assets | Where-Object { $_.name -like "revelations-coop-*.zip" })[0]
    if (-not $asset) { Log "no archive in the release"; return }
    # (only the mod's own release archive; another feed is for tests)
    if (-not $Feed -and -not $env:RE1_COOP_FEED -and "$($asset.browser_download_url)" -notlike "https://github.com/dolbaras/revelations-coop/releases/download/*") {
        Log "unexpected download url $($asset.browser_download_url)"; return
    }
    # GitHub gives each asset's SHA-256 ("digest": "sha256:<64 hex>"). Without it nothing proves the download is the published file: not installed (decided 2026-11-06).
    $dm = [regex]::Match("$($asset.digest)", '^sha256:([0-9a-fA-F]{64})$')
    if (-not $dm.Success) { Log "refused: the release gives no usable checksum ($($asset.digest))"; return }
    $want = $dm.Groups[1].Value.ToUpper()
    # (the update window also carries the one network-play line: the port and the firewall)
    Show-Window (((Lt "found") -f $new, $cur) + "`n" + (Lt "net"))
    $script:tmp = Join-Path ([IO.Path]::GetTempPath()) ("re1-coop-update-" + $new + "-" + $PID)
    $tmp = $script:tmp
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $zip = Join-Path $tmp $asset.name
    # The download, chunk by chunk (the bar shows it). github.com answers with a redirect to its asset host (TLS 1.2, followed by HttpWebRequest).
    $dreq = [Net.HttpWebRequest]::Create($asset.browser_download_url)
    $dreq.UserAgent = "re1-coop-launcher/$cur"; $dreq.Timeout = 30000; $dreq.ReadWriteTimeout = 30000
    $dresp = $dreq.GetResponse()
    $from = "$($dresp.ResponseUri.Scheme)://$($dresp.ResponseUri.Host)"   # (after the redirect: the host that sent the file)
    $total = [double]$dresp.ContentLength
    $in = $dresp.GetResponseStream()
    $out = [IO.File]::Create($zip)
    $got = 0.0
    try {
        $buf = New-Object byte[] 65536
        while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
            $out.Write($buf, 0, $n); $got += $n
            if ($total -gt 0) { Set-Progress (100 * $got / $total) $null }
        }
    } finally { $out.Close(); $in.Close(); $dresp.Close() }
    if ($total -gt 0 -and $got -ne $total) { throw "the download broke off ($got of $total bytes)" }
    Log "downloaded $got bytes from $from"
    Set-Progress 100 (Lt "verify")
    # (.NET, not Get-FileHash: a script cmdlet that fails to load when this runs under PowerShell 7's module path)
    $fs = [IO.File]::OpenRead($zip)
    try { $have = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($fs)) -replace "-", "" } finally { $fs.Close() }
    if ($have -ne $want) { throw (Lt "bad") }
    Log "checksum ok $have"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, (Join-Path $tmp "x"))
    $inst = Join-Path $tmp "x\coop-installer.ps1"
    if (-not (Test-Path -LiteralPath $inst)) { throw "coop-installer.ps1 missing in $($asset.name)" }
    if ($form) { $form.Hide() }
    # That version's installer, its window, for this folder; it closes by itself when done.
    $iargs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$inst`"", "-Game", "`"$game`"", "-AutoClose", "-NoShortcut")
    if ($NoWindow) { $iargs += "-Quiet" }
    $p = Start-Process powershell -PassThru -Wait -WindowStyle Hidden -ArgumentList $iargs
    Log "installer exit $($p.ExitCode)"
    if ($p.ExitCode -ne 0) { throw "installer exit $($p.ExitCode)" }
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

# One launcher at a time (a double-clicked shortcut): the first one updates and starts the game.
$script:once = New-Object Threading.Mutex($false, "Local\Re1CoopLauncher")
try { $first = $script:once.WaitOne(0) } catch [Threading.AbandonedMutexException] { $first = $true }
if (-not $first) { Log "another launcher runs"; exit 0 }
# This folder's game already running (another copy elsewhere does not count): nothing to do.
if (Get-Process -Name rerev -ErrorAction SilentlyContinue |
        Where-Object { -not $_.Path -or $_.Path.StartsWith($game + "\", [StringComparison]::OrdinalIgnoreCase) }) { Log "the game runs already"; exit 0 }
if (-not $NoUpdate) {
    try { Try-Update } catch {
        # (a rate limit or a missing release shows as the HTTP status; the downloaded part is removed)
        $why = Explain-Error $_.Exception
        Log "update: $why"
        if ($script:tmp) { Remove-Item -LiteralPath $script:tmp -Recurse -Force -ErrorAction SilentlyContinue }
        # (offline or GitHub slow: no message, the game just starts; a failed install is said)
        if ($form) {
            $form.Hide()
            $cur = Get-Content -LiteralPath (Join-Path $coop "version.txt") -TotalCount 1 -ErrorAction SilentlyContinue
            [Windows.Forms.MessageBox]::Show(((Lt "failed") -f $why, $cur), $script:TITLE, "OK", "Warning") | Out-Null
        }
    }
    if ($form) { $form.Close() }
}
Start-Game
