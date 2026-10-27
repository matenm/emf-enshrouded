# A game session for tests on this PC, in one command each (docs/modding/ai_agents.md):
#   game_session.ps1 start -LockOwner <you> [-Mods <folder>[=<id>],...] [-Character check|local|switch]
#                          [-CharacterName <name>] [-WorldName <name>]
#   game_session.ps1 stop -LockOwner <you> [-KeepInstalled]
#   game_session.ps1 status [-Json]
#
# What it records (so it never guesses which game is its own):
# - the session file (-StateFile; default EMF_GAME_SESSION or %LOCALAPPDATA%\EMF\game_session\session.json): the game
#   process start launched (pid and start time), the pid a menu screenshot was shown for, and this script's last
#   input. Any other game is someone else's: start and stop refuse it (-AdoptGame takes it over), and the MCP server's
#   tools that change the game refuse it too.
# - the game lock (-LockFile; default EMF_GAME_LOCK or %LOCALAPPDATA%\EMF\game_session\game.lock), always checked:
#   "<owner> <time> pid=<game pid>". Someone else's lock refuses start and stop (-IgnoreLock); a lock whose pid no
#   longer runs, or an empty one older than a minute, is stale and taken over.
# - a marker in the game folder (emf\game_session.json): EMF that start installed, and the -Mods folders it made. start
#   refuses an EMF it did not install (a modder's or the user's own), and stop uninstalls only with the marker.
#
# start:
#   1. refuses while a person uses the PC: input in the last 2 minutes (-IdleLimit) that was not this script's
#      (-IgnoreIdle when the user hands the PC over: their input from before this run is accepted);
#   2. save_guard.ps1 -Check: stops when the Steam Cloud saves changed (recording them again is the user's step);
#   3. installs the dev build with the debug bridge on (install_emf.ps1 -Bridge), plus -Mods folders (template_lua
#      goes in as its mod.json id, my_first_mod; <folder>=<id> names the target);
#   4. sets windowed mode, starts the game through Steam, waits for the bridge and the main menu;
#   5. always stops at the main menu after it launched the game (exit 2), with a screenshot to check which character
#      it shows. -Character local goes on only for a game whose menu screenshot this script showed before (the
#      session file says so); -Character switch clicks Change > LOCAL > slot 1, Esc twice, and stops with a new
#      screenshot (clicks for a 3840x2160 screen: load_world selects the LOCAL character itself);
#   6. with -Character local (after the person check): starts LOCAL world slot 1 with LOCAL character slot 1, which
#      must be named -CharacterName, through the bridge's load_world: no clicks, any screen, and the game refuses any
#      Steam Cloud save meanwhile. A refusal, a timeout or no answer stops start (exit 1, no clicks). Only a bridge
#      without load_world (an older build), or one whose load_world is off on this game build, falls back to the
#      clicks: it checks the screen and window size the click coordinates are for (3840x2160), clicks Play > Start a
#      Session > slot 1 > Offline Start, and stops if anyone else touches the mouse or keyboard. The output says which
#      way it went. Either way it watches the world's load event: a world in any other slot than the LOCAL slot 1 is
#      ended at once, before it runs, then the saves are checked and EMF removed; so is a world (load_world's reply,
#      then the state's world.name) that is not -WorldName, and one the state's character says runs with a Steam
#      character or another one than -CharacterName. A character the bridge cannot tell stops start (exit 1, nothing
#      ended). Then it waits for the player and prints the time it took.
#   A start that fails before the game runs removes what it installed; Ctrl+C prints what to run.
# stop: asks the game to quit through the bridge (it saves and exits in a few seconds) and waits for it. A game that
#   does not close is ended only when no world runs, or its world is the LOCAL slot 1 (after a "world saved" line and
#   15 s more), or with -AllowKill; otherwise it is left running and stop says so. Then it uninstalls EMF (only
#   game_session's install), sets Fullscreen, runs save_guard -Check and releases the lock (kept when the uninstall
#   failed). It refuses a game whose bridge does not answer (-AllowUnansweredBridge) and someone else's game.
# status: the game, the bridge, the world, the player, the install, the window mode, the lock, the session, the
#   person check.
# The game folder: -Game, else EMF_GAME_DIR, else the Steam library that has the game (scripts\find_game.ps1).
# Exit codes: 0 done (start: the world runs), 1 a step failed (stop: or the saves changed, or the game was left
# running), 2 start stopped at the main menu for the character check, 3 refused, 4 a wrong command line (or no game
# folder found: pass -Game).
param(
    [Parameter(Position = 0)] [string]$Command = "",
    [string]$Game = "",
    [string[]]$Mods = @(),
    [ValidateSet("check", "local", "switch")] [string]$Character = "check",
    [string]$LockFile = "",
    [string]$LockOwner = "",
    [string]$StateFile = "",
    [int]$IdleLimit = 120,
    [int]$WorldTimeoutS = 300,
    [ValidateNotNullOrEmpty()] [string]$CharacterName = "Character 2",
    [ValidateNotNullOrEmpty()] [string]$WorldName = "My World 1",
    [switch]$KeepInstalled,
    [switch]$Json,
    [switch]$IgnoreIdle,
    [switch]$IgnoreLock,
    [switch]$AllowUnansweredBridge,
    [switch]$AdoptGame,
    [switch]$AllowKill,
    [string]$Settings = (Join-Path $env:USERPROFILE "Saved Games\keen games\Enshrouded\local_settings.json")
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "find_game.ps1")  # Resolve-GameDir
$sessionRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$script:bridgePs = Join-Path $PSScriptRoot "emf_bridge.ps1"
$script:ui = Join-Path $PSScriptRoot "ui.ps1"
$script:shots = Join-Path $env:TEMP "emf_game_session"
$MenuMarker = "[GameUi] Entered PreGame"
$MarkerName = "game_session.json"  # in <game>\emf\
$SwitchClicks = @(
    @{ name = "Change"; x = 2534; y = 891 },
    @{ name = "the LOCAL tab"; x = 2331; y = 230 },
    @{ name = "slot 1 (it jumps to Play selection)"; x = 2419; y = 334 }
)
$GameSessionDir = Join-Path $(if ($env:LOCALAPPDATA) { $env:LOCALAPPDATA } else { $env:TEMP }) "EMF\game_session"
if (-not $LockFile) { $LockFile = if ($env:EMF_GAME_LOCK) { $env:EMF_GAME_LOCK } else { Join-Path $GameSessionDir "game.lock" } }
if (-not $StateFile) { $StateFile = if ($env:EMF_GAME_SESSION) { $env:EMF_GAME_SESSION } else { Join-Path $GameSessionDir "session.json" } }
if (-not $LockOwner -and $env:EMF_GAME_LOCK_OWNER) { $LockOwner = $env:EMF_GAME_LOCK_OWNER }

# ================================================================ decisions (no game needed: scripts/game_session_test.ps1)
# Someone uses the PC: the last input came within $Limit seconds and was not this script's own (input up to
# $OwnInput + 0.5 s is). With -IgnoreIdle, input from before this run ($RunStart) is the user's hand-over. Times are
# seconds since 1970 (UTC). Which window is in front does not matter: a person may be playing the game itself.
function Get-PersonVerdict($LastInput, $Now, $OwnInput, [int]$Limit, [bool]$IgnoreIdle, $RunStart) {
    if ($null -ne $OwnInput -and $LastInput -le $OwnInput + 0.5) { return $null }
    if ($Now - $LastInput -ge $Limit) { return $null }
    if ($IgnoreIdle -and $LastInput -le $RunStart) { return $null }
    return ("someone used the PC {0:N0} s ago (not this script's input; the limit is $Limit s): not touching the " +
        "game. Try again later; -IgnoreIdle is for when the user hands the PC over.") -f ($Now - $LastInput)
}

# During clicks: input later than $Since (this script's last input, or when the person check passed) is a person's.
# No switch skips this.
function Test-ForeignInput($LastInput, $Since) { return $LastInput -gt $Since + 0.5 }

