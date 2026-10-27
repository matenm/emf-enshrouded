# Smoke test for EMF.exe (the desktop mod manager, docs/app.md). It builds a fake game folder in %TEMP% (an
# enshrouded.exe stub, EMF's loader and core stubs, the clock, lua and life_skills mods, the EML content example, a DLL
# listed in dinput8-chain.ini with its emf\chain_status.txt, and a canary mod whose texts are HTML and script), runs
# EMF.exe --game <it> --smoke <out.json> and checks, from what the page drew:
# - the mod list (the titles are read back from the rows on the page);
# - the Lua starter's settings (template_lua): group headings, a slider with its step, a list, a colour, text with its
#   length limit and a key bind; switching "Show the line" off greys out the settings that need it, and a key bind
#   takes the next key press; both are written to the mod's ini;
# - the canary's texts show as text, and nothing in them runs;
# - the page cannot reach the network or leave https://emf.app/ (a fetch and a navigation from the page);
# - clicking the chained DLL's switch comments out its line in dinput8-chain.ini (page script -> host -> file);
# - with a stand-in enshrouded.exe running from the folder, that switch is locked and the file stays as it is;
# - mod profiles through the page: Save as writes emf\profiles\<name>.json and the picker shows it; a switch makes
#   it say "(changed)"; Switch puts the switch back (dinput8-chain.ini as it was);
# - the Browse tab, offline from recorded Nexus replies (--nexus-fixture tests\fixtures\nexus_app): the cards, their
#   pictures (answered by the host from the fixture), the "installed" badge, a mod's files with what each one is and
#   the note for a file Nexus has no listing of, the description in its own container; pictures from other hosts and
#   fetches stay blocked (the CSP and the host's request filter);
# - "Check for updates" (offline, the recorded reply in tests\fixtures\nexus_app\updates.json): nothing before the
#   click; then the summary line, an "Update" badge on a mod with an older version, "Update?" on one whose version EMF
#   does not know, and the details naming the file to take; "Link to a Nexus mod..." on a DLL: typing survives a redraw
#   of the details, Enter saves the link; with the Premium key, "Update with EMF" with the file's changelog opens the plan
#   of the same install;
# - "Install a downloaded file" through the page (a zip made here; install_path is the test's stand-in for the file
#   dialog): the plan, Install, the files and the dinput8-chain.ini line; then Uninstall takes them out again;
# - the Nexus account (offline, recorded replies): the EMF tab's account card says the key is "not set"; the nxm://
#   setting shows the current handler and switches in a registry in memory (the real HKCU is never written); a key
#   saved through the page is checked, stored encrypted (never in the page) and shows the account as Premium; "Download
#   with EMF" on a Browse file downloads it from the recorded CDN into the downloads folder and opens the plan with its
#   Nexus mod and file ids; an nxm:// link for another game is refused, and one for Enshrouded waits for a key; a
#   second EMF.exe started with a link hands it to the running one (WM_COPYDATA, with a window class of the test's own)
#   and exits;
# - Nexus only with the tester switch ([nexus] personal_key=1 in app.ini; docs/app.md "Nexus Mods"): every run above
#   has it, like the testing build for Nexus. One run is the public build without it: no Browse tab, no Check for
#   updates, no account or nxm:// cards, no Download with EMF; a Mod Manager Download link is answered with how to
#   download the file by hand; the host refuses the Nexus commands a page script sends; a key stored by a testing build
#   stays; and EMF.exe makes no Nexus request at all (out.json's nexus_requests, which the Browse run shows counting);
# - a dedicated server folder: added through the folder switcher, the Server tab without any password, chat
#   settings saved through the page, Start and Stop against a stand-in enshrouded_server.exe (clean stop on
#   Ctrl+C), the server's log with a password stripped, Install EMF with a package's own install.ps1, and a
#   client mod flagged on the server's Mods tab;
# - the Saves tab against fake save folders: the starter backup at the first start and the list, Back up now, restoring
#   one Local file (the Steam Cloud warning for a Steam file), Delete cancelled then confirmed, and the refusal while a
#   stand-in game runs;
# - without the WebView2 runtime EMF.exe says so and exits 1;
# - EMF.exe imports none of the proxy DLL names mods put next to the game.
# The runs are off-screen and never take the focus. Never touches the real game folder (--game, EMF_TEST_NO_STEAM) or
# the player's app data (a smoke run keeps WebView2's data in %TEMP%\emf-smoke-webview-<pid>). Smoke tests of other
# worktrees may run at the same time: EMF.exe gets a %TEMP% of this run's own, and every folder and name is this run's.
# Exit code 0 = passed.
#   app_smoke.ps1 [-Build build\msvc] [-Capture <png>] [-Select emf:clock] [-Page mods|warnings|emf] [-Keep]
#                 [-BrowseCapture <png>]   (a picture of the Browse tab with the recorded replies: docs\images\app-browse.png)
param(
    [string]$Build = (Join-Path $PSScriptRoot "..\build\msvc"),
    [string]$Capture = "",
    [string]$Select = "",
    [string]$Page = "",
    [string]$BrowseCapture = "",
    [switch]$Keep
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$exe = Join-Path $Build "app\EMF.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "missing $exe (build first: scripts\test.sh)" }
$exe = (Resolve-Path -LiteralPath $exe).Path
$env:EMF_TEST_NO_STEAM = "1"
$env:EMF_TEST = "1"  # allows --eval
$script:fails = 0
function Check([bool]$cond, [string]$what) {
    if ($cond) { "ok   $what" } else { "FAIL $what"; $script:fails++ }
}

# ---- EMF.exe imports no proxy DLL name (EML's proxy, for one, runs its patcher in any process that loads it) ----
function Get-PeImports([string]$Path) {
    $b = [System.IO.File]::ReadAllBytes($Path)
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    $sections = [BitConverter]::ToUInt16($b, $pe + 6)
    $optSize = [BitConverter]::ToUInt16($b, $pe + 20)
    $opt = $pe + 24
    $importRva = [BitConverter]::ToUInt32($b, $opt + 112 + 8)  # PE32+ data directory 1
    $delayRva = [BitConverter]::ToUInt32($b, $opt + 112 + 13 * 8)
    $secTable = $opt + $optSize
    $toOffset = {
        param([uint32]$rva)
        for ($i = 0; $i -lt $sections; $i++) {
            $s = $secTable + 40 * $i
            $va = [BitConverter]::ToUInt32($b, $s + 12); $size = [BitConverter]::ToUInt32($b, $s + 8)
            $raw = [BitConverter]::ToUInt32($b, $s + 20)
            if ($rva -ge $va -and $rva -lt $va + [Math]::Max($size, [BitConverter]::ToUInt32($b, $s + 16))) { return $raw + ($rva - $va) }
        }
        throw "RVA $rva is in no section"
    }
    $readName = {
        param([uint32]$rva)
        $o = & $toOffset $rva; $e = $o
        while ($b[$e] -ne 0) { $e++ }
        return [System.Text.Encoding]::ASCII.GetString($b, $o, $e - $o)
    }
    $names = @()
    if ($importRva) {
        $d = & $toOffset $importRva
        while ([BitConverter]::ToUInt32($b, $d + 12) -ne 0) { $names += & $readName ([BitConverter]::ToUInt32($b, $d + 12)); $d += 20 }
    }
    if ($delayRva) {
        $d = & $toOffset $delayRva
        while ([BitConverter]::ToUInt32($b, $d + 4) -ne 0) { $names += & $readName ([BitConverter]::ToUInt32($b, $d + 4)); $d += 32 }
    }
    return $names
}
$imports = @(Get-PeImports $exe | ForEach-Object { $_.ToLowerInvariant() })
$proxyNames = @("dinput8.dll", "dbghelp.dll", "dbgcore.dll", "winmm.dll", "powrprof.dll", "version.dll", "xinput1_4.dll", "dxgi.dll")
$bad = @($imports | Where-Object { $proxyNames -contains $_ })
Check ($imports.Count -gt 0 -and $bad.Count -eq 0) "EMF.exe imports no proxy DLL name (imports: $($imports -join ', '))"

# ---- a fake game folder ----
$game = Join-Path ([System.IO.Path]::GetTempPath()) ("emf app smoke " + [guid]::NewGuid().ToString("N").Substring(0, 8))
function Write-File([string]$rel, [string]$text) {
    $p = Join-Path $game $rel
    New-Item -ItemType Directory -Force (Split-Path $p) | Out-Null
    [System.IO.File]::WriteAllText($p, $text)
}
function Copy-In([string]$from, [string]$rel) {
    $p = Join-Path $game $rel
    New-Item -ItemType Directory -Force (Split-Path $p) | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo $from) -Destination $p
}
$version = "$(Get-Content -LiteralPath (Join-Path $repo "VERSION") -TotalCount 1)".Trim()
Write-File "enshrouded.exe" "fake"
Write-File "dinput8.dll" "EMF-LOADER-7d3c9f fake"
Write-File "dbghelp.dll" "EML stand-in EML_LOG_FILE_PATH"  # EML loads the mods in mods\
Write-File "emf\emf_core.dll" ("emf_core $version" + [char]0)
Copy-In "src\core\known_tools.txt" "emf\known_tools.txt"
Copy-In "mods\clock\mod.json" "emf\mods\clock\mod.json"
Write-File "emf\mods\clock\clock.dll" "fake"
Copy-In "mods\lua\mod.json" "emf\mods\lua\mod.json"
Write-File "emf\mods\lua\lua.dll" "fake"
Copy-In "mods\life_skills\mod.json" "emf\mods\life_skills\mod.json"
Copy-In "mods\life_skills\main.lua" "emf\mods\life_skills\main.lua"
Copy-In "template_lua\mod.json" "emf\mods\my_first_mod\mod.json"  # the Lua starter: every kind of setting
Copy-In "template_lua\main.lua" "emf\mods\my_first_mod\main.lua"
Copy-In "content\emberbrand\mod.json" "mods\emf_emberbrand\mod.json"
Write-File "dinput8-chain.ini" "[Chain]`r`ncreative.dll`r`n"
Write-File "creative.dll" "creative mode stand-in"
Write-File "emf\chain_status.txt" "creative.dll`tstarted`r`n"
# a canary: if any of its texts became markup, an <img> appears and its onerror sets window.__x
$canary = '<img src=x onerror="window.__x=1">Canary'
Write-File "emf\mods\canary\mod.json" (@'
{"id": "canary", "name": "<img src=x onerror=\"window.__x=1\">Canary", "version": "1.0.0",
 "description": "<script>window.__x=1</script><b>bold?</b>", "authors": ["<img src=x onerror=\"window.__x=1\">"],
 "settings": [{"key": "k", "type": "choice", "choices": ["<img src=x onerror=\"window.__x=1\">", "b"],
               "label": "<img src=x onerror=\"window.__x=1\">", "help": "<script>window.__x=1</script>"}]}
'@)
Write-File "emf\mods\canary\canary.dll" "fake"
Write-File "mods\auto-loot-plus\mod.json" '{"id": "auto-loot-plus", "name": "Auto Loot Plus", "nexus_id": 170}'  # Browse marks it installed
Write-File "emf\mods\canary\README.md" '<img src=x onerror="window.__x=1"><script>window.__x=1</script> readme'
$out = Join-Path $game "smoke.json"

# Runs EMF.exe on the fake folder and returns what it wrote to out.json (with -Eval: the page script's window.emfProbe
# and where the page is, as .probe). Unless EMF_TEST_APPDATA names one, the run gets an app-data folder of its own
# (nothing carries over from another run: the key, folders.json) with the tester switch for Nexus on (New-AppData).
function Invoke-Smoke([string]$Eval = "", [string]$SelectMod = "", [string[]]$More = @()) {
    if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Force }
    $a = @("--game", "`"$game`"", "--smoke", "`"$out`"") + $More
    if ($SelectMod) { $a += @("--select", $SelectMod) }
    if ($Eval) { $a += @("--eval", "`"$Eval`"") }
    $fresh = -not $env:EMF_TEST_APPDATA
    if ($fresh) { $env:EMF_TEST_APPDATA = New-AppData (Join-Path $runTemp ("appdata " + [guid]::NewGuid().ToString("N").Substring(0, 8))) $true }
    try {
        $p = Start-Process -FilePath $exe -ArgumentList $a -PassThru
    } finally {
        if ($fresh) { Remove-Item Env:\EMF_TEST_APPDATA -ErrorAction SilentlyContinue }
    }
    if (-not $p.WaitForExit(30000)) { $p.Kill(); throw "EMF.exe did not exit within 30 s" }
    $json = if (Test-Path -LiteralPath $out) { (Get-Content -LiteralPath $out -Raw).Trim() } else { "" }
    return [pscustomobject]@{ Code = $p.ExitCode; Json = $json; R = $(if ($json) { $json | ConvertFrom-Json } else { $null }) }
}
# page script: finds the row whose name is creative.dll, notes whether its switch is locked, clicks it
$click = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<120){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var g=function(){return [...document.querySelectorAll('.mod-row')].find(r=>r.querySelector('.mod-name')&&r.querySelector('.mod-name').textContent==='creative.dll');}; var row=g(); var sw=row.querySelector('.switch'); window.emfProbe={found:true,disabled:sw.disabled}; if(sw.disabled){window.emfDone=true;} else {var b=sw.getAttribute('aria-checked'); sw.click(); W(function(){var r=g(); var s=r&&r.querySelector('.switch'); return s&&s.getAttribute('aria-checked')!==b;},function(){window.emfDone=true;});}"

# EMF.exe's temp files (WebView2's data in emf-smoke-webview-<pid>, unpack folders) go to a %TEMP% of this run's own.
# With one %TEMP% for all, a smoke test ending in another worktree deleted every emf-smoke-webview-* folder, under the
# WebView2 of this run's EMF.exe too, and a pid Windows hands out again could lead two runs to the same folder.
$realTemp = [System.IO.Path]::GetTempPath()
$runTemp = Join-Path $realTemp ("emf smoke tmp " + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $runTemp | Out-Null
$oldTmp = $env:TMP
$oldTemp = $env:TEMP
$env:TMP = $runTemp
$env:TEMP = $runTemp
# EMF.exe's app-data folder (EMF_TEST_APPDATA) with or without the tester switch for Nexus: [nexus] personal_key=1 in
# its app.ini. Without it (and without the slug, which this build does not have) EMF.exe makes no Nexus request at all.
# The runs have it on, as the testing build for Nexus does, unless they say otherwise (8b).
function New-AppData([string]$Dir, [bool]$NexusSwitch) {
    New-Item -ItemType Directory -Force $Dir | Out-Null
    if ($NexusSwitch) { [System.IO.File]::WriteAllText((Join-Path $Dir "app.ini"), "[nexus]`r`npersonal_key=1`r`n") }
    return $Dir
}

try {
    # 1. the list, read back from the page; the canary's texts are text
    $more = @("--probe-wait", "9000")  # ends as soon as the page script is done; the room is for a busy PC
    if ($Capture) { $more += @("--capture", "`"$([System.IO.Path]::GetFullPath($Capture))`"") }
    if ($Page) { $more += @("--page", $Page) }
    $probe = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<120){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; W(function(){return document.querySelectorAll('.mod-row').length>0||!document.getElementById('page-nogame').hidden;},function(){window.emfProbe={imgs:document.querySelectorAll('img').length,inline:document.querySelectorAll('script:not([src])').length,ran:!!window.__x,readme:(document.querySelector('.readme')||{}).textContent||''};window.emfDone=true;});"
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $run = Invoke-Smoke -Eval $probe -SelectMod $(if ($Select) { $Select } else { "emf:canary" }) -More $more
    Check ($run.Code -eq 0) "EMF.exe --smoke exits 0 ($($run.Code), $([int]$watch.Elapsed.TotalSeconds) s)"
    "smoke output: $($run.Json)"
    $r = $run.R
    Check ($r -and $r.rendered) "the page reported its first render of the mod list"
    $titles = if ($r) { @($r.titles) } else { @() }
    foreach ($t in @("Canary-name", "Clock", "Life Skills", "Lua Scripts", "My First Mod", "Emberbrand (EMF content example)", "creative.dll", "EML (Enshrouded Mod Loader)", "Auto Loot Plus")) {
        $want = if ($t -eq "Canary-name") { $canary } else { $t }
        Check ($titles -contains $want) "the page shows the row $want"
    }
    Check ($r -and $r.count -eq $titles.Count) "count matches the rows ($($r.count))"
    $p1 = if ($r -and $r.probe) { $r.probe.probe } else { $null }
    Check ($null -ne $p1 -and $p1.imgs -eq 0 -and $p1.inline -eq 0 -and -not $p1.ran) "the canary's HTML and script stay text: no <img>, no inline script, nothing ran ($(if ($p1) { $p1 | ConvertTo-Json -Compress }))"
    Check ($null -ne $p1 -and $p1.readme -like "*<script>*") "the canary's README is shown as text"
    if ($Capture) { Check ((Test-Path -LiteralPath $Capture) -and (Get-Item -LiteralPath $Capture).Length -gt 1000) "the page was captured to $Capture" }
    Check ((Get-Content -LiteralPath (Join-Path $game "dinput8-chain.ini") -Raw) -eq "[Chain]`r`ncreative.dll`r`n") "looking changed nothing in the game folder"

    # 1b. the Lua starter's settings: every control, a setting greyed out by another one, a key bind
    $settingsJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<120){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var pr={}; window.emfProbe=pr; var by=function(t){return [...document.querySelectorAll('.setting')].find(function(x){return x.querySelector('.setting-label').textContent===t;});}; var kb=function(){var x=by('Key that shows and hides the line'); return x&&x.querySelector('.keybind');}; W(function(){return !!by('Text');},function(){ pr.groups=[...document.querySelectorAll('.setting-group')].map(function(h){return h.textContent;}); var t=by('Text').querySelector('input'); pr.textMax=t.maxLength; pr.textOff=t.disabled; var c=by('Corner'); pr.options=[...c.querySelectorAll('option')].map(function(o){return o.textContent;}); pr.corner=c.querySelector('select').value; pr.color=by('Colour').querySelector('input[type=color]').value; var r=by('Decimals of your position').querySelector('input[type=range]'); pr.range=r?[r.min,r.max,r.step].join(','):'none'; pr.key=kb().textContent; by('Show the line').querySelector('.switch').click(); W(function(){var x=by('Text'); return x&&x.querySelector('input').disabled;},function(){ var x=by('Text'); pr.textOffAfter=x.querySelector('input').disabled; pr.cond=(x.querySelector('.setting-cond')||{}).textContent||''; kb().click(); W(function(){return !!document.querySelector('.keybind.waiting');},function(){ pr.waiting=kb().textContent; window.dispatchEvent(new KeyboardEvent('keydown',{code:'KeyG',key:'g',bubbles:true})); W(function(){return kb()&&kb().textContent==='G';},function(){ pr.keyAfter=kb().textContent; window.emfDone=true; }); }); }); });"
    $run = Invoke-Smoke -Eval $settingsJs -SelectMod "emf:my_first_mod" -More @("--probe-wait", "9000")
    $p1b = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
    Check ($run.Code -eq 0 -and $null -ne $p1b) "the Lua starter's settings page ran its script ($($run.Code))"
    if ($p1b) {
        Check ((@($p1b.groups) -join "|") -eq "On screen|Keys") "its settings sit under their group headings ($(@($p1b.groups) -join ', '))"
        Check ($p1b.textMax -eq 40 -and -not $p1b.textOff) "the text field keeps maxLength 40 and starts usable"
        Check ((@($p1b.options) -join "|") -eq "Top right|Bottom right|Bottom left" -and $p1b.corner -eq "bottom_left") "the corner is a list of the options' labels, holding the value bottom_left"
        Check ($p1b.color -eq "#ffd070") "the colour is a colour picker at #ffd070 ($($p1b.color))"
        Check ($p1b.range -eq "0,2,1") "the decimals are a slider from 0 to 2 in steps of 1 ($($p1b.range))"
        Check ($p1b.key -eq "F7") "the key bind shows F7 ($($p1b.key))"
        Check ($p1b.textOffAfter -and $p1b.cond -eq "Only while Show the line is on.") "switching Show the line off greys out the text, saying why ($($p1b.cond))"
        Check ($p1b.waiting -like "Press a key*" -and $p1b.keyAfter -eq "G") "a click on the key bind waits for a key; G binds it ($($p1b.waiting) -> $($p1b.keyAfter))"
    }
    $starterIni = Join-Path $game "emf\config\my_first_mod.ini"
    $iniText = if (Test-Path -LiteralPath $starterIni) { Get-Content -LiteralPath $starterIni -Raw } else { "" }
    Check (($iniText -match "(?m)^show=0") -and ($iniText -match "(?m)^toggle_key=G")) "both changes are in emf\config\my_first_mod.ini ($($iniText.Trim() -replace "`r`n", ' | '))"

    # 1c. key binds bind the key the game will see: by the layout's virtual key, NumLock, never the IME's key; Shift,
    # Ctrl and Alt only when released alone (Shift+H binds H)
    $keysJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<120){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var pr={}; window.emfProbe=pr; var by=function(t){return [...document.querySelectorAll('.setting')].find(function(x){return x.querySelector('.setting-label').textContent===t;});}; var kb=function(){var x=by('Key that shows and hides the line'); return x&&x.querySelector('.keybind');}; var key=function(type,code,k){var ev=new KeyboardEvent(type,{code:code,bubbles:true}); Object.defineProperty(ev,'keyCode',{get:function(){return k;}}); window.dispatchEvent(ev);}; pr.names=[keyName({keyCode:101,code:'Numpad5'}),keyName({keyCode:12,code:'Numpad5'}),keyName({keyCode:38,code:'Numpad8'}),keyName({keyCode:229,key:'Process'}),keyName({keyCode:90,code:'KeyY'}),keyName({keyCode:186,code:'Semicolon'}),keyName({keyCode:0,code:'KeyQ'}),keyName({keyCode:113,code:'F2'})].join(','); W(function(){return !!kb();},function(){ kb().click(); W(function(){return !!document.querySelector('.keybind.waiting');},function(){ key('keydown','ShiftLeft',16); pr.waitingAfterShift=!!document.querySelector('.keybind.waiting'); key('keydown','KeyH',72); W(function(){return kb()&&kb().textContent==='H';},function(){ pr.shiftH=kb().textContent; kb().click(); W(function(){return !!document.querySelector('.keybind.waiting');},function(){ key('keydown','ShiftLeft',16); key('keyup','ShiftLeft',16); W(function(){return kb()&&kb().textContent==='Shift';},function(){ pr.shiftAlone=kb().textContent; window.emfDone=true; }); }); }); }); });"
    $run = Invoke-Smoke -Eval $keysJs -SelectMod "emf:my_first_mod" -More @("--probe-wait", "9000")
    $p1c = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
    Check ($run.Code -eq 0 -and $null -ne $p1c) "the key bind page script ran ($($run.Code))"
    if ($p1c) {
        Check ($p1c.names -eq "Numpad5,0x0c,Up,,Z,0xba,Q,F2") "keys are named as the game sees them: NumLock off, the layout's letter, no IME key ($($p1c.names))"
        Check ($p1c.waitingAfterShift -and $p1c.shiftH -eq "H") "Shift held, then H, binds H ($($p1c.shiftH))"
        Check ($p1c.shiftAlone -eq "Shift") "Shift pressed and released alone binds Shift ($($p1c.shiftAlone))"
    }
    $iniText = if (Test-Path -LiteralPath $starterIni) { Get-Content -LiteralPath $starterIni -Raw } else { "" }
    Check ($iniText -match "(?m)^toggle_key=Shift") "the last bind is saved ($($iniText.Trim() -replace "`r`n", ' | '))"

    # 2. lockdown: from the page, a fetch must fail and a navigation must be cancelled
    $lock = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<120){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var t0=Date.now(); window.emfProbe={fetch:'pending'}; fetch('https://example.com/').then(function(x){window.emfProbe.fetch='status '+x.status;},function(){window.emfProbe.fetch='blocked';}); setTimeout(function(){location.href='https://example.com/';},200); W(function(){return window.emfProbe.fetch!=='pending'&&Date.now()-t0>700;},function(){window.emfDone=true;});"
    # the script takes 0.7 s and the fetch's answer: with the default 1 s wait, a busy PC could read the page while the
    # cancelled navigation settled, get nothing back and fail both checks below. The run still ends when the script is done.
    $run = Invoke-Smoke -Eval $lock -More @("--probe-wait", "9000")
    $p2 = if ($run.R -and $run.R.probe) { $run.R.probe } else { $null }
    Check ($null -ne $p2 -and "$($p2.href)".StartsWith("https://emf.app/")) "a navigation from the page is cancelled: it stays on https://emf.app/ ($($p2.href))"
    Check ($null -ne $p2 -and $null -ne $p2.probe -and ($p2.probe.fetch -eq "blocked" -or $p2.probe.fetch -eq "status 403")) "a fetch from the page does not reach the network ($(if ($p2 -and $p2.probe) { $p2.probe.fetch }))"

    # 3. the page's switch for creative.dll, clicked: its line in dinput8-chain.ini gets "; " in front
    $run = Invoke-Smoke -Eval $click -More @("--probe-wait", "6000")
    $p3 = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
    Check ($run.Code -eq 0 -and $null -ne $p3 -and $p3.found -and -not $p3.disabled) "the page script found creative.dll's switch, unlocked ($($run.Code))"
    $chain = Get-Content -LiteralPath (Join-Path $game "dinput8-chain.ini") -Raw
    Check ($chain -eq "[Chain]`r`n; creative.dll`r`n") "clicking the switch of creative.dll comments out its line in dinput8-chain.ini ($($chain.Trim() -replace "`r`n", ' | '))"

    # 4. a process called enshrouded.exe running from the folder (a copy of ping.exe standing in for the game): the
    # switch is locked, so the same click changes nothing. The page script must have run (else this proves nothing).
    Copy-Item -LiteralPath (Join-Path $env:SystemRoot "System32\PING.EXE") -Destination (Join-Path $game "enshrouded.exe") -Force
    $stand = Start-Process -FilePath (Join-Path $game "enshrouded.exe") -ArgumentList @("-n", "30", "127.0.0.1") -WindowStyle Hidden -PassThru
    try {
        $run = Invoke-Smoke -Eval $click -More @("--probe-wait", "6000")
    } finally {
        if (-not $stand.HasExited) { $stand.Kill() }
        $null = $stand.WaitForExit(5000)
    }
    $p4 = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
    $after = Get-Content -LiteralPath (Join-Path $game "dinput8-chain.ini") -Raw
    Check ($run.Code -eq 0 -and $null -ne $p4 -and $p4.found -and $p4.disabled) "while enshrouded.exe runs from the folder, the page shows creative.dll's switch locked ($(if ($p4) { $p4 | ConvertTo-Json -Compress } else { 'the page script did not run' }))"
    Check ($after -eq $chain) "and clicking it changes nothing ($($after.Trim() -replace "`r`n", ' | '))"

    # 4b. profiles through the page: Save as "Smoke" (creative.dll is off now), switch creative.dll on (the picker says
    # "(changed)"), then Switch to the profile: the switch is back and dinput8-chain.ini is as it was
    $profileJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<120){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var p={}; window.emfProbe=p; var row=function(){return [...document.querySelectorAll('.mod-row')].find(r=>r.querySelector('.mod-name')&&r.querySelector('.mod-name').textContent==='creative.dll');}; var on=function(){var r=row(); return r?r.querySelector('.switch').getAttribute('aria-checked'):'';}; var pick=function(){var s=document.getElementById('profile-select'); return s&&s.selectedOptions[0]?s.selectedOptions[0].textContent:'';}; W(function(){return document.getElementById('profile-save')&&row();},function(){ p.before=on(); p.empty=pick(); document.getElementById('profile-save').click(); W(function(){return document.getElementById('profile-name');},function(){ document.getElementById('profile-name').value='Smoke'; document.getElementById('profile-sheet-go').click(); W(function(){return state.profiles&&state.profiles.current==='smoke'&&document.getElementById('profile-modal').hidden;},function(){ p.saved=pick(); row().querySelector('.switch').click(); W(function(){return state.profiles.changed&&on()!==p.before;},function(){ p.changed=pick(); p.status=document.getElementById('profile-status').textContent; p.moved=on(); document.getElementById('profile-switch').click(); W(function(){return !state.profiles.changed&&on()===p.before;},function(){ p.after=on(); p.back=pick(); window.emfDone=true; }); }); }); }); });"
    $run = Invoke-Smoke -Eval $profileJs -More @("--probe-wait", "9000")
    $pp = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
    $ppText = if ($pp) { $pp | ConvertTo-Json -Compress } else { "the page script did not run" }
    Check ($run.Code -eq 0 -and $null -ne $pp -and $pp.empty -eq "No profiles yet" -and $pp.saved -eq "Smoke (current)" -and (Test-Path -LiteralPath (Join-Path $game "emf\profiles\smoke.json"))) "Save as writes emf\profiles\smoke.json and the picker shows it as the current profile ($ppText)"
    Check ($null -ne $pp -and $pp.changed -eq "Smoke (changed)" -and $pp.moved -ne $pp.before -and $pp.status -like "Switches changed since Smoke*") "a switch in the list makes the picker say (changed)"
    $afterProfile = Get-Content -LiteralPath (Join-Path $game "dinput8-chain.ini") -Raw
    Check ($null -ne $pp -and $pp.after -eq $pp.before -and $pp.back -eq "Smoke (current)" -and $afterProfile -eq $chain) "Switch puts the switch back: dinput8-chain.ini is as when the profile was saved ($($afterProfile.Trim() -replace "`r`n", ' | '))"

    # 5. Browse, offline: recorded Nexus replies and pictures from tests\fixtures\nexus_app, nothing from the network
    $fixture = Join-Path $repo "tests\fixtures\nexus_app"
    $browseJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<120){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var pr={cards:[],imgs:[]}; window.emfProbe=pr; var tryImg=function(src,k){var i=new Image();i.onload=function(){pr[k]='loaded';};i.onerror=function(){pr[k]='blocked';};i.src=src;}; tryImg('https://example.com/a.png','example'); tryImg('https://staticdelivery.nexusmods.com/mods/5811/images/thumbnails/170/170-1792285344-301928562.jpg','nexus'); fetch('https://staticdelivery.nexusmods.com/mods/5811/x.json').then(function(){pr.fetch='reached';},function(){pr.fetch='blocked';}); W(function(){return document.querySelectorAll('.mod-card .card-name').length>0;},function(){ pr.cards=[...document.querySelectorAll('.mod-card .card-name')].map(n=>n.textContent); pr.installed=[...document.querySelectorAll('.mod-card')].filter(c=>c.querySelector('.badge.ok')).map(c=>c.querySelector('.card-name').textContent); document.querySelector('.mod-card').click(); W(function(){return document.querySelectorAll('.file').length>0;},function(){ pr.unlisted=[...document.querySelectorAll('.file-unlisted')].map(x=>x.textContent); pr.desc=document.querySelectorAll('.bb b').length; pr.descScripts=document.querySelectorAll('.bb script, .bb [onerror], .bb [onclick]').length; pr.categories=[...document.querySelectorAll('.file .badge.cat')].map(x=>x.textContent); browse.olderOpen=true; var older=document.querySelector('.older'); if(older)older.open=true; var f9=[...document.querySelectorAll('.file')].find(x=>x.dataset.file==='929'); if(f9)f9.querySelector('button').click(); W(function(){return document.querySelectorAll('.file .badge.kind').length>0;},function(){ pr.kinds=[...document.querySelectorAll('.file .badge.kind')].map(x=>x.textContent); pr.notes=[...document.querySelectorAll('.file .notes li')].map(x=>x.textContent); var f=document.querySelector('.d-section.files'); if(f)document.getElementById('browse-details').scrollTop=f.offsetTop-16; var imgs=function(){return [...document.querySelectorAll('.mod-card img')];}; W(function(){return pr.example&&pr.nexus&&pr.fetch&&imgs().length===4&&imgs().every(i=>i.complete);},function(){ pr.imgs=imgs().map(i=>i.naturalWidth); window.emfDone=true; }); }); }); });"
    $bmore = @("--page", "browse", "--nexus-fixture", "`"$fixture`"", "--probe-wait", "9000")
    if ($BrowseCapture) { $bmore += @("--capture", "`"$([System.IO.Path]::GetFullPath($BrowseCapture))`"") }
    $run = Invoke-Smoke -Eval $browseJs -More $bmore
    $p5 = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
    Check ($run.Code -eq 0 -and $null -ne $p5) "the Browse tab ran its page script ($($run.Code))"
    Check ($null -ne $run.R -and $run.R.nexus_requests -gt 0) "with the tester switch Browse asks (the recorded) Nexus, and out.json counts the requests ($(if ($run.R) { $run.R.nexus_requests }))"
    if ($p5) {
        $cards = @($p5.cards)
        Check ($cards -contains "Auto Loot Plus" -and $cards -contains "Loot Multiplier" -and $cards.Count -eq 4) "Browse shows the recorded search results ($($cards -join ', '))"
        $loaded = @($p5.imgs | Where-Object { $_ -gt 0 })
        Check ($loaded.Count -eq 4) "the cards' pictures come from the fixture through the host ($(@($p5.imgs) -join ', '))"
        Check (@($p5.installed) -contains "Auto Loot Plus" -and @($p5.installed).Count -eq 1) "the card of a mod the folder has is marked installed ($(@($p5.installed) -join ', '))"
        Check ($p5.example -eq "blocked") "a picture from example.com is blocked ($($p5.example))"
        Check ($p5.nexus -eq "loaded") "a picture from staticdelivery.nexusmods.com is allowed ($($p5.nexus))"
        Check ($p5.fetch -eq "blocked") "a fetch to Nexus's picture host from the page is blocked ($($p5.fetch))"
        Check (@($p5.categories)[0] -eq "Main" -and @($p5.categories)[-1] -eq "Archived") "the mod's files: MAIN first, ARCHIVED last ($(@($p5.categories) -join ', '))"
        Check (@($p5.unlisted).Count -ge 1 -and @($p5.unlisted)[0] -like "Nexus doesn*t list this file*s contents; EMF checks the archive when you install it.") "a file Nexus has no listing of says so ($(@($p5.unlisted).Count))"
        Check (@($p5.kinds) -contains "DLL mod (dinput8.dll)") "an older file's contents are classified on request ($(@($p5.kinds) -join ', '))"
        Check ((@($p5.notes) -join " ") -like "*dinput8-chain.ini*") "with the classifier's notes ($(@($p5.notes).Count) notes)"
        Check ($p5.desc -gt 0 -and $p5.descScripts -eq 0) "the description is in its own container, as HTML without scripts ($($p5.desc) bold, $($p5.descScripts) scripts)"
    }
    if ($BrowseCapture) { Check ((Test-Path -LiteralPath $BrowseCapture) -and (Get-Item -LiteralPath $BrowseCapture).Length -gt 1000) "the Browse tab was captured to $BrowseCapture" }

    # 5b. Check for updates, offline: Quick Deposit 1.3.0 (Nexus has 1.3.12) and Auto Loot Plus without a version
    Write-File "mods\quick-deposit\mod.json" '{"id": "quick-deposit", "name": "Quick Deposit", "version": "1.3.0", "nexus_id": 195}'
    try {
        $updJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<120){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var p={}; window.emfProbe=p; W(function(){return document.querySelectorAll('.mod-row').length>0;},function(){ p.before=document.querySelectorAll('.badge.upd').length+document.querySelectorAll('#updates-summary').length; document.getElementById('updates-check').click(); W(function(){return document.getElementById('updates-summary');},function(){ p.summary=document.getElementById('updates-summary').textContent; p.badges={}; [...document.querySelectorAll('.mod-row')].forEach(function(r){ p.badges[r.querySelector('.mod-name').textContent]=[...r.querySelectorAll('.badge')].map(function(b){return b.textContent;}).join('|'); }); p.state=(document.getElementById('update-state')||{}).textContent||''; var b=document.querySelector('#details [data-update]'); p.button=b?b.textContent+':'+b.dataset.update:''; p.facts=[...document.querySelectorAll('#details .upd-facts dd')].map(function(d){return d.textContent;}); var a=[...document.querySelectorAll('.mod-row')].find(function(r){return r.querySelector('.mod-name').textContent==='Auto Loot Plus';}); a.click(); setTimeout(function(){ p.maybeLabels=[...document.querySelectorAll('#details .upd-facts dt')].map(function(d){return d.textContent;}); window.emfDone=true; }, 300); }); });"
        $run = Invoke-Smoke -Eval $updJs -SelectMod "eml:quick-deposit" -More @("--nexus-fixture", "`"$fixture`"", "--probe-wait", "6000")
        $pu = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        Check ($run.Code -eq 0 -and $null -ne $pu -and $pu.before -eq 0) "Check for updates: nothing is shown before the click ($(if ($pu) { $pu | ConvertTo-Json -Compress -Depth 4 }))"
        Check ($null -ne $pu -and $pu.summary -like "1 update, 1 EMF can*t tell") "the summary line says what the check found ($(if ($pu) { $pu.summary }))"
        $qd = if ($pu) { @("$($pu.badges.'Quick Deposit')" -split '\|') } else { @() }
        $alp = if ($pu) { @("$($pu.badges.'Auto Loot Plus')" -split '\|') } else { @() }
        Check (($qd -contains "Update") -and ($alp -contains "Update?")) "the list marks Quick Deposit with Update and Auto Loot Plus (no version) with Update? ($($qd -join ','); $($alp -join ','))"
        Check ($null -ne $pu -and $pu.state -like "Update available*1.3.0*1.3.12*" -and @(@($pu.facts) -like "Quick Deposit 1.3.12*").Count -eq 1 -and $pu.button -eq "Open on Nexus:1304") "the details name the file to take, with Open on Nexus without a Premium key ($(if ($pu) { $pu.state + ' / ' + (@($pu.facts) -join ' / ') + ' / ' + $pu.button }))"
        Check ($null -ne $pu -and @(@($pu.maybeLabels) -like "Nexus*s main file").Count -eq 1 -and @(@($pu.maybeLabels) -eq "File to take").Count -eq 0) "a mod EMF can't tell about shows Nexus's main file, not a file to take ($(if ($pu) { @($pu.maybeLabels) -join ', ' }))"
    } finally {
        Remove-Item -LiteralPath (Join-Path $game "mods\quick-deposit") -Recurse -Force -ErrorAction SilentlyContinue
    }

    # 5c. Link to a Nexus mod... on creative.dll (no Nexus id): what is typed survives a redraw, Enter saves the link
    $linkFile = Join-Path $game "emf\config\nexus_links.json"
    try {
        $linkJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<120){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var p={}; window.emfProbe=p; W(function(){return document.getElementById('link-start');},function(){ document.getElementById('link-start').click(); var i=document.getElementById('link-input'); p.form=!!i; i.focus(); i.value='https://www.nexusmods.com/enshrouded/mods/182'; i.dispatchEvent(new Event('input')); render(); p.sameAfterRender=document.getElementById('link-input')===i&&i.value.indexOf('/mods/182')>0&&document.activeElement===i; renderDetails(true); var j=document.getElementById('link-input'); p.keptAfterRedraw=!!j&&j.value===i.value; j.focus(); j.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true})); W(function(){var n=document.getElementById('link-note'); return n&&n.textContent.indexOf('182')>=0;},function(){ p.note=(document.getElementById('link-note')||{}).textContent||''; p.formGone=!document.getElementById('link-input'); window.emfDone=true; }); });"
        $run = Invoke-Smoke -Eval $linkJs -SelectMod "chain:creative.dll" -More @("--probe-wait", "6000")
        $pl = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        Check ($run.Code -eq 0 -and $null -ne $pl -and $pl.form -and $pl.sameAfterRender -and $pl.keptAfterRedraw) "the link form keeps what is typed when the page draws again ($(if ($pl) { $pl | ConvertTo-Json -Compress }))"
        $saved = if (Test-Path -LiteralPath $linkFile) { Get-Content -LiteralPath $linkFile -Raw } else { "" }
        Check ($null -ne $pl -and $pl.note -like "*Nexus mod 182*" -and $pl.formGone -and $saved -match '"chain:creative\.dll": 182') "Enter saves the link in emf\config\nexus_links.json and the details say so ($(if ($pl) { $pl.note }); $($saved -replace '\s+', ' '))"
    } finally {
        Remove-Item -LiteralPath $linkFile -Force -ErrorAction SilentlyContinue
    }

    # 6. install a downloaded file through the page, then uninstall it
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $src = Join-Path ([System.IO.Path]::GetTempPath()) ("emf smoke zip " + [guid]::NewGuid().ToString("N").Substring(0, 8))
    New-Item -ItemType Directory -Force (Join-Path $src "mods\auto-loot-plus\src") | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $src "dinput8.dll"), "MZ stand-in DLL mod")
    [System.IO.File]::WriteAllText((Join-Path $src "AutoLootPlus-README.txt"), "read me")
    [System.IO.File]::WriteAllText((Join-Path $src "mods\auto-loot-plus\mod.json"), '{"id": "auto-loot-plus", "name": "Auto Loot Plus", "version": "1.1.8"}')
    [System.IO.File]::WriteAllText((Join-Path $src "mods\auto-loot-plus\src\mod.lua"), "-- lua")
    $zipDir = "$src zip"
    New-Item -ItemType Directory -Force $zipDir | Out-Null
    $zip = Join-Path $zipDir "AutoLootPlus-170-1-1-8.zip"  # a Nexus download's name
    [System.IO.Compression.ZipFile]::CreateFromDirectory($src, $zip)
    try {
        $zipJs = $zip.Replace("\", "/")
        $installJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<120){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; window.emfProbe={}; window.chrome.webview.postMessage({cmd:'install_path',path:'$zipJs',nexus_id:170,nexus_name:'Auto Loot Plus'}); W(function(){return document.querySelectorAll('.part').length>0&&document.querySelector('.sheet-foot .primary');},function(){ window.emfProbe.parts=[...document.querySelectorAll('.part')].map(p=>p.className); var b=document.querySelector('.sheet-foot .primary'); window.emfProbe.canInstall=!!b&&!b.disabled; setTimeout(function(){ if(b)b.click(); W(function(){var t=document.getElementById('sheet-title'); return t&&(t.textContent==='Installed'||t.textContent==='Not installed');},function(){ window.emfProbe.title=(document.getElementById('sheet-title')||{}).textContent; window.emfDone=true; }); }, 1200); });"
        $run = Invoke-Smoke -Eval $installJs -More @("--probe-wait", "9000")
        $p6 = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        Check ($null -ne $p6 -and $p6.canInstall -and @($p6.parts).Count -eq 2) "the page shows the plan with two parts and Install ($(if ($p6) { @($p6.parts) -join ', ' }))"
        Check ($null -ne $p6 -and $p6.title -eq "Installed") "Install reports Installed ($(if ($p6) { $p6.title }))"
        Check ((Test-Path -LiteralPath (Join-Path $game "autolootplus.dll")) -and (Test-Path -LiteralPath (Join-Path $game "mods\auto-loot-plus\src\mod.lua"))) "the files are in the game folder (autolootplus.dll, mods\auto-loot-plus\)"
        $chain = Get-Content -LiteralPath (Join-Path $game "dinput8-chain.ini") -Raw
        Check ($chain -eq "[Chain]`r`n; creative.dll`r`nautolootplus.dll`r`n") "autolootplus.dll is listed inside [Chain] ($($chain.Trim() -replace "`r`n", ' | '))"
        Check (Test-Path -LiteralPath (Join-Path $game "emf\installed_mods\nexus-170.json")) "the install is recorded in emf\installed_mods\nexus-170.json"
        $backup = @(Get-ChildItem -LiteralPath (Join-Path $game "emf\backup") -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -like "*\mods\auto-loot-plus\mod.json" })
        Check ($backup.Count -ge 1) "the mod's old file that was there moved to emf\backup\ first"

        $uninstallJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<120){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; W(function(){return document.querySelector('[data-focus=uninstall]');},function(){ var b=document.querySelector('[data-focus=uninstall]'); window.emfProbe={found:!!b}; b.click(); W(function(){return document.querySelector('.sheet-foot .primary');},function(){ document.querySelector('.sheet-foot .primary').click(); W(function(){var t=document.getElementById('sheet-title'); return t&&(t.textContent==='Uninstalled'||t.textContent==='Not uninstalled');},function(){ window.emfProbe.title=(document.getElementById('sheet-title')||{}).textContent; window.emfDone=true; }); }); });"
        $run = Invoke-Smoke -Eval $uninstallJs -SelectMod "eml:auto-loot-plus" -More @("--probe-wait", "9000")
        $p7 = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        Check ($null -ne $p7 -and $p7.found -and $p7.title -eq "Uninstalled") "the mod's details offer Uninstall, which reports Uninstalled ($(if ($p7) { $p7 | ConvertTo-Json -Compress }))"
        Check (-not (Test-Path -LiteralPath (Join-Path $game "autolootplus.dll")) -and -not (Test-Path -LiteralPath (Join-Path $game "AutoLootPlus-README.txt")) -and -not (Test-Path -LiteralPath (Join-Path $game "mods\auto-loot-plus"))) "uninstall removed the files it installed"
        $chain = Get-Content -LiteralPath (Join-Path $game "dinput8-chain.ini") -Raw
        Check ($chain -eq "[Chain]`r`n; creative.dll`r`n") "and took its line out of dinput8-chain.ini ($($chain.Trim() -replace "`r`n", ' | '))"
    } finally {
        Remove-Item -LiteralPath $src -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $zipDir -Recurse -Force -ErrorAction SilentlyContinue
    }

    # 8. the Nexus account, offline. The real nxm:// handler (HKCU) is read before and after: the smoke runs use a
    # registry in memory and must not change it.
    function Get-RegSnapshot([string]$Path) {
        if (-not (Test-Path -LiteralPath $Path)) { return "absent" }
        $lines = @()
        foreach ($k in @(@(Get-Item -LiteralPath $Path) + @(Get-ChildItem -LiteralPath $Path -Recurse -ErrorAction SilentlyContinue))) {
            $lines += "[" + $k.Name + "]"
            foreach ($v in $k.GetValueNames()) {
                $lines += "$v=" + $k.GetValueKind($v) + ":" + (@($k.GetValue($v, $null, 'DoNotExpandEnvironmentNames')) -join ",")
            }
        }
        return ($lines -join "`n")
    }
    $realBefore = (Get-RegSnapshot "HKCU:\Software\Classes\nxm") + "`n" + (Get-RegSnapshot "HKCU:\Software\EMF")
    # a handler at a path that exists nowhere else, so the page must show this very program
    $fakeVortex = "C:\EMF-smoke-$([guid]::NewGuid().ToString('N').Substring(0, 8))\Vortex\Vortex.exe"
    $env:EMF_TEST_NXM_COMMAND = "`"$fakeVortex`" -d `"%1`""
    try {
        $acctJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<120){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var o=function(){return (document.getElementById('nxm-owner')||{}).textContent;}; window.emfProbe={}; W(function(){return document.getElementById('acct-key-state')&&o();},function(){ var p=window.emfProbe; p.key=document.getElementById('acct-key-state').textContent; p.owner=o(); p.ownerPath=document.getElementById('nxm-owner').title; p.sso=!!document.getElementById('sso-start'); p.chip=document.getElementById('nexus-chip').textContent; document.getElementById('nxm-toggle').click(); W(function(){return o()!==p.owner;},function(){ p.taken=o(); p.toggle=document.getElementById('nxm-toggle').textContent; document.getElementById('nxm-toggle').click(); W(function(){return o()!==p.taken;},function(){ p.back=o(); window.emfDone=true; }); }); });"
        $run = Invoke-Smoke -Eval $acctJs -More @("--page", "emf", "--probe-wait", "6000")
        $p8 = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        Check ($run.Code -eq 0 -and $null -ne $p8 -and $p8.key -eq "not set") "the EMF tab's Nexus account says the API key is not set ($(if ($p8) { $p8 | ConvertTo-Json -Compress }))"
        Check ($null -ne $p8 -and -not $p8.sso) "Log in with Nexus is hidden while EMF has no application slug"
        Check ($null -ne $p8 -and $p8.owner -eq "Vortex" -and $p8.ownerPath -eq $fakeVortex) "the nxm:// setting names the program that has the links, with its full path ($(if ($p8) { $p8.ownerPath }))"
        Check ($null -ne $p8 -and $p8.taken -eq "EMF (this copy)" -and $p8.toggle -eq "Give them back to Vortex" -and $p8.back -eq "Vortex") "the setting lets EMF take the links and gives them back to Vortex"
    } finally {
        Remove-Item Env:\EMF_TEST_NXM_COMMAND -ErrorAction SilentlyContinue
    }

    # a recorded Nexus with a key, a file and its CDN copy (a zip made here)
    $nx = Join-Path ([System.IO.Path]::GetTempPath()) ("emf smoke nexus " + [guid]::NewGuid().ToString("N").Substring(0, 8))
    $appdata = "$nx appdata"
    Copy-Item -LiteralPath $fixture -Destination $nx -Recurse
    New-Item -ItemType Directory -Force (Join-Path $nx "downloads") | Out-Null
    New-AppData $appdata $true | Out-Null
    $zsrc = "$nx src"
    New-Item -ItemType Directory -Force (Join-Path $zsrc "mods\auto-loot-plus\src") | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $zsrc "dinput8.dll"), "MZ stand-in DLL mod")
    [System.IO.File]::WriteAllText((Join-Path $zsrc "mods\auto-loot-plus\mod.json"), '{"id": "auto-loot-plus", "name": "Auto Loot Plus", "version": "1.1.8"}')
    [System.IO.File]::WriteAllText((Join-Path $zsrc "mods\auto-loot-plus\src\mod.lua"), "-- lua")
    $zipName = "AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip"
    $cdnZip = Join-Path $nx "downloads\$zipName"
    [System.IO.Compression.ZipFile]::CreateFromDirectory($zsrc, $cdnZip)
    $zipSize = (Get-Item -LiteralPath $cdnZip).Length
    Copy-Item -LiteralPath (Join-Path $repo "tests\fixtures\nexus\v1_validate_premium.json") -Destination (Join-Path $nx "v1_validate.json")
    [System.IO.File]::WriteAllText((Join-Path $nx "v1_file_170_1197.json"), "{`"file_id`":1197,`"name`":`"AutoLootPlus 1.1.8`",`"version`":`"1.1.8`",`"category_name`":`"MAIN`",`"file_name`":`"$zipName`",`"size_in_bytes`":$zipSize}")
    [System.IO.File]::WriteAllText((Join-Path $nx "v1_link_170_1197.json"), '[{"name":"Nexus Global Content Delivery Network","short_name":"Nexus CDN","URI":"https://cf-files.nexusmods.com/cdn/5811/170/AutoLootPlus%201.1.8-170-1-1-8-1792790875.zip?md5=x&expires=1792800000&user_id=12345"}]')
    $env:EMF_TEST_APPDATA = $appdata
    $fixtureKey = "fixture-key-0123456789"
    try {
        # a key saved through the page, then Download with EMF on Auto Loot Plus's MAIN file
        $dlJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<150){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var p={}; window.emfProbe=p; W(function(){return document.getElementById('acct-key');},function(){ var k=document.getElementById('acct-key'); k.value='$fixtureKey'; document.getElementById('acct-save').click(); p.cleared=k.value===''; W(function(){return document.getElementById('acct-premium');},function(){ p.key=document.getElementById('acct-key-state').textContent; p.premium=document.getElementById('acct-premium').textContent; p.chip=document.getElementById('nexus-chip').textContent; p.rate=(document.getElementById('acct-rate')||{}).textContent||''; p.leak=document.documentElement.outerHTML.indexOf('$fixtureKey')>=0||JSON.stringify(account).indexOf('$fixtureKey')>=0; document.querySelector('.tab[data-tab=browse]').click(); W(function(){return document.querySelector('.mod-card .card-name');},function(){ document.querySelector('.mod-card').click(); W(function(){return document.querySelector('[data-download=\'1197\']');},function(){ var b=document.querySelector('[data-download=\'1197\']'); p.button=b.textContent; b.click(); W(function(){return modal&&modal.mode==='plan';},function(){ p.plan=modal&&modal.plan?{key:modal.plan.key,nexus_id:modal.plan.nexus_id,file_id:modal.plan.nexus_file_id,parts:modal.plan.parts.length,archive:modal.plan.archive}:null; p.bar=download?download.state:''; window.emfDone=true; }); }); }); }); });"
        $run = Invoke-Smoke -Eval $dlJs -More @("--nexus-fixture", "`"$nx`"", "--probe-wait", "10000")
        $p9 = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        Check ($run.Code -eq 0 -and $null -ne $p9 -and $p9.key -eq "set" -and $p9.premium -eq "Premium" -and $p9.chip -like "EmberTester*Premium") "a key saved through the page is checked and shows the account as Premium ($(if ($p9) { $p9 | ConvertTo-Json -Compress -Depth 4 }))"
        Check ($null -ne $p9 -and $p9.cleared -and -not $p9.leak) "the page empties the key field and never holds the key afterwards"
        Check ($null -ne $p9 -and $p9.rate -like "*19,999 left today*") "the account shows the requests Nexus allows ($(if ($p9) { $p9.rate }))"
        $keyFile = Join-Path $appdata "nexus.key"
        $keyBytes = if (Test-Path -LiteralPath $keyFile) { [System.IO.File]::ReadAllText($keyFile, [System.Text.Encoding]::GetEncoding(28591)) } else { "" }
        Check ($keyBytes.Length -gt 0 -and $keyBytes.IndexOf($fixtureKey) -lt 0 -and $keyBytes.IndexOf("EmberTester") -lt 0) "the key is stored encrypted in nexus.key (neither the key nor the name readable)"
        Check ($null -ne $p9 -and $p9.button -eq "Download with EMF") "a Premium account gets Download with EMF on a Browse file"
        $got = Join-Path $appdata "downloads\$zipName"
        Check ((Test-Path -LiteralPath $got) -and (Get-Item -LiteralPath $got).Length -eq $zipSize -and -not (Test-Path -LiteralPath "$got.part")) "the file is downloaded into the downloads folder, every byte"
        Check ($null -ne $p9 -and $null -ne $p9.plan -and $p9.plan.key -eq "nexus-170" -and $p9.plan.nexus_id -eq "170" -and $p9.plan.file_id -eq "1197" -and $p9.plan.parts -ge 1 -and $p9.plan.archive -eq $zipName) "the download opens the install plan, which knows the Nexus mod and file ($(if ($p9) { $p9.plan | ConvertTo-Json -Compress }))"
        Check (-not (Test-Path -LiteralPath (Join-Path $game "autolootplus.dll"))) "nothing is installed until Install is clicked"

        # a download that finishes while another sheet is open waits behind "Show plan"; its plan keeps the keyboard off
        # Install, and Install ignores a click right after the sheet appears (the key from the run above is stored)
        $waitPlanJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<150){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var p={}; window.emfProbe=p; W(function(){return account&&account.user&&account.user.premium;},function(){ showModal({mode:'done',ok:true,title:'A sheet in the way',lines:[]}); document.querySelector('.tab[data-tab=browse]').click(); W(function(){return document.querySelector('.mod-card .card-name');},function(){ document.querySelector('.mod-card').click(); W(function(){return document.querySelector('[data-download=\'1197\']');},function(){ document.querySelector('[data-download=\'1197\']').click(); W(function(){return document.getElementById('download-show-plan');},function(){ p.modeBefore=modal?modal.mode:''; p.titleBefore=(document.getElementById('sheet-title')||{}).textContent; document.getElementById('download-show-plan').click(); W(function(){return modal&&modal.mode==='plan';},function(){ p.fromDownload=!!modal.fromDownload; p.focus=(document.activeElement&&document.activeElement.dataset)?(document.activeElement.dataset.focus||document.activeElement.textContent):''; var b=document.querySelector('.sheet-foot .primary'); if(b)b.click(); setTimeout(function(){ p.afterQuickClick=modal?modal.mode:'none'; window.emfDone=true; }, 300); }); }); }); }); });"
        $run = Invoke-Smoke -Eval $waitPlanJs -More @("--nexus-fixture", "`"$nx`"", "--probe-wait", "10000")
        $p12 = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        Check ($null -ne $p12 -and $p12.modeBefore -eq "done" -and $p12.titleBefore -eq "A sheet in the way") "a download's plan does not replace the sheet that is open; it waits behind Show plan ($(if ($p12) { $p12 | ConvertTo-Json -Compress }))"
        Check ($null -ne $p12 -and $p12.fromDownload -and $p12.focus -eq "close" -and $p12.afterQuickClick -eq "plan") "Show plan opens it with the keyboard on Cancel, and a click on Install right away is ignored"

        # Check for updates with the Premium key: Auto Loot Plus 1.1.7 (Nexus has 1.1.8), Update with EMF, the plan
        Write-File "mods\auto-loot-plus\mod.json" '{"id": "auto-loot-plus", "name": "Auto Loot Plus", "version": "1.1.7", "nexus_id": 170}'
        try {
            $updDlJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<150){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var p={}; window.emfProbe=p; W(function(){return account&&account.user&&account.user.premium&&document.querySelectorAll('.mod-row').length>0;},function(){ document.getElementById('updates-check').click(); W(function(){return document.querySelector('#details [data-update]');},function(){ var b=document.querySelector('#details [data-update]'); p.summary=(document.getElementById('updates-summary')||{}).textContent||''; p.button=b.textContent+':'+b.dataset.update; p.log=[...document.querySelectorAll('#details .upd-log li')].map(function(l){return l.textContent;}); b.click(); W(function(){return modal&&modal.mode==='plan';},function(){ p.plan=modal&&modal.plan?{key:modal.plan.key,nexus_id:modal.plan.nexus_id,file_id:modal.plan.nexus_file_id}:null; window.emfDone=true; }); }); });"
            $run = Invoke-Smoke -Eval $updDlJs -SelectMod "eml:auto-loot-plus" -More @("--nexus-fixture", "`"$nx`"", "--probe-wait", "10000")
            $pp = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
            Check ($run.Code -eq 0 -and $null -ne $pp -and $pp.summary -like "1 update*" -and $pp.button -eq "Update with EMF:1197" -and @($pp.log).Count -ge 1) "with a Premium key the details offer Update with EMF for the file to take, with its changelog ($(if ($pp) { $pp | ConvertTo-Json -Compress -Depth 4 }))"
            Check ($null -ne $pp -and $null -ne $pp.plan -and $pp.plan.key -eq "nexus-170" -and $pp.plan.file_id -eq "1197") "Update with EMF downloads the file and opens the plan of the same install, nothing installed yet"
            Check (-not (Test-Path -LiteralPath (Join-Path $game "mods\auto-loot-plus\src\mod.lua"))) "nothing is installed until Install is clicked (Check for updates)"
        } finally {
            Remove-Item -LiteralPath (Join-Path $game "mods\auto-loot-plus") -Recurse -Force -ErrorAction SilentlyContinue
        }

        # nxm:// links at start: another game is refused (its key never shown); an Enshrouded link waits for a key
        $env:EMF_TEST_APPDATA = New-AppData "$appdata empty" $true
        $linkJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<100){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; W(function(){return document.getElementById('download-error')||document.getElementById('acct-waiting');},function(){ window.emfProbe={error:(document.getElementById('download-error')||{}).textContent||'', waiting:!!document.getElementById('acct-waiting'), tab:tab, page:document.documentElement.outerHTML.indexOf('SECRETKEY')>=0}; window.emfDone=true; });"
        $run = Invoke-Smoke -Eval $linkJs -More @("--nexus-fixture", "`"$nx`"", "--nxm", "`"nxm://skyrimspecialedition/mods/266/files/1000?key=SECRETKEY&expires=1&user_id=2`"")
        $p10 = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        Check ($run.Code -eq 0 -and $null -ne $p10 -and $p10.error -like "*skyrimspecialedition*" -and -not $p10.page) "an nxm:// link for another game is refused with its name, and its key appears nowhere ($(if ($p10) { $p10.error }))"
        $run = Invoke-Smoke -Eval $linkJs -More @("--nexus-fixture", "`"$nx`"", "--nxm", "`"nxm://enshrouded/mods/170/files/1197?key=k1&expires=4102444800`"")
        $p11 = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        Check ($run.Code -eq 0 -and $null -ne $p11 -and $p11.waiting -and $p11.tab -eq "emf") "an Enshrouded nxm:// link without a key opens the account and waits for the key ($(if ($p11) { $p11 | ConvertTo-Json -Compress }))"

        # 8b. the public build: no tester switch, no slug. It starts on Browse (--page browse) with an Enshrouded link,
        # the key a testing build stored, and the recorded Nexus at hand (so any request would be answered and counted);
        # a page script then asks for everything Nexus anyway
        $offData = New-AppData "$appdata off" $false
        Copy-Item -LiteralPath (Join-Path $appdata "nexus.key") -Destination (Join-Path $offData "nexus.key")
        $keyBefore = [System.IO.File]::ReadAllBytes((Join-Path $offData "nexus.key"))
        $env:EMF_TEST_APPDATA = $offData
        $offJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<120){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var p={results:{}}; window.emfProbe=p; window.chrome.webview.addEventListener('message',function(e){var m=e.data; if(m&&m.type==='result'&&m.cmd)p.results[m.cmd]=m.ok;}); var tryImg=function(src){var i=new Image();i.onload=function(){p.img='loaded';};i.onerror=function(){p.img='blocked';};i.src=src;}; W(function(){return document.getElementById('download-error')&&document.querySelectorAll('.mod-row').length>0;},function(){ var shown=function(id){var x=document.getElementById(id);return !!x&&!x.hidden;}; p.mode=state.nexus; p.tab=tab; p.error=document.getElementById('download-error').textContent; p.browseTab=shown('tab-browse'); p.browsePage=shown('page-browse'); p.updates=shown('updates-check'); p.account=shown('account')||shown('account-title'); p.nxm=shown('nxm-setting'); p.chip=shown('nexus-chip'); p.install=shown('mods-install'); selectTab('browse'); p.tabAfter=tab; p.dl=document.querySelectorAll('[data-download]').length; tryImg('https://staticdelivery.nexusmods.com/mods/5811/images/thumbnails/170/170-1792285344-301928562.jpg'); send({cmd:'nexus_search',text:'loot',sort:'downloads',offset:0,seq:1}); send({cmd:'nexus_mod',id:170,seq:2}); send({cmd:'nexus_file',mod_id:170,file_id:1197,seq:3}); send({cmd:'updates_check',seq:4}); send({cmd:'account_key',key:'$fixtureKey'}); send({cmd:'account_check'}); send({cmd:'account_logout'}); send({cmd:'nxm_take'}); send({cmd:'sso_start'}); W(function(){return Object.keys(p.results).length>=9&&p.img;},function(){ p.dlAfter=document.querySelectorAll('[data-download]').length; window.emfDone=true; }); });"
        $run = Invoke-Smoke -Eval $offJs -More @("--page", "browse", "--nexus-fixture", "`"$nx`"", "--probe-wait", "9000", "--nxm", "`"nxm://enshrouded/mods/170/files/1197?key=k1&expires=4102444800`"")
        $po = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        $poText = if ($po) { $po | ConvertTo-Json -Compress -Depth 4 } else { "" }
        Check ($run.Code -eq 0 -and $null -ne $po -and $po.mode -eq "off") "without the tester switch EMF.exe runs without Nexus ($poText)"
        Check ($null -ne $po -and -not $po.browseTab -and -not $po.browsePage -and $po.tab -eq "mods" -and $po.tabAfter -eq "mods") "no Browse tab: a start on Browse shows the Mods tab, and Browse cannot be opened"
        Check ($null -ne $po -and -not $po.updates) "no Check for updates"
        Check ($null -ne $po -and -not $po.account -and -not $po.nxm -and -not $po.chip) "no Nexus account card, no nxm:// card, no Nexus chip"
        Check ($null -ne $po -and $po.install) "Install a downloaded file stays on the Mods tab"
        Check ($null -ne $po -and $po.dl -eq 0 -and $po.dlAfter -eq 0) "no Download with EMF anywhere"
        Check ($null -ne $po -and $po.error -like "*doesn't download from Nexus*Manual Download*Install a downloaded file*") "a Mod Manager Download link says plainly that this EMF doesn't download from Nexus, and how to install by hand ($(if ($po) { $po.error }))"
        $refusedAll = $null -ne $po -and @("nexus_search", "nexus_mod", "nexus_file", "updates_check", "account_key", "account_check", "account_logout", "nxm_take", "sso_start" | Where-Object { $po.results.$_ -ne $false }).Count -eq 0
        Check $refusedAll "the host refuses every Nexus command a page script sends ($(if ($po) { $po.results | ConvertTo-Json -Compress }))"
        Check ($null -ne $po -and $po.img -eq "blocked") "a picture from Nexus's host is blocked ($(if ($po) { $po.img }))"
        Check ($null -ne $run.R -and $run.R.nexus_requests -eq 0) "and EMF.exe made no Nexus request at all ($(if ($run.R) { $run.R.nexus_requests }))"
        $keyAfter = if (Test-Path -LiteralPath (Join-Path $offData "nexus.key")) { [System.IO.File]::ReadAllBytes((Join-Path $offData "nexus.key")) } else { @() }
        Check ($keyAfter.Length -gt 0 -and [System.Linq.Enumerable]::SequenceEqual([byte[]]$keyBefore, [byte[]]$keyAfter)) "the key a testing build stored is left as it was (not deleted by log out)"
    } finally {
        Remove-Item Env:\EMF_TEST_APPDATA -ErrorAction SilentlyContinue
        foreach ($d in @($nx, $appdata, "$appdata empty", "$appdata off", $zsrc)) { Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue }
    }
    # a second EMF.exe started for a link hands it to the one that runs, and exits without a window of its own
    $env:EMF_TEST_HANDOFF = "smoke" + [guid]::NewGuid().ToString("N").Substring(0, 8)
    $env:EMF_TEST_APPDATA = New-AppData (Join-Path $runTemp "appdata handoff") $true
    $outB = Join-Path $game "smoke-b.json"
    try {
        if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Force }
        $waitJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<130){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; W(function(){return document.getElementById('download-error');},function(){ window.emfProbe={error:(document.getElementById('download-error')||{}).textContent||''}; window.emfDone=true; });"
        $a = Start-Process -FilePath $exe -ArgumentList @("--game", "`"$game`"", "--smoke", "`"$out`"", "--probe-wait", "10000", "--eval", "`"$waitJs`"") -PassThru
        Start-Sleep -Milliseconds 1500
        $watch = [System.Diagnostics.Stopwatch]::StartNew()
        $b = Start-Process -FilePath $exe -ArgumentList @("--game", "`"$game`"", "--smoke", "`"$outB`"", "--nxm", "`"nxm://skyrimspecialedition/mods/266/files/1000?key=k&expires=1`"") -PassThru
        $bDone = $b.WaitForExit(20000)
        $bSeconds = [int]$watch.Elapsed.TotalSeconds
        if (-not $a.WaitForExit(30000)) { $a.Kill() }
        $ra = if (Test-Path -LiteralPath $out) { (Get-Content -LiteralPath $out -Raw) | ConvertFrom-Json } else { $null }
        $pa = if ($ra -and $ra.probe) { $ra.probe.probe } else { $null }
        Check ($bDone -and $b.ExitCode -eq 0 -and -not (Test-Path -LiteralPath $outB) -and $bSeconds -lt 10) "a second EMF.exe started with a link hands it over and exits at once ($bSeconds s, no window of its own)"
        Check ($null -ne $pa -and $pa.error -like "*skyrimspecialedition*") "the running EMF.exe gets the link and answers it ($(if ($pa) { $pa.error }))"
    } finally {
        Remove-Item Env:\EMF_TEST_HANDOFF -ErrorAction SilentlyContinue
        Remove-Item Env:\EMF_TEST_APPDATA -ErrorAction SilentlyContinue
    }
    $realAfter = (Get-RegSnapshot "HKCU:\Software\Classes\nxm") + "`n" + (Get-RegSnapshot "HKCU:\Software\EMF")
    Check ($realBefore -eq $realAfter) "the real HKCU\Software\Classes\nxm and HKCU\Software\EMF are as they were ($($realAfter -replace "`n", ' | '))"

    # 9. a dedicated server folder (docs/app.md "Servers"): added through the folder switcher, the Server tab
    # without any password, chat settings saved through the page (other keys and comments kept), Start and Stop against a
    # stand-in enshrouded_server.exe (tools/standin_server, which stops on Ctrl+C like the real one), the logs with a
    # password stripped, Install EMF with a package's own install.ps1, and the server's mods with where they run
    $srvDir = Join-Path ([System.IO.Path]::GetTempPath()) ("emf smoke server " + [guid]::NewGuid().ToString("N").Substring(0, 8))
    $pkgDir = "$srvDir package"
    function Write-Srv([string]$rel, [string]$text) {
        $p = Join-Path $srvDir $rel
        New-Item -ItemType Directory -Force (Split-Path $p) | Out-Null
        [System.IO.File]::WriteAllText($p, $text)
    }
    function Stop-Leftovers {
        foreach ($p in @(Get-Process enshrouded_server -ErrorAction SilentlyContinue)) {
            $path = $null
            try { $path = $p.Path } catch { }
            if ($path -and $path.StartsWith($srvDir + "\", [System.StringComparison]::OrdinalIgnoreCase)) { $p.Kill(); $null = $p.WaitForExit(5000); $true }
        }
    }
    try {
        New-Item -ItemType Directory -Force $srvDir | Out-Null
        Copy-Item -LiteralPath (Join-Path $Build "standin_server\enshrouded_server.exe") -Destination (Join-Path $srvDir "enshrouded_server.exe")
        Write-Srv "enshrouded_server.json" '{"name": "Smoke Test Server", "queryPort": 15637, "slotCount": 6, "enableTextChat": true, "logDirectory": "./logs", "userGroups": [{"name": "Admin", "password": "SmokeSecret111", "canKickBan": true}, {"name": "Guest", "password": ""}]}'
        Write-Srv "emf\emf_core.ini" "[chat]`r`n; who may run EMF commands`r`nadmins=`r`neveryone=0`r`n[console]`r`nwindow=0`r`n"
        Write-Srv "emf\emf_core.log" "[11:06:10.000 t2] chat command refused from account 0x1111222233334444: help (to allow it, add the account to [chat] admins in emf\emf_core.ini)`r`n"
        Write-Srv "logs\enshrouded_server.log" "[I 00:00:00,001] Config Parsed`r`n[I 00:00:00,002] someone pasted SmokeSecret111 into the console`r`n"
        Write-Srv "emf\mods\clock\mod.json" (Get-Content -LiteralPath (Join-Path $repo "mods\clock\mod.json") -Raw)
        Write-Srv "emf\mods\clock\clock.dll" "fake"
        $srvJs = $srvDir.Replace("\", "/")
        # (two runs: a smoke run ends after 15 s) first: added through the folder switcher, the tab, the chat settings
        $serverJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<90){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var p={}; window.emfProbe=p; var t=function(id){return (document.getElementById(id)||{}).textContent||'';}; W(function(){return document.querySelector('.mod-row');},function(){ window.chrome.webview.postMessage({cmd:'folder_add',path:'$srvJs'}); W(function(){var b=document.getElementById('tab-server'); return b&&!b.hidden&&document.querySelector('.mod-row .badge.warn');},function(){ p.switcher=[...document.getElementById('folder-switch').options].map(function(o){return o.textContent;}); p.eyebrow=t('folder-eyebrow'); p.badges=[...document.querySelectorAll('.mod-row .badge')].map(function(b){return b.textContent;}); document.getElementById('tab-server').click(); W(function(){return document.getElementById('srv-config');},function(){ p.groups=[...document.querySelectorAll('#srv-groups tbody tr')].map(function(r){return r.textContent;}); p.leak=document.documentElement.outerHTML.indexOf('SmokeSecret111')>=0; var box=document.getElementById('srv-admins'); box.value='0x0123456789abcdef'; box.dispatchEvent(new Event('input')); document.getElementById('srv-chat-save').click(); W(function(){return t('srv-chat-note').indexOf('Not saved')<0;},function(){ document.querySelector('#srv-refused button').click(); W(function(){return !document.getElementById('srv-refused');},function(){ p.added=true; window.emfDone=true; }); }); }); }); });"
        $run = Invoke-Smoke -Eval $serverJs -More @("--probe-wait", "9000")
        $p13 = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        Check ($run.Code -eq 0 -and $null -ne $p13 -and (@($p13.switcher) -contains "Server: Smoke Test Server") -and $p13.eyebrow -eq "Server folder") "a server folder added through the page shows in the folder switcher and is shown ($(if ($p13) { @($p13.switcher) -join ', ' } else { "exit $($run.Code)" }))"
        Check ($null -ne $p13 -and (@($p13.badges) -contains "Not for servers")) "the server's Mods tab flags the clock, a client mod ($(if ($p13) { @($p13.badges) -join ', ' }))"
        Check ($null -ne $p13 -and @($p13.groups).Count -eq 2 -and @($p13.groups)[0] -like "Admin*set*" -and @($p13.groups)[1] -like "Guest*none*" -and -not $p13.leak) "the Server tab shows the user groups with passwords only as set or none, and no password anywhere in the page"
        $ini = if (Test-Path -LiteralPath (Join-Path $srvDir "emf\emf_core.ini")) { Get-Content -LiteralPath (Join-Path $srvDir "emf\emf_core.ini") -Raw } else { "" }
        Check ($ini -eq "[chat]`r`n; who may run EMF commands`r`nadmins=0x0123456789abcdef, 0x1111222233334444`r`neveryone=0`r`n[console]`r`nwindow=0`r`n") "the admins saved through the page, and a refused account added in one click, are in emf\emf_core.ini with its other keys and comments ($($ini.Trim() -replace "`r`n", ' | '))"

        # then Start and Stop, and the log
        $runJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<90){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var p={}; window.emfProbe=p; var t=function(id){return (document.getElementById(id)||{}).textContent||'';}; W(function(){var b=document.getElementById('srv-start'); return b&&!b.disabled;},function(){ document.getElementById('srv-start').click(); W(function(){return t('srv-running')==='Running';},function(){ p.pid=t('srv-pid'); W(function(){return (srv.logs.server||'').indexOf('Run after')>=0;},function(){ p.log=srv.logs.server; document.getElementById('srv-stop').click(); W(function(){return t('srv-running')==='Stopped'&&!document.getElementById('srv-stopping');},function(){ p.after=t('srv-running'); p.stopText=t('srv-stop-text'); window.emfDone=true; }); }); }); });"
        $run = Invoke-Smoke -Eval $runJs -More @("--game", "`"$srvDir`"", "--page", "server", "--probe-wait", "10000")
        $p15 = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        $left = @(Stop-Leftovers)
        Check ($run.Code -eq 0 -and $null -ne $p15 -and $p15.pid -like "process *started by this EMF.exe" -and $p15.after -eq "Stopped" -and $p15.stopText -like "*stopped cleanly*") "Start runs the server (with its pid) and Stop ends it cleanly ($(if ($p15) { "$($p15.pid); $($p15.stopText)" } else { "exit $($run.Code): $($run.Json)" }))"
        $stopped = if (Test-Path -LiteralPath (Join-Path $srvDir "stopped.txt")) { Get-Content -LiteralPath (Join-Path $srvDir "stopped.txt") -Raw } else { "" }
        Check ($stopped -eq "Ctrl_C" -and $left.Count -eq 0) "the server's own shutdown ran on Ctrl+C, and nothing was left running ($stopped)"
        Check ($null -ne $p15 -and $p15.log -like "*Config Parsed*" -and $p15.log -notlike "*SmokeSecret111*") "the Server tab follows the server's own log, with the password stripped"

        # Install EMF into the server through the page: a package made here, with the repo's install.ps1 in game\emf\setup\
        New-Item -ItemType Directory -Force (Join-Path $pkgDir "game\emf\setup") | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $pkgDir "game\dinput8.dll"), "EMF-LOADER-7d3c9f smoke loader")
        [System.IO.File]::WriteAllText((Join-Path $pkgDir "game\emf\emf_core.dll"), "emf_core 9.9.9" + [char]0)
        foreach ($f in @("install.ps1", "emf_common.ps1")) { Copy-Item -LiteralPath (Join-Path $repo "package\$f") -Destination (Join-Path $pkgDir "game\emf\setup\$f") }
        $env:EMF_TEST_EMF_PACKAGE = Join-Path $pkgDir "game\emf"
        $installEmfJs = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<120){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var p={}; window.emfProbe=p; W(function(){return document.getElementById('srv-install');},function(){ p.label=document.getElementById('srv-install').textContent; document.getElementById('srv-install').click(); W(function(){var s=document.getElementById('sheet-title'); return s&&(s.textContent==='EMF installed into the server'||s.textContent==='EMF not installed');},function(){ p.title=document.getElementById('sheet-title').textContent; p.lines=[...document.querySelectorAll('.part-lines li')].map(function(l){return l.textContent;}); window.emfDone=true; }); });"
        $run = Invoke-Smoke -Eval $installEmfJs -More @("--game", "`"$srvDir`"", "--page", "server", "--probe-wait", "10000")
        $p14 = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        Check ($run.Code -eq 0 -and $null -ne $p14 -and $p14.label -eq "Install EMF 9.9.9" -and $p14.title -eq "EMF installed into the server") "Install EMF runs the package's install.ps1 for the server ($(if ($p14) { "$($p14.title): $(@($p14.lines) -join ' / ')" }))"
        $core = Join-Path $srvDir "emf\emf_core.dll"
        Check ((Test-Path -LiteralPath $core) -and ((Get-Content -LiteralPath $core -Raw) -like "emf_core 9.9.9*") -and (Test-Path -LiteralPath (Join-Path $srvDir "emf\installed.txt")) -and ((Get-Content -LiteralPath (Join-Path $srvDir "emf\emf_core.ini") -Raw) -like "*0x1111222233334444*")) "EMF's files are in the server folder, recorded in emf\installed.txt, and the chat settings are kept"
    } finally {
        Remove-Item Env:\EMF_TEST_EMF_PACKAGE -ErrorAction SilentlyContinue
        $null = @(Stop-Leftovers)
        foreach ($d in @($srvDir, $pkgDir)) { Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue }
    }

    # 10. the Saves tab against fake save folders (EMF_TEST_STEAM_ROOT, EMF_TEST_SAVED_GAMES) and its own app
    # data (EMF_TEST_APPDATA): the starter backup at the first start and the list, Back up now with a label, restoring
    # one Local file (and the Steam warning when a Steam file is chosen), Delete cancelled then confirmed (the starter's
    # extra line, Cancel has the keyboard), and the refusal while a stand-in game runs. Never a real save folder.
    $savesRoot = Join-Path $realTemp ("emf app smoke saves " + [guid]::NewGuid().ToString("N").Substring(0, 8))
    $fakeSteam = Join-Path $savesRoot "Steam"
    $fakeSaved = Join-Path $savesRoot "Saved Games"
    $savesData = Join-Path $savesRoot "AppData EMF"
    $remote = Join-Path $fakeSteam "userdata\111\1203620\remote"
    $localDir = Join-Path $fakeSaved "keen games\Enshrouded\default"
    foreach ($f in @("character0.ksd", "world0.ksd", "global.ksd")) { New-Item -ItemType Directory -Force $remote | Out-Null; [System.IO.File]::WriteAllText((Join-Path $remote $f), "steam $f") }
    foreach ($f in @("character0.ksd", "world0.ksd")) { New-Item -ItemType Directory -Force $localDir | Out-Null; [System.IO.File]::WriteAllText((Join-Path $localDir $f), "local $f") }
    [System.IO.File]::WriteAllText((Join-Path $fakeSaved "keen games\Enshrouded\local_settings.json"), "{}")
    $store = Join-Path $savesData "save-backups"
    $localWorld = Join-Path $localDir "world0.ksd"
    function Get-SaveHashes { Get-ChildItem -LiteralPath $fakeSteam, $fakeSaved -Recurse -File | ForEach-Object { "$($_.FullName)=$((Get-FileHash -LiteralPath $_.FullName).Hash)" } }
    function Get-StoreKinds { @(Get-ChildItem -LiteralPath $store -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }) }
    $env:EMF_TEST_STEAM_ROOT = $fakeSteam
    $env:EMF_TEST_SAVED_GAMES = $fakeSaved
    $env:EMF_TEST_APPDATA = $savesData
    $W = "var W=function(c,f,n){n=n||0;if(c()){f();}else if(n<150){setTimeout(function(){W(c,f,n+1);},100);}else{f();}}; var pr={}; window.emfProbe=pr; var rows=function(){return [...document.querySelectorAll('.save-row')];}; var kind=function(r){return r.querySelector('.save-kind').textContent;}; var txt=function(id){return (document.getElementById(id)||{}).textContent||'';};"
    try {
        $hashes = @(Get-SaveHashes)
        # the starter at the first start, the list, Back up now
        $js = $W + " W(function(){return rows().length>=1;},function(){ pr.first=rows().map(function(r){return kind(r)+'|'+((r.querySelector('.save-label')||{}).textContent||'')+'|'+r.querySelector('.save-sub').textContent;}); pr.policy=txt('saves-policy'); pr.total=txt('saves-total'); document.getElementById('saves-label').value='smoke label'; document.getElementById('saves-backup').click(); W(function(){return rows().length>=2;},function(){ pr.after=rows().map(function(r){return kind(r)+'|'+((r.querySelector('.save-label')||{}).textContent||'');}); pr.total2=txt('saves-total'); window.emfDone=true; }); });"
        $run = Invoke-Smoke -Eval $js -More @("--page", "saves", "--probe-wait", "15000")
        $ps = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        Check ($run.Code -eq 0 -and $null -ne $ps -and @($ps.first).Count -eq 1 -and $ps.first[0] -like "Starter|Before EMF|Steam: character slot 1, world slot 1 * Local: character slot 1, world slot 1") "Saves: the first start made the starter backup, listed with what it holds ($(if ($ps) { $ps.first -join ' / ' }))"
        Check ($null -ne $ps -and $ps.policy -like "*newest 10 are kept*" -and $ps.policy -like "*kept until you delete them*" -and $ps.total -like "1 backup, * in all, in *") "Saves: the retention line and the total size show ($(if ($ps) { $ps.policy + ' | ' + $ps.total }))"
        Check ($null -ne $ps -and @($ps.after).Count -eq 2 -and $ps.after[0] -eq "Manual|smoke label" -and $ps.total2 -like "2 backups, *") "Saves: Back up now with a label adds a manual backup on top ($(if ($ps) { $ps.after -join ' / ' }))"
        $kinds = Get-StoreKinds
        Check ($kinds.Count -eq 2 -and @($kinds | Where-Object { $_ -like "*_starter" }).Count -eq 1 -and @($kinds | Where-Object { $_ -like "*_manual" }).Count -eq 1) "Saves: the store holds the two backups and nothing half done ($($kinds -join ', '))"
        Check ((@(Get-SaveHashes) -join "`n") -eq ($hashes -join "`n")) "Saves: backing up changed nothing in the save folders"

        # restore one Local file; choosing a Steam file shows the Steam Cloud warning
        [System.IO.File]::WriteAllText($localWorld, "ruined by a mod")
        $js = $W + " W(function(){return rows().length>=2;},function(){ rows()[0].querySelector('.save-restore').click(); W(function(){return !!document.getElementById('saves-pick-some');},function(){ pr.focus=document.activeElement&&document.activeElement.id; document.getElementById('saves-pick-some').click(); W(function(){return !!document.querySelector('input[data-path^=steam]');},function(){ document.querySelector('input[data-path^=steam]').click(); W(function(){return !!document.getElementById('saves-steam-warning');},function(){ pr.steam=txt('saves-steam-warning'); document.querySelector('input[data-path^=steam]').click(); W(function(){return !document.getElementById('saves-steam-warning');},function(){ var w=[...document.querySelectorAll('input[data-path]')].find(function(x){return x.getAttribute('data-path')==='local/default/world0.ksd';}); w.click(); W(function(){var g=document.getElementById('saves-sheet-go'); return g&&!g.disabled;},function(){ pr.go=txt('saves-sheet-go'); document.getElementById('saves-sheet-go').click(); W(function(){return /^(Restored|Not restored)$/.test(txt('saves-sheet-title'));},function(){ pr.title=txt('saves-sheet-title'); pr.restored=txt('saves-restored'); pr.err=txt('saves-restore-error'); W(function(){return rows().length>=3;},function(){ pr.kinds=rows().map(kind); window.emfDone=true; }); }); }); }); }); }); }); });"
        $run = Invoke-Smoke -Eval $js -More @("--page", "saves", "--probe-wait", "15000")
        $pr = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        Check ($null -ne $pr -and $pr.focus -eq "saves-sheet-cancel") "Saves: the restore dialog opens with Cancel holding the keyboard ($(if ($pr) { $pr.focus }))"
        Check ($null -ne $pr -and $pr.steam -like "*Steam Cloud saves*" -and $pr.steam -like "*files on this PC*") "Saves: choosing a Steam file shows the Steam Cloud warning"
        Check ($null -ne $pr -and $pr.go -eq "Restore 1 file" -and $pr.title -eq "Restored" -and $pr.restored -like "*world slot 1 (Local)*") "Saves: one Local file restored through the page ($(if ($pr) { $pr.go + ' | ' + $pr.title + ' | ' + $pr.restored + ' | ' + $pr.err }))"
        Check ((Get-Content -LiteralPath $localWorld -Raw) -eq "local world0.ksd") "Saves: the Local world file is the backup's again"
        Check ((Get-Content -LiteralPath (Join-Path $remote "world0.ksd") -Raw) -eq "steam world0.ksd") "Saves: the Steam files were left alone"
        Check ($null -ne $pr -and @($pr.kinds).Count -eq 3 -and $pr.kinds[0] -eq "Before restore") "Saves: a before-restore backup came first and the restored backup stays ($(if ($pr) { $pr.kinds -join ', ' }))"
        $leftovers = @(Get-ChildItem -LiteralPath $localDir -File | Where-Object { $_.Name -like "*emf-restore*" })
        Check ($leftovers.Count -eq 0) "Saves: no temporary file was left next to the saves"

        # delete the starter: cancelled first (the extra line, Cancel has the keyboard), then confirmed
        $js = $W + " var starter=function(){return rows().find(function(r){return kind(r)==='Starter';});}; W(function(){return rows().length>=3;},function(){ starter().querySelector('.save-delete').click(); W(function(){return !!document.getElementById('saves-sheet-go');},function(){ pr.focus=document.activeElement&&document.activeElement.id; pr.line=txt('saves-delete-starter'); pr.title=txt('saves-sheet-title'); document.getElementById('saves-sheet-cancel').click(); setTimeout(function(){ pr.afterCancel=rows().length; pr.hidden=document.getElementById('saves-modal').hidden; starter().querySelector('.save-delete').click(); W(function(){return !!document.getElementById('saves-sheet-go');},function(){ document.getElementById('saves-sheet-go').click(); W(function(){return rows().length===pr.afterCancel-1;},function(){ pr.kinds=rows().map(kind); window.emfDone=true; }); }); }, 800); }); });"
        $run = Invoke-Smoke -Eval $js -More @("--page", "saves", "--probe-wait", "15000")
        $pd = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        Check ($null -ne $pd -and $pd.focus -eq "saves-sheet-cancel" -and $pd.line -eq "This is your backup from before EMF was installed." -and $pd.title -like "Delete the backup from *?") "Saves: Delete asks first, with the starter's extra line and Cancel as the default ($(if ($pd) { $pd.focus + ' | ' + $pd.line + ' | ' + $pd.title }))"
        Check ($null -ne $pd -and $pd.afterCancel -eq 3 -and $pd.hidden) "Saves: Cancel deletes nothing"
        Check ($null -ne $pd -and @($pd.kinds).Count -eq 2 -and -not (@($pd.kinds) -contains "Starter") -and @(Get-StoreKinds | Where-Object { $_ -like "*_starter" }).Count -eq 0) "Saves: Delete, confirmed, removes the starter backup ($(if ($pd) { $pd.kinds -join ', ' }))"

        # while a stand-in game runs from the folder: Back up now and Restore are locked, and the host refuses a restore
        [System.IO.File]::WriteAllText($localWorld, "changed while the game runs")
        $stand = Start-Process -FilePath (Join-Path $game "enshrouded.exe") -ArgumentList @("-n", "30", "127.0.0.1") -WindowStyle Hidden -PassThru
        try {
            $js = $W + " W(function(){return rows().length>=1&&!!document.getElementById('saves-busy');},function(){ pr.busy=txt('saves-busy'); pr.locked=document.getElementById('saves-backup').disabled; var r=rows().find(function(x){return kind(x)==='Manual';}); r.querySelector('.save-restore').click(); W(function(){return !!document.getElementById('saves-sheet-go');},function(){ pr.goLocked=document.getElementById('saves-sheet-go').disabled; pr.warn=txt('saves-restore-busy'); send({cmd:'saves_restore', id:r.getAttribute('data-id'), paths:['local/default/world0.ksd']}); W(function(){return txt('saves-sheet-title')==='Not restored';},function(){ pr.err=txt('saves-restore-error'); window.emfDone=true; }); }); });"
            $run = Invoke-Smoke -Eval $js -More @("--page", "saves", "--probe-wait", "15000")
        } finally {
            if (-not $stand.HasExited) { $stand.Kill() }
            $null = $stand.WaitForExit(5000)
        }
        $pb = if ($run.R -and $run.R.probe) { $run.R.probe.probe } else { $null }
        Check ($null -ne $pb -and $pb.busy -like "Enshrouded is running*" -and $pb.locked -and $pb.goLocked -and $pb.warn -like "Enshrouded is running*") "Saves: while the game runs, Back up now and Restore are locked ($(if ($pb) { $pb | ConvertTo-Json -Compress } else { 'the page script did not run' }))"
        Check ($null -ne $pb -and $pb.err -like "*closed*") "Saves: and the host refuses a restore sent anyway"
        Check ((Get-Content -LiteralPath $localWorld -Raw) -eq "changed while the game runs") "Saves: the save file stayed as it was"
    } finally {
        foreach ($n in @("EMF_TEST_STEAM_ROOT", "EMF_TEST_SAVED_GAMES", "EMF_TEST_APPDATA")) { Remove-Item "Env:\$n" -ErrorAction SilentlyContinue }
        Remove-Item -LiteralPath $savesRoot -Recurse -Force -ErrorAction SilentlyContinue
    }

    # 7. without the WebView2 runtime (WebView2 looks in a folder that does not exist) EMF.exe says so and exits 1; a
    # smoke run writes the reason to out.json instead of showing the message box
    $env:WEBVIEW2_BROWSER_EXECUTABLE_FOLDER = Join-Path $game "no webview2 here"
    try {
        $run = Invoke-Smoke
    } finally {
        Remove-Item Env:\WEBVIEW2_BROWSER_EXECUTABLE_FOLDER
    }
    Check ($run.Code -eq 1 -and $run.Json -match "WebView2") "without the WebView2 runtime EMF.exe says so and exits 1 ($($run.Code): $($run.Json))"
} finally {
    if ($Keep) { "fake game folder kept: $game" } else { Remove-Item -LiteralPath $game -Recurse -Force -ErrorAction SilentlyContinue }
    $env:TMP = $oldTmp
    $env:TEMP = $oldTemp
    # this run's %TEMP%: WebView2's processes may hold files there for a few seconds after EMF.exe ended (a busy PC)
    for ($i = 0; $i -lt 20 -and (Test-Path -LiteralPath $runTemp); $i++) {
        Remove-Item -LiteralPath $runTemp -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $runTemp) { Start-Sleep -Milliseconds 500 }
    }
    # what earlier runs left (this script's, or older ones' WebView2 folders in the shared %TEMP%): only when an hour
    # old, and a WebView2 folder only when no process has its pid, so never a folder a smoke test running now uses
    $stale = (Get-Date).AddHours(-1)
    Get-ChildItem -LiteralPath $realTemp -Directory -ErrorAction SilentlyContinue |
        Where-Object {
            $_.LastWriteTime -lt $stale -and ($_.Name -like "emf smoke tmp *" -or
                ($_.Name -match '^emf-smoke-webview-(\d+)$' -and -not (Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue)))
        } |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
}
if ($script:fails) { "app smoke test: $($script:fails) failed"; exit 1 }
"app smoke test: all passed"
exit 0
