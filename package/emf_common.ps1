# Functions shared by install.ps1 and uninstall.ps1 (dot-sourced; no parameters, so it never changes the caller's).

function Get-SteamRoot {
    if ($env:EMF_TEST_NO_STEAM) { return $null }  # tests: never find the real game
    foreach ($key in @("HKCU:\Software\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam")) {
        $p = Get-ItemProperty $key -ErrorAction SilentlyContinue
        if ($p -and $p.SteamPath) { return $p.SteamPath.Replace("/", "\") }
        if ($p -and $p.InstallPath) { return $p.InstallPath }
    }
    return $null
}

# Every Steam library folder: the Steam folder itself and each "path" in steamapps\libraryfolders.vdf
function Get-SteamLibraries([string]$Root) {
    $libs = @()
    if (-not $Root) { return $libs }
    $libs += $Root
    $vdf = Join-Path $Root "steamapps\libraryfolders.vdf"
    if (Test-Path -LiteralPath $vdf) {
        foreach ($line in Get-Content -LiteralPath $vdf -Encoding UTF8) {  # Steam writes UTF-8 without a BOM
            if ($line -match '^\s*"path"\s+"(.+)"\s*$') { $libs += ($Matches[1] -replace '\\\\', '\') }
        }
    }
    return $libs | Select-Object -Unique
}

# The folder that holds enshrouded.exe (Steam app 1203620), or $null
function Find-EnshroudedDir([string]$SteamRoot = "") {
    if (-not $SteamRoot) { $SteamRoot = Get-SteamRoot }
    foreach ($lib in Get-SteamLibraries $SteamRoot) {
        $acf = Join-Path $lib "steamapps\appmanifest_1203620.acf"
        if (-not (Test-Path -LiteralPath $acf)) { continue }
        $dirName = "Enshrouded"
        foreach ($line in Get-Content -LiteralPath $acf -Encoding UTF8) {
            if ($line -match '^\s*"installdir"\s+"(.+)"\s*$') { $dirName = $Matches[1] }
        }
        $dir = Join-Path $lib "steamapps\common\$dirName"
        if (Test-Path -LiteralPath (Join-Path $dir "enshrouded.exe")) { return $dir }
    }
    return $null
}

# EMF's loader carries this text (src/loader/loader_id.h); "emf_probe" is the developer probe. Plain words such as
# "emf_loader" also appear in tools that merely mention EMF's files.
function Test-EmfLoader([string]$Path) {
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($Path))
    return $text.Contains("EMF-LOADER-7d3c9f") -or $text.Contains("emf_probe")
}

# ---- other mods that want the name dinput8.dll ----
# Only one file can be called dinput8.dll. When another mod's is there (Creative Mode, XHL-Core, ShroudForge, ...),
# EMF renames it, lists the new name in dinput8-chain.ini (the community's chain file, also read by XHL-Core and Auto
# Loot Plus) and its loader starts it before the game runs. emf\moved.txt records what was moved, so uninstall can put
# it back. Nothing EMF moves is ever overwritten or deleted.

$script:ChainFileName = "dinput8-chain.ini"

# emf\known_tools.txt (the table EMF also reads at start-up): "word | name | text:<in its DLL> | beside:<...> |
# same-loader:<another proxy name the same loader uses>" lines.
function Read-KnownTools([string]$Path) {
    $tools = @()
    foreach ($line in @(Read-Lines $Path)) {
        $l = $line.Trim()
        if (-not $l -or $l.StartsWith("#")) { continue }
        $parts = @($l.Split("|") | ForEach-Object { $_.Trim() })
        if ($parts.Count -lt 3 -or -not $parts[0] -or -not $parts[1]) { continue }
        $rest = @($parts | Select-Object -Skip 2)
        $texts = @($rest | Where-Object { $_ -like "text:?*" } | ForEach-Object { $_.Substring(5) })
        $same = @($rest | Where-Object { $_ -like "same-loader:?*" } | ForEach-Object { $_.Substring(12) })
        $tools += , @{ Word = $parts[0].ToLowerInvariant(); Texts = $texts; SameLoader = $same }
    }
    return , $tools
}

# The tool a proxy DLL belongs to, as a short file-name word, or $null when unknown: the first known tool whose text
# the DLL contains (the table's order matters), then the DLL's own version information.
function Get-DllToolName([string]$Path, $Tools = @()) {
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($Path))
    if ($text.Contains("EMF-LOADER-7d3c9f") -or $text.Contains("emf_probe")) { return "emf" }
    foreach ($t in $Tools) {
        foreach ($m in $t.Texts) {
            if ($text.IndexOf($m, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) { return $t.Word }
        }
    }
    $info = (Get-Item -LiteralPath $Path).VersionInfo
    foreach ($v in @($info.ProductName, $info.FileDescription, $info.OriginalFilename)) {
        if (-not $v -or $v -match "Microsoft") { continue }  # some proxies copy the real dinput8.dll's information
        $slug = (($v -replace "\.dll$", "").ToLowerInvariant() -replace "[^a-z0-9]+", "-").Trim("-")
        if ($slug.Length -gt 40) { $slug = $slug.Substring(0, 40).Trim("-") }
        if ($slug -and $slug -ne "dinput8") { return $slug }
    }
    return $null
}

# What to do with another mod's dinput8.dll:
#   Name       its new name: <tool>.dll (or <tool>-2.dll ...), dinput8-previous.dll for an unknown one
#   Identical  a file with the same bytes is already there under Name (no second copy is kept)
#   RunsAs     the other proxy name through which the same loader already runs (known_tools.txt same-loader): the
#              moved copy is kept aside, not started
#   SwitchOff  listed older copies of the same known tool: switched off so only the newest runs
#   Earlier    listed earlier unknown copies: left on, the player is told
function Get-MovePlan([string]$Dir, [string]$Proxy, $Tools = @()) {
    $tool = Get-DllToolName $Proxy $Tools
    $hash = (Get-FileHash -LiteralPath $Proxy).Hash
    $listed = @(Get-ChainEntries $Dir)
    $plan = @{ Tool = $tool; Name = $null; Identical = $false; RunsAs = $null; SwitchOff = @(); Earlier = @() }
    $base = if ($tool) { $tool } else { "dinput8-previous" }
    if (Test-ReservedDllName $base) { $base = "$base-mod" }  # Windows would load a dxgi.dll or winmm.dll as itself
    for ($i = 1; $i -lt 100 -and -not $plan.Name; $i++) {
        $name = if ($i -eq 1) { "$base.dll" } else { "$base-$i.dll" }
        $at = Join-Path $Dir $name
        if (-not (Test-Path -LiteralPath $at)) { $plan.Name = $name; break }
        if ((Get-FileHash -LiteralPath $at).Hash -eq $hash) { $plan.Name = $name; $plan.Identical = $true; break }
        if ($listed -contains $name) {
            if ($tool) { $plan.SwitchOff += $name } else { $plan.Earlier += $name }
        }
    }
    if (-not $plan.Name) { throw "too many earlier copies of $base in $Dir" }
    foreach ($t in $Tools) {
        if ($t.Word -ne $tool) { continue }
        foreach ($other in $t.SameLoader) {
            $at = Join-Path $Dir $other
            if ((Test-Path -LiteralPath $at) -and (Get-DllToolName $at $Tools) -eq $tool) { $plan.RunsAs = $other }
        }
    }
    return $plan
}

# A DLL name that must not be used for a renamed mod: a proxy name mods use, or a DLL Windows has in System32 (the game
# or Windows would load the renamed mod in its place).
function Test-ReservedDllName([string]$Base) {
    $proxies = @("dinput8", "dbghelp", "dbgcore", "winmm", "powrprof", "version", "xinput1_4", "dxgi", "umpdc", "cfgmgr32")
    if ($proxies -contains $Base.ToLowerInvariant()) { return $true }
    return Test-Path -LiteralPath (Join-Path ([Environment]::SystemDirectory) "$Base.dll")
}

function Read-Lines([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    return @([System.IO.File]::ReadAllLines($Path))
}

function Write-Lines([string]$Path, [string[]]$Lines) {
    [System.IO.File]::WriteAllText($Path, (($Lines -join "`r`n") + "`r`n"), (New-Object System.Text.UTF8Encoding $false))
}

# The chain file as lines plus the encoding it is written back in: UTF-8 with or without BOM, UTF-16 with BOM, or
# the ANSI code page when the bytes are not UTF-8 (a player's own file keeps its bytes).
function Read-ChainFile([string]$Dir) {
    $path = Join-Path $Dir $script:ChainFileName
    if (-not (Test-Path -LiteralPath $path)) { return @{ Path = $path; Lines = @(); Encoding = (New-Object System.Text.UTF8Encoding $false); Exists = $false } }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $enc = New-Object System.Text.UTF8Encoding $true; $text = $enc.GetString($bytes, 3, $bytes.Length - 3)
    } elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
        $enc = New-Object System.Text.UnicodeEncoding $false, $true; $text = $enc.GetString($bytes, 2, $bytes.Length - 2)
    } else {
        try { $enc = New-Object System.Text.UTF8Encoding $false, $true; $text = $enc.GetString($bytes) }
        catch { $enc = [System.Text.Encoding]::Default; $text = $enc.GetString($bytes) }
    }
    $lines = @($text -split "`r?`n")
    if ($lines.Count -and $lines[-1] -eq "") { $lines = @($lines | Select-Object -First ($lines.Count - 1)) }
    return @{ Path = $path; Lines = $lines; Encoding = $enc; Exists = $true }
}

function Write-ChainFile($File, [string[]]$Lines) {
    [System.IO.File]::WriteAllText($File.Path, (($Lines -join "`r`n") + "`r`n"), $File.Encoding)
}

# Indexes of the lines that count: lines before any section and lines in [Chain] (the loader reads only those).
function Get-ChainLineIndexes([string[]]$Lines) {
    $in = $true
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $t = $Lines[$i].Trim()
        if ($t.StartsWith("[")) { $in = ($t -ieq "[Chain]"); continue }
        if ($in) { $i }
    }
}

# The DLL names the chain file starts (switched-off lines, starting with ';', are not included).
function Get-ChainEntries([string]$Dir) {
    $f = Read-ChainFile $Dir
    foreach ($i in @(Get-ChainLineIndexes $f.Lines)) {
        $t = $f.Lines[$i].Trim()
        if ($t -and -not $t.StartsWith(";") -and -not $t.StartsWith("#") -and $t -match "\.dll$") { $t }
    }
}

# Adds $Name inside [Chain] (once; a switched-off line for it is switched on again). Returns @{ Added; Created }.
function Add-ChainEntry([string]$Dir, [string]$Name) {
    $f = Read-ChainFile $Dir
    $lines = @($f.Lines)
    if (-not $f.Exists) {
        Write-ChainFile $f @("[Chain]", $Name)  # XHL-Core documents the section this way
        return @{ Added = $true; Created = $true }
    }
    $idx = @(Get-ChainLineIndexes $lines)
    foreach ($i in $idx) {
        $t = $lines[$i].Trim()
        if ($t -ieq $Name) { return @{ Added = $false; Created = $false } }
        if ($t.StartsWith(";") -and $t.TrimStart(";").Trim() -ieq $Name) {
            $lines[$i] = $Name
            Write-ChainFile $f $lines
            return @{ Added = $true; Created = $false }
        }
    }
    $header = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim() -ieq "[Chain]") { $header = $i } }
    if ($header -lt 0 -and @($lines | Where-Object { $_.Trim().StartsWith("[") }).Count -eq 0) {
        $at = $lines.Count  # no sections at all: the whole file counts
    } elseif ($header -lt 0) {
        $lines += @("", "[Chain]"); $at = $lines.Count
    } else {
        $at = $header + 1
        while ($at -lt $lines.Count -and -not $lines[$at].Trim().StartsWith("[")) { $at++ }
        while ($at -gt $header + 1 -and $lines[$at - 1].Trim() -eq "") { $at-- }  # before the blank lines of the section
    }
    $before = @(); if ($at -gt 0) { $before = @($lines[0..($at - 1)]) }
    $after = @(); if ($at -lt $lines.Count) { $after = @($lines[$at..($lines.Count - 1)]) }
    Write-ChainFile $f (@($before) + $Name + @($after))
    return @{ Added = $true; Created = $false }
}

