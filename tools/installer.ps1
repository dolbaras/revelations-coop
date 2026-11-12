# Resident Evil Revelations Co-op installer / uninstaller for players (shipped in the release zip as "Install Co-op.bat" + coop-installer.ps1 + coop-texts.ps1,
# the payload in the "files" folder next to them). Built by tools\package.ps1 from tools\installer.ps1.
#   install: finds the game folder (this folder, the Steam install record, the Steam libraries, or the player picks it), checks that rerev.exe is the supported
#            Steam build (13298487), keeps a dinput8.dll of someone else's in <game>\coop-backup, copies the mod (dinput8.dll + coop\) and writes
#            <game>\coop\install.txt (what it installed, what it backed up). When that dinput8.dll is an Ultimate ASI Loader (the loader of the Fusion
#            Fix and of most ASI mods) a copy of it runs on as winmm.dll, so its plugins stay on (Chain-Plan).
#   update:  the same over an older version; originals already backed up are kept. A dinput8.dll another mod put over the co-op one meanwhile
#            becomes the original.
#   remove:  deletes what install.txt lists and puts the backed-up files back (a dinput8.dll another mod put over the co-op one, or a loader copy the
#            player changed, stays); also the mod's logs next to rerev.exe (rev1_coop.log, rev1_coop.N.log)
#            and rev1_coop\shots + rev1_coop\ when they are empty (screenshots stay). The game's saves and the co-op settings file
#            (%LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS\rev1_coop.ini) are not touched.
# A window shows the steps and a progress bar (also on the taskbar button). -Quiet: no window, the same steps written to the console (test scripts).
# -Game without -Uninstall starts installing into that folder right away.
# The window's install also makes the "Resident Evil Revelations Co-op" shortcut (desktop and Start menu, the game's icon with a co-op badge): it runs
# coop\coop-launcher.ps1, which installs a newer release from GitHub before starting the game. The launcher runs a new release's installer as
#   coop-installer.ps1 -Game <folder> -AutoClose -NoShortcut
# so every later installer has to take these: -AutoClose closes the window after a successful install, -NoShortcut keeps the shortcuts as they are (only
# those already made are refreshed). -ShortcutDir puts new shortcuts in that folder instead (tests).
# -AnyExe: skip the check of rerev.exe's hash (tests on a stand-in folder only).
# Usage: coop-installer.ps1 [-Game <folder>] [-Uninstall] [-Quiet] [-Lang en|ru|de|fr|it|es|pt|pt-br|nl|pl|ja|zh-cn|zh-tw]
#                           [-AutoClose] [-NoShortcut] [-ShortcutDir <folder>]
param([string] $Game = "", [switch] $Uninstall, [switch] $Quiet, [switch] $IgnoreRunning, [string] $Lang = "",
      [switch] $AutoClose, [switch] $NoShortcut, [string] $ShortcutDir = "", [switch] $AnyExe)
$ErrorActionPreference = "Stop"
# (tests: the launcher starts a new release's installer without -AnyExe; RE1_COOP_ANYEXE=1 does the same)
if ($env:RE1_COOP_ANYEXE -eq "1") { $AnyExe = [switch]::Present }
# The setup exe (revelations-coop-setup-<version>.exe, tools\mkinstaller) unpacks this installer into a temp folder and says where it lies (COOP_SETUP_DIR:
# the game is looked for there too) and which file to write as the first act (COOP_SETUP_FLAG: a failed run without it = PowerShell never ran the script).
if ($env:COOP_SETUP_FLAG -and -not $Quiet) { try { [IO.File]::WriteAllText($env:COOP_SETUP_FLAG, "1") } catch { } }
$script:setupDir = if ($env:COOP_SETUP_DIR -and (Test-Path -LiteralPath $env:COOP_SETUP_DIR -PathType Container)) { $env:COOP_SETUP_DIR } else { $null }
$here = $PSScriptRoot
$payload = Join-Path $here "files"
$script:TITLE = "Resident Evil Revelations Co-op"
# The exe the mod is made for (Steam build 13298487); any other exe: the mod stays off, so nothing is installed.
$script:EXE_SHA = "1573B79EB921B1E7F571E6DEB71C39961C7F1940FAD6D19283FC3AEA7FCA48AA"
$script:STEAM_APP = "222480"
$script:DLL_MARK = "RE Revelations co-op proxy"
. (Join-Path $here "coop-texts.ps1")
$script:lang = Pick-Lang $Lang
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
trap {
    $why = "Resident Evil Revelations Co-op installer: " + $_.Exception.Message
    try { Add-Content -LiteralPath (Join-Path ([IO.Path]::GetTempPath()) "re1-coop-install.log") -Value $why } catch { }
    if ($Quiet) { Write-Host $why } else {   # (i18n1, as Rev2 lang2: the box in the window's language, "Error: " + the message; the English line stays in the temp log)
        try { [System.Windows.Forms.MessageBox]::Show((L "err") + $_.Exception.Message, $script:TITLE, "OK", "Error") | Out-Null } catch { }
    }
    exit 1
}
# This version: the payload's, or (the uninstaller in the game folder) the installed one's.
$version = ""
foreach ($v in (Join-Path $payload "coop\version.txt"), (Join-Path $here "coop\version.txt"), (Join-Path $here "version.txt")) {
    if (-not $version -and (Test-Path -LiteralPath $v)) { $version = "$(Get-Content -LiteralPath $v -TotalCount 1)".Trim() }
}

# The taskbar button's progress, the console window and sharp text on scaled screens.
try {
    Add-Type -ErrorAction Stop @"
using System;
using System.Runtime.InteropServices;
public static class CoopWin {
    [ComImport, Guid("ea1afb91-9e28-4b86-90e9-9e9f8a5eefaf"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface ITaskbarList3 {
        void HrInit(); void AddTab(IntPtr h); void DeleteTab(IntPtr h); void ActivateTab(IntPtr h); void SetActiveAlt(IntPtr h);
        void MarkFullscreenWindow(IntPtr h, [MarshalAs(UnmanagedType.Bool)] bool f);
        void SetProgressValue(IntPtr h, ulong done, ulong total);
        void SetProgressState(IntPtr h, int state);
    }
    [ComImport, Guid("56fdf344-fd6d-11d0-958a-006097c9a090"), ClassInterface(ClassInterfaceType.None)]
    class TaskbarList {}
    static ITaskbarList3 bar;
    // state: 0 none, 1 busy, 2 normal, 4 error, 8 paused
    public static void Progress(IntPtr h, int state, int pct) {
        try {
            if (bar == null) { bar = (ITaskbarList3)new TaskbarList(); bar.HrInit(); }
            bar.SetProgressState(h, state);
            if (state == 2 || state == 4 || state == 8) bar.SetProgressValue(h, (ulong)pct, 100);
        } catch { }
    }
    [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)] public static extern int SetWindowTheme(IntPtr h, string app, string id);
    [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr h, int attr, ref int v, int size);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint PrivateExtractIcons(string f, int i, int cx, int cy, IntPtr[] h, uint[] id, uint n, uint flags);
    [DllImport("user32.dll")] public static extern bool DestroyIcon(IntPtr h);
    // A dark title bar (Windows 10 2004+ / 11; older ones keep theirs).
    public static void DarkTitle(IntPtr h) { int on = 1; try { DwmSetWindowAttribute(h, 20, ref on, 4); } catch { } }
}
"@
    $script:haveWin = $true
} catch { $script:haveWin = $false }

# ---------------------------------------------------------------------------------------------
# What the steps report: the window's controls, or the console (-Quiet).
$script:ui = $null
$script:logLines = New-Object System.Collections.Generic.List[string]

function Ui-Pump { if ($script:ui) { [System.Windows.Forms.Application]::DoEvents() } }

function Ui-Log($text) {
    $script:logLines.Add("$(Get-Date -Format 'HH:mm:ss') $text")
    if ($script:ui) { $script:ui.log.AppendText("$text`r`n"); Ui-Pump } else { Write-Host $text }
}

function Ui-Progress($pct, $text) {
    $pct = [Math]::Max(0, [Math]::Min(100, [int]$pct))
    if (-not $script:ui) { return }
    $u = $script:ui
    $u.fill.Width = [int]($u.bar.ClientSize.Width * $pct / 100)
    $u.pct.Text = "$pct %"
    if ($null -ne $text) { $u.action.Text = $text }
    if ($script:haveWin) { [CoopWin]::Progress($u.form.Handle, 2, $pct) }
    Ui-Pump
}

# state: run, done, warn, skip, fail
function Ui-Step($key, $state) {
    if (-not $script:ui) { Write-Host "[$state] $key"; return }
    $row = $script:ui.steps[$key]
    if (-not $row) { return }
    $c = $script:ui.colors
    switch ($state) {
        "run"  { $row.icon.Text = [char]0x25CF; $row.icon.ForeColor = $c.accent; $row.text.ForeColor = $c.text; $script:ui.running = $row }
        "done" { $row.icon.Text = [char]0x2714; $row.icon.ForeColor = $c.ok; $row.text.ForeColor = $c.text }
        "warn" { $row.icon.Text = "!"; $row.icon.ForeColor = $c.warn; $row.text.ForeColor = $c.text }
        "skip" { $row.icon.Text = [char]0x2013; $row.icon.ForeColor = $c.dim; $row.text.ForeColor = $c.dim }
        "fail" { $row.icon.Text = [char]0x2716; $row.icon.ForeColor = $c.bad; $row.text.ForeColor = $c.text }
    }
    if ($state -ne "run" -and $script:ui.running -eq $row) { $script:ui.running = $null }
    Ui-Pump
}


# ---------------------------------------------------------------------------------------------
# The game folder

# (Join-Path throws on a drive that does not exist: the D:\Games candidate on a PC without D:)
function Test-Game($dir) {
    if (-not $dir) { return $false }
    try { Test-Path -LiteralPath ([IO.Path]::Combine($dir, "rerev.exe")) -PathType Leaf } catch { $false }
}

function Sha256-File($p) {
    $fs = [IO.File]::OpenRead($p)
    try { ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($fs)) -replace "-", "").ToUpper() } finally { $fs.Close() }
}

