# Tests of scripts/game_session.ps1 without the game: its decisions (the person check, ownership, the lock, the world
# slot, when stop may end a game, the click geometry) and whole start / stop runs against a fake PC (a fake game
# process, bridge, clock, input, clicks, install and save check; real files in a temporary folder).
# scripts/test_tools.ps1 runs it. Exit code 0 = passed.
$ErrorActionPreference = "Stop"
$script:fails = 0
function Check([bool]$cond, [string]$what) {
    if ($cond) { "ok   $what" } else { "FAIL $what"; $script:fails++ }
}
$testRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $PSScriptRoot "game_session.ps1")  # dot-sourced: its functions, nothing runs

function New-TestFolder([string]$tag) {
    $f = Join-Path $env:TEMP ("emf_gs_" + $tag + "_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
    New-Item -ItemType Directory -Force $f | Out-Null
    return $f
}

# ---------------------------------------------------------------- the decisions
$now = 100000.0
Check ($null -ne (Get-PersonVerdict ($now - 5) $now $null 120 $false $now)) "person: input 5 s ago that was not this script's: refused"
Check ($null -eq (Get-PersonVerdict ($now - 300) $now $null 120 $false $now)) "person: nobody for 5 minutes: allowed"
Check ($null -eq (Get-PersonVerdict ($now - 5) $now ($now - 5) 120 $false $now)) "person: the last input was this script's own: allowed"
Check ($null -ne (Get-PersonVerdict ($now - 5) $now ($now - 60) 120 $false $now)) "person: input after this script's own (even with the game in front): refused"
Check ($null -eq (Get-PersonVerdict ($now - 5) $now $null 120 $true $now)) "person: -IgnoreIdle accepts input from before this run"
Check ($null -ne (Get-PersonVerdict ($now - 5) $now $null 120 $true ($now - 10))) "person: -IgnoreIdle does not accept input after the run began"
Check ((Test-ForeignInput ($now + 1) $now) -and -not (Test-ForeignInput ($now + 0.2) $now)) "Test-ForeignInput: input after the mark is a person's (not a no-op)"

$sess = [pscustomobject]@{ game_pid = 4242; game_start_epoch = 900.0; menu_shown_pid = 4242 }
Check (Test-OurGame $sess @{ Id = 4242; StartEpoch = 900.5 }) "ownership: the recorded pid and start time"
Check (-not (Test-OurGame $sess @{ Id = 4242; StartEpoch = 500.0 })) "ownership: a reused pid (another start time) is someone else's"
Check (-not (Test-OurGame $sess @{ Id = 77; StartEpoch = 900.0 })) "ownership: another pid is someone else's"
Check (-not (Test-OurGame $null @{ Id = 4242; StartEpoch = 900.0 })) "ownership: nothing recorded: not ours"

Check ((Get-MenuDecision "local" $true $sess 4242) -eq "show") "C1: -Character local right after this run launched the game shows the menu"
Check ((Get-MenuDecision "local" $false $sess 4242) -eq "local") "C1: -Character local after a screenshot shown for this pid goes on"
Check ((Get-MenuDecision "local" $false $sess 5555) -eq "show") "C1: a screenshot shown for another pid does not count"
Check ((Get-MenuDecision "local" $false ([pscustomobject]@{ game_pid = 4242 }) 4242) -eq "show") "C1: no screenshot recorded: show it first"
Check ((Get-MenuDecision "check" $false $sess 4242) -eq "show" -and (Get-MenuDecision "switch" $true $sess 4242) -eq "switch") "menu: check shows, switch switches"

Check ((Test-WorldSlot 1).ok) "slot 1 (LOCAL, the first slot) is the test world"
$v = Test-WorldSlot 257
Check (-not $v.ok -and $v.kill -and $v.text -match "Steam") "a Steam Cloud slot is to be ended"
$v = Test-WorldSlot 2
Check (-not $v.ok -and $v.kill -and $v.text -match "another LOCAL") "another local slot is to be ended"
$v = Test-WorldSlot $null
Check (-not $v.ok -and -not $v.kill -and $v.text -match "cannot tell") "an unknown slot stops start but ends nothing"

Check ((Get-StopPlan "none" $null $null 0 $false) -eq "kill") "stop: no world runs: end it at once"
Check ((Get-StopPlan "running" 1 5 30 $false) -eq "wait") "stop: saved 5 s ago: wait for it to exit by itself"
Check ((Get-StopPlan "running" 1 16 30 $false) -eq "kill") "stop: the LOCAL slot 1, saved 15 s ago: end it"
Check ((Get-StopPlan "running" 257 16 30 $false) -eq "leave") "stop: never ends a Cloud world by force"
Check ((Get-StopPlan $null $null 16 30 $false) -eq "leave") "stop: an unknown state counts as a world that may run"
Check ((Get-StopPlan "running" $null $null 130 $true) -eq "kill") "stop: -AllowKill ends it"
Check ((Get-StopPlan "running" 1 $null 60 $false) -eq "wait") "stop: no save line yet: wait up to 120 s"

Check ((Get-StartPath ([pscustomobject]@{ commands = @("hello", "load_world") })) -eq "bridge") "start path: a bridge that lists load_world starts the world without clicks"
Check ((Get-StartPath ([pscustomobject]@{ commands = @("hello", "quit") })) -eq "clicks" -and (Get-StartPath ([pscustomobject]@{ emf = "0.2.0" })) -eq "clicks" -and (Get-StartPath $null) -eq "clicks") "start path: the clicks only for a bridge without load_world (an older build)"
Check ((Test-LoadWorldMissing @{ ok = $false; code = "unavailable"; error = "load_world is off: the main-menu hooks are not installed" }) -and (Test-LoadWorldMissing @{ ok = $false; code = "unknown_command"; error = "unknown command 'load_world'" })) "load_world that is off on this build (or unknown) counts as missing: nothing was started"
Check (-not (Test-LoadWorldMissing @{ ok = $false; code = "unavailable"; error = "not at the main menu: a world is running" }) -and -not (Test-LoadWorldMissing @{ ok = $false; code = "refused"; error = "the guard stopped the start" }) -and -not (Test-LoadWorldMissing @{ ok = $false; code = "timeout"; error = "timed out" }) -and -not (Test-LoadWorldMissing $null)) "a refusal, a timeout or no reply is not 'missing': no clicks after them"
Check ((Test-WorldName "My World 1" "My World 1").ok) "world name: the expected one"
$v = Test-WorldName "Another World" "My World 1"
Check (-not $v.ok -and $v.kill -and $v.text -match "Another World") "world name: another name is a wrong world"
$v = Test-WorldName $null "My World 1"
Check ($v.ok -and $v.text -match "no world name") "world name: none known (an older build) is said, not a failure"

Check ((Test-Character @{ storage = "local"; slot = 1; name = "Character 2" } "Character 2").ok) "character: the LOCAL test character"
$v = Test-Character @{ storage = "steam"; slot = 1; name = "Steam Hero" } "Character 2"
Check (-not $v.ok -and $v.kill -and $v.text -match "uses a Steam character") "character: a Steam character is a wrong world"
$v = Test-Character @{ storage = "local"; slot = 2; name = "Someone" } "Character 2"
Check (-not $v.ok -and $v.kill -and $v.text -match "not 'Character 2'") "character: another LOCAL character is a wrong world"
foreach ($c in @(@{ storage = "unknown"; slot = $null; name = $null }, $null)) {
    $v = Test-Character $c "Character 2"
    Check (-not $v.ok -and -not $v.kill -and $v.text -match "can't tell which character runs: go back to the main menu and use load_world") "character: unknown or missing ($(if ($c) { 'unknown' } else { 'an older build' })) stops start but ends nothing"
}

$setup = Get-ClickSetup
Check (@($setup.steps).Count -eq 4 -and $setup.screen.width -eq 3840) "the world clicks come from start_local_world.ps1"
Check ($null -eq (Test-ClickGeometry @{ width = 3840; height = 2160 } @{ left = 114; top = 114; right = 2994; bottom = 1651 } $setup)) "geometry: the expected screen and window"
Check ((Test-ClickGeometry @{ width = 2560; height = 1440 } @{ left = 0; top = 0; right = 2880; bottom = 1537 } $setup) -match "screen") "geometry: another screen is refused"
Check ((Test-ClickGeometry @{ width = 3840; height = 2160 } @{ left = 0; top = 0; right = 1920; bottom = 1080 } $setup) -match "window") "geometry: another window size is refused"

$tmp = New-TestFolder "units"
try {
    # window mode: only the value changes, byte for byte, with or without a BOM
    $settingsFile = Join-Path $tmp "local_settings.json"
    $body = "{`r`n`t`"version`": 13,`r`n`t`"graphics`": {`r`n`t`t`"windowMode`": `"Fullscreen`",`r`n`t`t`"vsync`": true`r`n`t}`r`n}"
    [IO.File]::WriteAllText($settingsFile, $body, (New-Object Text.UTF8Encoding($false)))
    $before = Set-WindowMode $settingsFile "Windowed"
    $want = [Text.Encoding]::UTF8.GetBytes($body.Replace('"windowMode": "Fullscreen"', '"windowMode": "Windowed"'))
    Check ($before -eq "Fullscreen" -and [Convert]::ToBase64String([IO.File]::ReadAllBytes($settingsFile)) -eq [Convert]::ToBase64String($want)) "Set-WindowMode changes only the value"
    [IO.File]::WriteAllText($settingsFile, $body, (New-Object Text.UTF8Encoding($true)))
    [void](Set-WindowMode $settingsFile "Windowed")
    $bytes = [IO.File]::ReadAllBytes($settingsFile)
    Check ($bytes[0] -eq 0xEF -and (Get-WindowMode $settingsFile) -eq "Windowed") "Set-WindowMode keeps a BOM"

    # -Mods and the install marker
    $m = Resolve-ModSpec "template_lua" $testRepo
    Check ($m.Id -eq "my_first_mod") "-Mods template_lua installs as its mod.json id"
    foreach ($dots in @("..", ".", ".hidden", "Bad Id")) {
        $threw = $false
        try { [void](Resolve-ModSpec "template_lua=$dots" $testRepo) } catch { $threw = $_.Exception.Message -match "not a mod id" }
        Check $threw "-Mods with the id '$dots' is refused"
    }
    $g = Join-Path $tmp "game"
    New-Item -ItemType Directory -Force (Join-Path $g "emf\mods\clock") | Out-Null
    $threw = $false
    try { [void](Install-ExtraMods @("template_lua=clock") $g $testRepo) } catch { $threw = $_.Exception.Message -match "did not make it" }
    Check ($threw -and (Test-Path (Join-Path $g "emf\mods\clock"))) "-Mods refuses to replace a folder it did not make"
    $r = Install-ExtraMods @("template_lua") $g $testRepo
    Write-InstallMarker $g "t" $r.made
    $r = Install-ExtraMods @("template_lua") $g $testRepo  # an earlier start made it: replaced
    Check ((Test-Path (Join-Path $g "emf\mods\my_first_mod\main.lua")) -and $r.made -contains "my_first_mod") "-Mods replaces a folder an earlier start made"
    Check ((Get-EmfInstall $g).marker) "Get-EmfInstall sees game_session's marker"

    # the lock
    $lock = Join-Path $tmp "game.lock"
    $alive = { param($id) $id -eq 4242 }
    $r = Enter-GameLock $lock "me" 4242 $alive
    Check ($r.ok -and (Get-Content -Raw $lock) -match "^me \S+ \S+ pid=4242") "the lock reads '<owner> <time> pid=<game pid>'"
    Check (-not (Enter-GameLock $lock "other" 1 $alive).ok) "another lock owner is refused while the pid runs"
    Check ((Get-LockState $lock "me" $alive).state -eq "ours") "our own lock"
    Set-Content -LiteralPath $lock -Value "other 2026-10-27 01:00:00 pid=999999"
    $r = Enter-GameLock $lock "me" 4242 $alive
    Check ($r.ok -and $r.text -match "stale") "a lock whose pid is gone is stale and taken over"
    Set-Content -LiteralPath $lock -Value "manual 2026-10-27 02:20:10"
    Check (-not (Enter-GameLock $lock "me" 4242 $alive).ok) "a lock without a pid (made by hand) is never stale"
    [IO.File]::WriteAllText($lock, "")
    Check ((Get-LockState $lock "me" $alive).state -eq "held") "an empty lock just made is held (not free)"
    (Get-Item -LiteralPath $lock).LastWriteTime = (Get-Date).AddMinutes(-5)
    Check ((Get-LockState $lock "me" $alive).state -eq "stale") "an empty lock older than a minute is stale"

    # the menu marker and black frames
    $glog = Join-Path $tmp "enshrouded.log"
    Set-Content -LiteralPath $glog "[I 00:00:03,033] [GameUi] Entered PreGame"
    Add-Content -LiteralPath $glog "[I 00:00:06,283] [renderer] isReadyForRendering = true"
    Check (-not (Test-MenuShown $glog)) "the early 'Entered PreGame' is not the menu"
    Add-Content -LiteralPath $glog "[I 00:00:08,521] [GameUi] Entered PreGame"
    Check ((Test-MenuShown $glog) -and -not (Test-MenuShown $glog ((Get-Date).AddMinutes(5)))) "the menu marker after the renderer is ready, in this run's log"
    Add-Type -AssemblyName System.Drawing
    $black = Join-Path $tmp "black.png"
    $bmp = New-Object System.Drawing.Bitmap 96, 54
    $gr = [System.Drawing.Graphics]::FromImage($bmp)
    $gr.Clear([System.Drawing.Color]::Black)
    $bmp.Save($black, [System.Drawing.Imaging.ImageFormat]::Png)
    $gr.Clear([System.Drawing.Color]::FromArgb(40, 90, 140))
    $picture = Join-Path $tmp "picture.png"
    $bmp.Save($picture, [System.Drawing.Imaging.ImageFormat]::Png)
    $gr.Dispose(); $bmp.Dispose()
    Check ((Test-ShotBlank $black) -and -not (Test-ShotBlank $picture)) "Test-ShotBlank tells a black frame from a picture"
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- whole runs against a fake PC
# The fake PC: a hashtable the fake Env's script blocks read and change.
function New-Pc {
    return @{
        now = 100000.0; lastInput = 0.0; game = $null; nextPid = 4242; bridge = $true; status = "none"; slot = $null
        player = $null; loadSlot = 1; clicks = New-Object System.Collections.ArrayList; keys = New-Object System.Collections.ArrayList
        lines = New-Object System.Collections.ArrayList; launches = 0; installs = 0; uninstalls = 0
        killed = New-Object System.Collections.ArrayList; closed = 0; quitAsked = $false; quitExits = $true; log = ""; logOnQuit = ""
        screen = @{ width = 3840; height = 2160 }; rect = "rect 114 114 2994 1651"; places = 0
        saveGuard = @{ code = 0; text = "unchanged: character0.ksd; unchanged: world0.ksd" }; saveGuardCalls = 0
        menu = $true; blank = $false; noShot = $false; shots = 0; events = @(); eventsStarted = 0; eventsStopped = 0
        inFront = $true; uninstallThrows = $false; onClick = $null; gameDir = ""; stateFails = $false; failSleepAt = $null
        bridgeCalls = New-Object System.Collections.ArrayList; launchFails = $false
        eventsDelay = 0; eventReads = 0; readyAfter = 0; readyAsks = 0; readerFails = $false; firstClickReadyAsks = -1
        # load_world: whether hello lists it (an older build does not), what it answers (ok, refused, off, timeout,
        # busy, silent: no answer in time), after how many polls, the world's name in the state and in the reply
        hasLoadWorld = $true; loadWorld = "ok"; loadWorldPolls = 2; worldName = "My World 1"; replyWorld = $null
        loadWorldCalls = New-Object System.Collections.ArrayList; bridgeStops = 0; noWorldName = $false
        readyAsksAtLoadWorld = -1
        # the state's character: the menu's selection (the clicks start with it); load_world selects LOCAL character
        # 1 itself, or loadedCharacter when a test sets one; noCharacter: an older build without the field
        character = @{ storage = "local"; slot = 1; name = "Character 2" }; loadedCharacter = $null; noCharacter = $false
    }
}

# The fake game after a start: the load event (it comes before the world runs), then the world and the player.
function Set-FakeWorldLoaded($pc) {
    $pc.events = @(@{ name = "load"; fields = @{ slot = $pc.loadSlot } })
    $pc.status = "running"
    $pc.slot = $pc.loadSlot
    $pc.player = @{ id = 1; position = @{ x = 1.0; y = 2.0; z = 3.0 } }
}

function New-FakeEnv($pc) {
    $e = @{}
    $setLoaded = ${function:Set-FakeWorldLoaded}  # (closures do not see this script's functions: they take it along)
    $e.Now = { $pc.now }.GetNewClosure()
    $e.Sleep = {
        param($ms)
        if ($null -ne $pc.failSleepAt -and $pc.now -ge $pc.failSleepAt) { throw "something unexpected" }
        $pc.now += $ms / 1000.0
    }.GetNewClosure()
    $e.Say = { param($t) [void]$pc.lines.Add("$t") }.GetNewClosure()
    $e.GetGame = { $pc.game }.GetNewClosure()
    $e.PidAlive = { param($id) [bool]($pc.game -and $pc.game.Id -eq $id) }.GetNewClosure()
    $e.GameListed = { param($id) [bool]($pc.game -and $pc.game.Id -eq $id) }.GetNewClosure()
    $e.WaitExit = {
        param($id, $ms)
        if ($pc.game -and $pc.quitAsked -and $pc.quitExits) { $pc.game = $null; return $true }
        $pc.now += $ms / 1000.0
        if ($pc.logOnQuit -and $pc.quitAsked) { $pc.log += $pc.logOnQuit; $pc.logOnQuit = "" }
        return -not $pc.game
    }.GetNewClosure()
    $e.Kill = { param($id) [void]$pc.killed.Add($id); $pc.game = $null }.GetNewClosure()
    $e.CloseWindow = { param($h) $pc.closed++; $pc.quitAsked = $true }.GetNewClosure()
    $e.LastInputEpoch = { $pc.lastInput }.GetNewClosure()
    $e.GameInFront = { $pc.inFront }.GetNewClosure()
    $e.Screen = { $pc.screen }.GetNewClosure()
    $e.Bridge = {
        param([string[]]$a, [int]$t)
        [void]$pc.bridgeCalls.Add(($a | Select-Object -First 2) -join " ")
        if (-not $pc.game -or -not $pc.bridge) { return @{ code = 2; reply = $null; message = "no connection to the game" } }
        switch ($a[0]) {
            "hello" {
                $commands = @("hello", "console", "state", "mods", "log", "wait", "events", "screenshot", "mod_key", "quit")
                if ($pc.hasLoadWorld) { $commands += "load_world" }
                return @{ code = 0; reply = @{ emf = "0.3.0"; api = 17; game_build = 1273043; pid = $pc.game.Id; commands = $commands } }
            }
            "state" {
                if ($pc.stateFails) { return @{ code = 2; reply = $null; message = "no reply" } }
                $name = if ($pc.status -eq "running" -and -not $pc.noWorldName) { $pc.worldName } else { $null }
                $reply = @{ world = @{ status = $pc.status; kind = $(if ($pc.status -eq "running") { "server" }); slot = $pc.slot; name = $name }; player = $pc.player }
                if (-not $pc.noCharacter) { $reply.character = $pc.character }
                return @{ code = 0; reply = $reply }
            }
            "quit" { $pc.quitAsked = $true; return @{ code = 0; reply = @{ quitting = $true } } }
            "wait" {
                if ($a[1] -eq "world" -and $pc.status -eq "running") { return @{ code = 0; reply = @{ ok = $true } } }
                if ($a[1] -eq "player" -and $pc.player) { return @{ code = 0; reply = @{ ok = $true } } }
                return @{ code = 1; reply = @{ ok = $false; code = "timeout"; error = "timed out" } }
            }
        }
        return @{ code = 1; reply = $null; message = "unknown" }
    }.GetNewClosure()
    # a bridge command that runs while start watches the load events (load_world)
    $e.BridgeStart = {
        param([string[]]$a)
        [void]$pc.loadWorldCalls.Add(($a -join " "))
        $pc.readyAsksAtLoadWorld = $pc.readyAsks
        $h = @{ args = $a; polls = 0; done = $false }
        if ($pc.loadWorld -eq "ok") { $pc.events = @(@{ name = "load"; fields = @{ slot = $pc.loadSlot } }) }  # before the world runs
        return $h
    }.GetNewClosure()
    $e.BridgeDone = {
        param($h)
        $h.polls++
        if ($pc.loadWorld -eq "silent" -or $h.polls -lt $pc.loadWorldPolls) { return $false }
        if (-not $h.done -and $pc.loadWorld -eq "ok") {
            & $setLoaded $pc
            $pc.character = if ($pc.loadedCharacter) { $pc.loadedCharacter } else { @{ storage = "local"; slot = 1; name = $h.args[3] } }
        }
        $h.done = $true
        return $true
    }.GetNewClosure()
    $e.BridgeResult = {
        param($h)
        if (-not $h.done) { return @{ code = 2; reply = $null; message = "the bridge client did not finish" } }
        switch ($pc.loadWorld) {
            "ok" {
                $world = if ($pc.replyWorld) { $pc.replyWorld } else { $pc.worldName }
                return @{ code = 0; reply = @{ ok = $true; slot = 1; character = 1; world = $world; character_name = $h.args[3]; ms = 12109 } }
            }
            "refused" { return @{ code = 1; reply = @{ ok = $false; code = "refused"; error = "LOCAL character slot 1 is 'Character 1', not 'Character 2'" } } }
            "off" { return @{ code = 1; reply = @{ ok = $false; code = "unavailable"; error = "load_world is off: the main-menu hooks are not installed (an unknown game build)" } } }
            "timeout" { return @{ code = 1; reply = @{ ok = $false; code = "timeout"; error = "timed out waiting for the world to run" } } }
            "busy" { return @{ code = 1; reply = @{ ok = $false; code = "busy"; error = "a load_world is already in progress" } } }
        }
        return @{ code = 1; reply = $null; message = "unknown" }
    }.GetNewClosure()
    $e.BridgeStop = { param($h) $pc.bridgeStops++ }.GetNewClosure()
    $e.EventsStart = { $pc.eventsStarted++; return "events" }.GetNewClosure()
    $e.EventsRead = {
        param($h)
        $pc.eventReads++
        if ($pc.eventReads -le $pc.eventsDelay) { return @() }  # the event reaches the file a moment later
        return $pc.events
    }.GetNewClosure()
    $e.EventsReady = { param($h) $pc.readyAsks++; return (-not $pc.readerFails) -and $pc.readyAsks -gt $pc.readyAfter }.GetNewClosure()
    $e.EventsStop = { param($h) $pc.eventsStopped++ }.GetNewClosure()
    $e.Ui = {
        param([string[]]$a)
        switch ($a[0]) {
            "click" {
                if ($pc.clicks.Count -eq 0) { $pc.firstClickReadyAsks = $pc.readyAsks }
                [void]$pc.clicks.Add("$($a[1]),$($a[2])")
                if ($pc.onClick) { & $pc.onClick $pc }
                if ("$($a[1]),$($a[2])" -eq "1555,1174") { & $setLoaded $pc }  # Offline: Start: the world loads
                return @{ code = 0; out = "clicked" }
            }
            "key" { [void]$pc.keys.Add($a[1]); return @{ code = 0; out = "key" } }
            "rect" { return @{ code = 0; out = $pc.rect } }
            "place" { $pc.places++; return @{ code = 0; out = "placed" } }
        }
        return @{ code = 0; out = "focused" }
    }.GetNewClosure()
    $e.Install = {
        $pc.installs++
        New-Item -ItemType Directory -Force (Join-Path $pc.gameDir "emf\mods") | Out-Null
        Set-Content -LiteralPath (Join-Path $pc.gameDir "emf\emf_core.dll") "x"
        return @("installed EMF into $($pc.gameDir)")
    }.GetNewClosure()
    $e.Uninstall = {
        if ($pc.uninstallThrows) { throw "the game is running; close it first" }
        $pc.uninstalls++
        Remove-Item -Recurse -Force (Join-Path $pc.gameDir "emf") -ErrorAction SilentlyContinue
        return @("removed emf")
    }.GetNewClosure()
    $e.SaveGuard = { $pc.saveGuardCalls++; return $pc.saveGuard }.GetNewClosure()
    $e.Launch = {
        $pc.launches++
        if (-not $pc.launchFails) { $pc.game = @{ Id = $pc.nextPid; StartEpoch = $pc.now; WindowHandle = 1 }; $pc.status = "none" }
    }.GetNewClosure()
    $e.MenuShown = { param($s) $pc.menu }.GetNewClosure()
    $e.Shot = { param($tag) if ($pc.noShot) { return $null }; $pc.shots++; return "C:\fake\$tag.png" }.GetNewClosure()
    $e.ShotBlank = { param($p) $pc.blank }.GetNewClosure()
    $e.Forget = { param($p) }
    $e.LogText = { $pc.log }.GetNewClosure()
    return $e
}

# One command on the fake PC with these switches; returns the exit code (the lines are in $pc.lines).
function Invoke-Fake($pc, [string]$cmd, [hashtable]$opts = @{}) {
    $script:Character = "check"; $script:Mods = @(); $script:KeepInstalled = $false; $script:Json = $false
    $script:IgnoreIdle = $false; $script:IgnoreLock = $false; $script:AllowUnansweredBridge = $false
    $script:AdoptGame = $false; $script:AllowKill = $false; $script:LockOwner = "tester"
    $script:CharacterName = "Character 2"; $script:WorldName = "My World 1"
    foreach ($k in $opts.Keys) { Set-Variable -Scope Script -Name $k -Value $opts[$k] }
    $script:Env = New-FakeEnv $pc
    $pc.lines.Clear()
    return Invoke-GameSession $cmd
}

function New-Case {
    $dir = New-TestFolder "case"
    $script:Game = Join-Path $dir "game"
    New-Item -ItemType Directory -Force $script:Game | Out-Null
    $script:Settings = Join-Path $dir "local_settings.json"
    Set-Content -LiteralPath $script:Settings "{`"graphics`": {`"windowMode`": `"Fullscreen`"}}"
    $script:LockFile = Join-Path $dir "game.lock"
    $script:StateFile = Join-Path $dir "session.json"
    $pc = New-Pc
    $pc.gameDir = $script:Game
    $pc.dir = $dir
    return $pc
}
function Text($pc) { return ($pc.lines -join "`n") }
function Done($pc) { Remove-Item -Recurse -Force $pc.dir -ErrorAction SilentlyContinue }

# C1: a fresh start always stops at the menu, whatever -Character says
$pc = New-Case
$code = Invoke-Fake $pc "start" @{ Character = "local"; Mods = @("template_lua") }
$st = Read-SessionState $StateFile
Check ($code -eq 2 -and $pc.launches -eq 1 -and $pc.clicks.Count -eq 0 -and $st.menu_shown_pid -eq 4242) "C1: start that launched the game stops at the menu screenshot even with -Character local (no clicks)"
Check ((Text $pc) -match "character played last" -and (Get-WindowMode $Settings) -eq "Windowed" -and (Test-Path (Join-Path $Game "emf\mods\my_first_mod\main.lua")) -and (Get-EmfInstall $Game).marker) "C1: it says why; EMF, -Mods and the marker are installed; windowed"
Check ((Get-Content -Raw $LockFile) -match "^tester .* pid=4242") "the lock names the game's pid once it runs"
Check ((Text $pc) -match "load_world, which selects LOCAL character slot 1 itself" -and (Text $pc) -notmatch "Character switch") "C1: with load_world the menu message says it selects the LOCAL character itself"
# then -Character local loads the world through load_world: no clicks, no focus, the load event watched
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 0 -and $pc.clicks.Count -eq 0 -and $pc.keys.Count -eq 0 -and $pc.loadWorldCalls.Count -eq 1 -and $pc.eventsStarted -eq 1 -and $pc.eventsStopped -eq 1 -and $pc.bridgeStops -eq 1 -and (Text $pc) -match "ready: player 1") "start -Character local after the screenshot: load_world (no clicks), the load event watched, ready"
Check ($pc.loadWorldCalls[0] -eq "load_world 1 1 Character 2 -TimeoutMs 300000") "load_world asks for LOCAL world slot 1 with LOCAL character 1 named 'Character 2' ($($pc.loadWorldCalls[0]))"
Check ((Text $pc) -match "through the bridge's load_world \(no clicks\)" -and (Text $pc) -match "'My World 1' runs with 'Character 2'" -and (Text $pc) -match "world running: the LOCAL slot 1, 'My World 1', with LOCAL character slot 1 'Character 2'") "the output says which path started the world, the world's name and the character"
Check ($pc.readyAsksAtLoadWorld -ge 1) "load_world is sent after the load-event reader listens"
Check ((Read-SessionState $StateFile).own_input_epoch -eq $null) "load_world sends no input (the session records none)"
# stop: quit, uninstall (the marker), Fullscreen, saves, lock released, the session keeps only the own input
$code = Invoke-Fake $pc "stop"
$st = Read-SessionState $StateFile
Check ($code -eq 0 -and $pc.uninstalls -eq 1 -and (Get-WindowMode $Settings) -eq "Fullscreen" -and -not (Test-Path $LockFile) -and $pc.saveGuardCalls -ge 1) "stop: quit, uninstalled, Fullscreen, saves checked, lock released"
Check ($null -eq $st.game_pid) "stop: the session file forgets the game"
Done $pc

# C1: -Character local without a screenshot for this pid shows one first
$pc = New-Case
$pc.game = @{ Id = 4242; StartEpoch = 900.0; WindowHandle = 1 }
Write-SessionState $StateFile ([ordered]@{ owner = "tester"; game_pid = 4242; game_start_epoch = 900.0; menu_shown_pid = 1111 })
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 2 -and $pc.clicks.Count -eq 0 -and (Text $pc) -match "needs a menu screenshot shown for this game first") "C1: -Character local for a game whose menu was not shown: the screenshot first, no clicks"
Done $pc

