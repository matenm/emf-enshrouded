# Tests of the tools for agents and modders. Without the game:
# - scripts/game_session.ps1 without the game: scripts/game_session_test.ps1 (its decisions, and whole start / stop
#   runs against a fake PC);
# - the Python tools (tests/tools): the scenario runner, the MCP server and the llms.txt generator against a fake
#   bridge, the Nexus release tool (tools/nexus/nexus_publish.py) against a fake Nexus on 127.0.0.1,
#   docs/modding/llms.txt up to date, and both programs over the real pipe against emf_bridge_fake.exe.
# The Python half is skipped when Python 3 is not on the path. Exit code 0 = passed.
#   test_tools.ps1 [-Build build\msvc]
param([string]$Build = (Join-Path $PSScriptRoot "..\build\msvc"))
$ErrorActionPreference = "Stop"
$toolsRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$script:fails = 0
function Check([bool]$cond, [string]$what) {
    if ($cond) { "ok   $what" } else { "FAIL $what"; $script:fails++ }
}
function New-TestFolder([string]$tag) {
    $f = Join-Path $env:TEMP ("emf_tools_" + $tag + "_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
    New-Item -ItemType Directory -Force $f | Out-Null
    return $f
}

# ---------------------------------------------------------------- game_session.ps1 without the game
# (its decisions and whole start / stop runs against a fake PC: scripts/game_session_test.ps1)
$ErrorActionPreference = "Continue"
$gsOut = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $toolsRepo "scripts\game_session_test.ps1") 2>&1 | ForEach-Object { "$_" }
$gsCode = $LASTEXITCODE
$ErrorActionPreference = "Stop"
$gsOk = @($gsOut | Where-Object { $_ -match '^ok ' }).Count
Check ($gsCode -eq 0) "game_session.ps1 without the game ($gsOk checks)"
if ($gsCode -ne 0) { $gsOut | Where-Object { $_ -notmatch '^ok ' } }

# ---------------------------------------------------------------- the Python tools
$pyOk = $false
if (Get-Command python -ErrorAction SilentlyContinue) {
    $ErrorActionPreference = "Continue"
    $v = & python -c "import sys; print(sys.version_info[0])" 2>$null
    $pyOk = ($LASTEXITCODE -eq 0 -and "$v".Trim() -eq "3")
    $ErrorActionPreference = "Stop"
}
if (-not $pyOk) {
    "skip the Python tools (no Python 3 on the path)"
} else {
    function Run-Python([string[]]$pyArgs) {
        $ErrorActionPreference = "Continue"
        $out = & python -I @pyArgs 2>&1 | Out-String
        return @{ out = $out; code = $LASTEXITCODE }
    }
    Push-Location $toolsRepo
    try {
        $r = Run-Python @("-m", "unittest", "discover", "-v", "-s", "tests\tools")
        $summary = ($r.out -split "`n" | Where-Object { $_ -match "^Ran \d+ tests" }) -join ""
        # counted by name: Out-String wraps long lines, so "... ok" can land on the next line (the exit code says they passed)
        $nexusOk = @($r.out -split "`n" | Where-Object { $_ -match "\(test_nexus_publish\." }).Count
        Check ($r.code -eq 0 -and $nexusOk -gt 0) "python unit tests of the tools ($summary; $nexusOk of them the Nexus release tool's, against a fake Nexus)"
        if ($r.code -ne 0) { $r.out }
        $r = Run-Python @("tools\gen_llms_txt.py", "--check")
        Check ($r.code -eq 0) "docs\modding\llms.txt is up to date (python tools\gen_llms_txt.py makes it)"
        if ($r.code -ne 0) { $r.out }
        $r = Run-Python @("tools\bridge\emf_scenario.py", "--check", "tests\game\smoke.json", "tests\game\hud.json", "tests\game\lua_starter.json")
        Check ($r.code -eq 0) "the scenarios in tests\game are valid"

        # both programs over the real pipe, against the fake game
        $fake = Join-Path $Build "tools\emf_bridge_fake.exe"
        if (-not (Test-Path -LiteralPath $fake)) { throw "missing $fake (build first)" }
        $folder = New-TestFolder "fake"
        New-Item -ItemType Directory -Force (Join-Path $folder "emf\mods\demo") | Out-Null
        [IO.File]::WriteAllText((Join-Path $folder "emf\mods\demo\main.lua"), "emf.log(`"hello`")`n")
        $proc = Start-Process -FilePath $fake -ArgumentList @("`"$folder`"", "120") -PassThru -WindowStyle Hidden
        try {
            for ($i = 0; $i -lt 50 -and -not (Test-Path (Join-Path $folder "emf\bridge.txt")); $i++) { Start-Sleep -Milliseconds 100 }
            $env:EMF_BRIDGE_TEST_PROCESS = "emf_bridge_fake"
            $env:EMF_FAKE_GAME = $folder
            $r = Run-Python @("-m", "unittest", "discover", "-s", "tests\tools", "-p", "test_live*.py")
            $summary = ($r.out -split "`n" | Where-Object { $_ -match "^Ran \d+ tests" }) -join ""
            Check ($r.code -eq 0 -and $r.out -notmatch "skipped") "the scenario runner and the MCP server over the pipe, against emf_bridge_fake ($summary)"
            if ($r.code -ne 0) { $r.out }
            # game_session.ps1's real environment against the same fake
            $ErrorActionPreference = "Continue"
            $reOut = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $toolsRepo "scripts\game_session_realenv_test.ps1") -Folder $folder 2>&1 | ForEach-Object { "$_" }
            $reCode = $LASTEXITCODE
            $ErrorActionPreference = "Stop"
            Check ($reCode -eq 0) "game_session.ps1's real environment against emf_bridge_fake ($(@($reOut | Where-Object { $_ -match '^ok ' }).Count) checks)"
            if ($reCode -ne 0) { $reOut | Where-Object { $_ -notmatch '^ok ' } }
        } finally {
            Remove-Item Env:\EMF_FAKE_GAME -ErrorAction SilentlyContinue
            New-Item -ItemType File -Force (Join-Path $folder "emf\bridge.stop") | Out-Null
            if (-not $proc.WaitForExit(5000)) { Stop-Process -Id $proc.Id -Force }
            Remove-Item -Recurse -Force $folder -ErrorAction SilentlyContinue
        }
    } finally {
        Pop-Location
    }
}
if ($script:fails) { "$($script:fails) check(s) failed"; exit 1 }
"tools: all checks passed"
exit 0