# 1 = the supported exe, 0 = another one
function Exe-Ok($g) {
    if ($AnyExe) { return $true }
    (Sha256-File (Join-Path $g "rerev.exe")) -eq $script:EXE_SHA
}

# Candidate game folders, most likely first.
function Find-Games {
    $c = New-Object System.Collections.Generic.List[string]
    foreach ($d in @($here, (Split-Path $here -Parent), $script:setupDir)) { if ($d) { $c.Add($d) } }
    $keys = @("HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App $($script:STEAM_APP)",
              "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App $($script:STEAM_APP)")
    foreach ($k in $keys) {
        $p = Get-ItemProperty $k -ErrorAction SilentlyContinue
        if ($p -and $p.InstallLocation) { $c.Add($p.InstallLocation.Trim('"')) }
    }
    $steam = (Get-ItemProperty "HKCU:\SOFTWARE\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $libs = @($steam)
        $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs += $m.Groups[1].Value -replace '\\\\', '\' }
        }
        foreach ($l in $libs) {
            $acf = Join-Path $l "steamapps\appmanifest_$($script:STEAM_APP).acf"
            $dir = "RESIDENT EVIL REVELATIONS"
            if (Test-Path -LiteralPath $acf) {
                $t = Get-Content -LiteralPath $acf -Raw -ErrorAction SilentlyContinue
                if ($t -match '"installdir"\s+"([^"]+)"') { $dir = $Matches[1] }
            }
            $c.Add((Join-Path $l "steamapps\common\$dir"))
        }
    }
    foreach ($p in @("$env:ProgramFiles (x86)\Steam\steamapps\common\RESIDENT EVIL REVELATIONS", "C:\Games\RESIDENT EVIL REVELATIONS", "D:\Games\RESIDENT EVIL REVELATIONS")) { $c.Add($p) }
    # (one form of each folder: "...\Revelations\" is the same as "...\Revelations")
    $c | Where-Object { Test-Game $_ } | ForEach-Object {
        $p = (Resolve-Path -LiteralPath $_).Path
        if ($p.Length -gt 3) { $p.TrimEnd('\') } else { $p }
    } | Select-Object -Unique
}

function Read-Manifest($g) {
    $m = @{ files = @(); backups = @(); shortcuts = @(); chains = @(); version = "" }
    $f = Join-Path $g "coop\install.txt"
    if (Test-Path -LiteralPath $f) {
        foreach ($line in Get-Content -LiteralPath $f) {
            if ($line -match '^version=(.*)$') { $m.version = $Matches[1] }
            elseif ($line -match '^file=(.*)$') { $m.files += $Matches[1] }
            elseif ($line -match '^backup=(.*)$') { $m.backups += $Matches[1] }
            # (0.2.0: a copy the install made for the player's ASI loader, see Chain-Plan; also listed as file=)
            elseif ($line -match '^chain=(.*)$') { if ($script:CHAIN.Contains($Matches[1])) { $m.chains += $Matches[1] } }
            # (a line with several shortcuts, or an empty one: split after each .lnk, drop the empty ones)
            elseif ($line -match '^shortcut=(.*)$') { $m.shortcuts += @([regex]::Split($Matches[1], '(?<=\.lnk) ') | Where-Object { $_ }) }
        }
    }
    # An "original" kept by an install over the mod's own dinput8.dll is the mod's DLL: not the game's.
    $m.backups = @($m.backups | Where-Object { -not (Is-CoopDll (Join-Path $g "coop-backup\$_")) })
    $m
}

# The mod's proxy DLL by the words in it (a dinput8.dll of someone else's, or the original, does not have them).
function Has-Text($p, [string[]] $words) {
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return $false }
    try { $s = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($p)) } catch { return $false }
    foreach ($w in $words) { if ($s.Contains($w)) { return $true } }
    $false
}
function Is-CoopDll($p) { Has-Text $p @($script:DLL_MARK) }

# The machine and the exported names of a PE file, read from its bytes (nothing of it runs); $null for anything that is not one.
function Pe-Exports($p) {
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return $null }
    try { $b = [IO.File]::ReadAllBytes($p) } catch { return $null }
    $n = $b.Length
    if ($n -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return $null }
    $pe = [long][BitConverter]::ToUInt32($b, 0x3C)
    if ($pe + 26 -gt $n -or [Text.Encoding]::ASCII.GetString($b, [int]$pe, 4) -ne "PE`0`0") { return $null }
    $machine = [BitConverter]::ToUInt16($b, $pe + 4); $nsec = [BitConverter]::ToUInt16($b, $pe + 6); $optsize = [BitConverter]::ToUInt16($b, $pe + 20)
    $opt = $pe + 24; $sec = $opt + $optsize
    if ($sec + 40 * $nsec -gt $n) { return $null }
    $magic = [BitConverter]::ToUInt16($b, $opt)
    $dirs = if ($magic -eq 0x10B) { $opt + 96 } elseif ($magic -eq 0x20B) { $opt + 112 } else { return $null }
    if ($dirs + 8 -gt $opt + $optsize) { return $null }
    # (an RVA's offset in the file by the section table, -1 outside the file)
    $secs = @(for ($i = 0; $i -lt $nsec; $i++) { $s = $sec + 40 * $i
        @{ va = [long][BitConverter]::ToUInt32($b, $s + 12); vs = [long][BitConverter]::ToUInt32($b, $s + 8); raw = [long][BitConverter]::ToUInt32($b, $s + 16); ptr = [long][BitConverter]::ToUInt32($b, $s + 20) } })
    $off = { param([long] $rva)
        foreach ($s in $secs) { $span = [Math]::Max($s.vs, $s.raw)
            if ($rva -ge $s.va -and $rva -lt $s.va + $span) { $o = $rva - $s.va; if ($o -ge $s.raw -or $s.ptr + $o -ge $n) { return -1 }; return $s.ptr + $o } }
        -1 }
    $names = New-Object System.Collections.Generic.HashSet[string]
    $erva = [long][BitConverter]::ToUInt32($b, $dirs)
    $ed = if ($erva) { & $off $erva } else { -1 }
    if ($ed -ge 0 -and $ed + 40 -le $n) {
        $cnt = [long][BitConverter]::ToUInt32($b, $ed + 24)
        $np = & $off ([long][BitConverter]::ToUInt32($b, $ed + 32))
        if ($np -ge 0 -and $cnt -le 65536) {
            for ($i = 0; $i -lt $cnt -and $np + 4 * $i + 4 -le $n; $i++) {
                $so = & $off ([long][BitConverter]::ToUInt32($b, $np + 4 * $i))
                if ($so -lt 0) { continue }
                $e = $so; while ($e -lt $n -and $b[$e] -ne 0 -and $e - $so -lt 512) { $e++ }
                [void]$names.Add([Text.Encoding]::ASCII.GetString($b, [int]$so, [int]($e - $so)))
            }
        }
    }
    @{ machine = $machine; names = $names }
}

