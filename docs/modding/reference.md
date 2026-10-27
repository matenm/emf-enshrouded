# EMF API reference

The C API is one table of function pointers, `EmfApi`, declared in `sdk/include/emf/emf_api.h` and passed to your
`emf_mod_load`. It only grows: each version appends functions at the end (`EMF_API_VERSION` 20 today), so a mod built
for an older version works with a newer core. `sdk/include/emf/emf.hpp` wraps it in C++; prefer the wrappers.

**Threads.** "World callback" below means: inside `on_tick`, `run_on_world`, `on_event`, a command, a put/take result
or a wheel choice, on the world that callback received. "Program" means inside your mod program's own callback with
the context it received. "Any thread" functions are safe everywhere.

**Failure.** Functions report refusals by returning null / 0 / false and writing a line into `emf_core.log`. They never
throw. How often that line is written:

- the functions you call inside a program every tick (`emit_event`, `read_events`, `program_singleton`,
  `program_read_other`, `program_write_other`): each distinct refusal once, not on every tick;
- `put_items`, `put_items_ex`, `take_items` and `hud_set`: a repeated refusal at most every 10 s, followed by the
  number of repeats left out ("(12 more since the last one)");
- the rest, `find_pattern`, `hook_pattern` and `hook_address` included: every time. Those scan the exe or patch code
  on each call, so call them once (from `emf_mod_load`) and keep the result: a pattern that fails inside `on_tick`
  writes a line every tick.

## Loading

| C++ | C | Notes |
|---|---|---|
| `emf::init(api)` | — | Call first in `emf_mod_load`. Returns false (and logs why) when the core is older than the SDK you built with. |
| `EMF_LOG(mod, fmt, ...)` | `log(mod, fmt, ...)` | A line in `emf_core.log` prefixed `[mod]`. Any thread. |

`extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api)`: return true when loaded. Returning false
does not unload the DLL (what you registered stays registered), so validate first.

**Where mods live.** A mod is a folder `<game>\emf\mods\<id>\` holding `mod.json` and the DLL it names (folder mods,
since EMF 0.2), or a loose `<game>\emf\mods\<name>.dll` (id = the file name, lower case; no manifest, no
dependencies). The loader orders mods by their dependencies, then by folder/file name, and skips a mod whose
`[<id>] enabled=0` is set in `emf\config\<id>.ini` (the Mods tab's switch).

**mod.json:**

| Field | Meaning |
|---|---|
| `id` | required; 1-48 of `a-z 0-9 _ . -`; names the settings file and section (`emf\config\<id>.ini`, `[<id>]`) |
| `version` | required; `"1"`, `"1.2"` or `"1.2.3"` |
| `name`, `description`, `authors` | shown in the Mods tab and by `mods` (`authors` is a list of texts) |
| `dll` | the DLL in the folder (default `<id>.dll`); `main` instead names a script for a script host (Lua) |
| `emf` | the lowest `EMF_API_VERSION` the mod needs (a whole number); a newer one than the core's: not loaded |
| `depends` | the mods yours needs, with a version range each (below): `{"lua": ">=1.3", "helper": {"version": "~2.1", "optional": true}}`, or a list `[{"id": "helper", "version": "^2.0", "optional": true}, "other"]` (ShroudForge's form; its name `"dependencies"` works too, but not both). Yours loads after them; it does not load when a required one is missing, the wrong version, switched off or failed, and the Mods tab says which and why. An `optional` one may be missing or off; when it runs, it loads first and its version must fit. |
| `enabled` | the switch's value while the ini has none (default `true`) |
| `settings` | a list of settings, shown in the Mods tab and in EMF.exe (below) |
| `side` | where the mod has to run, for people who host a dedicated server: `"server"` (on the server or the host only; players do not need it), `"client"` (in a player's own game only: HUD lines, keys, notes; on a dedicated server it does nothing) or `"both"` (the server and every player need it). Optional: without it a mod counts as `"both"`, unknown. EMF loads the mod everywhere either way; the field only drives EMF.exe's notes for a server folder ("Players need this mod too", "Does nothing on a server", a warning for a client mod switched on there; docs/app.md "Servers"). Another value is a warning in EMF.exe, not an error. An EML mod's `mod.json` may say it too |

A broken `mod.json` rejects only that mod; the log and the Mods tab say why (with the line and column). Save it as
UTF-8 (a UTF-16 "Unicode" file is refused with that advice); at most 256 KB. `int` settings stay within
999999999999999 either way.

**Version ranges in `depends`** (SemVer, read as npm and Cargo read them; versions are numbers only, missing parts are
0):

| Range | Takes |
|---|---|
| `"*"` | any version |
| `"^1.2.0"`, `"^1.2"` | 1.2.0 up to below 2.0.0 (`^0.2.3`: below 0.3.0; `^0.0.3`: only 0.0.3) |
| `"~1.2.3"`, `"~1.2"` | 1.2.3 / 1.2.0 up to below 1.3.0 (`~1`: below 2.0.0) |
| `"1.2.3"`, `"1.2"` | a bare version: the same as `^` (that version or a compatible newer one) |
| `">=1.1"`, `">1.2"`, `"<2"`, `"<=1.2"` | comparisons; `>1.2` is newer than every 1.2.x, `<=1.2` up to every 1.2.x |
| `"=1.2.3"` | exactly that version (`=1.2` is exactly 1.2.0) |
| `"1.2.x"`, `"1.*"` | any number in place of the `x` |
| `"1.2.3 - 2.0.0"`, `"1.2 - 2"` | both ends included; a shorter end takes its whole family (`1.2 - 2`: 1.2.0 up to every 2.x, below 3.0.0) |
| `">=1.0, <2.0"`, `">=1.0 <2.0"` | all must hold |
| `"^1.0 \|\| ^2.0"` | either side |

A range that matches no version (`">=2.0 <1.0"`, `"<0"`) is refused. When a version does not fit, the reason says what
was found and what to do: `needs lua ^1.3 (1.3.0 or newer, below 2.0.0), found 1.2.0: too old (update lua)`, or
`too new (look for an update of this mod)`. Two mods that use each other
only optionally both load (the first by name goes first; the log notes it); a cycle of required dependencies rejects
the mods in it. EMF.exe's "Will not load" uses the same rules.

**Settings.** Each entry of `settings` is an object. Every kind takes `key` (1-48 of `A-Z a-z 0-9 _ . -`; `enabled` is
reserved for the mod's switch), `type`, `default`, `label` (default: the key), `help` (a line under it), `live`
(`true`: your mod applies a change at once; else the Mods tab says "next start"), `group` (the heading it is shown
under; settings without one come first, then each group in the order its first setting appears) and `enabledWhen`
(below). The kinds:

| `type` | Shown as | Extra fields | Stored and read as |
|---|---|---|---|
| `bool` | a switch | | `1` / `0`; Lua: true / false |
| `int`, `float` | a number box, or a slider when `min` and `max` are both given (for `int`: at most 1,000,000 apart, or with `"control": "slider"`) | `min`, `max`, `step`, `control` | the number; Lua: integer / number; a fraction for an `int` is refused (from the ini: rounded) |
| `slider` | a slider | `min` and `max` (required), `step` | **a whole number only with a whole `step`** (and whole `min`, `max`, `default`): `"step": 1`; without a step, or with a fractional one, a decimal. A value between steps moves to the nearest step. For a whole-number slider you can also write `"type": "int", "control": "slider"` |
| `choice` | a list | `choices`: `["top right", "bottom left"]` (default by index or text) | the index; Lua: integer |
| `select` | a list | `options`: `["a", "b"]` or `[{"value": "tr", "label": "Top right"}]` (default by value or label) | the option's value; Lua: text |
| `keybind` (or `key`) | a key: click it, then press a key (EMF.exe and the Mods tab); Shift, Ctrl or Alt bind when pressed and released alone (Shift+G binds G); EMF.exe binds the key the game sees (with NumLock off, Numpad 8 is Up) | | a key name as `emf/keys.hpp` reads it (`G`, `F7`, `Space`, `Numpad5`, `0xba`); `""` = no key. A default that is no key refuses that setting (logged); the mod still loads |
| `color` | a colour picker | | `#rrggbb` (`#rgb` and any case are read); C++: `emf::setting_rgba` for `emf::hud`; Lua: the text, which `emf.hud` takes |
| `text` | a text field | `maxLength` (1-256 characters) | the text: one line, at most 256 bytes, no spaces or quotes around it |

