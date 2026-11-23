# EMF: Enshrouded Modding Framework

Native code mods for Enshrouded (known builds 1269579, 1273043, 1283531, 1285601 and 1287369; other builds run on signatures only), in the spirit of BepInEx + Jotunn.
EMF loads your mod DLLs into the game, finds engine functions by byte signature, and gives mods a small,
stable C API to read and change the game's ECS (entities, components, singletons) at a safe point every tick.

Status: version 0 (the exact version is in `VERSION`), experimental. Runs in your own game, a game you host, and a dedicated server ([docs/modding/server.md](docs/modding/server.md)). Tested in a local single-player world (host) and on a dedicated server.

**Release package:** `powershell -File scripts\package.ps1` (after `bash scripts/test.sh`) builds `build\package\EMF-<version>.zip`: players run its `install.cmd`; modders get the SDK, a mod template (`template/`), the docs and the mod sources. `scripts/test_package.ps1` (run by `verify.sh`) checks the install scripts and builds the template from the unzipped package.

**Mod manager:** `EMF.exe` lists every installed mod (EMF, EML/ShroudForge, DLL mods EMF starts, other loaders), switches them with the game closed, saves their settings, keeps mod profiles (the switches under a name, to switch setups in one click, and to share a modlist), says which ones will not load and why, shows clashes and starts the game, also once without EMF mods or without any mods (to join a public game; the arrow next to Play): [docs/app.md](docs/app.md#play). The package installs it as `<game>\emf\EMF.exe`.

**Writing your own mod?** Lua: [docs/modding/lua.md](docs/modding/lua.md) (no compiler, live reload). C++: start with [docs/modding/README.md](docs/modding/README.md): a step-by-step first mod, a guide to how the
game and EMF work, a cookbook of tested recipes, the full API reference, and what to do after a game update.

## What is in the repo
| Path | What |
|---|---|
| `src/common/` | scanner, signatures (37), code offsets (5), MinHook wrapper, Keen hash map reader, safe world reader, logger |
| `src/probe/` | `emf_probe`: read-only test DLL (logs worlds, player position, villager states) |
| `src/loader/` + `src/proxy/` | `dinput8.dll` loader: forwards DirectInput to Windows, starts `emf/emf_core.dll`; builds EML content into a shadow copy when `emf/emf_content.dll` is installed (`content_*.h`) |
| `tools/standin_game/`, `tools/standin_content/`, `tools/standin_mod/` | stand-ins for the game exe, `emf_content.dll` and a chained DLL mod, for the loader's tests (never shipped) |
| `src/core/` | `emf_core.dll`: hooks the ECS sim step, runs mod callbacks, program hooks, mod programs (with engine events), spawn, items, save/load events, developer commands, the in-game console (`overlay/`: Dear ImGui through a Vulkan layer), loads mods |
| `app/` | `EMF.exe`, the desktop mod manager: a WebView2 page in a Win32 window ([docs/app.md](docs/app.md)) |
| `src/saves/` | save backups: the engine shared by EMF.exe's Saves tab and the core's automatic backups and `saves.*` commands, and `save-info.json` ([docs/saves.md](docs/saves.md)) |
| `src/content/` | `emf_content.dll` (Rust, built by CMake with cargo): a C ABI around EML's mod loader that builds the content of a game folder's EML mods; nothing in EMF loads it yet ([src/content/README.md](src/content/README.md)) |
| `third_party/eml/` | the pinned copy of EML's library crates (v0.2.0) that `emf_content.dll` is built from, with EMF's patches listed in `EMF_PATCHES.md` |
| `sdk/include/emf/` | mod SDK: `emf_api.h` (C API v32), `emf.hpp` (C++ helpers), `generated/components.hpp` (661 components, 65 dynamic halves, 129 events) |
| `mods/sample_mod/` | example EMF mod (see below) |
| `mods/companion/` | F6: a companion that follows you, fights at your side, takes orders (stay, attack) and wears gear you give it; saved per world (see below) |
| `mods/clock/` | the time of day on screen (read-only, on by default) |
| `mods/quick_stack/` | Quick Stack and Chest Machines: tap L to put what you carry into nearby chests that already hold it; hold L at a chest for sorting chests, recyclers and overflow chests; `/qs` for every player (see below) |
| `mods/villager_jobs/` | K: villagers farm, tend animals, haul between chests, craft (or trade) at a chest and gather wild pickables around it, assigned with a radial wheel; saved per world (see below) |
| `mods/big_stacks/` | bigger item stacks (x4, at most 9999) by changing the game's item definitions in memory |
| `mods/raids/` | Night Raids: at nightfall, by chance, waves of Vukah march on a base where players are; off in each world until `raids.world on` (see below) |
| `mods/loot_magnet/` | Loot Magnet: the loot of kills and what lies near a player (items on the ground, collectibles; by choice mushrooms, fireflies, wild plants, chests in ruins, barrels to smash) comes to them, for every player on the host or a dedicated server; `/loot.toggle` (see below) |
| `mods/champion/` | Wandering Champion (Lua): a roaming mini-boss appears 80-150 m away at night; the screen shows where; a bonus for the slayer (see below) |
| `mods/bounty/` | Bounty Board (Lua): a Notice Board in a base posts kill, gather and champion bounties; take them there (E for the host, the chat for everyone else) and claim the rewards there (see below) |
| `mods/grave/` | Grave Keeper (Lua): `/grave.where` finds your tombstone, `/grave.claim` brings its items back from a distance (see below) |
| `mods/sleep/` | Sleep Through the Day: sleeping in a bed by day skips to the evening (see below) |
| `mods/herald/` | Server Herald (server side, off by default): a message of the day, join/leave/death lines, announcements, a restart countdown, a chat log, playtime, `/players`, an opt-in Discord webhook (see below) |
| `mods/handover/` | Hand-over (Lua, off by default): `/handover wood 20` gives items to the nearest other player; nothing is ever lost; `/handover.block` refuses them (see below) |
| `mods/teleports/` | Waypoint Teleports: `/unstuck` and `/home` for every player, `tp`, `bring`, `tp.pos`, `tp.back` for admins (see below) |
| `mods/bulk_craft/` | Bulk Craft (off by default): craft one by hand, then F8 at the station (or `/craft 25`, every player) crafts many through the game's own crafting, at the recipe's pace (see below) |
| `mods/blessings/` | Flame Blessings (off by default): god mode, endless health, stamina and mana, no fall damage, per player and at once; the host's own in the Mods tab or on the F11 wheel, `/blessings` for admins on any player; shown on screen, told in chat, nothing saved (see below) |
| `mods/beastmaster/` | Beastmaster (Lua, off by default): tame wolves, boars, sabretooths and hounds (bears, aurochs and snakes with the EMF creature packs, which EMF builds itself), keep 8, call as many as your active slots (1-4, by level), choose them at a stable (the game's animal bed: E); hold F3 for the beast wheel, `/beastmaster` for every player; rare wild pack beasts (see below) |
| `mods/necromancer/` | Necromancer (Lua, off by default): raise up to 3 skeleton warriors that stay and follow you (a lich, a bone golem and bone wolves with the EMF creature packs, which EMF builds itself), cast raging spirits (flaming skulls, 6 s); hold F2 for the summoning wheel, shared with Beastmaster; `/necromancer` for every player (see below) |
| `mods/minimap/` | Minimap (Lua sample, off by default): the game's own map around you in a corner of the screen, north up, your arrow turning with you; F10 shows or hides it, Numpad + and - zoom; built on the canvas API as an example to copy (`mods/minimap/README.md`) |
| `mods/loot_scale/` | more materials per loot stack (x2 by default, off by default) by hooking the game's loot amount getter by its bytes; plants picked with E, chopping and mining (which never read that setting) get the difference after the game's PickupEvent, each with its own factor (0 = the same, so from 1.1.0 they follow the main factor); switching it off takes effect at the next start; factor 1 is the world's amounts at once for everything that follows it |
| `content/emberbrand/` | an example of a new item (an EML data patch; [docs/modding/content.md](docs/modding/content.md)) |
| `mods/event_log/` | logs gameplay events on the host (deaths, hits, pickups, crafts, xp, building) with `on_event` |
| `mods/guid_dump/` | EML (Lua) export mod: writes every template name → GUID to `export/template_guids.csv` (12,266 templates) |
| `tools/sigcheck/` | offline check that every signature matches the installed exe |
| `tools/gen_headers.py` | regenerates `components.hpp` and `reflection.hpp` from the game's own reflection data in `enshrouded.exe` (`tools/re/exe_types.py`; EML's `types.json` still works) |
| `tools/re/` | the static-analysis scripts used to build the engine map |
| `tools/bridge/` | the debug bridge's Python client, the scenario runner (`emf_scenario.py`) and the fake game for tests ([docs/modding/ai_agents.md](docs/modding/ai_agents.md)) |
| `tools/mcp/`, `.mcp.json` | the MCP server over the debug bridge, for AI coding assistants and other MCP clients |
| `tools/gen_llms_txt.py` | regenerates `docs/modding/llms.txt`, the API summary for AI coding assistants |
| `tools/nexus/nexus_publish.py` | the Nexus release tool: uploads a release's zips, page text and changelog through the API v3, a dry run unless `--yes` (the release config is described at the top of the file) |
| `tests/game/`, `scripts/game_session.ps1` | in-game test scenarios, and the script that starts and ends a test session ([AGENTS.md](AGENTS.md)) |
| `docs/ENGINE_MAP.md` | everything known about the engine (addresses, signatures, layouts, facts confirmed in game) |

## Build
Needs Visual Studio Build Tools 2022 (C++ workload) and CMake + Ninja.
```
scripts\dev.cmd cmake --preset msvc
bash scripts/verify.sh        # builds, runs the unit tests, checks the 38 signatures and 5 code offsets against the game exe
```
Outputs: `build\msvc\dist\` (framework, mirrors the install layout) and `build\msvc\probe\dinput8.dll` (probe).

## Install / uninstall (game closed)
```
powershell -ExecutionPolicy Bypass -File scripts\install_emf.ps1                # framework + sample mod (read-only)
powershell -ExecutionPolicy Bypass -File scripts\install_emf.ps1 -EnableWrite   # + the hotkey write tests, the companion and villager jobs (host only)
powershell -ExecutionPolicy Bypass -File scripts\uninstall_probe.ps1            # removes dinput8.dll + emf\ (archives logs to analysis\runs)
```
The developer scripts (these, `install_probe.ps1`, `content_test.ps1`, `game_session.ps1`) and `verify.sh` find the
game through Steam's library folders (`scripts\find_game.ps1`); `-Game <folder>` or `EMF_GAME_DIR` names another one.
`install_emf.ps1` (developer install) refuses to overwrite a `dinput8.dll` that is not EMF's. The player installer
(`package/install.ps1`) renames another mod's `dinput8.dll` instead, lists it in the `[EMF]` section of
`dinput8-chain.ini` and records it in `emf\moved.txt`; EMF's loader starts every DLL listed in `dinput8-chain.ini`
(`[Chain]` and `[EMF]`) from its `DllMain`, before the core
(`src/loader/chain_list.h`, `chain_load.h`; results in `emf\emf_loader.log` and `emf\chain_status.txt`).
Before that the loader takes EMF.exe's one-shot flag `emf\play_once.txt` ("Play without EMF mods" or "without any
mods", `src/loader/play_once.h`): it deletes the file at once and, for that one start, starts no core (and with "without
any mods" no chained DLL either). A flag older than ten minutes, one it cannot delete and the dedicated server are
ignored, so EMF never stays off.
EML content (the loader, `src/loader/content_*.h`, [docs/engine_parts/content_loader.md](docs/engine_parts/content_loader.md)):
with `emf\emf_content.dll` installed and EML mods in `<game>\mods\` that no other tool applies, the loader builds them
into a shadow copy of the game's data (`emf\content\client\`, hard links and copies; the game's files are never written)
and sends the game's data opens there. A start that needs a build holds the game's entry point, builds (a small window
says so, about 30 s), unloads the DLL, and only then starts the chained DLLs and the core; a start where nothing
changed loads no DLL. Without `emf_content.dll` it writes one line and does what it did before. `[content] mode=auto`
(the default), `on` (the console also says why content is off) or `off` (never) in `emf\emf_core.ini`; the build's own
log is `emf\content.log`.
EML content in EMF.exe ([docs/app.md](docs/app.md#eml-content)): the EMF tab's card **EML content** says who applies the
EML mods' content at the next start and why, by the loader's own rules (it reads sizes, times and the last build's
record; it never loads `emf_content.dll` and never writes a game file): **Ready** (the copy is current: the next start
uses it without building), **Builds at the next start** (and why: never built, a mod new or changed, the game or
`emf_content.dll` changed, the copy gone), **Failed** (the same files failed before, with the message: EMF tries again
when a mod, the game or `emf_content.dll` changes), **Off** (the loader's reason), **Applied by EML** or **ShroudForge**,
or **Not installed**; a dedicated server's folder gets the same card for the server (its copy is
`emf\content\server\`, and the server's console says when it builds at its start). The card lists
the mods EMF builds and the ones it leaves out with the loader's reason (the Mods tab says "loaded by EMF" or "left
out" with the same reason), the last build (when, how long, its errors) and the copy's disk use, and `emf\content.log`
is the log viewer's third choice. Its actions, all refused while the game runs: **When to apply** (`[content] mode`),
**Build again at the next start** (deletes `emf\content\client\emf_content_state.json`), **Remove the copy** (deletes
`emf\content\client\`: its links are deleted as links, so the game's files stay whole; refused when a folder on the way
is a junction) and, with EML installed, **Let EMF load EML content** (it first lists the EML mods that stop working
until you give EML back, such as those that need EML's runtime loader): EML's `dbghelp.dll` (never ShroudForge's) is
renamed to `eml-dbghelp.dll` and recorded in `emf\moved.txt`. **Give EML's dbghelp.dll back** reverses it, and EMF's
uninstall puts it back too (unless a `dbghelp.dll` is there again: then it stays set aside, and so does its record).
When the game's `enshrouded.kfc` was patched in place by EML or ShroudForge, the card advises Steam's "Verify integrity
of game files"; EMF never restores game files itself.
`docs/modding/other_mods.md` has the details for authors of DLL mods.
Logs: `<game>\emf\emf_core.log` (framework + mods), `<game>\emf\emf_loader.log` (the loader: chain-loaded DLLs and a
core that fails to load).
Mod settings: the console's Mods tab or the `set` command (below). Each mod starts with the defaults from its
`mod.json`; a change is saved in `<game>\emf\config\<mod>.ini`.
In the Mods tab's list, green means the mod is on (a DLL mod that loaded, or a Lua mod handed to the `lua` mod, which
pauses it on an error with a note on screen; `lua.status` shows whether it runs), grey that it is switched off and red
that it did not load (select it to see why); the colours are for this game start, a mod's switch for the next.

### Linux and the Steam Deck (Proton)

`install.cmd` does not run on Linux: copy by hand. Copy everything inside the zip's `game` folder into the game folder,
next to `enshrouded.exe`: `dinput8.dll` and the whole `emf` folder with all its subfolders. One mod's folder alone is not
enough: the Lua mods (Beastmaster, Necromancer, Kill Counter, ...) run through the `lua` mod in `emf\mods\lua`.
The creature packs (a separate download) go into the `mods` folder next to `enshrouded.exe` (with ShroudForge or EML
installed, those load them; otherwise EMF builds them when the game starts).

Proton loads its own `dinput8.dll` unless told otherwise. In Steam, open the game's **Properties**, and under
**Launch options** put:

```
WINEDLLOVERRIDES="dinput8=n,b" %command%
```

Then EMF and its mods start with the game. Since this version EMF also draws under Proton (the console, the HUD
editor, mod windows and wheels): it uses its own Vulkan hook there, because Proton's Vulkan never loads Vulkan layers.
A player on a Steam Deck saw EMF's console and HUD; it is still new there: if nothing EMF draws shows up, send
`emf\emf_loader.log` and `emf\emf_core.log` with your report. The loader's log says which way it took
(`overlay: Wine found: through EMF's Vulkan hook`). `[overlay] path` in `emf\emf_core.ini` picks the way by hand: `auto`
(the default: the hook under Proton, the layer on Windows), `layer` or `hook`.
The Steam Deck has no ` key: give a button that key in Steam Input, or pick another key with `[overlay] key=` (a scan
code, below).
Under Wine or Proton the game window sometimes gets the keys but no typed characters, so the console stays empty. EMF notices after a few key presses and types from the raw keys itself (the log says "the console's characters come from raw input now"). The first characters you typed are not lost.
When Proton cannot turn a key into a character, EMF types the character printed on the key: letters (with Shift and
Caps Lock for A-Z), digits, space and the symbols you type without Shift (`-` `.` `,` `/`). Characters made with Shift
(`_` `!` `?`), AltGr (`@`) or an accent key may not come. If the console still stays empty, add `debug_keys=1` under
`[overlay]` in `emf\emf_core.ini`, start the game, open the console, type a test word such as `help`, and send
`emf\emf_core.log` with your report: it then shows what happened to your first 20 key presses in the console (and only
those, and only in the console). Type nothing private while it is on, and set it back to 0 afterwards.
A dedicated server under Wine needs the same `WINEDLLOVERRIDES` (mods only: a server draws nothing).
EMF.exe runs under Wine too, but its **Play** cannot reach Linux's Steam: start the game from Steam.

## In-game console
Press the key under Esc (`~` on US keyboards, `§` or `|` on Nordic ones, `^` on German ones) in the game: a console
drops down over the top half of the screen. Type a command (see the table below) and press Enter; the reply appears
under it (green when it worked, red when it did not).
- **Suggestions** pop up under the input as you type: the commands that start with what you typed, then those that
  contain it (`reload` finds `lua.reload`), each with its arguments and, in grey, what it does. Once a command and a
  space are typed, a line shows its arguments with the one you are typing marked (`give <item name> [count]`), and the
  list offers what fits there: item names for `give`, `put`, `take` and `stack` (best match first, by the rules of
  `items`), mod ids and setting keys for `settings` and `set` (then the setting's values, such as `on`/`off` or a
  list's choices), component names for `tpl` and `tplset`, and choices such as `[on|off]`.
- **Tab** or **Right** (at the end of the line) takes the highlighted suggestion; grey text after the cursor shows what
  it adds. While no list shows (nothing suggested, or hidden by Esc), Tab completes and cycles through the matches as
  before.
- **Up / Down** move in the list while it shows; otherwise they go through the commands you typed before (kept in
  `<game>\emf\console_history.txt`). A command brought back from there shows no list until you type.
- **Page Up / Page Down** and the mouse wheel scroll back; the console keeps the last 2000 lines (`scrollback`).
- **Ctrl+F** (or **Find**) opens a find line beside the input: the lines that hold its text (any case) get a band and the
  newest one is shown; **Enter** goes to the next older one, **Shift+Enter** to the next newer one ("3 of 12").
- **Copy** (or **Ctrl+Shift+C**) puts the scrollback on the clipboard, one console line per line (only the matching lines
  while the find line finds some), with the times when they show.
- **`clear`** (or `cls`) empties the scrollback; it is the console's own, so it also works on the main menu.
- A time on each line (`21:04:17`) is a setting (`timestamps`, off by default; not in the notes on screen).
- **Esc** closes the find line or hides the suggestions first, then closes the console; the same key closes it at any
  time. Enter runs the line as you typed it (take a suggestion with Tab or Right first).

While it is open the game gets no keyboard or mouse input (only key releases, so nothing stays held) and mods' hotkeys
are paused. On the main menu, `help`, `mods`, `settings`, `set`, `profile`, `hud.*`, `saves.backup`, `saves.list`, `hooks` and `pattern` answer at once; the other
commands answer `not run: load a world you host first`, and in someone else's game `not run: commands need a world you
host`.

### The Mods tab
The console has three tabs, **Console**, **Mods** and **Settings** (click one, or Ctrl+Tab for the next). The Mods tab lists every mod in
`<game>\emf\mods`, coloured by what became of it (loaded, a Lua script, disabled, failed, not loaded); **Filter mods**
above it keeps those whose name, id, description or authors hold the text. Click one to see its
description and status, to switch it on or off ("Switched on", from the next game start) and to change its settings:
checkboxes, sliders, lists and text fields, with the range and the default when you hover over one. Settings marked
"(next start)" apply when you start the game again, the others at once. A setting someone changed has a small
**reset** next to it, which puts it back to its default. The same from the command line:
`settings <mod>` lists a mod's settings with their values, `set <mod> <key> <value>` changes one (`set big_stacks
enabled 1` switches a mod on). A value out of range or of the wrong kind is refused with the reason. Changes are saved
in `<game>\emf\config\<mod>.ini`.

At the top of the Mods tab, **Profile** is the picker for mod profiles (the on/off state of every mod under a name,
the same files as EMF.exe's, [docs/app.md](docs/app.md#profiles)): it shows the current one, or "(changed)" when a
mod it lists is switched another way now; **Load** switches every mod to the chosen profile from the next game
start and shows what it did; **Save as** saves the switches (and, with "with settings", the EMF mods' settings) under
a new name, and asks before it replaces one. The console's `profile` command does the same.

### The Settings tab
EMF's own settings, on four pages: **Text on screen** (the look of HUD lines, notes and wheels, with a preview),
**Console** (its key, mouse speed, fonts, size, colours and suggestions), **HUD** (the HUD editor, below) and
**Start-up** (the log window and the save backups). They apply at once, apart from those marked "(next start)". The console key takes two presses of the
new key (Esc cancels), so a key you cannot press again is never saved. The same from the command line: `settings emf`
lists them, `set emf <section>.<key> <value>` changes one (`set emf hud.size 140`). They are the keys of
`<game>\emf\emf_core.ini` below: a change writes only its own line, and a value you typed by hand that is out of range
is used inside its range but stays in the file as you wrote it. Switching the console off (`[overlay] enabled`), chat
admins and the debug bridge stay in the file and EMF.exe's Server tab.

Settings in `<game>\emf\emf_core.ini`:
```
[overlay]
enabled=1      ; 0 turns the console off
path=auto      ; how EMF draws: auto (the hook under Wine / Proton, the layer on Windows), layer or hook
key=0x29       ; the key's scan code (0x29 = the key under Esc)
chat=1         ; 0 turns chat commands off (below)
suggest=1      ; 0: no suggestions as you type (Tab still completes)
suggest_rows=8 ; at most this many suggestions (3 to 12)
timestamps=0   ; 1: each console line shows the time it came (21:04:17)
scrollback=2000  ; lines the console keeps, 500 to 20000 (from the next start)
debug_keys=0   ; 1: the first 20 key presses in the console go to emf_core.log (for a report when typing fails)
font=consolas  ; the console's font: consolas, cascadia, serif or file:<name>.ttf
tabs_font=serif  ; the tab bar and the Mods tab: serif, consolas, cascadia or file:<name>.ttf
text_size=100  ; the console's text size, 60 to 200 (%)
height=50      ; the console's height, 25 to 90 (% of the screen)
opacity=80     ; how solid its background is, 30 to 100 (%)
background=#080a0e  ; its background colour
accent=#c0b074 ; the tabs, the selection, the scrollbar and the wheels' ring
colour_command=#ffffff  ; the console's lines: commands you type,
colour_reply=#b3bac2    ; replies,
colour_error=#ff7a73    ; errors
colour_success=#7de887  ; and done

[hud]
style=game     ; game (serif, soft shadow, no box), boxed (EMF's earlier look) or plain (the text alone)
font=serif     ; on-screen text and wheels: serif, console (the console's font) or file:<name>.ttf
size=100       ; on-screen text size, 60 to 200 (%)
colour=#fffff4 ; notes, and lines whose mod chose no colour of its own
shadow=1       ; 0: no shadow (style=game)
notes_place=top_centre  ; top_centre (under the compass), top_left or right
notes_seconds=6  ; how long a note stays, 2 to 20 seconds

[hud_editor]
enabled=1                ; 0: EMF leaves the game's HUD alone
show_outlines=1          ; the game's outline around the HUD parts while the HUD page is open
allow_with_shroudhud=0   ; 1: run next to ShroudHud anyway
key_page=F9              ; opens the HUD page; empty: no key
key_reset=Pause          ; puts the whole HUD back; empty: no key
active_layout=Default    ; the layout in use (the HUD page changes it)
```
**The look.** EMF's on-screen text (the clock, Life Skills, Flame Blessings and the other mods' lines, notes such as
Sleep Through the Day's, and the wheels) uses a book serif in warm white with a soft shadow, like the game's own text
(lines and notes in bold); notes show top centre under the compass on a dark band that fades at both ends, at most
three rows each. `[hud]` changes that, `[overlay]` changes the console. Colours are `#rrggbb` or `#rgb`; sizes are a
percentage of the size EMF picks for the screen: on-screen text is the screen's height / 38 (28 px at 1080p), the
console's text its height / 54 (20 px at 1080p), and serif tabs 1.3 times the console's text (26 px at 1080p). The
text is sized again when the window's size has stayed the same for a moment. EMF reads these keys when the game
starts; the Settings tab (or `set emf ...`) changes them while it runs and writes only the line it changed. A value
EMF cannot use falls back to its default and a number out of range is kept inside its range, each with one line in
`emf_core.log`. `style=boxed` alone gives EMF's earlier look: dark boxes at
the console's size, the console's font, `#dbdbcc` and notes top left (the keys you set still apply).

**Fonts.** EMF ships Alegreya (the serif) and Cascadia Mono in `<game>\emf\fonts\` (both under the SIL Open Font
License, in the zip's `licenses\`); Consolas comes from Windows. To use a font of your own, put its `.ttf` or `.otf`
in `<game>\emf\fonts\` and set `font=file:<its file name>`; use only fonts from a source you trust. A font that is
missing or is not a font EMF can read is skipped with a line in the log: then the bundled one, then Consolas, then the
console's built-in font.

**Item names in Japanese, Korean, Chinese and Thai.** EMF's own fonts have no characters for these scripts, and EMF
does not ship any (they are big). Usually you need to do nothing: EMF uses the font Windows 10 and 11 already have
for your game's language (Yu Gothic, Malgun Gothic, Microsoft YaHei, Microsoft JhengHei or Leelawadee UI), and adds
only the characters your item names use. If your Windows lacks it, or you prefer the Noto look, get the free font
(SIL Open Font License) for your language from Google Fonts and put one file into `<game>\emf\fonts\`:

| Game language | Download (free) | The file to put in `emf\fonts\` |
|---|---|---|
| Japanese | https://fonts.google.com/noto/specimen/Noto+Sans+JP | `NotoSansJP-Regular.ttf` |
| Korean | https://fonts.google.com/noto/specimen/Noto+Sans+KR | `NotoSansKR-Regular.ttf` |
| Chinese (Simplified) | https://fonts.google.com/noto/specimen/Noto+Sans+SC | `NotoSansSC-Regular.ttf` |
| Chinese (Traditional) | https://fonts.google.com/noto/specimen/Noto+Sans+TC | `NotoSansTC-Regular.ttf` |
| Thai | https://fonts.google.com/noto/specimen/Noto+Sans+Thai | `NotoSansThai-Regular.ttf` |

On the page, choose **Get font**, then **Download all**; the file is in the zip's `static` folder. Then start the game
again: nothing to switch on. `emf\emf_core.log` says which font EMF uses ("item names: ja_jp on EMF's screens with
..."), or which file to add. Until a font is found, those names show in English on EMF's screens (the console, HUD
lines, wheels and mod windows); the game's chat shows every language by itself. A name with any other character EMF's
fonts lack shows in English too, instead of boxes. (A font of your own, `font=file:...`, must have the letters of your
game's language: EMF does not check those.)

**HDR.** With the game's HDR on, EMF does not convert its colours yet: the screen reads them as HDR values, so
bright ones (the warm white text, channel values above about 150) look very bright. A darker `colour=` (for example
`#969690`) tones the text down.

How it draws: the console is a Vulkan layer (`VK_LAYER_EMF_console`, manifest `<game>\emf\vk\`). EMF's `dinput8.dll`
enables it for the game process only; nothing is written to the registry and other games do not see it. It works next
to RivaTuner/RTSS, the Steam overlay and the Discord overlay.

**The console does not open, or Discord's overlay opens with it?** Discord's overlay lock is often Shift+\` (Shift
and the key under Esc). EMF's console opens on that key with or without Shift, so the two can clash: give
Discord's overlay lock another key in Discord's settings (Game Overlay), or give the console another key with `key=`
above (a scan code, from the next game start).

### The HUD editor
The **HUD** page of the Settings tab moves, scales and hides 19 parts of the HUD while you play. The parts, by the names
the commands use: `vitals` (health, mana and stamina together), `buffs` (buffs and food), `hotbar`, `compass`, `shroud`
(the shroud timer and meters), `quests`, `oxygen` (oxygen and glider), `loot` (loot log), `boss` (boss bar), `crosshair`,
`chat`, `notifications`, `multitool`, the three bars on their own: `health`, `mana` and `stamina`, and the lines mods keep
on screen: `lines_top_right`, `lines_bottom_right` and `lines_bottom_left`. A part
that sits inside another moves and scales with it: `vitals` carries the three bars and `buffs`, `compass` carries
`shroud`, and the `hotbar` carries its item hints and the item name above it. A part's own change adds on top. A part the
game has not drawn yet is marked "not seen yet" (a boss bar shows only in a boss fight; the stamina bar only while your
stamina is not full).

- **Health, mana and stamina** each move and hide on their own, anywhere on the screen (only the vitals part scales
  them). Inside the vitals' area a bar keeps its dark backing; moved far away from it, the game draws the bar without it.
  The vitals' card has **Swap health and mana** and **Swap health and stamina**: the bars stack in the new order with the
  game's own spacing (`hud.swap`, `hud.order`). A hidden bar, or stamina while it is full, leaves no gap. Where the bars
  cannot move on their own (a game build without the bars' drawing call, or after scale stopped), their cards say why
  and their offsets, Hidden and keys are off; the vitals part still moves all three.
- **Status effects grow upward:** a switch on the buffs' card (`hud.grow up`). The list of status effects then sits on the
  food icons' bottom edge and grows up instead of down; it never leaves the screen (at the top edge it stops and the rest
  grows down). With the vitals at the bottom of the screen, move the buffs part above the bars so the list has room.
- **Bar colours:** the Health, Mana and Stamina cards have a colour button each for the **Filled** and the **Empty** part
  (with opacity: an empty part at 0 opacity disappears) and **Game's colours**. The bright end of the fill follows a new
  fill colour. Three more colours per bar are set from the console: `end` (the bright end), `damage` (the part shown while
  the value drops) and `heal` (while it rises), for example `hud.colour health damage #ff8000`. The game's effects (the
  damage blink, the heal glow, the HUD fading out) work with any colour; the red "not enough mana / stamina" flash and the
  ticks stay as the game draws them. The colours follow the bar when the order changes. A card's **Reset** (and R,
  `hud.reset <part>`) moves the part back and keeps its colours (the vitals' Reset also puts the bar order back, the
  buffs' Reset switches grow upward off; `hud.reset` says so); **Game's colours** and **Reset all** put them back.
- **Panel colour:** a switch on the vitals' card for the dark panel behind the bars (`hud.colour panel #20208080`). EMF
  hands the game its colour for that one panel as the game draws it; nothing in the game's data changes. The panel fades
  with the HUD as before, and an opacity of 0 hides it (the bars stay). The panels behind the rested bar and the music
  bar keep the game's colour. On a game build where EMF cannot find the panel's drawing call, the switch and the log
  say the colour is not available.
- **Mods' lines** (`emf.hud`) are three parts, one per corner a mod can use: move, scale (the text size) and hide each
  corner like a part of the game's HUD. While the page is open they show behind it, and an empty corner shows a dim sample
  line so you can place it before a mod uses it.

- **Offsets** are percent of the screen, from where the game draws the part, so a layout fits other screen sizes. The page
  shows the pixels at your resolution next to each value. Tab picks the next part, the arrow keys move it by 0.5 % (Shift:
  5 %), `+` and `-` scale it (not the three bars), H hides it and R resets it.
- **Keys:** F9 opens the page (press it again to close the console) and Pause puts the whole HUD back. Both are
  settings on the page and take one key each.
- **Layouts** are saved under a name in `<game>\emf\hud\<name>.json`, one file each, and are the same for every character.
  The layout bar saves, loads and deletes them, and `hud.layout` does the same from the console. A reset of the whole HUD
  first saves what was there as "Before reset", so `hud.layout load "Before reset"` brings it back.
- **Outlines:** while the page is open, the game's own outline is drawn around each part and the selected one blinks.
- **Scale** goes from 0.5 to 2, around the screen edge the part is nearest. If the game's drawing does not match what EMF
  expects, scale stops for that session and says so (see `emf_core.log`); move and hide go on, and your scale values stay in
  the layout.
- **When it is off:** on a game build EMF does not know the editor leaves the HUD alone and the page says so; the same
  with ShroudHud installed (switch on "Use anyway with another HUD mod" at your own risk), after an error in the game's UI
  code (until the game restarts), and with the "HUD editor" setting off. `hud.reset` and the reset key always work, and the
  dedicated server has no HUD editor at all.

### Keys
EMF's keys by default:

| Key | What it does |
|---|---|
| the key under Esc | the console (`key=` under `[overlay]` above) |
| F2 | Necromancer: the summoning wheel |
| F3 | Beastmaster: the beast wheel |
| F6 | Companion: calls it, then opens its order wheel |
| F7 | Hand-over: the host's wheel |
| F8 | Bulk Craft: the wheel at a crafting station |
| F9 | the HUD editor's page (press it again to close the console) |
| F10 | Minimap: shows or hides it |
| Numpad + / Numpad - | Minimap: zoom in / out |
| F11 | Flame Blessings: the blessings wheel (the host's own; not F4, since Alt+F4 closes the game) |
| Pause | the HUD editor: puts the whole HUD back (the layout before is kept as "Before reset") |
| L | Quick Stack: tap to put things into nearby chests, hold at a chest for its wheel (the game also uses L: voice-over playback) |
| K | Villager Jobs: a job for the villager you look at |
| E | Bounty Board (from 1.2): reads the board you look at (E is also the game's interact key; the board itself has no game action) |
| E | Beastmaster: the Beasts window at a stable that shows no E prompt of its own, looking at it within 4 m (at one with the game's E Open prompt, that prompt opens it) |

The game's own default keys take every letter except K, so EMF's wheels sit on the free F-keys; the game also gets a
mod's key press. Every mod's key can
be changed in the console's Mods tab or in EMF.exe (click the key, then press the new one). When a mod's default key
changes, a key you chose yourself stays.

While the game's chat, its social screen (which holds a second chat) or a rename box is open, mods' keys are paused:
typing K does not open the Villager Jobs wheel, and the console key types its character into the chat instead of
opening the console (close the social screen to open the console from there). A key still held when the chat closes
counts again once you let go of it. The console's `screen` command says which game screen is open and whether mods'
keys are paused. On a game build EMF does not know yet, this check may be off (the log says so at start).

### Commands in the game's chat
You can also type a command into the game's own text chat with a `/` in front: `/give Material_T1_Wood_Logs 5`,
`/time 12`, `/help`. EMF runs it and does not send it, so other players never see it. The reply shows for a few
seconds at the top of the screen (and in the console). Only EMF's command names are taken: any other line, including
a `/` word EMF does not know, goes to the chat as before. The game's text chat only opens in an online session (Enter
does nothing in an offline world); use the console there.

Players in a game you host (with `[chat] players=1` in `emf\emf_core.ini`) and on a dedicated server type commands the
same way. The accounts in `[chat] admins` may run every command; every other player may run the commands mods mark
for players (API v17), such as `/raids.status`, `/loot.toggle`, `/champion.where`, `/qs`, `/craft`, `/bounty.board`,
`/grave.claim`, `/unstuck`, `/home` and `/help` (which lists what they
may run). See `docs/modding/server.md`.

### World settings while you play
In a game you host or on a dedicated server, `world.set <setting> <value>` changes a world setting while the world
runs: `world.set loot 2`, `world.set night_minutes 5`, `world.set weather Rare`. Every player gets the new value
within a second or two. Values outside the game's ranges are refused. A change lasts until the world is loaded again;
add `persist` to keep it for this world. `world.list` shows every setting, `world.get <setting>` its range,
`world.reset` goes back to the world's own values, and `world.export` prints a block for `enshrouded_server.json`
(EMF never edits that file). See `docs/modding/world_settings.md`.

## Developer commands
Type commands into the in-game console (above), or into `<game>\emf\commands.txt` (one per line, `#` starts a comment)
while you are in a world you host.
EMF reads the file about once a second, runs each line once, appends the lines to `commands.done.txt` and deletes the
file. Replies go to `emf_core.log` as `[cmd] ...` lines. For a window to type into, put this in `<game>\emf\emf_core.ini`:
```
[console]
window=1
```
The console window has no close button, and Ctrl+C is ignored, because closing it would end the game; it closes with the
game. (If Windows Terminal is your default terminal app, the console may open there instead, where closing the tab does
end the game; use `commands.txt` then.)

| Command | What it does (host only) |
|---|---|
| `help` | lists the commands, including those mods add |
| `mods` | the loaded mods |
| `settings <mod>` | a mod's settings: value, kind, range, default, and whether a change applies at once (works on the main menu too) |
| `set <mod> <key> <value>` | changes a mod's setting and saves it in `emf\config\<mod>.ini`, like the Mods tab; `set <mod> enabled 0` / `1` switches the mod off / on from the next game start (works on the main menu too) |
| `reset <mod> [key]` | puts one setting, or every setting of the mod (never its on/off switch), back to its default: its line goes from the ini, so a later version's default applies (works on the main menu too) |
| `profile` | the mod profiles in `emf\profiles` (the on/off state of every mod under a name, as EMF.exe's Mods tab keeps them), the current one and whether the mods changed since (works on the main menu too) |
| `profile save <name> [--settings] [--replace]` | saves the switches of every mod (EMF, EML, the DLLs in `dinput8-chain.ini`; others as read-only) as a profile; `--settings` also saves the EMF mods' settings; another profile with that name is replaced only with `--replace` (e.g. `profile save "Big Base" --settings`) |
| `profile load <name>` | switches every mod the profile lists to its state (through the same switches as EMF.exe) and reports what it did, what failed, what is missing (with Nexus ids) and what is not in the profile, and says what will still differ; restart the game to apply. A profile's settings apply at once in the Mods tab. The answer comes a moment later: the work runs on a thread of its own |
| `hud.list` | the HUD editor's state, every HUD part with its offset, scale and whether it is hidden, and the layouts (works on the main menu too) |
| `hud.move <part> <x> <y>` | moves a HUD part, in percent of the screen from where the game draws it (e.g. `hud.move vitals 5 -2.5`) |
| `hud.scale <part> <0.5-2>` | a HUD part's size, around the screen edge it is nearest (e.g. `hud.scale hotbar 0.75`) |
| `hud.hide <part>` / `hud.show <part>` | hides a HUD part / brings it back |
| `hud.reset [part]` | puts one HUD part, or every part, back where the game draws it (one part keeps its colours; a reset of every part puts the colours back too and keeps the layout before as "Before reset") |
| `hud.layout [save\|load\|delete <name>]` | the HUD layouts in `emf\hud`: without words the list, else save the current one under a name, switch to one, or delete one |
| `hud.swap <bar> <bar>` | two of the vitals' bars (`health`, `mana`, `stamina`) change places (e.g. `hud.swap health mana`: mana on top) |
| `hud.order <bar> <bar> <bar>` | the vitals' bars from the top (e.g. `hud.order mana health stamina`) |
| `hud.grow up\|down` | the status effects list grows upward from the food icons' bottom edge, or down as the game draws it |
| `hits` | the state of the hit hooks (API v31): whether the before-the-hit hook and the hit rules work, or why not; how many rules and callbacks there are; the hits seen, changed and cancelled; callback faults; the average microseconds per hit; whether fall, shroud, cold and drowning damage are on or off |
| `buffs.list [food\|buff\|debuff\|rested\|terrain] [text]` | the game's buff types with their ids (API v32) |
| `markers` | the markers this game draws (by mod, and those a server sent), and whether drawing and the game's map list are on (API v33) |
| `markers.test [text] [game icon]` | a marker 10 m ahead of you, over the world and on the map; `markers.test clear` removes it |
| `screen` | which game screen is open (play, a menu, the chat) by the game's own name, and whether mods' keys are paused (they are while the game's chat, social screen or a rename box is open); runs anywhere |
| `map.where` | the world spot under the mouse on the open map, and whether this game's server offers a map action (API v34); runs anywhere, also on a game you joined |
| `ground <x> <z>` | the ground's height at that spot (API v34); needs a world you host, the answer follows a moment later |
| `map.act <x> <z>` | this game's server does its map action at that spot, as Shift + hold on the map does (API v34; the server checks it the same way); runs anywhere |
| `buffs.of [entity]` | an entity's active buffs (default entity 1) |
| `buffs.give <entity> <type id> [seconds]` | gives a buff on the server world; answers "queued", the result is logged after the step. Ids are hex (`0xAC13C6C3`) or decimal |
| `buffs.clear <entity> <type id>` | removes a buff the game's way (a cleanse) |
| `hud.colour [bar] [part] [colour]` | the vitals' bar colours: `hud.colour health #ff4040` (the fill), `hud.colour mana empty #00000000` (a part: `fill`, `empty`, `end`, `damage`, `heal`), `hud.colour panel #20208080`, `game` for the game's colour again (`hud.colour health all game`: every colour of a bar); `hud.colour health` shows a bar's colours, `hud.colour` every colour you set |
| `saves.backup [label]` | backs up every save file now, Steam Cloud and Local, into `%LOCALAPPDATA%\EMF\save-backups` ([docs/saves.md](docs/saves.md)); only at the main menu: while a world runs it says why not (the game writes its saves every 30 seconds) |
| `saves.list` | the save backups, newest first, with what each holds and where they are; restoring needs the game closed (EMF.exe's Saves tab, or by hand: [docs/saves.md](docs/saves.md)) |
| `items [words]` | the game's items with the names the game shows (in your language) and their debug names, id and stack size, best match first (e.g. `items wood log`) |
| `give <item name> [count]` | drops items 1 m above you; the game collects them (e.g. `give wood logs 3`, the name the game shows in your language (`give holzscheite 3` in German), or `give Material_T1_Wood_Logs 3`: item names take spaces, any case, words in any order and parts of words; several matches are listed, see docs/modding/reference.md). Japanese, Korean, Chinese and Thai names on EMF's own screens: see "Item names in Japanese, Korean, Chinese and Thai" above |
| `put <item name> [count] [entity] [level] [rarity 0-4]` | puts items straight into an inventory, yours by default (e.g. `put Weapon_T1_1H_SwordEpic_01`, `put Material_T1_Wood_Logs 2 1025` for a villager, `put Weapon_T1_1H_SwordEpic_01 1 1 10 3` for a level-10 Epic sword; the game may cap the level an item can have); the reply comes one tick later with how many fitted, and why when fewer did (`0 added (entity 77 has no inventory ...)`, `3 added (only 3 of 5 fit: no room for the rest)`) |
| `take <item name> [count] [entity]` | takes items out of an inventory, yours by default: all or nothing (`take Material_T1_Wood_Logs 5`; when fewer are there nothing is taken and the reply says how many were missing). It searches every inventory of the entity, **equipped gear included**, and a taken gear item is gone for good (its level and perks with it) |
| `spawn <item name \| template guid> [count 1-20] [at <x> <y> <z>]` | spawns templates 2.5 m in front of you (the camera's direction on your own game, beside you otherwise), or at a place in metres (a dedicated server's console has no "you"). An item name must be a placeable item: it spawns the template the item puts down (`items <name>` shows "places <guid>"); an item that places nothing is refused with a note (`spawn wooden door`). GUIDs are in `export/template_guids.csv` |
| `remove <entity> [me\|drop\|<player>] [force]` | removes a placed object like a pick-up. `me` (or `player`) gives the item back to whoever typed it, `drop` leaves it on the ground, a player number puts it in that player's inventory, `force` also removes a non-empty container |
| `goto [#k] <x> <y> <z> [ground]`, `path <x> <y> <z>` | teleport you there; a path on the game's path grid. Coordinates can be written any way: `3 4 5`, `3, 4,5`, `(3, 4, 5)`, `[3 4 5]`, `<3> <4> <5>`, `3;4;5`, or a pasted `pos` line (`player 1 at (3.0, 4.0, 5.0) m`). The same goes for `spawn ... at` and `tp.pos` |
| `time <hours 0-24>` | sets the time of day (`time 12` = noon) and lets the weather roll again; on a game build EMF does not know it leaves the weather alone, and refuses when the world's tick fields are not confirmed |
| `pos` | your position in metres |
| `entities` | how many entities the server world has |
| `inspect <entity>` | every component the entity has, with its size; template data is marked (e.g. `inspect 1025`) |
| `tpl <entity> <component> [bytes] [offset]` | hex dump of the entity's template data (e.g. `tpl 1025 ActiveNpcState 48`) |
| `tplset <entity> <component> <offset> <f32\|i32\|u32\|u16\|u8> <value>` | changes one template value, for every entity of that template, until the game restarts |
| `stack <item name> <max 1-65535>` | changes an item's maximum stack size until the game restarts |
| `destroy <entity>` | removes an entity from the world (host); refuses players and ids the world does not have |
| `near [radius 1-200]` | the entities around you (default 10 m), nearest first, tagged: player, villager, inventory, animal, plant (level), health |
| `inv <entity>` | every stack in the entity's inventories: inventory entity, slot, slot id (for `take_items`), item, count, and a gear item's data entity, level and rarity |
| `god [entity \| me] [on\|off]` | for tests: the entity (you by default) cannot drop below 1 health from hits, falls, the shroud, the cold or drowning (a hit rule, API v31; `hits` says whether hit rules work on this game build). `god` alone lists who has it; `god off` or `god <entity> off` ends it. It also ends when a player leaves (their entity id goes to the next player), when `destroy` removes the entity, when the entity is gone otherwise (found and said at the next `god`), and for everyone when a new world starts. Refused while the entity's health reads 0 (dead, or just spawned). Examples: `god`, `god on`, `god 1025 off` |

Example reply (from a test run):
```
[cmd] > give Material_T1_Wood_Logs 3
[cmd] gave 3 x Material_T1_Wood_Logs to player 1
[cmd] > time 12
[cmd] time of day is now 12.00 h (was 15.35 h)
```

## If the game stutters

EMF can measure where the time goes, so a report can say what is slow:

1. **Frame times.** Put this in `<game>\emf\emf_core.ini`:

   ```
   [debug]
   frame_times=1
   ```

   From the next start, each frame's time goes to `emf\logs\frame_times_<date>_<time>.csv`, and a summary every 10 s
   to the `.log` next to it (frames per second, the slowest frames, and each frame over 100 ms with the time of day).
   It works in every way of starting, "Play without any mods" in EMF.exe too, so you can compare. `frame_times=0`
   (or no line) turns it off; it changes nothing in the game.
2. **Each mod's time.** In the console, `perf` shows how long the game's own world steps, EMF's wait for the server
   world's jobs and each mod took per step since the last `perf`; `lua.perf` the same for each Lua mod. Play a few
   minutes, then run them.
3. **Slow moments.** A mod's step over 25 ms (the game's own over 100 ms) is written to `emf\emf_core.log` as a
   `slow:` line, at most once a minute each.

When you report stutter, send the frame-time `.log`, `emf_core.log` and what `perf` and `lua.perf` showed.

## The sample mod
- Every 5 s (client world): the local player's position, and how often the hooked `client_player_state` program ran.
- Every 10 s (server world, host only): how often its own ECS program `sample_mod_server_tick` ran, and the villagers
  with their AI state and position.
- Logs `world loaded` / `world saved` from the load and autosave events.
- Adds the command `sample.villagers`: the villagers of your world with their AI state and position.
- With `enable_write=1` (host only), on test keys set in the same ini (none by default, so they never take a key a
  mod uses: Hand-over has F7, Bulk Craft F8; for example `key_spawn=F10`, `key_destroy=F11`):
  - **`key_time`** moves the time of day forward 3 hours (the game's own `setTimeOfDay`).
  - **`key_spawn`** spawns the template in `spawn_guid` (default: a barrel prop) 2 m from the player;
    **`key_spawn2`** spawns `spawn_guid2` (default: a test townsfolk).
  - **`key_destroy`** destroys the last spawned entity.
  - **`key_give`** gives the player `give_item` × `give_count` (an item's debug name, e.g. `Material_T1_Wood_Logs`) as a
    pickup dropped 1 m above them; the game's loot code collects it into the backpack (with the default
    auto-collect setting). `dump_items=1` writes every item (name, id, GUID,
    stack size) to `emf\items.csv` when the world loads (build 1269579 has 4207 items).
  - **`key_villager`** makes the villager nearest to the player walk to the player (the engine's own "go to position"
    command). It waits next to you briefly, then wanders back to its home area.

## The big_stacks mod
With `-EnableWrite` (`emf\config\big_stacks.ini`: `enabled=1`, `multiplier=4`, `max=9999`) every item that stacks gets
a bigger maximum stack (250 → 1000) once a world has loaded its items, on the host and in your client. It changes the
game's item definitions in memory only: the game restarts with its own sizes. Stacks you build above the normal size are
saved in your inventory as they are; how the game treats them without the mod is not tested. Tested: 600 wood logs
went onto one stack of 850.

## The event_log mod
On by default (read-only; `emf\config\event_log.ini` `[event_log] enabled=0` turns it off). On the host it logs the
first 20 deaths, hits, pickups, crafts, level ups, experience gains, buildings placed and items consumed, naming what
died or was hit (player, villager, enemy), then a count per kind every 10 s. Example (from a test run):
```
[event_log] hit: 1 (the player) by 6820, health -48, stamina +0
[event_log] died: entity 6814 (an enemy/creature)
[event_log] last 10 s: 2 deaths, 4 hits
```

## The companion mod
With `-EnableWrite` (`emf\config\companion.ini`: `enabled=1`), **F6** (the order wheel key) on the host spawns a
Vukah warrior (`Enemy_Vukah_Melee`) 2 m from you, turns it into an ally and makes it guard you through the game's own
`StartGuardingEvent`, sent from the mod's ECS program. It follows you (when you walk away it catches up and stays 1-3 m
from you; while you stand still it may wander a few metres), never attacks you, and **fights at your side at full
damage**: in the test it hit a hostile Vukah three times for 48 and killed it, and the game credited you with the kill
("Enemy slain", XP).

**Keeping up.** The companion only walks, and the game removes a creature of its kind that ends up away from every
player within a second. So:

- While it follows you (Follow me, Attack): left behind (more than 40 m away for 4 s, or more than 60 m even in a
  fight) or stuck (more than 20 m away without 2 m of way in 8 s), it is moved to the spot where you stand with the
  game's own teleport, which looks for the ground there (EMF API v20; on an older EMF, or when the teleport fails, a new
  companion comes instead). With `follow_distance` d these limits move out to d + 20, d + 30 and d + 10 m.
- Gone without dying while it follows (after a fast travel, a respawn at an altar, a long glide): "it got lost; it
  comes back to you when you stand still". The new one appears on the spot where you stand.
- Gone while it stays (you went far away): it is not brought to you. "it waits at its stay spot; it shows up again when
  you come back there": it appears at its spot once you are within 50 m of it.
- It comes back by itself 3 times within 2 minutes; at the 4th loss it stays away until you call it (F6), which
  starts the count over.

The catch-up and the return to you wait until you have stood still for 2 s and aim at the ground you stand on, not a
spot beside you (over a ledge, or beyond a wall). Gear, order and level stay the same.

**Orders.** With the companion out, hold **F6**: a wheel titled "Companion (level N)" offers

| Entry | What happens |
|---|---|
| Follow me | it guards you again (the default) |
| Stay here | a small barrel is put down where the companion stands and it guards that instead: it stays there when you walk off (and still fights what comes near; if you go so far that the game removes it, it shows up at its spot again when you come back). Follow me (or Dismiss) removes the barrel. If the barrel keeps disappearing (smashed, or out of the loaded area) it is put back at most every 5 s, and after the third time the companion follows you again ("The companion cannot keep its stay spot") |
| Attack (what you look at) | the creature under your crosshair within 40 m becomes its target: the game's own threat event, sent again every 2 s while the target lives and is within 40 m of you, for at most 30 s ("The companion gave up the attack"). Players, other allies and villagers are never picked |
| Gear | a second wheel: give it a weapon or armour from your backpack / action bar, or take all of it back (below) |
| Dismiss | it leaves (`StopGuardingEvent`, then removed) |

Release F6 over an entry, or tap F6 and click. Each order can also have its own key (`key_follow`, `key_stay`,
`key_attack`, `key_dismiss`; off by default). For Attack, aim with the crosshair; a fast animal can outrun it (in the
test it chased a rabbit for 30 s and gave up).

**Level and gear.** The companion is as strong as you: every 2 s its level is set to your level plus a gear bonus (at
most `gear_bonus_max`, capped at 100) and the game rescales its health and damage (level 1 -> 10: health 119 -> 417,
hits 48 -> 156). Gear gives the bonus:

- a weapon: `max(1, (weapon level - your level) / 2 + 1)` levels, so a better weapon than yours helps more;
- each armour piece (helmet, chest, arms, legs, boots): +1.

In the Gear wheel each weapon and armour piece shows its level and rarity ("Weapon_T1_1H_SwordEpic_01 (level 10,
Epic)"). Picking one takes it out of your inventory (from that exact slot: if the piece moved meanwhile, nothing is taken) once the game confirms the transaction, and
the companion wears it: one per slot, and a new piece for an occupied slot sends the old one back. "Take all gear back"
puts every piece back into your inventory with the same item, level and rarity. Gear with a socketed gem or a tint is
not offered: it would come back without them (the game removes a taken item's own data). The pieces are stats only:
the Vukah body does not show them. `companion.gear` lists what it wears and the level it gets.

**Saved per world.** Whether the companion is out, its order (Stay keeps its spot) and its gear are written to
`emf\config\companion\<world slot>.txt` (only when something changed, at most once a second). When you load that world
again, after a reload or a game restart, the companion comes back about 5 s after the world has loaded, with its order
and gear. If no room is left for a returned piece, the companion keeps wearing it and says so (a piece that a newer
one replaced waits instead: make room and use "Take all gear back"). Gear is only handed over once the world's file is
known (EMF learns the world slot when the save loads; on a game build where that does not work, giving is refused
with a note, because nothing could be saved). After a crash, gear
being handed over is never lost: a piece that was being given back is given back again at the next load (so a crash
in the 2 minutes after "Take all gear back" can return it twice), and a piece the game had not confirmed taking stays
yours.

**Every player their own (1.8).** In a game you host your guests can have a companion too, and on a dedicated server
every player: each one follows and fights for its own player, takes that player's orders, wears gear from that player's
backpack and is kept for them with the world. Your own (the host's) works exactly as before, in the same file. A
guest's or a server player's companion is in `emf\config\companion\<world slot>-<account>.txt`; when they leave the
world it goes with them and comes back when they join again (its order and gear kept). Their notes come as a line only
they see ("[Companion] following you"). A player who joined uses their own keys (F6 and the single order keys, as
they set them in their own game) when Companion is switched on in their game too: the key goes to the server, which
acts on their own companion only and shows the order and gear wheels in their game, and their notes come as notes
there (as system chat lines for a player without Companion in their game).
Without Companion in their game, their companion is ordered from the server's console: `companion.as <player entity>
summon|dismiss|follow|stay|attack|gear` (`chat.players` lists the players), or comes by itself with `auto_summon=1`.
`max_companions` (default 8) caps how many are out at once in the world; one whose player left holds no place.

**New in 1.7:** the order wheel key is F6 by default (it was G, which the game uses in build mode and to sort the
backpack); a key you chose yourself stays. F6 no longer dismisses at once: pick Dismiss on the wheel, or set
`key_dismiss`.

| companion.ini | What it does |
|---|---|
| `key=F6` | the order wheel key (with no companion out it calls one; every companion key is a setting): a letter, a digit or a keyboard key's virtual-key code (`0x71` = F2; mouse buttons are refused); counts only while the game window is in front |
| `key_follow=` `key_stay=` `key_attack=` `key_dismiss=` | optional one-key orders, same format; empty = off |
| `gear_bonus_max=10` | the most levels gear can add |
| `marker_guid=` | what "Stay here" puts down for it to guard (default `0ce28db1-522f-4455-b358-3888fb5e089b`, a small barrel). It must be a template the game can spawn standing still |
| `faction=11` | the faction number written on the companion right after it spawns (and again if the game changes it). 11 = `PlayerAlly`, the faction of the game's own summons: full damage. 7 = `Player` follows too but every hit does 0. -1 keeps the template's faction: **with the default enemy body that leaves a hostile Vukah next to you**, and other non-ally numbers can too. Anything that is not a number from -1 to 15 falls back to 11 (logged). Level and gear need 11 |
| `guid=` | the body (default `d5d05217-52dc-467a-9192-26b4f661d8d9`, `Enemy_Vukah_Melee`). Plain enemy-AI actors without `PlayerInput` work; the old default `FakePlayer_Vukah_Melee` (`9543cfb0-...`) follows but does no damage. Tested ones: `docs/engine_parts/re_companions.md` |
| `auto_summon=1` | every player in a world this game or server hosts gets one by itself (1.8: guests and server players too) |
| `max_companions=8` | how many companions are out at once in the world, every player's together; 0 = no limit (1.8) |
| `respawn_seconds=N` | after the companion dies, a new one comes N seconds later; 0 (default): press the order wheel key. One the game removed (left far behind) did not die: it always comes back (above) |
| `follow_distance=<m>` | changes the template's guard distances (start following at d + 2 m, stop at d m). Template data: with an enemy body it changes **every wild Vukah of that template** too, until the game restarts. Not set by default |

Console: `companion.gear` (what yours wears, its level, and pieces still on their way back to you), `companion.as <player entity> <word>` (a player's order: summon, dismiss, follow, stay, attack, gear), `companion.aim` (your camera ray and the creatures near it, with
how far each is off the ray: why Attack picks what it picks). On the host the faction reaches your own screen too (the
client world already shows the ally faction); other players of an online session were not tested.

Template GUIDs come from `export/template_guids.csv` (run the `guid_dump` EML mod once). Write them as text;
`guid_layout` ms (the default; `guid_layout=0` in the ini, 1 = raw) converts them to the byte order the game stores.

## The villager_jobs mod
With `-EnableWrite` (`emf\config\villager_jobs.ini`: `enabled=1`), villagers on the host work for you at a chest,
through the game's own systems: they farm, tend animals, haul goods between chests, craft (or trade), and gather the
wild pickables around a chest.

1. Look at a villager and press **K** (within 8 m; the crosshair picks). A note says "Villager 1026 chosen".
2. Within 30 s look at a chest and **hold K**: a wheel asks what to do there:
   - **Farm the crops here (N plants, M ripe)**: the villager walks to each fully grown plant within 20 m of the chest
     and harvests it straight into the chest (the game's own loot roll); the plant then grows again from a seedling,
     so the field stays planted (no seed is used). Plants still growing, or capped with the game's growth limit, are
     left alone, and so are trees you planted (you chop those), planted fruit trees and nut bushes whose fruit grows
     back on the plant, and plants that are cut rather than picked (aloe, cabbage, artichoke): once grown, the wheel
     counts only the crops it will farm. In the test a villager harvested three
     tomato plants (2 tomatoes each) and all three regrew.
   - **Tend the animals here (N animals, M feeders)**: it collects what the farm animals within 20 m have made (eggs,
     milk, wool...) into the chest, and fills feeders holding fewer than 5 items with 10 animal food items from the
     chest (items whose name contains `_Feeding`, e.g. `Material_T3_Turkey_Feeding`; a food a feeder refuses is not
     tried there again for 10 minutes). Collecting comes first; with no animal food in the chest it leaves the feeders
     alone and says so once.
   - **Haul to another chest**: then look at the chest to carry to and press K (K at a villager cancels). The villager walks between the two
     chests and moves up to 20 items per trip: the item kinds the target already holds (it sorts), or anything when
     the target is empty. Gear with its own data (level, perks) is never hauled.
   - **Craft (N recipes)**: a wheel of what the villager can make from the chest. At a trader NPC these
     are its trades (the game has no shop: traders are crafting stations with barter recipes).
   - **Gather around here (N pickables)**: the villager picks up the wild pickables within 20 m of the chest (scatter
     bushes with twigs or plant fibre, berries...) into the chest and rests 20 s after each one; the game decides when
     they grow back. Crops, containers and anything the game marks as a placed item (loose stones, keys, weapons,
     potions, rewards) are left alone. A full chest pauses the job for a minute, noted once.
   Move the mouse toward an entry and release K (or tap K and click), or press its number (1-8, on the top row or the
   numpad). Esc, a right click or releasing in the middle cancels; the mouse wheel or Left/Right turn the page when there
   are more than 8.
3. Crafting: the villager walks to the chest (sent there again every 10 s while it is more than 4 m away) and makes
   one item every `seconds_per_item` (default 30) while it is within 6 m. When the chest runs out of materials (or has
   no room) a note says so once; it goes on by itself when you refill the chest.
4. K on a working villager opens a wheel with "Stop farming (12 harvests)" (or the job it has: "Stop tending (3
   collected, 20 fed)", "Stop hauling (40 items moved)", "Stop gathering (5 brought in)") and "Keep working".

**Players who joined** (a dedicated server, or someone else's hosted game) use K the same way when their own game has
Villager Jobs 1.5 or newer turned on (their own key setting counts): their game sends the key press to the server's
Villager Jobs, which picks what that player looks at and shows its notes and wheels on their screen (`job.look
<player entity>` in the server's console shows where a player looks and what K picks there). Only admins give,
change and stop jobs: the host, the game's own admins (on a dedicated server whoever joined with the Admin group's
password; in a game someone hosts, the players the host made admins), and the accounts in `[chat] admins` of the
server's `emf\emf_core.ini`. For anyone else, K at a working villager says what it does. Notes of running jobs
(a chest gone, out of materials) go to the host and to every player who joined with the mod. A joined player's wheel
waits 30 s for a pick (after that, K again); a player who stops being an admin while their wheel is open picks
nothing. When the server has no Villager Jobs 1.5, K says so.

How the work moves: a walk order makes the villager roam near its target, so it acts within 6 m of it; the order is
repeated every 5 s, and a target it cannot get close to within 60 s is left alone for 2 minutes (noted once: villagers
stay near their home area). Every item move is the game's own all-or-nothing transaction; a feed or haul whose target
takes fewer items puts the rest back into the chest it came from. A harvest that does not fit (a full chest) is
noted once, and that plant is tried again a minute later.

Jobs are saved per world slot in `emf\config\villager_jobs\<slot>.txt` and come back when the world is loaded again; a
job also remembers the villager (its NPC id) and where its chests stood, so after a reload it ends with a note when
its villager or a chest is gone or is not the same one (entity ids can change); it waits up to 60 s after the load for
them to appear. Chests made with the `spawn` command are not saved by the game, so their jobs end after a reload;
chests you build are saved.

The game only simulates the area around players: plants grow, animals eat and produce, and villagers walk only while
you are near, so a village works while you are in or near it. Animals only produce while happy (shelter, bed, food:
the game's own rule).

| villager_jobs.ini | What it does |
|---|---|
| `key=K` | the key: a letter, a digit or a keyboard key's virtual-key code (`0x71` = F2; mouse buttons are refused); K is free in the game's default bindings. It only counts while the game window is in front |
| `seconds_per_item=30` | seconds per crafted item (at least 1; a recipe that takes longer in the game is never made faster) |
| `farm_radius=20` / `tend_radius=20` | metres around the chest in which a farming / tending villager works |
| `haul_per_trip=20` | items a hauling villager moves per trip |
| `gather_radius=20` / `gather_seconds=20` | metres around the chest in which a gathering villager picks up wild pickables (scatter bushes with twigs, plant fibre...; not crops, containers or anything the game marks as a placed item: loose stones, keys, weapons, potions, rewards) / seconds it rests after each one. A villager only works around its own home (its base) |

Console: `jobs` lists the jobs with what they did, `job <villager> <chest> <recipe hex>` assigns a craft,
`job.farm <villager> <chest>`, `job.tend <villager> <chest>`, `job.gather <villager> <chest>`,
`job.haul <villager> <from chest> <to chest>`,
`job.stop <villager>` ends one, `job.recipes <villager> [chest]` lists what the villager can make (from the chest, or
everything it knows), `job.recall [villager]` brings a stuck villager back beside its job's chest (else its flame
altar; look at it or name its id; API v20 teleport with the game's ground search), `job.look [player entity]` says
where a player looks and what K picks there. Recipes that need a crafting station's props are not offered; a recipe the game refuses for
another reason (not unlocked, no room in the chest for the product) makes the job wait with a note. Only the recipe's
own product, arriving in the chest, counts as made. The test world's villagers know the 57 hand-crafting recipes
(workshop 0); villagers with a trade in a base have their own lists.

## The clock mod
Shows the game's time of day ("14:05") in the bottom-right corner, through the API's on-screen text (v12). It only reads
the game, so it is on by default and also works in someone else's world. `emf\config\clock.ini`: `enabled=1`,
`place=1` (0 top right, below the game's quest tracker; 1 bottom right; 2 bottom left), `h24=1` (0 for "2:05 PM").

## The quick_stack mod (Quick Stack and Chest Machines)
**Tap L** on the host: what you carry goes into the chests within 10 m that already hold the same item (the nearest
such chest), through the game's own transactions: "Quick stack: 901 item(s) into 1 chest(s)". Gear with its own data
(level, perks) never moves, the action bar stays (`keep_action_bar`), and so do arrows, food and named items with
the filters (`keep_ammo`, `keep_food`, `exclude`); anything a chest has no room for comes back. Any storage counts,
crafting stations and feeders too. **Hold L at a chest** for the chest wheel: make it a **sorting chest** (every 10 s
each plain stack in it goes to the nearest chest within 30 m that already holds it: players drop things in through
the game's own chest window and need no mod), a **recycler** (off until `recycler` is on: crafted items go back into
50 % of their recipe's materials, rounded down, never more than they cost; gear, cooking, upgrades and items several
recipes make stay), or an **overflow chest** (with `overflow`: takes what no chest holds). Marks are kept per world.
`/qs` (or `/quick_stack`) quick stacks for the player who types it and `/qs.chests` lists the marked chests near them
(every player, also from the chat); `qs.mark` and `qs.list` for the host. Settings from 1.1 (`key`, `radius`,
`keep_action_bar`) keep their values. Details: `mods/quick_stack/README.md`.

## The life_skills mod (Lua)
Five skills that level up as you gather: woodcutting, mining, foraging, hunting and fishing. Everything you pick up from
the world (not out of a chest) gives XP to the skill of that item: wood logs and resin to woodcutting, stone, ore and
terrain blocks to mining, plant fibre, berries, mushrooms and herbs to foraging, meat, fur, hide and bones to hunting,
raw fish to fishing. Level L needs `50 * (L - 1)^2` XP (level 2 at 50, 5 at 800, 10 at 4050; the top level is 20). From
level 2 every pickup has a `(level - 1) * 2` % chance to give the same items once more, straight into your inventory.
A level-up shows a note; the levels stay on screen (top right by default).

Skills are kept per world and per player (by account), so friends in a game you host have their own. They can see
theirs with `/life_skills.skills` in the chat when `[chat] players=1` is set (a command for every player since Lua
1.4; no admin rights needed); you: the same command in the console. Settings
(Mods tab): `bonus_per_level` (0-100), `hud`, `corner`. It is a Lua mod (`emf\mods\life_skills\main.lua`): open it
to change the skill lists or the curve while the game runs.

## The raids mod (Night Raids)
At nightfall, by chance, waves of enemies gather 40-60 m outside a base where players are (never inside a base) and
march on its flame altar with the game's own guard order, at the level of the strongest player there plus the
difficulty. A warning first ("raiders gather outside your base and attack in 60 s"), a line on screen with the
raiders left, "Raid repelled!" and a reward for every player at the base when all are dead (runes and furs, or also a
rare weapon); survivors flee at dawn. Off in every world until you switch it on there: `raids.world on` (or the setting
"Raids in the world you are in"); kept per world in `emf\config\raids\<slot>.txt`. Commands: `raids.status` (every
player), `raids.world on|off`, `raids.start [seconds]`, `raids.stop`, `raids.kill` (host). Settings: **Raid strength**
(a preset: gentle, standard or hard, or your own settings below), chance, hour, warning, quiet nights, waves, raiders
per wave, gap, difficulty, boss wave, **Raiders attack** (players, companions and summons, or players only), **Only
creatures you are ready for** (on: the boss wave's brawler waits until the strongest player at the base is level 5,
your own templates for their `GUID@level`), who raids (the tested Vukah, or your own template GUIDs), which base, base
radius, reward, corner and colour. Spawned raiders are not saved: a reload ends the raid. Details:
`mods/raids/README.md`.

## The loot_magnet mod (Loot Magnet)
Installed switched off: switch it on in the Mods tab or EMF.exe. The corpses of creatures killed near a player are looted into that player a moment later, with the game's own loot
event. It runs where the world runs, so on the host or a dedicated server it works for every player, also those who
joined without EMF. Each player switches their own with `/loot.toggle` (and sees it with `/loot.status`); kept per world
and account. Never a corpse twice, a player's or an ally's body, a farm animal, anything tame or owned, or anything
within the base radius of a flame altar; by default only corpses that hold nothing but materials. A full inventory
loses nothing (what does not fit stays where it was). Most creatures leave their loot in a loot spot where they died
(a loot container the game spawns as the body fades): the magnet takes that one, or the body when it keeps its loot.
From 2.0 it also takes, once a second, what lies near each player whose magnet is on, each kind with its own switch and
reach:
- on by default: items on the ground (drops, sticks and stones after chopping or mining, loose stones and twig piles)
  and collectibles;
- off by default: mushrooms, fireflies and bait bugs, wild plants, chests in ruins (opened as you would, then emptied;
  never a player's chest or one that needs a lockpick or key), barrels, crates and urns to smash, and (experimental)
  bee hives to smash: their honey and wax drop, and a wasp swarm may come out.
Never inside a base (smashing 10 m more), never a quest item, never what is on its ignore list.
From 2.2, limits (off until set): how many of each item a player carries (backpack, action bar, equipment, currency) before the
magnet leaves more of it where it lies, one number each for materials, food and potions, arrows and everything else,
plus own numbers for single items; chests and the loot of kills are always emptied.
Settings: reach, delay after the kill, how long it looks for loot (the loot appears 1-3 s after the death), what comes, arrows,
the groups and their reaches, the ignore list, the limits, default for new players, the host's key, base radius. Details:
`mods/loot_magnet/README.md`.

## The champion mod (Wandering Champion, Lua)
Installed switched off: switch it on in the Mods tab or EMF.exe. Every so often, while a player is outdoors at night, a champion (the Vukah brawler mini-boss, with its boss bar)
appears 80-150 m away at that player's level + 2, patrols spots players walked, and hunts a player who comes within
30 m. The line on screen shows its distance and direction; the slayer gets 10 runes and a rare weapon at their level;
it leaves after 10 minutes, at dawn or when everyone is far away. With **Only creatures you are ready for** (on by
default) it comes only for a player of level 5 or more (the brawler's `min_level`). Commands: `champion.where`, `champion.stats` (every
player), `champion.summon`, `champion.dismiss`, `champion.kill` (host). The template table and the rewards are at the
top of `emf\mods\champion\main.lua`: change them while the game runs. Details: `mods/champion/README.md`.

## The bounty mod (Bounty Board, Lua)
Installed switched off: switch it on in the Mods tab or EMF.exe. The game's Notice Board, built within 40 m of a flame
altar, becomes a Bounty Board in a game you host (it is recognised through the host's own game, so a dedicated server
needs an admin's `/bounty.mark`; the host can make any prop a board with `bounty.mark`; the villages' notice boards
stay plain). Until a world's first board, bounties are taken and claimed anywhere. It posts 3 notices, the same for everyone, renewed every 4 in-game days at dawn: slay creatures of
a faction ("Slay 6 Vukah"), gather a raw material picked up in the world ("Gather 60 Wood logs", never out of a chest
or a tombstone), or slay a champion (2 levels above you, 1,000 health or more). A player takes one (holds 1 by
default, up to 4), works on it anywhere, and claims the reward at the board; a held bounty stays across new notices
until finished or dropped. The host looks at the board and presses E for a wheel; players who joined (and everyone
on a dedicated server) get the notices as a private chat line when they walk up, then type `/bounty.take <n>` and
`/bounty.claim`. With `need_board` off they work anywhere. Until a player finishes their first
bounty, they see quick starters ("Gather 20 Wood logs"); the size `tiny` (x0.2) makes every bounty quick to try.
Kills count for every player who hit the creature in its last 10 s (`share_kills`), and a hit by a player's companion
or summon (an ally guarding them) counts as theirs. Rewards: runes, furs, gear at your level; what does not fit is
dropped at your feet. Commands for every player: `/bounty.board`, `/bounty.take <n>`, `/bounty.drop <n>`,
`/bounty.claim`, `/bounty.reroll <n>`, `/bounty.top`; the host has `bounty.mark`, `bounty.unmark`, `bounty.boards`,
`bounty.give <id>` and `bounty.reset`. Kept per world and account in the mod's own file (from a 1.1 file the started
and finished bounties are kept: nothing earned is lost). The bounties are the `POOL` table at the top of `emf\mods\bounty\main.lua`.
Details: `mods/bounty/README.md`.

## The grave mod (Grave Keeper, Lua)
Installed switched off: switch it on in the Mods tab or EMF.exe. `/grave.where` says where your tombstone is
("230 m NE, 3 stacks"; the host also sees it on screen), and `/grave.claim` brings its stacks to you within 150 m
(0 = from anywhere), once per 10 minutes of play, at an optional cost. A tombstone loaded from a save gives its
stacks with the game's own loot event (what does not fit stays in it); a fresh one, whose stacks sit in linked
inventories that event does not reach, has each plain stack taken out and put into you (what does not fit is dropped
at your feet; gear stays, to be taken by hand). The cooldown starts when the claim is sent; when nothing left the
tombstone a few seconds later, the cost comes back and there is no cooldown. Only your own: the tombstone that
appears where you died, after your death and before you have been back up for a minute (the game's tombstone names
no owner a mod can read); one about as near to two players' deaths is nobody's. Tombstones from before the mod ran
are handed out with `grave.adopt` (host). Details: `mods/grave/README.md`.

## The sleep mod (Sleep Through the Day)
Installed switched off: switch it on in the Mods tab or EMF.exe. While players sleep in a bed by day, the day passes
60 times faster until the evening (17:30 by default, `wake_hour`); then the clock runs normally. Everyone has to be in
bed by default (`who`: or each sleeper speeds it up, as the game's night skip does, or a share of the players, which
also brings the night to full speed). A sleep that reached the evening never skips the next day. `/sleep.status` for
every player. It moves the game clock's anchor instead of its speed, so it never fights the game's own night skip.
Details: `mods/sleep/README.md`.

## The herald mod (Server Herald)
For dedicated servers (and a game you host with `[chat] players=1`). Installed switched off, because it changes what
players see: switch it on in EMF.exe or with `enabled=1` in `emf\config\herald.ini`. A message of the day for each
player who joins (`motd`, sent a few seconds after the join), "<name> joined / left (N online)" lines, deaths
(optional), announcements in turn from `emf\config\herald\announcements.txt`, a restart countdown (`herald.restart 10
nightly`, also written into `emf\commands.txt` by a scheduled task; it saves at 0, the stop itself stays outside EMF), a
chat log per day in `emf\logs\chat-YYYY-MM-DD.log`, playtime per account and world, and an opt-in Discord webhook (its
URL in `emf\config\herald\webhook.txt`, never logged; discord.com only unless allowed). Commands: `/players`,
`/playtime`, `/motd` (every player), `herald.say`, `herald.tell`, `herald.restart`, `herald.cancel`, `herald.save`,
`herald.seen`, `herald.reload` (admins). Details: `mods/herald/README.md`.

## The handover mod (Hand-over, Lua)
Installed switched off (it lets players put items into other players' bags): switch it on in the Mods tab or EMF.exe.
`/handover <item part> [n|all]` gives items to the nearest other player within reach (5 m by default),
`/handover.who` lists who is near, `/handover.to <name|#k> <item> [n]` picks one. The items are taken from the giver
first, each stack exactly from its slot (worn equipment never); what does not fit goes back to the giver, and what
does not fit back is dropped beside them (or the receiver, or the nearest player, if the giver left): nothing is lost.
Gear is refused (it would lose its gems and perks) unless `allow_gear` is on. `/handover.block` refuses hand-overs (kept
per world). Players who joined need nothing; the host also has a wheel on a key (F7). Details: `mods/handover/README.md`.

## The teleports mod (Waypoint Teleports)
Installed switched off: switch it on in the Mods tab or EMF.exe. Every player may type `/unstuck` (a short hop with the
game's ground search to free ground nearby, else a /home to their spawn point; not within 10 s of a hit, once a minute,
allowed in the shroud) and `/home` (to their spawn point's landing pads, else the nearest flame altar; every 10
minutes, an optional item cost, not from inside the shroud). Both wait for a 3 s countdown standing still (moving 2 m
cancels). Admins get `tp <#k>`, `tp <#k> <#m>`, `bring <#k>`, `tp.pos [#k] x y z [ground]`, `tp.back [#k]` and
`tp.list`. It moves players the way the game's fast travel does (API v20 `teleport`). Details: `mods/teleports/README.md`.

## The bulk_craft mod (Bulk Craft)
Installed switched off: switch it on in the Mods tab or EMF.exe. Craft one by hand, then craft many: look at a
crafting station and press **F8** for a wheel with the recipes you crafted there by hand, then x5 / x10 / x25 / max;
or `/craft <n|max> [part of the name]` (every player, also from the chat), `/craft.stop`, `/craft.list`. The game
crafts each one through its own craft request with you as the crafter, from what you carry, all or nothing per craft:
the mod never moves an item. Because the game's request does not check unlocks, the mod repeats only recipes you
crafted by hand through the game's window (kept per world and platform account, with the items you chose for "any
fibre" inputs, sent back every time; `craft.teach` for the host), at a station within 5 m (never a factory, the altar,
an upgrade or a recipe with station requirements such as comfort or crafting props), one craft at a time at the recipe's
crafting time and at least 0.5 s, carried over between orders (`pace = instant`: recipes without a crafting time all at
once), at most 100 per order; it stops if a craft's materials did not leave your bag (base storage). Details:
`mods/bulk_craft/README.md`; how the game's craft request works: `docs/engine_parts/inventory_crafting.md`.

## The blessings mod (Flame Blessings)
Installed switched off: switch it on in the Mods tab or EMF.exe, then restart. Cheats as plain switches, per player and
at once: **god mode** (health can't drop: the minimum of the player's `Health` held at its maximum, never saved, plus
a refill and no fall damage; players have no `CombatFlags`), **endless health** (hits land and health refills after the
step, so one very strong hit can still kill), **endless stamina**, **endless mana** and **no fall damage**
(`DynamicFallDamage.fallStartAltitude` kept no higher than the player). The host's
own are Mods-tab settings and the **F11** wheel (with Fill me up and All off); admins (the console, `commands.txt`, the
host's chat, `[chat] admins`) use `/blessings <god|refill|stamina|mana|nofall> <on|off> [#k|name]`, `heal`, `off` and
`list` on any player, kept by account until that player leaves or the world closes. God mode wins over endless health.
A line on the host's screen shows theirs, a chat line tells everyone ("[Blessings] Ember: god mode on for Ivar"), and
switching the mod off puts every player's minimum health back. When a game update switches EMF's attribute functions
off, god mode, endless health, stamina and mana say "not available on this game version". Free building is planned. Details:
`mods/blessings/README.md`.

## The beastmaster mod (Beastmaster)
Installed switched off: switch it on in the Mods tab or EMF.exe (it needs the `lua` mod 1.14). A Beastmaster class: tame
a wild wolf, boar, sabretooth, hound or bear by beating it under 25 % health (`/beastmaster tame`, a 3 s channel within
8 m) or by feeding it its bait 3 times within 10 s (`/beastmaster feed`). Up to 8 beasts per player are kept with the
world; call as many as your active slots (1 at the start, more at levels 10, 20 and 30; any kinds) for 15 % mana with
a 10 s cooldown (`/beastmaster call <name>`). A stable (the game's animal bed in your base, or a prop the host
marks with `beastmaster.stable mark`): press E at it for the Beasts window (Make active, Store, Release); once a world
has one, only active beasts are called. Each loyalty above 1 gives a beast a talent point (Strength, Toughness,
Swiftness, Second Wind, Ambush, Guard, Menace, Taunt), spent on its tab of the Beasts window or with
`/beastmaster talent`. Monsters in a fight split between you and your beasts (a tank 50 %, another beast 20 %, at
most 90 %; a boss 35 %), keep a target at least 8 s, and at least 1 in 4 stay on you. Send them at
what you look at (`/beastmaster attack`), dismiss one or all; a beast that falls rests 60 s. While you build,
your beasts step aside and come back after (`/beastmaster aside` does it by hand). While you stand still for 10 s
they pass the time (a wolf lies down, a beast sits by a fire or watches a villager, one keeps watch at night) and
follow again the moment you move. Your beasts' kills give you XP and
loot. Hold **F3** for the beast wheel (the host; B is the game's backpack key); every player has `/beastmaster` in the
chat. The bear and the aurochs come in the **EMF creature packs** download (EML content, which EMF builds when the
game starts: install the packs with EMF.exe, no EML needed). The bear pack
(`emf_beastmaster_bear`, a new model on the game wolf's skeleton): 1.4x the size, 2.5x the health, 1.5x the damage, a
little slower. The beasts pack (`emf_beastmaster_beasts`) adds the aurochs (a huge horned bull on the boar's skeleton:
1.3x, 3x health). Now and then a wild one of the packs' beasts appears 50-70 m from a player. Without the packs (or
when they were not built) those beasts are refused with a note that says how to install them. Details: `mods/beastmaster/README.md`; the model
tools: `tools/models/`.

## The necromancer mod (Necromancer)
Installed switched off: switch it on in the Mods tab or EMF.exe (it needs the `lua` mod 1.9). Raise skeleton warriors
(`/necromancer raise`, 25 % mana, up to 3) that guard and follow you until they fall and come back after a reload;
cast raging spirits (`/necromancer spirits`, 8 % mana): two flaming skulls rush what you look at, or the nearest foe,
for 6 s (at most 10). `attack`, `gather`, `dismiss` (one kind or all), `aside`, `list` for every player; the kills are
yours (never on bosses); while you build, your undead step aside and come back after. Hold **F2** for the wheel (V is
the game's crafting key): with Beastmaster installed, F2 and F3 open one "Summons" wheel with both classes' sections
(Lua 1.9 wheel groups; `shared_wheel` off gives a mod its own wheel). The undead pack (`emf_necromancer_undead`, in the
**EMF creature packs** download, which EMF builds when the game starts) adds a lich (`/necromancer raise
lich`, 40 % mana, one; on the skeleton mage), a bone golem (`raise golem`, 60 %, one; 3 m, on the Vukah elite, 2x
health) and bone wolves (`raise wolf`, 35 %, two; on the game wolf, 1.5x health). Details: `mods/necromancer/README.md`.

## Writing a mod
```cpp
#include "emf/emf.hpp"
#include "emf/generated/components.hpp"

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;
    emf::on_tick([](emf::World w) {           // runs after every world tick, on the world's own thread
        if (!w.is_client()) return;
        if (auto* t = w.get<emf::gen::ecs::CurrentTransform>(emf::local_player()))
            EMF_LOG("mymod", "x = %.1f m", t->transform.position.x / 4294967296.0);
    });
    return true;
}
```
Put the DLL in `<game>\emf\mods\`. What the API offers (details in `sdk/include/emf/emf_api.h`):

| Area | Functions |
|---|---|
| Worlds | `server_world`, `client_world`, `world_kind`, `world_tick`, `ecs_time_ns`, `singleton`, `local_player`, `server_session` (v9: a number that changes with every hosted session; a reload may reuse the world's address) |
| Entities | `get_component` (`World::get<T>`), `for_each_entity`, `spawn`, `destroy` |
| Timing | `on_tick` (after every world step), `run_on_world` (once, from any thread) |
| Engine code | `resolve` (signature address by name), `hook` (hook a signature), `hook_program` (wrap a game ECS program by name), `register_program` (add your own per-tick program) |
| Items (v5) | `for_each_item`, `find_item` (by the game's debug name), `item_info`, `give_item` (a pickup the game collects), `spawn_ex` (spawn with extra component data); C++: `emf::give_item(w, player, "Material_T1_Wood_Logs", 5)`, `emf::item_id(guid)`, `emf::guid_to_text` |
| Gameplay events (v4) | `on_event` (a copy of each engine event of a type, after the world's step, on its thread); C++: `emf::on_event<E>(kind, [](emf::World w, const E& e) { ... })` |
| Events (v3) | `register_program_def` (a per-tick program that declares the engine events it reads and sends), `emit_event`, `read_events`; C++: `emf::add_program`, `emf::emit<E>`, `emf::read<E>` |
| Template data (v7) | `get_template_component` (the entity's template half of a component, shared by every entity of the template), `write_memory` (write game data the game keeps read-only); C++: `w.get_template<emf::gen::ecs::Enemy01BehaviorState>(id)` (a const pointer, NULL if the game's size differs), `emf::write_value(&tpl->guardSetup.startDistance, 20.0f)` |
| Item definitions (v7) | `item_data` (the game's 2088-byte ItemInfo), `set_item_max_stack`; C++: `emf::set_item_max_stack(id, 1000)` |
| Inventories (v8) | `put_items` (straight into an entity's inventory through the game's own inventory transaction: stacking, gear items, popup; the result comes back after the server step); C++: `emf::put_items(w, entity, "Torch", 3, [](emf::World w, uint32_t added) { ... })` |
| Inventories (v11) | `put_items_ex` (put with an item level and, with the `EMF_ITEMS_RARITY` flag, a rarity, for gear), `take_items` (take items out of an inventory, all or nothing, from a given slot first, or only from it with `EMF_ITEMS_EXACT_SLOT`); C++: `emf::put_items_ex(w, entity, item, 1, level, rarity /* 0xFF = default */, f)`, `emf::take_items(w, entity, item, count, slot, [](emf::World w, uint32_t taken) { ... }, exact_slot)`, `emf::has_v11()` |
| Programs (v8) | `EmfProgramDef.singletons` / `.other_read` with `program_singleton`, `program_read_other` (other entities' components through the game's own accessors, inside your program); C++: `emf::add_program(name, kind, events_in, events_out, singletons, other_read, f)`, `emf::program_singleton`, `emf::read_other<T>(ctx, "CurrentTransform", id)` |
| Text on screen (v12) | `hud_set` (a line kept on screen by id, in a corner, until changed or removed); C++: `emf::hud("mymod.status", "text", EMF_HUD_TOP_RIGHT)`, `emf::has_v12()` |
| Wheel and notes (v10) | `wheel_open` (a radial wheel in the middle of the screen: up to 64 entries, 8 per page; the choice comes back after the next step of the world kind you name), `notify` (a line at the top of the screen, also in the console and the log); C++: `emf::wheel("Pick one", {"A", "B (details)"}, 'K', EMF_WORLD_SERVER, [](emf::World w, int choice) { ... })` |
| Commands (v6) | `register_command` (a command for the console / `commands.txt`), `command_reply`; C++: `emf::add_command("mymod.heal", "<amount>: heal", [](emf::World w, const std::vector<std::string>& args) { EMF_REPLY("..."); })` |
| Commands for players (v15, v17) | `command_sender` (who typed it: a player in the game's chat, or the console), `register_command_ex` with `EMF_COMMAND_PLAYERS` (any player may run it from the chat, not only the `[chat] admins`); C++: `emf::add_command(name, help, f, EMF_COMMAND_PLAYERS)`, `emf::command_sender(&s)`, `emf::has_v17()` |
| Attributes and damage (v18) | `attribute_get` (health, its maximum, stamina, mana as the game reads them, through its attribute registry), `attribute_add` (clamped, not a hit), `damage` (a real game hit: `HitEvent`, kill credit and XP for the attacker, a light reaction); C++: `emf::health(w, id, &cur, &max)`, `emf::attribute(w, id, EMF_ATTR_STAMINA)`, `emf::damage(w, player, target, 50)`; Lua 1.5: `emf.health`, `emf.attribute`, `emf.attribute_add`, `emf.damage` |
| Chat lines and saves (v19) | `chat_tell` (a line only one player sees, under their own name), `chat_broadcast`, `player_name`, `on_chat` (players' lines that are no EMF command), `request_save` (the game's own save now); where players' chat is on; C++: `emf::tell(w, player, "Welcome")`, `emf::broadcast(...)`, `emf::on_chat("mymod", f)`, `emf::request_save()`; Lua 1.6: `emf.tell`, `emf.broadcast`, `world:name`, `emf.on_chat`, `emf.request_save` |
| Teleport (v20) | `teleport` (a player or NPC moved the way the game's fast travel does: optional ground search, a spawn point's landing pads, a yaw; the callback says where it ended up, or why not); C++ `emf/teleport.hpp`: `emf::teleport(w, id, {x, y, z}, opts, f)`, `emf::player_spawn_point(w, player)`; Lua 1.7: `emf.teleport` |
| Saves | `on_save`, `on_load` |
| Helpers (header only) | `emf/storage.hpp`: `emf::WorldFile` (a per-world text file: loads once per session, saves atomically, never into another world's file); `emf/query.hpp`: `emf::position`, `emf::entities_near`, `emf::players` (every player of a server world, a dedicated server's too), `emf::player_account`; `emf/emf.hpp`: `emf::time_of_day(w)` (hours), `emf::kill(w, id)` (no attacker, never a player); `emf/inventory.hpp`: `emf::inventory_stacks`, `emf::inventory_contents`, `emf::slot_id`; `emf/aim.hpp`: `emf::aim::camera_ray(w, player)` (the player's camera as a ray in metres) and `emf::aim::pick(ray, from, points, limits)` (which point is under the crosshair); `emf/keys.hpp`: `emf::keys::parse_key(L"G")` (ini key text to a virtual-key code; refuses mouse buttons), `emf::keys::down(vk)` (only while the game window is in front), `emf::keys::Edge` (press detection) |

Rules:
- Touch ECS data only inside `on_tick` / `run_on_world` callbacks, on the world passed to the callback.
  `spawn` and `destroy` enforce this and refuse (with a log line) anywhere else, because the engine's entity
  storage is not thread safe. Component pointers are valid only until your callback returns.
- `spawn` returns the new entity id, or 0 if the game refused (the core log says why).
- Call `register_program` / `register_program_def` from `emf_mod_load`: once the first world has set up its programs, it is refused.
- `on_event`: subscribe from `emf_mod_load`. Your callback runs after the world's simulation step on its thread,
  like `on_tick`, so reading components and `spawn`/`destroy` are allowed there. All mods together can relay at most 24
  different event types per world kind; a name the game does not know is logged and never delivered.
- Events: list every event your program reads (`events_in`) and sends (`events_out`), at most 24, by their short type
  name (`"HitEvent"`). Call `emit_event` / `read_events` only inside that program's callback with the context it gets.
  A sent event is seen by game programs on the next tick. `emf::emit<E>` checks your struct size against the game's,
  so a game update that changes an event refuses instead of writing a wrong layout. Events of 256 bytes or more cannot
  be sent. A program whose event names do not exist in a world is not added to that world (logged).
  A `hook_program` name that no world registers is logged after the worlds are set up.
- Commands: name them `<mod>.<command>` (1-32 of `A-Z a-z 0-9 _ . -`, unique ignoring case). The callback runs on the
  host's server world after its step, like `on_tick` (components, `spawn`, `give_item` allowed); `args[0]` is the
  command name. A command that crashes is disabled. `register_command` works at any time.
- Template data and item definitions are shared: a change applies to every entity of that template (in both the server
  and the client world, which use the same data) and every stack of that item, until the game restarts; nothing is
  saved. `big_stacks` works on the host's server world only and leaves stack sizes someone else changed alone. The game keeps this memory read-only after loading, so write it with `write_memory` / `emf::write_value`, never
  through the pointer directly (that faults and disables your callback).
- `take_items` removes through the game's `removeItems`: the given slot (an `InventorySlotId`: one of the entity's own
  8-slot inventory entities in the low 32 bits, the slot index 0-7 in bits 32..39; anything else is refused) first,
  then the rest of the entity's inventories, equipment included. With `EMF_ITEMS_EXACT_SLOT` only that slot counts:
  if it no longer holds the item the callback gets 0, so you never take another copy by mistake. A gear
  item's own data entity (level, perks, gem) is removed by the game when it commits (checked in game); `put_items_ex`
  with the same item id, level and rarity makes a new one. The callback's count is all or 0.
- `put_items` works on the host only and needs a receiver with an initialized `InventorySetup` (players, villagers,
  chests: all three tested; the `companion` mod's FakePlayer body has none: the callback then reports 0 added and the
  log says why). The console `put` reply says why when fewer fitted (mods get the count); the same refusal
  is logged at most every 10 s. The check below is logged once per world when it passes.
  Before the first transaction in a world the core checks the accessors it can probe on the receiver
  (`InventorySetup`, `SlotSelection`, and `Inventory` / `InventoryEmptyState` on its first linked inventory) against
  `getComponent`, and the iterator slots around them, and refuses on any mismatch. Requests that wait while the core's
  `emf_inventory` program does not run in the world (2 s) end with 0 added and a log line. EMF's own programs
  (`emf_inventory`, the event relays) do not count against the 16 mod programs.
- Other-entity reads (`program_read_other`): declare the components in `other_read`, per-entity ones only (a split
  component's `Dynamic*` half; a template-only name keeps the program out of that world, logged). A read through an
  accessor that does not match `getComponent` is refused from then on (logged). Read only; the engine's own
  functions do the writes.
- Wheels: one at a time; refused (false, logged) while the console or another wheel is open, when the overlay
  cannot draw or before the game window is hooked. `hold_key` is 0 or a keyboard key's generic virtual-key code
  (`VK_SHIFT`, not `VK_LSHIFT`; mouse buttons are refused). A quick tap of the hold key (released within 300 ms)
  leaves the wheel open for a click unless the mouse made a clear flick toward an entry; alt-tabbing away cancels it;
  smooth mouse wheels turn a page per whole notch; a C++ exception in an `emf::wheel` callback is caught and logged. While it is open the mouse steers the highlight instead of the camera and keys go to the wheel; releases
  of keys the game saw pressed still reach it. With `hold_key` held when it appears, releasing that key picks;
  a left click on a highlighted entry picks in either mode. After the wheel closes, its hold key stays hidden from
  mods' GetAsyncKeyState until released, so closing with Esc does not count as a new press. The callback gets -1
  when cancelled.
- Returning false from `emf_mod_load` does not unload your DLL: whatever you registered before returning
  stays active, so do your checks first.
- Gameplay changes only work on the **host** (server world). A client of someone else's game has no server world.
- Components are looked up by their types.json `qualifiedHash` (`T::kHash` in the generated headers).
  `World::get<T>` also checks the size and returns NULL on a mismatch.
- Split components: the per-entity data is the `Dynamic*` struct, stored under its **own** hash.
  Villager AI state is `w.get<emf::gen::ecs::DynamicActiveNpcState>(id)`; asking for `ActiveNpcState` returns nothing.
- Entity ids are the same in the server and client worlds on the host.
- A mod callback that crashes is caught, logged (naming your DLL) and disabled; the game keeps running.

## Safety notes
- No anti-cheat exists in Enshrouded; still, use mods only in your own worlds and servers.
- EMF itself never writes save files, but the game autosaves the world every 30 s: anything a mod changes in a
  world (spawned entities, time of day) is saved with it (spawned villagers come back after a reload). Test in a separate local world, and back up
  `%USERPROFILE%\Saved Games\keen games\Enshrouded\` and Steam's `userdata\<id>\1203620\remote\` first.
  `scripts\save_guard.ps1 -Record` / `-Check` confirms that the saves you care about did not change.
- A game update will move code. On a build it does not know, EMF logs "UNKNOWN build: signatures only", uses every
  signature and code offset that still matches exactly once (the save result and world slot must also sit close
  together) and disables the features whose signatures or offsets no longer match. On a known build the recorded
  code offsets are used even if their scan fails (the exe's hash pins the code; the log says so). To add a build:
  1. `build\msvc\emf_sigcheck.exe <enshrouded.exe>` prints every signature's RVA and every code offset it read (exit
     code 0: a known build and all OK, 1: something did not match, 2: a build EMF does not know, 3: the check itself
     failed (bad path); `verify.sh` fails on anything but 0).
  2. Regenerate the headers from the new exe: `python tools/gen_headers.py <game>\enshrouded.exe` reads the game's own
     reflection data ([docs/engine_parts/re_reflection.md](docs/engine_parts/re_reflection.md)) and rewrites
     `sdk/include/emf/generated/components.hpp` and `reflection.hpp`; the diff shows every component layout that changed.
     `python tools/re/exe_types.py --exe <exe>` writes all types to `%LOCALAPPDATA%\EMF\types\` for a closer look
     (game data: keep it out of the repository; `.gitignore` has `*.emf.json`).
  3. Check the engine offsets the type registry does not describe. They live in `src/common/offsets.h` (world, events,
     items, program job, Keen map), `src/core/daytime.h` (Daytime / Weather, written by `time`), `src/core/programs.h`,
     `src/core/system_programs.h` (ProgramDesc), `src/core/spawn.h`, `src/core/events.h`, and the sample mod's
     `npc_command.h` / `time_skip.h`. Offsets that `src/common/code_offsets.cpp` reads from code are checked by step 1.
  4. Add the exe's sha256 to `known_builds` and the new RVAs to every `SigDef::rva` in `src/common/signatures.cpp`, and
     the new values to every `kCodeOffsets` entry in `src/common/code_offsets.cpp` (the tests fail while one is missing).
  Build 1273043 was added this way: no component layout changed, but the save state moved by 0x10 (found in game;
  EMF reads those offsets from code). Build 1283531 too: every ECS program entry grew by 8 bytes, which moved the
  world's block after the program table by +0x1C00; EMF now reads the entity storage's offset and the program table's
  count and stride from code (`src/common/world_offsets.h`), and five types changed size (docs/ENGINE_MAP.md).
