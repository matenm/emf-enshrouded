# EML content in the loader: the shadow copy, the build at the held start, the redirect

EMF's `dinput8.dll` can apply the content of EML mods (mods made for EML, the Enshrouded Mod Loader) without a separate
EML install. It builds them with `emf\emf_content.dll` into a copy of the game's data and sends the game's data opens
there (the redirect: [file_redirect_spike.md](file_redirect_spike.md)). The game's own files are never written.

Status: the loader side is in place and tested against stand-ins. It does nothing until `emf_content.dll` is installed
next to `emf_core.dll`; that DLL is built separately (it contains EML, so it is GPL). Tested so far with the real DLL:
81 MB of patches, the largest Lua file 8.5 MB.

Code: `src/loader/` — `content_owner.h` (the decision), `content_base.h` and `kfc_header.h` (the base), `eml_mods.h`
(the mods), `content_fingerprint.h` and `content_state.h` (what was built), `content_shadow.h` (the copy),
`content_build.h` and `content_abi.h` (the DLL call), `content_start.h` (the order), `content_splash.h` (the window and
the worker), `entry_hold.h` (the hold), wired in `dllmain.cpp`. The core's side: `src/core/content_status.h`. Tests:
`tests/test_content_*.cpp`, with a stand-in DLL (`tools/standin_content`) and stand-in games (`tools/standin_game`:
MSVC's start-up, Steam's DRM stub, another tool's jump at the entry point).

## Who applies the content (every start)

The checks, in this order; the first that says no decides, and the loader writes one line to `emf\emf_loader.log`
(`EML content: ...`):

1. `[content] mode=off` in `emf\emf_core.ini`.
2. EMF.exe's "Play without any mods". ("Play without EMF mods" keeps EML content: the redirect goes in without EMF's
   core.)
3. Not the game: only `enshrouded.exe` and the dedicated server, `enshrouded_server.exe` (below), get EML content, no
   other program that loads the DLL.
4. Another tool applies it: a `dbghelp.dll` in the game folder (EML's or ShroudForge's proxy), or EML's or
   ShroudForge's `dinput8.dll` in `dinput8-chain.ini` (by the names EMF's installer gives them, `eml.dll` and
   `shroudforge.dll`; under another name by its text). `mode=on` never overrides another tool.
