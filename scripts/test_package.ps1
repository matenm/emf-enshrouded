# Tests the release package: the player install/uninstall scripts against a fake game folder, and (unless -InstallOnly)
# package.ps1's refusals, then builds the package, extracts the zip into a folder with a space in its name and builds
# the mod template there with its CMake preset and only the packaged SDK. Folder names with brackets check that the
# scripts take paths literally. Every folder it makes in %TEMP% is removed at the end, also when a step throws or the
# run is stopped; the template builds' logs are in build\test_package. Exit code 0 = all passed.
param([switch]$InstallOnly)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$script:fails = 0
$script:made = New-Object System.Collections.ArrayList      # the folders this run made in %TEMP%
$script:regRoots = New-Object System.Collections.ArrayList  # the scratch registry roots it made
$origLocalAppData = $env:LOCALAPPDATA
$env:EMF_TEST_NO_STEAM = "1"  # the scripts under test must never find the real game through Steam
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

# ---- install / uninstall against a fake game and a fake package ----
# The game folder's name has brackets, which PowerShell's -Path reads as a wildcard: the scripts must take it literally.
function Test-InstallScripts([string]$scripts, [string]$gameTree) {
    $game = New-TempDir "game [1]"
    Write-File (Join-Path $game "enshrouded.exe") "fake"
    $inst = Join-Path $scripts "install.ps1"
    $uninst = Join-Path $scripts "uninstall.ps1"
    # what EMF.exe keeps outside the game folder: a fake %LOCALAPPDATA% and a scratch registry root, never the real ones
    $oldLocal = $env:LOCALAPPDATA
    $fakeLocal = New-TempDir "localappdata"
    $env:LOCALAPPDATA = $fakeLocal
    $regRoot = "HKCU:\Software\EMF-test-" + [guid]::NewGuid().ToString("N")
    [void]$script:regRoots.Add($regRoot)
    $env:EMF_TEST_REGISTRY_ROOT = $regRoot
    function Set-ScratchNxm([string]$command) {
        New-Item -Path "$regRoot\Classes\nxm\shell\open\command" -Force | Out-Null
        Set-ItemProperty -LiteralPath "$regRoot\Classes\nxm\shell\open\command" -Name "(default)" -Value $command
        New-Item -Path "$regRoot\EMF\NxmBackup" -Force | Out-Null
        Set-ItemProperty -LiteralPath "$regRoot\EMF\NxmBackup" -Name "had_key" -Value "0"
    }

    & $inst -Game $game -From $gameTree | Out-Null
    $all = Get-ChildItem -LiteralPath $gameTree -Recurse -File | ForEach-Object { $_.FullName.Substring($gameTree.Length + 1) }
    $missing = @($all | Where-Object { -not (Test-Path -LiteralPath (Join-Path $game $_)) })
    Check ($missing.Count -eq 0) "fresh install copies every file ($($all.Count) files; missing: $($missing -join ', '))"

    # a player's settings file (the package ships none; defaults come from mod.json)
    Write-File (Join-Path $game "emf\config\clock.ini") "[clock]`r`nplace=2`r`n"
    $cfg = Get-Item -LiteralPath (Join-Path $game "emf\config\clock.ini")
    Write-File $cfg.FullName "[mine]`r`nkept=1`r`n"
    $mod = Get-ChildItem -LiteralPath (Join-Path $game "emf\mods") -Recurse -Filter *.dll | Select-Object -First 1
    Write-File $mod.FullName "old build"
    & $inst -Game $game -From $gameTree | Out-Null
    Check ((Get-Content -LiteralPath $cfg.FullName -Raw) -match "kept=1") "reinstall keeps the player's config file"
    Check ((Get-Content -LiteralPath $mod.FullName -Raw) -ne "old build") "reinstall replaces a mod DLL"

    # Another mod's dinput8.dll (Creative Mode, XHL-Core, ...): EMF renames it, lists it in dinput8-chain.ini and starts
    # it; uninstall puts it back.
    $foreign = New-TempDir "foreign [x]"
    Write-File (Join-Path $foreign "enshrouded.exe") "fake"
    Write-File (Join-Path $foreign "dinput8.dll") "someone else's proxy"
    $out = (& $inst -Game $foreign -From $gameTree) -join "`n"
    Check ((Get-FileHash -LiteralPath (Join-Path $foreign "dinput8.dll")).Hash -eq (Get-FileHash -LiteralPath (Join-Path $gameTree "dinput8.dll")).Hash) "install puts EMF's dinput8.dll in place of another mod's"
    $moved = Join-Path $foreign "dinput8-previous.dll"
    Check ((Test-Path -LiteralPath $moved) -and (Get-Content -LiteralPath $moved -Raw) -eq "someone else's proxy") "the other mod's dinput8.dll is kept under a new name (dinput8-previous.dll for an unknown tool)"
    $chain = Join-Path $foreign "dinput8-chain.ini"
    Check ((Test-Path -LiteralPath $chain) -and ((Get-Content -LiteralPath $chain) -contains "dinput8-previous.dll")) "the renamed DLL is listed in dinput8-chain.ini, so EMF starts it"
    Check ((Get-Content -LiteralPath $chain)[0] -ceq "[Chain]") "a chain file EMF creates starts with [Chain], as XHL-Core documents it"
    Check (($out -match "dinput8-previous.dll") -and ($out -match "dinput8-chain.ini")) "install says what it moved and where it is listed ($out)"
    & $inst -Game $foreign -From $gameTree | Out-Null
    Check (@(Get-ChildItem -LiteralPath $foreign -Filter "dinput8-previous*.dll").Count -eq 1 -and @((Get-Content -LiteralPath $chain) | Where-Object { $_ -eq "dinput8-previous.dll" }).Count -eq 1) "a reinstall moves nothing again and lists the DLL once"

    # the other mod's installer puts a newer copy of the same tool over EMF's: the old copy is kept (never overwritten)
    # but switched off in the chain file, so only the newest runs (same tool = same EML marker here)
    $eml = New-TempDir "eml"
    Write-File (Join-Path $eml "enshrouded.exe") "fake"
    Write-File (Join-Path $eml "dinput8.dll") "EML v1 EML_LOG_FILE_PATH"
    & $inst -Game $eml -From $gameTree | Out-Null
    Check ((Get-Content -LiteralPath (Join-Path $eml "eml.dll") -Raw) -match "EML v1") "a known tool's DLL is named after it (eml.dll)"
    Write-File (Join-Path $eml "dinput8.dll") "EML v2 EML_LOG_FILE_PATH"
    $out = (& $inst -Game $eml -From $gameTree) -join "`n"
    $lines = @(Get-Content -LiteralPath (Join-Path $eml "dinput8-chain.ini"))
    Check (((Get-Content -LiteralPath (Join-Path $eml "eml.dll") -Raw) -match "EML v1") -and ((Get-Content -LiteralPath (Join-Path $eml "eml-2.dll") -Raw) -match "EML v2")) "a newer copy of the same tool never overwrites the old one (eml.dll kept, eml-2.dll new)"
    Check (($lines -contains "; eml.dll") -and ($lines -contains "eml-2.dll")) "the old copy is switched off in the chain file and the new one listed ($($lines -join ' | '))"
    Check ($out -match "eml.dll") "install says it switched the older copy off ($out)"
    & $uninst -Game $eml | Out-Null
    Check ((Get-Content -LiteralPath (Join-Path $eml "dinput8.dll") -Raw) -match "EML v2") "uninstall puts the newest copy back as dinput8.dll"
    Check (Test-Path -LiteralPath (Join-Path $eml "eml.dll")) "the older copy stays (nothing EMF moved is deleted)"
    # the same file again: no second copy
    $same = New-TempDir "same"
    Write-File (Join-Path $same "enshrouded.exe") "fake"
    Write-File (Join-Path $same "dinput8.dll") "unknown proxy"
    & $inst -Game $same -From $gameTree | Out-Null
    Write-File (Join-Path $same "dinput8.dll") "unknown proxy"
    $out = (& $inst -Game $same -From $gameTree) -join "`n"
    Check (-not (Test-Path -LiteralPath (Join-Path $same "dinput8-previous-2.dll"))) "an identical file is not kept twice ($out)"

    # two different unknown DLLs: both run, with a note that the earlier one is still started
    $two = New-TempDir "two"
    Write-File (Join-Path $two "enshrouded.exe") "fake"
    Write-File (Join-Path $two "dinput8.dll") "unknown mod A"
    & $inst -Game $two -From $gameTree | Out-Null
    Write-File (Join-Path $two "dinput8.dll") "unknown mod B"
    $out = (& $inst -Game $two -From $gameTree) -join "`n"
    $lines = @(Get-Content -LiteralPath (Join-Path $two "dinput8-chain.ini"))
    Check (($lines -contains "dinput8-previous.dll") -and ($lines -contains "dinput8-previous-2.dll")) "two unknown DLLs are both listed ($($lines -join ' | '))"
    Check (($out -match "dinput8-previous.dll") -and ($out -match "newer copy")) "install notes the earlier unknown one is still started ($out)"
    # uninstall: the newest goes back; the other stays renamed, and the message says why
    $out = (& $uninst -Game $two -All -Force) -join "`n"
    Check ((Get-Content -LiteralPath (Join-Path $two "dinput8.dll") -Raw) -eq "unknown mod B") "uninstall puts the newest back"
    Check ((Test-Path -LiteralPath (Join-Path $two "dinput8-previous.dll")) -and ($out -match "only one file can be dinput8.dll")) "the older stays renamed, and uninstall says why ($out)"
    Check (Test-Path -LiteralPath (Join-Path $two "emf\moved.txt")) "uninstall -All keeps the record while a moved file is still renamed"

    # ShroudForge ships one loader as dbghelp.dll and dinput8.dll (same-loader in known_tools.txt): its dinput8.dll copy
    # is kept aside, not started (starting both runs its start-up twice)
    $sf = New-TempDir "sf"
    Write-File (Join-Path $sf "enshrouded.exe") "fake"
    Write-File (Join-Path $sf "dinput8.dll") "SF dinput8 shroudforge"
    Write-File (Join-Path $sf "dbghelp.dll") "SF dbghelp shroudforge"
    $out = (& $inst -Game $sf -From $gameTree) -join "`n"
    Check ((Get-Content -LiteralPath (Join-Path $sf "shroudforge.dll") -Raw) -match "SF dinput8") "ShroudForge's dinput8.dll is kept as shroudforge.dll"
    Check (-not (Test-Path -LiteralPath (Join-Path $sf "dinput8-chain.ini"))) "it is not listed for EMF to start: ShroudForge already runs through its dbghelp.dll"
    Check (($out -match "shroudforge.dll") -and ($out -match "dbghelp.dll")) "install says why it is not started ($out)"
    $out = (& $uninst -Game $sf) -join "`n"
    Check ((Get-Content -LiteralPath (Join-Path $sf "dinput8.dll") -Raw) -match "SF dinput8") "uninstall puts ShroudForge's dinput8.dll back ($out)"
    # a tool without that mark (a DLL with EML's text next to EML's dbghelp.dll) is still started
    $xe = New-TempDir "xe"
    Write-File (Join-Path $xe "enshrouded.exe") "fake"
    Write-File (Join-Path $xe "dinput8.dll") "XHL with EML_LOG_FILE_PATH inside"
    Write-File (Join-Path $xe "dbghelp.dll") "EML dbghelp EML_LOG_FILE_PATH"
    & $inst -Game $xe -From $gameTree | Out-Null
    Check ((Get-Content -LiteralPath (Join-Path $xe "dinput8-chain.ini")) -contains "eml.dll") "only a tool marked same-loader is kept aside; another is listed and started"

    # a chain file the player already had (XHL-Core's way) keeps its entries; only EMF's line comes and goes
    $own = New-TempDir "ownchain"
    Write-File (Join-Path $own "enshrouded.exe") "fake"
    Write-File (Join-Path $own "dinput8.dll") "XHL core"
    Write-File (Join-Path $own "dinput8-chain.ini") "[chain]`r`nother.dll`r`n"
    & $inst -Game $own -From $gameTree | Out-Null
    $lines = @(Get-Content -LiteralPath (Join-Path $own "dinput8-chain.ini"))
    Check (($lines -contains "other.dll") -and ($lines -contains "dinput8-previous.dll") -and ($lines -contains "[chain]")) "install adds its line to an existing chain file and keeps the rest ($($lines -join ' | '))"
    & $uninst -Game $own | Out-Null
    $lines = @(Get-Content -LiteralPath (Join-Path $own "dinput8-chain.ini"))
    Check (($lines -contains "other.dll") -and -not ($lines -contains "dinput8-previous.dll")) "uninstall takes only its own line out of the chain file"
    Check ((Get-Content -LiteralPath (Join-Path $own "dinput8.dll") -Raw) -eq "XHL core") "uninstall puts the other mod's dinput8.dll back"

    # a one-line chain file (only "[Chain]", or only a name) works: PowerShell must not treat the line as a string
    foreach ($one in @("[Chain]", "atlas.dll")) {
        $d1 = New-TempDir "oneline"
        Write-File (Join-Path $d1 "enshrouded.exe") "fake"
        Write-File (Join-Path $d1 "dinput8.dll") "a proxy"
        Write-File (Join-Path $d1 "dinput8-chain.ini") $one
        $threw = $false
        try { & $inst -Game $d1 -From $gameTree | Out-Null } catch { $threw = $true }
        $lines = @(Get-Content -LiteralPath (Join-Path $d1 "dinput8-chain.ini"))
        Check ((-not $threw) -and ($lines -contains "dinput8-previous.dll") -and ($lines -contains $one)) "a chain file holding only '$one' gets EMF's line ($($lines -join ' | '))"
        Remove-Item -LiteralPath $d1 -Recurse -Force -ErrorAction SilentlyContinue
    }

    # EMF's line goes into [Chain], not into a section after it
    $sec = New-TempDir "sections"
    Write-File (Join-Path $sec "enshrouded.exe") "fake"
    Write-File (Join-Path $sec "dinput8.dll") "a proxy"
    Write-File (Join-Path $sec "dinput8-chain.ini") "[Chain]`r`nother.dll`r`n`r`n[Settings]`r`nlog=1`r`n"
    & $inst -Game $sec -From $gameTree | Out-Null
    $lines = @(Get-Content -LiteralPath (Join-Path $sec "dinput8-chain.ini"))
    Check ([array]::IndexOf($lines, "dinput8-previous.dll") -lt [array]::IndexOf($lines, "[Settings]") -and [array]::IndexOf($lines, "dinput8-previous.dll") -gt 0) "EMF's line is added inside [Chain] ($($lines -join ' | '))"

    # a chain file in the ANSI code page keeps its bytes through install and uninstall
    $ansi = New-TempDir "ansi"
    Write-File (Join-Path $ansi "enshrouded.exe") "fake"
    Write-File (Join-Path $ansi "dinput8.dll") "a proxy"
    $chainA = Join-Path $ansi "dinput8-chain.ini"
    $bytes = [System.Text.Encoding]::GetEncoding(1252).GetBytes("[Chain]`r`n; f" + [char]0xFC + "r die Karte`r`nother.dll`r`n")
    [System.IO.File]::WriteAllBytes($chainA, $bytes)
    & $inst -Game $ansi -From $gameTree | Out-Null
    & $uninst -Game $ansi | Out-Null
    Check (([System.IO.File]::ReadAllBytes($chainA) -join ",") -eq ($bytes -join ",")) "an ANSI chain file is byte for byte the same after install and uninstall"

    # a copy of EMF's own loader listed in the chain file (another tool's chain instructions): its line is taken out
    $self = New-TempDir "self"
    Write-File (Join-Path $self "enshrouded.exe") "fake"
    Write-File (Join-Path $self "emf-old.dll") "EMF-LOADER-7d3c9f an older EMF loader"
    Write-File (Join-Path $self "dinput8-chain.ini") "[Chain]`r`nemf-old.dll`r`nkeep.dll`r`n"
    & $inst -Game $self -From $gameTree | Out-Null
    $lines = @(Get-Content -LiteralPath (Join-Path $self "dinput8-chain.ini"))
    Check ((-not ($lines -contains "emf-old.dll")) -and ($lines -contains "keep.dll")) "a chained copy of EMF's loader is taken out of the chain file ($($lines -join ' | '))"
    # a DLL that merely mentions EMF's log is not EMF's loader
    $men = New-TempDir "mention"
    Write-File (Join-Path $men "enshrouded.exe") "fake"
    Write-File (Join-Path $men "dinput8.dll") "a tool that reads emf\emf_loader.log"
    & $inst -Game $men -From $gameTree | Out-Null
    Check ((Get-Content -LiteralPath (Join-Path $men "dinput8-previous.dll") -Raw) -match "reads emf") "a DLL that only mentions emf_loader is moved, not taken for EMF's"

    # a failure while writing the chain file (read-only) puts everything back
    $ro = New-TempDir "readonly"
    Write-File (Join-Path $ro "enshrouded.exe") "fake"
    Write-File (Join-Path $ro "dinput8.dll") "a proxy"
    Write-File (Join-Path $ro "dinput8-chain.ini") "[Chain]`r`nother.dll`r`n"
    (Get-Item -LiteralPath (Join-Path $ro "dinput8-chain.ini")).IsReadOnly = $true
    $threw = $false
    try { & $inst -Game $ro -From $gameTree | Out-Null } catch { $threw = $true }
    (Get-Item -LiteralPath (Join-Path $ro "dinput8-chain.ini")).IsReadOnly = $false
    Check ($threw -and ((Get-Content -LiteralPath (Join-Path $ro "dinput8.dll") -Raw) -eq "a proxy") -and -not (Test-Path -LiteralPath (Join-Path $ro "dinput8-previous.dll"))) "a failed chain-file write puts the other mod's dinput8.dll back"
    # a failure while copying EMF's files (a locked file) puts everything back, including EMF's dinput8.dll copied first
    $lk = New-TempDir "locked"
    Write-File (Join-Path $lk "enshrouded.exe") "fake"
    Write-File (Join-Path $lk "dinput8.dll") "a proxy"
    Write-File (Join-Path $lk "emf\mods\clock\clock.dll") "in use"
    $lock = [System.IO.File]::Open((Join-Path $lk "emf\mods\clock\clock.dll"), "Open", "Read", "None")
    $threw = $false
    try { & $inst -Game $lk -From $gameTree | Out-Null } catch { $threw = $true }
    $lock.Close()
    $lines = @(Get-Content -LiteralPath (Join-Path $lk "dinput8-chain.ini") -ErrorAction SilentlyContinue)
    Check ($threw -and ((Get-Content -LiteralPath (Join-Path $lk "dinput8.dll") -Raw) -eq "a proxy") -and -not ($lines -contains "dinput8-previous.dll")) "a failed copy puts the other mod's dinput8.dll back and takes EMF's line out"

    # a moved file deleted by hand: uninstall still takes its line out (and the chain file EMF created)
    $gone = New-TempDir "gone"
    Write-File (Join-Path $gone "enshrouded.exe") "fake"
    Write-File (Join-Path $gone "dinput8.dll") "a proxy"
    & $inst -Game $gone -From $gameTree | Out-Null
    Remove-Item -LiteralPath (Join-Path $gone "dinput8-previous.dll")
    & $uninst -Game $gone | Out-Null
    Check (-not (Test-Path -LiteralPath (Join-Path $gone "dinput8-chain.ini"))) "a moved file deleted by hand: its line and EMF's chain file go"

    # a DLL whose tool name would be a Windows or proxy DLL name never gets that name (Windows would load it as that DLL)
    $sys = New-TempDir "sysname"
    Write-File (Join-Path $sys "enshrouded.exe") "fake"
    Write-File (Join-Path $sys "dinput8.dll") "a proxy"
    . (Join-Path $scripts "emf_common.ps1")
    $plan = Get-MovePlan $sys (Join-Path $sys "dinput8.dll") @(@{ Word = "dxgi"; Texts = @("a proxy"); SameLoader = @() })
    Check ($plan.Name -eq "dxgi-mod.dll") "a tool word that is a Windows DLL name becomes <name>-mod.dll ($($plan.Name))"
    $plan = Get-MovePlan $sys (Join-Path $sys "dinput8.dll") @(@{ Word = "winmm"; Texts = @("a proxy"); SameLoader = @() })
    Check ($plan.Name -eq "winmm-mod.dll") "a proxy name becomes <name>-mod.dll ($($plan.Name))"
    Remove-Item -LiteralPath $sys -Recurse -Force -ErrorAction SilentlyContinue

    # a DLL with version information is named after its product (the stand-in, when it is built)
    $standin = Join-Path $repo "build\msvc\standin\dinput8.dll"
    if (Test-Path -LiteralPath $standin) {
        $vi = New-TempDir "versioninfo"
        Write-File (Join-Path $vi "enshrouded.exe") "fake"
        Copy-Item -LiteralPath $standin -Destination (Join-Path $vi "dinput8.dll")
        & $inst -Game $vi -From $gameTree | Out-Null
        Check (Test-Path -LiteralPath (Join-Path $vi "emf-stand-in-mod.dll")) "a DLL with version information is named after its product (emf-stand-in-mod.dll)"
        Remove-Item -LiteralPath $vi -Recurse -Force -ErrorAction SilentlyContinue
    }
    foreach ($d in @($same, $two, $xe, $sec, $ansi, $self, $men, $ro, $lk, $gone)) { Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue }

    $nogame = New-TempDir "nogame"
    $threw = $false
    try { & $inst -Game $nogame -From $gameTree | Out-Null } catch { $threw = $true }
    Check $threw "install refuses a folder without enshrouded.exe"

    # a dedicated server folder (enshrouded_server.exe) takes EMF the same way
    $server = New-TempDir "server"
    Write-File (Join-Path $server "enshrouded_server.exe") "fake"
    try { & $inst -Game $server -From $gameTree | Out-Null } catch { }
    Check (Test-Path (Join-Path $server "emf\emf_core.dll")) "install works on a dedicated server folder (enshrouded_server.exe)"
    try { & $uninst -Game $server | Out-Null } catch { }
    Check (-not (Test-Path (Join-Path $server "dinput8.dll"))) "uninstall works on a dedicated server folder"
    Check (Test-Path (Join-Path $server "enshrouded_server.exe")) "uninstall leaves the server alone"

    # files the new package no longer ships go; files EMF never installed stay
    $old = Join-Path $game "emf\mods\retired_mod.dll"
    Write-File $old "from an older EMF"
    Add-Content -LiteralPath (Join-Path $game "emf\installed.txt") "emf\mods\retired_mod.dll"
    $third = Join-Path $game "emf\mods\someone_elses_mod.dll"
    Write-File $third "not EMF's"
    & $inst -Game $game -From $gameTree | Out-Null
    Check (-not (Test-Path -LiteralPath $old)) "reinstall removes a file the older install copied that the new package does not ship"
    Check (Test-Path -LiteralPath $third) "reinstall keeps a mod EMF did not install"
    $rec = Get-Content -LiteralPath (Join-Path $game "emf\installed.txt")
    Check (($rec -contains "emf\emf_core.dll") -and -not ($rec -contains "emf\mods\retired_mod.dll") -and -not ($rec -match "^emf\\config")) "install records what it copied (emf\installed.txt, without config files)"

    Write-File (Join-Path $game "emf\emf_core.ini") "[overlay]`r`nmine=1`r`n"
    # what EMF.exe keeps for the player: backups of replaced mods and the records of mods it installed
    Write-File (Join-Path $game "emf\backup\foo-1\mod.json") "{}"
    Write-File (Join-Path $game "emf\installed_mods\nexus-1.json") "{}"
    Write-File (Join-Path $game "emf\profiles\vanilla.json") '{"name":"Vanilla+","mods":[]}'  # the player's mod profiles
    # the player's Nexus key and downloads, and EMF's saved nxm:// setting while Vortex has the links again
    Write-File (Join-Path $fakeLocal "EMF\nexus.key") "dpapi blob"
    Write-File (Join-Path $fakeLocal "EMF\downloads\Foo-1-1.zip") "zip"
    Set-ScratchNxm "`"C:\Vortex\Vortex.exe`" -d `"%1`""
    $out = (& $uninst -Game $game) -join "`n"
    Check ((Test-Path -LiteralPath (Join-Path $fakeLocal "EMF\nexus.key")) -and (Test-Path -LiteralPath (Join-Path $fakeLocal "EMF\downloads\Foo-1-1.zip")) -and (Test-Path -LiteralPath "$regRoot\EMF\NxmBackup")) "uninstall keeps the Nexus key, the downloads and the saved nxm:// setting"
    Check ($out -match "kept your Nexus API key and EMF's downloads in .*EMF \(uninstall.cmd -All removes them too\)") "uninstall says where the key and the downloads are ($out)"
    Check ((Test-Path -LiteralPath (Join-Path $game "emf\backup\foo-1\mod.json")) -and (Test-Path -LiteralPath (Join-Path $game "emf\installed_mods\nexus-1.json"))) "uninstall keeps emf\backup and emf\installed_mods"
    Check ($out -match "kept .*backups") "uninstall says it kept the backups ($out)"
    Check (Test-Path -LiteralPath (Join-Path $game "emf\profiles\vanilla.json")) "uninstall keeps emf\profiles (the player's mod profiles)"
    Check ($out -match "kept your settings, mod profiles") "uninstall says it kept the profiles ($out)"
    Check (-not (Test-Path -LiteralPath (Join-Path $game "dinput8.dll"))) "uninstall removes dinput8.dll"
    Check (-not (Test-Path -LiteralPath (Join-Path $game "emf\emf_core.dll"))) "uninstall removes the core"
    Check (-not (Test-Path -LiteralPath (Join-Path $game "emf\mods"))) "uninstall removes the mods"
    Check (Test-Path -LiteralPath $cfg.FullName) "uninstall keeps emf\config"
    Check (Test-Path -LiteralPath (Join-Path $game "emf\emf_core.ini")) "uninstall keeps emf\emf_core.ini (overlay settings)"
    Check (Test-Path -LiteralPath (Join-Path $game "enshrouded.exe")) "uninstall leaves the game alone"

    # -All refuses while the companion still holds gear the player gave it (the items exist only in its file)
    & $inst -Game $game -From $gameTree | Out-Null
    $gear = Join-Path $game "emf\config\companion\1.txt"
    Write-File $gear "out 1`npiece 2 0000abcd 6 3 held`n"
    $threw = $false
    try { & $uninst -Game $game -All | Out-Null } catch { $threw = ($_.Exception.Message -match "gear") }
    Check $threw "uninstall -All refuses while the companion holds gear, and says why"
    Check (Test-Path -LiteralPath $gear) "the companion's gear file is kept"
    Write-File (Join-Path $fakeLocal "EMF\nexus.key.tmp") "left over"
    $out = (& $uninst -Game $game -All -Force) -join "`n"
    Check (-not (Test-Path -LiteralPath (Join-Path $game "emf"))) "uninstall -All -Force removes emf\ with the config"
    Check (-not (Test-Path -LiteralPath (Join-Path $fakeLocal "EMF\nexus.key")) -and -not (Test-Path -LiteralPath (Join-Path $fakeLocal "EMF\nexus.key.tmp")) -and -not (Test-Path -LiteralPath (Join-Path $fakeLocal "EMF\downloads"))) "uninstall -All removes the Nexus key and the downloads ($out)"
    Check (-not (Test-Path -LiteralPath "$regRoot\EMF\NxmBackup") -and -not (Test-Path -LiteralPath "$regRoot\EMF")) "uninstall -All removes the saved nxm:// setting once Vortex has the links again"
    # another game folder's EMF.exe still opens the links: its saved setting is still needed
    Set-ScratchNxm "`"D:\Other Game\emf\EMF.exe`" --nxm `"%1`""
    $out = (& $uninst -Game $game -All -Force) -join "`n"
    Check ((Test-Path -LiteralPath "$regRoot\EMF\NxmBackup") -and ($out -match "kept EMF's saved nxm:// setting")) "uninstall -All keeps the saved nxm:// setting while a copy of EMF still opens the links ($out)"

    $clean = New-TempDir "clean"
    Write-File (Join-Path $clean "enshrouded.exe") "fake"
    $out = (& $uninst -Game $clean) -join "`n"
    Check (($out -match "nothing to remove") -and $out.Contains($clean)) "uninstall on a game folder without EMF says so, about that folder ($out)"

    # never outside a game folder: a folder that merely has an "emf" subfolder (e.g. Enhanced Metafiles)
    Write-File (Join-Path $nogame "emf\diagram1.emf") "a picture"
    $threw = $false
    try { & $uninst -Game $nogame -All -Force | Out-Null } catch { $threw = $true }
    Check $threw "uninstall refuses a folder without enshrouded.exe"
    Check (Test-Path (Join-Path $nogame "emf\diagram1.emf")) "nothing is deleted outside a game folder"

    $out = (& $uninst -Game $foreign) -join "`n"
    Check ((Get-Content -LiteralPath (Join-Path $foreign "dinput8.dll") -Raw) -eq "someone else's proxy") "uninstall puts the other mod's dinput8.dll back ($out)"
    Check (-not (Test-Path -LiteralPath (Join-Path $foreign "dinput8-previous.dll"))) "the renamed copy is gone after uninstall"
    Check (-not (Test-Path -LiteralPath (Join-Path $foreign "dinput8-chain.ini"))) "uninstall removes the chain file EMF created once it lists nothing"
    $out = (& $uninst -Game $foreign) -join "`n"
    Check ((Get-Content -LiteralPath (Join-Path $foreign "dinput8.dll") -Raw) -eq "someone else's proxy") "a second uninstall leaves the other mod's dinput8.dll alone"

    # a dinput8.dll that came after EMF (another mod installed over it): uninstall does not overwrite it
    & $inst -Game $foreign -From $gameTree | Out-Null
    Write-File (Join-Path $foreign "dinput8.dll") "a third tool"
    $out = (& $uninst -Game $foreign) -join "`n"
    Check ((Get-Content -LiteralPath (Join-Path $foreign "dinput8.dll") -Raw) -eq "a third tool") "uninstall never overwrites a dinput8.dll that came after EMF"
    Check ((Test-Path -LiteralPath (Join-Path $foreign "dinput8-previous.dll")) -and ($out -match "dinput8-previous.dll")) "the moved DLL stays where it is, and uninstall says so ($out)"

    # Steam library discovery: a fake Steam root whose libraryfolders.vdf names a library path with a space and brackets
    $steam = New-TempDir "steam"
    $lib = New-TempDir "lib [1]"
    $libEsc = $lib.Replace("\", "\\")
    Write-File (Join-Path $steam "steamapps\libraryfolders.vdf") ("`"libraryfolders`"`n{`n`t`"0`"`n`t{`n`t`t`"path`"`t`t`"" +
        $steam.Replace("\", "\\") + "`"`n`t}`n`t`"1`"`n`t{`n`t`t`"path`"`t`t`"" + $libEsc + "`"`n`t}`n}`n")
    Write-File (Join-Path $lib "steamapps\appmanifest_1203620.acf") "`"AppState`"`n{`n`t`"installdir`"`t`t`"Enshrouded`"`n}`n"
    Write-File (Join-Path $lib "steamapps\common\Enshrouded\enshrouded.exe") "fake"
    . (Join-Path $scripts "emf_common.ps1")
    $found = Find-EnshroudedDir -SteamRoot $steam
    Check ($found -eq (Join-Path $lib "steamapps\common\Enshrouded")) "Find-EnshroudedDir finds the game in a second library ($found)"
    $empty = New-TempDir "emptysteam"
    Check ($null -eq (Find-EnshroudedDir -SteamRoot $empty)) "Find-EnshroudedDir returns nothing without a library file"
    # a library whose path is not ASCII (Steam writes libraryfolders.vdf as UTF-8 without a BOM)
    $steam2 = New-TempDir "steam2"
    $lib2 = New-TempDir ("lib " + [char]0x00DC + [char]0x0418 + "gry")
    Write-File (Join-Path $steam2 "steamapps\libraryfolders.vdf") ("`"libraryfolders`"`n{`n`t`"1`"`n`t{`n`t`t`"path`"`t`t`"" + $lib2.Replace("\", "\\") + "`"`n`t}`n}`n")
    Write-File (Join-Path $lib2 "steamapps\appmanifest_1203620.acf") "`"AppState`"`n{`n`t`"installdir`"`t`t`"Enshrouded`"`n}`n"
    Write-File (Join-Path $lib2 "steamapps\common\Enshrouded\enshrouded.exe") "fake"
    $found = Find-EnshroudedDir -SteamRoot $steam2
    Check ($found -eq (Join-Path $lib2 "steamapps\common\Enshrouded")) "Find-EnshroudedDir finds a library with a non-ASCII path ($found)"

    # "is the game running from this folder?": a process whose path cannot be read (the game runs as administrator,
    # the script does not; Windows then gives no path, without an error) must count as running, with the reason
    $elevated = [pscustomobject]@{ Name = "enshrouded"; Id = 4242; Path = $null }
    $why = Get-GameRunning $game @($elevated)
    Check (($why -match "administrator") -and ($why -match "4242")) "a game process whose folder cannot be read counts as running, and the message says why ($why)"
    $here = [pscustomobject]@{ Name = "enshrouded_server"; Id = 4243; Path = (Join-Path $game "enshrouded_server.exe") }
    Check ((Get-GameRunning $game @($here)) -match "4243") "a game running from the folder counts as running"
    $sibling = [pscustomobject]@{ Name = "enshrouded"; Id = 4244; Path = ($game + "2\enshrouded.exe") }
    Check ($null -eq (Get-GameRunning $game @($sibling))) "a game in another folder whose name starts the same does not count"
    Check ($null -eq (Get-GameRunning $game @())) "no game process: not running"
    # EMF's own manager (emf\EMF.exe) holds its files open: install and uninstall wait for it to be closed
    $app = [pscustomobject]@{ Name = "EMF"; Id = 4245; Path = (Join-Path $game "emf\EMF.exe") }
    Check ((Get-GameRunning $game @($app)) -match "EMF.exe") "EMF.exe running from the folder counts as running, by name"
    $otherApp = [pscustomobject]@{ Name = "EMF"; Id = 4246; Path = $null }
    Check ($null -eq (Get-GameRunning $game @($otherApp))) "an unrelated EMF process whose folder cannot be read does not block"

    # the starter backup of the saves with fake save folders, never the real ones: made before anything is
    # installed, and only once, also when it could not copy everything (later chances are the game's automatic backups);
    # a failed copy stops the install (-IfBackupFails Stop) or is said and passed (-IfBackupFails Continue); uninstall -All
    # keeps the backups and says where they are
    $sv = New-TempDir "saves"
    $sg = New-TempDir "saves game"
    Write-File (Join-Path $sg "enshrouded.exe") "fake"
    $svSteam = Join-Path $sv "Steam"
    $svSaved = Join-Path $sv "Saved Games"
    $svRemote = Join-Path $svSteam "userdata\4242\1203620\remote"
    Write-File (Join-Path $svRemote "character0.ksd") "steam char"
    Write-File (Join-Path $svRemote "world0.ksd") "steam world"
    Write-File (Join-Path $svRemote "backup\world0.ksd.backup-9") "the game's own backup"
    Write-File (Join-Path $svSaved "keen games\Enshrouded\default\world0.ksd") "local world"
    Write-File (Join-Path $svSaved "keen games\Enshrouded\local_settings.json") "{}"
    $svHashes = { Get-ChildItem -LiteralPath $sv -Recurse -File | ForEach-Object { "$($_.FullName)=$((Get-FileHash -LiteralPath $_.FullName).Hash)" } }
    $svBefore = @(& $svHashes)
    $store = Join-Path $fakeLocal "EMF\save-backups"
    $starters = { @(Get-SaveBackups $store | Where-Object { $_.Manifest.kind -eq "starter" }) }
    $env:EMF_TEST_STEAM_ROOT = $svSteam
    $env:EMF_TEST_SAVED_GAMES = $svSaved
    try {
        Check ((Get-SaveStoreMutexName "D:\Data\EMF\save-backups") -eq "Local\EMF.SaveBackups.72b0c584") "the installer takes the same store lock as EMF.exe and the game (the name src/saves/backup.cpp makes)"
        # 1. the copy fails, -IfBackupFails Stop: nothing installed; the incomplete starter stays
        $lock = [System.IO.File]::Open((Join-Path $svRemote "world0.ksd"), "Open", "Read", "None")
        $msg = ""
        try { & $inst -Game $sg -From $gameTree -IfBackupFails Stop | Out-Null } catch { $msg = $_.Exception.Message } finally { $lock.Close() }
        Check (($msg -match "not all of them") -and ($msg -match "world0.ksd") -and ($msg -match "Nothing was installed") -and
               -not (Test-Path -LiteralPath (Join-Path $sg "dinput8.dll")) -and -not (Test-Path -LiteralPath (Join-Path $sg "emf"))) "a starter backup that fails stops the install before anything is copied (-IfBackupFails Stop: $msg)"
        Check (@(& $starters).Count -eq 1) "the incomplete starter backup is kept"
        # 2. the next install makes no second starter (the game's automatic backup at its start takes over)
        $out = (& $inst -Game $sg -From $gameTree) -join "`n"
        Check ((Test-Path -LiteralPath (Join-Path $sg "emf\emf_core.dll")) -and @(& $starters).Count -eq 1 -and $out -notmatch "Backed up") "once a starter is there, even an incomplete one, no install makes another ($out)"
        # 3. -IfBackupFails Continue, from a fresh store: says so and installs
        Remove-Item -LiteralPath $store -Recurse -Force
        $lock = [System.IO.File]::Open((Join-Path $svRemote "world0.ksd"), "Open", "Read", "None")
        $out = ""
        try { $out = (& $inst -Game $sg -From $gameTree -IfBackupFails Continue) -join "`n" } finally { $lock.Close() }
        Check (($out -match "Installing anyway") -and @(& $starters).Count -eq 1) "with -IfBackupFails Continue it says so and installs ($out)"
        # 4. a fresh store and the files free: a whole starter
        Remove-Item -LiteralPath $store -Recurse -Force
        $out = (& $inst -Game $sg -From $gameTree) -join "`n"
        $whole = @(& $starters | Where-Object { @($_.Manifest.problems).Count -eq 0 })
        Check ($whole.Count -eq 1 -and $out -match "Backed up your saves first") "with the files free, the install makes a whole starter backup ($out)"
        if ($whole.Count -eq 1) {
            $m = $whole[0].Manifest
            Check ($m.label -eq "Before EMF" -and $m.made_by -eq "install" -and @($m.files).Count -eq 4 -and $m.format -eq 1) "the starter backup is labelled Before EMF and holds the 4 save files ($(@($m.files).Count) files, label $($m.label))"
            $entry = @($m.files | Where-Object { $_.path -eq "steam/4242/world0.ksd" })[0]
            $copy = Join-Path $whole[0].Folder "steam\4242\world0.ksd"
            Check ($null -ne $entry -and (Get-Content -LiteralPath $copy -Raw) -eq "steam world" -and $entry.sha256 -eq (Get-FileHash -LiteralPath $copy).Hash.ToLowerInvariant() -and $entry.storage -eq "steam" -and $entry.steam_id -eq "4242" -and $entry.type -eq "world" -and $entry.slot -eq 0) "its copies match the hashes in backup.json, with storage, Steam id, type and slot"
            Check (-not (Test-Path -LiteralPath (Join-Path $whole[0].Folder "steam\4242\backup"))) "the game's own backup folder is not copied"
            Check ($whole[0].Id -match '^\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d_starter(_\d+)?$') "the backup folder is named after the time and the kind ($($whole[0].Id))"
        }
        Check (@(Get-ChildItem -LiteralPath $store -Directory -Force | Where-Object { $_.Name.StartsWith(".") }).Count -eq 0) "no half-made backup is left in the store"
        $n = @(Get-SaveBackups $store).Count
        $out = (& $inst -Game $sg -From $gameTree) -join "`n"
        Check (@(Get-SaveBackups $store).Count -eq $n -and $out -notmatch "Backed up") "a reinstall makes no second starter backup"
        Check ((@(& $svHashes) -join "`n") -eq ($svBefore -join "`n")) "installing only read the save folders"
        $out = (& $uninst -Game $sg -All -Force) -join "`n"
        Check ((@(Get-SaveBackups $store).Count -eq $n) -and ($out -match [regex]::Escape($store)) -and ($out -match "save backup")) "uninstall -All keeps the save backups and says where they are ($out)"
    } finally {
        Remove-Item Env:\EMF_TEST_STEAM_ROOT, Env:\EMF_TEST_SAVED_GAMES -ErrorAction SilentlyContinue
        foreach ($d in @($sv, $sg)) { Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue }
    }

    # a failure while copying leaves the old loader in place: dinput8.dll is copied last
    $old = New-TempDir "oldemf"
    Write-File (Join-Path $old "enshrouded.exe") "fake"
    Write-File (Join-Path $old "dinput8.dll") "EMF-LOADER-7d3c9f an older EMF"
    Write-File (Join-Path $old "emf\mods\clock\clock.dll") "in use"
    $lock = [System.IO.File]::Open((Join-Path $old "emf\mods\clock\clock.dll"), "Open", "Read", "None")
    $threw = $false
    try { & $inst -Game $old -From $gameTree | Out-Null } catch { $threw = $true }
    $lock.Close()
    Check ($threw -and ((Get-Content -LiteralPath (Join-Path $old "dinput8.dll") -Raw) -match "an older EMF")) "a failed copy leaves the older loader (dinput8.dll is copied last)"
    Remove-Item -LiteralPath $old -Recurse -Force -ErrorAction SilentlyContinue

    foreach ($d in @($game, $foreign, $eml, $own, $sf, $nogame, $server, $clean, $steam, $lib, $empty, $steam2, $lib2, $fakeLocal)) { Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue }
    $env:LOCALAPPDATA = $oldLocal
    Remove-Item Env:\EMF_TEST_REGISTRY_ROOT -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $regRoot -Recurse -Force -ErrorAction SilentlyContinue
}

try {
    "== install scripts (fake package) =="
    $fake = New-TempDir "pkg [1]"
    Write-File (Join-Path $fake "dinput8.dll") "EMF-LOADER-7d3c9f fake"
    Write-File (Join-Path $fake "emf\emf_core.dll") "core"
    Write-File (Join-Path $fake "emf\vk\VkLayer_EMF_console.json") "{}"
    Write-File (Join-Path $fake "emf\mods\clock\clock.dll") "clock"
    Write-File (Join-Path $fake "emf\mods\clock\mod.json") "{}"
    Write-File (Join-Path $fake "emf\config\clock.ini") "[clock]`r`nenabled=1`r`n"
    Copy-Item (Join-Path $repo "src\core\known_tools.txt") (Join-Path $fake "emf\known_tools.txt")  # the real table
    Test-InstallScripts (Join-Path $repo "package") $fake

    # content_test.ps1 against a fake game folder and a stand-in for EML's emm.exe (it rewrites the kfc, appends to
    # kfc_resources, truncates an existing small container and writes a new one; with EMF_FAKE_EMM_FAIL=1 it fails)
    "== content_test.ps1 (fake game, stand-in emm) =="
    function Test-ContentScript([string]$script) {
        # the default backup folder (the only copy of the game's files during a test) must not be under %TEMP%, where a
        # temp cleaner may delete it mid-test; read from the script's param block without running it
        $ast = [System.Management.Automation.Language.Parser]::ParseFile($script, [ref]$null, [ref]$null)
        $param = $ast.ParamBlock.Parameters | Where-Object { $_.Name.VariablePath.UserPath -eq "Backup" }
        $default = "$($ExecutionContext.InvokeCommand.InvokeScript($param.DefaultValue.Extent.Text))"
        $inTemp = @($env:TEMP, $env:TMP, [System.IO.Path]::GetTempPath()) | Where-Object { $_ } |
            Where-Object { ($default + "\").StartsWith($_.TrimEnd("\") + "\", [System.StringComparison]::OrdinalIgnoreCase) }
        Check ($default -and -not $inTemp) "content_test's default backup folder is not under %TEMP% ($default)"

        $g = New-TempDir "content game"
        Write-File (Join-Path $g "enshrouded.exe") "fake"
        Write-File (Join-Path $g "enshrouded.kfc") "kfc vanilla"
        Write-File (Join-Path $g "enshrouded.kfc_resources") "resources vanilla"
        Write-File (Join-Path $g "enshrouded_000.dat") "vanilla container"
        Write-File (Join-Path $g "enshrouded_001.dat") "an earlier EML patch"
        $other = New-TempDir "content other"
        Write-File (Join-Path $other "enshrouded.kfc") "another folder"
        $emm = Join-Path (New-TempDir "content emm") "emm.cmd"
        Write-File $emm ("@echo off`r`nif `"%EMF_FAKE_EMM_FAIL%`"==`"1`" (echo Error: Failed to create AppState 1>&2 & exit /b 1)`r`n" +
            "echo patched>> `"%~4\enshrouded.kfc`"`r`necho more>> `"%~4\enshrouded.kfc_resources`"`r`n" +
            "echo rewritten> `"%~4\enshrouded_001.dat`"`r`necho new> `"%~4\enshrouded_002.dat`"`r`nexit /b 0`r`n")
        $mod = New-TempDir "content mod"
        Write-File (Join-Path $mod "mod.json") '{"id": "fake_content"}'
        $backup = Join-Path (New-TempDir "content backup parent") "b"

        & $script -Apply -Mod $mod -Game $g -Backup $backup -Emm $emm | Out-Null
        Check ((Get-Content -LiteralPath (Join-Path $g "enshrouded.kfc") -Raw) -match "patched") "content_test -Apply runs EML's patch"
        $threw = $false
        try { & $script -Restore -Game $other -Backup $backup | Out-Null } catch { $threw = $true }
        Check $threw "content_test -Restore refuses a folder other than the one it backed up"
        Check ((Get-Content -LiteralPath (Join-Path $other "enshrouded.kfc") -Raw) -match "another folder") "the other folder is untouched"
        & $script -Restore -Backup $backup | Out-Null
        Check ((Get-Content -LiteralPath (Join-Path $g "enshrouded.kfc") -Raw).Trim() -eq "kfc vanilla") "-Restore puts enshrouded.kfc back"
        Check ((Get-Content -LiteralPath (Join-Path $g "enshrouded_001.dat") -Raw).Trim() -eq "an earlier EML patch") "-Restore puts back a container EML rewrote"
        Check (-not (Test-Path (Join-Path $g "enshrouded_002.dat"))) "-Restore removes the container EML added"
        Check (Test-Path (Join-Path $g "enshrouded_000.dat")) "-Restore keeps the game's own containers"
        Check (-not (Test-Path (Join-Path $g "mods\fake_content"))) "-Restore removes the content mod"

        $env:EMF_FAKE_EMM_FAIL = "1"
        $msg = ""
        try { & $script -Apply -Mod $mod -Game $g -Backup $backup -Emm $emm | Out-Null } catch { $msg = $_.Exception.Message }
        $env:EMF_FAKE_EMM_FAIL = $null
        Check ($msg -match "Restore") "a failing EML run says to run -Restore ($msg)"
        & $script -Restore -Backup $backup | Out-Null
        Check ((Get-Content -LiteralPath (Join-Path $g "enshrouded.kfc") -Raw).Trim() -eq "kfc vanilla") "-Restore after a failed run leaves the folder as it was"

        # without -Game: EMF_GAME_DIR, else the game a Steam library has (scripts\find_game.ps1; a fake Steam, never the real
        # one); with neither it says to pass -Game
        $steam = New-TempDir "content steam"
        $found = Join-Path $steam "steamapps\common\Enshrouded"
        Write-File (Join-Path $steam "steamapps\appmanifest_1203620.acf") "`"AppState`"`n{`n`t`"installdir`"`t`t`"Enshrouded`"`n}`n"
        foreach ($f in "enshrouded.exe", "enshrouded.kfc", "enshrouded.kfc_resources") { Write-File (Join-Path $found $f) "$f vanilla" }
        $savedGameDir = $env:EMF_GAME_DIR
        Remove-Item Env:\EMF_GAME_DIR -ErrorAction SilentlyContinue
        $env:EMF_TEST_STEAM_ROOT = $steam
        try {
            & $script -Apply -Mod $mod -Backup $backup -Emm $emm | Out-Null
            Check ((Get-Content -LiteralPath (Join-Path $found "enshrouded.kfc") -Raw) -match "patched") "content_test -Apply without -Game patches the game a Steam library has"
            & $script -Restore -Backup $backup | Out-Null
            Check ((Get-Content -LiteralPath (Join-Path $found "enshrouded.kfc") -Raw).Trim() -eq "enshrouded.kfc vanilla") "and -Restore puts it back"
            $env:EMF_GAME_DIR = $g
            & $script -Apply -Mod $mod -Backup $backup -Emm $emm | Out-Null
            Check (((Get-Content -LiteralPath (Join-Path $g "enshrouded.kfc") -Raw) -match "patched") -and -not ((Get-Content -LiteralPath (Join-Path $found "enshrouded.kfc") -Raw) -match "patched")) "content_test -Apply: EMF_GAME_DIR wins over Steam"
            & $script -Restore -Backup $backup | Out-Null
            Remove-Item Env:\EMF_GAME_DIR, Env:\EMF_TEST_STEAM_ROOT -ErrorAction SilentlyContinue
            $msg = ""
            try { & $script -Apply -Mod $mod -Backup $backup -Emm $emm | Out-Null } catch { $msg = $_.Exception.Message }
            Check (($msg -match "pass -Game <folder>") -and -not (Test-Path -LiteralPath $backup)) "content_test -Apply without a game found says to pass -Game and backs up nothing ($msg)"
        } finally {
            $env:EMF_GAME_DIR = $savedGameDir
            Remove-Item Env:\EMF_TEST_STEAM_ROOT -ErrorAction SilentlyContinue
        }
        foreach ($d in @($g, $other, (Split-Path $emm), $mod, (Split-Path $backup), $steam)) { Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue }
    }
    Test-ContentScript (Join-Path $repo "scripts\content_test.ps1")
    Remove-Item -LiteralPath $fake -Recurse -Force

    if (-not $InstallOnly) {
        "== package.ps1 refusals (fake repo, fake build) =="
        $ver = (Get-Content -LiteralPath (Join-Path $repo "VERSION") -TotalCount 1).Trim()
        # VERSION must be x.y.z: a fake repo with only the script and a VERSION file (the check comes before anything else)
        $fr = New-TempDir "fake repo"
        Write-File (Join-Path $fr "scripts\package.ps1") (Get-Content -LiteralPath (Join-Path $repo "scripts\package.ps1") -Raw)
        foreach ($bad in @("0.2", "0.2.0-beta", "v0.2.0", "")) {
            Write-File (Join-Path $fr "VERSION") "$bad`r`n"
            $msg = ""
            try { & (Join-Path $fr "scripts\package.ps1") -Out (Join-Path $fr "out") | Out-Null } catch { $msg = $_.Exception.Message }
            Check ($msg -match "x\.y\.z") "package.ps1 refuses VERSION '$bad' and says the form it wants ($msg)"
        }
        Remove-Item -LiteralPath $fr -Recurse -Force -ErrorAction SilentlyContinue

        # a copy of the build (without .pdb files) in a folder whose name has brackets, packaged into another such folder:
        # every path must be taken literally
        $fb = New-TempDir "build [x]"
        $fbOut = New-TempDir "out [x]"
        $realBuild = Join-Path $repo "build\msvc"
        $realDist = Join-Path $realBuild "dist"
        foreach ($f in Get-ChildItem -LiteralPath $realDist -Recurse -File | Where-Object { $_.Extension -ne ".pdb" }) {
            $to = Join-Path $fb ("dist\" + $f.FullName.Substring($realDist.Length + 1))
            New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
            Copy-Item -LiteralPath $f.FullName -Destination $to
        }
        Copy-Item -LiteralPath (Join-Path $realBuild "emf_sigcheck.exe") -Destination (Join-Path $fb "emf_sigcheck.exe")
        foreach ($f in @("EMF.exe", "ui\index.html", "ui\app.css", "ui\app.js", "ui\saves.js")) {  # EMF.exe and its page
            New-Item -ItemType Directory -Force (Split-Path (Join-Path $fb "app\$f")) | Out-Null
            Copy-Item -LiteralPath (Join-Path $realBuild "app\$f") -Destination (Join-Path $fb "app\$f")
        }
        $msg = ""
        try { & (Join-Path $repo "scripts\package.ps1") -Build $fb -Out $fbOut | Out-Null } catch { $msg = $_.Exception.Message }
        $fbZip = Join-Path $fbOut "EMF-$ver.zip"
        Check (($msg -eq "") -and (Test-Path -LiteralPath $fbZip)) "package.ps1 works with brackets in the build and output folder names ($msg)"
        if (Test-Path -LiteralPath $fbZip) {
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $za = [System.IO.Compression.ZipFile]::OpenRead($fbZip)
            $names = @($za.Entries | ForEach-Object { $_.FullName })
            $za.Dispose()
            Check (($names -contains "EMF-$ver/game/emf/emf_core.dll") -and ($names -contains "EMF-$ver/sdk/include/emf/emf.hpp")) "that zip has the core and the SDK ($($names.Count) entries)"
            Check (-not @($names | Where-Object { $_ -match "/content/emf_fake_pack/" }).Count) "without -ExtraContent no extra pack is shipped"
        }
        # -ExtraContent: prebuilt EML packs (the Beastmaster bear pack is built outside the repository), each named by its
        # own folder, go into both zips' content\; a folder that is not a pack (a folder of packs, too: it would ship
        # every test pack in it) is refused before anything is written
        $xc = New-TempDir "extra content [x]"
        Write-File (Join-Path $xc "emf_fake_pack\mod.json") '{"id": "emf_fake_pack", "capabilities": ["patch"]}'
        Write-File (Join-Path $xc "emf_fake_pack\src\mod.lua") "-- a fake pack"
        Write-File (Join-Path $xc "emf_other_pack\mod.json") '{"id": "emf_other_pack", "capabilities": ["patch"]}'
        $msg = ""
        try { & (Join-Path $repo "scripts\package.ps1") -Build $fb -Out $fbOut -ExtraContent $xc | Out-Null } catch { $msg = $_.Exception.Message }
        Check (($msg -match "mod\.json") -and ($msg -match "pack's own folder")) "package.ps1 -ExtraContent refuses a folder of packs (only named packs ship) ($msg)"
        $msg = ""
        try { & (Join-Path $repo "scripts\package.ps1") -Build $fb -Out $fbOut -ExtraContent (Join-Path $xc "emf_fake_pack") | Out-Null } catch { $msg = $_.Exception.Message }
        Check ($msg -eq "") "package.ps1 -ExtraContent packages a prebuilt pack ($msg)"
        foreach ($zp in @(@{f = $fbZip; p = "EMF-$ver"}, @{f = (Join-Path $fbOut "EMF-$ver-manual.zip"); p = "EMF-$ver-manual"})) {
            if (Test-Path -LiteralPath $zp.f) {
                $za = [System.IO.Compression.ZipFile]::OpenRead($zp.f)
                $names = @($za.Entries | ForEach-Object { $_.FullName })
                $za.Dispose()
                Check (($names -contains "$($zp.p)/content/emf_fake_pack/mod.json") -and ($names -contains "$($zp.p)/content/emf_fake_pack/src/mod.lua") -and ($names -contains "$($zp.p)/content/emberbrand/mod.json")) "$($zp.p).zip has the extra pack in content\ next to the repository's examples"
                Check (-not @($names | Where-Object { $_ -match "/content/emf_other_pack/" }).Count) "$($zp.p).zip has only the pack that was named"
            } else { Check $false "$($zp.f) was written" }
        }
        Write-File (Join-Path $xc "not_a_pack\readme.txt") "no mod.json here"
        $msg = ""
        try { & (Join-Path $repo "scripts\package.ps1") -Build $fb -Out $fbOut -ExtraContent (Join-Path $xc "emf_fake_pack"), (Join-Path $xc "not_a_pack") | Out-Null } catch { $msg = $_.Exception.Message }
        Check (($msg -match "not_a_pack") -and ($msg -match "mod\.json")) "package.ps1 -ExtraContent refuses a folder without mod.json and names it ($msg)"
        $msg = ""
        try { & (Join-Path $repo "scripts\package.ps1") -Build $fb -Out $fbOut -ExtraContent (Join-Path $xc "missing") | Out-Null } catch { $msg = $_.Exception.Message }
        Check ($msg -match "no folder") "package.ps1 -ExtraContent refuses a folder that is not there ($msg)"
        Remove-Item -LiteralPath $xc -Recurse -Force -ErrorAction SilentlyContinue
        # a core built before a VERSION bump says the old version (its first log line "emf_core <version> starting")
        Write-File (Join-Path $fb "dist\emf\emf_core.dll") ("MZ an old build: emf_core 0.0.1" + [char]0 + " starting")
        $msg = ""
        try { & (Join-Path $repo "scripts\package.ps1") -Build $fb -Out $fbOut | Out-Null } catch { $msg = $_.Exception.Message }
        Check (($msg -match "0\.0\.1") -and ($msg -match [regex]::Escape($ver)) -and ($msg -match "(?i)rebuild")) "package.ps1 refuses a core that is not built from VERSION and says to rebuild ($msg)"
        Remove-Item -LiteralPath $fb, $fbOut -Recurse -Force -ErrorAction SilentlyContinue

        "== package + template build =="
        $out = New-TempDir "out [1]"
        & (Join-Path $repo "scripts\package.ps1") -Out $out | Out-Host
        $zip = Get-Item -LiteralPath (Join-Path $out "EMF-$ver.zip") -ErrorAction SilentlyContinue
        Check ($null -ne $zip) "package.ps1 writes EMF-$ver.zip (named after VERSION)"
        $manual = Get-Item -LiteralPath (Join-Path $out "EMF-$ver-manual.zip") -ErrorAction SilentlyContinue
        Check ($null -ne $manual) "package.ps1 also writes EMF-$ver-manual.zip (no .exe)"
        & (Join-Path $repo "scripts\package.ps1") -Out $out | Out-Null  # a second run replaces, never merges
        Check (@(Get-ChildItem -LiteralPath $out -Filter "EMF-*.zip").Count -eq 2) "packaging twice leaves the two zips"
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zm = [System.IO.Compression.ZipFile]::OpenRead($manual.FullName)
        $mnames = @($zm.Entries | ForEach-Object { $_.FullName })
        $zm.Dispose()
        $zf = [System.IO.Compression.ZipFile]::OpenRead($zip.FullName)
        $fnames = @($zf.Entries | ForEach-Object { $_.FullName })
        $zf.Dispose()
        Check (($fnames -contains "EMF-$ver/LICENSE.txt") -and ($fnames -contains "EMF-$ver/licenses/THIRD_PARTY_NOTICES.md") -and ($mnames -contains "EMF-$ver-manual/LICENSE.txt")) "both zips carry EMF's MIT licence and the third-party notices"
        # in both downloads: Loot Magnet (DLL + mod.json), Wandering Champion (Lua); Night Raids is left out
        foreach ($z in @(@{n = $fnames; p = "EMF-$ver"}, @{n = $mnames; p = "EMF-$ver-manual"})) {
            $missing = @(@("game/emf/mods/loot_magnet/loot_magnet.dll",
                         "game/emf/mods/loot_magnet/mod.json", "game/emf/mods/champion/main.lua", "game/emf/mods/champion/mod.json",
                         "mods/loot_magnet/README.md") | Where-Object { $z.n -notcontains "$($z.p)/$_" })
            Check ($missing.Count -eq 0) "$($z.p).zip has Loot Magnet and Wandering Champion (missing: $($missing -join ', '))"
            # Chest Machines (quick_stack 1.2), Sleep Through the Day (DLL), Bounty Board and Grave Keeper (Lua)
            $missing = @(@("game/emf/mods/quick_stack/quick_stack.dll", "game/emf/mods/quick_stack/mod.json",
                         "game/emf/mods/sleep/sleep.dll", "game/emf/mods/sleep/mod.json", "game/emf/mods/bounty/main.lua",
                         "game/emf/mods/bounty/mod.json", "game/emf/mods/bounty/README.md", "game/emf/mods/grave/main.lua",
                         "game/emf/mods/grave/mod.json", "game/emf/mods/grave/README.md", "mods/quick_stack/README.md",
                         "mods/quick_stack/qs_glue.h", "mods/sleep/README.md", "mods/sleep/sleep_logic.h") |
                         Where-Object { $z.n -notcontains "$($z.p)/$_" })
            Check ($missing.Count -eq 0) "$($z.p).zip has Chest Machines, Sleep Through the Day, Bounty Board and Grave Keeper (missing: $($missing -join ', '))"
            # Server Herald (DLL + mod.json + its source) and Hand-over (Lua)
            $missing = @(@("game/emf/mods/herald/herald.dll", "game/emf/mods/herald/mod.json", "game/emf/mods/handover/main.lua",
                         "game/emf/mods/handover/mod.json", "game/emf/mods/handover/README.md", "mods/herald/README.md",
                         "mods/herald/herald_logic.h") | Where-Object { $z.n -notcontains "$($z.p)/$_" })
            Check ($missing.Count -eq 0) "$($z.p).zip has Server Herald and Hand-over (missing: $($missing -join ', '))"
            # Waypoint Teleports (DLL), installed switched off, and the SDK's teleport helpers
            $missing = @(@("game/emf/mods/teleports/teleports.dll", "game/emf/mods/teleports/mod.json", "mods/teleports/README.md",
                         "mods/teleports/tp_logic.h", "mods/teleports/tp_glue.h", "sdk/include/emf/teleport.hpp") |
                         Where-Object { $z.n -notcontains "$($z.p)/$_" })
            Check ($missing.Count -eq 0) "$($z.p).zip has Waypoint Teleports and emf/teleport.hpp (missing: $($missing -join ', '))"
            # Bulk Craft (DLL + mod.json, installed switched off, and its source with the README)
            $missing = @(@("game/emf/mods/bulk_craft/bulk_craft.dll", "game/emf/mods/bulk_craft/mod.json",
                         "mods/bulk_craft/README.md", "mods/bulk_craft/bc_logic.h", "mods/bulk_craft/bc_glue.h",
                         "mods/bulk_craft/bc_recipes.h", "mods/bulk_craft/bulk_craft.cpp") |
                         Where-Object { $z.n -notcontains "$($z.p)/$_" })
            Check ($missing.Count -eq 0) "$($z.p).zip has Bulk Craft (missing: $($missing -join ', '))"
            # left out of the release (their code stays in the repository): Villager Jobs, Night Raids, Beastmaster, Necromancer
            $present = @($z.n | Where-Object { $_ -match "/(villager_jobs|raids|beastmaster|necromancer)/" })
            Check ($present.Count -eq 0) "$($z.p).zip leaves out Villager Jobs, Night Raids, Beastmaster and Necromancer (found: $($present -join ', '))"
        }
        # the example mods are in both downloads under examples\, and never installed
        # every folder in examples\lua and examples\cpp, read from the repository (a new example cannot be left out)
        $luaExamples = @(Get-ChildItem -LiteralPath (Join-Path $repo "examples\lua") -Directory | ForEach-Object { $_.Name })
        $cppExamples = @(Get-ChildItem -LiteralPath (Join-Path $repo "examples\cpp") -Directory | ForEach-Object { $_.Name })
        Check (($luaExamples.Count -ge 6) -and ($cppExamples -contains "waypoints")) "the repository has the example mods (Lua: $($luaExamples -join ', '); C++: $($cppExamples -join ', '))"
        $exampleFiles = @($luaExamples | ForEach-Object { "examples/lua/$_/main.lua"; "examples/lua/$_/mod.json"; "examples/lua/$_/README.md" }) +
            @($cppExamples | ForEach-Object { "examples/cpp/$_/CMakeLists.txt"; "examples/cpp/$_/CMakePresets.json"; "examples/cpp/$_/mod.json"; "examples/cpp/$_/README.md" }) +
            @("src/waypoints.cpp", "src/waypoint_book.h" | ForEach-Object { "examples/cpp/waypoints/$_" })
        $exampleIds = ($luaExamples + $cppExamples) -join "|"
        foreach ($z in @(@{n = $fnames; p = "EMF-$ver"}, @{n = $mnames; p = "EMF-$ver-manual"})) {
            $missing = @($exampleFiles | Where-Object { $z.n -notcontains "$($z.p)/$_" })
            Check ($missing.Count -eq 0) "$($z.p).zip has the example mods in examples\ (missing: $($missing -join ', '))"
            $installed = @($z.n | Where-Object { $_ -match "/game/emf/mods/($exampleIds)/" })
            Check ($installed.Count -eq 0) "$($z.p).zip does not install the examples ($($installed -join ', '))"
            $stray = @($z.n | Where-Object { $_ -match "/examples/cpp/([^/]+/)?(build|out|\.vs)/" })
            Check ($stray.Count -eq 0) "$($z.p).zip has no build folders in examples\ ($($stray.Count))"
        }
        Check (-not @($mnames | Where-Object { $_ -match "(?i)\.exe$" -and $_ -notmatch "/tools/" }).Count -and ($mnames -contains "EMF-$ver-manual/install.cmd") -and ($mnames -contains "EMF-$ver-manual/game/dinput8.dll") -and -not ($mnames -match "/game/emf/ui/")) "the manual zip has EMF and the install scripts but no EMF.exe and no manager page"

        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $za = [System.IO.Compression.ZipFile]::OpenRead($zip.FullName)
        $back = @($za.Entries | Where-Object { $_.FullName.Contains("\") }).Count
        $first = $za.Entries[0].FullName
        $za.Dispose()
        Check ($back -eq 0) "zip entries use forward slashes, so tools on Linux / Steam Deck unpack folders ($back with '\', e.g. $first)"

        $x = New-TempDir "unzipped"
        Expand-Archive -LiteralPath $zip.FullName -DestinationPath $x
        $pkg = Join-Path $x "EMF-$ver"
        foreach ($f in @("install.cmd", "install.ps1", "uninstall.cmd", "uninstall.ps1", "emf_common.ps1", "README.txt",
                         "game\dinput8.dll", "game\emf\emf_core.dll", "game\emf\vk\VkLayer_EMF_console.json", "game\emf\known_tools.txt",
                         "game\emf\mods\clock\clock.dll", "game\emf\mods\clock\mod.json", "sdk\include\emf\emf.hpp",
                         "sdk\include\emf\generated\components.hpp", "template\CMakeLists.txt", "template\src\mymod.cpp",
                         "template_lua\mod.json", "template_lua\main.lua", "template_lua\README.md", "docs\modding\first_mod.md",
                         "examples\hello\hello.cpp", "mods\companion\companion.cpp", "docs\modding\README.md",
                         "docs\ENGINE_MAP.md", "licenses\MinHook-LICENSE.txt", "tools\emf_sigcheck.exe",
                         "game\emf\mods\lua\lua.dll", "game\emf\mods\lua\mod.json", "game\emf\mods\kill_counter\main.lua",
                         "game\emf\mods\kill_counter\mod.json", "game\emf\mods\life_skills\main.lua",
                         "game\emf\mods\life_skills\mod.json", "licenses\Lua-LICENSE.txt", "docs\modding\lua.md",
                         "game\emf\mods\loot_magnet\loot_magnet.dll",
                         "game\emf\mods\champion\main.lua", "game\emf\mods\champion\README.md",
                         "mods\loot_magnet\magnet_logic.h",
                         "docs\components.txt", "docs\saves.md", "game\emf\EMF.exe", "game\emf\ui\index.html", "game\emf\ui\app.js", "game\emf\ui\saves.js",
                         "game\emf\ui\app.css", "licenses\WebView2-LICENSE.txt", "game\emf\tools\emf_scenario.py",
                         "game\emf\tools\emf_mcp.py", "docs\modding\llms.txt", "docs\modding\ai_agents.md")) {
            Check (Test-Path (Join-Path $pkg $f)) "package has $f"
        }
        Check (-not (Test-Path (Join-Path $pkg "game\EMF.exe"))) "EMF.exe ships in game\emf\, never next to enshrouded.exe"
        # every relative link in the packaged engine map points at a file the package has
        $map = Join-Path $pkg "docs\ENGINE_MAP.md"
        $links = @([regex]::Matches((Get-Content -LiteralPath $map -Raw), '\]\(([^)#\s]+)') | ForEach-Object { $_.Groups[1].Value } |
            Where-Object { $_ -notmatch '^[a-z]+:' } | Select-Object -Unique)
        $broken = @($links | Where-Object { -not (Test-Path -LiteralPath (Join-Path (Split-Path $map) $_)) })
        Check (($links.Count -gt 0) -and ($broken.Count -eq 0)) "docs\ENGINE_MAP.md links only files the package has ($($links.Count) links; missing: $($broken -join ', '))"
        $readme = Get-Content (Join-Path $pkg "README.txt") -Raw
        Check (-not ($readme -match "\.ps1 -")) "README.txt tells players to use the .cmd files (scripts are blocked by execution policy)"
        Check ($readme -match "(?i)extract") "README.txt says to extract the zip first"
        Check (-not (Test-Path (Join-Path $pkg "game\emf\mods\sample_mod"))) "developer sample mods are not installed for players"
        Check (-not (Test-Path (Join-Path $pkg "game\emf\config"))) "no settings files are shipped (defaults come from mod.json)"
        Check (-not (Test-Path (Join-Path $pkg "template\build"))) "the template has no build folder"
        Check ((Get-Content (Join-Path $pkg "game\emf\mods\big_stacks\mod.json") -Raw) -match '"enabled": false') "big_stacks is off by default"
        Check ((Get-Content (Join-Path $pkg "game\emf\mods\bulk_craft\mod.json") -Raw) -match '"enabled": false') "bulk_craft is off by default"
        Check ((Get-Content (Join-Path $pkg "game\emf\mods\companion\mod.json") -Raw) -match '"enabled": false') "Companion is off by default"
        Check ((Get-Content (Join-Path $pkg "game\emf\mods\life_skills\mod.json") -Raw) -match '"enabled": false') "Life Skills is off by default"
        Check ((Get-Content (Join-Path $pkg "game\emf\mods\kill_counter\mod.json") -Raw) -match '"enabled": false') "Kill Counter is off by default"
        Check (-not ((Get-Content (Join-Path $pkg "game\emf\mods\clock\mod.json") -Raw) -match '"enabled": false')) "the clock is on by default"
        Check (-not ((Get-Content (Join-Path $pkg "game\emf\mods\quick_stack\mod.json") -Raw) -match '"enabled": false')) "Quick Stack is on by default"
        Check (-not ((Get-Content (Join-Path $pkg "game\emf\mods\lua\mod.json") -Raw) -match '"enabled": false')) "the Lua host is on by default"
        Check ((Get-Content (Join-Path $pkg "game\emf\mods\teleports\mod.json") -Raw) -match '"enabled": false') "Waypoint Teleports is off by default"

        # build the template inside the unzipped package (path with spaces), with only the packaged SDK, the way its README
        # says: with the preset it ships (cmake --preset release, then cmake --build --preset release, in its folder)
        $tpl = Join-Path $pkg "template"
        $logs = Join-Path $repo "build\test_package"  # kept after the run (the temp folders are not)
        New-Item -ItemType Directory -Force $logs | Out-Null
        $log = Join-Path $logs "template_build.log"
        $dev = Join-Path $repo "scripts\dev.cmd"
        $presets = Get-Content -LiteralPath (Join-Path $tpl "CMakePresets.json") -Raw | ConvertFrom-Json
        $preset = $presets.configurePresets | Where-Object { $_.name -eq "release" }
        Check (($null -ne $preset) -and (@($presets.buildPresets | Where-Object { $_.name -eq "release" }).Count -eq 1)) "the template ships the configure and build preset 'release' its README names"
        $bld = Join-Path $tpl "build\release"
        cmd /c "cd /d `"$tpl`" && `"$dev`" cmake --preset release > `"$log`" 2>&1"
        cmd /c "cd /d `"$tpl`" && `"$dev`" cmake --build --preset release >> `"$log`" 2>&1"
        $dll = Join-Path $bld "mymod\mymod.dll"
        Check (Test-Path $dll) "the template builds with its preset from the package, with only the packaged SDK (log: $log)"
        Check (Test-Path (Join-Path $bld "mymod\mod.json")) "the built mod folder has its mod.json"
        if (Test-Path $dll) {
            $exports = (cmd /c "`"$dev`" dumpbin /exports `"$dll`" 2>nul") -join "`n"
            Check ($exports -match "emf_mod_load") "mymod.dll exports emf_mod_load"
        }
        $warn = @(Select-String -LiteralPath $log -Pattern "warning C" -ErrorAction SilentlyContinue)
        Check ($warn.Count -eq 0) "the template builds without warnings ($($warn.Count))"

        # the C++ example builds the same way from the package: its own preset, only the packaged SDK (three folders up)
        $wp = Join-Path $pkg "examples\cpp\waypoints"
        $wlog = Join-Path $logs "waypoints_build.log"
        cmd /c "cd /d `"$wp`" && `"$dev`" cmake --preset release > `"$wlog`" 2>&1"
        cmd /c "cd /d `"$wp`" && `"$dev`" cmake --build --preset release >> `"$wlog`" 2>&1"
        $wdll = Join-Path $wp "build\release\waypoints\waypoints.dll"
        Check (Test-Path $wdll) "the waypoints example builds with its preset from the package, with only the packaged SDK (log: $wlog)"
        Check (Test-Path (Join-Path $wp "build\release\waypoints\mod.json")) "the built waypoints folder has its mod.json"
        if (Test-Path $wdll) {
            $exports = (cmd /c "`"$dev`" dumpbin /exports `"$wdll`" 2>nul") -join "`n"
            Check ($exports -match "emf_mod_load") "waypoints.dll exports emf_mod_load"
        }
        $wwarn = @(Select-String -LiteralPath $wlog -Pattern "warning C" -ErrorAction SilentlyContinue)
        Check ($wwarn.Count -eq 0) "the waypoints example builds without warnings ($($wwarn.Count))"

        # renaming the mod is one change: the project() name in CMakeLists.txt
        $ren = Join-Path $x "renamed mod"
        Copy-Item -LiteralPath $tpl -Destination $ren -Recurse
        Remove-Item -LiteralPath (Join-Path $ren "build") -Recurse -Force -ErrorAction SilentlyContinue
        $cm = Join-Path $ren "CMakeLists.txt"
        (Get-Content -LiteralPath $cm -Raw).Replace("project(mymod ", "project(coolmod ") | Set-Content -LiteralPath $cm -Encoding ascii -NoNewline
        $rbld = Join-Path $ren "build\release"
        $sdk = (Join-Path $pkg "sdk\include").Replace("\", "/")
        cmd /c "`"$dev`" cmake -S `"$ren`" -B `"$rbld`" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=cl `"-DEMF_SDK_DIR=$sdk`" >> `"$log`" 2>&1"
        cmd /c "`"$dev`" cmake --build `"$rbld`" >> `"$log`" 2>&1"
        Check (Test-Path (Join-Path $rbld "coolmod\coolmod.dll")) "renaming project() in CMakeLists.txt renames the mod (coolmod\coolmod.dll)"
        $rjson = Join-Path $rbld "coolmod\mod.json"
        Check ((Test-Path $rjson) -and ((Get-Content -LiteralPath $rjson -Raw) -match '"id"\s*:\s*"coolmod"')) "the built mod.json carries the new id"

        "== install scripts (real package) =="
        Test-InstallScripts $pkg (Join-Path $pkg "game")
    }
} finally {
    foreach ($d in $script:made) {
        # (the compiler's PDB server may hold a file of the template build for a moment after it ended)
        for ($i = 0; $i -lt 10 -and (Test-Path -LiteralPath $d); $i++) {
            Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
            if (Test-Path -LiteralPath $d) { Start-Sleep -Milliseconds 500 }
        }
    }
    foreach ($r in $script:regRoots) { Remove-Item -LiteralPath $r -Recurse -Force -ErrorAction SilentlyContinue }
    $env:LOCALAPPDATA = $origLocalAppData
    Remove-Item Env:\EMF_TEST_REGISTRY_ROOT, Env:\EMF_FAKE_EMM_FAIL, Env:\EMF_TEST_STEAM_ROOT -ErrorAction SilentlyContinue
}

if ($script:fails -gt 0) { "$($script:fails) check(s) failed"; exit 1 }
"all package checks passed"
exit 0