# someone else's game
$pc = New-Case
$pc.game = @{ Id = 4242; StartEpoch = 900.0; WindowHandle = 1 }
$code = Invoke-Fake $pc "start"
Check ($code -eq 3 -and -not (Test-Path $LockFile) -and (Text $pc) -match "someone else's") "start refuses a game it did not record (and takes no lock)"
$code = Invoke-Fake $pc "stop"
Check ($code -eq 3 -and -not $pc.quitAsked -and $pc.killed.Count -eq 0) "stop refuses a game it did not record"
$code = Invoke-Fake $pc "start" @{ AdoptGame = $true }
Check ($code -eq 2 -and (Read-SessionState $StateFile).game_pid -eq 4242) "-AdoptGame takes the running game over (and still shows the menu)"
Done $pc

# the lock: someone else's refuses start and stop
$pc = New-Case
Set-Content -LiteralPath $LockFile "manual 2026-10-27 02:20:10"
$code = Invoke-Fake $pc "start"
Check ($code -eq 3 -and $pc.launches -eq 0) "start refuses while someone else holds the default lock"
$code = Invoke-Fake $pc "stop"
Check ($code -eq 3 -and $pc.uninstalls -eq 0) "stop refuses while someone else holds the lock"
[IO.File]::WriteAllText($LockFile, "")
$code = Invoke-Fake $pc "stop"
Check ($code -eq 3) "an empty lock is not free for stop"
Done $pc