5. `emf\emf_content.dll` is not there.
6. No EML mod EMF can build in `<game>\mods\` (below).
7. No base to build from (below).

### The dedicated server

The server decides and builds the same way, with its own files: `enshrouded_server.exe`, `enshrouded_server.kfc`,
`.kfc_resources` and `enshrouded_server_NNN.dat`, the EML mods in the server's own `mods\`, and the copy in
`emf\content\server\` (the client's is `emf\content\client\`; neither name ever matches the other's files). Its build
also runs on the held entry point, before the chained DLLs, EMF's core and the mods, with these differences: no window
and no message box (a server may run without a desktop, and a box would stop a server nobody watches): its console gets
one line when the build begins and the build's warnings and errors, `emf_loader.log` and `emf\content.log` the rest.
The wait for the build uses no message loop, so the server loads nothing of user32 for it. After the build the server's
own hold for EMF's core applies, as at every server start. Players who join need the same EML mods on their game
(multiplayer reads the same content on both sides); EMF builds the client's copy at their game's start.

The facts are gathered lazily: a start without `emf_content.dll` looks whether the DLL and a `dbghelp.dll` are there,
reads neither of them nor any mod folder, writes its line and goes on exactly as before. A `dbghelp.dll` is read for its
name (`emf\known_tools.txt`) only when `emf_content.dll` is there; without it the line says "applied by dbghelp.dll (EML
or ShroudForge)". The chained DLLs' text is read only just before a build would happen, and a DLL that the last good
build read and that has not changed since (its size and time are in the state) is not read again.

**Usable EML mods:** a folder in `<game>\mods\` whose name does not start with `.`, with a `mod.json` that parses and
has `id`, `name` and `version`, an id no earlier folder has, and `capabilities` that include `patch` and otherwise
only `export`. A mod without `patch` (a tool that only exports game data) is left out: EML runs it only for exports,
which EMF's build turns off, so it would never change anything. A mod that needs EML's runtime loader (`runtime`, `runtime-register-dll`) is left out, and so are `.zip` mods and junctions.
Each one left out gets a line (`EML content: left out <folder>: <why>`) when `emf_content.dll` is there.

**The base** (never patch on top of patched data). The KFC4 header of `enshrouded.kfc` (and of `enshrouded.kfc.bak`,
which EML and ShroudForge make when they patch in place) gives a version string and a container count:

| Header | Field |
|---|---|
| 0 `u32` | magic `0x3443464B` ("KFC4") |
| 16 `u32 + u32` | version: offset from byte 16, length |
| 24 `u32 + u32` | containers: offset from byte 24, count |

No `.bak`, or one of another game build (another version): the base is the `.kfc`. The same version, size and count:
not patched, the `.kfc`. The same version but another size or count: the game's files were patched in place, the base
is the `.bak` and only its containers count. A header that does not read means content off.

## The paths

| Path | When | DllMain | The held entry |
|---|---|---|---|
| Off | any check says no | today's order: chained DLLs, the spike's hooks (only with its switches), EMF's core | not held |
| Current | the fingerprint equals the state's, that build worked, and the shadow is whole | chained DLLs, the redirect, the core | not held |
| Failed before | a build with the same fingerprint failed, or stopped the game twice | chained DLLs, no redirect, the core (it warns) | not held |
| Build | anything else | only the hold of the exe's entry point | the build, then chained DLLs, the redirect (after a good build), the core |

**The entry point.** The game client's entry is Steam's DRM stub (SteamStub): it sits in a section named `.bind` and
starts `call $+5; push rax; push rbx; push rcx; push rdx` (`E8 00 00 00 00 50 53 51 52`). The loader holds that, or
MSVC's usual start-up code (the dedicated server's), by writing 12 bytes over it; another tool's jump there (`E9`,
`FF 25`, a `mov`/`jmp`) is never held: that tool holds the start itself, and content is then off for that start, with
the reason, the order is today's, and the player is warned.

**The build, on the held entry** (the main thread, after every `DllMain` and the exe's TLS callbacks, before any of
the game's code):

1. The entry point's 12 bytes go back first: a chained DLL that patches the entry itself finds the game's own bytes.
2. A small topmost window that never takes the focus, scaled to the screen's DPI: "EMF is preparing EML content (first
   start after a change)..." and the build's last log line.
3. A state that is never current (`code -7`, "a build was running", how many starts in a row began it), so a crash or a
   kill from here on can never leave a shadow that counts as current.
4. On a worker thread with a 256 MB stack (EML's Lua and parsers recurse deep on big mod files; `emm.exe` ended with
   a stack overflow on packs with 22-31 MB Lua files), while the main thread only keeps the window drawn: the shadow
   copy (below), then `emf_content.dll`: `LoadLibrary`, `emf_content_abi() == 1`, one `emf_content_build` call with a
   1 MB reply buffer, a check that no thread started in the DLL still runs (`module_threads.h`), `FreeLibrary` on the
   same thread, and a check that it is gone.
5. The state; after a good build (code 0, the reply whole, the DLL unloaded and not in the process any more) the
   shadow's `shadow.txt`, written from the record taken before the copy. The window goes.
6. A good build: the chained DLLs, the redirect, EMF's core. A failed build (or a cut reply, whose result is unknown):
   no `shadow.txt`, the game starts on its own data, the chained DLLs and the core start, the reason goes to the log,
   the state and the player.
7. The DLL still loaded (a thread of its own runs, or the module is still there), or the build not back within 15
   minutes: neither the chained DLLs nor EMF's core start at this start, a message box says "EMF could not finish
   preparing EML content; the game starts without EMF's mods this time" (OK only), and the log says why. After a
   timeout the worker never loads the DLL any more (it checks right before loading), so the DLL cannot run while the
   game starts Steam.
8. The game's entry point runs.

**After a failure.** The state keeps the fingerprint and the outcome: the next start with the same files does not
build again; the core warns at every such start ("EMF tries again when a mod, the game or emf_content.dll changes").
A build the game stopped in (a crash or a kill: the state is still "running") is tried once more, then left alone.

**Licence rules this keeps** (EMF stays MIT; only `emf_content.dll` is GPL): it is loaded only on the held entry,
before any chained DLL, EMF's core or a mod; it is unloaded before they start, or they do not start; it is never loaded
when the shadow copy is current or the same files failed before; nothing in EMF's core, EMF.exe or the API loads it.
Windows' crypto is never called while the game starts up (the 2026-11-01 hang): the fingerprint is sizes and times
hashed with EMF's own SHA-256, and the DLL runs only while the main thread waits at the entry point.

## The shadow copy: `<game>\emf\content\client\`

| Name | What |
|---|---|
| `enshrouded.exe`, `enshrouded_000.dat` .. `_(N-1).dat` | hard links to the game's files (N: the base's containers); EML reads its types from the exe |
| `enshrouded.kfc`, `enshrouded.kfc_resources` | private copies of the base; EML patches them in place and keeps its own `enshrouded.kfc.bak` |
| `enshrouded_N.dat` and up | containers EML writes (deleted before each build) |
| `mods\<folder>\` | real folders with copies of the usable mods (EML leaves junctions out) |
| `.cache\` | EML's own (types, Lua definitions; removed before each build, the build is forced) |
| `shadow.txt` | the record of the game's files the redirect checks at every start: taken before anything is copied, written only after a good build |
| `emf_content_state.json` | what the last build did |

Before each build the loader removes the index files, `shadow.txt`, every `.tmp`, `.cache\`, `mods\` and every
container at N or past it; a link is only ever deleted, never opened for writing (a write through it would change the
game's file). A link whose file is no longer the game's (Steam replaced it) is made again. The DLL refuses to build when
one of the index files has a second name. Every file the loader writes is written as `.tmp` and renamed, and EMF writes
or deletes its own files there only while no folder on the way is a junction. No hard links (exFAT, another drive):
content is off with that reason; the 55 GB are never copied. A current shadow must still be whole: its copies of the
index files there, and its exe and container links still the game's files (Steam can replace a container without
touching the `.kfc`); otherwise it is built again.

## The fingerprint and the state

`emf_content_state.json` next to the shadow:

```json
{"fingerprint": "sha256:...", "code": 0, "message": "...", "mods": ["beasts 1.0.0"], "errors": [], "warnings": [],
 "ms": 28000, "built_at": "2026-11-01T20:00:00Z", "unloaded": true, "truncated": false,
 "chain_checked": ["creative.dll\t1000\t1340..."], "attempts": 0, "reported": true}
