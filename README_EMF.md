# EMF: Enshrouded Modding Framework

Native code mods for Enshrouded (known builds 1269579 and 1273043; other builds run on signatures only), in the spirit of BepInEx + Jotunn.
EMF loads your mod DLLs into the game, finds engine functions by byte signature, and gives mods a small,
stable C API to read and change the game's ECS (entities, components, singletons) at a safe point every tick.

Status: version 0 (the exact version is in `VERSION`), experimental. Runs in your own game, a game you host, and a dedicated server ([docs/modding/server.md](docs/modding/server.md)). Tested in a local single-player world (host) and on a dedicated server.

**Release package:** `powershell -File scripts\package.ps1` (after `bash scripts/test.sh`) builds `build\package\EMF-<version>.zip`: players run its `install.cmd`; modders get the SDK, a mod template (`template/`), the docs and the mod sources. `scripts/test_package.ps1` (run by `verify.sh`) checks the install scripts and builds the template from the unzipped package.

**Mod manager:** `EMF.exe` lists every installed mod (EMF, EML/ShroudForge, DLL mods EMF starts, other loaders), switches them with the game closed, saves their settings, keeps mod profiles (the switches under a name, to switch setups in one click, and to share a modlist), says which ones will not load and why, shows clashes and starts the game: [docs/app.md](docs/app.md). The package installs it as `<game>\emf\EMF.exe`.

**Writing your own mod?** Lua: [docs/modding/lua.md](docs/modding/lua.md) (no compiler, live reload). C++: start with [docs/modding/README.md](docs/modding/README.md): a step-by-step first mod, a guide to how the
game and EMF work, a cookbook of tested recipes, the full API reference, and what to do after a game update.