# a wrong world caught on the load event (load_world, and the click fallback of an older build)
foreach ($lw in @($true, $false)) {
    $how = if ($lw) { "load_world" } else { "clicks" }
    $pc = New-Case
    $pc.hasLoadWorld = $lw
    $code = Invoke-Fake $pc "start" @{ Mods = @() }
    $pc.loadSlot = 257
    $pc.bridgeCalls.Clear()
    $code = Invoke-Fake $pc "start" @{ Character = "local" }
    Check ($pc.bridgeCalls -notcontains "wait world") "${how}: the wrong world is caught on its load event, before any wait for the world"
    Check ($code -eq 1 -and $pc.killed -contains 4242 -and (Text $pc) -match "WRONG WORLD" -and $pc.saveGuardCalls -ge 2) "${how}: a Cloud world on the load event: ended at once, saves checked, loud"
    Check ($pc.uninstalls -eq 1 -and (Get-WindowMode $Settings) -eq "Fullscreen" -and -not (Test-Path $LockFile)) "${how}: then EMF removed, Fullscreen, lock released"
    if ($lw) { Check ($pc.bridgeStops -eq 1 -and $pc.eventsStopped -eq 1) "load_world: the load_world client and the load-event reader are stopped" }
    Done $pc
}

# load_world: its refusals stop the session (exit 1) with the reason; no clicks follow, nothing is ended
foreach ($case in @(
        @{ answer = "refused"; words = "refused.*not 'Character 2'" },
        @{ answer = "timeout"; words = "may still start" },
        @{ answer = "busy"; words = "already in progress" },
        @{ answer = "silent"; words = "no answer from load_world" })) {
    $pc = New-Case
    [void](Invoke-Fake $pc "start")
    $pc.loadWorld = $case.answer
    $code = Invoke-Fake $pc "start" @{ Character = "local" }
    Check ($code -eq 1 -and $pc.clicks.Count -eq 0 -and $pc.keys.Count -eq 0 -and $pc.killed.Count -eq 0 -and (Text $pc) -match $case.words) "load_world $($case.answer): reported, the session stops (exit 1), no clicks, nothing ended"
    Check ((Test-Path $LockFile) -and $pc.uninstalls -eq 0 -and $pc.bridgeStops -eq 1 -and $pc.eventsStopped -eq 1) "load_world $($case.answer): the game runs on at the menu, so EMF and the lock stay for stop"
    Done $pc
}

