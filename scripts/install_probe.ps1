# Copies the probe (built as dinput8.dll) into the game folder.
# Refuses to overwrite a dinput8.dll that is not ours (e.g. EML or another mod). Without -Game: EMF_GAME_DIR, else the
# Steam library that has the game (scripts\find_game.ps1).
param(
    [string]$Game = "",
    [string]$Dll = (Join-Path $PSScriptRoot "..\build\msvc\probe\dinput8.dll"),
    [string]$Marker = "emf_probe"
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "find_game.ps1")
$Game = Resolve-GameDir $Game
function Test-OurDll([string]$path, [string]$marker) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    return $text.Contains($marker)
}
if (-not (Test-Path $Dll)) { throw "build output not found: $Dll" }
$dst = Join-Path $Game "dinput8.dll"
if ((Test-Path $dst) -and -not (Test-OurDll $dst $Marker)) {
    throw "$dst exists and is not ours; not overwriting"
}
if (Get-Process enshrouded -ErrorAction SilentlyContinue) { throw "the game is running; close it first" }
Copy-Item $Dll $dst -Force
"installed $Dll -> $dst"