# Switches $Name off (';' in front) or on again in the chain file.
function Set-ChainEntryOn([string]$Dir, [string]$Name, [bool]$On) {
    $f = Read-ChainFile $Dir
    if (-not $f.Exists) { return }
    $lines = @($f.Lines)
    foreach ($i in @(Get-ChainLineIndexes $lines)) {
        $t = $lines[$i].Trim()
        if (-not $On -and $t -ieq $Name) { $lines[$i] = "; $Name" }
        if ($On -and $t.StartsWith(";") -and $t.TrimStart(";").Trim() -ieq $Name) { $lines[$i] = $Name }
    }
    Write-ChainFile $f $lines
}

# Takes $Name (on or switched off) out of the chain file; with $DeleteIfEmpty also removes the file once it lists no
# DLL.
function Remove-ChainEntry([string]$Dir, [string]$Name, [bool]$DeleteIfEmpty) {
    $f = Read-ChainFile $Dir
    if (-not $f.Exists) { return }
    $drop = @(Get-ChainLineIndexes $f.Lines | Where-Object {
            $t = $f.Lines[$_].Trim(); ($t -ieq $Name) -or ($t.StartsWith(";") -and $t.TrimStart(";").Trim() -ieq $Name) })
    $left = @(for ($i = 0; $i -lt $f.Lines.Count; $i++) { if ($drop -notcontains $i) { $f.Lines[$i] } })
    $entries = @($left | Where-Object { $_.Trim() -match "\.dll$" })
    if ($DeleteIfEmpty -and $entries.Count -eq 0) { Remove-Item -LiteralPath $f.Path -Force; return }
    Write-ChainFile $f $left
}

