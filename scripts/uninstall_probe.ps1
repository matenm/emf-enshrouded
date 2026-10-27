# Removes our dinput8.dll (and the emf folder) from the game folder and archives the probe logs, and the game's crash
# dumps written since the install (<game>\crash_dumps\<folder>, moved to the archive as <stamp>-crash-<folder>).
# Without -Game: EMF_GAME_DIR, else the Steam library that has the game (scripts\find_game.ps1).
param(
    [string]$Game = "",
    [string]$Archive = (Join-Path $PSScriptRoot "..\analysis\runs"),
    [string[]]$Markers = @("emf_probe", "EMF-LOADER-7d3c9f"),
    [string]$ProcessName = "enshrouded"  # the game's process (tests pass another name)
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "find_game.ps1")
$Game = Resolve-GameDir $Game
if (Get-Process $ProcessName -ErrorAction SilentlyContinue) { throw "the game is running; close it first" }

# When EMF was installed (UTC): install_emf.ps1's emf\install_stamp.txt, else the emf folder's creation time (an older
# install); $null when EMF is not installed.
function Get-InstallTime([string]$emfDir) {
    if (-not (Test-Path -LiteralPath $emfDir)) { return $null }
    $stampFile = Join-Path $emfDir "install_stamp.txt"
    if (Test-Path -LiteralPath $stampFile) {
        try {
            return [datetime]::Parse((Get-Content -LiteralPath $stampFile -Raw).Trim(),
                [System.Globalization.CultureInfo]::InvariantCulture,
                [System.Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
        } catch { }  # unreadable: the folder's time instead
    }
    return (Get-Item -LiteralPath $emfDir).CreationTimeUtc
}

# The game's crash dumps (a folder each: enshrouded.dmp, .info, .log) written since the install go next to the logs and
# leave the game folder; older ones belong to other runs and stay.
$installed = Get-InstallTime (Join-Path $Game "emf")
$dumps = Join-Path $Game "crash_dumps"
if ($installed -and (Test-Path -LiteralPath $dumps)) {
    Get-ChildItem -LiteralPath $dumps -Directory | Where-Object { $_.CreationTimeUtc -gt $installed } | ForEach-Object {
        New-Item -ItemType Directory -Force $Archive | Out-Null
        $to = Join-Path $Archive ("{0}-crash-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss"), $_.Name)
        Copy-Item -LiteralPath $_.FullName -Destination $to -Recurse
        Remove-Item -LiteralPath $_.FullName -Recurse -Force
        "archived crash dump $($_.Name) to $to"
    }
}
$dst = Join-Path $Game "dinput8.dll"
if (Test-Path $dst) {
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($dst))
    $ours = $false
    foreach ($m in $Markers) { if ($text.Contains($m)) { $ours = $true } }
    if ($ours) { Remove-Item $dst -Force; "removed $dst" } else { "left $dst alone (not ours)" }
}
$emf = Join-Path $Game "emf"
if (Test-Path $emf) {
    New-Item -ItemType Directory -Force $Archive | Out-Null
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    Get-ChildItem $emf -Filter *.log -Recurse -ErrorAction SilentlyContinue | ForEach-Object { Copy-Item $_.FullName (Join-Path $Archive "$stamp-$($_.Name)") }
    if (Test-Path (Join-Path $emf "moved.txt")) {
        # the player installer renamed another mod's dinput8.dll: its uninstaller puts it back
        & (Join-Path $PSScriptRoot "..\package\uninstall.ps1") -Game $Game -All -Force
    }
    if (Test-Path (Join-Path $emf "moved.txt")) {
        Get-ChildItem $emf -Force | Where-Object { $_.Name -ne "moved.txt" } | Remove-Item -Recurse -Force
        "removed $emf except moved.txt (other mods' files are still renamed)"
    } elseif (Test-Path $emf) {
        Remove-Item $emf -Recurse -Force
        "removed $emf"
    }
}
$log = Join-Path $Game "emf_probe.log"
if (Test-Path $log) {
    New-Item -ItemType Directory -Force $Archive | Out-Null
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    Move-Item $log (Join-Path $Archive "$stamp-emf_probe.log")
    "archived $log"
}
