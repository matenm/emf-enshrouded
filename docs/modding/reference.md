# EMF API reference

The C API is one table of function pointers, `EmfApi`, declared in `sdk/include/emf/emf_api.h` and passed to your
`emf_mod_load`. It only grows: each version appends functions at the end (`EMF_API_VERSION` 42 today), so a mod built
for an older version works with a newer core. Another framework that runs EMF mods: see [hosting.md](hosting.md). `sdk/include/emf/emf.hpp` wraps it in C++; prefer the wrappers.

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
| `emf::init(api)` | — | Call first in `emf_mod_load`. Returns false (and logs why) when the core is older than the SDK you built with. `emf::init(api, N)` (since 0.6.10): your mod needs only API vN and checks `emf::has_vNN()` before newer calls; on an older core, or another host with a smaller table, it then starts with the newer functions null. |
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
| `emf::api()->spawn(w.raw(), guid, x, y, z)` | `spawn(w, guid, x, y, z)` | spawn a template (16-byte GUID as stored; `emf::parse_guid("xxxxxxxx-...")`) at metres; returns the entity id or 0. Server world, world callback. **Not saved** by the game, except villagers and props (template family Prop) spawned inside a base's build zone, which the game saves like placed objects (seen on build 1285601). |
| — | `destroy(w, id)` | remove an entity (never a player). Same rules as spawn. On a placed object it removes it the way the game removes any object: the same engine call, its attached parts go with it, players see it vanish and the next save no longer has it. It does not give the item back, drop loot, play the pick-up sound or tell the game a player picked it up. A container's contents are deleted with it (the game itself refuses to pick up a non-empty chest). Never destroy a Flame Altar: it is the base, and every object of that base would be lost at the next save. To have a raid break an object the way enemies do (loot, fade-out), use `damage` on an object with `Destructible`. For a pick-up-style removal that gives the item back, use `remove_building` (v36). |
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
| `emf::add_command(name, help, f)` | `register_command(name, help, cb, user)` | `f(World, args)` when typed in the console or `emf\commands.txt` (`args[0]` is the name). Name `mod.command`, 1-32 of `A-Za-z0-9_.-`. Runs on the host's server world after its step. Register any time. Write `help` as `<args>: what it does` (`"[on\|off]: your Loot Magnet"`, `": your position"`): the console shows it as players type, marks the argument being typed, offers the words of `a\|b` choices and item names for an argument named `<item>` or `<item name>`. Words that stand for a value are not offered as choices (`[count\|all]` offers only `all`): count, name, entity, item, number, n, k, m, id, value, text, player, amount, metres, meters, radius, x, y, z, guid, words, word, hours, setting, mod, key, page, label, villager, chest, species, offset, bytes, part; an argument takes item names when its name has the word `item` (not `<itemset>`). |
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
| `emf::hud(id, text, place, rgba)` | `hud_set(id, text, place, rgba)` | keeps a line of text on screen until changed; empty text removes it. `id`: 1-48 of `A-Za-z0-9_.-` (prefix your mod); `place`: `EMF_HUD_TOP_RIGHT` (default), `EMF_HUD_BOTTOM_RIGHT`, `EMF_HUD_BOTTOM_LEFT`; `rgba` 0xRRGGBBAA (0 = default). At most 32 lines in all; hidden while the console is open and while no world runs (main menu, loading); text longer than 160 bytes is cut. The player can move, scale and hide each place in the HUD editor (parts `lines_top_right`, `lines_bottom_right`, `lines_bottom_left`). Any thread. |
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
`Health`, only the replicated `NetworkHealth`. Only on game builds whose layouts EMF checked (1273043, 1283531, 1285601 and 1287369,
game and server): on an unknown build (after a game update) or 1269579 the three functions are off (logged) and `emf::kill`
takes its old path, so a shifted offset can never turn a hit into something else.

| C++ | C | Rules |
|---|---|---|
| `emf::attribute(w, id, EMF_ATTR_HEALTH)` | `attribute_get(world, entity, attr_id, &out)` | an attribute as the game reads it: `EMF_ATTR_HEALTH` (current health), `EMF_ATTR_MAX_HEALTH` (the maximum, computed by the game), `EMF_ATTR_STAMINA`, `EMF_ATTR_MAX_STAMINA`, `EMF_ATTR_MANA`, `EMF_ATTR_MAX_MANA`, or another id of those components. Nothing / false when the entity has none (a prop, a client world), its component is not set up yet, or the feature is off on this game build (logged once). World callback, like `get_component`. |
| `emf::health(w, id, &cur, &max)` | — | current and maximum health: the attributes in a world you host, else the replicated `NetworkHealth` (a client of someone else's game, an older core). False when neither is there. |
| `emf::attribute_add(w, id, attr, delta)` | `attribute_add(world, entity, attr_id, delta)` | adds `delta` through the game's own `attrAdd`, which also updates the values computed from it; the game's death program and `NetworkHealth` (what players see) pick the change up on the next step: health at 0 dies like any death. **Clamped:** damage stops at 0, a heal of current health, stamina or mana at its maximum; a heal of an attribute whose maximum cannot be read (any other id) is refused. **Not a hit:** no kill credit or XP, no hit reaction, no damage numbers, no killing blow (a destructible prop is not destroyed). Inside a world callback on that world it runs at once; anywhere else (another thread, a mod program) it runs after the server world's next step (true = queued, a later refusal is logged). |
| `emf::damage(w, attacker, target, amount)` | `damage(world, attacker, target, amount)` | `amount` (> 0) of **true damage** (no armour, block, crit or base protection) as a real game hit: EMF queues a pending hit and the game's own `impact_hit_conclusion` applies it on the next step, with a `HitEvent` (damage numbers, **always a light hit reaction**: a parameter for it may come later; aggro toward the attacker), the killing blow, and **kill credit and XP** for the attacker when it is a player (0 = no attacker and no aggro). The target needs `Health` and, when it has `CombatFlags`, `handleHits` on (creatures and players have it); a target with `CombatFlags.immune` is refused. **Placed objects** (v31): an entity with `Destructible` (on it or its template) and no `CombatFlags`, or `CombatFlags` with `handleHits` on, is hit too: the game gives a `Destructible` object `Health` on its first hit and removes it at 0; that `Health` is not saved, so a reload heals the object. Inside a world callback the target is checked at once (false when refused); elsewhere at the next step (logged). |
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
| `emf::tell(w, player, text)` | `chat_tell(world, player, text)` | a chat line only `player` (a player entity of the server world) sees: their game shows it **under their own name** (`Ember: Welcome!`); for a line with no name use `chat_tell_ex` with `EMF_CHAT_SYSTEM` (v30), as EMF's command replies do. In a game you host, a line for the host becomes a `notify` (the host's own game does not show the server's lines). `'\n'` splits lines: at most `EMF_CHAT_MAX_LINES` (10) per call, the next becomes `...`, the rest are dropped; each line is cut at `EMF_CHAT_LINE_BYTES` (1024) bytes; control characters become `?`. The line reaches the player after the server's next network step. Inside a world callback on `world` it is queued at once; elsewhere after the server world's next step. False (logged) when refused: not a server world, no such player or no account for them, players' chat is off, the limit below, or the server's chat functions were not found on this game build. **Not a format:** pass players' text as it is. |
| `emf::broadcast(text)` | `chat_broadcast(text)` | the same line for every player connected to the server, each one seeing it under their own name (in a game you host: the guests, and a notify for the host; without `[chat] players=1` the host only, still true). Any thread. |
| `emf::player_name(w, player)` | `player_name(world, player, buf, size)` | the player's name, UTF-8: the name their connection carries, the one the game's chat shows as the speaker (their platform name, e.g. their Steam name; **not the character's name**: that is `player_character_name`, v30). Nothing / false when it is not known: not a player, no connection record for them, or players' chat is off (the name is read from the server's connection records). Server world, world callback. Say `player #<id>` when it is nothing. |
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
1273043, 1283531, 1285601 and 1287369, game and dedicated server). Called inside a world callback on that world, the entity is checked at once as
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

## v21: world settings

The running world's settings (the game's `ecs::GameSettings`: `enshrouded_server.json`'s `"gameSettings"` or the
world's preset), changed while it runs: [world_settings.md](world_settings.md) has the settings, their ranges and the
`world.*` commands. Server world only (the host or a dedicated server); any thread.

| C++ (`emf/emf.hpp`) | C | Rules |
|---|---|---|
| `emf::world_setting(key)` | `world_setting_get(key, &value)` | the value now: factors as they are, durations in minutes, choices by number, on/off as 1/0. Nothing / false for an unknown key or without a hosted world (a joined client, the menu). |
| `emf::set_world_setting(key, value, persist)` | `world_setting_set(key, value, flags)` | queued and written on the next server step; several changes in one step are one write, and the game then sends the settings to every player (at most once a second). False (logged, with the allowed range) for an unknown key, a value outside the setting's range (never cut to fit), other flag bits, or no hosted world. `EMF_WS_PERSIST`: keep it for this world (applied again after each load). |
| `emf::reset_world_setting(key, persist)` | `world_setting_reset(key, flags)` | back to the world's own value (as it loaded); `nullptr` / `NULL` or `"all"`: every setting. With `EMF_WS_PERSIST` the kept value is forgotten too. |
| — | `on_world_setting_changed(cb, user)` | `cb(key, value, user)` once per setting the server step wrote, whoever changed it (a command, a mod, a kept value applied after a load), on the server world's thread; `key` as `enshrouded_server.json` names it. A callback that faults is logged and skipped. False: `cb` NULL or no world settings on this core. |
| — | `world_setting_info(index, &info)` | the settings one by one from index 0 (`EmfWorldSettingInfo`, set `size`): `key`, `alias` (`""` when none), `group`, `type` (`EMF_WS_FLOAT`, `_BOOL`, `_ENUM`, `_MINUTES`, `_WHOLE`), `min`, `max`, `value` (now), `world` (the world's own), `flags` (`EMF_WS_CHANGED`, `EMF_WS_NEEDS_CLIENT`: players' games read it too). False past the last one, without a hosted world, or when `size` is too small. The strings stay valid. |
| `emf::has_v21()` | — | true when the core has them. |

A key is the setting's name as `enshrouded_server.json` has it (`playerStaminaFactor`, any case) or its short name
(`loot`, `day_minutes`). The game never saves a live change into the world; EMF never edits `enshrouded_server.json`.
On a game build where EMF does not find the game's resync flag, the write still happens and the log says so; players'
games may then keep the old values of the settings they read until they reconnect.

## v22: XP and level

A player's XP and level are attributes of the `Experience` and `Level` components, read and given through the v18
attribute calls, and three functions read the game's XP curve and level cap and set a level (check that `api->size`
covers `player_level_set`, or `emf::has_v22()`). Server world only.

| C++ (`emf/emf.hpp`) | C | Rules |
|---|---|---|
| `emf::give_xp(w, player, amount)` | `attribute_add(world, player, EMF_ATTR_XP_PENDING, amount)` | gives `amount` (> 0) XP as every XP source in the game does: as **pending XP**. On its next steps the game's `player_level` program counts it into XP and levels the player up, once per step while XP reaches the requirement, with the rest carried over, a `LevelUpEvent` and the skill points of the new level (they follow the level; nothing else stores them). At the level cap pending XP is thrown away. **Saved with the character**, like XP from play. A negative or zero amount is refused. |
| `emf::attribute(w, id, EMF_ATTR_XP)` | `attribute_get(..., EMF_ATTR_XP, &out)` | XP towards the next level; `EMF_ATTR_XP_REQUIRED` what the next level needs; `EMF_ATTR_LEVEL` the level; `EMF_ATTR_XP_PENDING` what was given and not yet counted. |
| — | `attribute_add(..., EMF_ATTR_XP / _XP_REQUIRED / _LEVEL, ...)` | refused (logged): the game keeps the three consistent, and a change to one alone would break it (a requirement of 0 above level 1 even hands out a free level each step). To raise a level, give the XP it needs: `required - xp - pending` for the next level. |
| `emf::xp_required(w, level)` | `player_xp_required(world, level)` | the XP `level` needs before the next one, from the game's own curve (`EMF_ATTR_XP_REQUIRED` at that level); 0 for a level outside 1..cap, on a client, or on a game build where EMF does not find the game's function. Any thread. |
| `emf::level_cap(w)` | `player_level_cap(world)` | the highest level a player can reach (pending XP at the cap is thrown away); 0 when unknown. |
| `emf::set_level(w, player, level, xp)` | `player_level_set(world, player, level, xp)` | sets the level (1..cap) with `xp` (default 0, cut to below the requirement) towards the next one, the way the game's own admin "set level" does: the level, the XP, the requirement for that level and no pending XP (it would level the player up again), all four together. Raises and lowers; no `LevelUpEvent` (to level up the game's way, `give_xp` the XP the levels need: `required - xp - pending` for the next level, plus `xp_required` of each level after it). The skill points follow the level. Skills already unlocked stay, but at the next load the game checks the tree: when it costs more points than the level gives, the game resets the whole tree itself (the points can be spent again). Saved with the character. Inside a world callback on the server world at once, elsewhere after its next step (true = queued; a later refusal is logged). False (logged) for a level outside 1..cap, a non-player, or without the game's function. |
| `emf::has_v22()` | — | true when the core has them. |

