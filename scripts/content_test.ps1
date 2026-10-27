# Applies an EML content mod (content\<name>) to a game folder for an in-game test, and takes it out again exactly.
#   content_test.ps1 -Apply -Mod content\emberbrand [-Game <folder>]   backs up the files EML changes (enshrouded.kfc,
#                                                     enshrouded.kfc_resources and every small enshrouded_NNN.dat an
#                                                     earlier EML patch wrote), copies the mod into <game>\mods, runs
#                                                     EML's patch step (emm.exe run --patch). Without -Game:
#                                                     EMF_GAME_DIR, else the Steam library that has the game
#                                                     (scripts\find_game.ps1)
#   content_test.ps1 -Restore                         restores THE FOLDER IT BACKED UP (recorded at -Apply), removes the
#                                                     containers EML added and the mod, and checks every backed-up file
#                                                     is byte for byte what it was
# EML rewrites enshrouded.kfc, appends to enshrouded.kfc_resources and (re)writes small enshrouded_NNN.dat files; its
# own "restore" only puts enshrouded.kfc back, so this script keeps its own copies, in -Backup (default
# %LOCALAPPDATA%\EMF\content_test_backup: not under %TEMP%, where a temp cleaner could delete it during a test).
param(
    [switch]$Apply,
    [switch]$Restore,
    [string]$Mod = "",
    [string]$Game = "",
    [string]$Backup = (Join-Path $env:LOCALAPPDATA "EMF\content_test_backup"),
    [string]$Emm = (Join-Path $PSScriptRoot "..\tools\eml-0.2.0\emm.exe")
)
$ErrorActionPreference = "Stop"
if (Get-Process enshrouded -ErrorAction SilentlyContinue) { throw "the game is running; close it first" }
$oldBackup = Join-Path $env:TEMP "emf_content_backup"  # the old default
if (-not $PSBoundParameters.ContainsKey("Backup") -and (Test-Path -LiteralPath (Join-Path $oldBackup "complete.txt"))) {
    throw "an older content_test.ps1 left a backup at ${oldBackup}; restore it first: content_test.ps1 -Restore -Backup `"$oldBackup`""
}
$kSmallContainer = 256MB  # the game's own containers are about 1 GB each; EML's are small

function Get-Containers([string]$dir) {
    @(Get-ChildItem -LiteralPath $dir -File | Where-Object { $_.Name -match '^enshrouded_(\d+)\.dat$' })
}

if ($Apply) {
    if (-not $Mod) { throw "-Mod <content folder> is needed" }
    . (Join-Path $PSScriptRoot "find_game.ps1")
    $Game = (Resolve-Path -LiteralPath (Resolve-GameDir $Game)).Path
    if (-not (Test-Path -LiteralPath (Join-Path $Game "enshrouded.kfc") -PathType Leaf)) { throw "no enshrouded.kfc in $Game" }
    if (-not (Test-Path -LiteralPath $Emm -PathType Leaf)) {
        throw "EML's emm.exe was not found at ${Emm}; download EML 0.2.0 (github.com/Brabb3l/kfc-parser, releases) and pass -Emm <path>"
    }
    if (Test-Path -LiteralPath $Backup) { throw "a backup is already at ${Backup}; run -Restore first (or delete it if it is incomplete)" }
    $id = (Get-Content -LiteralPath (Join-Path $Mod "mod.json") -Raw | ConvertFrom-Json).id
    if (-not $id) { throw "no id in $Mod\mod.json" }
    if (Test-Path -LiteralPath (Join-Path $Game "mods\$id")) { throw "$Game\mods\$id exists already" }

    New-Item -ItemType Directory -Force $Backup | Out-Null
    $keep = @("enshrouded.kfc", "enshrouded.kfc_resources") +
        @(Get-Containers $Game | Where-Object { $_.Length -lt $kSmallContainer } | ForEach-Object { $_.Name })
    foreach ($f in $keep) {
        Copy-Item -LiteralPath (Join-Path $Game $f) -Destination $Backup
        "$f $((Get-FileHash -LiteralPath (Join-Path $Game $f) -Algorithm SHA256).Hash)" |
            Add-Content -LiteralPath (Join-Path $Backup "hashes.txt") -Encoding UTF8
    }
    $highest = (Get-Containers $Game | ForEach-Object { [int]($_.Name -replace '^enshrouded_(\d+)\.dat$', '$1') } |
        Measure-Object -Maximum).Maximum
    if ($null -eq $highest) { $highest = -1 }
    Set-Content -LiteralPath (Join-Path $Backup "game.txt") $Game -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $Backup "highest.txt") $highest -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $Backup "mod.txt") $id -Encoding UTF8
    $madeMods = -not (Test-Path -LiteralPath (Join-Path $Game "mods"))
    Set-Content -LiteralPath (Join-Path $Backup "made_mods.txt") $madeMods -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $Backup "complete.txt") "ok" -Encoding UTF8  # last: the backup is whole
    New-Item -ItemType Directory -Force (Join-Path $Game "mods") | Out-Null
    Copy-Item -LiteralPath $Mod -Destination (Join-Path $Game "mods\$id") -Recurse
    "backed up $($keep.Count) files of $Game to $Backup; patching with $id"
    # emm writes its log to stderr; in Windows PowerShell that must not turn into a terminating error
    $ErrorActionPreference = "Continue"
    $out = & $Emm run --patch -g $Game 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    $out | Select-String -Pattern "Running mod|$id|Applied|ERROR|Error" | ForEach-Object { $_.Line }
    if ($code -ne 0) { throw "emm.exe failed (exit $code): $(($out | Select-Object -Last 1)). Run content_test.ps1 -Restore" }
}

if ($Restore) {
    if (-not (Test-Path -LiteralPath (Join-Path $Backup "complete.txt"))) {
        throw "no complete backup at $Backup (nothing to restore, or -Apply stopped before it finished; delete the folder then)"
    }
    $recorded = (Get-Content -LiteralPath (Join-Path $Backup "game.txt")).Trim()
    if ($Game -and (Resolve-Path -LiteralPath $Game).Path -ne $recorded) {
        throw "this backup is of $recorded, not $Game; run -Restore without -Game (or with that folder)"
    }
    $Game = $recorded
    $hashes = @(Get-Content -LiteralPath (Join-Path $Backup "hashes.txt"))
    foreach ($line in $hashes) {
        $name = ($line -split " ")[0]
        Copy-Item -LiteralPath (Join-Path $Backup $name) -Destination (Join-Path $Game $name) -Force
    }
    $highest = [int](Get-Content -LiteralPath (Join-Path $Backup "highest.txt")).Trim()
    foreach ($c in Get-Containers $Game) {  # only containers numbered above the highest one at -Apply are EML's new ones
        $n = [int]($c.Name -replace '^enshrouded_(\d+)\.dat$', '$1')
        if ($n -gt $highest) { Remove-Item -LiteralPath $c.FullName -Force; "removed $($c.Name)" }
    }
    $id = (Get-Content -LiteralPath (Join-Path $Backup "mod.txt")).Trim()
    if ($id -and (Test-Path -LiteralPath (Join-Path $Game "mods\$id"))) { Remove-Item -LiteralPath (Join-Path $Game "mods\$id") -Recurse -Force; "removed mods\$id" }
    if ((Get-Content -LiteralPath (Join-Path $Backup "made_mods.txt")).Trim() -eq "True") {
        $mods = Join-Path $Game "mods"
        if ((Test-Path -LiteralPath $mods) -and -not (Get-ChildItem -LiteralPath $mods -Force)) { Remove-Item -LiteralPath $mods -Force }
    }
    $ok = $true
    foreach ($line in $hashes) {
        $name, $hash = $line -split " "
        $now = (Get-FileHash -LiteralPath (Join-Path $Game $name) -Algorithm SHA256).Hash
        if ($now -ne $hash) { $ok = $false; "MISMATCH $name" }
    }
    if (-not $ok) { throw "a file differs from its backup; the backup stays at $Backup" }
    "restored $($hashes.Count) files of $Game (sha256 match)"
    Remove-Item -LiteralPath $Backup -Recurse -Force
    "EML's changes are undone"
}