## What is in the repo
| Path | What |
|---|---|
| `src/common/` | scanner, signatures (37), code offsets (5), MinHook wrapper, Keen hash map reader, safe world reader, logger |
| `src/probe/` | `emf_probe`: read-only test DLL (logs worlds, player position, villager states) |
| `src/loader/` + `src/proxy/` | `dinput8.dll` loader: forwards DirectInput to Windows, starts `emf/emf_core.dll` |
| `src/core/` | `emf_core.dll`: hooks the ECS sim step, runs mod callbacks, program hooks, mod programs (with engine events), spawn, items, save/load events, developer commands, the in-game console (`overlay/`: Dear ImGui through a Vulkan layer), loads mods |
| `app/` | `EMF.exe`, the desktop mod manager: a WebView2 page in a Win32 window ([docs/app.md](docs/app.md)) |
| `src/saves/` | save backups: the engine shared by EMF.exe's Saves tab and the core's automatic backups and `saves.*` commands, and `save-info.json` ([docs/saves.md](docs/saves.md)) |
| `sdk/include/emf/` | mod SDK: `emf_api.h` (C API v20), `emf.hpp` (C++ helpers), `generated/components.hpp` (661 components, 65 dynamic halves, 129 events) |
| `mods/sample_mod/` | example EMF mod (see below) |
| `mods/companion/` | G: a companion that follows you, fights at your side, takes orders (stay, attack) and wears gear you give it; saved per world (see below) |
| `mods/clock/` | the time of day on screen (read-only, on by default) |
| `mods/quick_stack/` | Quick Stack and Chest Machines: tap L to put what you carry into nearby chests that already hold it; hold L at a chest for sorting chests, recyclers and overflow chests; `/qs` for every player (see below) |
| `mods/villager_jobs/` | K: villagers farm, tend animals, haul between chests and craft (or trade) at a chest, assigned with a radial wheel; saved per world (see below) |
| `mods/big_stacks/` | bigger item stacks (x4, at most 9999) by changing the game's item definitions in memory |
| `mods/raids/` | Night Raids: at nightfall, by chance, waves of Vukah march on a base where players are; off in each world until `raids.world on` (see below) |
| `mods/loot_magnet/` | Loot Magnet: corpses of kills near a player are looted into them, for every player on the host or a dedicated server; `/loot.toggle` (see below) |
| `mods/champion/` | Wandering Champion (Lua): a roaming mini-boss appears 80-150 m away at night; the screen shows where; a bonus for the slayer (see below) |
| `mods/bounty/` | Bounty Board (Lua): daily kill, gather and champion bounties per player, `/bounty.board` and `/bounty.claim` for every player (see below) |
| `mods/grave/` | Grave Keeper (Lua): `/grave.where` finds your tombstone, `/grave.claim` brings its items back from a distance (see below) |
| `mods/sleep/` | Sleep Through the Day: sleeping in a bed by day skips to the evening (see below) |
| `mods/herald/` | Server Herald (server side, off by default): a message of the day, join/leave/death lines, announcements, a restart countdown, a chat log, playtime, `/players`, an opt-in Discord webhook (see below) |
| `mods/handover/` | Hand-over (Lua, off by default): `/handover wood 20` gives items to the nearest other player; nothing is ever lost; `/handover.block` refuses them (see below) |
| `mods/teleports/` | Waypoint Teleports: `/unstuck` and `/home` for every player, `tp`, `bring`, `tp.pos`, `tp.back` for admins (see below) |
| `mods/bulk_craft/` | Bulk Craft (off by default): craft one by hand, then U at the station (or `/craft 25`, every player) crafts many through the game's own crafting, at the recipe's pace (see below) |
| `mods/beastmaster/` | Beastmaster (Lua, off by default): tame wolves, boars, sabretooths and hounds (bears, aurochs and panthers with the content packs), keep 8, call 2 to fight beside you; hold B for the beast wheel, `/beastmaster` for every player; rare wild pack beasts (see below) |
| `mods/necromancer/` | Necromancer (Lua, off by default): raise up to 3 skeleton warriors that stay and follow you (a lich and a bone golem with the undead pack), cast raging spirits (flaming skulls, 6 s); hold V for the summoning wheel, shared with Beastmaster; `/necromancer` for every player (see below) |
| `mods/loot_scale/` | more materials per loot stack (x2 by default, off by default) by hooking the game's loot amount getter by its bytes; switching it off takes effect at the next start, factor 1 at once |
| `content/emberbrand/` | an example of a new item (an EML data patch; [docs/modding/content.md](docs/modding/content.md)) |
| `mods/event_log/` | logs gameplay events on the host (deaths, hits, pickups, crafts, xp, building) with `on_event` |
| `mods/guid_dump/` | EML (Lua) export mod: writes every template name → GUID to `export/template_guids.csv` (12,266 templates) |
| `tools/sigcheck/` | offline check that every signature matches the installed exe |
| `tools/gen_headers.py` | regenerates `components.hpp` from EML's `types.json` |
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
(`package/install.ps1`) renames another mod's `dinput8.dll` instead, lists it in `dinput8-chain.ini` and records it in
`emf\moved.txt`; EMF's loader starts every DLL listed in `dinput8-chain.ini` from its `DllMain`, before the core
(`src/loader/chain_list.h`, `chain_load.h`; results in `emf\emf_loader.log` and `emf\chain_status.txt`).
`docs/modding/other_mods.md` has the details for authors of DLL mods.
Logs: `<game>\emf\emf_core.log` (framework + mods), `<game>\emf\emf_loader.log` (the loader: chain-loaded DLLs and a
core that fails to load).
Mod settings: the console's Mods tab or the `set` command (below). Each mod starts with the defaults from its
`mod.json`; a change is saved in `<game>\emf\config\<mod>.ini`.

## In-game console
Press the key under Esc (`~` on US keyboards, `§` or `|` on Nordic ones, `^` on German ones) in the game: a console
drops down over the top half of the screen. Type a command (see the table below) and press Enter; the reply appears
under it (green when it worked, red when it did not).
- **Tab** completes command names, item names for `give` and `stack`, and component names for `tpl` and `tplset`;
  press it again for the next match (a list of matches shows under the input).
- **Up / Down** go through the commands you typed before (kept in `<game>\emf\console_history.txt`).
- **Page Up / Page Down** and the mouse wheel scroll back; the console keeps the last 2000 lines.
- **Esc** or the same key closes it.

