# Records or checks SHA-256 hashes of the user's real save files so tests can prove they were not touched.
#   save_guard.ps1 -Record   store current hashes
#   save_guard.ps1 -Check    compare with stored hashes; exit 1 if any changed
param(
    [switch]$Record,
    [switch]$Check,
    [string]$Remote = "",  # the Steam Cloud saves: <Steam>\userdata\<your Steam id>\1203620\remote (found when there is one)
    [string]$Store = (Join-Path $env:USERPROFILE "Documents\enshrouded-save-backups\save_guard.json"),
    [string[]]$Files = @("character0.ksd", "world0.ksd")
)
$ErrorActionPreference = "Stop"
if (-not $Remote) {
    $steam = (Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
    $found = @()
    if ($steam) {
        $found = @(Get-ChildItem -LiteralPath (Join-Path $steam "userdata") -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName "1203620\remote" } | Where-Object { Test-Path -LiteralPath $_ })
    }
    if ($found.Count -ne 1) {
        throw "found $($found.Count) Enshrouded Steam Cloud save folders: pass -Remote <Steam>\userdata\<your Steam id>\1203620\remote"
    }
    $Remote = $found[0]
}
$current = [ordered]@{}
foreach ($f in $Files) {
    $p = Join-Path $Remote $f
    $current[$f] = if (Test-Path $p) { (Get-FileHash $p -Algorithm SHA256).Hash } else { "MISSING" }
}
if ($Record) {
    New-Item -ItemType Directory -Force (Split-Path $Store) | Out-Null
    $current | ConvertTo-Json | Set-Content -Encoding utf8 $Store
    "recorded save hashes:"
    $current.GetEnumerator() | ForEach-Object { "  $($_.Key) $($_.Value)" }
    exit 0
}
if ($Check) {
    if (-not (Test-Path $Store)) { "no stored hashes at $Store (run -Record first)"; exit 2 }
    $old = Get-Content $Store -Raw | ConvertFrom-Json
    $changed = $false
    foreach ($f in $Files) {
        if ($old.$f -ne $current[$f]) { "CHANGED: $f"; $changed = $true } else { "unchanged: $f" }
    }
    if ($changed) { exit 1 } else { exit 0 }
}
"usage: save_guard.ps1 -Record | -Check"
exit 2
