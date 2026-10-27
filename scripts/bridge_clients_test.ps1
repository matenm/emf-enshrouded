# Checks both debug bridge clients (scripts/emf_bridge.ps1 and tools/bridge/emf_bridge.py) against emf_bridge_fake.exe,
# which serves the bridge's protocol with made-up game data (no game needed). Each command of each client is run and its
# output and exit code checked: hello, console (output, a late line, a command that did not run, --json), state, mods,
# wait world/player/log, log, log follow, events, shot (scaled down), key, a refused key, load_world (a start and its
# refusals), raw, quit, and the refusals:
# a pipe that does not exist, a bridge.txt naming another machine's pipe, a malformed or mismatched pid, and a pipe its
# named game does not serve; with several game processes (copies of the fake named enshrouded.exe and
# enshrouded_server.exe), only those that serve a bridge count. The Python half is skipped when python is not on the
# path. Every folder it makes in %TEMP% is removed at the end, also when a step throws. Exit code 0 = passed.
#   bridge_clients_test.ps1 [-Build build\msvc]
param([string]$Build = (Join-Path $PSScriptRoot "..\build\msvc"))
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$fake = Join-Path $Build "tools\emf_bridge_fake.exe"
if (-not (Test-Path -LiteralPath $fake)) { throw "missing $fake (build first)" }
$ps = Join-Path $repo "scripts\emf_bridge.ps1"
$py = Join-Path $repo "tools\bridge\emf_bridge.py"
$env:EMF_BRIDGE_TEST_PROCESS = "emf_bridge_fake"  # the clients accept the fake as the game (tests only)
$script:fails = 0
$script:made = New-Object System.Collections.ArrayList  # the folders this run made in %TEMP%
function Check([bool]$cond, [string]$what) {
    if ($cond) { "ok   $what" } else { "FAIL $what"; $script:fails++ }
}
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { "skip the Python client (no python on the path)" }