# A folder EMF installs into: the game's (enshrouded.exe) or a dedicated server's (enshrouded_server.exe).
function Test-GameFolder([string]$Dir) {
    return (Test-Path -LiteralPath (Join-Path $Dir "enshrouded.exe")) -or
           (Test-Path -LiteralPath (Join-Path $Dir "enshrouded_server.exe"))
}

# Why EMF must not change $Dir now, or $null: the game, a server or EMF's manager (emf\EMF.exe, which holds its
# files open) runs from that folder. A game process whose path cannot be read counts as running from it: when the game
# runs as administrator and this script does not, Windows gives no path (and no error), and EMF cannot tell which
# folder it runs from. (Another program that happens to be called EMF does not count.) `Processes` is for the tests.
function Get-GameRunning([string]$Dir, $Processes = $null) {
    if ($null -eq $Processes) { $Processes = @(Get-Process enshrouded, enshrouded_server, EMF -ErrorAction SilentlyContinue) }
    $full = [System.IO.Path]::GetFullPath($Dir).TrimEnd("\") + "\"
    foreach ($p in $Processes) {
        $path = $null
        try { $path = $p.Path } catch { }
        $what = "$($p.Name).exe (process $($p.Id))"
        if (-not $path -and $p.Name -eq "EMF") { continue }
        if (-not $path) {
            return "$what is running and Windows does not show this script its folder (it probably runs as " +
                   "administrator), so it may be the one in $Dir. Close it first (or run this as administrator)."
        }
        if ($path.StartsWith($full, [System.StringComparison]::OrdinalIgnoreCase)) {
            return "$what is running from $Dir; close it first"
        }
    }
    return $null
}

# ---- save backups (docs/saves.md) ----
# The same backups EMF.exe and EMF's core make (src/saves/backup.h): <store>\<yyyy-MM-dd_HH-mm-ss>_<kind>\ with
# backup.json and the files under steam\<id>\, local\default\ and local\. Written into a .tmp- folder first, each copy
# checked against the hash of what was read, then renamed into place. Nothing is ever written into a save folder.

# Where the saves and the backups are: @{ Steam; Local; Store; Info } ($null for a place that is not there). Tests name
# fake places with EMF_TEST_STEAM_ROOT, EMF_TEST_SAVED_GAMES and EMF_TEST_APPDATA; a test run (EMF_TEST_NO_STEAM set)
# never uses a real save folder.
function Get-SaveRoots {
    $test = [bool]$env:EMF_TEST_NO_STEAM
    $steam = if ($env:EMF_TEST_STEAM_ROOT) { $env:EMF_TEST_STEAM_ROOT } elseif ($test) { $null } else { Get-SteamRoot }
    $saved = if ($env:EMF_TEST_SAVED_GAMES) { $env:EMF_TEST_SAVED_GAMES } elseif ($test) { $null } else { Get-SavedGamesFolder }
    $app = if ($env:EMF_TEST_APPDATA) { $env:EMF_TEST_APPDATA } elseif ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA "EMF" } else { $null }
    return @{
        Steam = $steam
        Local = $(if ($saved) { Join-Path $saved "keen games\Enshrouded" } else { $null })
        Store = $(if ($app) { Join-Path $app "save-backups" } else { $null })
        Info  = $(if ($app) { Join-Path $app "save-info.json" } else { $null })
    }
}

