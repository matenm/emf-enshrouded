# The real WebSocket client behind "Log in with Nexus" (src/nexus/sso.cpp, WinHTTP) against a local fake of Nexus's
# login service (scripts/sso_fake_server.py, plain ws:// on 127.0.0.1): the hello with protocol 2 and no token, the
# connection token, the browser page with the id and the application, a key sent in two fragments, a dropped
# connection made again with the same id and the token, and the time limit aborting a socket that waits. It never
# contacts Nexus: EMF has no application slug yet, and `emf_nexus sso-selftest` only talks to ws://127.0.0.1. Needs
# Python 3. Exit code 0 = passed.
#   sso_selftest.ps1 [-Build build\msvc]
param([string]$Build = (Join-Path $PSScriptRoot "..\build\msvc"))
$ErrorActionPreference = "Stop"
$cli = Join-Path $Build "emf_nexus.exe"
if (-not (Test-Path -LiteralPath $cli)) { throw "missing $cli (build first)" }
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { throw "Python 3 is needed for the fake login service" }
$server = Join-Path $PSScriptRoot "sso_fake_server.py"
$script:fails = 0
# emf_nexus with its error output as text (PowerShell 5.1 would turn a native program's stderr into an error)
function Invoke-Cli([string[]]$CliArgs) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { $text = & $cli @CliArgs 2>&1 | ForEach-Object { "$_" } | Out-String } finally { $ErrorActionPreference = $old }
    return [pscustomobject]@{ Text = $text; Code = $LASTEXITCODE }
}
function Check([bool]$cond, [string]$what) {
    if ($cond) { "ok   $what" } else { "FAIL $what"; $script:fails++ }
}

foreach ($mode in @("plain", "drop")) {
    $dir = Join-Path ([System.IO.Path]::GetTempPath()) ("emf sso selftest " + [guid]::NewGuid().ToString("N").Substring(0, 8))
    New-Item -ItemType Directory -Force $dir | Out-Null
    try {
        $portFile = Join-Path $dir "port.txt"
        $logFile = Join-Path $dir "log.json"
        # the server runs from this script's folder with -I (no site packages, no current-folder imports)
        $srv = Start-Process -FilePath $python -ArgumentList @("-I", "`"$server`"", "`"$portFile`"", "`"$logFile`"", $mode) -PassThru -WindowStyle Hidden
        $watch = [System.Diagnostics.Stopwatch]::StartNew()
        while (-not (Test-Path -LiteralPath $portFile) -and $watch.Elapsed.TotalSeconds -lt 10) { Start-Sleep -Milliseconds 50 }
        Start-Sleep -Milliseconds 100
        $port = (Get-Content -LiteralPath $portFile -Raw).Trim()
        $r = Invoke-Cli @("sso-selftest", "ws://127.0.0.1:$port/")
        $output = $r.Text
        $code = $r.Code
        if (-not $srv.WaitForExit(15000)) { $srv.Kill() }
        $log = if (Test-Path -LiteralPath $logFile) { @((Get-Content -LiteralPath $logFile -Raw) | ConvertFrom-Json) } else { @() }
        $browser = [regex]::Matches($output, "browser: (\S+)") | ForEach-Object { $_.Groups[1].Value }
        Check ($code -eq 0 -and $output -match "api key: 42 characters, starts ZmFrZS") "$mode`: the key arrives ($($output.Trim() -replace "`r?`n", ' | '))"
        Check (@($browser).Count -eq 1 -and $browser -match "^https://www\.nexusmods\.com/sso\?id=[0-9a-f-]{36}&application=emf-selftest$") "$mode`: the browser opens once, at Nexus's SSO page with the id and the application"
        $id = if (@($browser).Count -ge 1) { ([regex]::Match(@($browser)[0], "id=([0-9a-f-]{36})")).Groups[1].Value } else { "" }
        Check ($log.Count -ge 1 -and $log[0].hello.protocol -eq 2 -and $null -eq $log[0].hello.token -and $log[0].hello.id -eq $id) "$mode`: the hello names the id, no token, protocol 2"
        Check ($log.Count -ge 1 -and $log[0].request -like "GET / HTTP/1.1" -and $log[0].user_agent -like "EMF/*") "$mode`: a WebSocket upgrade with EMF's User-Agent ($(if ($log.Count) { $log[0].user_agent }))"
        if ($mode -eq "drop") {
            Check ($log.Count -eq 2 -and $log[1].hello.token -eq "fakeConnectionToken123" -and $log[1].hello.id -eq $id) "drop: the dropped connection is made again with the same id and the connection token"
        }
    } finally {
        Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
# the time limit: the fake sends the token and then nothing; EMF's watcher must abort the waiting socket
$dir = Join-Path ([System.IO.Path]::GetTempPath()) ("emf sso selftest " + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $dir | Out-Null
try {
    $portFile = Join-Path $dir "port.txt"
    $srv = Start-Process -FilePath $python -ArgumentList @("-I", "`"$server`"", "`"$portFile`"", "`"$(Join-Path $dir 'log.json')`"", "hang") -PassThru -WindowStyle Hidden
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path -LiteralPath $portFile) -and $watch.Elapsed.TotalSeconds -lt 10) { Start-Sleep -Milliseconds 50 }
    Start-Sleep -Milliseconds 100
    $port = (Get-Content -LiteralPath $portFile -Raw).Trim()
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $r = Invoke-Cli @("sso-selftest", "ws://127.0.0.1:$port/", "2")
    $output = $r.Text
    $code = $r.Code
    $took = $watch.Elapsed.TotalSeconds
    if (-not $srv.WaitForExit(15000)) { $srv.Kill() }
    Check ($code -eq 1 -and $output -match "timed out" -and $took -lt 6) "hang: the time limit aborts the waiting socket ($([math]::Round($took, 1)) s: $($output.Trim() -replace "`r?`n", ' | '))"
} finally {
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:fails) { "sso selftest: $($script:fails) failed"; exit 1 }
"sso selftest: all passed"
exit 0