## v23: add and remove components

A mod adds a component to an entity, or removes one, through the engine's own "add components" and "remove
components" commands (the ones the game's programs use; EMF's teleport has used the same path since v20). Only the
components EMF lists can be changed, and in this version only the two the world save does not keep:

| Component | Add | Remove | Saved with the world |
|---|---|---|---|
| `ScaleComponent` (`uniformScale` from 0.1 to 10: a size, 1 = as made; see below) | yes | what the mod added | no |
| `InteractionPause` | yes | what the mod added | no |
| `IsTame`, `LifeTime`, `DoNotSave`, `ForceSave` (add, remove); `ConditionalDespawn`, `DespawnLink` (remove) | refused for now | refused for now | **yes** |

The saved ones are listed but refused ("not checked in game yet") until their effect on the world save is checked in
game. Once they are open, such a change outlives the mod in that world and needs `EMF_COMPONENT_PERSISTENT`.

| C++ (`emf/emf.hpp`) | C | Rules |
|---|---|---|
| `emf::add_component(w, id, emf::gen::ecs::ScaleComponent{1.25f}, flags[, f])` | `change_component(world, &change)` with `op = EMF_COMPONENT_ADD`, `hash`, `data`, `data_size` | adds the component with that data (C: `data` NULL for zeros; otherwise `data_size` must be the component's size in this game build, `X::kSize`, also when `data` is given with `data_size` 0). A `ScaleComponent` needs a finite `uniformScale` from 0.1 to 10 (zeros are refused). Only when absent: a component already there is never overwritten (answer `EMF_COMPONENT_ALREADY`). SERVER world, never a player. True = queued: checked after the server world's next step, applied in the step after; `f(World, status, why)` (C: `cb(world, entity, hash, status, why, user)`) answers once on the server world's thread: `EMF_COMPONENT_DONE`, `_ALREADY` or `_FAILED` (the entity is gone, the game did not apply it, the world ended...). When the world ended first (a new world, the menu) the answer is `_FAILED` with an empty `World` (C: a NULL world; the old one may be freed). A callback that faults (C) or throws (C++) is caught and logged. False (logged) when refused at once: not listed or not open yet, the wrong size or value, a client world, a player, a change of that component on that entity under way, 256 under way; the callback is not called then. |
| `emf::remove_component<emf::gen::ecs::ScaleComponent>(w, id, flags[, f])` | `change_component` with `op = EMF_COMPONENT_REMOVE` | removes it (absent: `_ALREADY`). Allowed for the components listed, and for any a mod added itself in this session. |
| `emf::has_v23()` | — | true when the core has it. |

The game owns what a component does. Checked in game (build 1283531): a `ScaleComponent` added to a creature that
already exists reaches the server and the client world, but the creature is still drawn at its old size: the game
reads the size when it makes the creature. For a bigger or smaller creature, spawn it with the component:
`spawn_ex(world, guid, x, y, z, &init, 1)` with `{ScaleComponent::kHash, 4, &scale}` (Lua: `emf.spawn(w, guid, x, y, z,
{ScaleComponent = {uniformScale = 1.25}})`). `EMF_COMPONENT_EXPERT` is reserved.

## v24: mod windows

A mod opens a window that EMF's overlay draws in the console's look: a title, then widgets top to bottom (side by side
where asked). Clicks come back to the mod after the next step of the world it asked for, as a wheel's pick does. One
window at a time takes the mouse and keyboard; Esc, its close key or its X close it. It may open while the console is
open: it shows dim under the console and gets the input when the console closes. A wheel and a window exclude each
other. The dedicated server has no overlay: there `window_open` returns false (keep chat commands for players).

| Widget (`EmfUiWidget.type`) | Looks like | Events |
|---|---|---|
| `EMF_UI_TEXT` | a line of text, wrapped at the window's width | none |
| `EMF_UI_HEADING` | a heading in the bold font | none |
| `EMF_UI_BUTTON` | a button | click, right click (with a key) |
| `EMF_UI_ROW` | a full-width list row; `"Name (detail)"` shows the detail dim at the right | click, right click |
| `EMF_UI_SLOT` | an item-like square about 3 lines high: its text, `count` in the corner | click, right click |
| `EMF_UI_BAR` | a bar filled to `value`/1000 with its text on it | none |
| `EMF_UI_SEPARATOR` | a line | none |

Flags: `EMF_UI_SAME_LINE` (on the line of the widget before), `_DISABLED` (dim, no events), `_SELECTED` (lit), `_DIM`,
`_EMPTY` and `_LOCKED` (a slot's looks). `rgba` colours a widget's text; `tooltip` shows while the pointer rests on it.
Keys (1-48 bytes) name what was clicked even after the mod changed the list; a widget without a key sends nothing.

| C++ (`emf/emf.hpp`) | C | Rules |
|---|---|---|
| `emf::ui::Window w("mymod.stable", "Beasts"); w.heading("Active").slot("active.1", "Wolf").same_line().button("call", "Call"); w.open(EMF_WORLD_SERVER, f)` | `window_open(&def)` with an `EmfWindowDef` (`id`, `title`, `widgets`, `count`, `widget_size = sizeof(EmfUiWidget)`, `close_key`, `place`, `width`, `world_kind`, `cb`, `user`) | opens it (copied, strings too). The id is 1-48 of `A-Z a-z 0-9 _ . -`; start it with your mod's id and a dot. The open window's id replaces it (its callback gets CLOSED, BY_MOD). False (logged) while another window or a wheel is open, with no world of the asked kind running (a player in someone else's game has no server world: pass `EMF_WORLD_CLIENT` there), without the overlay, or for a bad definition (up to 256 widgets, a title up to 64 bytes, a text up to 256, a tooltip up to 512). `f(World, const emf::ui::Event&)` (C: `cb(world, event, user)`) gets `EMF_UI_CLICK` / `_RIGHT_CLICK` with the widget's `key`, and last `EMF_UI_CLOSED` with the `reason`: `EMF_UI_BY_PLAYER` (Esc, the close key, the X), `_BY_MOD` (`window_close`, a replacement), `_BY_CORE` (the world ended, the overlay turned off). With no world running CLOSED comes at once with `EMF_UI_NO_WORLD` and an empty `World` (C: NULL): only free your data. A callback that faults or throws is caught and logged. |
| `w.clear(); ...; w.update()` | `window_update(&def)` | new contents for the open window `def->id` (its callback stays). False when it is not open (the player closed it): it never reopens a window. |
| `emf::ui::close("mymod.stable")` | `window_close(id)` | closes it (CLOSED, BY_MOD). False when it is not open. |
| `emf::has_v24()` | -- | true when the core has it. |

`place`: `EMF_UI_PLACE_CENTRE` (default), `_LEFT`, `_RIGHT` (beside the game's open inventory). `width`: in text
heights (the console's text size scales it), 0 = about 28. `close_key`: a keyboard key's generic virtual-key code (the
key that opened it), acting on a press; 0 = Esc only. Console commands for tests (the debug bridge cannot click EMF's
ImGui): `ui.windows` lists the open window and its widgets with their keys, `ui.click <key> [right]` sends the same
event as a click, `ui.close` closes it as Esc would. The debug bridge's state has `overlay.window`, the open window's
id. Players in someone else's game see only windows from mods running in their own game.

## v25: E prompts

The game's own "E <verb>" prompt on an entity your mod spawns or names, and a callback when any player (host, joined,
dedicated server) presses E there. An offer is three components the game adds to its own objects at run time; none is
saved, so an uninstalled mod leaves plain entities. Seen in game on build 1285601 ("E Open" on a spawned statue,
  "E Rest" on one that existed).

```c
EmfOffer o = {sizeof o};
o.verb = EMF_VERB_OPEN;  // the game's word: every player sees it
uint32_t id = api->spawn_offer(w, guid, x, y, z, &o, on_e, NULL);  // or api->offer(w, entity, &o, on_e, NULL)
void on_e(EmfWorld* w, uint32_t player, uint32_t entity, void* user) { /* server world thread */ }
```

- `verb`: `EMF_VERB_USE` (default), `OPEN`, `TALK`, `READ`, `SIT`, `SLEEP`, `PET`, `TOUCH`, `ACTIVATE`, `PRESS`,
  `ENTER`, `INSPECT`, `TAKE`, `COLLECT`, `REST`, `RELEASE`, or any loca id (Open, Rest and Talk seen in game). `text`:
  your own words (1-63 bytes). The game shows them once a content mod adds them to its text table (the helper's
  `add_prompt_words`, content.md); without that, the prompt shows "E" with no word (it works).
- `sequence`: `EMF_SEQUENCE_TOUCH` (default, no screen) or `EMF_SEQUENCE_BUTTON`. `range_m` (0 = 32), `flags`:
  `EMF_OFFER_LOW_PRIORITY`, `EMF_OFFER_BUILD_MODE`.
- From a world callback on the server world. `offer` refuses a player, an entity with the game's own offer (a chest, a
  bench, a bed) or another mod's. The game picks one target near the crosshair.
- The entity needs a collider (the game's target choice tests it): `offer` accepts one without, but no prompt shows.
  Refused also: your own offer there with other options (`offer_remove` it first; an identical one only replaces the
  callback).
- EMF drops your offer, without telling you, when the entity is gone or its offer is no longer EMF's (streaming,
  destroyed, the game changed it); it never puts one back by itself, since the id may be another entity's by then.
  `offer_active(world, entity)` (v26) says whether yours is still in EMF's hands. Offering again after a drop takes
  your own offer back when its parts are still EMF's (no "the game's own offer" refusal). Offer again when your mod finds the
  entity (a stable search, a scan). A new world forgets all; a spawned prop inside a base stays in the save without its
  offer and comes back with a new id: find it, offer again, don't spawn another. `offer_remove` takes yours off.
  Console: `offers.list`.

## v26: pictures in windows

Pictures in your windows (v24): a still one, or a turning one (frames at an fps), as a widget of their own, inside a
slot, or before a row's text. EMF decodes the PNG files when you load them and draws them with the overlay.

```c
uint32_t wolf = api->image_load("pics/wolf.png");          // a file in your mod's folder (the folder of your DLL)
uint32_t turn = api->image_load("pics/wolf_turn");         // a folder: its .png files, sorted by name, are the frames
uint32_t cache = api->image_load("portrait:wolf");         // EMF's portrait cache: <game>/emf/cache/portraits/wolf/
EmfUiWidget pic = {0};
pic.type = EMF_UI_IMAGE;  pic.icon = turn;  pic.fps = 12;  pic.height = 8;  pic.key = "portrait";  // clickable
EmfUiWidget slot = {0};
slot.type = EMF_UI_SLOT;  slot.icon = wolf;  slot.text = "Wolf";  slot.key = "active.1";            // text: a caption
```

- `image_load` takes a .png, or a folder of .png frames that are all the same size: at most 512 x 512 pixels, 64
  frames. A path is relative to your mod's folder, absolute, or `portrait:<key>` (key: A-Z a-z 0-9 _ . -). The same
  path again gives the same handle; 0 when refused (logged once per path) and on a dedicated server (no windows).
  Pictures stay until the game closes: at most 128, 64 MB of pixels in all. Any thread.
- `EMF_UI_IMAGE`: `height` text lines high (0 = 6, at most 40), as wide as its frames and at most the window.
  `EmfUiWidget.icon` on a slot draws the picture inside the square (its text becomes a caption over the bottom), on a
  row a square one line high before the text. `fps` 0..60 turns a picture of several frames (0: its first frame).
- `EmfUiWidget` grew to 64 bytes (`fps`, `height` appended): pass `widget_size = sizeof(EmfUiWidget)`. A mod built
  against v24 or v25 keeps working (its widgets read `fps` and `height` as 0; `icon` was reserved and 0).
- A picture shows from the frame after the overlay put it on the GPU; until then the widget keeps its place empty.
- PNGs are sRGB. On an HDR swapchain EMF converts them for you (HDR10: BT.2020, PQ, white at 203 nits; scRGB:
  linear) and again when the game switches HDR on or off.
- Renders of the game's own creatures or items are the game's art: don't ship them. The portrait cache is written on
  the player's PC from their own game files.
- A mod asks for pictures in its mod.json: `"portraits": [{"key": "wolf", "template": "<template GUID>" | "model":
  "<RenderModel GUID>", "frames": 24, "size": 256}]` (frames 1-64, size 8-512). The loader draws them at game start (never on
  the dedicated server, never in Play without EMF mods), at most 32 per start; a start that draws holds the game's entry
  and shows a splash. It asks again for a key whose picture is missing, whose entry in mod.json changed, or when the
  game's exe or data, `emf_content.dll` or the EML content mods changed since (the DLL answers "current" without drawing when what the
  picture is drawn from is unchanged). A key that failed is not tried again at every start: the loader remembers each
  attempt in `<game>/emf/cache/portraits_attempts.json` and retries only after `emf_content.dll`, the game, the EML content mods (`<game>/mods`: a creature pack installed or
  changed since) or the entry changed. A key is written to that file as failed before each call, so a crash or hang while drawing is remembered the
  same way. Drawing has its own time limit (2 minutes and 5 s per picture), and never touches the EML content build's
  state. `[content] mode=off` in `emf_core.ini` also stops this. Keys that differ only in case are one key (the first
  mod's stays). `<game>/emf/portraits.log` has the
  last run.

## v27: mod messages

Your mod on a player's game and the same mod on the server send each other messages on named channels. In the host's
own game and in an offline game (the server world and the player's game in one process) they are delivered in process.
Joined players and the dedicated server use the game's chat as the transport (below). Checked live on build 1287369:
a game joined a dedicated server on the same PC; the greeting, text, 3000 bytes and all 256 byte values went both
ways (answers in about 30 ms) and a send to every player arrived, with nothing shown in the chat.

```c
EmfNetChannel c = {sizeof c};
c.name = "mymod/pick";  c.version = 1;  c.flags = EMF_NET_TO_SERVER;
c.on_server = on_pick;  // void on_pick(EmfWorld* w, const EmfNetFrom* from, const void* data, uint32_t size, void* u)
api->net_channel(&c);
api->net_to_server("mymod/pick", "3", 1);                    // a player's game -> the server
api->net_to_player(w, from->player, "mymod/show", "ok", 2);  // the server -> that player's game
```

- A channel is `<mod id>/<name>` (your DLL's file name, then 1-32 of a-z 0-9 _ . -), a version (1..65535), its
  directions, a size limit (default 4096 bytes, at most 16384) and a rate (messages one sender may send in 10 s,
  default 100). Register it once, from `emf_mod_load`; the core keeps it until the game closes.
- Handlers run on the world's thread after its step: `on_server` on the server world (components, spawn), `on_player`
  on the player's client world. `from` names the sender: the player entity and account on the server (from the
  server's own records, never from the message), `local` in process, `admin` for the host's own game.
- Refusals (a bad channel, too large, over the rate, the wrong direction) are false and logged once per reason; a
  player whose game has no such channel is false without a log.

- A message to or from a peer is sent only when the other side has the same channel at the same version (`net_peer`
  says which version a player's game has, 0 for none); nothing is delivered across versions.
- Order and delivery: in order per peer and direction, at most once, very likely delivered while both stay
  connected. A message lost under heavy load or on a disconnect is not resent: a mod that needs certainty sends an
  acknowledgement, or sends its state again when a player's game says hello (`net_peer` turns non-zero).

### The chat transport (joined players, the dedicated server)

- **Lines.** A message becomes up to 24 marked chat lines (`#!emf1 <type> <seq> <channel>@<ver> <frag>/<count> <enc>
  <payload>`, at most 1024 bytes each, text or base64url). All lines of one message have the same type, channel,
  version, count and encoding, and their `<frag>` counts up from 0: a line that differs from the one before it drops
  the message, and the next message that starts with fragment 0 is read again. The server's end takes a player's
  marked line in the receive loop before the game adds it (so no one sees it, and it never reaches commands, `on_chat`
  or the log); the player's end takes the server's marked lines before the chat shows them. The sender of a message is
  the server's own connection record (handle and account), never the message.
- **Discovery.** Nothing is sent to a game that did not prove it runs EMF. A server that has at least one mod channel (or
  only the core's own `emf/mods`, when it has mods its players must match: server.md "Players' mods") sends
  each new connection a beacon line (spoken as that player, kind 0) 10 s after it appears, and once more 35 s later
  if that game has not said hello (at most 2 times; a line that arrives while the game still loads can be lost); an EMF
  game hides it and answers with a hello (its EMF version, API, game build and channels); the server answers with a
  welcome (its channels). A game without EMF shows the beacon line, at most twice: its marker and fields, then "This
  server runs EMF mods. Get EMF to see their windows; you can play without it."; `[net] greet=0` in `emf\emf_core.ini` turns the beacon
  off (and with it the whole transport for that server). A server without EMF sends no beacon, so an EMF game sends
  nothing to it; a marked line another player types is dropped (the line must be spoken as the player itself: the
  game knows its own account from its world's player list; a beacon that comes before it does, for example while the
  game still loads, is answered once it does, when it was spoken as this player). Each new
  session (another world, another server) starts the conversation over: nothing from the last server's welcome or
  queue goes to the next one.
- **Limits.** A player's game: 20 lines a second (burst 40) and 16 KiB a second; a flood of ten times that gets the
  account ignored for a minute. Per channel and sender: `per_10s`. The server holds a player's mod lines back while
  that player's chat reader has more than 16 KiB waiting (at most 256 lines wait per player; one that waited 30 s is
  dropped), and adds at most 32 lines per receive-loop run (shared with command replies). A message goes into the
  outbox whole or not at all, and only to the connection that said hello (after a reconnect, the new one). A game
  sends at most 4 lines a frame (up to 64 wait). The receiving end drops a message whose next line takes more than
  60 s to arrive (twice the 30 s a line may wait in the server's outbox, so a message the server still sent is not
  dropped for its lag).
- **Settings** (`emf\emf_core.ini`): `[net] enabled=0` leaves the chat hooks out (messages stay in process),
  `greet=0` as above. On a game that hosts, the hooks are installed even without `[chat] players=1` (only the
  command policy stays behind that switch). `mods_check`, `mods_required` and `mods_info` are the mod check on join's
  (warn or refuse; [server.md](server.md) "Players' mods").
- Works with text chat turned off in the server's settings (the game's own send call is used, which skips the UI's
  gate). Needs two engine functions (`ChatClientSend`, `ClientOutboxFlush`) that `emf_sigcheck` finds on the known
  builds; where they are missing a joined game cannot send, the server end and the loopback still work.

## v28: player events

Who joins, leaves or dies on the server world (the host's game or a dedicated server), once for every mod, instead of
each mod comparing the player list itself.

```c
static void on_player(EmfWorld* w, const EmfPlayerEvent* e, void* user) {
    if (e->kind == EMF_PLAYER_JOINED && !e->host) api->chat_tell(w, e->player, "Welcome!");
}
api->on_player(on_player, NULL);  /* in emf_mod_load, for "died" too */
```

| C++ (`emf.hpp`) | C | What |
|---|---|---|
| `emf::on_player(mod, f)` | `on_player(cb, user)` | `cb(world, event, user)` for each event on the server world's thread, after its step. False for a null cb. A callback that faults is switched off and named in `emf_core.log`. |

`EmfPlayerEvent`: `size`, `kind`, `account` (as `player_account` and the command sender give it), `player` (the entity
in the server world; for left, the one they had), `killer` (died only), `host` (1 = the host's own player), `name` (the
chat name, "" where players' chat is off).

- **`EMF_PLAYER_JOINED`**: the player is in the world (PlayerState and a position), their account is known and their
  game is ready to play (`PlayerInput.fromClientIsReadyForGameplay`; the host's own player at once). Once per
  arrival. Nothing comes before a world's save has loaded: 1 s after the first step that follows the load event (the
  mods' world data first) everyone there joins, the host first, then by entity id; a new world starts over without
  "left" events. A mod that starts watching later (or a Lua script reloaded) is told about the players already there,
  as joined, at its first step.
- **`EMF_PLAYER_LEFT`**: that account's entity is gone. A player back as another entity: left, then joined. None when
  the world itself ends.
- **`EMF_PLAYER_DIED`**: a joined player died (the game's `EntityDiedEvent`); `killer` is the root source of the last
  hit on them within 10 s (`HitEvent`, the step's killing hit included; not themselves), else 0. The core asks the event relay for both events at the
  first `on_player`, which therefore has to come before the first world starts (in `emf_mod_load`, or a Lua script's
  first run); a later first call gets joined and left only (logged).
- The player list is read at most 4 times a second and only while a mod watches.

## v29: item names and the game's language

The names the game shows for items ("Wood Logs", "Holzscheite" in German), from the game's own text tables, in the
language the player plays in. Only the game exe has text: on the dedicated server both answer nothing, so use the debug
name there (and send item ids over mod messages for each player's game to name in its own language).

```c
char name[128];
if (!api->item_name(w, item_id, 0, name, sizeof name)) { /* no name here: use the debug name (item_info) */ }
```

| C++ (`emf.hpp`) | C | What |
|---|---|---|
| `emf::shown_item_name(w, item[, game_text])` | `item_name(world, item_id, flags, buf, size)` | the item's name, UTF-8 with the game's markup removed; the full length, 0 when there is none (the dedicated server, before the game's text is known, an item without a name, an unknown item). flags 0: for EMF's screens (windows, wheels, HUD lines, the console): the English name where they cannot draw it (Japanese, Korean, Chinese and Thai until EMF found a font for them: Windows' own or the player's Noto file, README_EMF.md; any name with a character EMF's fonts lack); `EMF_ITEM_NAME_GAME_TEXT`: the player's language always, for the game's own chat. Any thread but the render thread. |
| `emf::game_language()` | `game_language(code_out, size)` | the `keen::LanguageId` the game shows text in (1 En_Us, 3 De_De, 11 Ja_Jp, ...; a language without a table reports En_Us), 0 on the dedicated server; `code_out` gets `"en_us"`, `"de_de"`, ... |

EMF copies the text at once and checks that the game's table did not change during the copy (a language change in the
settings frees the old one), so a name is never half old, half new; the names are cached per language.

## v30: character names and chat lines without a speaker

| C | What |
|---|---|
| `player_character_name(world, player, buf, size)` | the player's character name (the one the game's Social tab shows; `player_name` is their platform name), UTF-8, cut to size-1 and NUL-terminated. It may already be there at `EMF_PLAYER_JOINED` (from the character's last visit to this world) or come a few seconds later when the player's game sends it, and it can change during a session: read it when you need it, never cache it. It is text a player's game set, not an identity: key data by the account. A player's grave carries the name too. False when it is not known, for an entity without one, on a client world. Server world (a game you host or the dedicated server); players' chat need not be on. C++: `emf::character_name(w, player)`. |
| `chat_tell_ex(world, player, text, flags)` / `chat_broadcast_ex(text, flags)` | (C++: `emf::tell(w, player, text, true)` / `emf::broadcast(text, true)`) `chat_tell` / `chat_broadcast` with flags. `EMF_CHAT_SYSTEM`: the line shows with no speaker's name, the way the game shows a player joining, still only to that player, and also on a server with text chat off. A label is only possible as a prefix in the text (`"[Raid] The gate is under attack"`). Players without EMF see the same. 0: as `chat_tell`. Other bits: refused (logged). |

EMF's own command replies to players and the mods-check refusal are such lines now (`[EMF] ...` with no name in front).

## v31: hits

The server world applies every hit through the game's `applyHitToEntity`. Mods see each landing hit after it was applied
(`on_hit`) and change hits before they land (`on_hit_before`, rules). Server world only (a game you host or the
dedicated server); a joined player's game sees no hits and needs nothing.

| C | C++ | What |
|---|---|---|
| `on_hit(cb, user)` | `api()->on_hit(cb, user)` | `cb(world, const EmfHit*, user)` after the server world's step, on its thread, once per hit the server applied to an entity, from the game's `HitEvent`: the final numbers, `EMF_HIT_KILLED` on the killing blow, `health_after` = the target's health then (`health_before` is -1). Not for terrain or building hits. Works on any game build where `on_event` works. False for a null `cb` or 16 callbacks already (logged). |
| `on_hit_before(cb, user)` | `api()->on_hit_before(cb, user)` | `cb(world, EmfHit*, user)` inline for each landing entity hit, after the rules and before the game applies it: change `amount`, `stamina`, `reaction` or set `cancel`. It runs on the game's impact thread inside the world's step, so: **be fast**, change only the hit, read only the hit's entities (their `Health`, `Stamina`, `Mana`), no spawn, kill or component writes (`damage` only queues). Once per hit, also for a target of several parts. A callback that faults is switched off. False when the hook is not usable on this game build, for a null `cb` or 16 already (logged). |
| `hit_rule_add(&rule)` | `emf::add_hit_rule(r)` | adds a rule the core applies to every landing entity hit before the `on_hit_before` callbacks, in id order, without calling the mod. Its id, or 0 when refused (logged: a bad rule, 64 rules already, the hook not usable on this game build). Any thread; it counts from the next hit. `emf::hit_rule()` returns a rule with the defaults (`size` set, `scale` 1). |
| `hit_rule_remove(id)` | `emf::remove_hit_rule(id)` | removes a rule. False: no rule with that id. |
| | `emf::has_v31()` | true when the core has these functions. |

`EmfHit` (`size` = `sizeof(EmfHit)`): `attacker` (the root source: the shooter of an arrow, the caster of a spell; 0 =
none), `source` (the direct source), `target` (the actor hit), `part` (the entity hit; the same for most hits), `amount`
(health damage: > 0 damage, < 0 healing), `stamina` (stamina damage of a blocked hit), `flags` (`EMF_HIT_*`),
`damage_types` (bit per type: 0 cut, 1 pierce, 2 blunt, 3 fire, 4 ice, 5 shroud, 6 lightning, 8 poison, 9 wood, 10
stone, 11 metal, 12 explosion), `attacks` (the attack's kinds, a bit each: 1 melee, 2 ranged, 4 magic, 8 tool, 16 other, 32 area, 64 skill shot; most
hits have one, a `damage` hit none), `kind`
(`EMF_HIT_KIND_*`: 0 an entity hit, 1 fall, 2 shroud, 3 cold, 4 drowning: see "Fall, shroud, cold and drowning" below), `weapon_category` (the game's hash, 0 = none), `weapon_item` (0 = none or not
known), `program` (the attack's impact program), `impact` (the swing or projectile: hits of one share it), `reaction`
(the game's hit reaction type: 0 none, 2 light), `target_faction`, `from_mod` (1 = a `damage` hit), `health_before` and
`health_after` (-1 = not known; before the hit `health_after` is what it will be as the hit stands), `position`
(metres), `cancel` (before the hit: 1 drops it silently). `amount`, `stamina`, `reaction` and `cancel` are the fields
`on_hit_before` may change. A blocked or parried hit changes stamina, not health: its `amount` is 0.

Flags: `EMF_HIT_BLOCKED` 0x1, `EMF_HIT_PARRIED` 0x2, `EMF_HIT_CRIT` 0x8, `EMF_HIT_BACKSTAB` 0x20, `EMF_HIT_WEAKSPOT` 0x40,
`EMF_HIT_KILLED` 0x2000 (after the hit only; also on a fall, shroud, cold or drowning hit that left no health), `EMF_HIT_HEALING` 0x4000, `EMF_HIT_INSTANT` 0x01000000 (kinds 1 to 4 only: see below). `EMF_HIT_TERRAIN` and `EMF_HIT_BUILDING` are
the game's terrain and building hits: never passed to mods.

`EmfHitRule` (`size`, then): `attacker` and `target` (0 = anyone, an entity id, or a class: `EMF_HIT_PLAYERS`,
`EMF_HIT_CREATURES` (not a player, has health), `EMF_HIT_OBJECTS` (a placed object with `Destructible`); an entity id
as `target` matches the actor hit or the part hit), `flags_all` (the hit has all these flags: `EMF_HIT_CRIT` = crits
only), `damage_types_any` (0 = any), `attacks` (any of these `EmfHit.attacks` bits; 0 = any), `healing` (0 = damage hits
only, 1 = healing hits only: a "PvP off" rule does not stop a friend's heal), then the effect: `cancel`, `scale`
(`amount` and a blocked hit's `stamina` times `scale`), `add` (then `amount += add`), `min_health` (> 0: the target
keeps at least that much health, only when its health is known), and `kinds` (a bit per kind, `1 << EMF_HIT_KIND_*`;
`EMF_HIT_KINDS_ALL` 0x1F; 0 = entity hits only, so a rule made before v31 got kinds keeps its meaning). Rules apply in id order: a `cancel` wins; for every
matching rule `amount = trunc(amount * scale) + add`, but a damage hit never becomes a heal and a heal never becomes
damage; then the largest `min_health` caps the damage. A cancelled hit: no damage, no damage number, no reaction, no
aggro, no XP credit, no destruction.

EMF detours the game's `applyHitToEntity` (checked on game build 1287369, game and dedicated server). When it cannot be
used on a build, `hit_rule_add` and `on_hit_before` are refused (logged once) and `on_hit` still works. The console
command `hits` shows the state ([Console commands](#console-commands-built-in)).

### Fall, shroud, cold and drowning

These four damage kinds are not game hits (the game writes `Health` directly), but they go through the same rules and
callbacks. `EmfHit.kind`: `EMF_HIT_KIND_FALL` 1 (once per landing), `EMF_HIT_KIND_SHROUD` 2 (every server step, about 60
a second, while the shroud timer is out), `EMF_HIT_KIND_COLD` 3 (every step while body heat is 0 and the entity is
freezing), `EMF_HIT_KIND_DROWNING` 4 (every step while oxygen is 0). For these kinds `attacker` and `source` are 0,
`target` = `part` = the entity, `flags` is 0 or `EMF_HIT_INSTANT`, there are no `damage_types` or `attacks`, and
`health_before` and `health_after` are known.

- `EMF_HIT_INSTANT` 0x01000000: the game set health to 0 at once (deadly shroud, a creature out of air). EMF sets it
  from the game's own signals: shroud while the entity stands in deadly shroud (its `EnvironmentSensor` fog type 3),
  drowning for a creature (only players drown tick by tick).
- A rule has `kinds`; without it a rule is for entity hits only. The rule's other fields match as before, but an
  environment hit has no attacker, flags, types or attack kind, so a rule that asks for them does not match it: `attacker`
  = `EMF_HIT_PLAYERS` never matches a fall.
- `scale` on shroud, cold and drowning is exact: EMF carries the fraction per entity and kind, so 0.5 halves a 1-point
  tick instead of rounding it to 0. `add` counts per event, which for these three is about 60 times a second.
  `min_health` works on every kind (a lethal fall leaves the health it names).
- `on_hit_before` and `on_hit` get kinds 1 to 4 too (a C mod checks `hit->kind`). A cancelled hit, of any kind, never
  reaches `on_hit`. A cancelled fall gives the health back, but the game's hard-landing stagger still plays.
- Server world only. Works on game build 1287369 (game and dedicated server). The console command `hits` says whether fall,
  shroud, cold and drowning are on.

What never becomes a hit, so no hook sees it: regeneration, direct attribute writes (`attribute_add`), and voxel damage
(terrain and player walls).

## v32: buffs

The game's own buffs and debuffs: potions, food, Wet, Poisoned, auras, terrain debuffs. A mod can list the buff types,
read an entity's active buffs, give a buff, remove a buff, and hear the players' buff changes.

- Listing and reading work on any world, server or client (buffs are replicated).
- Giving and removing happen on the **server world only** (the host's game or the dedicated server). EMF queues the
  request for its own server program `emf_buffs`, which calls the game's own buff code in the next step. So every game
  rule runs: slot groups, giving again (timer starts again, replace, or refused), faction exclusions, the food time
  factor, effects, the HUD icon, saving. The result comes after that step: applied, or refused with a reason.
- Removing is the game's own cleanse: the buff's effects stop and it is gone after the next step.
- A joined player's game cannot give or remove (it has no server world). Players without EMF see the buffs EMF gives:
  the game replicates them.

| C | C++ | What |
|---|---|---|
| `buff_types(world, out, cap)` | `api()->buff_types(w, out, cap)` | the game's buff types in `world`: the number there is; fills `out` up to `cap` (`out` may be NULL to count). 0 when the world's buff registry cannot be read. Where no program runs (`on_tick`, commands). |
| `buff_type(world, id, &out)` | `api()->buff_type(w, id, &out)` | one type; false: no such type. |
| `buff_text(id, which, buf, cap)` | `api()->buff_text(id, which, buf, cap)` | a type's name (`which` 0) or description (1) in the player's language, NUL-terminated (cut to fit). A game client only: false on the dedicated server, for no text or a 0 tag. Not on the render thread. |
| `buffs_of(world, entity, out, cap)` | `api()->buffs_of(w, entity, out, cap)` | an entity's active buffs on any world, in slot order: the number there is; fills `out` up to `cap`. Where no program runs. |
| `buff_apply(world, target, type, duration_ns, source, item, done, user)` | `api()->buff_apply(...)` | gives `target` a buff on the server world. `duration_ns` 0 = the type's default; `source` 0 = the target; `item` an item type id (`find_item`, `item_info`), 0 = none; an unknown item id is refused. `done` (may be NULL) is called after the step, on the server world's thread: `EMF_BUFF_APPLIED` or `EMF_BUFF_REFUSED` with `why`. False when refused at once (logged): not the server world, an unknown type, 64 requests waiting, not available on this game build. |
| `buff_remove(world, target, type, done, user)` | `api()->buff_remove(...)` | removes `target`'s buff of that type the game's way. `done` as for `buff_apply`: `EMF_BUFF_REMOVED`, or `EMF_BUFF_REFUSED` ("the target does not have it"). |
| `on_buff(cb, user)` | `api()->on_buff(cb, user)` | `cb(world, const EmfBuffEvent*, user)` after each server step, on its thread, for every change of a **player's** buffs, whoever made it: `EMF_BUFF_APPLIED`, `_REFRESHED`, `_REMOVED` (never `_REFUSED`). False for a null `cb` or 16 callbacks already. |
| | `emf::has_v32()` | true when the core has these functions. |

`EmfBuffType` (`size`, then): `id` (stable: store ids, not indexes), `slot` (`EMF_BUFF_FOOD` 0, `_BUFF` 1, `_DEBUFF` 2,
`_RESTED` 3, `_TERRAIN` 4), `apply_type` (giving again: 0 refused, 1 its timer starts again, 2 and 3 replaced by a new
buff), `faction_exclude` (a bit per faction the type is never given to; bit 7 = players), `flags`, `default_ns` (its
default time in nanoseconds), `follow_up` (the type given when this one ends, Soaked then Wet; 0 = none), `name_tag` and
`desc_tag` (the game's text tags; `buff_text` reads them).

Type flags: `EMF_BUFF_HIDDEN` 0x01 (not shown in the game's UI), `EMF_BUFF_SAVED` 0x02 (kept in the world save: potions,
Wet, food; combat buffs are not), `EMF_BUFF_DESPAWN_ON_DEATH` 0x04 (ends when its target dies; EMF does not check
whether the target is alive when it gives one), `EMF_BUFF_FOOD_POISON` 0x08, `EMF_BUFF_SHOW_AS_TAG` 0x10 (shown as a tag on an enemy's health bar).

`EmfBuff` (`size`, then): `type`, `entity` (the buff's own entity), `slot`, `index` (its place in that slot group),
`cleansed` (1: being removed, gone after the next step), `level` (from its source's level, 0 = none), `total_ns` (its
whole time) and `remaining_ns` (the time left); both -1 when not known yet (the step it was given).

`EmfBuffEvent` (`size`, then): `kind` (`EMF_BUFF_APPLIED` 1, `_REFRESHED` 2, `_REMOVED` 3, `_REFUSED` 4), `target` (the
entity that has or had the buff), `type`, `buff_entity` (0 when refused), `slot`, `cleansed` (REMOVED: 1 when it was
cleansed: always for a mod's buff_remove, for the game's own cleanses when EMF saw the mark; 0 when it ended or was
replaced), `replaced` (REMOVED: 1 when a new buff of the same type took its place in the same step, given again with a
replace type; its APPLIED comes right after), `total_ns` and `remaining_ns` (-1 = not known),
`why` (REFUSED: the reason, valid during the callback; else NULL).

**Names.** Mods should store ids, not names. Names repeat ("Burning" is ten types) and change with the language. The
game's text is there on a game client only; the dedicated server has ids. Find ids with the console command
`buffs.list` or `buff_types`. On game build 1287369 there are 274 types: 127 buff, 118 debuff, 17 food, 11 terrain, 1
rested. Some ids: Wet `0xAC13C6C3` (debuff, 900 s, saved; rain gives it again every step), Elixir Buff `0xFF24634F`
(buff, 1800 s, saved, ends on death), Rested `0xCDBE9C80`, Poisoned `0x369C3618` (players only).

**Refusal reasons** (`why`): unknown buff type; the target has no buff container; the type is never given to the
target's faction; the game did not give it (the target's faction could not be read); no free slot of that group; already there and this type cannot be given again; the
target does not have it (remove; also when it is already being removed); not the server world; giving not available on this game build (signature); 64
requests waiting.

**Limits and notes.**
- 16 requests are handled per step; 64 may wait.
- Food types are categories: without an item a food buff likely has no stats. Pass the eaten item.
- Damage over time scales from the source's stats: give a creature as `source`. With `source` = target a
  damage-over-time buff may do nothing.
- Long saved buffs stay in the world save, like the game's own.
- Giving a type that is already there follows the type's `apply_type`: its timer starts again, a new buff replaces the
  old one, or the request is refused.

**Events.** `on_buff` covers players only. After each server step EMF compares their buffs with the step before (the
player list itself is read at most 4 times a second, so a player who just joined is compared within a quarter second).
The first sight of a player (a new world, a join) says nothing. A buff counts as `REFRESHED` when its end moved later by more than a second (rain
gives Wet again every step, so only a move that large counts).

Works on game build 1287369 (game and dedicated server). When the game's buff code cannot be found on a build,
`buff_apply` is refused (logged) and listing and reading still work.

Console commands (on the host; [Console commands](#console-commands-built-in)): `buffs.list [food|buff|debuff|rested|terrain] [text]`,
`buffs.of [entity]` (default 1), `buffs.give <entity> <type id> [seconds]`, `buffs.clear <entity> <type id>`. Ids are hex
(`0xAC13C6C3`) or decimal. `give` and `clear` answer "queued" and log the result after the step.

Live-checked 2026-11-11 (hosted game): the Elixir Buff given for 60 s showed on the game's HUD as "Elixir Buff 0:47" and
counted down; given again for 120 s it got a new buff (the Replace type); a clear removed it and a second clear was
refused ("the target does not have it"); in rain Wet was given again by the game right after a clear.

Lua mods use the `emf` table described in [lua.md](lua.md) (run by the `lua` mod, built on this API).

## v33: markers

Markers that players see: over the 3D view (a shape or a picture, a text and the distance, an arrow at the screen's edge
when it is behind or off screen) and on the game's map screen. EMF draws them on each player's game, in the game's
style (the HUD font and shadow), only while the game shows its own world-space UI (not on the map, in menus or with the
console open). On the map they show while it is open, with their text.

- `marker_set` is for the game the mod runs in (a single-player or hosted game, or a joined player's game with the
  mod). The dedicated server draws nothing: there `marker_set` is false and `marker_send` is the way.
- `marker_send` (server world: the host's game or the dedicated server) puts markers on one player's game, or on every
  player's: they arrive over EMF's core channel `emf/markers` (mod messages), so a player needs EMF on their game, not
  the mod. The host's own player gets them in the same process.
- A marker can follow an entity (`entity`): each game draws it where that entity is in its own world. A server refreshes
  the position every 2 s while the entity moved more than 4 m (an entity far from a player is not in that player's
  game, so it is drawn at the last position the server sent).
- `EMF_MARKER_GAME_MAP` draws the marker on the map screen; with `game_icon` the game draws one of its own map icons
  there instead of EMF's shape (EMF adds an entry to the game's own map list; `marker_types` gives the icon types the
  game's own entries used in this world). EMF's entries never take the room the game's own map markers need.
- Caps: 256 markers per mod and 1024 in all on one game (a server's count apart from the game's own mods); a text is
  cut at 64 bytes; at most 256 in one `marker_send`.

| C | C++ | What |
|---|---|---|
| `marker_set(&m)` | `api()->marker_set(&m)` | sets (or updates) the calling mod's marker `m.id`. Any thread. False (logged once per kind) for a bad id or numbers, the mod's 257th or the game's 1025th marker, or on the dedicated server. |
| `marker_remove(id)` / `marker_clear()` | | removes one of the mod's markers (false: none) / all of them (how many). |
| `marker_types(out, cap)` | | the game's map icon types its own map entries used in this world (`EmfMapMarkerType`: `id`, `flags` 0, `name` empty): the number there is; fills `out` up to `cap`. |
| `world_to_screen(x, y, z, &px, &py)` | | a world point in display pixels with the camera of the newest frame; false when it is behind the camera or no frame is known (a menu, a loading screen, the dedicated server). Any thread. |
| `marker_send(player, markers, count)` | | server world: markers for player `player`'s game (their entity in the server world), or every player's (0); how many players it went to. |
| `marker_unsend(player, id)` | | takes a sent marker off that player's game (0: everyone's). |
| | `emf::has_v33()` | true when the core has these functions. |

`EmfMarker` (`size` = `sizeof(EmfMarker)`, then): `entity` (follow it; 0 = the position), `id` (1-48 of `A-Z a-z 0-9 _
. - :`), `x`, `y`, `z` (world metres: x east, y up, z north), `text` (may be NULL), `height` (metres above the position
or the entity), `max_distance` (fades out toward it, hidden beyond; 0 = no limit), `rgba` (`0xRRGGBBAA` of the shape;
0 = the HUD's accent colour), `icon` (a picture from `image_load`, v26, instead of the shape; not for `marker_send`),
`flags` (`EMF_MARKER_WORLD` over the 3D view, `_GAME_MAP` on the map, `_EDGE_ARROW`, `_DISTANCE` the distance under the
text), `shape` (`EMF_MARKER_DIAMOND`, `_CIRCLE`, `_SQUARE`, `_NONE`: text only), `game_icon`.

```cpp
EmfMarker m{};
m.size = sizeof m;
m.id = "bounty";
m.x = 4120; m.y = 96; m.z = 7333;
m.height = 2.5f;
m.text = "Bounty: Fell Brute";
m.rgba = 0xffcc33ffu;
m.flags = EMF_MARKER_WORLD | EMF_MARKER_GAME_MAP | EMF_MARKER_EDGE_ARROW | EMF_MARKER_DISTANCE;
api()->marker_send(0, &m, 1);  // on the server: every player with EMF sees it
```

Console: `markers` (what is there, whether drawing and the game's map list are on), `markers.test [text] [game icon]`
(a marker 10 m ahead of you; `markers.test clear` removes it).

## v34: the map screen's mouse, map actions, the ground

Players can act on the game's world map, and a server mod can find the ground under a spot.

- `map_cursor` and `on_map_click` are for the game the mod runs in. They work while the game's map screen is open. The
  game gets the click too. `on_map_click` calls come on the game window's thread (its window procedure): keep them short and do not call the game.
- `map_action` is for the server end (the host's game or the dedicated server). A map action is something players do by
  holding Shift and the left mouse button on their map for `hold_ms` (0 = 1000 ms, at most 10000); a ring fills at the
  mouse, and the game does not get that click. EMF on each player's game does the gesture and sends the spot over its
  core channel `emf/mapact` (mod messages), so a player needs EMF on their game, not the mod. The callback runs on the
  server world's thread.
- v34 has one gesture: only the FIRST announced action is what Shift + hold does. While no server end has an action,
  Shift + clicks stay the game's.
- The server checks every spot: inside 0..10240, a known action, and at most 20 per player in 10 s.
- `ground_at` (server world only) finds the ground under a spot with the game's voxel ray after the next server steps
  (two readings that agree), so an answer may take a few seconds. The callback runs once on the server world's thread.
- Caps: 16 `on_map_click` listeners, 16 map actions, 64 `ground_at` requests waiting. Ids are 1-48 of `A-Z a-z 0-9 _ . - :`.
- The answer also says which shroud a player standing at the found spot would be in: `fog` and `deadly`. EMF reads it
  1 m and 2 m over the ground and 3 m around, and waits for the shroud data to load (5 s at most). `fog` is the worst
  reading found; `EMF_FOG_UNKNOWN` when it could not be read (this game build, or it did not load in time).

| C | C++ | What |
|---|---|---|
| `map_cursor(&x, &z)` | `api()->map_cursor(&x, &z)` | the world x, z under the mouse while the map is open; false when it is not, or the mouse is off the map. |
| `on_map_click(cb, user)` | | `cb(const EmfMapClick*, user)` for each click on this game's map screen. False for a null `cb` or 16 listeners already. |
| `map_action(id, label, hold_ms, cb, user)` | | server end: adds a map action. The same id again replaces it; a null `cb` removes the caller's action with that id (false: it had none). False for a bad id, 16 actions already, or no mod messages on this game build. |
| `ground_at(world, x, z, cb, user)` | | server world: the ground under (x, z), once. False when refused at once: not the server world, outside the world, 64 waiting, or not available on this game build. |
| | `emf::has_v34()` | true when the core has these functions. |

`EmfMapClick` (`size`, then): `button` (0 left, 1 right), `mods` (`EMF_MAP_SHIFT` 1, `EMF_MAP_CTRL` 2, `EMF_MAP_ALT` 4),
`x`, `z` (world metres).

`EmfMapDone` (`size`, then): `player` (their entity in the server world), `admin` (1 = the host's own game, or an account
in `[chat] admins`; also 1 for everyone when `everyone=1`; it is the server's own view, never taken from the message),
`x`, `z` (inside the world), `action` (the id; valid during the callback).

`EmfGround` (`size`, then): `status`, `x`, `y`, `z`, `hop_y`, `why` (valid during the callback), `fog`, `deadly`. `status` is
`EMF_GROUND_OK` (0: `y` is the ground), `EMF_GROUND_NOT_LOADED` (1: nobody is near the spot; `hop_y` is a height over it
where a player may be teleported so the game loads the place; ask again once they are there) or `EMF_GROUND_FAILED` (2:
`why` says why). When `status` is OK, `fog` is `EMF_FOG_NONE` (0), `EMF_FOG_TEMPORARY` (1: shroud the players removed),
`EMF_FOG_DANGEROUS` (2: the shroud, its timer runs), `EMF_FOG_DEATH` (3: deeper shroud than this world's progress allows;
a player there dies) or `EMF_FOG_UNKNOWN` (255). `deadly` (0..1) is the share of deadly shroud there; `EMF_FOG_DEATH`
starts at 0.25. From EMF 0.6.8 (check that `size` covers `water_y`): `water` is 1 when the spot is under water at least
1 m deep (64 m at most), and `water_y` is the water's surface; `y` stays the ground under it, and `fog` is the worse of
the surface and the bed. A player
teleported there should land 0.5 m under `water_y` without `EMF_TELEPORT_FIND_GROUND` (the game's search would take the
lake's bed; at the surface itself a player falls in, dives and sinks to the bed; 1 m under it they hang still, drowning:
seen in game). The water comes from the game's own water simulation (`WaterSurfacePlane`); where this build has none, `water`
stays 0.

```cpp
static void on_ground(EmfWorld*, const EmfGround* g, void* user) {
    uint32_t player = (uint32_t)(uintptr_t)user;
    if (g->status == EMF_GROUND_OK)
        EMF_LOG("mymod", "ground at %.0f %.0f is %.1f (asked by %u)", g->x, g->z, g->y, player);
    else
        EMF_LOG("mymod", "no ground: %s", g->status == EMF_GROUND_FAILED ? g->why : "not loaded");
}
static void on_ping(EmfWorld* w, const EmfMapDone* d, void*) {
    api()->ground_at(w, d->x, d->z, on_ground, (void*)(uintptr_t)d->player);
}
// at load, in the server end:
api()->map_action("ping", "Ping here", 800, on_ping, nullptr);
```

Console: `map.where` (runs anywhere, also on a game you joined: the world spot under the mouse on the open map, and whether
this game's server offers a map action), `map.act <x> <z>` (runs anywhere: the server's map action at that spot, as the
gesture's end sends it, with the same checks) and `ground <x> <z>` (needs a world you host: the ground's height at that
spot; the answer follows a moment later). The Teleports mod uses a map action: see `mods/teleports/README.md`, "Teleport by the
map".

## v35: a minimap's parts

The parts a minimap is made of: where the player is and faces, how much of the map is explored, the game's own map
markers, and canvases: drawing panels a mod owns, which EMF draws over the HUD. Client only: the dedicated server draws
nothing, and these functions answer false or 0 there. The sample minimap (`mods/minimap`) is built from them.

- `player_view` gives the local player's position, heading (0 north = +z, 90 east = +x) and camera yaw (the same way;
  set when a frame of the 3D view was seen, `EMF_VIEW_CAMERA`), and the map's size in metres (10240 on Embervale).
  False at the main menu and before the player is known.
- `fog_at` gives the fog of war at a point: 0 unexplored .. 1 explored. It reads a copy that is refreshed twice a second.
- `map_markers` gives the game's own map markers (altars, shrines, places found) as its map list had them last: at
  most 512. `EMF_GAME_MARKER_SHOWN` is set when the game's map shows one.
- `canvas_set` makes, changes or removes a canvas. `corner`, `x`, `y` (measured from the corner, inward) and `w`, `h`
  are in units of a 1080-pixel-high screen; `w` and `h` are 8..1080. `shape` is `EMF_CANVAS_RECT` or
  `EMF_CANVAS_CIRCLE`. A canvas belongs to the mod that made it (another mod cannot change it). 32 canvases at most.
  Lua ids are `"<mod>:<id>"` in the core. Ids are 1-48 of `A-Z a-z 0-9 _ . - :`.
- `canvas_draw` replaces the canvas's drawing with a list of commands (2048 at most; more are left out, and a command
  with bad numbers is left out). EMF keeps the list and draws it every frame, so call it only when the picture changes.
- Commands (`EmfCanvasCmd`, positions in canvas units, 0, 0 is the top left): `EMF_CANVAS_RECT_CMD`,
  `EMF_CANVAS_CIRCLE_CMD`, `EMF_CANVAS_LINE`, `EMF_CANVAS_TRI`, `EMF_CANVAS_TEXT`, `EMF_CANVAS_IMAGE` (a picture from
  `image_load`), `EMF_CANVAS_FOG` (the fog of war drawn as cells), `EMF_CANVAS_MARKERS` (the markers flagged
  `EMF_MARKER_MINIMAP`) and `EMF_CANVAS_MAP` (the game's own world map). The numbers each kind takes are in the header
  next to the define. FOG, MARKERS and MAP take `a[0..1]` = the world x, z at the canvas centre and `a[2]` = metres
  per canvas unit.
- The MAP command is drawn by the game's own map renderer, so it looks like the world map (relief, shroud, fog of war).
  It is north up: a mod turns its own arrow and icons.
- Draw order: the game draws the MAP command before EMF draws anything else. What a list puts BEFORE its last MAP
  command is under the map, and EMF draws it only when the game cannot draw the map. Put a background first, then the
  map, then what goes on top.
- The game clips its map to the canvas's rectangle, also for a round canvas. A mod that wants a round look draws a ring
  over the corners.
- EMF hides canvases where it hides markers: on the map screen, in menus and in the console.

| C | What |
|---|---|
| `player_view(&view)` | fills `EmfPlayerView`: `x, y, z`, `heading_deg`, `camera_yaw_deg`, `world_size`. False without a player. Any thread. |
| `fog_at(x, z, &explored)` | 0..1 at that point. False without a map (the dedicated server, before a world). Any thread. |
| `map_markers(out, cap)` | how many there are; fills `out` (`EmfGameMarker`: `type`, `flags`, `x, y, z`) up to `cap`; `out` may be NULL to count. Any thread. |
| `canvas_set(id, &canvas)` | a canvas; a NULL canvas removes it. False: a bad id, bad numbers or 32 canvases already. |
| `canvas_draw(id, cmds, count)` | the drawing. False for an unknown canvas. Any thread. |

`EMF_MARKER_MINIMAP` (0x10) is a marker flag: a marker with it is drawn by a canvas's MARKERS command.

```cpp
// A square minimap at the bottom right: the game's map around the player, 2 m per unit.
EmfCanvas c{};
c.size = sizeof c;
c.corner = EMF_CANVAS_BOTTOM_RIGHT;
c.shape = EMF_CANVAS_RECT;
c.x = 16; c.y = 64; c.w = 220; c.h = 220;
api()->canvas_set("map", &c);

// every client step:
EmfPlayerView v{};
v.size = sizeof v;
if (api()->player_view(&v)) {
    EmfCanvasCmd cmds[1] = {};
    cmds[0].kind = EMF_CANVAS_MAP;
    cmds[0].a[0] = (float)v.x;
    cmds[0].a[1] = (float)v.z;
    cmds[0].a[2] = 2.0f;  // metres per canvas unit
    api()->canvas_draw("map", cmds, 1);
}
```

## v36: removing a placed object

`remove_building(world, &req)` removes an object a player placed (it has a `UsedItem` component) the way a pick-up does:
the same engine call as `destroy`, and the item goes back to a player or to the ground. Same calling rules as `destroy`:
the SERVER world, inside a world callback on it (`on_tick`, `run_on_world`, `on_event`). C++: `emf::has_v36()` in
`emf.hpp`.

| C | Rules |
|---|---|
| `remove_building(world, &EmfRemoveBuilding)` | Removes `req->entity`. Gives `{the UsedItem's item, 1}` back, and only when placing that item used it up (`ItemInfo.placementConsume`). Returns true when it was removed. |

Set `size` to `sizeof(EmfRemoveBuilding)`. Fields:

| Field | What |
|---|---|
| `entity` | the object to remove |
| `refund` | `EMF_REFUND_NONE` (0): nothing back. `EMF_REFUND_DROP` (1): a loose pickup at the object, 0.5 m up. `EMF_REFUND_PLAYER` (2): put into `player`'s inventory through the game's transaction; when nothing fit, dropped at the object instead. |
| `player` | with `EMF_REFUND_PLAYER`: who gets the item |
| `flags` | `EMF_REMOVE_FORCE` (0x01): remove it even when it holds items: they drop at its spot as loose pickups, a stack each, with their item, count, level and rarity (other per-item state, such as wear, is not kept; 0.6.7 lost them). Never works on a Flame Altar. |
| `refunded` | out: the item id given back (0: none). With `_PLAYER` the inventory's answer comes after the step, and a full inventory means a drop at the object. |
| `why` | out: why it was refused (`""` when removed); also written to the log |

Refused, with nothing changed:

- a player;
- no such entity (or it has no position);
- a Flame Altar, even with `EMF_REMOVE_FORCE` (use `destroy` if you really mean it: destroying an altar loses the whole
  base at the next save);
- an object no player placed (no `UsedItem`): use `destroy`;
- an object holding items in its own or linked inventories, unless `EMF_REMOVE_FORCE` (then the items drop at its spot; refused when one of them cannot be dropped, or an inventory cannot be read);
- a refund player who is not a player;
- an unknown `refund` or `flags`;
- an object removed already in this step;
- a call outside a world callback, or not on the server world;
- on a game build where EMF cannot destroy the object, give its item back or drop what it holds.

If the game's destroy call fails, it returns false with that reason ("the game's destroy call failed (see the log)").

Like `destroy`, it plays no pick-up sound and sends no `BuildingPickUpEvent` (nothing in the game needs it for player
objects).

```cpp
EmfRemoveBuilding req{};
req.size = sizeof req;
req.entity = object;
req.refund = EMF_REFUND_PLAYER;
req.player = player;
if (!api()->remove_building(world, &req)) {
    // req.why says why, e.g. a chest that is not empty
}
```

## v37: paths on the game's path grid

`path_find(world, &query, cb, user)` asks the game's own path grid for a path. The grid knows terrain, buildings,
placed objects and doors. It moves nothing: you get points and walk them yourself. C++: `emf::has_v37()` in `emf.hpp`.

| C | Rules |
|---|---|
| `uint32_t path_find(world, const EmfPathQuery* q, EmfPathCallback cb, void* user)` | Returns the query's id, or 0 when refused. The answer comes once, after a server step or more, to `cb(world, id, result, points, user)` on the server world's thread. `points` is valid during the call only. |
| `bool path_cancel(uint32_t id)` | Cancels an open query. The callback still comes, with `EMF_PATH_CANCELLED`. |

Refused (0, logged once per 10 s):

- not the server world: a joined game has no path grid, so use it on the host or the dedicated server;
- bad numbers;
- `from` and `to` more than 200 m apart (the game does not search farther);
- no owner (`owner` 0 means the first player, and none is there);
- 32 EMF queries already open;
- the feature is off on this build.

`EmfPathQuery` (set `size` to `sizeof(EmfPathQuery)`):

| Field | What |
|---|---|
| `owner` | an entity the query belongs to; 0 = the first player |
| `from`, `to` | start and target, in metres |
| `moves` | `EMF_PATH_WALK` is always added. Add `EMF_PATH_JUMP_UP`, `_JUMP_DOWN`, `_JUMP_ACROSS`, `_OPEN_DOOR`, `_SWIM`. A closed door is crossed only with `_OPEN_DOOR` (since 0.6.11 EMF forbids closed-door nodes, the node rule for type 19, for every other move: the grid counts a closed door's spot as ground, so a walk could cross it). The whole box of a closed door's collider, on its lowest 1 m layer, is closed: a door at an angle closes the floor beside it too; a start inside that box may find no start node. This is path_find only; creatures walk with the game's own rules |
| `max_jump_up`, `max_jump_down` | metres; 0 = no limit |
| `width`, `height` | the room the unit needs, in metres; 0 = the game's default. `width` works as a radius (the game doubles it into half metres of free room all round, as it does with a creature's `PathWalker.horizontalRadius`); the grid keeps at most about 2 m across and 3.5 m up, so more finds no start node. Jump flights are checked against fixed room, not these |
| `closest` | 1: fall back to the closest reachable point |
| `max_nodes` | 0 = 2000, at most 20000 |

`EmfPathResult`:

| Field | What |
|---|---|
| `status` | `EMF_PATH_FOUND`, `_NO_PATH`, `_TOO_LONG`, `_NO_START`, `_NO_TARGET`, `_NO_MEMORY`, `_CANCELLED`, `_OTHER` |
| `reaches` | true when the last point is within 1.5 m across and 3 m up or down of `to` |
| `count` | the points passed (at most 512 are kept) |
| `length` | metres from `from` along the points |
| `doors` | API v42 (0.6.11; read it when `size` covers it, `emf::has_v42()`): per point, the door entity the path goes through there (a point with `move` 5), else 0; `count` entries; NULL when no point is a door (or the game gave no usable list). Valid during the callback only. The game's own record of the door for that point |

Each `EmfPathPoint` has `x`, `y`, `z` and `move`: 0 walk, 2 jump up, 3 jump down, 4 jump across, 5 a door.

Seen live:

- A 22 m path on open ground answered in about 0.2 s.
- **Use `closest = 1` and check `reaches`.** Without `closest`, a target the grid cannot match (on a rock, in the air)
  spends all its nodes and ends `TOO_LONG`. With `closest` the path ends at the nearest reachable point and `reaches`
  is false.
- Long paths need more nodes: 2000 nodes got about 36 m of a 141 m route, 20000 got about 90 m in about 1.3 s.
- The grid exists only where the world is loaded (near players). Far ends give `NO_START` or `NO_TARGET`, or `OTHER`
  after 10 s with no answer.
- The game's own march asks with "closest" and treats `FOUND` as success, which is why its enemies walk to a wall and
  stand there. A raid mod should check `reaches`.
- Each query is a search in the game's own jobs, shared with every walking creature. Do not ask every step (the game's
  enemies ask at most every 0.5 s per creature).
- A new world, or `path_cancel`, ends open queries as `CANCELLED`.

```cpp
EmfPathQuery q{};
q.size = sizeof q;
q.from = raider_pos;
q.to = altar_pos;
q.closest = 1;
q.max_nodes = 5000;
uint32_t id = api()->path_find(world, &q, [](EmfWorld*, uint32_t, const EmfPathResult* r, const EmfPathPoint* pts, void*) {
    if (r->status == EMF_PATH_FOUND && r->reaches) { /* walk pts[0..r->count) */ }
}, nullptr);
```

## v38: doors and gates

A door is an entity with `ServerToggleState` and `DirectionalInteraction` (player doors and gates). Use these on the
server world: the host, or the dedicated server. C++: `emf::has_v38()` in `emf.hpp`.

| C | Rules |
|---|---|
| `uint32_t door_state(world, entity)` | `EMF_DOOR_NONE` (not a door, or not the server world), `EMF_DOOR_CLOSED` or `EMF_DOOR_OPEN`, plus the flags `EMF_DOOR_LOCKED` (0x10: EMF holds it locked) and `EMF_DOOR_LOGIC` (0x20: a switch drives it; EMF does not touch it) |
| `bool door_set(world, entity, uint32_t open, uint32_t side)` | Opens (1) or closes (0) the door at the next server step. Players see it swing, and the game's collider and path grid follow it, so enemies' paths change. `side`: 0 = the game's default (back), 1 front, 2 back |
| `bool door_lock(world, entity, uint32_t locked)` | While locked, no player gets the E prompt on it (EMF keeps it so each step, from the next step on). From 0.6.8 EMF keeps the lock with the world, by the door's place (`emf\config\emf_doors\<slot>.txt`; entity ids change between loads): after a load the door standing there (within 0.5 m) is locked again, looked for every 3 s while it is not loaded (every 30 s after a minute). A locked door that goes while a player is within 40 m (broken, taken down) loses its lock; one that goes with nobody near was unloaded and is locked again when it loads. `door_set` still works on a locked door |

`door_set` is refused (false, logged once per 10 s) when: it is not the server world, the entity is not a door, a
switch drives it, it is already so, `side` is bad, 256 door orders are waiting already, or the feature is off on this
build. `door_lock` is refused for the same reasons except "already so" and `side`, and a lock also when 1024 doors are
locked already. A door EMF locked can still be unlocked when a switch drives it now. No open or close sound
plays.

Seen live on the host: a spawned Wooden Door opened and closed by `door_set` (it swings); locking removed the "E Open"
prompt and unlocking brought it back. Not seen yet: a joined player's view, enemies walking through an opened door, and
what a locked door does to villagers or enemies that want to open it.

```cpp
uint32_t st = api()->door_state(world, gate);
if (st != EMF_DOOR_NONE && !(st & EMF_DOOR_LOGIC)) {
    api()->door_set(world, gate, 1, 0);   // open it at the next step
    api()->door_lock(world, vault, 1);    // nobody opens the vault door with E
}
```

## v39: enemy orders

A creature with an enemy AI (`DynamicAggro`, `SelectedTargets`) sent at any entity with a position for a while. While
the order holds, EMF writes the target as its pick (`SelectedTargets` slot 0, target and root) with its target switches
off (`DynamicAggro.allowTargetSwitches`) and gives the target an alarmed entry (aggro 100) in its aggro list, at once and
after every server step; an enemy at rest (Idle, Roaming, ReturnHome, Guard, Sleep and the like) is put in its `Combat`
state (one in an attack, a hit, a fall, spawning or dead is left as it is). Seen in game: with the pick alone a Vukah did
not move; an idle one with the entry did not move either; in Combat it walked 30 m to a door, and one hit with 200
damage added broke it. The game's own hits on a placed door did 0. C++: `emf::has_v39()`.

| C | Rules |
|---|---|
| `uint32_t enemy_order(world, EmfEnemyOrder* o)` | `o->size = sizeof(EmfEnemyOrder)`; `enemy`, `target`, `flags` (`EMF_ORDER_ALLIES`: the creature may be a player's ally), `seconds` (0 = until it ends otherwise), `damage` (0..100000: added to each of the enemy's hits on the target, through a hit rule). The order's id, or 0 with `o->why` |
| `bool enemy_order_cancel(uint32_t id)` | Ends it: at once inside a server world callback, else at the next step. False: no such order |
| `uint32_t enemy_target(world, enemy)` | What it goes for now (its pick), 0 = nothing |

An order ends after its time, when the target is gone or dead (a target without health, such as a door, is never
dead), when the enemy is gone, dies, becomes a player or a player's ally (tamed: unless `EMF_ORDER_ALLIES`), on cancel, with a new order on the same enemy, or when the world closes; then the
switches go back on and its entry (and the pick, if it is still the target) are cleared, an enemy left in `Combat` with
nothing picked goes back to `Idle`, and its hit rule goes. `enemy_order_cancel` may be called from any thread.
`enemy_order` is refused when: not inside a world callback on the server world, `enemy` or `target` is 0 or they are the
same entity, `flags` has a bit this core does not know, the enemy has no enemy AI, it is a player, a boss (`Boss`,
`BossHealthBar`: its fight and leash stay the game's), a player's ally without `EMF_ORDER_ALLIES`, the target has no
position or is dead, `seconds` or `damage` is out of range, 256 orders are held, or `damage` is asked and gets no hit
rule: all 64 hit rules, shared by every mod (`hit_rule_add`, other orders with `damage`), are in use, or hit rules are
off on this build. A new order with `damage` needs a free hit rule even when it replaces one with `damage`: its rule is
asked for while the old order still holds its own, so a mod that re-targets its own raiders at the limit is refused
(the old order keeps running). Nothing is saved.

```cpp
EmfEnemyOrder o{};
o.size = sizeof o;
o.enemy = raider;
o.target = gate;
o.seconds = 60;
o.damage = 40;  // the game's own swings do not hurt a placed gate
if (!api()->enemy_order(world, &o)) EMF_LOG(mod, "no order: %s", o.why);
```

Console: `order <enemy> <target> [seconds] [damage]` and `order.stop <id>` (never a tamed beast or a boss).

## v40: move orders

A creature with the enemy AI (`DynamicEnemy01BehaviorState`: an enemy, or a tamed beast with `EMF_MOVE_ALLIES`) walked
through up to 16 points by the game's own guard order (`StartGuardingEvent`) on a hidden waypoint EMF spawns at each point
in turn (the game's `TestTransform` template: a position, not saved), sent from EMF's `emf_moves` program. It walks the
game's way: its own path and jumps where its kind can. The guard order is sent again every 3 s while it is not fighting
and not guarding its waypoint, and a calm creature (roaming, at home, patrolling, investigating, asleep) is put in `Idle`
with it (seen: a roaming Vukah took the guard only from Idle), and one that took the waypoint as its guard target but stays
calm is put in `Guard` (seen: a freshly spawned Vukah sent 30 m kept roaming and never walked; in `Guard` it walked).
Spawn where the ground is loaded: about 150 m from every player it was not, and a Vukah spawned there fell through the
world (`ground_at` says whether a spot is loaded). A guarding creature stops its guard distance
(`currentGuardTargetDistance`, about 3 m) short of its waypoint, so a point counts as reached within that + 1 m, or
within `arrive` if larger (and that + 2 m up or down). Seen in game: a Vukah walked 36 m in 10 s; to a point 37 m up it
roamed and the move ended stuck. C++: `emf::has_v40()`.

| C | Rules |
|---|---|
| `uint32_t enemy_move(world, EmfMoveOrder* o, EmfMoveCallback cb, void* user)` | `o->size = sizeof(EmfMoveOrder)`; `enemy`, `flags` (`EMF_MOVE_ALLIES`, `EMF_MOVE_PEACEFUL`, `EMF_MOVE_PEACEFUL_ALL`), `count` (1..16) `points` (`EmfPathPoint`: x, y, z; `move` is ignored, so path_find's points fit: thin a longer path to 16), `arrive` (0 = 2; 0.5..50 m), `seconds` (0 = no limit; at most 3600), `stuck_seconds` (API v41, 0.6.10: read when `size` covers it; a v40 core ignores it: no metre closer for this long while not fighting ends it stuck; 0 = 15; 2..120). The move's id, or 0 with `o->why`. `cb(world, id, status, user)` (may be NULL) is called once when it ends |
| `bool enemy_move_cancel(uint32_t id)` | Ends it (`EMF_MOVE_CANCELLED`): at once inside a server world callback, else at the next step. False: no such move |
| `uint32_t enemy_moving(world, enemy)` | Its move's id, 0 = it is not moving |

Status: `EMF_MOVE_ARRIVED` (at the last point), `EMF_MOVE_STUCK` (no metre closer for 15 s, or `stuck_seconds`, while not fighting),
`EMF_MOVE_TIMEOUT`, `EMF_MOVE_GONE` (it died, went, became a player, or a player's ally without `EMF_MOVE_ALLIES`),
`EMF_MOVE_CANCELLED` (`enemy_move_cancel`, a new move or `enemy_order` on it, or the world closed: then `world` is
NULL). The callback runs once, after the move is gone (it may start another move): after a server step on the server
world's thread, inside `enemy_move_cancel`, inside the `enemy_move` or `enemy_order` that replaced it, or (world NULL) when
the world closes. A waiting guard order of an ended move is dropped; a waypoint that vanishes is placed again up to 3 times. `EMF_MOVE_PEACEFUL`
clears its fights against players each step (`EMF_MOVE_PEACEFUL_ALL`, API v41: every fight, animals and players'
beasts too; a v40 core refuses the flag: check `emf::has_v41()`, which goes by the version number since the table did
not grow) (a creature left in `Combat` with nothing to fight goes to `Idle`); without
it the creature may fight on the way (the stuck clock waits) and goes on afterwards. When a move ends its waypoint goes;
a creature that guarded someone before guards them again (a tamed beast: its owner); any other stops guarding, and if it
arrived its home becomes the last point (it lives there). A move and an enemy order on the same creature end each other (once the new one holds: a refused one ends nothing).
`enemy_move` is refused when: not inside a world callback on the server world, no enemy AI, a player, a boss, a player's
ally without `EMF_MOVE_ALLIES`, it is dead, bad points or numbers, 256 moves are held, or no waypoint could be spawned.
Nothing is saved.

```cpp
EmfPathPoint gap{x, y, z, 0};
EmfMoveOrder o{};
o.size = sizeof o;
o.enemy = raider;
o.flags = EMF_MOVE_PEACEFUL;
o.count = 1;
o.points = &gap;
if (!api()->enemy_move(world, &o, on_moved, nullptr)) EMF_LOG(mod, "no move: %s", o.why);
```

Console: `move <enemy> <x> <y> <z> [peaceful]`, `move <enemy> <entity | me> [peaceful]` and `move.stop <id>` (never a
tamed beast); the console says later how the move ended.

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
| `emf/storage.hpp` | `emf::WorldFile`: a per-world text file for your mod (loads once per session, saves atomically, never into another world's file), and a text per player in it (`player(account)`, `save_player(account, text)`: at most 16 KiB each, 2 MiB in all) |
| `emf/teleport.hpp` | v20: `emf::teleport(w, id, pos, opts, f)`, `emf::teleport_to_spawn_point(w, id, spawn_point, f)`, `emf::player_spawn_point(w, player)`, `emf::spawn_points_near(w, pos, radius)` (see v20 above) |

## Console commands (built in)

`help`, `mods` (every mod, its version and status), `hooks` (every inline hook and its owner), `pattern <bytes> [.section]` (test a pattern live), `settings <mod>`, `set <mod> <key> <value>` (`enabled` = the switch,
from the next start), `profile [save <name> [--settings] [--replace] | load <name>]` (mod profiles: the switches of
every mod under a name, as in EMF.exe; a load applies at the next start), `hud.list`, `hud.move <part> <x> <y>`, `hud.scale <part> <s>`, `hud.hide <part>`, `hud.show <part>`, `hud.reset [part]`,
`hud.layout [save|load|delete <name>]`, `hud.swap <bar> <bar>`, `hud.order <bar> <bar> <bar>`, `hud.grow up|down`,
`buffs.list`, `buffs.of`, `buffs.give`, `buffs.clear` (v32: the game's buffs, see "v32: buffs"), `hits` (v31: one line with the state of the hit hooks: whether `on_hit_before` and the rules work or why not, how many rules and callbacks, hits seen, changed and cancelled, callback faults, the average microseconds per hit; whether fall, shroud, cold and drowning damage are on), `hud.colour [bar] [part] [colour]` (the HUD editor: offsets in percent of the screen, scale 0.5 to 2, the bars `health`,
`mana`, `stamina` move on their own and take colours, the mods' HUD lines are the parts `lines_top_right`,
`lines_bottom_right`, `lines_bottom_left`, layouts in `emf\hud`), `items [part]`, `give <item> [n]`, `put <item> [n] [entity] [level] [rarity]`,
`take <item> [n] [entity]`, `spawn <item name | template guid> [count 1-20] [at <x> <y> <z>]` (a placeable item's name spawns the template the item puts down; `items <name>` shows "places <guid>"; an item that places nothing is refused with a note; it spawns 2.5 m in front of you: the camera's direction on your own game, beside you otherwise), `time <hours>`, `pos`, `perf` (how long each world step took since the last `perf`: the game's own steps, EMF's wait
for the server world's jobs and each mod), `goto [#k] <x> <y> <z> [ground]` / `goto [#k] <entity> [ground]`
(v20: teleports you, or the k-th player, there or beside that entity; the result follows a few steps later). Coordinates for `goto`, `path`, `spawn ... at` and Teleports' `tp.pos` can be written many ways: `3 4 5`, `3, 4,5`, `(3, 4, 5)`, `[3 4 5]`, `<3> <4> <5>`, `3;4;5`, or a pasted `pos` line (`player 1 at (3.0, 4.0, 5.0) m`). For mod makers: `emf::coordinate_words(words)` in `emf.hpp` turns such words into plain numbers, `entities`, `near [radius]`, `inv <entity>`, `health <entity> [percent 1-100]` (read, or set to that share of the maximum; refused while it reads 0: dead, or a spawn not set up yet), `god [entity|me] [on|off]` (v31: the entity, you by default, keeps at least 1 health from hits, falls, the shroud, the cold and drowning, through one hit rule; `god` alone lists who has it; it ends with `off`, when a player leaves, when the entity is gone, and when a new world starts; refused while health reads 0), `ai <entity>` (the enemy01 state, the guarded entity, the combat target, the aggro entries and the faction),
`inspect <entity>`, `tpl <entity> <component> [bytes] [offset]`, `tplset <entity> <component> <offset> <type> <value>`,
`stack <item> <max>`, `destroy <entity>`, `remove <entity> [me|drop|<player>] [force]` (v36: removes a placed object like a pick-up; `me` (or `player`) gives the item back to whoever typed the command; `drop` leaves its item on the ground, a player number puts it in that player's inventory, `force` also removes a non-empty container), `path <x> <y> <z> [jumps] [doors] [closest] [nodes 1-20000]` (v37: a path from you to that spot on the game's path grid; prints `path N: asked ...`, then the answer: found or why not, whether it reaches or stops short, the points, the metres, where it ends, and the jumps and doors; `doors` (0.6.11) lets it open doors, and each door point names its door entity), `path.door <entity>` (0.6.11, host: what the game's path grid knows about a door, after a step: whether it is in the grid's door table ("not in the path grid's door table (N doors registered)" if not), open or closed there (kind 1 or 2; 3 is a moving object), its box in metres, the door row (the box's lowest 1 m node layer: a closed door's ClosedDoor nodes), and for the node in the row's middle whether its cell is built, its node type (19 ClosedDoor), ground or not, and its room across and up; "not available on this game build" where EMF does not know the grid's layout), `door <entity> [open|close|lock|unlock] [front|back]` (v38: without a word it prints the state: "closed", "open, locked", "driven by a switch" or "not a door"; `items door` lists door items and `spawn <guid>` puts one down for a test), `chat.players` (v19: the players of the world you host or of the
dedicated server, with their account hashes for `[chat] admins`, their connection and name), `world.get [setting]`,
`world.set <setting> <value> [persist]`, `world.reset [setting|all] [persist]`, `world.list`, `world.export` (v21: the
running world's settings, [world_settings.md](world_settings.md)). Commands go into the in-game console (the key under Esc), the optional
console window (`[console] window=1` in `emf\emf_core.ini`), or `<game>\emf\commands.txt` (one per line; EMF runs the
file about once a second). Replies are `[cmd]` lines in the log.

**Item names** in `give`, `put`, `take` and `stack` are the names the game shows, in the language you play in (`give wood
logs 20`, `give holzscheite 20` in German; the replies show them too: "gave 20 x Wood Logs (Material_T1_Wood_Logs)"),
or the game's debug names (`Material_T1_Wood_Logs`). A shown name typed exactly wins; when several items share it, or
the words only partly match, the debug-name rules below decide, and the shown names' words count when those decide
nothing. Debug names are typed the easy way: any case, spaces instead of `_`, words in any order, a part of a word is enough (`give wood logs 20`, `give
simple bait 5`, `give hardwood 10`). An item matches when its name holds every word typed; a typed number only matches
a whole number in the name (the 3 of `torch 3` matches the 03 of `Tool_Torch03_Wax`, never the 3 of a T3). Of the matches, in this order: the exact
name; the item with exactly the words typed; the only match; the only match whose own words were all typed (`wood logs`
is `Material_T1_Wood_Logs`, not `..._Logs_Dead`; the category, tier and number words such as `Material`, `T1`, `01`
and `simple` do not count), unless a typed word is only the start of a word in it while another match has it as a whole
word (`wolf` is not `Material_T5_Wolfsbane`: seven items have Wolf as a word); the only match that has every typed word
as a whole word (`hardwood` is `Material_T2_Wood_Hardwood`; the props have Hard and Wood). Otherwise nothing is given
and the reply names the candidates (`'logs' could be 6 items: ...`); `items <words>` lists them by the same rules, best
match first.

The plain numbers at the end are the command's numbers (`put wood logs 20 1025`); a number with a leading zero always
belongs to the name (`take tool torch 01 5` takes 5 torches from you, not 1 from entity 5). When a split whose words are
exactly an item's (`put tool torch 1 5`: Tool_Torch01) differs from reading every number at the end as the command's,
the exact one wins; when two readings name different items, nothing happens and the reply shows how to type each
(`'torch 2' can be read more than one way; type one of: give Torch_2 | give Torch 2`). A "quoted name" is the whole
name. `items wood` lists the matches with both names ("Wood Logs: Material_T1_Wood_Logs (...)"). The dedicated server
has no shown names (it has no text): there only the debug names work. Mods still look items up by the exact debug name
(`find_item`, `emf.item`) and get the shown name with `item_name` / `emf.item_name`.