# An Ultimate ASI Loader that can stand in for winmm.dll: 32-bit (the game is), exports IsUltimateASILoader (the name plugins and other loaders know it by) and
# winmm's timeGetTime (it serves winmm.dll). Both builds a player may have do: v10.0.2 and the Fusion Fix's v7.8.0 (2026-11-07).
function Is-AsiLoader($p) {
    $x = Pe-Exports $p
    [bool]($x -and $x.machine -eq 0x14C -and $x.names.Contains("IsUltimateASILoader") -and $x.names.Contains("timeGetTime"))
}

function Same-File($a, $b) {
    (Test-Path -LiteralPath $a -PathType Leaf) -and (Test-Path -LiteralPath $b -PathType Leaf) -and
        ((Get-Item -LiteralPath $a).Length -eq (Get-Item -LiteralPath $b).Length) -and ((Sha256-File $a) -eq (Sha256-File $b))
}

# The player's Ultimate ASI Loader keeps running next to the mod. It knows itself only by its file name: as dinput8_real.dll (the mod's chain rule) it shows
# "This library isn't supported." and closes the game (its source; native/chain_logic.h), so the copy is named winmm.dll, a name the loader serves (it forwards winmm's
# functions to the system winmm.dll) and rerev.exe imports: Windows loads it from the game folder at the start, the loader then loads its plugins (scripts\*.asi) as
# it does as dinput8.dll. The original stays in coop-backup\dinput8.dll; the loader's own settings file dinput8.ini goes along as winmm.ini (it reads <its name>.ini).
# Copy -> where it comes from (relative to the game folder).
$script:CHAIN = [ordered]@{ "winmm.dll" = "coop-backup\dinput8.dll"; "winmm.ini" = "dinput8.ini" }

# What the install does for a foreign dinput8.dll kept in coop-backup: state "asi" (the loader runs as winmm.dll), "off" (the file stays off: not such a loader, or a
# winmm.dll of someone else's is in the folder already), "" (none); keep = copies of an earlier install that are still the same as their source; make = copies to make.
# A copy that is no longer the same as its source is the player's now: it is left alone and no longer listed.
function Chain-Plan($g, $old, $backups) {
    $plan = @{ state = ""; keep = @(); make = @() }
    if ($backups -notcontains "dinput8.dll") { return $plan }
    $plan.state = "off"
    if (-not (Is-AsiLoader (Join-Path $g "coop-backup\dinput8.dll"))) { return $plan }
    foreach ($name in $script:CHAIN.Keys) {
        $from = Join-Path $g $script:CHAIN[$name]; $dst = Join-Path $g $name
        if (-not (Test-Path -LiteralPath $from -PathType Leaf)) { continue }
        if (Test-Path -LiteralPath $dst) {
            if (($old.chains -contains $name) -and (Same-File $dst $from)) { $plan.keep += $name }
            elseif ($name -eq "winmm.dll") { Ui-Log (L "winmmtaken"); $plan.keep = @(); $plan.make = @(); return $plan }
        } else { $plan.make += $name }
        # (the settings file only goes along with the loader)
        if ($name -eq "winmm.dll" -and $plan.keep -notcontains $name -and $plan.make -notcontains $name) { return $plan }
    }
    $plan.state = "asi"
    $plan
}

# The game running from this folder (a copy elsewhere does not lock these files).
function Game-Running($g) {
    $g = [IO.Path]::GetFullPath($g).TrimEnd('\') + "\"
    Get-Process -Name rerev -ErrorAction SilentlyContinue |
        Where-Object { -not $_.Path -or $_.Path.StartsWith($g, [StringComparison]::OrdinalIgnoreCase) }
}

# The window's icon: the co-op one once made, else the game's.
function Window-Icon($g) {
    try {
        $ico = Join-Path $g "coop\coop.ico"
        if (Test-Path -LiteralPath $ico) { return New-Object Drawing.Icon($ico, 32, 32) }
        [Drawing.Icon]::ExtractAssociatedIcon((Join-Path $g "rerev.exe"))
    } catch { $null }
}

# ---------------------------------------------------------------------------------------------
# Remove

function Do-Uninstall($g) {
    $m = Read-Manifest $g
    if ($m.files.Count -eq 0) { throw (L "notmod" $g) }
    Lock-Install
    Ui-Step "rm" "run"
    $all = @($m.files)
    $leftIn = New-Object System.Collections.Generic.List[string]
    $i = 0
    foreach ($rel in $all) {
        $i++
        Ui-Progress (5 + 50 * $i / [Math]::Max(1, $all.Count)) $rel
        # (a copy made for the player's ASI loader goes only while it is still the same as its source: the loader in coop-backup, the player's dinput8.ini)
        if ($m.chains -contains $rel -and (Test-Path -LiteralPath (Join-Path $g $rel)) -and -not (Same-File (Join-Path $g $rel) (Join-Path $g $script:CHAIN[$rel]))) {
            Ui-Log ((L "leftalone") + $rel); continue
        }
        # (a dinput8.dll another mod put over the co-op one after the install is the player's now: it stays, and the older original in coop-backup is not put back over it)
        if ($rel -eq "dinput8.dll" -and (Test-Path -LiteralPath (Join-Path $g $rel)) -and -not (Is-CoopDll (Join-Path $g $rel))) {
            Ui-Log ((L "leftin") + $rel); $leftIn.Add($rel); continue
        }
        Remove-Item -LiteralPath (Join-Path $g $rel) -Force -ErrorAction SilentlyContinue
    }
    Ui-Log (L "nrem" $($all.Count))
    # (only "Resident Evil Revelations Co-op.lnk" files the install made: the record names them)
    foreach ($lnk in $m.shortcuts) {
        if ($lnk -like "*.lnk" -and (Test-Path -LiteralPath $lnk)) { Remove-Item -LiteralPath $lnk -Force -ErrorAction SilentlyContinue; Ui-Log ((L "lnk") + $lnk) }
    }
    Ui-Step "rm" "done"
    Ui-Step "restore" "run"
    $i = 0
    foreach ($rel in $m.backups) {
        $i++
        Ui-Progress (55 + 35 * $i / [Math]::Max(1, $m.backups.Count)) $rel
        if ($leftIn -contains $rel) { continue }
        $b = Join-Path $g "coop-backup\$rel"
        if (Test-Path -LiteralPath $b) { Copy-Item -LiteralPath $b (Join-Path $g $rel) -Force; Ui-Log ((L "putback") + $rel) }
    }
    Ui-Step "restore" "done"
    Ui-Step "clean" "run"
    Ui-Progress 95 "coop, coop-backup, rev1_coop.log"
    Remove-Item -LiteralPath (Join-Path $g "coop") -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $g "coop-backup") -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $g "Uninstall Co-op.bat") -Force -ErrorAction SilentlyContinue
    # What the mod's DLL itself leaves next to rerev.exe when the game ran: its log (rev1_coop.log, the older starts rev1_coop.1.log ... .4.log)
    # and, after a screenshot (F11), rev1_coop\shots. The logs go; the two folders only when empty (screenshots are the player's own).
    foreach ($f in @(Get-ChildItem -LiteralPath $g -Filter "rev1_coop*.log" -File -ErrorAction SilentlyContinue)) {
        if ($f.Name -match '^rev1_coop(\.[0-9])?\.log$') { Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue }
    }
    foreach ($d in (Join-Path $g "rev1_coop\shots"), (Join-Path $g "rev1_coop")) {
        if ((Test-Path -LiteralPath $d -PathType Container) -and -not @(Get-ChildItem -LiteralPath $d -Force -ErrorAction SilentlyContinue).Count) {
            Remove-Item -LiteralPath $d -Force -ErrorAction SilentlyContinue
        }
    }
    Ui-Step "clean" "done"
    Ui-Progress 100 ""
    @{ removed = $m.version; left = $leftIn.Count }
}

# A command line argument as the C runtime parses it back.
function Quote-Arg($a) {
    if ($a -and $a -notmatch '[\s"]') { return $a }
    '"' + (($a -replace '(\\*)"', '$1$1\"') -replace '(\\+)$', '$1$1') + '"'
}