`"enabledWhen": {"setting": "show", "equals": true}` greys the setting out in the Mods tab and EMF.exe unless the other
setting (of the same mod) has that value. `equals` must be a value that setting holds as written: `true`/`false` only
for a switch, a number it can hold without being moved to a step, a choice's index or text, a select's value or label.
It only greys out: the value stays, and your mod decides what it means. A condition on a setting that is itself
refused is dropped, with a log line, so the setting is never greyed out for good.

A mistake in `step`, `options`, `maxLength`, `group`, `enabledWhen` or `control` on one of the old kinds (`bool`,
`int`, `float`, `choice`, `key`, `text`) refuses only that setting, with a log line; on a new kind (`slider`,
`select`, `keybind`, `color`) it refuses the mod. `null` counts as a field left out.

Values are checked the same way in the Mods tab, the console's `set`, EMF.exe and `emf::set_setting`: out of range is
refused (an ini value out of range is clamped and written back), a number between steps moves to the nearest step, a
select or choice must be one of its options, text must be UTF-8. A setting added to or changed in `mod.json` while
the game runs shows in the Mods tab within a second (while a world is loaded); one taken out stays until the next
start. Such a live change fits the saved value to the new definition in memory only (it never writes the ini), and a
value that moves is reported to the mod as a change.

## v1: worlds, entities, timing, engine code

