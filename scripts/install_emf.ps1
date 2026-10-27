# Installs the EMF framework (loader + core + mods from build\msvc\dist) into the game folder.
#   install_emf.ps1 [-Game <folder>] [-EnableWrite] [-Bridge]   without -Game: EMF_GAME_DIR, else the Steam library
#   that has the game (scripts\find_game.ps1). -EnableWrite turns on the sample mod's write tests and the companion mod
#   (F6); -Bridge turns on the debug bridge ([debug] bridge=1 in emf\emf_core.ini, docs/modding/debug_bridge.md), and
#   without it an earlier bridge=1 is turned off. emf\install_stamp.txt records when (UTC): uninstall_probe.ps1 archives
#   the game's crash dumps written since then.
param(
    [string]$Game = "",
    [string]$Dist = (Join-Path $PSScriptRoot "..\build\msvc\dist"),
    [switch]$EnableWrite,
    [switch]$Bridge,
    [string]$ProcessName = "enshrouded"  # the game's process (tests pass another name)
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "find_game.ps1")
$Game = Resolve-GameDir $Game
function Test-Ours([string]$path) {
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($path))
    return $text.Contains("emf_probe") -or $text.Contains("EMF-LOADER-7d3c9f")  # src/loader/loader_id.h
}
if (Get-Process $ProcessName -ErrorAction SilentlyContinue) { throw "the game is running; close it first" }
foreach ($f in @("dinput8.dll", "emf\emf_core.dll")) {
    if (-not (Test-Path (Join-Path $Dist $f))) { throw "missing build output: $Dist\$f" }
}
$dst = Join-Path $Game "dinput8.dll"
if ((Test-Path $dst) -and -not (Test-Ours $dst)) { throw "$dst exists and is not ours; not overwriting" }
Copy-Item (Join-Path $Dist "dinput8.dll") $dst -Force
New-Item -ItemType Directory -Force (Join-Path $Game "emf\mods"), (Join-Path $Game "emf\config") | Out-Null
Copy-Item (Join-Path $Dist "emf\emf_core.dll") (Join-Path $Game "emf\emf_core.dll") -Force
Copy-Item (Join-Path $Dist "emf\known_tools.txt") (Join-Path $Game "emf\known_tools.txt") -Force  # loaders EMF names
# The in-game console's Vulkan layer manifest (names ..\emf_core.dll; only EMF's loader enables it, for the game only)
New-Item -ItemType Directory -Force (Join-Path $Game "emf\vk") | Out-Null
Copy-Item (Join-Path $Dist "emf\vk\*.json") (Join-Path $Game "emf\vk") -Force
# Folder mods: emf\mods\<id>\<id>.dll + mod.json. A loose <id>.dll left by an older install would load twice.
Get-ChildItem (Join-Path $Dist "emf\mods") -Directory -ErrorAction SilentlyContinue | ForEach-Object {
    $to = Join-Path $Game "emf\mods\$($_.Name)"
    New-Item -ItemType Directory -Force $to | Out-Null
    Get-ChildItem $_.FullName -File | Where-Object { $_.Extension -in ".dll", ".json", ".lua" } |
        ForEach-Object { Copy-Item $_.FullName (Join-Path $to $_.Name) -Force }
    $loose = Join-Path $Game "emf\mods\$($_.Name).dll"
    if (Test-Path $loose) { Remove-Item $loose -Force }
}
$ini = Join-Path $Game "emf\config\sample_mod.ini"
$value = if ($EnableWrite) { 1 } else { 0 }
# Test spawns: F9 = a decorative barrel prop, F11 = a test villager (template GUIDs from export/template_guids.csv)
Set-Content -Encoding ascii $ini ("[sample_mod]`r`nenable_write=$value`r`nguid_layout=ms`r`n" +
    "spawn_guid=0ce28db1-522f-4455-b358-3888fb5e089b`r`n" +
    "spawn_guid2=8bd1dac4-91a5-4287-93c1-7481a4211fce`r`n" +
    "dump_items=1`r`ngive_item=Material_T1_Wood_Logs`r`ngive_count=5`r`n")  # F12 gives 5 wood logs; emf\items.csv lists the names
# Companion mod (F6): spawns Enemy_Vukah_Melee as an ally (faction 11 = PlayerAlly), which follows, guards the player
# and fights at full damage. Only with -EnableWrite. No follow_distance: with an enemy body it would change every wild
# Vukah of that template too.
Set-Content -Encoding ascii (Join-Path $Game "emf\config\companion.ini") ("[companion]`r`nenabled=$value`r`n" +
    "guid=d5d05217-52dc-467a-9192-26b4f661d8d9`r`nfaction=11`r`nauto_summon=0`r`nrespawn_seconds=0`r`nkey=G`r`n")
# villager_jobs mod: villagers craft from a chest (K on a villager, then on a chest). Only with -EnableWrite.
Set-Content -Encoding ascii (Join-Path $Game "emf\config\villager_jobs.ini") ("[villager_jobs]`r`nenabled=$value`r`n" +
    "key=K`r`nseconds_per_item=30`r`nfarm_radius=20`r`ntend_radius=20`r`nhaul_per_trip=20`r`n")
# quick_stack mod: L puts what you carry into nearby chests that already hold it. Only with -EnableWrite.
Set-Content -Encoding ascii (Join-Path $Game "emf\config\quick_stack.ini") ("[quick_stack]`r`nenabled=$value`r`n" +
    "key=L`r`nradius=10`r`nkeep_action_bar=1`r`n")
# clock mod: the time of day on screen (read-only: on by default).
Set-Content -Encoding ascii (Join-Path $Game "emf\config\clock.ini") "[clock]`r`nenabled=1`r`nplace=1`r`nh24=1`r`n"
# big_stacks mod: stack sizes x4 (at most 9999), in memory. Only with -EnableWrite.
Set-Content -Encoding ascii (Join-Path $Game "emf\config\big_stacks.ini") ("[big_stacks]`r`nenabled=$value`r`n" +
    "multiplier=4`r`nmax=9999`r`n")
# The debug bridge ([debug] bridge in emf\emf_core.ini, the file's other sections untouched): on with -Bridge, else off
# if an earlier install turned it on. Release packages never write it.
Add-Type -Namespace EmfInstall -Name Ini -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern bool WritePrivateProfileString(string section, string key, string value, string file);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern uint GetPrivateProfileInt(string section, string key, int fallback, string file);
'@
$coreIni = Join-Path $Game "emf\emf_core.ini"
$bridgeWas = [EmfInstall.Ini]::GetPrivateProfileInt("debug", "bridge", 0, $coreIni)
if ($Bridge) {
    [EmfInstall.Ini]::WritePrivateProfileString("debug", "bridge", "1", $coreIni) | Out-Null
} elseif ($bridgeWas -ne 0) {
    [EmfInstall.Ini]::WritePrivateProfileString("debug", "bridge", "0", $coreIni) | Out-Null
}
# When this install happened (UTC): crash dumps the game writes after it belong to runs of this build.
[System.IO.File]::WriteAllText((Join-Path $Game "emf\install_stamp.txt"),
    [datetime]::UtcNow.ToString("o", [System.Globalization.CultureInfo]::InvariantCulture))
$bridgeText = if ($Bridge) { "on: scripts\emf_bridge.ps1 talks to the running game" } else { "off" }
"installed EMF into $Game (sample_mod write test and companion: $value; debug bridge: $bridgeText)"
Get-ChildItem (Join-Path $Game "emf") -Recurse -File | ForEach-Object { "  " + $_.FullName.Substring($Game.Length + 1) }