# load_world: a wrong world name after the start closes the game as a wrong slot does
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.replyWorld = "Other World"
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 1 -and $pc.killed -contains 4242 -and (Text $pc) -match "WRONG WORLD" -and (Text $pc) -match "Other World" -and $pc.bridgeCalls -notcontains "wait player") "load_world: the reply names another world: ended at once, loud"
Done $pc
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.worldName = "Other World"; $pc.replyWorld = "My World 1"
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 1 -and $pc.killed -contains 4242 -and (Text $pc) -match "WRONG WORLD" -and (Text $pc) -match "Other World") "state.world.name names another world after the start: ended at once, loud"
Done $pc
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.noWorldName = $true
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 0 -and (Text $pc) -match "no world name") "a state without world.name (an older build) is said; the slot decides"
Done $pc
# a world this run did not open, with another name: refused, not ended
$pc = New-Case
$pc.game = @{ Id = 4242; StartEpoch = 900.0; WindowHandle = 1 }
$pc.status = "running"; $pc.slot = 1; $pc.worldName = "Other World"
Write-SessionState $StateFile ([ordered]@{ owner = "tester"; game_pid = 4242; game_start_epoch = 900.0; menu_shown_pid = 4242 })
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 3 -and $pc.killed.Count -eq 0 -and $pc.loadWorldCalls.Count -eq 0) "a running world with another name that this run did not open: refused, not ended"
Done $pc