| C++ | C | Rules |
|---|---|---|
| `emf::server_world()`, `emf::client_world()` | `server_world()`, `client_world()` | the most recently stepping world of that kind or null (no server world on a client of someone else's game). Any thread, but use the world only in its own callbacks. |
| `w.is_server()`, `w.is_client()` | `world_kind(w)` | `EMF_WORLD_SERVER`, `EMF_WORLD_CLIENT`, prediction. |
| `w.tick()` | `world_tick(w)` | the world's tick counter; 0 on a game build where its fields are not confirmed. |
| `w.ecs_time_ns()` | `ecs_time_ns(w)` | "now" as the game's programs see it (tick × tick length, 60 Hz). |
| `w.singleton(name)` | `singleton(w, name)` | a world singleton by name (`"g38_daytime::Daytime"`, `"g38_items::RecipeRegistry"`), or null. World callback. |
| `emf::local_player()` | `local_player()` | the local player's entity id (from the client world), 0 before it exists. Any thread. |
| `w.get<T>(id)` | `get_component(w, hash, id, &size)` | the entity's component (`T::kHash`), null if absent; the C++ form also refuses a size mismatch. Split components: ask for `DynamicX`. World callback. |
| `w.for_each_entity(f)` | `for_each_entity(w, cb, user)` | every entity id (a snapshot taken first). World callback. |
| `emf::on_tick(f)` | `on_tick(cb, user)` | `f(World)` after every step of every world, on its thread. Register any time. |
| `emf::run_on_world(kind, f)` | `run_on_world(kind, cb, user)` | `f(World)` once, after the next step of a world of `kind`. Any thread. |
| `emf::resolve<Fn>(name)` | `resolve(name)` | the address of an engine signature by name (`src/common/signatures.cpp`), or null. |
| — | `hook(name, detour, &original)` | hook an engine signature (MinHook). False if unknown or unusable. |

## v2: programs, spawning, saves

| C++ | C | Rules |
|---|---|---|
| — | `hook_program(name, pre, post, user)` | wrap a game ECS program's update by name (`docs/ecs_programs.md`): `pre` before, `post` after, on game worker threads. Programs sharing one function all trigger it. |
| — | `register_program(name, kind, fn, user)` | your own per-tick system program (iterates no entities). Register in `emf_mod_load`; at most 16 mod programs. In C++ use `emf::add_program` (v3, below), which also declares events. |
| `emf::api()->spawn(w.raw(), guid, x, y, z)` | `spawn(w, guid, x, y, z)` | spawn a template (16-byte GUID as stored; `emf::parse_guid("xxxxxxxx-...")`) at metres; returns the entity id or 0. Server world, world callback. **Not saved** by the game (except villagers). |
| — | `destroy(w, id)` | remove an entity (never a player). Same rules as spawn. |
| `api()->on_save(cb, user)` | `on_save` | `cb(slot)` after the host wrote a world save (save thread). |
| `api()->on_load(cb, user)` | `on_load` | `cb(slot)` after the host loaded a world save, before the world starts. |

## v3: events in programs

| C++ | C | Rules |
|---|---|---|
| `emf::add_program(name, kind, {"EventIn"...}, {"EventOut"...}, f)` | `register_program_def(&def)` | a program that reads `events_in` and sends `events_out` (short type names, at most 24). A program whose events a world lacks is left out of it (logged). Register in `emf_mod_load`. |
| `emf::emit<E>(ctx)` | `emit_event(ctx, name, size)` | inside the program: a zeroed event of type `E` to fill in; game programs see it next tick. Null when not declared, size mismatch, buffer full, or 256 bytes or more. |
| `emf::read<E>(ctx, f)` | `read_events(ctx, name, size, cb, user)` | inside the program: `f(const E&)` for each event of the type this tick. |

## v4: gameplay events

| C++ | C | Rules |
|---|---|---|
| `emf::on_event<E>(kind, f)` | `on_event(kind, name, size, cb, user)` | `f(World, const E&)` for every event `E` in worlds of `kind`, after the step, on its thread (components, spawn allowed). Register in `emf_mod_load`; at most 24 event types per world kind across all mods. |

## v5: items

| C++ | C | Rules |
|---|---|---|
| `emf::for_each_item(w, f)` | `for_each_item(w, cb, user)` | every item definition (`EmfItemInfo`: id, name, max stack...). Walks ~4200 items: cache what you need. |
| `emf::item_id(guid)` | `find_item(w, name)` | an item id by GUID / by debug name (exact, then case-insensitive). |
| — | `item_info(w, id, &info)` | fill an `EmfItemInfo` (`info.size = sizeof(info)` first). |
| `emf::give_item(w, player, name, n)` | `give_item(w, player, id, n)` | drop a loose pickup 1 m above `player` that the game collects. Server world, world callback. |
| — | `spawn_ex(w, guid, x, y, z, comps, n)` | spawn with up to 15 extra spawn components (not `CurrentTransform`). |

## v6: console commands

| C++ | C | Rules |
|---|---|---|
| `emf::add_command(name, help, f)` | `register_command(name, help, cb, user)` | `f(World, args)` when typed in the console or `emf\commands.txt` (`args[0]` is the name). Name `mod.command`, 1-32 of `A-Za-z0-9_.-`. Runs on the host's server world after its step. Register any time. |
| `EMF_REPLY(fmt, ...)` | `command_reply(fmt, ...)` | a reply line to whoever typed the running command. |

## v7: template data and item definitions

| C++ | C | Rules |
|---|---|---|
| `w.get_template<T>(id)` | `get_template_component(w, hash, id, &size)` | the template half of a component (shared by every entity of the template, both worlds). Read-only memory. |
| `emf::write_value(&field, v)` | `write_memory(dst, src, n)` | write into read-only game data (template data, item definitions). Refuses code and constants. Lasts until restart, never saved. |
| — | `item_data(w, id, &size)` | the game's 2088-byte `ItemInfo` (read-only; layout in `docs/engine_parts/inventory_crafting.md`). |
| `emf::set_item_max_stack(id, max)` | `set_item_max_stack(id, max)` | change an item's maximum stack size in every live world. World callback. |

## v8: program reads, inventories

| C++ | C | Rules |
|---|---|---|
| `emf::add_program(name, kind, in, out, singletons, other_read, f)` | `EmfProgramDef.singletons/.other_read` | declare world singletons and other entities' components your program reads. |
| `emf::program_singleton(ctx, name)` | `program_singleton(ctx, name)` | a declared singleton, inside the program. |
| `emf::read_other<T>(ctx, "Name", id)` | `program_read_other(ctx, name, id, size)` | another entity's component through the game's accessor, inside the program; read only. Cross-checked against the engine's lookup on first use. |
| `emf::put_items(w, entity, "Item_Name", n, f)` | `put_items(w, entity, id, n, cb, user)` | put items into an entity's inventory through the game's transaction on the next server step; `f(World, added)`. Host only. Refused (false, logged) at once for bad input; later failures give `added` 0 with the reason logged. |

## v9: sessions

| C++ | C | Rules |
|---|---|---|
| `emf::server_session()` | `server_session()` | a number that changes with every hosted session (new game, reload, other world). Compare this, never world pointers. Any thread. |

## v10: radial wheel and notes

| C++ | C | Rules |
|---|---|---|
| `emf::wheel(title, items, hold_vk, kind, f)` | `wheel_open(&EmfWheel)` | a radial menu in the middle of the screen (1-64 entries, 8 per page; `"Name (details)"` shows details on a second line). With `hold_vk` held when it opens, releasing the key picks; a quick tap or no key: a click picks; Esc / right click cancel. `f(World, choice)` (−1 = cancelled) after the next step of a world of `kind`. One wheel at a time; refused while the console is open. |
| `api()->notify(fmt, ...)` | `notify(fmt, ...)` | a line at the top of the screen for a few seconds (and in the console and log). Any thread. |

## v11: items with level and rarity, taking items

| C++ | C | Rules |
|---|---|---|
| `emf::put_items_ex(w, entity, id, n, level, rarity, f)` | `put_items_ex(w, &EmfItemsRequest)` | put with an item level (0 = default) and rarity (0-4 with `EMF_ITEMS_RARITY`; the C++ form: 0xFF = default). |
| `emf::take_items(w, entity, id, n, slot, f, exact)` | `take_items(w, &EmfItemsRequest)` | take `n` all or nothing; `slot` (an `InventorySlotId`: inventory entity in bits 0-31, slot 0-7 in 32-39, one of the entity's own inventories) is tried first, or used alone with `EMF_ITEMS_EXACT_SLOT`. A gear item's data entity is destroyed. `f(World, taken)`. Host only. |
| `emf::has_v11()` | — | true when the core has these. |

## v12: text on screen

| C++ | C | Rules |
|---|---|---|
| `emf::hud(id, text, place, rgba)` | `hud_set(id, text, place, rgba)` | keeps a line of text on screen until changed; empty text removes it. `id`: 1-48 of `A-Za-z0-9_.-` (prefix your mod); `place`: `EMF_HUD_TOP_RIGHT` (default), `EMF_HUD_BOTTOM_RIGHT`, `EMF_HUD_BOTTOM_LEFT`; `rgba` 0xRRGGBBAA (0 = default). At most 32 lines in all; hidden while the console is open and while no world runs (main menu, loading); text longer than 160 bytes is cut. Any thread. |
| `emf::has_v12()` | — | true when the core has it. |

## v13: settings and mod info

| C++ | C | Rules |
|---|---|---|
| `emf::setting_int(mod, key, fallback)`, `setting_float`, `setting_bool`, `setting_text(mod, key, fallback)` | `setting_int`, `setting_float`, `setting_text(mod, key, buf, size)` | the current value of a declared setting (bool = 1/0, choice = index); `fallback` when it is not declared (or on an older core). Values from the ini are already checked: out-of-range numbers are clamped, bad values replaced by the default. Any thread. |
| `emf::setting_rgba(mod, key, fallback)` | — | a `color` setting as `0xRRGGBBAA` (full alpha), ready for `emf::hud`; `fallback` when it is not declared or holds no colour. Reads through `setting_text`. |
| `emf::set_setting(mod, key, value)` | `setting_set(mod, key, value)` | changes and saves a setting with the same checks as the Mods tab; false (logged) when refused. Change callbacks run as for any change. |
| — | `setting_declare(mod, &EmfSettingDef)` | declares a setting at run time (mods without a `mod.json`); `EMF_SETTING_BOOL/INT/FLOAT/CHOICE/KEY/TEXT`, `EMF_SETTING_HAS_MIN/HAS_MAX/LIVE`, `choices` as `"a|b|c"`. The newer kinds (`slider` steps, `select`, `color`, `maxLength`) and `group` / `enabledWhen` are declared in `mod.json`. |
| `emf::on_setting_changed(mod, f)` | `on_setting_changed(mod, cb, user)` | `f(World, key)` after each change of one of `mod`'s settings (`key` `"enabled"` = its switch; `mod` NULL in C = every mod), after the next step of any world, on its thread; waits while no world runs. A faulting callback is switched off. |
| `emf::mod_info(id)` | `mod_info(id, &EmfModInfo)`, `mod_info_at(i, &EmfModInfo)` | what the loader found: id, name, version, folder, `EMF_MOD_LOADED/FAILED/DISABLED/REJECTED/SCRIPT`. |
| `emf::has_v13()` | — | true when the core has these. |

## v14: patterns, hooks, build info

| C++ | C | Rules |
|---|---|---|
| `emf::find_pattern(bytes, offset, follow_rel32, rel32_tail, section)` | `find_pattern(&EmfPattern)` | the address an IDA-style byte pattern (`"48 8B 05 ?? ?? ?? ??"`) finds in the game exe's `.text` (or `.rdata`, `.data`): it must match exactly once; `offset` is added; with `EMF_FOLLOW_REL32` the int32 at that address is followed (call/jmp targets, RIP-relative data: result = match + offset + 4 + `rel32_tail` + int32), and the result must lie in the exe. 0 (logged with the reason) otherwise. Scans each call: find once, keep it. Any thread. |
| `emf::hook_pattern(name, bytes, detour, &original, offset, follow)` | `hook_pattern(name, &EmfPattern, detour, &original)` | find + inline hook. `original` (may be null) is set before the hook is live. **Hooks chain:** when EMF or another mod already hooks the function, yours runs first and `original` calls the previous detour. `name` shows in the console's `hooks`. Hooks are never removed. |
| `emf::hook_address(name, address, detour, &original)` | `hook_address(...)` | the same for an address you computed; it must be executable code of a loaded image. |
| — | `hook(sig_name, detour, &original)` (v1) | chains too (a signature EMF itself hooks, like `SimStep`, can be hooked). |
| `emf::game_build()` | `game_build(&EmfGameBuild)` | the build number (0 when EMF does not know the exe), its sha256, `known`, `server` (inside the dedicated server), the exe's base and `.text` range. |
| `emf::has_v14()` | — | true when the core has these. |

## v15: commands from players

| C++ | C | Rules |
|---|---|---|
| `emf::command_sender(&EmfCommandSender)` | `command_sender(&EmfCommandSender)` | inside a command callback: who typed it. `from_chat` 1: a player in the game's chat (a dedicated server or a game you host; see [server.md](server.md)), with `account` (the account hash the game uses) and `entity` (the player in the server world, 0 if EMF could not tell); 0: the console, `emf\commands.txt` or the overlay. False outside commands. Lua: `emf.sender()`. |
| `emf::has_v15()` | — | true when the core has it. |

A command from chat means its sender by "you": the built-in commands (`give`, `pos`, `spawn`...) act on the sender's
entity, never on the host. Do the same: use `entity` when `from_chat` is 1, `local_player()` otherwise. The host's own
`/commands` from their chat box run through the client side and report `from_chat` 0 (the host is the local player).

## v16: writes the players see

| C++ | C | Rules |
|---|---|---|
| `emf::add_program(name, kind, events_in, events_out, singletons, other_read, other_write, f)` | `EmfProgramDef.other_write` | the components your program writes on OTHER entities (short names, per-entity components, at most 16). |
| `emf::write_other<T>(ctx, "Component", entity)` | `program_write_other(ctx, component, entity, size)` | inside the program callback: the entity's component to write into now. The game's own `writeOther` gives it and marks it changed, the call the game's programs use (`admin_player` writes Health and Level this way; for attribute stores such as Health it also recomputes the attributes afterwards, which EMF does not), so the change is meant to reach the players' copies and the save. A component may be in both `other_read` and `other_write`. NULL when the entity has none, the size differs, it is not in `other_write`, or outside a program (logged with the reason). |
| `emf::has_v16()` | — | true when the core has it. |

Tested in a game you host: the write applies and the local client sees it, but there the client also sees writes
through a plain `get_component` pointer, so the difference (players who joined over the network, the save) is not
shown yet. Some components are set again by the game when a world loads (a villager's `Faction` is), whatever you
wrote. `mods/write_test/` is the developer check.

## v17: commands any player may run

| C++ | C | Rules |
|---|---|---|
| `emf::add_command(name, help, f, EMF_COMMAND_PLAYERS)` | `register_command_ex(&EmfCommandDef)` | a command with options: `EmfCommandDef {size, flags, name, help, cb, user}`. With `EMF_COMMAND_PLAYERS` **every player** may run it from the game's chat; without it only the accounts in `[chat] admins` (or everyone, with `[chat] everyone=1`), as for `register_command`. The console, `emf\commands.txt` and the host's own chat run every command. Registering a name again with the same `cb` and `user` updates its help and flags (a script host re-declaring after a reload); any other duplicate is refused. Unknown flags are refused (a newer mod). |
| `emf::has_v17()` | — | true when the core has it. On an older core `add_command` with flags adds the command for the admins only (logged). |

`EmfCommandSender.admin` (v17, the old padding's first byte, so the struct keeps its size): 1 when the sender may run
every command (the console, `emf\commands.txt`, the host's own chat, an account in `[chat] admins` or everyone with
`everyone=1`), 0 for a player who may run only the commands for players. A command for players can branch on it (more
for admins). A core older than v17 leaves it 0: read it only when `emf::has_v17()`. The server owner can take a command
for players away from players who are no admins with `[chat] deny=<names>` ([server.md](server.md)).

Chat commands still need players' chat commands to be on: always on a dedicated server, with `[chat] players=1` in a
game you host ([server.md](server.md)). A player who is no admin is checked twice: when the line arrives and again when
it runs, and may send at most 5 command lines in 10 seconds. Mark a command for players only when it is harmless from anyone: it reads (a status), or it acts on its sender
(`command_sender`), never on the host or the world as a whole. `help` is such a command: for a player who is no admin
it lists only the commands they may run.

## v18: attributes and damage

The game keeps current and maximum health, stamina and mana as **attributes**: slots of the `Health`, `Stamina` and
`Mana` components whose meaning comes from the game's attribute registry (which slot is the maximum differs per
creature). These functions go through that registry and the game's own functions, the way the game does
([re_attributes_menu.md](../engine_parts/re_attributes_menu.md) Part 1). Server world only: a joined client has no
`Health`, only the replicated `NetworkHealth`. Only on game builds whose layouts EMF checked (1273043, game and
server): on an unknown build (after a game update) or 1269579 the three functions are off (logged) and `emf::kill`
takes its old path, so a shifted offset can never turn a hit into something else.

| C++ | C | Rules |
|---|---|---|
| `emf::attribute(w, id, EMF_ATTR_HEALTH)` | `attribute_get(world, entity, attr_id, &out)` | an attribute as the game reads it: `EMF_ATTR_HEALTH` (current health), `EMF_ATTR_MAX_HEALTH` (the maximum, computed by the game), `EMF_ATTR_STAMINA`, `EMF_ATTR_MAX_STAMINA`, `EMF_ATTR_MANA`, `EMF_ATTR_MAX_MANA`, or another id of those components. Nothing / false when the entity has none (a prop, a client world), its component is not set up yet, or the feature is off on this game build (logged once). World callback, like `get_component`. |
| `emf::health(w, id, &cur, &max)` | — | current and maximum health: the attributes in a world you host, else the replicated `NetworkHealth` (a client of someone else's game, an older core). False when neither is there. |
| `emf::attribute_add(w, id, attr, delta)` | `attribute_add(world, entity, attr_id, delta)` | adds `delta` through the game's own `attrAdd`, which also updates the values computed from it; the game's death program and `NetworkHealth` (what players see) pick the change up on the next step: health at 0 dies like any death. **Clamped:** damage stops at 0, a heal of current health, stamina or mana at its maximum; a heal of an attribute whose maximum cannot be read (any other id) is refused. **Not a hit:** no kill credit or XP, no hit reaction, no damage numbers, no killing blow (a destructible prop is not destroyed). Inside a world callback on that world it runs at once; anywhere else (another thread, a mod program) it runs after the server world's next step (true = queued, a later refusal is logged). |
| `emf::damage(w, attacker, target, amount)` | `damage(world, attacker, target, amount)` | `amount` (> 0) of **true damage** (no armour, block, crit or base protection) as a real game hit: EMF queues a pending hit and the game's own `impact_hit_conclusion` applies it on the next step, with a `HitEvent` (damage numbers, **always a light hit reaction**: a parameter for it may come later; aggro toward the attacker), the killing blow, and **kill credit and XP** for the attacker when it is a player (0 = no attacker and no aggro). The target needs `CombatFlags.handleHits` (creatures and players have it) and `Health`; a target with `CombatFlags.immune` is refused. Inside a world callback the target is checked at once (false when refused); elsewhere at the next step (logged). |
| `emf::has_v18()` | — | true when the core has them. |

Notes:
- `emf::kill` (below) goes through `attribute_add` with the most negative delta, which the clamp floors at 0
  wherever the current-health slot is. It falls back to a write of `Health` slot 0 only on a core older than v18 or a game build where the attribute
  signatures were not found.
- EMF's core hooks the game programs `impact_hit_conclusion` and `clear_hit_candidates` for `damage` (and logs once
  that the first runs after the second in the same step: confirmed in game). A mod's `hook_program` on those two names
  is refused with that reason in the log. `damage` is on once the first hook is really installed (at world setup); a
  failed install is logged.
- A queued hit waits for the next `impact_hit_conclusion` of its world; one that waited 180 server steps (the game's
  pending-hit list stayed full) or belongs to an earlier session is dropped with a log line.
- Tested in game (a world you host): the player's health 60/100 read, -10 and +25 applied, +1000 stopped at
  100; a Vukah read at 119/119 (stamina 24, no mana), healed and hurt, killed with `emf::kill`; `damage` from the
  player for 50 then 500 gave two `HitEvent`s with root source = the player and a light reaction, the second with the
  killing blow, the Vukah's `ExperienceSource` = the player, its death, and +50 XP for the player.

## v19: chat lines and saves on a server

Lines in the game's chat from mods, players' chat lines for mods, and a save on demand, for server mods (`mods/herald`,
`mods/handover`). They work where players' chat is on: always on a dedicated server; in a game you host with
`[chat] players=1` in `emf\emf_core.ini` ([server.md](server.md)).

| C++ | C | Rules |
|---|---|---|
| `emf::tell(w, player, text)` | `chat_tell(world, player, text)` | a chat line only `player` (a player entity of the server world) sees, the way EMF's command replies reach a player: their game shows it **under their own name** (`Ember: Welcome!`), because the game shows no line without a speaker. In a game you host, a line for the host becomes a `notify` (the host's own game does not show the server's lines). `'\n'` splits lines: at most `EMF_CHAT_MAX_LINES` (10) per call, the next becomes `...`, the rest are dropped; each line is cut at `EMF_CHAT_LINE_BYTES` (1024) bytes; control characters become `?`. The line reaches the player after the server's next network step. Inside a world callback on `world` it is queued at once; elsewhere after the server world's next step. False (logged) when refused: not a server world, no such player or no account for them, players' chat is off, the limit below, or the server's chat functions were not found on this game build. **Not a format:** pass players' text as it is. |
| `emf::broadcast(text)` | `chat_broadcast(text)` | the same line for every player connected to the server, each one seeing it under their own name (in a game you host: the guests, and a notify for the host; without `[chat] players=1` the host only, still true). Any thread. |
| `emf::player_name(w, player)` | `player_name(world, player, buf, size)` | the player's name, UTF-8: the name their connection carries, the one the game's chat shows as the speaker (their platform name, e.g. their Steam name; **not the character's name**). Nothing / false when it is not known: not a player, no connection record for them, or players' chat is off (the name is read from the server's connection records). Server world, world callback. Say `player #<id>` when it is nothing. |
| `emf::on_chat(mod, f)` | `on_chat(cb, user)` | `f(world, account, entity, text)` for every chat line a player sends to the server that is **not an EMF command** (a `/word` that is no command counts), on the server world's thread after its next step, inside its callbacks (spawn, tell allowed). Read-only: the line is shown as usual. `entity` is 0 when EMF could not tell; `account` is the value `command_sender` reports. At most 256 lines wait for that step (more are dropped, logged). False (logged) where players' chat is off. A callback that faults is switched off (logged). Any thread, any time. |
| `emf::request_save()` | `request_save()` | asks the game to save the world now: the same save its 30 s timer makes, through the game's own `requestServerSave`; `on_save` follows once it is written (the log says `world saved`). Host or dedicated server. Inside a server world callback it asks at once, elsewhere after the server world's next step. The game skips the request while a save is already running or the world is not running. False (logged) when the function was not found on this game build, no save was loaded in this session (EMF knows the game's server from that load), or this game hosts no world. |
| `emf::has_v19()` | — | true when the core has them. |

The limit: a mod (its DLL) may send at most **30 chat lines in 10 seconds** with `chat_tell` and `chat_broadcast` (a
broadcast counts its lines once, however many players there are); a call that does not fit is refused whole (false,
logged). Only lines that were queued or shown count: a refused call (no such player, players' chat off, a full outbox)
uses none. The Lua host sends for every Lua mod, so its DLL (`lua`) may send 120 lines in 10 s, and it holds each Lua mod
to 30 lines in 10 s itself (`emf.tell` / `emf.broadcast` answer false past that). EMF logs every line it sends
(`chat to player 3: ...`, `chat to everyone: ...`) and adds at most 32 lines to the game's chat per network step (the
rest follow a step later: the game's chat keeps a limited number of lines).

How a line finds its player: their `BasicPlayerInfos` entry holds their connection handle at `+0x04` (confirmed in game:
the same 0x40 their chat lines carry) and their account at `+0x08`; the server's connection record for that handle names
their chat reader. When the record holds another connection, the record that holds their account is used; a player who
left gets nothing (logged). The console command `chat.players` lists every player with their account hash (the value
for `[chat] admins`), handle, whether the record matches, and name.

Tested in game: on a dedicated server with a game joined from the same PC, the MOTD and join line (`chat_tell`,
`chat_broadcast` from Server Herald), `herald.say`, a `herald.tell` outside any command and `/players` all showed in the
joined game's chat under the player's name, a plain chat line reached `on_chat` (Herald's chat log), `herald.save`
gave `world saved` within a few ms, and `player_name` gave the name the chat shows. In a game you host the host has a
connection record and a chat reader too, and the server sends it the lines, but an offline session has no text chat
to show them, so the host keeps getting a notify.

**Helpers in `emf.hpp` and `emf/query.hpp` (any core)**:

| C++ | What |
|---|---|
| `emf::time_of_day(w)` | the world's time of day in hours (0 <= h < 24) from `g38_daytime::Daytime`, what the clock mod shows; nothing when the world has none. Lua: `emf.time_of_day()` |
| `emf::kill(w, id)` | kills without a hit: the current-health attribute becomes 0 (v18: through `attribute_add`; on an older core or without the attribute signatures: `Health` slot 0 when it holds `NetworkHealth.health`) and the game's own `death` program sends `EntityDiedEvent` the next step; the corpse keeps its loot. **No attacker, so no kill credit** (use `emf::damage` with more than its health for that). Server world, world callback; false for a player (never), no `Health`, already dead, or (old path) a health that is changing this step. Lua: `emf.kill(world, id)` |
| `emf::players(w)` | every player in a server world (the host and every guest; on a dedicated server all of them): ids 1-1023 with `PlayerState` and a position |
| `emf::player_account(w, id)` | a player's account hash (`BasicPlayerInfos`), the value `command_sender` reports: a key that stays the same across sessions; 0 if unknown |

**Pattern files.** Put your patterns into `<id>.patterns` next to your mod (`name = bytes [; offset=N] [; rel32]
[; tail=N] [; section=.rdata]`, N decimal or `0x` hex, `#` comments) and check them against any exe, without starting
the game:
`emf_sigcheck --patterns mymod.patterns "<game>\enshrouded.exe"` (exit 0 = every pattern found once, 1 = one is not
or a line is bad, 3 = the file or the exe could not be read; [updating.md](updating.md)). The release zip
has the tool in `tools\`; `mods\sim_rate\` is a complete example.

## v20: teleport

Moves a player, a villager, an animal or a creature the way the game's own fast travel does
([re_teleport.md](../engine_parts/re_teleport.md)): EMF's internal server program `emf_teleport` hands the game a
`Teleport` component for the entity, and the game's own teleport program moves it on a later step: it loads the place
around a player, resets fall damage, cancels gliding and climbing, resets NPC behaviour, and every player's game snaps
the entity to the new place (no slide, no rubber-banding). Server world only (the host or a dedicated server).

| C++ (`emf/teleport.hpp`) | C | Rules |
|---|---|---|
| `emf::teleport(w, id, {x, y, z}, opts, f)` | `teleport(world, &EmfTeleport)` | Queues the move and returns true; then `f(World, const TeleportResult&)` / `cb(world, result, user)` runs after the step that moved it (or when it failed), on the server world's thread (components, spawn and destroy allowed). **MOVED**: `x/y/z` is where the entity is now, `distance` how far that is from the target. **FAILED**: `reason` says why (refused when it was checked, or given up). Any thread. |
| `opts.ground` | `EMF_TELEPORT_FIND_GROUND` | the game's ground search: the nearest walkable spot **within about 10 m** of the target (up or down; in game: +4..+8 m and −3..−10 m snapped to the surface, ±16 m and more did not). When it finds none the entity stands **exactly at the target** and the result has `raw_target` / `EMF_TELEPORT_RAW_TARGET`: in the air it falls (a 16 m fall kills a low-level character), inside the terrain the game frees a player by sending it to its spawn point about 1.5 s later. It waits for the destination to load (1-32 steps). |
| `opts.spawn_point = id` | `spawn_point` | an entity with `PlayerSpawnPoint` (a flame altar, a spire, a start location): the game lands the entity on one of its landing pads, as a respawn there does; x/y/z are not used. `distance` is then from the spawn point and `off_target` means more than 25 m from it. |
| `opts.yaw = deg` | `EMF_TELEPORT_FACE_YAW`, `yaw_deg` | turn the body to that yaw (in game: the body turns, the camera does not); without it the current orientation stays. |
| `opts.checkpoint` | `EMF_TELEPORT_CHECKPOINT` | players: also a replication checkpoint, as fast travel sets one (in game: no visible difference; for long moves). |
| `opts.notify_ready` | `EMF_TELEPORT_NOTIFY_READY` | after MOVED a second call with **READY** once the player can play again (the game keeps a player "not ready for gameplay" for 0.3-3 s while it loads the destination; at most 10 s, then READY with a reason); for other entities right after the MOVED. |
| `r.off_target` | `EMF_TELEPORT_OFF_TARGET` | MOVED more than 1 m from the target: the ground search moved it, or the game sent it elsewhere. |
| `r.last` | `EMF_TELEPORT_LAST` | no further call for this request: free what `user` points to (the C++ wrapper does). |
| `r.session_ended` | `EMF_TELEPORT_SESSION_ENDED` | the session ended first (see below): do not touch the world, give nothing back. |
| `emf::player_spawn_point(w, player)` | — | the player's own respawn point: the entity the game keeps in the player's `SelectedTargets` slot 9 (the flame altar, spire or start location whose area the player last stood in; the game's own "emergency teleport" reads it there). 0 when there is none or it is gone. |
| `emf::spawn_points_near(w, pos, radius)` | — | the spawn points around a position, nearest first (`PlayerSpawnPoint` and not "no respawn"); `emf::is_spawn_point(w, id)` |
| `emf::has_v20()` | — | true when the core has it. |

**Refused at once** (false, logged once per kind of problem, no callback): not the server world (a joined client),
entity 0, unknown flags, a non-finite number, x or z outside 0-10240 m or y outside -1000-5000 m, a teleport of that
entity already under way (one at a time per entity, so two requests in one step never reach the game), 64 under way,
or teleport is off on this game build (it needs the `QueuePush` signature and a build whose layouts EMF checked:
1273043, game and dedicated server). Called inside a world callback on that world, the entity is checked at once as
well: it needs `CurrentTransform`, `LastTransform` and `TeleportCount` (players, villagers, animals and enemies have
them; props, chests, altars and spawned barrels do not, and a `Teleport` on them would never go away), and it must not be
dead: its health attribute at 0 (a player in the server world has `Health` but no `NetworkHealth`; `NetworkHealth` 0
counts only where there is no attribute), and a player whose health cannot be read at all is refused (a dead player moved
into the terrain fell through the world). A creature spawned a moment ago reads as dead until the game sets its health, so it is refused for its first moments: try again a second or two later (seen in game by the Beastmaster mod). Called anywhere else, that check comes after the server world's next step and a
refusal reaches the callback as FAILED. A request that waits to be pushed (a full buffer) is checked again every step.

**Waits and gives up.** While a game teleport of the entity is still under way (a respawn, fast travel) or a player is
not ready for gameplay (respawning, loading), the request waits, at most 5 s. When the game has not moved the entity
10 s after EMF's push (its ground search never finished), the request ends FAILED and EMF removes the `Teleport` again,
so a player is never left frozen "not ready". It removes only its own: before the move, and only when the same session's
entity still holds exactly the `Teleport` EMF gave (checked again before every retry); a `Teleport` after the move (a
death, a respawn) is the game's. If the game's `QueuePush` faults, teleport is off until the game restarts; what was
already pushed is still watched, and the log names the entities whose `Teleport` EMF can no longer take back (also
when `emf_teleport` stops running in the world).

**Every request is answered once.** With `EMF_TELEPORT_LAST` on the last answer. When the session ends first (a new
world is set up, or no server world has run for 10 s: back to the menu) the answer comes at once with
`EMF_TELEPORT_SESSION_ENDED`, possibly outside a world callback and on another thread: then touch neither the world nor
entity ids, and give nothing back (the take you would refund may not have been saved); only free your data. A game
process that simply ends answers nothing.

**Timing.** Checked after one step, pushed in the next (inside a world callback: pushed in the next), moved in the
one after: 2-3 steps (33-50 ms) without the ground search.

**NPCs.** The game removes an enemy moved away from every player within a second (in game: 0.6 s after a 150 m move);
keep NPC teleports near a player. Villagers stay and keep their behaviour (one brought 250 m walked about afterwards).

**Saves.** `Teleport` is never saved: a save while a teleport is under way just drops it (the callback then says
FAILED after 10 s, or "a new world was set up" on a reload).

Tested in game. In a game you host (a LOCAL character in a LOCAL world): a 10 m hop moved 2 steps after the command and said READY 0.43 s later; 8 m above the ground with the ground
search it landed on the surface; 1 km away with the ground search and back to the flame altar with `spawn_point` (its
pad); a villager moved and brought back; a dead player refused (EMF reads the health attribute: a player in the server
world has `Health` but no `NetworkHealth`); a player put 3 m inside the terrain was lifted to the
surface by a hop with the ground search, 0.65 s after the game had flagged it, and the game's own emergency teleport,
already under way, then still sent it to its spawn point. On a dedicated server with a joined player: the same pushes
from the server's console and from a player's chat command; the joined game showed the server's position each time.
[re_teleport.md](../engine_parts/re_teleport.md) section 8 has every kind of move, tested in game in both setups.

## Lua

Lua mods use the `emf` table described in [lua.md](lua.md) (run by the `lua` mod, built on this API).

## SDK headers (header-only C++, no ABI)

| Header | What |
|---|---|
| `emf/emf.hpp` | the C++ layer above |
| `emf/generated/components.hpp` | every component and event struct (`emf::gen::ecs`), with `kHash` / `kSize` and layout asserts |
| `emf/generated/reflection.hpp` | every struct's fields by name at run time (`emf::gen::reflect::kTypes`, `kFields`: offset, size, kind, nested type) |
| `emf/aim.hpp` | `emf::aim::camera_ray(w, player)` (the player's camera as a ray in metres), `pick(ray, from, points, limits)` (which point is under the crosshair), vector helpers |
| `emf/keys.hpp` | `emf::keys::parse_key(L"G", &name)` (ini key text: a letter, a digit, a name like `F2`, `Space`, `PageUp`, `Numpad7`, `Shift`, or `0x71`; 0 for mouse buttons; `name` optional), `down(vk)` (only while the game window is in front), `Edge` (press detection) |
| `emf/query.hpp` | `emf::position(w, id)`, `emf::entities_near(w, center, radius, pred)`, `emf::players(w)`, `emf::player_account(w, id)` |
| `emf/inventory.hpp` | `emf::inventory_stacks(w, entity)`, `emf::inventory_contents(w, entity, plain_only)`, `emf::slot_id(inv, slot)` |
| `emf/storage.hpp` | `emf::WorldFile`: a per-world text file for your mod (loads once per session, saves atomically, never into another world's file) |
| `emf/teleport.hpp` | v20: `emf::teleport(w, id, pos, opts, f)`, `emf::teleport_to_spawn_point(w, id, spawn_point, f)`, `emf::player_spawn_point(w, player)`, `emf::spawn_points_near(w, pos, radius)` (see v20 above) |

## Console commands (built in)

`help`, `mods` (every mod, its version and status), `hooks` (every inline hook and its owner), `pattern <bytes> [.section]` (test a pattern live), `settings <mod>`, `set <mod> <key> <value>` (`enabled` = the switch,
from the next start), `profile [save <name> [--settings] [--replace] | load <name>]` (mod profiles: the switches of
every mod under a name, as in EMF.exe; a load applies at the next start), `items [part]`, `give <item> [n]`, `put <item> [n] [entity] [level] [rarity]`,
`take <item> [n] [entity]`, `spawn <guid> [n]`, `time <hours>`, `pos`, `goto [#k] <x> <y> <z> [ground]` / `goto [#k] <entity> [ground]`
(v20: teleports you, or the k-th player, there or beside that entity; the result follows a few steps later), `entities`, `near [radius]`, `inv <entity>`,
`inspect <entity>`, `tpl <entity> <component> [bytes] [offset]`, `tplset <entity> <component> <offset> <type> <value>`,
`stack <item> <max>`, `destroy <entity>`, `chat.players` (v19: the players of the world you host or of the
dedicated server, with their account hashes for `[chat] admins`, their connection and name). Commands go into the in-game console (the key under Esc), the optional
console window (`[console] window=1` in `emf\emf_core.ini`), or `<game>\emf\commands.txt` (one per line; EMF runs the
file about once a second). Replies are `[cmd]` lines in the log.