function New-CoopIcon($exe, $ico) {
    $frames = @()
    foreach ($size in 16, 24, 32, 48, 64, 128, 256) {
        $h = New-Object IntPtr[] 1; $id = New-Object UInt32[] 1
        $n = [CoopWin]::PrivateExtractIcons($exe, 0, $size, $size, $h, $id, 1, 0)
        if ($n -lt 1 -or $h[0] -eq [IntPtr]::Zero) { throw "no icon in $exe" }
        $src = [Drawing.Icon]::FromHandle($h[0]).ToBitmap()
        [CoopWin]::DestroyIcon($h[0]) | Out-Null
        $bmp = New-Object Drawing.Bitmap($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $gr = [Drawing.Graphics]::FromImage($bmp)
        $gr.SmoothingMode = "AntiAlias"; $gr.InterpolationMode = "HighQualityBicubic"
        $gr.DrawImage($src, 0, 0, $size, $size)
        # (bigger on the small sizes, where it has to be seen at all)
        $d = [single]($size * $(if ($size -le 24) { 0.62 } else { 0.46 }))
        $x = [single]($size - $d); $y = [single]($size - $d)
        $rim = [single][Math]::Max(1, $size / 32)
        $gr.FillEllipse((New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(24, 24, 28))), $x, $y, $d, $d)
        $gr.FillEllipse((New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(196, 40, 48))), $x + $rim, $y + $rim, $d - 2 * $rim, $d - 2 * $rim)
        # head and shoulders twice, the one behind a little higher and paler
        $u = $d / 10
        foreach ($f in @(@(6.1, 3.0, [Drawing.Color]::FromArgb(240, 200, 200)), @(3.9, 3.6, [Drawing.Color]::White))) {
            $br = New-Object Drawing.SolidBrush($f[2])
            $cx = $x + $f[0] * $u; $top = $y + $f[1] * $u
            $gr.FillEllipse($br, [single]($cx - 1.25 * $u), [single]$top, [single](2.5 * $u), [single](2.5 * $u))
            $path = New-Object Drawing.Drawing2D.GraphicsPath
            $path.AddPie([single]($cx - 2.4 * $u), [single]($top + 2.7 * $u), [single](4.8 * $u), [single](4.2 * $u), [single]180, [single]180)
            $gr.FillPath($br, $path)
            $path.Dispose(); $br.Dispose()
        }
        $gr.Dispose(); $src.Dispose()
        $frames += , @($size, $bmp)
    }
    # ICO: 256 as PNG, the others as 32-bit DIBs (what both the shell and .NET's Icon read).
    $blobs = @()
    foreach ($fr in $frames) {
        $size, $bmp = $fr
        $p = New-Object IO.MemoryStream
        if ($size -ge 256) { $bmp.Save($p, [Drawing.Imaging.ImageFormat]::Png) } else {
            $bw = New-Object IO.BinaryWriter($p)
            $mask = [int]([Math]::Ceiling($size / 32.0) * 4)
            $bw.Write([UInt32]40); $bw.Write([Int32]$size); $bw.Write([Int32](2 * $size)); $bw.Write([UInt16]1); $bw.Write([UInt16]32)
            $bw.Write([UInt32]0); $bw.Write([UInt32]($size * $size * 4 + $mask * $size))
            $bw.Write([Int32]0); $bw.Write([Int32]0); $bw.Write([UInt32]0); $bw.Write([UInt32]0)
            $data = $bmp.LockBits((New-Object Drawing.Rectangle(0, 0, $size, $size)), [Drawing.Imaging.ImageLockMode]::ReadOnly, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
            $px = New-Object byte[] ($data.Stride * $size)
            [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $px, 0, $px.Length)
            $bmp.UnlockBits($data)
            for ($row = $size - 1; $row -ge 0; $row--) { $bw.Write($px, $row * $data.Stride, $size * 4) }
            $bw.Write((New-Object byte[] ($mask * $size)))
            $bw.Flush()
        }
        $blobs += , $p.ToArray()
    }
    $ms = New-Object IO.MemoryStream
    $w = New-Object IO.BinaryWriter($ms)
    $w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$frames.Count)
    $off = 6 + 16 * $frames.Count
    for ($i = 0; $i -lt $frames.Count; $i++) {
        $size = $frames[$i][0]
        $w.Write([byte]($size % 256)); $w.Write([byte]($size % 256)); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]$blobs[$i].Length); $w.Write([UInt32]$off)
        $off += $blobs[$i].Length
    }
    foreach ($b in $blobs) { $w.Write($b) }
    $w.Flush()
    [IO.File]::WriteAllBytes($ico, $ms.ToArray())
    foreach ($fr in $frames) { $fr[1].Dispose() }
}


# "Resident Evil Revelations Co-op" shortcuts to coop\coop-launcher.ps1: on the desktop and in the Start menu (the window's install), those an earlier install
# made and the player kept (refreshed), or in -ShortcutDir. Returns their paths for the install record.
function Make-Shortcuts($g, $old) {
    $ico = Join-Path $g "coop\coop.ico"
    $iconAt = "$(Join-Path $g 'rerev.exe'),0"
    if ($script:haveWin) {
        try { New-CoopIcon (Join-Path $g "rerev.exe") $ico; $iconAt = "$ico,0" } catch { Ui-Log "coop.ico: $($_.Exception.Message)" }
    }
    $links = New-Object System.Collections.Generic.List[string]
    foreach ($lnk in $old.shortcuts) {
        if ((Test-Path -LiteralPath $lnk) -and -not $links.Contains($lnk)) { $links.Add($lnk) }
    }
    $dirs = @()
    if ($ShortcutDir) { $dirs = @($ShortcutDir -split ';' | Where-Object { $_ }) }
    elseif (-not $Quiet -and -not $NoShortcut) { $dirs = @([Environment]::GetFolderPath("Desktop"), [Environment]::GetFolderPath("Programs")) }
    foreach ($d in $dirs) {
        if (-not $d) { continue }
        New-Item -ItemType Directory -Force $d | Out-Null
        $lnk = Join-Path $d "Resident Evil Revelations Co-op.lnk"
        if (-not $links.Contains($lnk)) { $links.Add($lnk) }
    }
    $shell = New-Object -ComObject WScript.Shell
    foreach ($lnk in $links) {
        $sc = $shell.CreateShortcut($lnk)
        $sc.TargetPath = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
        $sc.Arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + (Join-Path $g "coop\coop-launcher.ps1") + '"'
        $sc.WorkingDirectory = $g
        $sc.IconLocation = $iconAt
        $sc.Description = L "lnkdesc"
        $sc.WindowStyle = 7
        $sc.Save()
        Ui-Log ((L "lnk") + $lnk)
    }
    $links.ToArray()
}

# One install or removal at a time (a double-clicked shortcut, a second installer window).
function Lock-Install {
    if (-not $script:installLock) { $script:installLock = New-Object Threading.Mutex($false, "Local\Re1CoopInstall") }
    try { $got = $script:installLock.WaitOne(0) } catch [Threading.AbandonedMutexException] { $got = $true }
    if (-not $got) { throw (L "another") }
}

# Program Files (the retail disc's default folder): writing there needs administrator rights.
function Need-Admin($g) {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    if ((New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { return $false }
    foreach ($d in @($g)) {
        $probe = Join-Path $d ("coop-write-test-" + $PID + ".tmp")
        try { [IO.File]::WriteAllText($probe, "1"); Remove-Item -LiteralPath $probe -Force }
        catch [UnauthorizedAccessException] { return $true }
        catch { }
    }
    $false
}

# This installer again with administrator rights for the same folder (Windows asks the player);
# its exit code, -1 when the player said no.
function Run-Elevated($g, [switch] $Remove) {
    $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Quote-Arg $PSCommandPath), "-Game", (Quote-Arg $g))
    if ($Remove) { $a += "-Uninstall" }
    if ($AutoClose) { $a += "-AutoClose" }
    if ($NoShortcut) { $a += "-NoShortcut" }
    if ($ShortcutDir) { $a += @("-ShortcutDir", (Quote-Arg $ShortcutDir)) }
    if ($Lang) { $a += @("-Lang", $Lang) }
    try {
        $p = Start-Process powershell -Verb RunAs -ArgumentList $a -Wait -PassThru
        return $p.ExitCode
    } catch { return -1 }
}


# ---------------------------------------------------------------------------------------------
# Install