# the click fallback: only for a bridge without load_world, or one whose load_world is off on this build
$pc = New-Case
$pc.hasLoadWorld = $false
[void](Invoke-Fake $pc "start")
Check ((Text $pc) -match "must read '\[LOCAL\] Character 2'" -and (Text $pc) -match "start -Character switch") "an older build: the menu message says the label must read LOCAL, else -Character switch"
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 0 -and $pc.clicks.Count -eq 4 -and $pc.loadWorldCalls.Count -eq 0 -and (Text $pc) -match "no load_world" -and (Text $pc) -match "click fallback") "an older build without load_world: the click fallback, said so"
$code = Invoke-Fake $pc "stop"
$st = Read-SessionState $StateFile
Check ($code -eq 0 -and $null -eq $st.game_pid -and $null -ne $st.own_input_epoch) "after the clicks, the session file keeps this script's last input through stop"
Done $pc
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.loadWorld = "off"
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 0 -and $pc.loadWorldCalls.Count -eq 1 -and $pc.clicks.Count -eq 4 -and (Text $pc) -match "load_world is off" -and (Text $pc) -match "click fallback") "load_world off on this build (nothing started): the click fallback, said so"
Done $pc
# with load_world, the screen size does not matter (no clicks); the clicks still refuse another size
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.screen = @{ width = 2560; height = 1440 }; $pc.rect = "rect 0 0 1920 1080"
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 0 -and $pc.clicks.Count -eq 0 -and $pc.places -eq 0) "load_world on another screen size: the world starts (the 3840x2160 limit is the fallback's)"
Done $pc
# the person check comes before load_world too
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.lastInput = $pc.now - 10
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 3 -and $pc.loadWorldCalls.Count -eq 0 -and (Text $pc) -match "someone used the PC") "someone used the PC 10 s ago: no load_world"
Done $pc