# The Saved Games known folder (Windows lets a player move it), or $null.
function Get-SavedGamesFolder {
    $key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders"
    $p = (Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue).'{4C5C32FF-BB9D-43b0-B5B4-2D72E54EAAA4}'
    if ($p) { return [Environment]::ExpandEnvironmentVariables($p) }
    if ($env:USERPROFILE) { return Join-Path $env:USERPROFILE "Saved Games" }
    return $null
}

# @{ Type; Slot } for a save file name the game uses in a storage folder, or $null.
function Get-SaveName([string]$Name) {
    $n = $Name.ToLowerInvariant()
    if ($n -eq "global.ksd") { return @{ Type = "global"; Slot = -1; Order = 2 } }
    if ($n -eq "user_settings.json") { return @{ Type = "user_settings"; Slot = -1; Order = 3 } }
    if ($n -match '^(character|world)(\d{1,3})\.ksd$') {
        return @{ Type = $Matches[1]; Slot = [int]$Matches[2]; Order = $(if ($Matches[1] -eq "character") { 0 } else { 1 }) }
    }
    return $null
}

function Get-StorageSaves([string]$Dir, [string]$Storage, [string]$SteamId, [string]$Prefix) {
    $found = @()
    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { return $found }
    foreach ($f in Get-ChildItem -LiteralPath $Dir -File -Force) {
        $sn = Get-SaveName $f.Name
        if (-not $sn) { continue }
        $found += [pscustomobject]@{ Storage = $Storage; SteamId = $SteamId; Type = $sn.Type; Slot = $sn.Slot; Order = $sn.Order
                                     Name = $f.Name; Path = $Prefix + $f.Name; Source = $f.FullName }
    }
    return @($found | Sort-Object Order, Slot, Name)
}