function Do-Install($g) {
    Ui-Step "check" "run"
    Ui-Progress 1 $g
    if (-not (Test-Path -LiteralPath $payload)) { throw (L "nofiles") }
    if (-not (Exe-Ok $g)) { throw (L "badexe") }
    # The payload first into a folder of this installer's own: whatever happens to the folder it came in (an old launcher started twice cleans it up) cannot
    # take files away mid-copy.
    $snap = Join-Path ([IO.Path]::GetTempPath()) ("re1-coop-payload-" + $PID)
    Remove-Item -LiteralPath $snap -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item -LiteralPath $payload $snap -Recurse -Force
    $payload = $snap
    # (the package's list of its files: all of them here, or the game is not touched)
    $plist = Join-Path $payload "coop\payload.txt"
    if (-not (Test-Path -LiteralPath $plist)) { Ui-Log "missing: coop\payload.txt"; throw (L "incomplete") }
    $packageFiles = @(Get-Content -LiteralPath $plist | Where-Object { $_ })
    foreach ($rel in $packageFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $payload $rel))) { Ui-Log ("missing: " + $rel); throw (L "incomplete") }
    }
    if (-not $IgnoreRunning -and (Game-Running $g)) { throw (L "closegame") }
    Lock-Install
    $old = Read-Manifest $g
    if ($old.version) { Ui-Log ((L "have" $old.version $version)) }
    # (the mod's own DLL kept as the game's original by some earlier install: dropped)
    $bad = Join-Path $g "coop-backup\dinput8.dll"
    if (Is-CoopDll $bad) { Remove-Item -LiteralPath $bad -Force -ErrorAction SilentlyContinue }
    Ui-Step "check" "done"

    Ui-Step "copy" "run"
    $backups = New-Object System.Collections.Generic.List[string]
    foreach ($b in $old.backups) { $backups.Add($b) }
    $files = New-Object System.Collections.Generic.List[string]
    $all = @(Get-ChildItem -LiteralPath $payload -Recurse -File)
    # The install record as far as it got: a failure later on can still be removed or retried. coop\version.txt goes last, once everything is in: the launcher
    # reads it, and an update that broke off is tried again at the next start.
    $links = @($old.shortcuts)
    $chains = New-Object System.Collections.Generic.List[string]
    $writeRecord = {
        $lines = @("version=$version") + ($files | ForEach-Object { "file=$_" }) + ($backups | ForEach-Object { "backup=$_" }) + ($chains | ForEach-Object { "chain=$_" }) +
                 ($links | ForEach-Object { "shortcut=$_" })
        New-Item -ItemType Directory -Force (Join-Path $g "coop") | Out-Null
        Set-Content -LiteralPath (Join-Path $g "coop\install.txt") -Value $lines -Encoding UTF8
    }
    # A dinput8.dll another mod put over the co-op one after an earlier install (an ASI loader, a fix): the player's now, kept like any other file of someone
    # else's before the co-op DLL goes back in. It replaces an older original in coop-backup (the file the player put there last is the one the removal puts
    # back); copies made from that older one (Chain-Plan) go with it and are made again from this one.
    $proxyPath = Join-Path $g "dinput8.dll"
    if (($old.files -contains "dinput8.dll") -and (Test-Path -LiteralPath $proxyPath) -and -not (Is-CoopDll $proxyPath)) {
        foreach ($c in $old.chains) {
            $cp = Join-Path $g $c
            if (Same-File $cp (Join-Path $g $script:CHAIN[$c])) { Remove-Item -LiteralPath $cp -Force -ErrorAction SilentlyContinue }
        }
        $bk = Join-Path $g "coop-backup\dinput8.dll"
        New-Item -ItemType Directory -Force (Split-Path $bk -Parent) | Out-Null
        Copy-Item -LiteralPath $proxyPath $bk -Force
        if ($backups -notcontains "dinput8.dll") {
            $backups.Add("dinput8.dll")
            # (in the record at once, the earlier install's files with it: a run that breaks off still puts it back)
            foreach ($f in $old.files) { $files.Add($f) }
            foreach ($c in $old.chains) { $chains.Add($c) }
            & $writeRecord
            $files.Clear(); $chains.Clear()
        }
        Ui-Log ((L "overcoop") + "dinput8.dll")
    }
    $versionFile = $null
    $i = 0
    foreach ($item in $all) {
        $i++
        $rel = $item.FullName.Substring($payload.Length + 1)
        if ($rel -eq "coop\version.txt") { $versionFile = $item; $files.Add($rel); continue }
        $dst = Join-Path $g $rel
        Ui-Progress (3 + 70 * $i / $all.Count) $rel
        # A file of someone else's that we replace (another dinput8.dll: an ASI loader, a graphics fix): keep the original once (our own DLL of an earlier version is not one).
        if ((Test-Path -LiteralPath $dst) -and ($old.files -notcontains $rel) -and ($backups -notcontains $rel) -and -not $rel.StartsWith("coop\") -and
            -not (Is-CoopDll $dst)) {
            $bk = Join-Path $g "coop-backup\$rel"
            # A backup from an earlier, interrupted install is the real original: kept.
            if (-not (Test-Path -LiteralPath $bk)) {
                New-Item -ItemType Directory -Force (Split-Path $bk -Parent) | Out-Null
                Copy-Item -LiteralPath $dst $bk -Force
            }
            $backups.Add($rel)
            Ui-Log ((L "kept") + $rel)
        }
        New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
        Copy-Item -LiteralPath $item.FullName $dst -Force
        $files.Add($rel)
    }
    Ui-Log (L "copied" $($all.Count))
    # The player's ASI loader (kept in coop-backup) runs on as winmm.dll (Chain-Plan): the copies are in the record before they are made, so a run that breaks off
    # leaves nothing the uninstaller does not know.
    $plan = Chain-Plan $g $old $backups
    foreach ($nm in @($plan.keep) + @($plan.make)) { $chains.Add($nm); if ($files -notcontains $nm) { $files.Add($nm) } }
    if ($plan.make.Count) { & $writeRecord }
    foreach ($nm in $plan.make) { Copy-Item -LiteralPath (Join-Path $g $script:CHAIN[$nm]) (Join-Path $g $nm) -Force; Ui-Log ((L "asicopy") + $nm) }
    # Files of an older version that this one no longer has (by the package's list: never a file it has; a loader copy the player changed is his now).
    foreach ($rel in $old.files) {
        if ($packageFiles -notcontains $rel -and $files -notcontains $rel -and $old.chains -notcontains $rel) { Remove-Item -LiteralPath (Join-Path $g $rel) -Force -ErrorAction SilentlyContinue }
    }
    # The uninstaller: a .bat at the game folder's root that runs the installer kept in coop\.
    $un = "Uninstall Co-op.bat"
    Set-Content -LiteralPath (Join-Path $g $un) -Encoding ASCII -Value '@start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0coop\coop-installer.ps1" -Uninstall -Game "%~dp0."'
    if ($files -notcontains $un) { $files.Add($un) }
    & $writeRecord
    Ui-Step "copy" "done"

    Ui-Step "finish" "run"
    Ui-Progress 80 "coop\install.txt"
    $links = @()
    try { $links = @(Make-Shortcuts $g $old) } catch { Ui-Log "shortcut: $($_.Exception.Message)"; $links = @($old.shortcuts | Where-Object { Test-Path -LiteralPath $_ }) }
    & $writeRecord
    if ($versionFile) { Copy-Item -LiteralPath $versionFile.FullName (Join-Path $g "coop\version.txt") -Force }
    Remove-Item -LiteralPath $snap -Recurse -Force -ErrorAction SilentlyContinue
    Ui-Step "finish" "done"
    Ui-Progress 100 ""
    @{ old = $old.version; links = $links.Count; chain = $plan.state }
}

function Install-Message($g, $r) {
    $t = if ($r.links -gt 0) { L "done" } else { L "donenolink" }
    # (another mod's dinput8.dll: what became of it)
    if ($r.chain -eq "asi") { $t += "`n`n" + (L "asinote") } elseif ($r.chain -eq "off") { $t += "`n`n" + (L "offnote") }
    $t
}

function Save-Log($g) {
    $text = $script:logLines -join "`r`n"
    try { Set-Content -LiteralPath (Join-Path ([IO.Path]::GetTempPath()) "re1-coop-install.log") -Value $text -Encoding UTF8 } catch { }
    if ($g -and (Test-Path -LiteralPath (Join-Path $g "coop"))) {
        try { Set-Content -LiteralPath (Join-Path $g "coop\install-log.txt") -Value $text -Encoding UTF8 } catch { }
    }
}

# ---------------------------------------------------------------------------------------------
# -Quiet: the console only

if ($Quiet) {
    try {
        $g = $Game
        if ($g) { if (-not (Test-Game $g)) { throw (L "notgame" $g) }; $g = (Resolve-Path -LiteralPath $g).Path }
        else { $g = @(Find-Games)[0]; if (-not $g) { throw (L "nogame") } }
        if ($Uninstall) {
            $u = Do-Uninstall $g
            Write-Host ((L "gone") + $(if ($u.left) { "`n`n" + (L "leftnote") } else { "" }))
        } else {
            $r = Do-Install $g
            Save-Log $g
            Write-Host ((L "installed" $version) + " - " + $g + "`n" + (Install-Message $g $r))
        }
        exit 0
    } catch {
        Write-Host ((L "err") + $_.Exception.Message)
        Save-Log $g
        exit 1
    }
}

# ---------------------------------------------------------------------------------------------
# The window

if ($script:haveWin) {
    [CoopWin]::ShowWindow([CoopWin]::GetConsoleWindow(), 0) | Out-Null
    [CoopWin]::SetProcessDPIAware() | Out-Null
}
[System.Windows.Forms.Application]::EnableVisualStyles()