# when the clean-up after a wrong world fails, the lock is kept (as stop keeps it)
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.loadSlot = 257
$pc.uninstallThrows = $true
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 1 -and $pc.killed -contains 4242 -and (Test-Path $LockFile) -and (Text $pc) -match "kept the game lock") "a failed clean-up after a wrong world keeps the lock and says so"
Done $pc

# a load event that reaches the reader as the world already runs is still read (and the world ended at once)
foreach ($lw in @($true, $false)) {
    $pc = New-Case
    $pc.hasLoadWorld = $lw
    [void](Invoke-Fake $pc "start")
    $pc.loadSlot = 257; $pc.eventsDelay = 1; $pc.loadWorldPolls = 1; $pc.bridgeCalls.Clear()
    $code = Invoke-Fake $pc "start" @{ Character = "local" }
    Check ($code -eq 1 -and $pc.killed -contains 4242 -and $pc.bridgeCalls -notcontains "wait world") "$(if ($lw) { 'load_world' } else { 'clicks' }): a load event read only once the world runs still ends it before any wait"
    Done $pc
}
# the world clicks (and load_world) wait until the load-event reader listens; a reader that never does is said,
# the state decides
$pc = New-Case
$pc.hasLoadWorld = $false
[void](Invoke-Fake $pc "start")
$pc.readyAfter = 3
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 0 -and $pc.firstClickReadyAsks -ge 4) "the first world click comes after the load-event reader listens"
Done $pc
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.readyAfter = 3
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 0 -and $pc.readyAsksAtLoadWorld -ge 4) "load_world is sent after the load-event reader listens"
Done $pc
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.readerFails = $true
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 0 -and (Text $pc) -match "did not start listening") "a load-event reader that never listens is said; the state's slot decides"
Done $pc

# -CharacterName and -WorldName: another PC's test character and world (still LOCAL slot 1 each)
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.worldName = "Test World"
$code = Invoke-Fake $pc "start" @{ Character = "local"; CharacterName = "Tester"; WorldName = "Test World" }
Check ($code -eq 0 -and $pc.loadWorldCalls[0] -eq "load_world 1 1 Tester -TimeoutMs 300000" -and (Text $pc) -match "'Test World'") "-CharacterName and -WorldName name another PC's test character and world"
Done $pc
$pc = New-Case
[void](Invoke-Fake $pc "start")
$code = Invoke-Fake $pc "status"
Check ($code -eq 0 -and (Text $pc) -match "bridge: .*load_world" -and (Text $pc) -match "character: +local, slot 1, 'Character 2'") "status says whether the bridge has load_world, and the character"
Done $pc

