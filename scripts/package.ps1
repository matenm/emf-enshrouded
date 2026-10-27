# Builds the release package build\package\EMF-<version>\ and its zip from a finished build (scripts\test.sh first).
#   package.ps1 [-Build build\msvc] [-Out build\package] [-ExtraContent <pack folder>[,<pack folder>...]]
# -ExtraContent: prebuilt EML content packs, each named by its own folder (with mod.json), that ship in the zips'
# content\ next to the repository's examples, e.g. Beastmaster's bear pack, built from sources that are not in the
# repository. A folder of packs is refused: only packs named one by one ship. A normal build needs none.
# Layout: game\ (what goes next to enshrouded.exe), the player install scripts, sdk\, template\, template_lua\, examples\, mods\
# (sources of the shipped mods, as examples), docs\, licenses\.
# Paths are always taken literally (-LiteralPath): -Path reads brackets in a folder name as a wildcard.
param(
    [string]$Build = (Join-Path $PSScriptRoot "..\build\msvc"),
    [string]$Out = (Join-Path $PSScriptRoot "..\build\package"),
    [string[]]$ExtraContent = @()
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$version = "$(Get-Content -LiteralPath (Join-Path $repo "VERSION") -TotalCount 1)".Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') {  # CMake's project(VERSION) and the zip name want three numbers
    throw "VERSION must be three numbers x.y.z (for example 0.2.0), not '$version'"
}
$dist = Join-Path $Build "dist"
# Not in the release (their code stays in the repository): villager_jobs, raids, beastmaster, necromancer.
$playerMods = @("companion", "quick_stack", "clock", "big_stacks", "loot_scale", "lua", "loot_magnet", "sleep", "herald", "teleports", "bulk_craft")
$scriptMods = @("kill_counter", "life_skills", "champion", "bounty", "grave", "handover")  # Lua mods: a folder with mod.json and .lua files
$exampleMods = @("companion", "quick_stack", "clock", "big_stacks", "sample_mod", "event_log", "sim_rate", "loot_scale", "lua", "loot_magnet", "sleep", "herald", "teleports", "bulk_craft")

$extraPacks = @()
foreach ($pack in $ExtraContent) {
    if (-not $pack) { continue }
    if (-not (Test-Path -LiteralPath $pack -PathType Container)) { throw "-ExtraContent: there is no folder $pack" }
    $p = Get-Item -LiteralPath $pack
    if (-not (Test-Path -LiteralPath (Join-Path $p.FullName "mod.json") -PathType Leaf)) {
        throw "-ExtraContent: $($p.FullName) has no mod.json (not an EML content pack: name the pack's own folder)"
    }
    if (Test-Path -LiteralPath (Join-Path $repo "content\$($p.Name)")) {
        throw "-ExtraContent: content\$($p.Name) is in the repository already (rename the pack)"
    }
    $extraPacks += $p
}

$needed = @("dinput8.dll", "emf\emf_core.dll", "emf\vk\VkLayer_EMF_console.json", "emf\known_tools.txt") + ($playerMods | ForEach-Object { "emf\mods\$_\$_.dll", "emf\mods\$_\mod.json" })
foreach ($f in $needed) { if (-not (Test-Path -LiteralPath (Join-Path $dist $f))) { throw "missing build output $dist\$f (run scripts\test.sh first)" } }
if (-not (Test-Path -LiteralPath (Join-Path $Build "emf_sigcheck.exe"))) { throw "missing build output $Build\emf_sigcheck.exe (run scripts\test.sh first)" }
# EMF.exe, the desktop mod manager, with its page (docs/app.md)
$appFiles = @("EMF.exe", "ui\index.html", "ui\app.css", "ui\app.js", "ui\saves.js")
foreach ($f in $appFiles) { if (-not (Test-Path -LiteralPath (Join-Path $Build "app\$f"))) { throw "missing build output $Build\app\$f (run scripts\test.sh first)" } }
# The core must be built from this VERSION: the build bakes it into emf_core.dll as "emf_core <version>" (the first
# line of emf_core.log), and a build that missed a VERSION change would ship under the new name with the old number.
$corePath = (Resolve-Path -LiteralPath (Join-Path $dist "emf\emf_core.dll")).Path  # .NET reads relative to the process
$coreText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($corePath))
if (-not $coreText.Contains("emf_core $version" + [char]0)) {
    $has = [regex]::Match($coreText, "emf_core (\d+\.\d+\.\d+)\x00")
    $says = if ($has.Success) { "it says $($has.Groups[1].Value)" } else { "it has no version string" }
    throw "$dist\emf\emf_core.dll is not built from VERSION $version ($says); rebuild first (scripts\test.sh)"
}