$C = @{
    bg = [Drawing.Color]::FromArgb(24, 24, 28); panel = [Drawing.Color]::FromArgb(34, 34, 40)
    text = [Drawing.Color]::FromArgb(232, 232, 236); dim = [Drawing.Color]::FromArgb(140, 140, 150)
    accent = [Drawing.Color]::FromArgb(196, 40, 48); accentHi = [Drawing.Color]::FromArgb(224, 62, 70)
    ok = [Drawing.Color]::FromArgb(84, 196, 122); warn = [Drawing.Color]::FromArgb(236, 176, 64); bad = [Drawing.Color]::FromArgb(236, 84, 84)
    hdr1 = [Drawing.Color]::FromArgb(86, 14, 20); hdr2 = [Drawing.Color]::FromArgb(18, 18, 22)
    btn = [Drawing.Color]::FromArgb(58, 58, 66); btnHi = [Drawing.Color]::FromArgb(76, 76, 86)
}
function Font($size, $style = "Regular", $name = "Segoe UI") { New-Object Drawing.Font($name, $size, [Drawing.FontStyle]$style) }

function New-Label($parent, $text, $x, $y, $w, $h, $font, $color) {
    $l = New-Object Windows.Forms.Label
    $l.Text = $text; $l.Location = New-Object Drawing.Point($x, $y); $l.Size = New-Object Drawing.Size($w, $h)
    $l.Font = $font; $l.ForeColor = $color; $l.BackColor = [Drawing.Color]::Transparent
    $l.UseMnemonic = $false
    $parent.Controls.Add($l)
    $l
}

function New-Button($parent, $text, $x, $y, $w, $primary) {
    $b = New-Object Windows.Forms.Button
    $b.Text = $text; $b.Location = New-Object Drawing.Point($x, $y); $b.Size = New-Object Drawing.Size($w, 36)
    $b.FlatStyle = "Flat"; $b.Font = Font 10 $(if ($primary) { "Bold" } else { "Regular" })
    $b.ForeColor = [Drawing.Color]::White; $b.Cursor = [Windows.Forms.Cursors]::Hand
    $b.BackColor = $(if ($primary) { $C.accent } else { $C.btn })
    $b.FlatAppearance.BorderSize = 0
    $b.FlatAppearance.MouseOverBackColor = $(if ($primary) { $C.accentHi } else { $C.btnHi })
    $b.UseMnemonic = $false
    $parent.Controls.Add($b)
    $b
}

# Test mode (never set for a player): RE1_COOP_UI_POS="x,y" puts the window at that screen position WITHOUT taking the focus (a form class that shows
# without activation, no Activate call), so a window test can run on the second monitor while the owner works on the first. RE1_COOP_UI_SHOTS=<folder> saves
# window pictures (Control.DrawToBitmap: the window's own pixels, never the desktop), RE1_COOP_UI_AUTO=install|remove drives the button, only for the one folder in
# RE1_COOP_UI_ONLY (a test copy), then the window closes.
$script:quietPos = $null
if ($env:RE1_COOP_UI_POS -match '^(\d+),(\d+)$') { $script:quietPos = New-Object Drawing.Point([int]$Matches[1], [int]$Matches[2]) }
if ($script:quietPos) {
    if (-not ("QuietCoopForm" -as [type])) {
        Add-Type -WarningAction SilentlyContinue -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition 'public class QuietCoopForm : System.Windows.Forms.Form { protected override bool ShowWithoutActivation { get { return true; } } }'
    }
    $form = New-Object QuietCoopForm
} else {
    $form = New-Object Windows.Forms.Form
}
$form.SuspendLayout()
$form.AutoScaleDimensions = New-Object Drawing.SizeF(96, 96)
$form.AutoScaleMode = "Dpi"
$form.Text = "$($script:TITLE) $version"
$form.ClientSize = New-Object Drawing.Size(660, 556)
$form.FormBorderStyle = "FixedSingle"; $form.MaximizeBox = $false
if ($script:quietPos) { $form.StartPosition = "Manual"; $form.Location = $script:quietPos; $form.ShowInTaskbar = $false } else { $form.StartPosition = "CenterScreen" }
$form.BackColor = $C.bg; $form.ForeColor = $C.text
$form.Font = Font 10

# Header: a dark red to black band, the title, a red line under it.
$hdr = New-Object Windows.Forms.Panel
$hdr.Location = New-Object Drawing.Point(0, 0); $hdr.Size = New-Object Drawing.Size(660, 96)
$hdr.Add_Paint({
    param($s, $e)
    $r = $s.ClientRectangle
    if ($r.Width -le 0) { return }
    $br = New-Object Drawing.Drawing2D.LinearGradientBrush($r, $C.hdr1, $C.hdr2, [single]0)
    $e.Graphics.FillRectangle($br, $r); $br.Dispose()
    $pen = New-Object Drawing.Pen($C.accent, [single]3)
    $e.Graphics.DrawLine($pen, 0, $r.Height - 2, $r.Width, $r.Height - 2); $pen.Dispose()
})
$form.Controls.Add($hdr)
$null = New-Label $hdr "REVELATIONS CO-OP" 26 16 470 40 (Font 22 "Bold") ([Drawing.Color]::White)
$lbSub = New-Label $hdr "" 28 58 600 24 (Font 10) ([Drawing.Color]::FromArgb(210, 190, 190))
# The language of the window.
$cbLang = New-Object Windows.Forms.ComboBox
$cbLang.DropDownStyle = "DropDownList"; $cbLang.FlatStyle = "Flat"
$cbLang.Location = New-Object Drawing.Point(480, 20); $cbLang.Size = New-Object Drawing.Size(154, 28)
$cbLang.BackColor = $C.hdr1; $cbLang.ForeColor = [Drawing.Color]::White; $cbLang.Font = Font 10
foreach ($k in $script:LANGS.Keys) { [void]$cbLang.Items.Add($script:LANGS[$k]) }
$cbLang.SelectedIndex = @($script:LANGS.Keys).IndexOf($script:lang)
$hdr.Controls.Add($cbLang)

# --- page 1: the folder and what will be done
$pSetup = New-Object Windows.Forms.Panel
$pSetup.Location = New-Object Drawing.Point(0, 96); $pSetup.Size = New-Object Drawing.Size(660, 460)
$form.Controls.Add($pSetup)
$lbFolder = New-Label $pSetup "" 28 22 400 22 (Font 10 "Bold") $C.text
$tbGame = New-Object Windows.Forms.TextBox
$tbGame.Location = New-Object Drawing.Point(28, 48); $tbGame.Size = New-Object Drawing.Size(474, 28)
$tbGame.BackColor = $C.panel; $tbGame.ForeColor = $C.text; $tbGame.BorderStyle = "FixedSingle"; $tbGame.Font = Font 10.5
$pSetup.Controls.Add($tbGame)
$btBrowse = New-Button $pSetup (L "browse") 512 45 120 $false
$lbInfo = New-Label $pSetup "" 28 96 604 286 (Font 10.5) $C.text
$btRemove = New-Button $pSetup (L "remove") 28 396 220 $false   # (i18n: 220 / 160 px hold the longest labels: pt "Remover o mod cooperativo", pl "Zainstaluj ponownie")
$btCancel = New-Button $pSetup (L "cancel") 342 396 120 $false
$btGo = New-Button $pSetup (L "install") 472 396 160 $true