# the character after the start: a Steam or another character is the wrong world (ended at once); one the bridge
# cannot tell stops start and ends nothing (an older build without the field: also after the click fallback)
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.loadedCharacter = @{ storage = "steam"; slot = 1; name = "Steam Hero" }
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 1 -and $pc.killed -contains 4242 -and (Text $pc) -match "WRONG WORLD" -and (Text $pc) -match "uses a Steam character" -and $pc.bridgeCalls -notcontains "wait player") "load_world, then the state says a Steam character: ended at once, loud"
Done $pc
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.loadedCharacter = @{ storage = "local"; slot = 2; name = "Someone" }
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 1 -and $pc.killed -contains 4242 -and (Text $pc) -match "not 'Character 2'") "load_world, then the state says another LOCAL character: ended at once"
Done $pc
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.loadedCharacter = @{ storage = "unknown"; slot = $null; name = $null }
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 1 -and $pc.killed.Count -eq 0 -and (Text $pc) -match "can't tell which character runs" -and (Test-Path $LockFile)) "load_world, then an unknown character: start stops (exit 1), nothing ended, the lock stays for stop"
Done $pc
$pc = New-Case
$pc.hasLoadWorld = $false
[void](Invoke-Fake $pc "start")
$pc.character = @{ storage = "steam"; slot = 1; name = "Steam Hero" }  # the menu had the user's Steam character selected
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 1 -and $pc.clicks.Count -eq 4 -and $pc.killed -contains 4242 -and (Text $pc) -match "WRONG WORLD") "the click fallback with a Steam character selected: the world is ended at once"
Done $pc
$pc = New-Case
$pc.hasLoadWorld = $false; $pc.noCharacter = $true
[void](Invoke-Fake $pc "start")
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 1 -and $pc.clicks.Count -eq 4 -and $pc.killed.Count -eq 0 -and (Text $pc) -match "can't tell which character runs") "an older build without state.character: after the clicks start stops (exit 1), nothing ended"
Done $pc
# a world this run did not open, played with a Steam character: refused, not ended; unknown: stopped, not ended
$pc = New-Case
$pc.game = @{ Id = 4242; StartEpoch = 900.0; WindowHandle = 1 }
$pc.status = "running"; $pc.slot = 1; $pc.character = @{ storage = "steam"; slot = 1; name = "Steam Hero" }
Write-SessionState $StateFile ([ordered]@{ owner = "tester"; game_pid = 4242; game_start_epoch = 900.0; menu_shown_pid = 4242 })
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 3 -and $pc.killed.Count -eq 0 -and (Text $pc) -match "uses a Steam character") "a running world this run did not open, with a Steam character: refused (exit 3), not ended"
$pc.character = @{ storage = "unknown"; slot = $null; name = $null }
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 1 -and $pc.killed.Count -eq 0 -and (Text $pc) -match "can't tell which character runs") "a running world this run did not open, character unknown: stopped (exit 1), not ended"
Done $pc

# the openedWorld guard: a wrong world this run did not open is left alone
$pc = New-Case
$pc.game = @{ Id = 4242; StartEpoch = 900.0; WindowHandle = 1 }
$pc.status = "running"; $pc.slot = 257
Write-SessionState $StateFile ([ordered]@{ owner = "tester"; game_pid = 4242; game_start_epoch = 900.0; menu_shown_pid = 4242 })
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 3 -and $pc.killed.Count -eq 0 -and $pc.clicks.Count -eq 0 -and $pc.loadWorldCalls.Count -eq 0) "openedWorld guard: a wrong world this run did not open: refused, not ended"
# an unknown slot after the load: stop, end nothing
$pc.slot = $null
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 1 -and $pc.killed.Count -eq 0 -and (Text $pc) -match "cannot tell") "an unknown slot: start stops (exit 1), nothing ended"
Done $pc

# a person who touches the PC during the clicks stops them; -IgnoreIdle does not skip that
foreach ($ignore in @($false, $true)) {
    $pc = New-Case
    $pc.hasLoadWorld = $false
    [void](Invoke-Fake $pc "start")
    $pc.onClick = { param($p) if ($p.clicks.Count -eq 2) { $p.lastInput = $p.now + 1 } }  # during the 2.5 s after it
    $pc.lastInput = $pc.now - 1000
    $code = Invoke-Fake $pc "start" @{ Character = "local"; IgnoreIdle = $ignore }
    Check ($code -eq 3 -and $pc.clicks.Count -eq 2 -and (Text $pc) -match "someone used the mouse or keyboard") "input during the clicks stops them after the 2nd (-IgnoreIdle $ignore)"
    Done $pc
}
# the person check at start
$pc = New-Case
$pc.lastInput = $pc.now - 10
$code = Invoke-Fake $pc "start"
Check ($code -eq 3 -and $pc.launches -eq 0 -and -not (Test-Path $LockFile)) "someone used the PC 10 s ago: start refuses (and releases the lock)"
$code = Invoke-Fake $pc "start" @{ IgnoreIdle = $true }
Check ($code -eq 2 -and $pc.launches -eq 1) "-IgnoreIdle: the user handed over"
Done $pc

# the click geometry (the fallback's), and a black menu
$pc = New-Case
$pc.hasLoadWorld = $false
[void](Invoke-Fake $pc "start")
$pc.screen = @{ width = 2560; height = 1440 }
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 3 -and $pc.clicks.Count -eq 0 -and (Text $pc) -match "coordinates") "the click fallback on another screen size: refused before any click"
$pc.screen = @{ width = 3840; height = 2160 }; $pc.blank = $true
$code = Invoke-Fake $pc "start" @{ Character = "local" }
Check ($code -eq 1 -and $pc.clicks.Count -eq 0 -and (Text $pc) -match "black") "a black menu stops start (no clicks)"
Done $pc

# EMF installed without the marker
$pc = New-Case
New-Item -ItemType Directory -Force (Join-Path $Game "emf") | Out-Null
Set-Content -LiteralPath (Join-Path $Game "emf\emf_core.dll") "someone's"
$code = Invoke-Fake $pc "start"
Check ($code -eq 3 -and $pc.installs -eq 0 -and $pc.launches -eq 0) "start refuses an EMF it did not install"
$code = Invoke-Fake $pc "stop"
Check ($pc.uninstalls -eq 0 -and (Test-Path (Join-Path $Game "emf\emf_core.dll")) -and (Text $pc) -match "not game_session's install") "stop leaves an EMF it did not install"
Done $pc

# save_guard: a change stops start with the user's step
$pc = New-Case
$pc.saveGuard = @{ code = 1; text = "CHANGED: character0.ksd" }
$code = Invoke-Fake $pc "start"
Check ($code -eq 1 -and $pc.installs -eq 0 -and (Text $pc) -match "user's step" -and -not (Test-Path $LockFile)) "a save change stops start; recording again is the user's step"
Done $pc