While it is open the game gets no keyboard or mouse input (only key releases, so nothing stays held) and mods' hotkeys
are paused. On the main menu, `help`, `mods`, `settings`, `set`, `profile`, `saves.backup`, `saves.list`, `hooks` and `pattern` answer at once; the other
commands answer `not run: load a world you host first`, and in someone else's game `not run: commands need a world you
host`.

### The Mods tab
The console has two tabs, **Console** and **Mods** (click one, or Ctrl+Tab). The Mods tab lists every mod in
`<game>\emf\mods`, coloured by what became of it (loaded, a Lua script, disabled, failed, not loaded). Click one to see its
description and status, to switch it on or off ("Switched on", from the next game start) and to change its settings:
checkboxes, sliders, lists and text fields, with the range and the default when you hover over one. Settings marked
"(next start)" apply when you start the game again, the others at once. The same from the command line:
`settings <mod>` lists a mod's settings with their values, `set <mod> <key> <value>` changes one (`set big_stacks
enabled 1` switches a mod on). A value out of range or of the wrong kind is refused with the reason. Changes are saved
in `<game>\emf\config\<mod>.ini`.

At the top of the Mods tab, **Profile** is the picker for mod profiles (the on/off state of every mod under a name,
the same files as EMF.exe's, [docs/app.md](docs/app.md#profiles)): it shows the current one, or "(changed)" when a
mod it lists is switched another way now; **Load** switches every mod to the chosen profile from the next game
start and shows what it did; **Save as** saves the switches (and, with "with settings", the EMF mods' settings) under
a new name, and asks before it replaces one. The console's `profile` command does the same.

Settings in `<game>\emf\emf_core.ini`:
```
[overlay]
enabled=1      ; 0 turns the console off
key=0x29       ; the key's scan code (0x29 = the key under Esc)
chat=1         ; 0 turns chat commands off (below)
```
How it draws: the console is a Vulkan layer (`VK_LAYER_EMF_console`, manifest `<game>\emf\vk\`). EMF's `dinput8.dll`
enables it for the game process only; nothing is written to the registry and other games do not see it. It works next
to RivaTuner/RTSS, the Steam overlay and the Discord overlay.

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
| `profile` | the mod profiles in `emf\profiles` (the on/off state of every mod under a name, as EMF.exe's Mods tab keeps them), the current one and whether the mods changed since (works on the main menu too) |
| `profile save <name> [--settings] [--replace]` | saves the switches of every mod (EMF, EML, the DLLs in `dinput8-chain.ini`; others as read-only) as a profile; `--settings` also saves the EMF mods' settings; another profile with that name is replaced only with `--replace` (e.g. `profile save "Big Base" --settings`) |
| `profile load <name>` | switches every mod the profile lists to its state (through the same switches as EMF.exe) and reports what it did, what failed, what is missing (with Nexus ids) and what is not in the profile, and says what will still differ; restart the game to apply. A profile's settings apply at once in the Mods tab. The answer comes a moment later: the work runs on a thread of its own |
| `saves.backup [label]` | backs up every save file now, Steam Cloud and Local, into `%LOCALAPPDATA%\EMF\save-backups` ([docs/saves.md](docs/saves.md)); only at the main menu: while a world runs it says why not (the game writes its saves every 30 seconds) |
| `saves.list` | the save backups, newest first, with what each holds and where they are; restoring needs the game closed (EMF.exe's Saves tab, or by hand: [docs/saves.md](docs/saves.md)) |
| `items [part of a name]` | the game's items with their debug names, id and stack size (e.g. `items Wood_Log`) |
| `give <item name> [count]` | drops items 1 m above you; the game collects them (e.g. `give Material_T1_Wood_Logs 3`) |
| `put <item name> [count] [entity] [level] [rarity 0-4]` | puts items straight into an inventory, yours by default (e.g. `put Weapon_T1_1H_SwordEpic_01`, `put Material_T1_Wood_Logs 2 1025` for a villager, `put Weapon_T1_1H_SwordEpic_01 1 1 10 3` for a level-10 Epic sword; the game may cap the level an item can have); the reply comes one tick later with how many fitted, and why when fewer did (`0 added (entity 77 has no inventory ...)`, `3 added (only 3 of 5 fit: no room for the rest)`) |
| `take <item name> [count] [entity]` | takes items out of an inventory, yours by default: all or nothing (`take Material_T1_Wood_Logs 5`; when fewer are there nothing is taken and the reply says how many were missing). It searches every inventory of the entity, **equipped gear included**, and a taken gear item is gone for good (its level and perks with it) |
| `spawn <template guid> [count 1-20]` | spawns templates 2 m from you (GUIDs from `export/template_guids.csv`) |
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

Example reply (from a test run):
```
[cmd] > give Material_T1_Wood_Logs 3
[cmd] gave 3 x Material_T1_Wood_Logs to player 1
[cmd] > time 12
[cmd] time of day is now 12.00 h (was 15.35 h)
```

## The sample mod
- Every 5 s (client world): the local player's position, and how often the hooked `client_player_state` program ran.
- Every 10 s (server world, host only): how often its own ECS program `sample_mod_server_tick` ran, and the villagers
  with their AI state and position.
- Logs `world loaded` / `world saved` from the load and autosave events.
- Adds the command `sample.villagers`: the villagers of your world with their AI state and position.
- With `enable_write=1` (host only):
  - **F8** moves the time of day forward 3 hours (the game's own `setTimeOfDay`).
  - **F9** spawns the template in `spawn_guid` (default: a barrel prop) 2 m from the player; **F11** spawns
    `spawn_guid2` (default: a test townsfolk).
  - **F10** destroys the last spawned entity.
  - **F12** gives the player `give_item` × `give_count` (an item's debug name, e.g. `Material_T1_Wood_Logs`) as a
    pickup dropped 1 m above them; the game's loot code collects it into the backpack (with the default
    auto-collect setting). `dump_items=1` writes every item (name, id, GUID,
    stack size) to `emf\items.csv` when the world loads (build 1269579 has 4207 items).
  - **F7** makes the villager nearest to the player walk to the player (the engine's own "go to position"
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
With `-EnableWrite` (`emf\config\companion.ini`: `enabled=1`), **G** (or F6) on the host spawns a Vukah warrior
(`Enemy_Vukah_Melee`) 2 m from you, turns it into an ally and makes it guard you through the game's own
`StartGuardingEvent`, sent from the mod's ECS program. It follows you (when you walk away it catches up and stays 1-3 m
from you; while you stand still it may wander a few metres), never attacks you, and **fights at your side at full
damage**: in the test it hit a hostile Vukah three times for 48 and killed it, and the game credited you with the kill
("Enemy slain", XP).

**Orders.** With the companion out, hold **G**: a wheel titled "Companion (level N)" offers

| Entry | What happens |
|---|---|
| Follow me | it guards you again (the default) |
| Stay here | a small barrel is put down where the companion stands and it guards that instead: it stays there when you walk off (and still fights what comes near). Follow me (or Dismiss) removes the barrel. If the barrel keeps disappearing (smashed, or out of the loaded area) it is put back at most every 5 s, and after the third time the companion follows you again ("The companion cannot keep its stay spot") |
| Attack (what you look at) | the creature under your crosshair within 40 m becomes its target: the game's own threat event, sent again every 2 s while the target lives and is within 40 m of you, for at most 30 s ("The companion gave up the attack"). Players, other allies and villagers are never picked |
| Gear | a second wheel: give it a weapon or armour from your backpack / action bar, or take all of it back (below) |
| Dismiss | it leaves (`StopGuardingEvent`, then removed) |

Release G over an entry, or tap G and click. Each order can also have its own key (`key_follow`, `key_stay`,
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

| companion.ini | What it does |
|---|---|
| `key=G` | the order wheel key (with no companion out it summons one): a letter, a digit or a keyboard key's virtual-key code (`0x71` = F2; mouse buttons are refused); counts only while the game window is in front |
| `key_follow=` `key_stay=` `key_attack=` `key_dismiss=` | optional one-key orders, same format; empty = off |
| `gear_bonus_max=10` | the most levels gear can add |
| `marker_guid=` | what "Stay here" puts down for it to guard (default `0ce28db1-522f-4455-b358-3888fb5e089b`, a small barrel). It must be a template the game can spawn standing still |
| `faction=11` | the faction number written on the companion right after it spawns (and again if the game changes it). 11 = `PlayerAlly`, the faction of the game's own summons: full damage. 7 = `Player` follows too but every hit does 0. -1 keeps the template's faction: **with the default enemy body that leaves a hostile Vukah next to you**, and other non-ally numbers can too. Anything that is not a number from -1 to 15 falls back to 11 (logged). Level and gear need 11 |
| `guid=` | the body (default `d5d05217-52dc-467a-9192-26b4f661d8d9`, `Enemy_Vukah_Melee`). Plain enemy-AI actors without `PlayerInput` work; the old default `FakePlayer_Vukah_Melee` (`9543cfb0-...`) follows but does no damage. Tested ones: `docs/engine_parts/re_companions.md` |
| `auto_summon=1` | summons one by itself once a world you host is running |
| `respawn_seconds=N` | after the companion dies (or is gone), a new one comes N seconds later; 0 (default): press G |
| `follow_distance=<m>` | changes the template's guard distances (start following at d + 2 m, stop at d m). Template data: with an enemy body it changes **every wild Vukah of that template** too, until the game restarts. Not set by default |

Console: `companion.gear` (what it wears, its level, and pieces still on their way back to you), `companion.aim` (your camera ray and the creatures near it, with
how far each is off the ray: why Attack picks what it picks). On the host the faction reaches your own screen too (the
client world already shows the ally faction); other players of an online session were not tested.

Template GUIDs come from `export/template_guids.csv` (run the `guid_dump` EML mod once). Write them as text;
`guid_layout` ms (the default; `guid_layout=0` in the ini, 1 = raw) converts them to the byte order the game stores.

## The villager_jobs mod
With `-EnableWrite` (`emf\config\villager_jobs.ini`: `enabled=1`), villagers on the host work for you at a chest,
through the game's own systems: they farm, tend animals, haul goods between chests, and craft (or trade).

1. Look at a villager and press **K** (within 8 m; the crosshair picks). A note says "Villager 1026 chosen".
2. Within 30 s look at a chest and **hold K**: a wheel asks what to do there:
   - **Farm the crops here (N plants, M ripe)**: the villager walks to each fully grown plant within 20 m of the chest
     and harvests it straight into the chest (the game's own loot roll); the plant then grows again from a seedling,
     so the field stays planted (no seed is used). Plants still growing, or capped with the game's growth limit, are
     left alone. In the test a villager harvested three tomato plants (2 tomatoes each) and all three regrew.
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
   Move the mouse toward an entry and release K (or tap K and click). Esc, a right click or releasing in the middle
   cancels; the mouse wheel turns the page when there are more than 8.
3. Crafting: the villager walks to the chest (sent there again every 10 s while it is more than 4 m away) and makes
   one item every `seconds_per_item` (default 30) while it is within 6 m. When the chest runs out of materials (or has
   no room) a note says so once; it goes on by itself when you refill the chest.
4. K on a working villager opens a wheel with "Stop farming (12 harvests)" (or the job it has: "Stop tending (3
   collected, 20 fed)", "Stop hauling (40 items moved)") and "Keep working".

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
altar; look at it or name its id; API v20 teleport with the game's ground search). Recipes that need a crafting station's props are not offered; a recipe the game refuses for
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
player), `raids.world on|off`, `raids.start [seconds]`, `raids.stop`, `raids.kill` (host). Settings: chance, hour,
warning, quiet nights, waves, raiders per wave, gap, difficulty, boss wave, who raids (the tested Vukah, or your own
template GUIDs), which base, base radius, reward, corner and colour. Spawned raiders are not saved: a reload ends the
raid. Details: `mods/raids/README.md`.

## The loot_magnet mod (Loot Magnet)
Installed switched off: switch it on in the Mods tab or EMF.exe. The corpses of creatures killed near a player are looted into that player a moment later, with the game's own loot
event. It runs where the world runs, so on the host or a dedicated server it works for every player, also those who
joined without EMF. Each player switches their own with `/loot.toggle` (and sees it with `/loot.status`); kept per world
and account. Never a corpse twice, a player's or an ally's body, a farm animal, anything tame or owned, or anything
within the base radius of a flame altar; by default only corpses that hold nothing but materials. A full inventory
loses nothing (what does not fit stays in the corpse). Settings: reach, delay after the kill, how long it looks for loot (the game gives a corpse its loot 1-3 s after the death), what comes, arrows,
default for new players, the host's key, base radius. Details: `mods/loot_magnet/README.md`.

## The champion mod (Wandering Champion, Lua)
Installed switched off: switch it on in the Mods tab or EMF.exe. Every so often, while a player is outdoors at night, a champion (the Vukah brawler mini-boss, with its boss bar)
appears 80-150 m away at that player's level + 2, patrols spots players walked, and hunts a player who comes within
30 m. The line on screen shows its distance and direction; the slayer gets 10 runes and a rare weapon at their level;
it leaves after 10 minutes, at dawn or when everyone is far away. Commands: `champion.where`, `champion.stats` (every
player), `champion.summon`, `champion.dismiss`, `champion.kill` (host). The template table and the rewards are at the
top of `emf\mods\champion\main.lua`: change them while the game runs. Details: `mods/champion/README.md`.

## The bounty mod (Bounty Board, Lua)
Installed switched off: switch it on in the Mods tab or EMF.exe. Every day each player gets three bounties: slay
creatures of a faction ("Slay 6 Vukah"), gather a raw material picked up in the world ("Gather 60 Wood logs", never
out of a chest or a tombstone), or slay a champion (2 levels above you, 1,000 health or more). Kills count for every
player who hit the creature in its last 10 s (`share_kills`). Rewards (runes, furs, gear at your level) with
`/bounty.claim`, or at once with `auto_claim`; what does not fit is dropped at your feet. Commands for every player:
`/bounty.board`, `/bounty.claim`, `/bounty.reroll <n>`, `/bounty.top`; the host has `bounty.give <id>` and
`bounty.reset`. New bounties every real day at an hour, every game dawn, or every few hours of play; kept per world
and account. The bounties are the `POOL` table at the top of `emf\mods\bounty\main.lua`. Details:
`mods/bounty/README.md`.

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
per world). Players who joined need nothing; the host also has a wheel on a key (H). Details: `mods/handover/README.md`.

## The teleports mod (Waypoint Teleports)
Installed switched off: switch it on in the Mods tab or EMF.exe. Every player may type `/unstuck` (a short hop with the
game's ground search to free ground nearby, else a /home to their spawn point; not within 10 s of a hit, once a minute,
allowed in the shroud) and `/home` (to their spawn point's landing pads, else the nearest flame altar; every 10
minutes, an optional item cost, not from inside the shroud). Both wait for a 3 s countdown standing still (moving 2 m
cancels). Admins get `tp <#k>`, `tp <#k> <#m>`, `bring <#k>`, `tp.pos [#k] x y z [ground]`, `tp.back [#k]` and
`tp.list`. It moves players the way the game's fast travel does (API v20 `teleport`). Details: `mods/teleports/README.md`.