# --- page 2: the steps and the progress
$pWork = New-Object Windows.Forms.Panel
$pWork.Location = New-Object Drawing.Point(0, 96); $pWork.Size = New-Object Drawing.Size(660, 460); $pWork.Visible = $false
$form.Controls.Add($pWork)
$lbWorkTitle = New-Label $pWork "" 28 18 470 30 (Font 14 "Bold") $C.text
$lbTime = New-Label $pWork "0:00" 500 24 132 22 (Font 10) $C.dim
$lbTime.TextAlign = "MiddleRight"
$steps = @{}
function Add-Steps($list) {
    foreach ($k in @($steps.Keys)) { $pWork.Controls.Remove($steps[$k].icon); $pWork.Controls.Remove($steps[$k].text) }
    $steps.Clear()
    $y = 62
    foreach ($s in $list) {
        $ic = New-Label $pWork ([string][char]0x25CB) 30 $y 24 26 (Font 11 "Bold" "Segoe UI Symbol") $C.dim
        $tx = New-Label $pWork $s[1] 58 ($y + 2) 574 24 (Font 10.5) $C.dim
        $steps[$s[0]] = @{ icon = $ic; text = $tx }
        $y += 30
    }
}
$lbPct = New-Label $pWork "0 %" 540 232 92 22 (Font 10 "Bold") $C.text
$lbPct.TextAlign = "MiddleRight"
$lbAction = New-Label $pWork "" 28 232 510 22 (Font 9.5) $C.dim
$lbAction.AutoEllipsis = $true
$bar = New-Object Windows.Forms.Panel
$bar.Location = New-Object Drawing.Point(28, 258); $bar.Size = New-Object Drawing.Size(604, 14); $bar.BackColor = $C.panel
$pWork.Controls.Add($bar)
$fill = New-Object Windows.Forms.Panel
$fill.Location = New-Object Drawing.Point(0, 0); $fill.Size = New-Object Drawing.Size(0, 14); $fill.BackColor = $C.accent
$bar.Controls.Add($fill)
$tbLog = New-Object Windows.Forms.TextBox
$tbLog.Location = New-Object Drawing.Point(28, 284); $tbLog.Size = New-Object Drawing.Size(604, 70)
$tbLog.Multiline = $true; $tbLog.ReadOnly = $true; $tbLog.ScrollBars = "Vertical"; $tbLog.BorderStyle = "None"
$tbLog.BackColor = $C.panel; $tbLog.ForeColor = $C.dim; $tbLog.Font = Font 8.5 "Regular" "Consolas"
$pWork.Controls.Add($tbLog)
$btLogCopy = New-Button $pWork (L "copylog") 28 402 200 $false
$btLogCopy.Visible = $false
$btFolder = New-Button $pWork (L "openfolder") 236 402 250 $false
$btFolder.Visible = $false
$btClose = New-Button $pWork (L "close") 502 402 130 $true
$btClose.Visible = $false

# --- page 3 (over page 2's steps): the result
$lbResIcon = New-Label $pWork "" 24 60 60 60 (Font 30 "Bold" "Segoe UI Symbol") $C.ok
$lbResIcon.Visible = $false
$lbResText = New-Label $pWork "" 92 66 540 160 (Font 10.5) $C.text
$lbResText.Visible = $false

$form.ResumeLayout($false)

$script:ui = $null
$script:busy = $false
$script:exitCode = 1
$script:target = ""
$script:t0 = Get-Date

$timer = New-Object Windows.Forms.Timer
$timer.Interval = 500
$timer.Add_Tick({
    if (-not $script:busy) { return }
    $s = [int]((Get-Date) - $script:t0).TotalSeconds
    $lbTime.Text = "{0}:{1:00}" -f [int][Math]::Floor($s / 60), ($s % 60)
    # the running step's dot pulses: something is being done
    $r = $script:ui.running
    if ($r) { $r.icon.ForeColor = $(if ($r.icon.ForeColor.ToArgb() -eq $C.accent.ToArgb()) { $C.accentHi } else { $C.accent }) }
})

$form.Add_FormClosing({
    param($sender, $ev)
    if ($script:busy) {
        $ev.Cancel = $true
        [Windows.Forms.MessageBox]::Show($form, (L "wait"), $script:TITLE, "OK", "Information") | Out-Null
    }
})

# The texts of the window in the chosen language.
function Set-Texts {
    $lbSub.Text = L "sub" $version
    $lbFolder.Text = L "folder"
    $btBrowse.Text = L "browse"; $btRemove.Text = L "remove"; $btCancel.Text = L "cancel"
    $btLogCopy.Text = L "copylog"; $btFolder.Text = L "openfolder"; $btClose.Text = L "close"
    Update-Setup
}
$cbLang.Add_SelectedIndexChanged({
    $script:lang = @($script:LANGS.Keys)[$cbLang.SelectedIndex]
    Set-Texts
})

# What page 1 says about the chosen folder.
function Update-Setup {
    $g = $tbGame.Text.Trim().Trim('"')
    $ok = $false
    $lines = @()
    $btRemove.Visible = $false; $btRemove.Tag = 0   # (Tag = what the page means to show: Visible reads false while the window is not shown)
    $btGo.Text = L "install"
    if (-not $g) {
        $lines += (L "nogame")
    } elseif (-not (Test-Game $g)) {
        $lines += ([string][char]0x2716 + "  " + (L "noexe"))
    } elseif (-not (Exe-Ok $g)) {
        $lines += ([string][char]0x2714 + "  " + (L "found"))
        $lines += ([string][char]0x2716 + "  " + (L "badexe"))
    } else {
        $ok = $true
        $lines += ([string][char]0x2714 + "  " + (L "found"))
        $m = Read-Manifest $g
        if ($m.files.Count -gt 0) {
            $btRemove.Visible = $true; $btRemove.Tag = 1
            if ($m.version -eq $version) {
                $lines += ([string][char]0x2022 + "  " + (L "same" $version))
                $btGo.Text = L "reinstall"
            } else {
                $lines += ([string][char]0x2022 + "  " + (L "have" $($m.version) $version))
                $btGo.Text = L "update"
            }
        } else { $btGo.Text = L "install" }
        $lines += ""
        $lines += (L "what")
        $lines += ""
        $lines += (L "need")
        $lines += ""
        $lines += (L "net")
    }
    $lbInfo.Text = $lines -join "`n"
    # (the uninstaller's page keeps asking its question when the language is switched; the install text was shown under the Remove button before, window test 2026-11-04)
    if ($Uninstall -and $ok) { $lbInfo.Text = (L "confirm" $g) }
    $lbInfo.ForeColor = $(if ($ok -or -not $g) { $C.text } else { $C.warn })
    $btGo.Enabled = $ok
    $btGo.BackColor = $(if ($ok) { $C.accent } else { $C.btn })
}

function Show-Work($title, $stepList) {
    $pSetup.Visible = $false
    $pWork.Visible = $true
    $lbWorkTitle.Text = $title
    Add-Steps $stepList
    $script:ui = @{ form = $form; bar = $bar; fill = $fill; pct = $lbPct; action = $lbAction; log = $tbLog; steps = $steps; colors = $C; running = $null }
    $script:t0 = Get-Date
    $script:busy = $true
    $cbLang.Enabled = $false
    $timer.Start()
    Ui-Progress 0 ""
}

function Show-Result($ok, $title, $text) {
    $script:busy = $false
    $timer.Stop()
    foreach ($k in @($steps.Keys)) { $steps[$k].icon.Visible = $false; $steps[$k].text.Visible = $false }
    $lbWorkTitle.Text = $title
    $lbWorkTitle.ForeColor = $(if ($ok) { $C.ok } else { $C.bad })
    $lbResIcon.Text = $(if ($ok) { [string][char]0x2714 } else { [string][char]0x2716 })
    $lbResIcon.ForeColor = $(if ($ok) { $C.ok } else { $C.bad })
    $lbResText.Text = $text
    $lbResIcon.Visible = $true; $lbResText.Visible = $true
    $lbAction.Text = ""
    if ($ok) { $fill.BackColor = $C.ok } else { $fill.BackColor = $C.bad; $fill.Width = $bar.ClientSize.Width; $lbPct.Text = "" }
    if ($script:haveWin) { [CoopWin]::Progress($form.Handle, $(if ($ok) { 0 } else { 4 }), 100) }
    $btClose.Visible = $true
    $btLogCopy.Visible = -not $ok
    $btFolder.Visible = $ok -and $script:target -and (Test-Path -LiteralPath $script:target)
    $btClose.Focus()
    $form.Activate()
}

# A folder Windows protects (Program Files): the same work by an elevated copy of this installer, this window closes with its result.
function Elevated-Instead($g, [switch] $Remove) {
    if (-not (Need-Admin $g)) { return $false }
    Ui-Log (L "noadmin")
    $form.Hide()
    $code = Run-Elevated $g -Remove:$Remove
    if ($code -lt 0) {
        $form.Show()
        Show-Result $false $(if ($Remove) { L "rfail" } else { L "ifail" }) (L "noadmin")
        return $true
    }
    $script:exitCode = $code
    $form.Close()
    $true
}

function Start-Install($g) {
    $g = (Resolve-Path -LiteralPath $g).Path
    if (Elevated-Instead $g) { return }
    $script:target = $g
    $ic = Window-Icon $g
    if ($ic) { $form.Icon = $ic }
    Show-Work (L "installing" $version) @(
        @("check", (L "s_check")),
        @("copy", (L "s_copy")),
        @("finish", (L "s_finish")))
    Ui-Log ($g)
    try {
        $r = Do-Install $g
        Save-Log $g
        $title = $(if ($r.old -and $r.old -ne $version) { L "updated" $($r.old) $version } else { L "installed" $version })
        $script:exitCode = 0
        $ic = Window-Icon $g
        if ($ic) { $form.Icon = $ic }
        Show-Result $true $title (Install-Message $g $r)
        # (the launcher's update: the game starts once this window is gone)
        if ($AutoClose) {
            $script:closer = New-Object Windows.Forms.Timer
            $script:closer.Interval = 2000
            $script:closer.Add_Tick({ $script:closer.Stop(); $form.Close() })
            $script:closer.Start()
        }
    } catch {
        Ui-Log ("ERROR: " + $_.Exception.Message)
        if ($script:ui.running) { $script:ui.running.icon.Text = [string][char]0x2716; $script:ui.running.icon.ForeColor = $C.bad }
        Save-Log $g
        Show-Result $false (L "ifail") ($_.Exception.Message + "`n`n" + (L "login" (Join-Path ([IO.Path]::GetTempPath()) "re1-coop-install.log")))
    }
}

