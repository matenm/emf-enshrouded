# Tests install_emf.ps1's install stamp and uninstall_probe.ps1's archiving against fake game folders in %TEMP% (never the
# real game): EMF's logs and the game's crash dumps newer than the install go to the archive folder and leave the game
# folder; older dumps stay; without a stamp the emf folder's creation time counts; with nothing installed no dump is
# touched. Then how the developer scripts find the game without -Game (scripts\find_game.ps1): EMF_GAME_DIR, else a
# fake Steam's library folders, never the real Steam; -Game wins over both; with neither they say to pass -Game.
# Every folder it makes is removed at the end, also when a step fails. Exit code 0 = all passed.
$ErrorActionPreference = "Stop"
$script:fails = 0
$script:made = New-Object System.Collections.ArrayList  # the folders this run made, removed at the end
function Check([bool]$cond, [string]$what) {
    if ($cond) { "ok   $what" } else { "FAIL $what"; $script:fails++ }
}
function New-TempDir([string]$name) {
    $d = Join-Path ([System.IO.Path]::GetTempPath()) ("emf test " + $name + " " + [guid]::NewGuid().ToString("N").Substring(0, 8))
    New-Item -ItemType Directory -Force $d | Out-Null
    [void]$script:made.Add($d)
    return $d
}
function Write-File([string]$path, [string]$text) {
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    [System.IO.File]::WriteAllText($path, $text)
}
# A crash dump folder as the game writes it (<game>\crash_dumps\enshrouded_<date>_<time>\), created at $when (UTC).
function New-CrashDump([string]$game, [string]$name, [datetime]$when) {
    $d = Join-Path $game "crash_dumps\$name"
    foreach ($f in "enshrouded.dmp", "enshrouded.info", "enshrouded.log") { Write-File (Join-Path $d $f) "$name $f" }
    (Get-Item -LiteralPath $d).CreationTimeUtc = $when
    return $d
}
# A fake Steam folder whose libraryfolders.vdf names a second library that has the game (as Steam writes them).
function New-FakeSteam([string]$tag) {
    $steam = New-TempDir "steam $tag"
    $lib = New-TempDir "lib $tag"
    Write-File (Join-Path $steam "steamapps\libraryfolders.vdf") ("`"libraryfolders`"`n{`n`t`"1`"`n`t{`n`t`t`"path`"`t`t`"" +
        $lib.Replace("\", "\\") + "`"`n`t}`n}`n")
    Write-File (Join-Path $lib "steamapps\appmanifest_1203620.acf") "`"AppState`"`n{`n`t`"installdir`"`t`t`"Enshrouded`"`n}`n"
    $game = Join-Path $lib "steamapps\common\Enshrouded"
    Write-File (Join-Path $game "enshrouded.exe") "fake"
    return @{ Root = $steam; Game = $game }
}
# find_game.ps1 as verify.sh runs it: what it prints, its error output and its exit code
function Invoke-FindGame {
    $ErrorActionPreference = "Continue"
    $lines = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $findGame 2>&1)
    $code = $LASTEXITCODE
    # (Windows PowerShell wraps a native program's stderr lines as error records)
    $err = @($lines | Where-Object { $_ -is [System.Management.Automation.ErrorRecord] } | ForEach-Object { "$_" }) -join "`n"
    $out = @($lines | Where-Object { $_ -isnot [System.Management.Automation.ErrorRecord] } | ForEach-Object { "$_" }) -join "`n"
    return @{ out = $out.Trim(); err = $err; code = $code }
}
$uninstall = Join-Path $PSScriptRoot "uninstall_probe.ps1"
$install = Join-Path $PSScriptRoot "install_emf.ps1"
$installProbe = Join-Path $PSScriptRoot "install_probe.ps1"
$findGame = Join-Path $PSScriptRoot "find_game.ps1"
$noGame = "emf-test-no-such-process"  # the scripts' "is the game running" check looks for this name instead
# the scripts under test never find the real game: no real Steam, and the developer's own EMF_GAME_DIR is put aside
$savedEnv = @{}
foreach ($name in "EMF_TEST_NO_STEAM", "EMF_TEST_STEAM_ROOT", "EMF_GAME_DIR") {
    $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}
$env:EMF_TEST_NO_STEAM = "1"
Remove-Item Env:\EMF_TEST_STEAM_ROOT, Env:\EMF_GAME_DIR -ErrorAction SilentlyContinue

try {
    # ---- install_emf.ps1 writes the stamp; uninstall_probe.ps1 archives logs and only the newer crash dumps ----
    $dist = New-TempDir "dist"
    Write-File (Join-Path $dist "dinput8.dll") "fake loader EMF-LOADER-7d3c9f"
    Write-File (Join-Path $dist "emf\emf_core.dll") "fake core"
    Write-File (Join-Path $dist "emf\known_tools.txt") ""
    Write-File (Join-Path $dist "emf\vk\emf_layer.json") "{}"
    $game = New-TempDir "game"
    $archive = New-TempDir "archive"
    Write-File (Join-Path $game "enshrouded.exe") "fake"
    $old = New-CrashDump $game "enshrouded_20261027_001919" ([datetime]::UtcNow.AddHours(-2))
    $before = [datetime]::UtcNow.AddSeconds(-2)
    & $install -Game $game -Dist $dist -ProcessName $noGame | Out-Null
    $stampFile = Join-Path $game "emf\install_stamp.txt"
    Check (Test-Path -LiteralPath $stampFile) "install_emf.ps1 writes emf\install_stamp.txt"
    $stamp = [datetime]::Parse((Get-Content -LiteralPath $stampFile -Raw).Trim(), [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::RoundtripKind)
    Check ($stamp.Kind -eq [DateTimeKind]::Utc -and $stamp -ge $before -and $stamp -le [datetime]::UtcNow.AddSeconds(2)) "the stamp is the install time in UTC ($stamp)"
    Write-File (Join-Path $game "emf\emf_core.log") "core log"
    $new = New-CrashDump $game "enshrouded_20991231_235959" ([datetime]::UtcNow.AddMinutes(1))
    $out = (& $uninstall -Game $game -Archive $archive -ProcessName $noGame) -join "`n"
    $moved = @(Get-ChildItem -LiteralPath $archive -Directory | Where-Object { $_.Name -like "*-crash-enshrouded_20991231_235959" })
    Check ($moved.Count -eq 1) "the crash dump made after the install is in the archive (<stamp>-crash-<name>)"
    if ($moved.Count -eq 1) {
        $files = @(Get-ChildItem -LiteralPath $moved[0].FullName -File | ForEach-Object { $_.Name } | Sort-Object)
        Check (($files -join ",") -eq "enshrouded.dmp,enshrouded.info,enshrouded.log") "with its dump, info and log ($($files -join ', '))"
        Check ((Get-Content -LiteralPath (Join-Path $moved[0].FullName "enshrouded.dmp") -Raw) -eq "enshrouded_20991231_235959 enshrouded.dmp") "the files' contents are kept"
    }
    Check (-not (Test-Path -LiteralPath $new)) "and it is gone from the game folder"
    Check (Test-Path -LiteralPath $old) "a crash dump older than the install stays in the game folder"
    Check (@(Get-ChildItem -LiteralPath $archive -Directory | Where-Object { $_.Name -like "*001919" }).Count -eq 0) "and is not archived"
    Check ($out -match "archived crash dump enshrouded_20991231_235959") "the script says which dump it archived"
    Check (@(Get-ChildItem -LiteralPath $archive -File -Filter "*-emf_core.log").Count -eq 1) "EMF's log is archived as before"
    Check (-not (Test-Path -LiteralPath (Join-Path $game "emf"))) "the emf folder is removed"
    Check (-not (Test-Path -LiteralPath (Join-Path $game "dinput8.dll"))) "our dinput8.dll is removed"

    # ---- an install without a stamp (an older install_emf.ps1): the emf folder's creation time counts ----
    $game2 = New-TempDir "game nostamp"
    $archive2 = New-TempDir "archive nostamp"
    Write-File (Join-Path $game2 "emf\emf_core.dll") "fake core"
    (Get-Item -LiteralPath (Join-Path $game2 "emf")).CreationTimeUtc = [datetime]::UtcNow.AddHours(-1)
    $older = New-CrashDump $game2 "enshrouded_20200101_000000" ([datetime]::UtcNow.AddHours(-3))
    $newer = New-CrashDump $game2 "enshrouded_20200101_000001" ([datetime]::UtcNow.AddMinutes(-5))
    & $uninstall -Game $game2 -Archive $archive2 -ProcessName $noGame | Out-Null
    Check (-not (Test-Path -LiteralPath $newer)) "without a stamp: a dump newer than the emf folder is archived"
    Check (@(Get-ChildItem -LiteralPath $archive2 -Directory | Where-Object { $_.Name -like "*-crash-enshrouded_20200101_000001" }).Count -eq 1) "into the archive"
    Check (Test-Path -LiteralPath $older) "without a stamp: an older dump stays"

    # ---- nothing installed: no install time, so no dump is touched ----
    $game3 = New-TempDir "game clean"
    $archive3 = New-TempDir "archive clean"
    $any = New-CrashDump $game3 "enshrouded_20990101_000000" ([datetime]::UtcNow)
    & $uninstall -Game $game3 -Archive $archive3 -ProcessName $noGame | Out-Null
    Check (Test-Path -LiteralPath $any) "with EMF not installed no crash dump is moved"
    Check (@(Get-ChildItem -LiteralPath $archive3 -Force).Count -eq 0) "and nothing is archived"

    # ---- without -Game: the game Steam's library folders name (a fake Steam here) ----

    $steam = New-FakeSteam "a"
    $env:EMF_TEST_STEAM_ROOT = $steam.Root
    $r = Invoke-FindGame
    Check ($r.code -eq 0 -and $r.out -eq $steam.Game) "find_game.ps1 prints the game folder it finds in Steam's library folders ($($r.out))"
    $archive4 = New-TempDir "archive found"
    & $install -Dist $dist -ProcessName $noGame | Out-Null
    Check (Test-Path -LiteralPath (Join-Path $steam.Game "dinput8.dll")) "install_emf.ps1 without -Game installs into the game found through Steam"
    & $uninstall -Archive $archive4 -ProcessName $noGame | Out-Null
    Check (-not (Test-Path -LiteralPath (Join-Path $steam.Game "dinput8.dll")) -and -not (Test-Path -LiteralPath (Join-Path $steam.Game "emf"))) "uninstall_probe.ps1 without -Game removes it from there"

    # ---- EMF_GAME_DIR wins over Steam, -Game wins over both ----
    $own = New-TempDir "game own"
    Write-File (Join-Path $own "enshrouded.exe") "fake"
    $env:EMF_GAME_DIR = $own
    $r = Invoke-FindGame
    Check ($r.code -eq 0 -and $r.out -eq $own) "find_game.ps1: EMF_GAME_DIR wins over Steam ($($r.out))"
    & $install -Dist $dist -ProcessName $noGame | Out-Null
    Check ((Test-Path -LiteralPath (Join-Path $own "dinput8.dll")) -and -not (Test-Path -LiteralPath (Join-Path $steam.Game "dinput8.dll"))) "install_emf.ps1: EMF_GAME_DIR wins over Steam"
    $given = New-TempDir "game given"
    Write-File (Join-Path $given "enshrouded.exe") "fake"
    & $install -Game $given -Dist $dist -ProcessName $noGame | Out-Null
    Check (Test-Path -LiteralPath (Join-Path $given "dinput8.dll")) "install_emf.ps1: -Game wins over EMF_GAME_DIR"
    & $uninstall -Game $given -Archive $archive4 -ProcessName $noGame | Out-Null
    Check ((-not (Test-Path -LiteralPath (Join-Path $given "dinput8.dll"))) -and (Test-Path -LiteralPath (Join-Path $own "dinput8.dll"))) "uninstall_probe.ps1: -Game wins over EMF_GAME_DIR"
    & $uninstall -Archive $archive4 -ProcessName $noGame | Out-Null
    Check (-not (Test-Path -LiteralPath (Join-Path $own "dinput8.dll"))) "uninstall_probe.ps1 without -Game uses EMF_GAME_DIR"
    Remove-Item -LiteralPath $archive4 -Recurse -Force
    New-Item -ItemType Directory -Force $archive4 | Out-Null

    # ---- neither: a plain message that says to pass -Game, and nothing is touched ----
    Remove-Item Env:\EMF_GAME_DIR, Env:\EMF_TEST_STEAM_ROOT -ErrorAction SilentlyContinue
    $r = Invoke-FindGame
    Check ($r.code -eq 1 -and $r.out -eq "" -and $r.err -match "pass -Game <folder>") "find_game.ps1 without a game: exit code 1 and says to pass -Game ($($r.err.Trim()))"
    $probeDll = Join-Path $dist "probe.dll"
    Write-File $probeDll "emf_probe"
    $calls = @(
        @{ name = "install_emf.ps1"; run = { & $install -Dist $dist -ProcessName $noGame } },
        @{ name = "install_probe.ps1"; run = { & $installProbe -Dll $probeDll } },
        @{ name = "uninstall_probe.ps1"; run = { & $uninstall -Archive $archive4 -ProcessName $noGame } }
    )
    foreach ($c in $calls) {
        $msg = ""
        try { & $c.run | Out-Null } catch { $msg = $_.Exception.Message }
        Check ($msg -match "not found" -and $msg -match "pass -Game <folder>") "$($c.name) without a game found says to pass -Game ($msg)"
    }
    Check (@(Get-ChildItem -LiteralPath $archive4 -Force).Count -eq 0) "and archives nothing"
} finally {
    foreach ($d in $script:made) { Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue }
    foreach ($name in @($savedEnv.Keys)) { [Environment]::SetEnvironmentVariable($name, $savedEnv[$name], "Process") }
}
if ($script:fails) { "$($script:fails) check(s) failed"; exit 1 }
"uninstall_probe: all checks passed"
exit 0