# The Steam user folders (userdata\<digits>) that have an Enshrouded remote folder, in id order.
function Get-SteamSaveUsers($Roots) {
    if (-not $Roots.Steam) { return @() }
    $ud = Join-Path $Roots.Steam "userdata"
    if (-not (Test-Path -LiteralPath $ud -PathType Container)) { return @() }
    return @(Get-ChildItem -LiteralPath $ud -Directory | Where-Object {
            $_.Name -match '^\d+$' -and (Test-Path -LiteralPath (Join-Path $_.FullName "1203620\remote") -PathType Container) } |
        Sort-Object { $_.Name.Length }, Name | ForEach-Object { $_.Name })
}

# Every save file: Steam users first, then Local.
function Find-SaveFiles($Roots) {
    $files = @()
    foreach ($id in @(Get-SteamSaveUsers $Roots)) {
        $files += @(Get-StorageSaves (Join-Path $Roots.Steam "userdata\$id\1203620\remote") "steam" $id "steam/$id/")
    }
    if ($Roots.Local) {
        $files += @(Get-StorageSaves (Join-Path $Roots.Local "default") "local" "" "local/default/")
        $ls = Join-Path $Roots.Local "local_settings.json"
        if (Test-Path -LiteralPath $ls -PathType Leaf) {
            $files += [pscustomobject]@{ Storage = "local"; SteamId = ""; Type = "local_settings"; Slot = -1; Order = 4
                                         Name = "local_settings.json"; Path = "local/local_settings.json"; Source = $ls }
        }
    }
    return $files
}