function Start-Uninstall($g) {
    $g = (Resolve-Path -LiteralPath $g).Path
    if (Elevated-Instead $g -Remove) { return }
    $script:target = $g
    if (-not $IgnoreRunning -and (Game-Running $g)) {
        [Windows.Forms.MessageBox]::Show($form, (L "closegame"), $script:TITLE, "OK", "Warning") | Out-Null
        return
    }
    $ic = Window-Icon $g
    if ($ic) { $form.Icon = $ic }
    Show-Work (L "removing") @(
        @("rm", (L "s_rm")),
        @("restore", (L "s_restore")),
        @("clean", (L "s_clean")))
    try {
        $u = Do-Uninstall $g
        $script:exitCode = 0
        Show-Result $true (L "removed") ((L "gone") + $(if ($u.left) { "`n`n" + (L "leftnote") } else { "" }))
    } catch {
        Ui-Log ("ERROR: " + $_.Exception.Message)
        Save-Log ""
        Show-Result $false (L "rfail") $_.Exception.Message
    }
}

$tbGame.Add_TextChanged({ Update-Setup })
$btBrowse.Add_Click({
    $dlg = New-Object Windows.Forms.FolderBrowserDialog
    $dlg.Description = L "pick"
    $cur = $tbGame.Text.Trim()
    if ($cur -and (Test-Path -LiteralPath $cur)) { $dlg.SelectedPath = $cur }
    if ($dlg.ShowDialog($form) -eq "OK") { $tbGame.Text = $dlg.SelectedPath }
})
$btCancel.Add_Click({ $form.Close() })
$btClose.Add_Click({ $form.Close() })
$btGo.Add_Click({
    $g = $tbGame.Text.Trim().Trim('"')
    if (-not (Test-Game $g) -or -not (Exe-Ok $g)) { return }
    if (-not $IgnoreRunning -and (Game-Running $g)) {
        [Windows.Forms.MessageBox]::Show($form, (L "closegame"), $script:TITLE, "OK", "Warning") | Out-Null
        return
    }
    Start-Install $g
})
$btRemove.Add_Click({
    $g = $tbGame.Text.Trim().Trim('"')
    # (the uninstaller's page is the question already)
    if (-not $Uninstall) {
        $q = [Windows.Forms.MessageBox]::Show($form, (L "confirm" $g), $script:TITLE, "YesNo", "Question")
        if ($q -ne "Yes") { return }
    }
    Start-Uninstall $g
})
$btFolder.Add_Click({ if ($script:target) { Start-Process explorer.exe -ArgumentList ('"' + $script:target + '"') } })
$btLogCopy.Add_Click({ [Windows.Forms.Clipboard]::SetText(($script:logLines -join "`r`n") + "`r`n") })

# Started for a folder: remove asks first, install goes straight on.
# (the title bar's colour is taken as the window is created)
$form.Add_HandleCreated({ if ($script:haveWin) { [CoopWin]::DarkTitle($form.Handle) } })
$form.Add_Shown({
    if ($script:haveWin) { [CoopWin]::SetWindowTheme($tbLog.Handle, "DarkMode_Explorer", $null) | Out-Null }
    if (-not $script:quietPos) { $form.Activate() }
    if ($Game) {
        if (-not (Test-Game $Game)) {
            $tbGame.Text = $Game
            return
        }
        $tbGame.Text = (Resolve-Path -LiteralPath $Game).Path
        if ($Uninstall) {
            $m = Read-Manifest $tbGame.Text
            $lbInfo.Text = (L "confirm" $tbGame.Text)
            $btGo.Visible = $false
            $btRemove.Visible = $true
            $btRemove.BackColor = $C.accent
            $btRemove.Location = New-Object Drawing.Point(($btGo.Left - ($btRemove.Width - $btGo.Width)), $btGo.Top)
            $btCancel.Left = $btRemove.Left - $btCancel.Width - 10
            $tbGame.ReadOnly = $true; $btBrowse.Enabled = $false
            $btRemove.Focus()
        } else {
            Start-Install $tbGame.Text
        }
    } else {
        $found = @(Find-Games)
        if ($found.Count -ge 1) { $tbGame.Text = $found[0] } else { Update-Setup }
        $btGo.Focus()
    }
})

# Window test driver (RE1_COOP_UI_SHOTS / RE1_COOP_UI_AUTO, see the top of the form code; a player never sets them): pictures of the window itself in the languages of
# RE1_COOP_UI_LANGS, then the install / remove button for the one folder in RE1_COOP_UI_ONLY, a picture of the result page, and the window closes.
function Save-WinShot($tag) {
    if (-not $env:RE1_COOP_UI_SHOTS) { return }
    try {
        New-Item -ItemType Directory -Force $env:RE1_COOP_UI_SHOTS | Out-Null
        $bmp = New-Object Drawing.Bitmap($form.Width, $form.Height)
        $form.DrawToBitmap($bmp, (New-Object Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
        $bmp.Save((Join-Path $env:RE1_COOP_UI_SHOTS ($tag + ".png")), [Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        Write-Host ("window picture " + $tag)
    } catch { Write-Host ("window picture failed: " + $_) }
}
$form.Add_Shown({
    if (-not ($env:RE1_COOP_UI_SHOTS -or $env:RE1_COOP_UI_AUTO)) { return }
    $tag = if ($env:RE1_COOP_UI_TAG) { $env:RE1_COOP_UI_TAG } else { "win" }
    $form.Refresh(); [Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 500
    Save-WinShot ($tag + "-1-start")
    foreach ($lg in @(($env:RE1_COOP_UI_LANGS -split ',') | Where-Object { $_ })) {
        if ($script:LANGS.Contains($lg)) {
            $cbLang.SelectedIndex = @($script:LANGS.Keys).IndexOf($lg)
            Set-Texts; $form.Refresh(); [Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 300
            Save-WinShot ($tag + "-lang-" + $lg)
        }
    }
    if ($env:RE1_COOP_UI_AUTO) {
        $cur = $tbGame.Text.Trim().Trim('"')
        $only = $env:RE1_COOP_UI_ONLY
        $same = $false
        if ($only -and $cur) { try { $same = ([IO.Path]::GetFullPath($cur).TrimEnd('\') -ieq [IO.Path]::GetFullPath($only).TrimEnd('\')) } catch { } }
        if (-not $same) {
            Write-Host ("auto refused: the folder in the window (" + $cur + ") is not RE1_COOP_UI_ONLY (" + $only + ")")
        } elseif ($env:RE1_COOP_UI_AUTO -eq "install" -and -not $Game) {
            $btGo.PerformClick()
        } elseif ($env:RE1_COOP_UI_AUTO -eq "remove" -and $Uninstall) {
            $btRemove.PerformClick()
        }
        for ($i = 0; $i -lt 300 -and $script:busy; $i++) { [Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 200 }
        $form.Refresh(); [Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 400
        Save-WinShot ($tag + "-2-result")
    }
    $form.Close()
})

Set-Texts
# Tests (tests\test_installer.ps1): build everything above without showing the window, then run the texts and the page-1 state code for every language over the
# folders listed in RE1_COOP_UI_SELFTEST (';' separated; "" = no folder) and print one line per language and folder: lang|folder|info text|install button|remove visible.
if ($env:RE1_COOP_UI_SELFTEST -ne $null) {
    [Console]::OutputEncoding = [Text.Encoding]::UTF8
    foreach ($f in ($env:RE1_COOP_UI_SELFTEST -split ';')) {
        foreach ($lg in $script:LANGS.Keys) {
            $script:lang = $lg
            $tbGame.Text = $f
            Set-Texts
            Write-Host ("UI|" + $lg + "|" + $f + "|" + ($lbInfo.Text -replace "`n", " // ") + "|" + $btGo.Text + "|" + $btRemove.Tag + "|" + $btGo.Enabled + "|" + $lbSub.Text + "|" + $btBrowse.Text + "|" + $btCancel.Text)
        }
    }
    exit 0
}
[Windows.Forms.Application]::Run($form)
exit $script:exitCode