# (stderr lines from a native program are errors in Windows PowerShell 5.1: not fatal here)
function Run-Ps([string[]]$cmdArgs) {
    $ErrorActionPreference = "Continue"
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $ps @cmdArgs 2>&1 | Out-String
    return @{ out = ($out -replace '\s+', ' '); code = $LASTEXITCODE }  # (stderr lines come wrapped)
}
function Run-Py([string[]]$cmdArgs) {
    $ErrorActionPreference = "Continue"
    $out = & python -I $py @cmdArgs 2>&1 | Out-String
    return @{ out = ($out -replace '\s+', ' '); code = $LASTEXITCODE }  # (stderr lines come wrapped)
}
# Starts a client that streams (log follow, events), stops it after $seconds and returns what it printed.
function Run-Stream([string]$exe, [string[]]$cmdArgs, [int]$seconds, [string]$folder) {
    $outFile = Join-Path $folder ("stream_" + [guid]::NewGuid().ToString("N").Substring(0, 6) + ".txt")
    $p = Start-Process -FilePath $exe -ArgumentList $cmdArgs -RedirectStandardOutput $outFile -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds $seconds
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Start-Sleep -Milliseconds 200
    return (Get-Content -Raw -LiteralPath $outFile -ErrorAction SilentlyContinue)
}
function New-Folder([string]$tag) {
    $f = Join-Path $env:TEMP ("emf_bridge_" + $tag + "_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
    New-Item -ItemType Directory -Force (Join-Path $f "emf") | Out-Null
    [void]$script:made.Add($f)
    return $f
}
function Start-Fake([string]$folder, [string[]]$extra = @()) {
    $p = Start-Process -FilePath $fake -ArgumentList (@("`"$folder`"", "120") + $extra) -PassThru -WindowStyle Hidden
    for ($i = 0; $i -lt 50 -and -not (Test-Path (Join-Path $folder "emf\bridge.txt")); $i++) { Start-Sleep -Milliseconds 100 }
    return $p
}
function Stop-Fake($p, [string]$folder) {
    New-Item -ItemType File -Force (Join-Path $folder "emf\bridge.stop") | Out-Null
    if (-not $p.WaitForExit(5000)) { Stop-Process -Id $p.Id -Force }
}

try {
    $folder = New-Folder "clients"
    $proc = Start-Fake $folder
    try {
        Check (Test-Path (Join-Path $folder "emf\bridge.txt")) "the fake game wrote emf\bridge.txt"
        $clients = @(@{ name = "ps"; run = { param($a) Run-Ps ($a + @("-Game", $folder)) } })
        if ($python) { $clients += @{ name = "py"; run = { param($a) Run-Py ($a + @("--game", $folder)) } } }
        foreach ($c in $clients) {
            $n = $c.name
            $run = $c.run
            $r = & $run @("hello")
            Check ($r.code -eq 0 -and $r.out -match "EMF fake \(API 16\), game build 1273043, client, pid \d+, bridge protocol 1") "$n hello"
            $r = & $run @("console", "near 10")
            Check ($r.code -eq 0 -and $r.out -match "3 entities within 10 m" -and $r.out -notmatch "> near 10") "$n console near 10 (output without the echo)"
            $r = & $run @("console", "put Material_T1_Wood_Logs 1")
            Check ($r.code -eq 0 -and $r.out -match "put: queued" -and $r.out -match "1 added") "$n console waits for put's late line"
            $r = & $run @("console", "nosuch")
            Check ($r.code -eq 1 -and $r.out -match "unknown command 'nosuch'" -and $r.out -match "did not run: unknown") "$n console: a command that did not run (exit 1)"
            $json = if ($n -eq "ps") { "-Json" } else { "--json" }
            $r = & $run @("console", "pos", $json)
            Check ($r.code -eq 0 -and $r.out -match '"status":\s*"ran"' -and $r.out -match '"output":' -and $r.out -match '"log_seq":') "$n console with json prints the reply"
            $r = & $run @("wait", "world", "10")
            Check ($r.code -eq 0 -and $r.out -match "a world is running") "$n wait world"
            $r = & $run @("wait", "player", "10")
            Check ($r.code -eq 0 -and $r.out -match "the local player exists") "$n wait player"
            $r = & $run @("state")
            Check ($r.code -eq 0 -and $r.out -match '"status":\s*"running"' -and $r.out -match '"x":\s*100.5') "$n state (running, the player's position)"
            Check ($r.out -match '"character":\s*\{\s*"storage":\s*"local",\s*"slot":\s*1,\s*"name":\s*"Character 2"') "$n state has the character (LOCAL slot 1, Character 2)"
            Check ($r.out -match '"value":\s*180' -and $r.out -match '"slot":\s*1') "$n state has health and the world's slot"
            $r = & $run @("mods")
            Check ($r.code -eq 0 -and $r.out -match "quick_stack" -and $r.out -match "disabled") "$n mods"
            $r = & $run @("wait", "log", "[life_skills]", "10")
            Check ($r.code -eq 0 -and $r.out -match "log line \d+: \[life_skills\] fake level up") "$n wait log"
            $r = & $run @("wait", "log", "never printed", "1")
            Check ($r.code -eq 1 -and $r.out -match "timeout: timed out") "$n wait log times out (exit 1, code timeout)"
            $mw = if ($n -eq "ps") { "-MaxWidth" } else { "--max-width" }
            $r = & $run @("shot", $mw, "32")
            $shot = if ($r.out -match "screenshot: (.+\.png) \(32x18") { $Matches[1].Trim() } else { "" }
            Check ($r.code -eq 0 -and $shot -and (Test-Path -LiteralPath $shot) -and $shot.StartsWith((Join-Path $folder "emf\screens"))) "$n shot (a PNG in emf\screens, scaled down)"
            $r = & $run @("key", "L")
            Check ($r.code -eq 0 -and $r.out -match "pressed L for mods; they read it 9 times") "$n key L"
            $r = & $run @("key", "Banana")
            Check ($r.code -eq 1 -and $r.out -match "bad_request: unknown key 'Banana'") "$n key Banana is refused (exit 1)"
            # load_world: the fake answers 300 ms later, as if the world had loaded. With a reply margin of 100 ms the
            # client must still wait: its deadline is the game's own time for the start (180 s and a 10 s backstop).
            $env:EMF_BRIDGE_REPLY_MARGIN_MS = "100"
            try { $r = & $run @("load_world", "1", "1", "Character 2") } finally { Remove-Item Env:\EMF_BRIDGE_REPLY_MARGIN_MS -ErrorAction SilentlyContinue }
            Check ($r.code -eq 0 -and $r.out -match "'My World 1' runs with 'Character 2' \(LOCAL world slot 1, character 1, \d+ ms\)") "$n load_world 1 1 'Character 2' (the reply time fits a world start)"
            $r = & $run @("load_world", "9", "1")
            Check ($r.code -eq 1 -and $r.out -match "LOCAL world slot 9 does not exist") "$n load_world: a world slot that does not exist is refused (exit 1)"
            $r = & $run @("load_world", "1", "1", "Someone")
            Check ($r.code -eq 1 -and $r.out -match "not 'Someone'") "$n load_world: another character name is refused (exit 1)"
            if ($n -eq "py") { Check ($r.out -match "nothing started") "py load_world says in plain words that nothing started" }
            $r = & $run @("load_world", "1", "1", "Character 2", $json)
            Check ($r.code -eq 0 -and $r.out -match '"world":\s*"My World 1"' -and $r.out -match '"character_name":\s*"Character 2"') "$n load_world with json prints the reply"
        }

        $r = Run-Ps @("raw", '{\"cmd\":\"fly\"}', "-Game", $folder)  # (powershell.exe -File drops unescaped inner quotes)
        Check ($r.code -eq 1 -and $r.out -match '"code":"unknown_command"') "ps raw with an unknown command (exit 1, code)"
        $r = Run-Ps @("log", "-Max", "2", "-Game", $folder)
        Check ($r.code -eq 0 -and $r.out -match "\[fake\] tick") "ps log"
        $out = Run-Stream "powershell" @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$ps`"", "log", "-Follow", "-Max", "1", "-Game", "`"$folder`"") 3 $folder
        Check (([regex]::Matches($out, "\[fake\] tick")).Count -ge 3) "ps log -Follow streams new lines"
        $out = Run-Stream "powershell" @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$ps`"", "events", "pickup", "-Game", "`"$folder`"") 3 $folder
        Check ($out -match '"name":"pickup"' -and $out -match "Material_T1_Wood_Logs") "ps events pickup"
        if ($python) {
            $out = Run-Stream "python" @("-I", "`"$py`"", "log", "--follow", "--game", "`"$folder`"") 3 $folder
            Check (([regex]::Matches($out, "\[fake\] tick")).Count -ge 3) "py log --follow streams new lines"
            $out = Run-Stream "python" @("-I", "`"$py`"", "events", "pickup", "--game", "`"$folder`"") 3 $folder
            Check ($out -match '"name": "pickup"') "py events pickup"
        }
        $r = Run-Ps @("hello", "-GamePid", "1")
        Check ($r.code -eq 2 -and $r.out -match "no game runs with pid 1") "ps: a pid that is not a game (exit 2)"
        $r = Run-Ps @("quit", "-Game", $folder)
        Check ($r.code -eq 0 -and $r.out -match "the game saves and closes") "ps quit"
        Check ($proc.WaitForExit(5000)) "the fake game ended after quit"
    } finally {
        Stop-Fake $proc $folder
    }
    Check (-not (Test-Path (Join-Path $folder "emf\bridge.txt"))) "the fake game removed emf\bridge.txt when it ended"
    Remove-Item -Recurse -Force $folder -ErrorAction SilentlyContinue

    # bridge.txt that is not to be trusted: another machine's pipe (an SMB connection would leak this user's NTLM hash), a
    # malformed pid, a pid that does not match the pipe. Nothing is connected to.
    $bad = New-Folder "badfile"
    $cases = @(
        @{ text = "pipe=\\attacker\pipe\emf-bridge-$PID`r`npid=$PID`r`n"; want = "names no local EMF bridge pipe"; what = "another machine's pipe" },
        @{ text = "pipe=\\.\pipe\emf-bridge-$PID`r`npid=12ab`r`n"; want = "no valid pid"; what = "a malformed pid" },
        @{ text = "pipe=\\.\pipe\emf-bridge-$PID`r`npid=4`r`n"; want = "inconsistent"; what = "a pid that is not the pipe's" }
    )
    foreach ($case in $cases) {
        [System.IO.File]::WriteAllText((Join-Path $bad "emf\bridge.txt"), $case.text)
        $r = Run-Ps @("hello", "-Game", $bad)
        Check ($r.code -eq 2 -and $r.out -match [regex]::Escape($case.want)) "ps refuses a bridge.txt with $($case.what)"
        if ($python) {
            $r = Run-Py @("hello", "--game", $bad)
            Check ($r.code -eq 2 -and $r.out -match [regex]::Escape($case.want)) "py refuses a bridge.txt with $($case.what)"
        }
    }
    Remove-Item -Recurse -Force $bad -ErrorAction SilentlyContinue

    # A pipe named after a running "game" (this PowerShell, allowed for the test) but served by another process: refused.
    $env:EMF_BRIDGE_TEST_PROCESS = "powershell"
    $squat = New-Folder "squat"
    $proc = Start-Fake $squat @("$PID")
    try {
        $r = Run-Ps @("hello", "-GamePid", "$PID")
        Check ($r.code -eq 2 -and $r.out -match "not by the game") "ps refuses a pipe its game does not serve"
        if ($python) {
            $r = Run-Py @("hello", "--pid", "$PID")
            Check ($r.code -eq 2 -and $r.out -match "not by the game") "py refuses a pipe its game does not serve"
        }
    } finally {
        Stop-Fake $proc $squat
        Remove-Item -Recurse -Force $squat -ErrorAction SilentlyContinue
    }

    # Several game processes: only those that serve their bridge count. Copies of the fake run as enshrouded.exe (with
    # the bridge, bridge.txt in its folder) and as enshrouded_server.exe stand-ins without one (their pipe is named after
    # another pid, their bridge.txt goes to another folder). The clients get neither a folder nor a pid. Skipped while a
    # real game or server runs (the clients would see it too).
    $env:EMF_BRIDGE_TEST_PROCESS = "emf_bridge_fake"
    if (Get-Process enshrouded, enshrouded_server -ErrorAction SilentlyContinue) {
        "skip the several-games check (a game or a dedicated server is running)"
    } else {
        $multi = New-Object System.Collections.ArrayList
        function Start-Copy([string]$exeName, [string]$tag, [string[]]$extra = @(), [switch]$OwnFolder) {
            $dir = New-Folder $tag
            $data = if ($OwnFolder) { $dir } else { New-Folder "$tag-data" }
            $exe = Join-Path $dir $exeName
            Copy-Item -LiteralPath $fake $exe
            $p = Start-Process -FilePath $exe -ArgumentList (@("`"$data`"", "60") + $extra) -PassThru -WindowStyle Hidden
            for ($i = 0; $i -lt 50 -and -not (Test-Path (Join-Path $data "emf\bridge.txt")); $i++) { Start-Sleep -Milliseconds 100 }
            $c = @{ proc = $p; dir = $dir; data = $data }
            [void]$multi.Add($c)
            return $c
        }
        function Stop-Copy($c) {
            New-Item -ItemType File -Force (Join-Path $c.data "emf\bridge.stop") | Out-Null
            if (-not $c.proc.WaitForExit(5000)) { Stop-Process -Id $c.proc.Id -Force }
        }
        try {
            $standIn = Start-Copy "enshrouded_server.exe" "standin" @("999999")
            $game1 = Start-Copy "enshrouded.exe" "game1" -OwnFolder
            $r = Run-Ps @("hello")
            Check ($r.code -eq 0 -and $r.out -match "EMF fake.*pid $($game1.proc.Id)") "ps picks the game that serves its bridge while a stand-in enshrouded_server.exe runs"
            if ($python) {
                $r = Run-Py @("hello")
                Check ($r.code -eq 0 -and $r.out -match "EMF fake.*pid $($game1.proc.Id)") "py picks the game that serves its bridge while a stand-in enshrouded_server.exe runs"
            }
            $game2 = Start-Copy "enshrouded.exe" "game2" -OwnFolder
            $r = Run-Ps @("hello")
            Check ($r.code -eq 2 -and $r.out -match "more than one game with the debug bridge") "ps asks for -GamePid when two games serve a bridge"
            if ($python) {
                $r = Run-Py @("hello")
                Check ($r.code -eq 2 -and $r.out -match "more than one game with the debug bridge") "py asks for --pid when two games serve a bridge"
            }
            Stop-Copy $game1
            Stop-Copy $game2
            $standIn2 = Start-Copy "enshrouded_server.exe" "standin2" @("999995")
            $r = Run-Ps @("hello")
            # (another test may start a stand-in of its own meanwhile: the count is not checked)
            Check ($r.code -eq 2 -and $r.out -match "\d+ game processes run, none with the debug bridge on") "ps: several game processes, none with the bridge"
            if ($python) {
                $r = Run-Py @("hello")
                Check ($r.code -eq 2 -and $r.out -match "\d+ game processes run, none with the debug bridge on") "py: several game processes, none with the bridge"
            }
        } finally {
            foreach ($c in $multi) {
                if (-not $c.proc.HasExited) { Stop-Copy $c }
                Remove-Item -Recurse -Force $c.dir, $c.data -ErrorAction SilentlyContinue
            }
        }
    }

    # A game that is stuck (its pipe takes requests and never answers): both clients give up after the command's own
    # time plus a margin (1.5 s here instead of 15 s) with exit 2, instead of hanging.
    if ($python) {
        $stuckDir = New-Folder "stuck"
        $stub = Start-Process -FilePath "python" -ArgumentList @("-I", "`"$(Join-Path $repo 'tests\tools\stuck_bridge.py')`"", "`"$stuckDir`"", "60") -PassThru -WindowStyle Hidden
        try {
            $info = Join-Path $stuckDir "emf\bridge.txt"
            for ($i = 0; $i -lt 100 -and -not (Test-Path $info); $i++) { Start-Sleep -Milliseconds 100 }
            $stubPid = [int]((@(Get-Content $info) -match '^pid=' | Select-Object -First 1) -replace 'pid=', '')
            $env:EMF_BRIDGE_TEST_PROCESS = (Get-Process -Id $stubPid).ProcessName
            $env:EMF_BRIDGE_REPLY_MARGIN_MS = "1500"
            $t0 = Get-Date
            $r = Run-Ps @("hello", "-Game", $stuckDir)
            $took = ((Get-Date) - $t0).TotalSeconds
            Check ($r.code -eq 2 -and $r.out -match "no reply" -and $took -lt 20) ("ps gives up on a game that never answers (exit 2 after {0:N1} s)" -f $took)
            $t0 = Get-Date
            $r = Run-Py @("hello", "--game", $stuckDir)
            $took = ((Get-Date) - $t0).TotalSeconds
            Check ($r.code -eq 2 -and $r.out -match "no reply" -and $took -lt 20) ("py gives up on a game that never answers (exit 2 after {0:N1} s)" -f $took)
        } catch {
            Check $false "the stuck-game check could not run: $($_.Exception.Message)"
        } finally {
            Remove-Item Env:\EMF_BRIDGE_REPLY_MARGIN_MS -ErrorAction SilentlyContinue
            New-Item -ItemType File -Force (Join-Path $stuckDir "emf\bridge.stop") | Out-Null
            Start-Sleep -Milliseconds 500
            if ($stubPid) { Get-Process -Id $stubPid -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }
            if (-not $stub.HasExited) { Stop-Process -Id $stub.Id -Force -ErrorAction SilentlyContinue }
            Remove-Item -Recurse -Force $stuckDir -ErrorAction SilentlyContinue
        }
    }
} finally {
    foreach ($d in $script:made) {
        # (a fake game that was just ended may hold its files for a moment)
        for ($i = 0; $i -lt 10 -and (Test-Path -LiteralPath $d); $i++) {
            Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
            if (Test-Path -LiteralPath $d) { Start-Sleep -Milliseconds 300 }
        }
    }
}
if ($script:fails) { "$($script:fails) check(s) failed"; exit 1 }
"bridge clients: all checks passed"
exit 0
