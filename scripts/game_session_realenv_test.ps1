# Checks game_session.ps1's real environment (New-RealEnv) against a running emf_bridge_fake.exe (no game): the bridge
# client as a child process with its exit codes and its time limit, load_world as a child that runs meanwhile, the
# load-event reader, the input time and the screen size. scripts/test_tools.ps1 runs it with the fake's folder (EMF_BRIDGE_TEST_PROCESS=emf_bridge_fake).
#   game_session_realenv_test.ps1 -Folder <the fake game's folder>
param([Parameter(Mandatory = $true)] [string]$Folder)
$ErrorActionPreference = "Stop"
$script:fails = 0
function Check([bool]$cond, [string]$what) {
    if ($cond) { "ok   $what" } else { "FAIL $what"; $script:fails++ }
}
$realFolder = $Folder
. (Join-Path $PSScriptRoot "game_session.ps1")  # its functions
$Game = $realFolder
$script:Env = New-RealEnv
# the client children's output files go to a %TEMP% of this test's own, which must be empty after each step
$ownTemp = Join-Path ([IO.Path]::GetTempPath()) ("emf_gs_realenv_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $ownTemp | Out-Null
$oldTmp = $env:TMP
$oldTemp = $env:TEMP
$env:TMP = $ownTemp
$env:TEMP = $ownTemp
function Get-Left { return @(Get-ChildItem -LiteralPath $ownTemp -Force | ForEach-Object { $_.Name }) }

try {
    $h = & $script:Env.Bridge @("hello") 30
    Check ($h.code -eq 0 -and $h.reply.emf -eq "fake") "real Env: hello through the client child (exit code 0, the reply)"
    $s = & $script:Env.Bridge @("state") 30
    Check ($s.code -eq 0 -and $null -ne $s.reply.world) "real Env: state"
    $w = & $script:Env.Bridge @("wait", "log", "never printed", "1") 30
    Check ($w.code -eq 1 -and (Get-BridgeWhy $w) -match "timeout") "real Env: a wait that times out (exit code 1, why)"
    Check ((Get-Left).Count -eq 0) "real Env: the client child's files are removed ($((Get-Left) -join ', '))"
    $t0 = Get-Date
    $x = & $script:Env.Bridge @("wait", "log", "never printed", "30") 2
    Check ($x.code -eq 2 -and $x.message -match "did not finish" -and ((Get-Date) - $t0).TotalSeconds -lt 10) "real Env: the client child is ended after its time limit"
    Check ((Get-Left).Count -eq 0) "real Env: the files of a client child ended after its time limit are removed too ($((Get-Left) -join ', '))"
    $ev = & $script:Env.EventsStart
    for ($i = 0; $i -lt 40 -and -not (& $script:Env.EventsReady $ev); $i++) { Start-Sleep -Milliseconds 250 }
    Check (& $script:Env.EventsReady $ev) "real Env: the load-event reader says when it listens"
    Check (-not $ev.proc.HasExited -and @(& $script:Env.EventsRead $ev).Count -eq 0) "real Env: the load-event reader runs (the fake sends no load)"
    & $script:Env.EventsStop $ev
    Start-Sleep -Milliseconds 500
    Check ($ev.proc.HasExited -and -not (Test-Path -LiteralPath $ev.out) -and (Get-Left).Count -eq 0) "real Env: the load-event reader is stopped and its files removed ($((Get-Left) -join ', '))"
    # load_world as a client child that runs while start watches the load events (the fake answers after 300 ms)
    $h = & $script:Env.BridgeStart @("load_world", "1", "1", "Character 2", "-TimeoutMs", "20000")
    for ($i = 0; $i -lt 100 -and -not (& $script:Env.BridgeDone $h); $i++) { Start-Sleep -Milliseconds 100 }
    $lw = & $script:Env.BridgeResult $h
    & $script:Env.BridgeStop $h
    Check ($lw.code -eq 0 -and $lw.reply.ok -and $lw.reply.world -eq "My World 1" -and $lw.reply.character_name -eq "Character 2") "real Env: load_world through a client child (exit code 0, the reply)"
    Check ((Get-Left).Count -eq 0) "real Env: the load_world child's files are removed ($((Get-Left) -join ', '))"
    $h = & $script:Env.BridgeStart @("load_world", "9", "1", "Character 2")
    for ($i = 0; $i -lt 100 -and -not (& $script:Env.BridgeDone $h); $i++) { Start-Sleep -Milliseconds 100 }
    $lw = & $script:Env.BridgeResult $h
    & $script:Env.BridgeStop $h
    Check ($lw.code -eq 1 -and $lw.reply.code -eq "refused" -and -not (Test-LoadWorldMissing $lw.reply)) "real Env: a refused load_world (exit code 1, code refused; not a reason for clicks)"
    $h = & $script:Env.BridgeStart @("wait", "log", "never printed", "30")
    Start-Sleep -Milliseconds 300
    $lw = & $script:Env.BridgeResult $h
    & $script:Env.BridgeStop $h
    Start-Sleep -Milliseconds 300
    Check ($lw.code -eq 2 -and $h.proc.HasExited -and (Get-Left).Count -eq 0) "real Env: a client child stopped before it answered is ended and its files removed ($((Get-Left) -join ', '))"
    $now = & $script:Env.Now
    $last = & $script:Env.LastInputEpoch
    Check ($last -gt 0 -and $last -le $now + 1) "real Env: the last input's time"
    $scr = & $script:Env.Screen
    Check ($scr.width -gt 0 -and $scr.height -gt 0) "real Env: the screen size ($($scr.width)x$($scr.height))"
} finally {
    $env:TMP = $oldTmp
    $env:TEMP = $oldTemp
    Remove-Item -LiteralPath $ownTemp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:fails) { "$($script:fails) check(s) failed"; exit 1 }
"game_session real Env: all checks passed"
exit 0