New-Item -ItemType Directory -Force $Out | Out-Null
$name = "EMF-$version"
$root = Join-Path ((Resolve-Path -LiteralPath $Out).Path) $name
$zip = "$root.zip"
# A second download without EMF.exe (docs/release-signing.md): the same EMF, installed with the scripts, managed from
# the in-game Mods tab. For players who don't want to run an unsigned program.
$manualName = "EMF-$version-manual"
$manualRoot = Join-Path ((Resolve-Path -LiteralPath $Out).Path) $manualName
$manualZip = "$manualRoot.zip"
foreach ($old in @($root, $manualRoot)) { if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Recurse -Force } }
foreach ($old in @($zip, $manualZip)) { if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Force } }

function Copy-Into([string]$from, [string]$to) {
    New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
    Copy-Item -LiteralPath $from -Destination $to -Force
}
function Copy-Tree([string]$from, [string]$to, [string[]]$skipDirs = @()) {
    foreach ($f in Get-ChildItem -LiteralPath $from -Recurse -File) {
        $rel = $f.FullName.Substring($from.Length).TrimStart("\")
        $skip = $false
        foreach ($d in $skipDirs) { if ($rel -like "$d\*") { $skip = $true } }
        if (-not $skip) { Copy-Into $f.FullName (Join-Path $to $rel) }
    }
}

# game\: next to enshrouded.exe
$game = Join-Path $root "game"
Copy-Into (Join-Path $dist "dinput8.dll") (Join-Path $game "dinput8.dll")
Copy-Into (Join-Path $dist "emf\emf_core.dll") (Join-Path $game "emf\emf_core.dll")
Copy-Into (Join-Path $dist "emf\vk\VkLayer_EMF_console.json") (Join-Path $game "emf\vk\VkLayer_EMF_console.json")
Copy-Into (Join-Path $dist "emf\known_tools.txt") (Join-Path $game "emf\known_tools.txt")  # loaders EMF recognises
# EMF.exe goes in emf\, never next to enshrouded.exe (where mods put proxy DLLs); it finds the game as its parent folder
foreach ($f in $appFiles) { Copy-Into (Join-Path $Build "app\$f") (Join-Path $game "emf\$f") }
# the installer's scripts again in emf\setup\: EMF.exe installs EMF into a dedicated server's folder with them
# (docs/app.md "Servers"), so a game folder EMF is installed in still has the installer's rules
foreach ($f in @("install.ps1", "emf_common.ps1")) { Copy-Into (Join-Path $repo "package\$f") (Join-Path $game "emf\setup\$f") }
foreach ($m in $playerMods) {  # folder mods; their defaults come from mod.json (no config files are shipped)
    foreach ($f in @("$m.dll", "mod.json")) {
        Copy-Into (Join-Path $dist "emf\mods\$m\$f") (Join-Path $game "emf\mods\$m\$f")
    }
}
foreach ($m in $scriptMods) {  # whole folder: mod.json and the scripts
    Copy-Tree (Join-Path $repo "mods\$m") (Join-Path $game "emf\mods\$m")
}

# player scripts and readmes
foreach ($f in @("install.ps1", "uninstall.ps1", "emf_common.ps1", "install.cmd", "uninstall.cmd", "README.txt")) {
    Copy-Into (Join-Path $repo "package\$f") (Join-Path $root $f)
}
Copy-Into (Join-Path $repo "README_EMF.md") (Join-Path $root "README_EMF.md")

# modder files
Copy-Tree (Join-Path $repo "sdk\include") (Join-Path $root "sdk\include")
Copy-Tree (Join-Path $repo "template") (Join-Path $root "template") @("build", "out", ".vs")
Copy-Tree (Join-Path $repo "template_lua") (Join-Path $root "template_lua")  # the Lua starter (docs\modding\first_mod.md)
Copy-Into (Join-Path $repo "examples\hello\hello.cpp") (Join-Path $root "examples\hello\hello.cpp")
# the example mods (docs\modding\README.md "Examples"), not installed: Lua folders to copy into <game>\emf\mods\, C++ ones
# to build with their own preset (a build folder someone left in the repository is not shipped)
foreach ($kind in @("lua", "cpp")) {
    Copy-Tree (Join-Path $repo "examples\$kind") (Join-Path $root "examples\$kind") @("build", "out", ".vs", "*\build", "*\out", "*\.vs")
}
foreach ($m in $exampleMods) { Copy-Tree (Join-Path $repo "mods\$m") (Join-Path $root "mods\$m") }
Copy-Tree (Join-Path $repo "docs\modding") (Join-Path $root "docs\modding")
Copy-Tree (Join-Path $repo "content") (Join-Path $root "content")  # EML content examples (docs\modding\content.md)
foreach ($p in $extraPacks) {  # -ExtraContent
    Copy-Tree $p.FullName (Join-Path $root "content\$($p.Name)")
    "package: content\$($p.Name) from $($p.FullName)"
}
Copy-Tree (Join-Path $repo "docs\engine_parts") (Join-Path $root "docs\engine_parts")
# the engine map and the tables it names (impact_natives.txt is linked, components.txt is named)
foreach ($f in @("ENGINE_MAP.md", "ecs_programs.md", "impact_natives.txt", "components.txt", "saves.md")) {  # saves.md: README.txt names it
    Copy-Into (Join-Path $repo "docs\$f") (Join-Path $root "docs\$f")
}

# EMF's own licence at the top, third-party notices in licenses\
Copy-Into (Join-Path $repo "LICENSE") (Join-Path $root "LICENSE.txt")
$lic = Join-Path $root "licenses"
Copy-Into (Join-Path $repo "THIRD_PARTY_NOTICES.md") (Join-Path $lic "THIRD_PARTY_NOTICES.md")
Copy-Into (Join-Path $repo "third_party\minhook\LICENSE.txt") (Join-Path $lic "MinHook-LICENSE.txt")
Copy-Into (Join-Path $repo "third_party\imgui\LICENSE.txt") (Join-Path $lic "DearImGui-LICENSE.txt")
Copy-Into (Join-Path $repo "third_party\vulkan-headers\LICENSE.md") (Join-Path $lic "Vulkan-Headers-LICENSE.md")
Copy-Into (Join-Path $repo "third_party\vulkan-headers\LICENSES\Apache-2.0.txt") (Join-Path $lic "Apache-2.0.txt")
Copy-Into (Join-Path $repo "third_party\lua\LICENSE.txt") (Join-Path $lic "Lua-LICENSE.txt")
Copy-Into (Join-Path $repo "third_party\webview2\LICENSE.txt") (Join-Path $lic "WebView2-LICENSE.txt")  # EMF.exe links its loader

# offline signature check for modders (docs/modding/updating.md)
Copy-Into (Join-Path $Build "emf_sigcheck.exe") (Join-Path $root "tools\emf_sigcheck.exe")
# the debug bridge's clients (docs/modding/debug_bridge.md), installed as <game>\emf\tools\ so they find their game; the
# bridge stays off unless emf\emf_core.ini says bridge=1
Copy-Into (Join-Path $repo "scripts\emf_bridge.ps1") (Join-Path $game "emf\tools\emf_bridge.ps1")
Copy-Into (Join-Path $repo "tools\bridge\emf_bridge.py") (Join-Path $game "emf\tools\emf_bridge.py")
# the scenario runner and the MCP server for agents (docs/modding/ai_agents.md), next to the Python client they use
Copy-Into (Join-Path $repo "tools\bridge\emf_scenario.py") (Join-Path $game "emf\tools\emf_scenario.py")
Copy-Into (Join-Path $repo "tools\mcp\emf_mcp.py") (Join-Path $game "emf\tools\emf_mcp.py")

# The zip spec wants '/' in entry names. Compress-Archive and ZipFile.CreateFromDirectory in Windows PowerShell write
# '\', which tools on Linux and the Steam Deck unpack as flat file names, so the entries are written one by one.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Write-Zip([string]$Dir, [string]$Path, [string]$Prefix) {
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew)
    $archive = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in Get-ChildItem -LiteralPath $Dir -Recurse -File) {
            $entry = $Prefix + "/" + $f.FullName.Substring($Dir.Length + 1).Replace("\", "/")
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry,
                [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally {
        $archive.Dispose()
        $stream.Dispose()
    }
    $files = (Get-ChildItem -LiteralPath $Dir -Recurse -File).Count
    $mb = [math]::Round((Get-Item -LiteralPath $Path).Length / 1MB, 1)
    "package: $Path ($files files, $mb MB)"
}
Write-Zip $root $zip $name

# the manual download: everything but EMF.exe and its page
Copy-Tree $root $manualRoot @("game\emf\ui")
Remove-Item -LiteralPath (Join-Path $manualRoot "game\emf\EMF.exe") -Force
$readme = Join-Path $manualRoot "README.txt"
$note = "THIS IS THE MANUAL DOWNLOAD: the same EMF without EMF.exe, the desktop mod manager. Install with install.cmd, " +
        "manage mods in the game's console (Mods tab). The other download (EMF-$version.zip) adds EMF.exe.`r`n`r`n"
[System.IO.File]::WriteAllText($readme, $note + [System.IO.File]::ReadAllText($readme))
Write-Zip $manualRoot $manualZip $manualName