## The bulk_craft mod (Bulk Craft)
Installed switched off: switch it on in the Mods tab or EMF.exe. Craft one by hand, then craft many: look at a
crafting station and press **U** for a wheel with the recipes you crafted there by hand, then x5 / x10 / x25 / max;
or `/craft <n|max> [part of the name]` (every player, also from the chat), `/craft.stop`, `/craft.list`. The game
crafts each one through its own craft request with you as the crafter, from what you carry, all or nothing per craft:
the mod never moves an item. Because the game's request does not check unlocks, the mod repeats only recipes you
crafted by hand through the game's window (kept per world and platform account, with the items you chose for "any
fibre" inputs, sent back every time; `craft.teach` for the host), at a station within 5 m (never a factory, the altar,
an upgrade or a recipe with station requirements such as comfort or crafting props), one craft at a time at the recipe's
crafting time and at least 0.5 s, carried over between orders (`pace = instant`: recipes without a crafting time all at
once), at most 100 per order; it stops if a craft's materials did not leave your bag (base storage). Details:
`mods/bulk_craft/README.md`; how the game's craft request works: `docs/engine_parts/inventory_crafting.md`.

## The beastmaster mod (Beastmaster)
Installed switched off: switch it on in the Mods tab or EMF.exe (it needs the `lua` mod 1.9). A Beastmaster class:
tame a wild wolf, boar, sabretooth, hound or bear by beating it under 25 % health (`/beastmaster tame`, a 3 s channel
within 8 m) or by feeding it its bait 3 times within 10 s (`/beastmaster feed`). Up to 8 beasts per player are kept with
the world; call up to 2 (one of each kind) for 15 % mana with a 10 s cooldown (`/beastmaster call <name>`), send them
at what you look at (`/beastmaster attack`), dismiss them; a beast that falls rests 2 minutes. Your beasts' kills give
you XP and loot. Hold **B** for the beast wheel (the host); every player has `/beastmaster` in the chat. The bear comes
in an EML pack (`emf_beastmaster_bear`, a new model on the game wolf's skeleton): 1.4x the size, 2.5x the health, 1.5x
the damage, a little slower. The beasts pack (`emf_beastmaster_beasts`) adds the aurochs (a huge horned bull on the
boar's skeleton: 1.3x, 3x health) and the panther (a black big cat on the sabretooth's: 0.85x, quicker, harder hitting). Now and then a
wild one of the packs' beasts appears 50-70 m from a player. Without a pack its beasts are refused with a note. Details:
`mods/beastmaster/README.md`; the model tools: `tools/models/`.

## The necromancer mod (Necromancer)
Installed switched off: switch it on in the Mods tab or EMF.exe (it needs the `lua` mod 1.9). Raise skeleton warriors
(`/necromancer raise`, 25 % mana, up to 3) that guard and follow you until they fall and come back after a reload;
cast raging spirits (`/necromancer spirits`, 8 % mana): two flaming skulls rush what you look at, or the nearest foe,
for 6 s (at most 10). `attack`, `gather`, `dismiss`, `list` for every player; the kills are yours (never on bosses).
Hold **V** for the wheel: with Beastmaster installed, V and B open one "Summons" wheel with both classes' sections
(Lua 1.9 wheel groups; `shared_wheel` off gives a mod its own wheel). The undead pack (`emf_necromancer_undead`, EML)
adds a lich (`/necromancer raise lich`, 40 % mana, one; on the skeleton mage) and a bone golem (`raise golem`, 60 %,
one; 3 m, on the Vukah elite, 2x health). Details: `mods/necromancer/README.md`.

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
  2. Regenerate EML's `types.json` (`emm.exe run -g <game> --export --export-directory <dir>`, read-only) and compare it
     with the old one (component layouts); regenerate `sdk/include/emf/generated/components.hpp` with `tools/gen_headers.py`.
  3. Check the engine offsets the type registry does not describe. They live in `src/common/offsets.h` (world, events,
     items, program job, Keen map), `src/core/daytime.h` (Daytime / Weather, written by `time`), `src/core/programs.h`,
     `src/core/system_programs.h` (ProgramDesc), `src/core/spawn.h`, `src/core/events.h`, and the sample mod's
     `npc_command.h` / `time_skip.h`. Offsets that `src/common/code_offsets.cpp` reads from code are checked by step 1.
  4. Add the exe's sha256 to `known_builds` and the new RVAs to every `SigDef::rva` in `src/common/signatures.cpp`, and
     the new values to every `kCodeOffsets` entry in `src/common/code_offsets.cpp` (the tests fail while one is missing).
  Build 1273043 was added this way: no component layout changed, but the save state moved by 0x10 (found in game;
  EMF reads those offsets from code).