# Copies one save file, reading it with every share flag (the game may write it meanwhile); a file that is locked or
# changes while it is read is tried once more. @{ Ok; Sha; Size; Modified } or @{ Ok = $false; Why }.
function Copy-SaveFileChecked([string]$Src, [string]$Dst, [int]$RetryMs = 250) {
    $why = "it could not be read"
    for ($attempt = 0; $attempt -lt 2; $attempt++) {
        if ($attempt) { Start-Sleep -Milliseconds $RetryMs }
        $in = $null; $out = $null; $sha = $null
        try {
            $in = [System.IO.File]::Open($Src, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                                         [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
        } catch {
            $why = if (Test-Path -LiteralPath $Src) { "it is in use by another program" } else { "it was removed while EMF copied it" }
            continue
        }
        # the whole file with one read and the handle closed at once: Windows refuses to rename a new save over a file
        # that is open, so the game's or Steam's write must never wait for this copy
        try {
            $len1 = $in.Length
            $t1 = [System.IO.File]::GetLastWriteTimeUtc($Src)
            $data = New-Object byte[] ($len1 + 1)
            $total = 0
            while ($total -lt $data.Length) {
                $n = $in.Read($data, $total, $data.Length - $total)
                if ($n -le 0) { break }
                $total += $n
            }
            $len2 = $in.Length
            $in.Dispose(); $in = $null
            $t2 = [System.IO.File]::GetLastWriteTimeUtc($Src)
            if ($total -eq $len2 -and $len1 -eq $len2) {
                $sha = [System.Security.Cryptography.SHA256]::Create()
                $hex = -join ($sha.ComputeHash($data, 0, $total) | ForEach-Object { $_.ToString("x2") })
                $out = [System.IO.File]::Open($Dst, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
                $out.Write($data, 0, $total)
                $out.Flush($true)
            }
        } catch {
            $why = "reading or writing it failed: $($_.Exception.Message)"
            continue
        } finally {
            if ($out) { $out.Dispose() }
            if ($in) { $in.Dispose() }
            if ($sha) { $sha.Dispose() }
        }
        if ($len1 -ne $len2 -or $t1 -ne $t2 -or $total -ne $len2) { $why = "it changed while EMF copied it (the game was probably saving)"; continue }
        if ((Get-FileHash -LiteralPath $Dst -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hex) { $why = "the copy did not match the file"; continue }
        return @{ Ok = $true; Sha = $hex; Size = [int64]$total; Modified = [int64](([DateTimeOffset]$t2).ToUnixTimeSeconds()) }
    }
    Remove-Item -LiteralPath $Dst -Force -ErrorAction SilentlyContinue
    return @{ Ok = $false; Why = $why }
}

# The backup folders in the store with their manifests: @( @{ Id; Folder; Manifest } ), damaged ones left out.
function Get-SaveBackups([string]$Store) {
    $list = @()
    if (-not $Store -or -not (Test-Path -LiteralPath $Store -PathType Container)) { return $list }
    foreach ($d in Get-ChildItem -LiteralPath $Store -Directory) {
        if ($d.Name.StartsWith(".")) { continue }
        $json = Join-Path $d.FullName "backup.json"
        if (-not (Test-Path -LiteralPath $json -PathType Leaf)) { continue }
        try { $m = [System.IO.File]::ReadAllText($json) | ConvertFrom-Json } catch { continue }
        if ($m.kind) { $list += @{ Id = $d.Name; Folder = $d.FullName; Manifest = $m } }
    }
    return $list
}

# A starter backup is there (also one with problems: later chances are automatic backups at game start, never a second
# "Before EMF").
function Test-StarterBackup([string]$Store) {
    return @(Get-SaveBackups $Store | Where-Object { $_.Manifest.kind -eq "starter" }).Count -gt 0
}

# The named mutex every EMF program takes while it writes into the store (src/saves/backup.cpp store_lock_name): FNV-1a
# over the path's UTF-16 code units with A-Z lowered.
function Get-SaveStoreMutexName([string]$Store) {
    [uint32]$h = 2166136261
    foreach ($ch in $Store.ToCharArray()) {
        $c = [int]$ch
        if ($c -ge 65 -and $c -le 90) { $c += 32 }
        $h = [uint32]((([uint64]($h -bxor [uint32]$c)) * [uint64]16777619) % [uint64]4294967296)
    }
    return "Local\EMF.SaveBackups." + $h.ToString("x8")
}

# Makes one backup. @{ Ok; Nothing; Error; Id; Folder; Files; Bytes; Problems }.
function New-SaveBackup($Roots, [string]$Kind, [string]$Label, [string]$MadeBy, [string]$Version, [int]$RetryMs = 250) {
    $r = @{ Ok = $false; Nothing = $false; Error = $null; Id = $null; Folder = $null; Files = 0; Bytes = [int64]0; Problems = @() }
    if (-not $Roots.Store) { $r.Error = "EMF has no folder for backups (LOCALAPPDATA is not set)"; return $r }
    $files = @(Find-SaveFiles $Roots)
    if ($files.Count -eq 0) { $r.Nothing = $true; $r.Error = "there are no save files to back up"; return $r }
    $now = [DateTimeOffset]::Now
    $tmp = Join-Path $Roots.Store (".tmp-" + $now.ToUnixTimeSeconds() + "-" + $PID + "-" + [guid]::NewGuid().ToString("N").Substring(0, 6))
    # the same lock as EMF.exe and the game: never two EMF programs writing into the store at once
    $mutex = New-Object System.Threading.Mutex($false, (Get-SaveStoreMutexName $Roots.Store))
    $owned = $false
    try {
        try { $owned = $mutex.WaitOne(120000) } catch [System.Threading.AbandonedMutexException] { $owned = $true }
        if (-not $owned) { throw "another EMF program (EMF.exe or the game) is working on the save backups right now; try again in a moment" }
        New-Item -ItemType Directory -Force $tmp | Out-Null
        $entries = @()
        $ids = @()
        foreach ($f in $files) {
            $dst = Join-Path $tmp ($f.Path -replace '/', '\')
            New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
            $c = Copy-SaveFileChecked $f.Source $dst $RetryMs
            $storageName = if ($f.Storage -eq "steam") { "Steam" } else { "Local" }
            if (-not $c.Ok) { $r.Problems += "$($f.Name) ($storageName): $($c.Why)"; continue }
            $e = [ordered]@{ storage = $f.Storage }
            if ($f.Storage -eq "steam") { $e.steam_id = $f.SteamId; $ids += $f.SteamId }
            $e.type = $f.Type; $e.slot = $f.Slot; $e.name = $f.Name; $e.path = $f.Path
            $e.size = $c.Size; $e.sha256 = $c.Sha; $e.modified = $c.Modified
            $entries += $e
            $r.Bytes += $c.Size
        }
        if ($entries.Count -eq 0) { throw "no save file could be copied: $($r.Problems -join '; ')" }
        # the names and levels EMF's core saw in the game's menu, for the slots in this backup. A Steam name goes with the
        # account it was seen for; one without an account only when one Steam user's files are in the backup.
        $info = @()
        if ($Roots.Info -and (Test-Path -LiteralPath $Roots.Info -PathType Leaf)) {
            try {
                $si = [System.IO.File]::ReadAllText($Roots.Info) | ConvertFrom-Json
                $oneSteam = @($ids | Select-Object -Unique).Count -eq 1
                foreach ($s in @($si.slots)) {
                    if (-not $s) { continue }
                    $account = "$($s.steam_id)"
                    if ($s.storage -eq "steam" -and -not $account -and -not $oneSteam) { continue }
                    $match = @($entries | Where-Object { $_.storage -eq $s.storage -and $_.type -eq $s.type -and $_.slot -eq $s.slot -and
                                                         ($_.storage -ne "steam" -or -not $account -or $_.steam_id -eq $account) })
                    if ($match.Count) { $info += $s }
                }
            } catch { }
        }
        $manifest = [ordered]@{
            format = 1; kind = $Kind; label = $Label; time = [int64]$now.ToUnixTimeSeconds()
            time_local = $now.ToString("yyyy-MM-dd HH:mm:ss", [cultureinfo]::InvariantCulture); emf_version = $Version; made_by = $MadeBy
            files = @($entries); info = @($info); problems = @($r.Problems)
        }
        [System.IO.File]::WriteAllText((Join-Path $tmp "backup.json"), ($manifest | ConvertTo-Json -Depth 6),
                                       (New-Object System.Text.UTF8Encoding $false))
        $base = $now.ToString("yyyy-MM-dd_HH-mm-ss", [cultureinfo]::InvariantCulture) + "_" + $Kind
        for ($n = 1; $n -lt 1000 -and -not $r.Id; $n++) {
            $id = if ($n -eq 1) { $base } else { "${base}_$n" }
            $final = Join-Path $Roots.Store $id
            if (Test-Path -LiteralPath $final) { continue }
            # Directory.Move fails when the name was taken meanwhile (Move-Item would move the folder into it)
            try { [System.IO.Directory]::Move($tmp, $final) } catch [System.IO.IOException] { continue }
            $r.Id = $id; $r.Folder = $final
        }
        if (-not $r.Id) { throw "no free name for the backup" }
        $r.Ok = $true
        $r.Files = $entries.Count
    } catch {
        $r.Error = $_.Exception.Message
    } finally {
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
        if ($owned) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
    return $r
}

# "0.4 MB" / "130 KB"
function Format-Size([int64]$Bytes) {
    if ($Bytes -ge 1MB) { return [string]::Format([cultureinfo]::InvariantCulture, "{0:0.0} MB", $Bytes / 1MB) }
    if ($Bytes -ge 10KB) { return [string]::Format([cultureinfo]::InvariantCulture, "{0:0} KB", $Bytes / 1KB) }
    return [string]::Format([cultureinfo]::InvariantCulture, "{0:0.#} KB", $Bytes / 1KB)
}