```

The fingerprint covers, by size and last write time (and names relative to the game folder, never the folder's own path,
which the loader and EMF.exe may spell differently): the base (and which file it is), the game's own `.kfc`,
`.kfc_resources` and exe, the base's container count and the game folder's `.dat` count, `emf_content.dll`, each usable
mod sorted by id (id, version, every file's relative path, size and time), and EMF's version. The shadow is current
only when the state's fingerprint equals this start's, its build worked (code 0, unloaded, not cut) and the shadow is
whole. A Steam update changes the fingerprint: the next start rebuilds from the new base (and the redirect's own check
refuses the old copy anyway). `reported` says whether the core was told of the outcome (it was not when the DLL stayed
loaded: then it is told, and backs up the saves, at the next start).

## What the player sees

The loader tells EMF's core how it went through the environment variable `EMF_CONTENT_STATUS`
(`<kind>\t<mods>\t<ms>\t<message>`, the message at most 512 bytes, set only in the game's process; the loader clears an
inherited one at every start, the core clears it once read):

| Kind | The core |
|---|---|
| `built` | console: `EML content: 3 mods (built in 28 s)` |
| `current` | console: `EML content: 3 mods (unchanged since the last build)` |
| `built` or `current` with a message | a good build whose mods warned: the message is the first warning and how many more, from the state's `warnings`. The console line ends `; a mod reported a problem: <warning>`, and on screen once the first world loads: `An EML mod reported a problem while its content was built: <warning>. The rest of the content is in; emf\content.log has every line.` On `built`, also " Your saves were backed up." and a save backup first ("before a start with part of the EML content missing", not held back by `min_minutes`: a missing template removes its placed objects when a world loads); `current` repeats the lines without a second backup. EMF.exe's card does not show the warnings yet. |
| `failed` | console, and on screen once the first world loads: `EML content could not be built: <why>. The game runs without it. Your saves were backed up.`; a save backup first ("before a start without EML content"), not held back by `min_minutes` (mod items vanish when a world loads without their mod) |
| `failed_again` | the same warning with "EMF tries again when a mod, the game or emf_content.dll changes.", no second backup |
| `off` | only with `mode=on`: `EML content is off (<why>)` in the console |

Before a start, EMF.exe's EMF tab shows the same with its card **EML content** (`app/content_panel.h`,
[docs/app.md](../app.md#eml-content)). It runs these rules on the folder for the next "Play" with the loader's own
headers: the facts in the order above, `decide_content`, the fingerprint (with the installed `emf_core.dll`'s version)
and the state as a start judges them, and the redirect's `shadow.txt` check. So its status is what the next start will
do:

| The card | The next start |
|---|---|
| **Ready** | Current: the redirect, no build (`current`) |
| **Builds at the next start** | Build: never built, a mod new or changed, the game or `emf_content.dll` changed, the copy gone or not matching, a build the game stopped in (tried once more). The state keeps only the fingerprint, so the cause is named as far as the state's mods and the files' times tell |
| **Failed** | Failed before: the same files failed, or a build stopped the game twice (`failed_again`) |
| **Off** | the decision's reason (`mode=off`, no EML mod EMF can build, no base) |
| **Applied by <tool>** | another tool applies the content |
| **Not installed** | no `emf\emf_content.dll` |

It also lists the usable mods and the ones left out with this page's reasons, the last build from the state, the copy's
own bytes (files with one name; its links to the game's files are not counted) and a base that is the `.kfc.bak` (the
game's files were patched in place: Steam's "Verify integrity of game files" is advised, EMF never restores them). Its
actions are refused while the game runs: `[content] mode`, deleting the state (as below), deleting the shadow with the
loader's `remove_tree` (a link is deleted, never opened; only while `shadow_path_safe`), and E5's take-over (offered
only when it can work: `emf_content.dll` there, `mode` not off, no EML or ShroudForge DLL chained; it first lists the
EML mods that would stop working, such as those that need EML's runtime loader): EML's
`dbghelp.dll` (by `known_tools.txt`; never ShroudForge's) becomes `eml-dbghelp.dll`, recorded in `emf\moved.txt` as
`set-aside<TAB>eml-dbghelp.dll<TAB>dbghelp.dll` so that EMF's uninstall puts it back; the card gives it back too. EMF.exe
never loads `emf_content.dll` (a test pins it). A dedicated server's folder gets the same card, for the server's own
files and its copy in `emf\content\server\`.

## Settings and logs

```
[content]
mode=auto      ; auto (the default): EMF builds EML content when nothing else applies it; on: the same, and the
               ; console says why when it is off; off: never
log_opens=1    ; the redirect spike's diagnostic log of the exe's file calls (file_redirect_spike.md)
```

- `emf\emf_loader.log`: the decision, the path, the build's outcome (one line when content is off).
- `emf\content.log`: what a build did, the DLL's own lines included; emptied at each build.
- `emf\file_opens.log`: with the redirect on, every open of a content file (`=> "<shadow path>"`).
- `[content] redirect_test=1` (the spike's manual switch with `content_shadow.ps1`) still works while EMF does not own
  the content; once it does, the content state decides.
- To make EMF try a failed build again without changing anything: delete `emf\content\client\emf_content_state.json`.
