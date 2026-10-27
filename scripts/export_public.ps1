# Makes the public copy of EMF: the committed tree of a branch (main by default), without the folders and files that
# hold private run logs and internal notes, checked for personal data, as a new git repository with no history and no
# commit. You then commit it under the e-mail you want public (for example GitHub's noreply address) and push it.
# Nothing here pushes, and the private repository is not changed.
#   export_public.ps1 -Out <new folder> [-Ref main]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Ref = "main",
    [switch]$NoGitInit  # tests: only the files and the check
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

# Never published (folders and single files, relative to the repository): run logs and screenshots (local paths,
# account ids, desktop captures), development plans and reports, scratch folders, feature and market research
# (docs/research, RESEARCH.md), the Nexus page drafts and release notes (docs/release, docs/nexus), the e-mails to
# Nexus (docs/nexus-registration), the design history (docs/FRAMEWORK_DESIGN.md) and throwaway test mods
# (tools/spikes). The public build does not need any of them: CMakeLists.txt builds the tools/spikes targets only when
# that folder exists, and the tests that read docs/release skip without it.
$excluded = @("analysis", "docs/superpowers", ".superpowers", ".playwright-mcp",
    "docs/research", "docs/release", "docs/nexus", "docs/nexus-registration", "docs/FRAMEWORK_DESIGN.md",
    "RESEARCH.md", "tools/spikes")
# Personal data that must not appear in any published text file. A hit stops the export. Only generic patterns are
# written here: this script is published too. Your own strings (account ids, e-mail, names) go in a local file
# that is never published: <the repository's .git folder>\info\emf-export-private.txt, one rule per line as
# 'what<TAB>regex' (lines starting with # are notes). Without that file only the generic checks run, and it says so.
$forbidden = @(
    @{ What = "a Windows user folder"; Pattern = "(?i)C:\\Users\\(?!Public\\|<|%|Default)[A-Za-z0-9._-]+\\" },
    @{ What = "a home network address"; Pattern = "\b192\.168\.\d{1,3}\.\d{1,3}\b" }
)
$gitCommon = (& git -C $repo rev-parse --path-format=absolute --git-common-dir 2>$null)
$privateRules = if ($gitCommon) { Join-Path $gitCommon "info\emf-export-private.txt" } else { "" }
if ($privateRules -and (Test-Path -LiteralPath $privateRules)) {
    foreach ($line in [System.IO.File]::ReadAllLines($privateRules)) {
        if (-not $line.Trim() -or $line.TrimStart().StartsWith("#")) { continue }
        $parts = $line.Split("`t", 2)
        if ($parts.Count -ne 2 -or -not $parts[1]) { throw "$privateRules has a line that is not 'what<TAB>regex': $line" }
        $forbidden += @{ What = $parts[0]; Pattern = $parts[1] }
    }
    "checking for personal data: $($forbidden.Count) rules, $($forbidden.Count - 2) of them from $privateRules"
} else {
    "note: no $privateRules, so only the generic checks run (a user folder, a home network address). Put your own"
    "strings there (one 'what<TAB>regex' per line) to have them checked too."
}

if (Test-Path -LiteralPath $Out) {
    if (@(Get-ChildItem -LiteralPath $Out -Force).Count -gt 0) { throw "$Out is not empty: give a new folder" }
} else {
    New-Item -ItemType Directory -Force $Out | Out-Null
}
$Out = (Resolve-Path -LiteralPath $Out).Path
if ($Out.StartsWith($repo, [System.StringComparison]::OrdinalIgnoreCase)) { throw "the public copy must be outside the repository" }

# the committed tree only (no untracked or ignored files), through git archive and Windows' tar
$tar = Join-Path ([System.IO.Path]::GetTempPath()) ("emf-public-" + [guid]::NewGuid().ToString("N") + ".tar")
try {
    & git -C $repo archive --format=tar -o $tar $Ref
    if ($LASTEXITCODE -ne 0) { throw "git archive $Ref failed" }
    & "$env:SystemRoot\System32\tar.exe" -xf $tar -C $Out
    if ($LASTEXITCODE -ne 0) { throw "unpacking the archive failed" }
} finally {
    Remove-Item -LiteralPath $tar -Force -ErrorAction SilentlyContinue
}
foreach ($e in $excluded) {
    $p = Join-Path $Out $e
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }  # a folder or a single file
    if (Test-Path -LiteralPath $p) { throw "$e is still in the public copy after removing it" }
}

# the check: every text file (binary files such as pictures are skipped: their text cannot be read this way, so
# screenshots must be made without personal paths on screen)
$hits = New-Object System.Collections.Generic.List[string]
foreach ($f in Get-ChildItem -LiteralPath $Out -Recurse -File) {
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    if ($bytes.Length -gt 0 -and [Array]::IndexOf($bytes, [byte]0) -ge 0) { continue }  # binary
    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    foreach ($rule in $forbidden) {
        $m = [regex]::Match($text, $rule.Pattern)
        if ($m.Success) {
            $line = ($text.Substring(0, $m.Index) -split "`n").Count
            $hits.Add("$($f.FullName.Substring($Out.Length + 1)):${line}: $($rule.What) ($($m.Value))")
        }
    }
}
if ($hits.Count -gt 0) {
    "the public copy holds personal data; nothing was published, fix these in the repository and export again:"
    $hits | ForEach-Object { "  $_" }
    exit 1
}

if (-not $NoGitInit) {
    & git -C $Out init -q -b main
    if ($LASTEXITCODE -ne 0) { throw "git init failed" }
}
$files = @(Get-ChildItem -LiteralPath $Out -Recurse -File).Count
"public copy of $Ref in $Out ($files files), checked: no personal data found"
if (-not $NoGitInit) {
    "next, in that folder:"
    "  git config user.name  ""<the name you want public>"""
    "  git config user.email ""<your GitHub noreply address>"""
    "  git add -A"
    "  git commit -m ""EMF <version>"""
    "  git remote add origin https://github.com/<you>/<repo>.git"
    "  git push -u origin main"
}