# stop is careful
$pc = New-Case
[void](Invoke-Fake $pc "start")
[void](Invoke-Fake $pc "start" @{ Character = "local" })
$pc.slot = 257; $pc.quitExits = $false
$code = Invoke-Fake $pc "stop"
Check ($code -eq 1 -and $pc.killed.Count -eq 0 -and $pc.uninstalls -eq 0 -and (Test-Path $LockFile) -and (Text $pc) -match "LEFT RUNNING") "a Cloud world that does not close is never ended by force; EMF and the lock are kept"
$pc.stateFails = $true; $pc.slot = 1
$code = Invoke-Fake $pc "stop"
Check ($code -eq 1 -and $pc.killed.Count -eq 0) "an unknown state counts as a world that may run (not ended)"
$pc.stateFails = $false
$t0 = $pc.now
$pc.logOnQuit = "[x] world saved (slot 0x0001)`n"
$code = Invoke-Fake $pc "stop"
Check ($code -eq 0 -and $pc.killed -contains 4242 -and ($pc.now - $t0) -ge 75) "the LOCAL slot 1 is ended only after a save line and 15 s more"
Done $pc
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.quitExits = $false  # at the main menu: nothing to save
$code = Invoke-Fake $pc "stop"
Check ($code -eq 0 -and $pc.killed -contains 4242) "stop: a game at the main menu that ignores the quit is ended"
Done $pc

# the refusal for a bridge that does not answer
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.bridge = $false
$code = Invoke-Fake $pc "stop"
Check ($code -eq 3 -and $pc.closed -eq 0 -and -not $pc.quitAsked) "stop refuses a game whose bridge does not answer"
$code = Invoke-Fake $pc "stop" @{ AllowUnansweredBridge = $true }
Check ($code -eq 0 -and $pc.closed -eq 1) "-AllowUnansweredBridge closes its window instead"
Done $pc

# a failed uninstall keeps the lock
$pc = New-Case
[void](Invoke-Fake $pc "start")
$pc.uninstallThrows = $true
$code = Invoke-Fake $pc "stop"
Check ($code -eq 1 -and (Test-Path $LockFile) -and (Text $pc) -match "kept the game lock") "stop keeps the lock when the uninstall failed, and says so"
Done $pc

# an early end (interrupted, an unexpected error) says what to run and cleans up when the game does not run
$pc = New-Case
$pc.failSleepAt = $pc.now  # the first wait for the game throws
$code = Invoke-Fake $pc "start"
Check ($code -eq 1 -and (Text $pc) -match "ended early" -and (Text $pc) -match "game_session.ps1 stop -LockOwner tester") "an early end says what to run"
Done $pc

# a launch after which the game never comes: what start installed is removed again, and the lock released
$pc = New-Case
$pc.launchFails = $true
$code = Invoke-Fake $pc "start" @{ Mods = @("template_lua") }
Check ($code -eq 1 -and $pc.uninstalls -eq 1 -and -not (Test-Path (Join-Path $Game "emf")) -and (Get-WindowMode $Settings) -eq "Fullscreen" -and -not (Test-Path $LockFile)) "a start that fails before the game runs removes its install, sets Fullscreen and releases the lock"
Done $pc

$pc = New-Case
$pc.launchFails = $true
$pc.uninstallThrows = $true
$code = Invoke-Fake $pc "start"
Check ($code -eq 1 -and (Test-Path $LockFile) -and (Text $pc) -match "kept the game lock") "a failed clean-up after a failed start keeps the lock and says so"
Done $pc

# no -Game: EMF_GAME_DIR, else the game a Steam library has (scripts\find_game.ps1; a fake Steam here, never the real
# one); with neither, start says to pass -Game and touches nothing
$pc = New-Case
$savedEnv = @{}
foreach ($name in "EMF_TEST_NO_STEAM", "EMF_TEST_STEAM_ROOT", "EMF_GAME_DIR") {
    $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}
try {
    $env:EMF_TEST_NO_STEAM = "1"
    Remove-Item Env:\EMF_GAME_DIR -ErrorAction SilentlyContinue
    $steamDir = Join-Path $pc.dir "steam"
    $libDir = Join-Path $pc.dir "library"
    $foundGame = Join-Path $libDir "steamapps\common\Enshrouded"
    New-Item -ItemType Directory -Force (Join-Path $steamDir "steamapps"), $foundGame | Out-Null
    Set-Content -LiteralPath (Join-Path $steamDir "steamapps\libraryfolders.vdf") ("`"libraryfolders`"`n{`n`t`"1`"`n`t{`n`t`t`"path`"`t`t`"" + $libDir.Replace("\", "\\") + "`"`n`t}`n}")
    Set-Content -LiteralPath (Join-Path $libDir "steamapps\appmanifest_1203620.acf") "`"AppState`"`n{`n`t`"installdir`"`t`t`"Enshrouded`"`n}"
    Set-Content -LiteralPath (Join-Path $foundGame "enshrouded.exe") "fake"
    $env:EMF_TEST_STEAM_ROOT = $steamDir
    $script:Game = ""
    $code = Invoke-Fake $pc "status"
    Check ($code -eq 0 -and $Game -eq $foundGame) "no -Game: the game folder comes from Steam's library folders ($Game)"
    $script:Game = ""
    $env:EMF_GAME_DIR = $pc.gameDir
    $code = Invoke-Fake $pc "status"
    Check ($code -eq 0 -and $Game -eq $pc.gameDir) "no -Game: EMF_GAME_DIR wins over Steam ($Game)"
    $script:Game = ""
    Remove-Item Env:\EMF_GAME_DIR, Env:\EMF_TEST_STEAM_ROOT -ErrorAction SilentlyContinue
    $code = Invoke-Fake $pc "start"
    Check ($code -eq 4 -and (Text $pc) -match "pass -Game <folder>" -and $pc.installs -eq 0 -and $pc.launches -eq 0 -and -not (Test-Path $LockFile)) "no game folder found: start says to pass -Game (exit 4) and installs, launches and locks nothing ($(Text $pc))"
} finally {
    foreach ($name in @($savedEnv.Keys)) { [Environment]::SetEnvironmentVariable($name, $savedEnv[$name], "Process") }
}
Done $pc

# usage errors
$pc = New-Case
Check ((Invoke-Fake $pc "dance") -eq 4) "an unknown command exits 4"
Check ((Invoke-Fake $pc "start" @{ LockOwner = "" }) -eq 4) "start without -LockOwner exits 4"
Done $pc

if ($script:fails) { "$($script:fails) check(s) failed"; exit 1 }
"game_session: all checks passed"
exit 0