function Read-Utf8([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $bom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    $skip = if ($bom) { 3 } else { 0 }
    return @{ text = (New-Object Text.UTF8Encoding($false)).GetString($bytes, $skip, $bytes.Length - $skip); bom = $bom }
}

function Get-WindowMode([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $m = [regex]::Match((Read-Utf8 $Path).text, '"windowMode"\s*:\s*"([^"]*)"')
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}

# Sets "windowMode" in the game's local_settings.json and changes nothing else (not the BOM, not the line ends).
# Returns the mode it had.
function Set-WindowMode([string]$Path, [string]$Mode) {
    $f = Read-Utf8 $Path
    $m = [regex]::Match($f.text, '"windowMode"\s*:\s*"([^"]*)"')
    if (-not $m.Success) { throw "$Path has no windowMode setting" }
    $before = $m.Groups[1].Value
    if ($before -ne $Mode) {
        $g = $m.Groups[1]
        $text = $f.text.Substring(0, $g.Index) + $Mode + $f.text.Substring($g.Index + $g.Length)
        [IO.File]::WriteAllText($Path, $text, (New-Object Text.UTF8Encoding($f.bom)))
    }
    return $before
}

# -Mods "<folder>[=<id>]": a folder (relative to the repository) installed as <game>\emf\mods\<id>. The id is the one
# after '=', else the folder's mod.json id, else the folder's name.
function Resolve-ModSpec([string]$Spec, [string]$Root) {
    $src = $Spec
    $id = ""
    $eq = $Spec.LastIndexOf("=")
    if ($eq -gt 0) {
        $src = $Spec.Substring(0, $eq)
        $id = $Spec.Substring($eq + 1)
    }
    $full = if ([IO.Path]::IsPathRooted($src)) { $src } else { Join-Path $Root $src }
    if (-not (Test-Path -LiteralPath $full -PathType Container)) { throw "no mod folder $full (from -Mods $Spec)" }
    $full = (Resolve-Path -LiteralPath $full).Path
    $manifestId = $null
    $manifest = Join-Path $full "mod.json"
    if (Test-Path -LiteralPath $manifest) {
        try { $manifestId = [string](Get-Content -Raw -LiteralPath $manifest -Encoding UTF8 | ConvertFrom-Json).id }
        catch { throw "$manifest is not valid JSON: $($_.Exception.Message)" }
    }
    if (-not $id) { $id = if ($manifestId) { $manifestId } else { Split-Path -Leaf $full } }
    # (not "." or "..": the target folder would be emf\mods or emf itself, which an install replaces)
    if ($id -cnotmatch '^[a-z0-9][a-z0-9_.-]{0,47}$') {
        throw "'$id' is not a mod id (1-48 of a-z 0-9 _ . -, starting with a letter or digit), from -Mods $Spec"
    }
    $warning = $null
    if ($manifestId -and $manifestId -ne $id) { $warning = "its mod.json says id '$manifestId', the folder is '$id'" }
    return @{ Source = $full; Id = $id; Warning = $warning }
}

# The install marker: <game>\emf\game_session.json, written when start installs ({owner, time, mods: [folders made by
# -Mods]}). $null without one.
function Read-InstallMarker([string]$GameDir) {
    $p = Join-Path $GameDir "emf\$MarkerName"
    if (-not (Test-Path -LiteralPath $p)) { return $null }
    try { return Get-Content -Raw -LiteralPath $p | ConvertFrom-Json } catch { return [pscustomobject]@{ owner = "?"; mods = @() } }
}

function Write-InstallMarker([string]$GameDir, [string]$Owner, [string[]]$ModsMade) {
    $data = [ordered]@{ owner = $Owner; time = (Get-Date -Format "s"); mods = @($ModsMade | Where-Object { $_ }) }
    [IO.File]::WriteAllText((Join-Path $GameDir "emf\$MarkerName"), ($data | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
}

# Copies each -Mods folder into <game>\emf\mods\<id>\. A folder that is there already is replaced only when an earlier
# start made it (the marker lists it); another one (a mod EMF installed, or the user's) is refused. Returns
# @{ lines; made } (made: every folder -Mods made, earlier ones included).
function Install-ExtraMods([string[]]$Specs, [string]$GameDir, [string]$Root) {
    $marker = Read-InstallMarker $GameDir
    $made = @(if ($marker -and $marker.mods) { $marker.mods })
    $lines = @()
    foreach ($spec in $Specs) {
        if (-not $spec) { continue }
        $m = Resolve-ModSpec $spec $Root
        $modsDir = [IO.Path]::GetFullPath((Join-Path $GameDir "emf\mods"))
        $to = [IO.Path]::GetFullPath((Join-Path $modsDir $m.Id))
        if ((Split-Path -Parent $to) -ne $modsDir) { throw "-Mods $spec would not land in a folder of $modsDir" }
        if (Test-Path -LiteralPath $to) {
            if ($made -notcontains $m.Id) { throw "emf\mods\$($m.Id) is there already and -Mods did not make it: not replacing it (pick another id with <folder>=<id>)" }
            Remove-Item -LiteralPath $to -Recurse -Force
        }
        New-Item -ItemType Directory -Force $to | Out-Null
        Copy-Item -Path (Join-Path $m.Source "*") -Destination $to -Recurse -Force
        if ($made -notcontains $m.Id) { $made += $m.Id }
        $lines += "installed $($m.Source) as emf\mods\$($m.Id)" + $(if ($m.Warning) { " ($($m.Warning))" } else { "" })
    }
    return @{ lines = $lines; made = $made }
}

# The game lock: "<owner> <time> pid=<n>". State: free, ours, held (someone else's, or empty and new), stale (its pid
# no longer runs, or empty and older than a minute: taken over).
function Get-LockState([string]$Path, [string]$Owner, [scriptblock]$PidAlive) {
    if (-not (Test-Path -LiteralPath $Path)) { return @{ state = "free"; text = "" } }
    $text = "$(Get-Content -Raw -LiteralPath $Path -ErrorAction SilentlyContinue)".Trim()
    if (-not $text) {
        $old = (Get-Item -LiteralPath $Path).LastWriteTime -lt (Get-Date).AddMinutes(-1)
        return @{ state = $(if ($old) { "stale" } else { "held" }); text = "(empty)" }
    }
    if (($text -split '\s+')[0] -eq $Owner) { return @{ state = "ours"; text = $text } }
    if ($text -match '\bpid=(\d+)\b' -and -not (& $PidAlive ([int]$Matches[1]))) { return @{ state = "stale"; text = $text } }
    return @{ state = "held"; text = $text }
}

function Write-GameLock([string]$Path, [string]$Owner, [int]$GamePid, [switch]$New) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $Path) | Out-Null
    $b = [Text.Encoding]::ASCII.GetBytes("$Owner $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') pid=$GamePid")
    $mode = if ($New) { [IO.FileMode]::CreateNew } else { [IO.FileMode]::Create }
    $fs = [IO.File]::Open($Path, $mode, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $fs.Write($b, 0, $b.Length) } finally { $fs.Close() }
}

# Takes the lock (CreateNew, so two takers cannot both win). @{ ok; text }.
function Enter-GameLock([string]$Path, [string]$Owner, [int]$GamePid, [scriptblock]$PidAlive) {
    $s = Get-LockState $Path $Owner $PidAlive
    switch ($s.state) {
        "ours" { Write-GameLock $Path $Owner $GamePid; return @{ ok = $true; text = "the game lock is ours already" } }
        "held" { return @{ ok = $false; text = "the game lock $Path is held ('$($s.text)'): someone else uses the game. Wait and try again." } }
        "stale" { Remove-Item -LiteralPath $Path -Force; $note = " (took over a stale lock: '$($s.text)')" }
        default { $note = "" }
    }
    try { Write-GameLock $Path $Owner $GamePid -New }
    catch {
        if (Test-Path -LiteralPath $Path) { return @{ ok = $false; text = "someone took the game lock $Path at the same moment" } }
        throw
    }
    return @{ ok = $true; text = "took the game lock $Path$note" }
}

function Exit-GameLock([string]$Path, [string]$Owner) {
    if (-not (Test-Path -LiteralPath $Path)) { return "no game lock to release" }
    $held = "$(Get-Content -Raw -LiteralPath $Path -ErrorAction SilentlyContinue)".Trim()
    if (($held -split '\s+')[0] -ne $Owner) { return "left the game lock alone: it is '$held', not ours" }
    Remove-Item -LiteralPath $Path -Force
    return "released the game lock"
}

# The session file: what start recorded about the game it launched.
function Read-SessionState([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try { return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json } catch { return $null }
}

function Write-SessionState([string]$Path, $Data) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $Path) | Out-Null
    $tmp = "$Path.tmp"
    [IO.File]::WriteAllText($tmp, ($Data | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $tmp -Destination $Path -Force
}

# The running game is the one the session recorded: the same pid and start time (a pid can be reused).
function Test-OurGame($Session, $GameProc) {
    if (-not $Session -or -not $GameProc) { return $false }
    if ([int]$Session.game_pid -ne [int]$GameProc.Id) { return $false }
    return [Math]::Abs([double]$Session.game_start_epoch - [double]$GameProc.StartEpoch) -lt 2.0
}

# Which way start goes at the main menu: show (screenshot, exit 2), switch, or local (the world clicks). -Character
# local counts only for a game this script did not launch in this run and whose menu screenshot it showed before.
function Get-MenuDecision([string]$Character, [bool]$LaunchedThisRun, $Session, [int]$GamePid) {
    if ($Character -eq "switch") { return "switch" }
    if ($Character -eq "local" -and -not $LaunchedThisRun -and $Session -and $null -ne $Session.menu_shown_pid -and
        [int]$Session.menu_shown_pid -eq $GamePid) { return "local" }
    return "show"
}

# The bridge's state.world.slot is the world's save slot id: low byte = slot + 1, high byte = storage (0 LOCAL,
# 1 Steam). The test world is the first LOCAL slot: 1.
function Test-WorldSlot($Slot) {
    if ($null -eq $Slot) {
        return @{ ok = $false; kill = $false; text = "cannot tell which save slot loaded (the bridge has no slot), so the LOCAL test world is not confirmed: check the screen yourself" }
    }
    $s = [int]$Slot
    if (($s -band 0xFF00) -ne 0) {
        return @{ ok = $false; kill = $true; text = ("a Steam Cloud world loaded (slot 0x{0:x4}), not the LOCAL test world" -f $s) }
    }
    if ($s -ne 1) { return @{ ok = $false; kill = $true; text = ("another LOCAL world loaded (slot 0x{0:x4}), not the LOCAL slot 1 (0x0001)" -f $s) } }
    return @{ ok = $true; kill = $false; text = "the LOCAL slot 1" }
}

# The world's display name (the bridge's state.world.name, or load_world's reply) against the test world's. An older
# build gives none: then the slot alone decides.
function Test-WorldName($Name, [string]$Expect) {
    if ($null -eq $Name) { return @{ ok = $true; kill = $false; text = "the state gives no world name (an older build): the slot decides" } }
    if ("$Name" -ceq $Expect) { return @{ ok = $true; kill = $false; text = "'$Name'" } }
    return @{ ok = $false; kill = $true; text = "the world is '$Name', not the test world '$Expect'" }
}

# The running world's character (the bridge's state.character) against the test character: LOCAL and named $Expect.
# A Steam character or another name is a wrong world (kill); one the bridge cannot tell (unknown, or an older build
# without the field) stops start without ending anything.
function Test-Character($Character, [string]$Expect) {
    $storage = if ($Character) { "$($Character.storage)" } else { "" }
    $shown = "character slot $($Character.slot)$(if ($Character.name) { " '$($Character.name)'" })"
    if ($storage -eq "steam") { return @{ ok = $false; kill = $true; text = "the running world uses a Steam character (Steam $shown)" } }
    if ($storage -ne "local") { return @{ ok = $false; kill = $false; text = "can't tell which character runs: go back to the main menu and use load_world" } }
    if ("$($Character.name)" -cne $Expect) { return @{ ok = $false; kill = $true; text = "the running world uses LOCAL $shown, not '$Expect'" } }
    return @{ ok = $true; kill = $false; text = "LOCAL $shown" }
}

# How -Character local starts the world: "bridge" (the bridge's load_world: no clicks, any screen) when hello lists
# load_world, else "clicks" (an older EMF build; the coordinates fit one 3840x2160 screen).
function Get-StartPath($Hello) {
    if ($Hello -and @($Hello.commands) -contains "load_world") { return "bridge" }
    return "clicks"
}

# A load_world reply that means this build cannot run load_world at all (its menu hooks are off: another game build),
# so nothing was started and the clicks may take over. A refusal, a timeout or no reply is not that.
function Test-LoadWorldMissing($Reply) {
    if (-not $Reply) { return $false }
    if ("$($Reply.code)" -eq "unknown_command") { return $true }
    return "$($Reply.code)" -eq "unavailable" -and "$($Reply.error)" -match '^load_world is (off|not available)'
}

# load_world's refusal in plain words: what happened (the game's own words) and what it means.
function Get-LoadWorldWhy($Reply) {
    $e = "$($Reply.error)".TrimEnd(".")
    switch ("$($Reply.code)") {
        "refused" { return "the game refused the start, nothing started: $e" }
        "timeout" { return "the world did not start in time: $e. The game may still start it within a minute (its guard against Steam saves stays on meanwhile)" }
        "busy" { return "another load_world runs: $e" }
        "unavailable" { return "load_world could not start the world: $e" }
    }
    return "$($Reply.code): $e"
}

# A game that did not close after the quit: kill (now), wait, or leave (running; stop says so). Only a game with no
# world (status none) is ended at once. A running or unknown world is ended only when it is the LOCAL slot 1 (or with
# -AllowKill): after a "world saved" line and 15 s more for it to exit by itself, or after 120 s without one.
function Get-StopPlan($WorldStatus, $Slot, $SavedAgoS, [double]$WaitedS, [bool]$AllowKill) {
    if ($WorldStatus -eq "none") { return "kill" }
    $mayKill = $AllowKill -or ($null -ne $Slot -and [int]$Slot -eq 1)
    if ($null -ne $SavedAgoS) {
        if ($SavedAgoS -lt 15) { return "wait" }
        return $(if ($mayKill) { "kill" } else { "leave" })
    }
    if ($WaitedS -lt 120) { return "wait" }
    return $(if ($mayKill) { "kill" } else { "leave" })
}

# The click coordinates are for one screen and window size (start_local_world.ps1). $null when they fit.
function Test-ClickGeometry($Screen, $Rect, $Expect) {
    if ([int]$Screen.width -ne $Expect.screen.width -or [int]$Screen.height -ne $Expect.screen.height) {
        return "the screen is $($Screen.width)x$($Screen.height); the click coordinates are for $($Expect.screen.width)x$($Expect.screen.height)"
    }
    if (-not $Rect) { return "the game window's size is unknown" }
    $w = $Rect.right - $Rect.left
    $h = $Rect.bottom - $Rect.top
    if ($w -ne $Expect.window.width -or $h -ne $Expect.window.height) {
        return "the game window is ${w}x${h}; the click coordinates are for its default size $($Expect.window.width)x$($Expect.window.height)"
    }
    return $null
}

# What of EMF is in the game folder: our loader, the core, [debug] bridge=1 in emf\emf_core.ini, game_session's marker.
function Get-EmfInstall([string]$GameDir) {
    $loader = Join-Path $GameDir "dinput8.dll"
    $ours = $false
    if (Test-Path -LiteralPath $loader) {
        $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($loader))
        $ours = $text.Contains("EMF-LOADER-7d3c9f") -or $text.Contains("emf_probe")  # src/loader/loader_id.h
    }
    $bridge = $false
    $ini = Join-Path $GameDir "emf\emf_core.ini"
    if (Test-Path -LiteralPath $ini) {
        $section = ""
        foreach ($line in Get-Content -LiteralPath $ini) {
            if ($line -match '^\s*\[(.+)\]\s*$') { $section = $Matches[1].Trim().ToLowerInvariant(); continue }
            if ($section -eq "debug" -and $line -match '^\s*bridge\s*=\s*(\S+)') { $bridge = $Matches[1] -eq "1" }
        }
    }
    return [ordered]@{ loader = $ours; core = (Test-Path -LiteralPath (Join-Path $GameDir "emf\emf_core.dll")); bridge = $bridge;
        marker = [bool](Read-InstallMarker $GameDir) }
}

function Read-SharedText([string]$Path) {
    try {
        $fs = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        try { return (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Close() }
    } catch { return "" }
}

# The game's own log (enshrouded.log, which the game starts afresh) says when the main menu is up: "[GameUi] Entered
# PreGame" comes once early and again after "[renderer] isReadyForRendering = true"; the second one counts. With
# $Since, a log last written before then is an older run's.
function Test-MenuShown([string]$LogPath, $Since = $null) {
    if ($Since -and (Test-Path -LiteralPath $LogPath) -and (Get-Item -LiteralPath $LogPath).LastWriteTime -lt $Since) { return $false }
    $text = Read-SharedText $LogPath
    $ready = $text.IndexOf("[renderer] isReadyForRendering = true")
    return $ready -ge 0 -and $text.IndexOf($MenuMarker, $ready) -ge 0
}

# A screenshot that is (nearly) black: the menu has not faded in yet. Samples a grid of pixels.
function Test-ShotBlank([string]$Path) {
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap $Path
    try {
        $sum = 0.0
        $n = 0
        for ($y = 0; $y -lt 24; $y++) {
            for ($x = 0; $x -lt 24; $x++) {
                $c = $bmp.GetPixel([int](($x + 0.5) * $bmp.Width / 24), [int](($y + 0.5) * $bmp.Height / 24))
                $sum += 0.2126 * $c.R + 0.7152 * $c.G + 0.0722 * $c.B
                $n++
            }
        }
        return ($sum / $n) -lt 10
    } finally {
        $bmp.Dispose()
    }
}

# The world-start clicks and what they are for (start_local_world.ps1, dot-sourced in here: its lists only).
function Get-ClickSetup {
    . (Join-Path $PSScriptRoot "start_local_world.ps1")
    return @{ steps = $steps; screen = $clickScreen; window = $clickWindow }
}

# ================================================================ the session (through $script:Env, so tests can fake it)
function Exit-With([int]$Code) { throw "EMF-GAME-SESSION-EXIT $Code" }
function Test-IsExit($err) { return "$($err.Exception.Message)" -match '^EMF-GAME-SESSION-EXIT \d+$' }
function Say([string]$text) { & $script:Env.Say ("[{0,5:N0} s] {1}" -f ((& $script:Env.Now) - $script:t0), $text) }

function Save-State { Write-SessionState $StateFile $script:state }
function Set-State([string]$Name, $Value) {
    if (-not $script:state) { $script:state = [ordered]@{} }
    if ($script:state -is [System.Management.Automation.PSCustomObject]) {
        $h = [ordered]@{}
        foreach ($p in $script:state.PSObject.Properties) { $h[$p.Name] = $p.Value }
        $script:state = $h
    }
    $script:state[$Name] = $Value
    Save-State
}
function Get-StateValue([string]$Name) {
    if (-not $script:state) { return $null }
    if ($script:state -is [System.Collections.IDictionary]) { return $script:state[$Name] }
    return $script:state.$Name
}

# The game is gone: the session file forgets it, and keeps only when this script last sent input (so the person check
# of the next start does not take this script's own clicks for a person's).
function Clear-SessionGame {
    $own = Get-StateValue "own_input_epoch"
    if ($null -eq $own) { $own = (Read-SessionState $StateFile).own_input_epoch }
    if ($null -ne $own) { $script:state = [ordered]@{ own_input_epoch = $own }; Save-State }
    elseif (Test-Path -LiteralPath $StateFile) { Remove-Item -LiteralPath $StateFile -Force }
}

function Get-BridgeWhy($r) {
    if ($r.reply -and $r.reply.error) { return "$($r.reply.code): $($r.reply.error)" }
    if ($r.message) { return $r.message }
    return "exit code $($r.code)"
}

function Get-PersonNow {
    Get-PersonVerdict (& $script:Env.LastInputEpoch) (& $script:Env.Now) (Get-StateValue "own_input_epoch") $IdleLimit ([bool]$IgnoreIdle) $script:t0
}
# Before a run of clicks: nobody may be at the PC. From then on, any input newer than this script's own is a person's.
function Assert-NobodyThere([string]$before) {
    $v = Get-PersonNow
    if ($v) { Say "refused before $before`: $v"; Exit-Session 3 }
    $script:inputSince = & $script:Env.Now
}
function Assert-NoForeignInput([string]$before) {
    $since = [Math]::Max([double]$script:inputSince, [double](Get-StateValue "own_input_epoch"))
    if (Test-ForeignInput (& $script:Env.LastInputEpoch) $since) {
        Say "stopped before $before`: someone used the mouse or keyboard meanwhile; not sending more input"
        Exit-Session 3
    }
}
function Invoke-Ui([string[]]$UiArgs, [string]$what) {
    Assert-NoForeignInput $what
    $r = & $script:Env.Ui $UiArgs
    Set-State "own_input_epoch" (& $script:Env.Now)
    if ($r.code -eq 3 -or "$($r.out)" -match "^refused") { Say "stopped at $what`: $($r.out)"; Exit-Session 1 }
    return $r
}

# Uninstalls (only game_session's install) and restores Fullscreen. Returns false when a part failed.
function Invoke-Cleanup {
    $ok = $true
    if ((Get-EmfInstall $Game).marker) {
        try { foreach ($l in @(& $script:Env.Uninstall)) { Say "$l" } }
        catch { Say "the uninstall failed: $($_.Exception.Message) (run game_session.ps1 stop again)"; $ok = $false }
    } elseif ((Get-EmfInstall $Game).core -or (Get-EmfInstall $Game).loader) {
        Say "EMF in $Game is not game_session's install (no emf\$MarkerName): left alone"
    }
    try { Say "window mode: Fullscreen (was $(Set-WindowMode $Settings 'Fullscreen'))" }
    catch { Say "could not set Fullscreen: $($_.Exception.Message)"; $ok = $false }
    return $ok
}

# Ends start. On a failure while the game is not running, what this run installed is removed again and the lock is
# released; while the game runs, game_session.ps1 stop does that.
function Exit-Session([int]$Code) {
    if ($Code -ne 0 -and $Code -ne 2 -and -not (& $script:Env.GetGame)) {
        $cleanOk = $true
        if ($script:installedHere) { $cleanOk = Invoke-Cleanup }
        if ($script:lockTaken) {
            if ($cleanOk) { Say (Exit-GameLock $LockFile $LockOwner) }
            else { Say "kept the game lock: the clean-up failed; run game_session.ps1 stop -LockOwner $LockOwner" }
        }
    }
    Exit-With $Code
}

# A world in the wrong slot: end the game at once (before it saves anything), wait for it, check the saves, remove
# EMF, release the lock, and say so loudly.
function Invoke-WrongWorld([string]$why) {
    $g = & $script:Env.GetGame
    if ($g) {
        & $script:Env.Kill $g.Id
        [void](& $script:Env.WaitExit $g.Id 30000)
        $deadline = (& $script:Env.Now) + 30  # an ended process stays listed while others (Steam) still hold it
        while ((& $script:Env.GameListed $g.Id) -and (& $script:Env.Now) -lt $deadline) { & $script:Env.Sleep 1000 }
    }
    $sg = & $script:Env.SaveGuard
    $cleanOk = Invoke-Cleanup
    if ($script:lockTaken) {
        if ($cleanOk) { Say (Exit-GameLock $LockFile $LockOwner) }
        else { Say "kept the game lock: the clean-up failed; run game_session.ps1 stop -LockOwner $LockOwner" }
    }
    Clear-SessionGame
    Say "=================================================================="
    Say "WRONG WORLD: $why."
    Say "The game was ended at once, without saving. save_guard -Check: $($sg.text)"
    Say "Check the main menu before the next start; report this to the user if a Cloud save shows CHANGED."
    Say "=================================================================="
    Exit-With 1
}

function Enter-GameFront {
    for ($try = 1; $try -le 3; $try++) {
        Assert-NoForeignInput "focusing the game"
        [void](& $script:Env.Ui @("focus"))
        Set-State "own_input_epoch" (& $script:Env.Now)  # focus taps Alt
        for ($i = 0; $i -lt 15; $i++) {
            if (& $script:Env.GameInFront) { & $script:Env.Sleep 1500; return }
            & $script:Env.Sleep 300
        }
    }
    Say "stopped: the game window did not come to the front (Windows refused the focus)"
    Exit-Session 1
}

function Get-WindowRect {
    $r = & $script:Env.Ui @("rect")
    if ("$($r.out)" -match '^rect (-?\d+) (-?\d+) (-?\d+) (-?\d+)') {
        return @{ left = [int]$Matches[1]; top = [int]$Matches[2]; right = [int]$Matches[3]; bottom = [int]$Matches[4] }
    }
    return $null
}

# The window where the coordinates expect it (it can drift), after checking the screen and its size.
function Assert-ClickGeometry($setup) {
    $why = Test-ClickGeometry (& $script:Env.Screen) (Get-WindowRect) $setup
    if ($why) { Say "refused before the clicks: $why (start_local_world.ps1's coordinates do not fit this PC)"; Exit-Session 3 }
    $r = Get-WindowRect
    if ($r.left -ne $setup.window.left -or $r.top -ne $setup.window.top) {
        [void](& $script:Env.Ui @("place", "$($setup.window.left)", "$($setup.window.top)"))
        Say "moved the game window to $($setup.window.left),$($setup.window.top) (it was at $($r.left),$($r.top))"
    }
}

# The main menu: the game's own marker, then a frame that is not black. A menu that does not show stops start.
function Wait-Menu($g) {
    $deadline = (& $script:Env.Now) + 120
    while (-not (& $script:Env.MenuShown $g.StartEpoch)) {
        if ((& $script:Env.Now) -ge $deadline) { Say "the main menu did not come up within 120 s (no '$MenuMarker' after the renderer was ready in the game's log)"; Exit-Session 1 }
        & $script:Env.Sleep 2000
    }
    $deadline = (& $script:Env.Now) + 45
    while ($true) {
        & $script:Env.Sleep 3000
        $probe = & $script:Env.Shot "probe"
        if ($probe) {
            $blank = $true
            try { $blank = & $script:Env.ShotBlank $probe } catch { $blank = $true }
            & $script:Env.Forget $probe
            if (-not $blank) { break }
        }
        if ((& $script:Env.Now) -ge $deadline) { Say "stopped: the screen stayed black (or gave no screenshot) for 45 s after the menu marker; no clicks on a screen nobody can see"; Exit-Session 1 }
    }
    Say "the main menu is up"
}

function Show-MenuShot($g, [string]$why, [bool]$HasLoadWorld) {
    $shot = & $script:Env.Shot "menu"
    if (-not $shot) { Say "stopped: no menu screenshot could be taken"; Exit-Session 1 }
    Set-State "menu_shown_pid" $g.Id
    Say "main menu screenshot: $shot"
    if ($why) { Say $why }
    if ($HasLoadWorld) {
        Say "look at it: the label on the right should read '[LOCAL] $CharacterName'. Then: game_session.ps1 start -Character local -LockOwner $LockOwner. It starts the world through the bridge's load_world, which selects LOCAL character slot 1 itself (so a [STEAM] <name> label is switched too) while the game refuses any Steam save."
    } else {
        Say "look at it: the label on the right must read '[LOCAL] $CharacterName'. Then: game_session.ps1 start -Character local -LockOwner $LockOwner. If it shows [STEAM] <name>: game_session.ps1 start -Character switch -LockOwner $LockOwner."
    }
    Exit-With 2
}

# Waits up to 10 s until the load-event reader listens (else says so: the state's slot decides later).
function Wait-EventsReady($events) {
    $deadline = (& $script:Env.Now) + 10
    while (-not (& $script:Env.EventsReady $events)) {
        if ((& $script:Env.Now) -ge $deadline) {
            Say "the load-event reader did not start listening within 10 s: the world's slot is checked from the state once it runs"
            return
        }
        & $script:Env.Sleep 250
    }
}

# The slot of a load event the reader got so far, or $null.
function Read-LoadSlot($events) {
    $slot = $null
    foreach ($e in @(& $script:Env.EventsRead $events)) {
        if ($e.name -eq "load" -and $null -ne $e.fields.slot) { $slot = [int]$e.fields.slot }
    }
    return $slot
}

# The world through the bridge's load_world: no clicks and no input, on any screen. It starts LOCAL world slot 1 with
# LOCAL character slot 1, which must be named $CharacterName; while it runs the game refuses any Steam Cloud character
# or world. The load event is watched meanwhile, as for the clicks: a world in another slot is ended at once. Returns
# @{ fallback } when this build's load_world is off (nothing was started: the clicks may take over), else
# @{ loadSlot } (the load event's slot, $null when none came). A refusal, a timeout or no answer stops start (exit 1).
function Start-WorldByBridge {
    Say "starting the world through the bridge's load_world (no clicks): LOCAL world slot 1 with LOCAL character slot 1 '$CharacterName'; the game refuses any Steam Cloud save meanwhile"
    $events = & $script:Env.EventsStart
    $lw = $null
    $loadSlot = $null
    $r = $null
    try {
        Wait-EventsReady $events
        $lw = & $script:Env.BridgeStart @("load_world", "1", "1", $CharacterName, "-TimeoutMs", "$($WorldTimeoutS * 1000)")
        $script:openedWorld = $true
        # the game's own time for the start, its 10 s backstop and the client's 15 s margin
        $deadline = (& $script:Env.Now) + $WorldTimeoutS + 40
        $done = $false
        while ($true) {
            if ($null -eq $loadSlot) { $loadSlot = Read-LoadSlot $events }
            if ($null -ne $loadSlot) {
                $v = Test-WorldSlot $loadSlot
                if ($v.kill) { Invoke-WrongWorld $v.text }
            }
            if ($done) { break }  # (the events were read once more after the reply: the load event comes before it)
            $done = [bool](& $script:Env.BridgeDone $lw)
            if (-not $done) {
                if ((& $script:Env.Now) -ge $deadline) { break }
                & $script:Env.Sleep 500
            }
        }
        $r = & $script:Env.BridgeResult $lw
    } finally {
        if ($lw) { & $script:Env.BridgeStop $lw }
        & $script:Env.EventsStop $events
    }
    if ($r.code -eq 0 -and $r.reply -and $r.reply.ok) {
        $reply = $r.reply
        Say "load_world: '$($reply.world)' runs with '$($reply.character_name)' (LOCAL world slot $($reply.slot), character $($reply.character), $($reply.ms) ms in the game)"
        if ([int]$reply.slot -ne 1 -or [int]$reply.character -ne 1) { Invoke-WrongWorld "load_world started LOCAL world slot $($reply.slot) with character $($reply.character), not slot 1 with character 1" }
        if ("$($reply.character_name)" -cne $CharacterName) { Invoke-WrongWorld "load_world started the character '$($reply.character_name)', not '$CharacterName'" }
        $n = Test-WorldName $reply.world $WorldName
        if ($n.kill) { Invoke-WrongWorld "load_world started another world: $($n.text)" }
        return @{ fallback = $false; loadSlot = $loadSlot }
    }
    if (Test-LoadWorldMissing $r.reply) {
        Say "load_world cannot run on this build ($($r.reply.error)): nothing was started; starting the world with the click fallback (made for a 3840x2160 screen)"
        return @{ fallback = $true; loadSlot = $null }
    }
    if ($r.reply -and $r.reply.code) {
        Say "load_world did not start the world: $(Get-LoadWorldWhy $r.reply)."
    } else {
        Say "no answer from load_world within $($WorldTimeoutS + 40) s ($(Get-BridgeWhy $r)): the game may still be loading"
    }
    Say "no clicks follow. The game runs on: check it with game_session.ps1 status, and end the session with game_session.ps1 stop -LockOwner $LockOwner."
    Exit-Session 1
}

function Start-GameSession {
    $g = & $script:Env.GetGame
    $script:state = Read-SessionState $StateFile
    if ($g) {
        $ours = Test-OurGame $script:state $g
        if (-not $ours -and -not $AdoptGame) {
            Say "a game runs (pid $($g.Id)) that this script did not start (the session file records $(if ($script:state) { "pid $($script:state.game_pid)" } else { 'no game' })): someone else's game, not touching it (-AdoptGame takes it over)"
            Exit-With 3
        }
        $hello = & $script:Env.Bridge @("hello") 60
        if ($hello.code -ne 0) {
            Say "the game (pid $($g.Id)) runs but its debug bridge does not answer ($(Get-BridgeWhy $hello)): not touching it"
            Exit-With $(if ($ours) { 1 } else { 3 })
        }
    }
    $lock = Enter-GameLock $LockFile $LockOwner $(if ($g) { $g.Id } else { $PID }) $script:Env.PidAlive
    if (-not $lock.ok) {
        if (-not $IgnoreLock) { Say $lock.text; Exit-With 3 }
        Say "$($lock.text) -IgnoreLock: going on anyway"
    } else {
        $script:lockTaken = $true
        Say $lock.text
    }
    $launched = $false
    if ($g) {
        if (-not (Test-OurGame $script:state $g)) {  # -AdoptGame
            $script:state = [ordered]@{ owner = $LockOwner; game = $Game; game_pid = $g.Id; game_start_epoch = $g.StartEpoch; adopted = $true }
            Save-State
            Say "took over the running game (pid $($g.Id)) as this session's"
        } else {
            Say "the game runs already (pid $($g.Id)): going on from where it is"
        }
        foreach ($spec in $Mods) {
            try { $id = (Resolve-ModSpec $spec $sessionRepo).Id } catch { Say "-Mods $spec`: $($_.Exception.Message)"; continue }
            if (-not (Test-Path -LiteralPath (Join-Path $Game "emf\mods\$id"))) { Say "-Mods $spec is not installed and the game runs: stop first to install it" }
        }
    } else {
        Assert-NobodyThere "starting the game"
        $inst = Get-EmfInstall $Game
        if (($inst.core -or $inst.loader) -and -not $inst.marker) {
            Say "EMF is installed in $Game and not by game_session (no emf\$MarkerName): a modder's or the user's own install, not replacing it"
            Exit-Session 3
        }
        $sg = & $script:Env.SaveGuard
        if ($sg.code -ne 0) {
            Say "stopped: save_guard -Check says the Steam Cloud saves changed or cannot be checked ($($sg.text)). Stop here and tell the user: recording them again (save_guard.ps1 -Record) is the user's step, after their own play."
            Exit-Session 1
        }
        Say "saves unchanged ($($sg.text))"
        try {
            $script:installedHere = $true
            $out = @(& $script:Env.Install)
            Say "$($out[0])"
            $extra = Install-ExtraMods $Mods $Game $sessionRepo
            Write-InstallMarker $Game $LockOwner $extra.made
            foreach ($line in $extra.lines) { Say $line }
            Say "window mode: Windowed (was $(Set-WindowMode $Settings 'Windowed'))"
        } catch {
            if (Test-IsExit $_) { throw }
            Say "the install failed: $($_.Exception.Message)"
            Exit-Session 1
        }
        & $script:Env.Launch
        Say "asked Steam to start the game"
        $deadline = (& $script:Env.Now) + 180
        $hello = $null
        while ((& $script:Env.Now) -lt $deadline) {
            & $script:Env.Sleep 2000
            $g = & $script:Env.GetGame
            if (-not $g) { continue }
            $hello = & $script:Env.Bridge @("hello") 30
            if ($hello.code -eq 0) { break }
        }
        if (-not $g -or -not $hello -or $hello.code -ne 0) {
            Say "the game's bridge did not answer within 180 s ($(if ($hello) { Get-BridgeWhy $hello } else { 'no game process' }))"
            Exit-Session 1
        }
        $script:state = [ordered]@{ owner = $LockOwner; game = $Game; game_pid = $g.Id; game_start_epoch = $g.StartEpoch;
            launched_epoch = $script:t0; own_input_epoch = (Get-StateValue "own_input_epoch") }
        Save-State
        if ($script:lockTaken) { Write-GameLock $LockFile $LockOwner $g.Id }  # the lock names the game from now on
        $launched = $true
        Say "bridge up: EMF $($hello.reply.emf) (API $($hello.reply.api)), game build $($hello.reply.game_build), pid $($g.Id)"
    }

    $st = (& $script:Env.Bridge @("state") 60).reply
    if (-not $st) { Say "the bridge gave no state"; Exit-Session 1 }
    $setup = Get-ClickSetup
    $loadSlot = $null
    if ($st.world.status -eq "loading") {
        Say "a world is loading already: no clicks, waiting for it"
    } elseif ($st.world.status -ne "running") {
        Wait-Menu $g
        $path = Get-StartPath $hello.reply
        $decision = Get-MenuDecision $Character $launched $script:state $g.Id
        if ($decision -eq "show") {
            $why = if ($Character -eq "local") {
                if ($launched) { "this run started the game, and the game shows the character played last (it may be the user's [STEAM] one): look before the world loads" }
                else { "-Character local needs a menu screenshot shown for this game first (pid $($g.Id)): here it is" }
            } else { $null }
            Show-MenuShot $g $why ($path -eq "bridge")
        }
        if ($decision -eq "switch") {
            Assert-NobodyThere "the clicks"
            Assert-ClickGeometry $setup
            Enter-GameFront
            foreach ($c in $SwitchClicks) {
                [void](Invoke-Ui @("click", "$($c.x)", "$($c.y)") "clicking $($c.name)")
                Say "clicked $($c.name)"
                & $script:Env.Sleep 2500
            }
            [void](Invoke-Ui @("key", "Esc") "pressing Esc")
            & $script:Env.Sleep 1500
            [void](Invoke-Ui @("key", "Esc") "pressing Esc")
            & $script:Env.Sleep 2500
            Say "switched to the LOCAL tab, slot 1, and pressed Esc twice"
            Show-MenuShot $g $null ($path -eq "bridge")
        }
        # local: through the bridge's load_world when this build has it; the clicks only as the fallback
        if ($path -eq "bridge") {
            Assert-NobodyThere "starting the world"
            $started = Start-WorldByBridge
            if ($started.fallback) { $path = "clicks" } else { $loadSlot = $started.loadSlot }
        } else {
            Say "this game's bridge has no load_world (an older EMF build): starting the world with the click fallback (made for a 3840x2160 screen)"
        }
        if ($path -eq "clicks") {
            Assert-NobodyThere "the clicks"
            Assert-ClickGeometry $setup
            Enter-GameFront
            # the world clicks, with the load event watched (it comes before the world's first step)
            $events = & $script:Env.EventsStart
            try {
                Wait-EventsReady $events
                foreach ($c in @($setup.steps)) {
                    [void](Invoke-Ui @("click", "$($c.x)", "$($c.y)") "clicking $($c.name)")
                    $script:openedWorld = $true
                    Say "clicked $($c.name)"
                    & $script:Env.Sleep 2500
                }
                $deadline = (& $script:Env.Now) + $WorldTimeoutS
                while ($null -eq $loadSlot -and (& $script:Env.Now) -lt $deadline) {
                    $loadSlot = Read-LoadSlot $events
                    if ($null -ne $loadSlot) { break }
                    $now = (& $script:Env.Bridge @("state") 30).reply
                    if ($now -and $now.world.status -eq "running") {
                        # the load event comes before the world runs: read once more for one that reached the file meanwhile
                        $loadSlot = Read-LoadSlot $events
                        break  # still none seen: the state's slot decides below
                    }
                    & $script:Env.Sleep 500
                }
            } finally {
                & $script:Env.EventsStop $events
            }
        }
        if ($null -ne $loadSlot) {
            $v = Test-WorldSlot $loadSlot
            if ($v.kill) { Invoke-WrongWorld $v.text }
            Say "the world's load event: $($v.text)"
        }
    }
    $w = & $script:Env.Bridge @("wait", "world", "$WorldTimeoutS") ($WorldTimeoutS + 30)
    if ($w.code -ne 0) {
        $shot = & $script:Env.Shot "no_world"
        Say "no world within $WorldTimeoutS s ($(Get-BridgeWhy $w)); screenshot: $(if ($shot) { $shot } else { '(none)' })"
        Exit-Session 1
    }
    # the slot comes with the world's load; right after "running" it can still be missing for a moment
    $deadline = (& $script:Env.Now) + 20
    do {
        $st = (& $script:Env.Bridge @("state") 30).reply
        if ($st -and $null -ne $st.world.slot) { break }
        & $script:Env.Sleep 1000
    } while ((& $script:Env.Now) -lt $deadline)
    $slot = Test-WorldSlot $st.world.slot
    if ($slot.kill -and $script:openedWorld) { Invoke-WrongWorld $slot.text }  # this run opened it
    if ($slot.kill) {
        Say "the running world is not the test world: $($slot.text). This run did not open it, so it leaves it alone."
        Exit-With 3
    }
    if (-not $slot.ok) { Say "stopped: $($slot.text)"; Exit-Session 1 }
    $name = Test-WorldName $st.world.name $WorldName
    if ($name.kill -and $script:openedWorld) { Invoke-WrongWorld $name.text }
    if ($name.kill) {
        Say "the running world is not the test world: $($name.text). This run did not open it, so it leaves it alone."
        Exit-With 3
    }
    $ch = Test-Character $st.character $CharacterName
    if ($ch.kill -and $script:openedWorld) { Invoke-WrongWorld $ch.text }
    if ($ch.kill) {
        Say "the running world is not for tests: $($ch.text). This run did not open it, so it leaves it alone."
        Exit-With 3
    }
    if (-not $ch.ok) { Say "stopped: $($ch.text). Nothing was ended; game_session.ps1 stop -LockOwner $LockOwner ends the session."; Exit-Session 1 }
    Say ("world running: $($slot.text)" + $(if ($null -ne $st.world.name) { ", $($name.text)" } else { " ($($name.text))" }) + ", with $($ch.text)")
    $pl = & $script:Env.Bridge @("wait", "player", "120") 150
    if ($pl.code -ne 0) { Say "no local player within 120 s ($(Get-BridgeWhy $pl))"; Exit-Session 1 }
    $deadline = (& $script:Env.Now) + 20
    do {
        $st = (& $script:Env.Bridge @("state") 30).reply
        if ($st -and $st.player -and $st.player.position) { break }
        & $script:Env.Sleep 1000
    } while ((& $script:Env.Now) -lt $deadline)
    $pos = $st.player.position
    Say ("ready: player {0} at ({1:F1}, {2:F1}, {3:F1}), world kind {4}, after {5:N0} s" -f $st.player.id, $pos.x, $pos.y, $pos.z, $st.world.kind, ((& $script:Env.Now) - $script:t0))
    Exit-With 0
}

function Stop-GameSession {
    $code = 0
    $lockState = Get-LockState $LockFile $LockOwner $script:Env.PidAlive
    if ($lockState.state -eq "held" -and -not $IgnoreLock) {
        Say "the game lock is held by someone else ('$($lockState.text)'): their session, not stopping it (-IgnoreLock does)"
        Exit-With 3
    }
    $script:state = Read-SessionState $StateFile
    $g = & $script:Env.GetGame
    if ($g) {
        if (-not (Test-OurGame $script:state $g) -and -not $AdoptGame) {
            Say "the running game (pid $($g.Id)) is not the one this script started: someone else's game, not closing it (-AdoptGame does)"
            Exit-With 3
        }
        $hello = & $script:Env.Bridge @("hello") 60
        if ($hello.code -ne 0 -and -not $AllowUnansweredBridge) {
            Say "the game (pid $($g.Id)) runs but its debug bridge does not answer: not closing it (-AllowUnansweredBridge does)"
            Exit-With 3
        }
        $status = $null
        $slot = $null
        if ($hello.code -eq 0) {
            $st = (& $script:Env.Bridge @("state") 60).reply
            if ($st) { $status = $st.world.status; $slot = $st.world.slot }
        }
        $asked = & $script:Env.Now
        $logStart = (& $script:Env.LogText).Length
        $quit = if ($hello.code -eq 0) { & $script:Env.Bridge @("quit") 60 } else { $null }
        if ($quit -and $quit.code -eq 0) { Say "asked the game to quit through the bridge (it saves first)" }
        else {
            & $script:Env.CloseWindow $g.WindowHandle
            Say "asked the game's window to close (it saves first)$(if ($quit) { " (the bridge's quit failed: $(Get-BridgeWhy $quit))" })"
        }
        $exited = & $script:Env.WaitExit $g.Id 60000
        if (-not $exited) {
            Say "the game still runs after 60 s (world: $(if ($status) { $status } else { 'unknown' })$(if ($null -ne $slot) { ', slot ' + $slot }))"
            $savedAt = $null
            while ($true) {
                if (-not $savedAt) {
                    $text = & $script:Env.LogText
                    if ($text.Length -gt $logStart -and $text.Substring($logStart).Contains("world saved")) { $savedAt = & $script:Env.Now; Say "the game saved the world" }
                }
                $now = & $script:Env.Now
                $plan = Get-StopPlan $status $slot $(if ($savedAt) { $now - $savedAt } else { $null }) ($now - $asked - 60) ([bool]$AllowKill)
                if ($plan -eq "wait") {
                    if (& $script:Env.WaitExit $g.Id 2000) { $exited = $true; break }
                    continue
                }
                if ($plan -eq "kill") {
                    & $script:Env.Kill $g.Id
                    Say "ended the game process"
                    $exited = $true
                    break
                }
                Say "the game did not close and is LEFT RUNNING: a world that is not the LOCAL slot 1 (or whose slot is unknown) is never ended by force (-AllowKill does). Close it by hand, then run stop again."
                Say "kept the game lock and EMF (the game still runs)"
                Exit-With 1
            }
        }
        Say ("the game ended ({0:N0} s after asking)" -f ((& $script:Env.Now) - $asked))
        $deadline = (& $script:Env.Now) + 30  # an ended process stays listed while others (Steam) still hold it
        while ((& $script:Env.GameListed $g.Id) -and (& $script:Env.Now) -lt $deadline) { & $script:Env.Sleep 1000 }
    } else {
        Say "the game is not running"
    }
    $cleanOk = $true
    if ($KeepInstalled) {
        try { Say "window mode: Fullscreen (was $(Set-WindowMode $Settings 'Fullscreen'))" } catch { Say "could not set Fullscreen: $($_.Exception.Message)"; $cleanOk = $false }
    } else {
        $cleanOk = Invoke-Cleanup
    }
    if (-not $cleanOk) { $code = 1 }
    $sg = & $script:Env.SaveGuard
    Say "save_guard -Check: $($sg.text)"
    if ($sg.code -ne 0) { $code = 1; Say "the Steam Cloud saves changed: stop and tell the user (recording them again is the user's step)" }
    Clear-SessionGame
    if ($cleanOk) { Say (Exit-GameLock $LockFile $LockOwner) }
    else { Say "kept the game lock: the clean-up failed; run game_session.ps1 stop again" }
    Exit-With $code
}

function Show-GameStatus {
    $g = & $script:Env.GetGame
    $sess = Read-SessionState $StateFile
    $s = [ordered]@{ game = $null; ours = $false; bridge = $null; world = $null; character = $null; player = $null; install = (Get-EmfInstall $Game);
        window_mode = (Get-WindowMode $Settings); lock = (Get-LockState $LockFile $LockOwner $script:Env.PidAlive); session = $sess; person = $null }
    if ($g) {
        $s.game = [ordered]@{ pid = $g.Id; start_epoch = $g.StartEpoch }
        $s.ours = Test-OurGame $sess $g
        $h = & $script:Env.Bridge @("hello") 30
        if ($h.code -eq 0) {
            $s.bridge = [ordered]@{ emf = $h.reply.emf; api = $h.reply.api; game_build = $h.reply.game_build; load_world = ((Get-StartPath $h.reply) -eq "bridge") }
            $st = (& $script:Env.Bridge @("state") 30).reply
            if ($st) {
                $s.world = [ordered]@{ status = $st.world.status; kind = $st.world.kind; slot = $st.world.slot }
                if ($st.character) { $s.character = [ordered]@{ storage = $st.character.storage; slot = $st.character.slot; name = $st.character.name } }
                if ($st.player) { $s.player = [ordered]@{ id = $st.player.id; position = $st.player.position } }
            }
        }
    }
    $s.person = [ordered]@{ idle_s = [math]::Round((& $script:Env.Now) - (& $script:Env.LastInputEpoch)); verdict = (Get-PersonVerdict (& $script:Env.LastInputEpoch) (& $script:Env.Now) $(if ($sess) { $sess.own_input_epoch }) $IdleLimit $false (& $script:Env.Now)) }
    if ($Json) { & $script:Env.Say ($s | ConvertTo-Json -Depth 6); Exit-With 0 }
    & $script:Env.Say ("game:        " + $(if ($s.game) { "running (pid $($s.game.pid)$(if ($s.ours) { ', this session''s' } else { ', NOT this session''s' }))" } else { "not running" }))
    & $script:Env.Say ("bridge:      " + $(if ($s.bridge) { "up (EMF $($s.bridge.emf), API $($s.bridge.api), game build $($s.bridge.game_build), $(if ($s.bridge.load_world) { 'load_world' } else { 'no load_world: the world starts by the click fallback' }))" } elseif ($g) { "not answering" } else { "-" }))
    & $script:Env.Say ("world:       " + $(if ($s.world) { "$($s.world.status)$(if ($s.world.kind) { ", $($s.world.kind)" })$(if ($null -ne $s.world.slot) { ", slot $($s.world.slot)" })" } else { "-" }))
    & $script:Env.Say ("character:   " + $(if ($s.character) { "$($s.character.storage)$(if ($null -ne $s.character.slot) { ", slot $($s.character.slot)" })$(if ($s.character.name) { ", '$($s.character.name)'" })" } else { "-" }))
    & $script:Env.Say ("player:      " + $(if ($s.player) { "{0} at ({1:F1}, {2:F1}, {3:F1})" -f $s.player.id, $s.player.position.x, $s.player.position.y, $s.player.position.z } else { "-" }))
    & $script:Env.Say ("EMF:         " + $(if ($s.install.loader -or $s.install.core) { "installed (loader $($s.install.loader), core $($s.install.core), bridge $($s.install.bridge), game_session's: $($s.install.marker))" } else { "not installed" }))
    & $script:Env.Say "window mode: $($s.window_mode)"
    & $script:Env.Say ("lock:        $($s.lock.state)$(if ($s.lock.text) { " ('$($s.lock.text)')" }) ($LockFile)")
    & $script:Env.Say ("person:      idle $($s.person.idle_s) s" + $(if ($s.person.verdict) { " -> start would refuse" } else { "" }))
    Exit-With 0
}

# Runs one command; returns its exit code. A session that ends early (Ctrl+C, an unexpected error) says what to run,
# and removes what it installed when the game does not run.
function Invoke-GameSession([string]$Cmd) {
    $script:t0 = & $script:Env.Now
    $script:state = $null
    $script:installedHere = $false
    $script:lockTaken = $false
    $script:openedWorld = $false  # this run started a world (clicks or load_world): a wrong one is ended at once
    $script:inputSince = $script:t0
    if ($Cmd -notin @("start", "stop", "status")) {
        & $script:Env.Say "usage: game_session.ps1 start | stop | status (see the top of this file)"
        return 4
    }
    if ($Cmd -ne "status" -and -not $LockOwner) {
        & $script:Env.Say "say who you are with -LockOwner (one word, the same for start and stop, e.g. -LockOwner tester1; or EMF_GAME_LOCK_OWNER)"
        return 4
    }
    if (-not $script:Game) {
        try { $script:Game = Resolve-GameDir "" } catch { & $script:Env.Say $_.Exception.Message; return 4 }
    }
    $finished = $false
    $code = 1
    try {
        switch ($Cmd) {
            "start" { Start-GameSession }
            "stop" { Stop-GameSession }
            "status" { Show-GameStatus }
        }
    } catch {
        if (Test-IsExit $_) {
            $code = [int]("$($_.Exception.Message)" -replace '^EMF-GAME-SESSION-EXIT ', '')
            $finished = $true
        } else {
            Say "internal error: $($_.Exception.Message)"
        }
    } finally {
        if (-not $finished -and $Cmd -ne "status") {
            Say "the session ended early (interrupted, or the error above). The game may still run: run game_session.ps1 stop -LockOwner $LockOwner (the session file is $StateFile)."
            if ($Cmd -eq "start" -and -not (& $script:Env.GetGame)) {
                if ($script:installedHere) { [void](Invoke-Cleanup) }
                if ($script:lockTaken) { Say (Exit-GameLock $LockFile $LockOwner) }
            }
        }
    }
    return $code
}

# ================================================================ the real PC
# A bridge client (emf_bridge.ps1 <args>) in a process of its own: @{proc, dir, out, err}. Its output files are in a
# folder of its own, which is also its %TEMP%: Windows PowerShell compiles a script's Add-Type code there, and a child
# ended part-way leaves those files. Remove-BridgeChildDir removes the folder once the child has ended.
function Start-BridgeChild([string[]]$BridgeArgs) {
    $dir = Join-Path ([IO.Path]::GetTempPath()) ("emf_gs_child_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
    New-Item -ItemType Directory -Force $dir | Out-Null
    $out = Join-Path $dir "out.txt"
    $err = Join-Path $dir "err.txt"
    $argv = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $script:bridgePs) + $BridgeArgs
    $quoted = ($argv | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -join " "
    $oldTmp = $env:TMP
    $oldTemp = $env:TEMP
    try {
        $env:TMP = $dir  # (the child takes this process's environment as it is now)
        $env:TEMP = $dir
        $p = Start-Process -FilePath "powershell" -ArgumentList $quoted -RedirectStandardOutput $out -RedirectStandardError $err -WindowStyle Hidden -PassThru
    } catch {
        Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
        throw
    } finally {
        $env:TMP = $oldTmp
        $env:TEMP = $oldTemp
    }
    return @{ proc = $p; dir = $dir; out = $out; err = $err }
}

# The reply of a bridge client child that has ended (it ran with -Json): @{code, reply, message}.
function Read-BridgeChildReply($c) {
    $reply = $null
    $other = @()
    foreach ($l in @(Get-Content -LiteralPath $c.out) + @(Get-Content -LiteralPath $c.err)) {
        if ("$l".TrimStart().StartsWith("{") -and -not $reply) { try { $reply = "$l" | ConvertFrom-Json } catch { $other += "$l" } }
        elseif ("$l".Trim()) { $other += "$l" }
    }
    return @{ code = $c.proc.ExitCode; reply = $reply; message = (($other -join " ") -replace '\s+', ' ').Trim() }
}

function Remove-BridgeChildDir([string]$Dir) {
    for ($i = 0; $i -lt 20 -and (Test-Path -LiteralPath $Dir); $i++) {  # (an ended child lets go of its files a moment later)
        Remove-Item -LiteralPath $Dir -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $Dir) { Start-Sleep -Milliseconds 100 }
    }
}

function New-RealEnv {
    if (-not ("EmfSession.Native" -as [type])) {
        Add-Type -Namespace EmfSession -Name Native -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
[DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO info);
[DllImport("kernel32.dll")] public static extern uint GetTickCount();
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
// the last input's wall-clock time, in ms since 1970 (UTC)
public static double LastInputEpochMs() {
    LASTINPUTINFO i = new LASTINPUTINFO(); i.cbSize = 8;
    if (!GetLastInputInfo(ref i)) return 0;
    return DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - (double)unchecked(GetTickCount() - i.dwTime);
}
public static uint ForegroundPid() { uint pid; GetWindowThreadProcessId(GetForegroundWindow(), out pid); return pid; }
'@
    }
    [void][EmfSession.Native]::SetProcessDPIAware()
    # (plain script blocks, no closures: they run in this script's scope and see its variables and functions)
    $e = @{}
    $e.Now = { [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() / 1000.0 }
    $e.Sleep = { param($ms) Start-Sleep -Milliseconds $ms }
    $e.Say = { param($text) Write-Host $text }
    $e.GetGame = {
        $p = Get-Process enshrouded -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $p) { return $null }
        return @{ Id = $p.Id; StartEpoch = ([DateTimeOffset]$p.StartTime).ToUnixTimeMilliseconds() / 1000.0; WindowHandle = $p.MainWindowHandle }
    }
    $e.PidAlive = { param($id) [bool](Get-Process -Id $id -ErrorAction SilentlyContinue) }
    $e.GameListed = { param($id) [bool](Get-Process -Id $id -ErrorAction SilentlyContinue) }
    $e.WaitExit = {
        param($id, $ms)
        $p = Get-Process -Id $id -ErrorAction SilentlyContinue
        if (-not $p) { return $true }
        try { return $p.WaitForExit($ms) } finally { $p.Dispose() }
    }
    $e.Kill = { param($id) Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
    $e.CloseWindow = { param($handle) [void][EmfSession.Native]::PostMessage($handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
    $e.LastInputEpoch = { [EmfSession.Native]::LastInputEpochMs() / 1000.0 }
    $e.GameInFront = {
        $p = Get-Process enshrouded -ErrorAction SilentlyContinue | Select-Object -First 1
        return [bool]($p -and [EmfSession.Native]::ForegroundPid() -eq [uint32]$p.Id)
    }
    $e.Screen = { @{ width = [EmfSession.Native]::GetSystemMetrics(0); height = [EmfSession.Native]::GetSystemMetrics(1) } }
    # One bridge command through emf_bridge.ps1 in its own process, ended after $timeoutS: @{code, reply, message}.
    $e.Bridge = {
        param([string[]]$BridgeArgs, [int]$timeoutS)
        $c = Start-BridgeChild ($BridgeArgs + @("-Game", $Game, "-Json"))
        try {
            $p = $c.proc
            $null = $p.Handle  # (Windows PowerShell 5.1: without the handle opened now, ExitCode stays empty)
            if (-not $p.WaitForExit([Math]::Max(1, $timeoutS) * 1000)) {
                Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
                [void]$p.WaitForExit(5000)  # until it has let go of its files, so they can be removed
                return @{ code = 2; reply = $null; message = "the bridge client did not finish within $timeoutS s" }
            }
            $p.WaitForExit()
            return Read-BridgeChildReply $c
        } finally {
            Remove-BridgeChildDir $c.dir
        }
    }
    # A bridge command that runs while start watches the load events (load_world): started, polled, read, stopped.
    $e.BridgeStart = {
        param([string[]]$BridgeArgs)
        $c = Start-BridgeChild ($BridgeArgs + @("-Game", $Game, "-Json"))
        $null = $c.proc.Handle  # (Windows PowerShell 5.1: without the handle opened now, ExitCode stays empty)
        return $c
    }
    $e.BridgeDone = { param($h) return $h.proc.HasExited }
    $e.BridgeResult = {
        param($h)
        if (-not $h.proc.HasExited) { return @{ code = 2; reply = $null; message = "the bridge client did not finish in time" } }
        $h.proc.WaitForExit()
        return Read-BridgeChildReply $h
    }
    $e.BridgeStop = {
        param($h)
        if (-not $h.proc.HasExited) {
            Stop-Process -Id $h.proc.Id -Force -ErrorAction SilentlyContinue
            [void]$h.proc.WaitForExit(5000)
        }
        Remove-BridgeChildDir $h.dir
    }
    # The load events, from an `emf_bridge.ps1 events load` that runs meanwhile and writes them to a file.
    $e.EventsStart = { Start-BridgeChild @("events", "load", "-Game", $Game) }
    $e.EventsReady = { param($h) (Read-SharedText $h.out).Contains("pushing:") }  # the client's line once subscribed
    $e.EventsRead = {
        param($h)
        $events = @()
        foreach ($l in (Read-SharedText $h.out) -split "`n") {
            if ($l.TrimStart().StartsWith("{")) { try { $events += ($l | ConvertFrom-Json) } catch { } }
        }
        return $events
    }
    $e.EventsStop = {
        param($h)
        if ($h.proc -and -not $h.proc.HasExited) {
            Stop-Process -Id $h.proc.Id -Force -ErrorAction SilentlyContinue
            [void]$h.proc.WaitForExit(5000)
        }
        Remove-BridgeChildDir $h.dir
    }
    $e.Ui = {
        param([string[]]$UiArgs)
        $global:LASTEXITCODE = 0
        $o = "$(& $script:ui @UiArgs)"
        return @{ code = $LASTEXITCODE; out = $o }
    }
    $e.Install = { & (Join-Path $PSScriptRoot "install_emf.ps1") -Game $Game -Bridge }
    $e.Uninstall = { & (Join-Path $PSScriptRoot "uninstall_probe.ps1") -Game $Game }
    $e.SaveGuard = {
        try {
            $global:LASTEXITCODE = 0
            $o = & (Join-Path $PSScriptRoot "save_guard.ps1") -Check 2>&1 | ForEach-Object { "$_" }
            return @{ code = $LASTEXITCODE; text = ($o -join "; ") }
        } catch {
            return @{ code = 2; text = $_.Exception.Message }
        }
    }
    $e.Launch = { Start-Process "steam://rungameid/1203620" }
    $e.MenuShown = {
        param($sinceEpoch)
        Test-MenuShown (Join-Path $Game "enshrouded.log") ([DateTimeOffset]::FromUnixTimeMilliseconds([long]($sinceEpoch * 1000)).LocalDateTime)
    }
    # The menu screenshot, copied out of the game folder (uninstalling removes emf\screens).
    $e.Shot = {
        param([string]$tag)
        $r = & $script:Env.Bridge @("shot", "-MaxWidth", "1920") 30
        if ($r.code -ne 0 -or -not $r.reply.path) { return $null }
        New-Item -ItemType Directory -Force $script:shots | Out-Null
        $to = Join-Path $script:shots ("{0}_{1}.png" -f (Get-Date -Format "yyyyMMdd-HHmmss"), $tag)
        Copy-Item -LiteralPath $r.reply.path -Destination $to -Force
        return $to
    }
    $e.ShotBlank = { param($path) Test-ShotBlank $path }
    $e.Forget = { param($path) Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue }
    $e.LogText = { Read-SharedText (Join-Path $Game "emf\emf_core.log") }
    return $e
}

if ($MyInvocation.InvocationName -eq ".") { return }  # dot-sourced by the tests: functions only, they bring a fake Env
$script:Env = New-RealEnv
exit (Invoke-GameSession $Command)
