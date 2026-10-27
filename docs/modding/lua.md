# Lua mods: start here

A Lua mod is a folder with two text files. No compiler, no Visual Studio: write the files, start the game, and change
them while the game runs; EMF reloads a Lua mod within a second of you saving it.

Lua mods need the `lua` mod (in the EMF zip, on by default): without it (missing, switched off or failed) a Lua mod
and every mod that depends on it do not run, and the Mods tab says why. They run on the same EMF API as C++ mods, with
the same rules: gameplay changes happen on the **host** (your own game, or the game you host).

## Your first Lua mod (five minutes)

The quickest start is the starter in the EMF zip: copy `template_lua\` into `<game>\emf\mods\` and follow
[first_mod.md](first_mod.md) (a line on screen, a key, one setting of each kind, in 10 minutes). Or by hand:

1. Make a folder `<game>\emf\mods\hello_lua\`.
2. Put `mod.json` into it:

   ```json
   {
     "id": "hello_lua",
     "name": "Hello Lua",
     "version": "1.0.0",
     "emf": 14,
     "main": "main.lua",
     "depends": {"lua": ">=1.1"}
   }
   ```

   (`lua` 1.1 is the Lua host that has `emf.on_world_data`, `emf.save_world_data` and `world:account`; a mod that uses
   none of them can depend on `>=1.0`. 1.2 also takes full type names such as `keen::ecs::HitEvent`, and
   `emf.command` returns true or false. 1.3 adds `emf.on_key` and `emf.key_down`, and `emf.hud` takes a corner name
   and a `#rrggbb` colour: depend on `">=1.3"` for those. 1.4 adds `emf.time_of_day`, `emf.kill`, a level and rarity
   for `emf.put`, and commands for every player (`emf.command(..., {players = true})`): depend on `">=1.4"`. 1.5 adds
   `emf.health`, `emf.attribute`, `emf.attribute_add` and `emf.damage` (the host needs EMF API v18): depend on
   `">=1.5"`. 1.6 adds `emf.tell`, `emf.broadcast`, `world:name`, `emf.on_chat` and `emf.request_save` (EMF API
   v19), and `emf.command(emf.id, ...)` makes the bare command `/<id>`: depend on `">=1.6"`. 1.7 adds `emf.teleport`
   (EMF API v20): depend on `">=1.7"`. 1.8 adds `world:template`: depend on `">=1.8"`. 1.9 adds wheel groups (`emf.wheel_section`, `emf.wheel_group`): depend on `">=1.9"`. Ranges are explained in [guide.md](guide.md), "Dependencies
   and version ranges".)

3. Put `main.lua` next to it:

   ```lua
   emf.log("hello from Lua")

   emf.command("where", ": where am I?", function(world, args)
     local x, y, z = world:position(emf.local_player())
     emf.reply(string.format("you are at %.1f, %.1f, %.1f", x, y, z))
   end)

   emf.on_tick(function(world)
     if world:is_client() then
       emf.hud("clock", "Lua says hi", 2)  -- bottom left
     end
   end)
   ```

4. Start the game and load a world. "Lua says hi" shows in the bottom left; open the console (the key under Esc) and
   type `hello_lua.where`. Now change the text in `main.lua` and save: within a second the game shows "Lua mod
   hello_lua reloaded" and the new text.

`emf\mods\kill_counter\` is a complete example (it counts your kills): read it, then copy the folder under a new id
(and change `"id"` in its `mod.json`) to change it. Installing EMF again replaces `kill_counter` itself. It is installed
switched off: switch it on in the Mods tab to see it count.

## What happens when a script fails

A script error or an endless loop does not crash or freeze the game. EMF runs every piece of Lua protected:

- An error stops that call, the log gets `[hello_lua] hello_lua/main.lua:12: attempt to ...` and a note shows on
  screen. The mod is **paused**: its callbacks stop until you save one of its files again, then it reloads.
- A call that runs longer than 250 ms (an endless loop) is stopped the same way, with the line it was running;
  `pcall` cannot catch that stop. The time `emf.save_world_data` spends writing (a slow disk) does not count, up to
  1 s per call.
- A Lua mod may use up to 256 MB of memory. Past that the call fails with "not enough memory" and the mod is paused
  the same way.
- `require` loads `.lua` files from the mod's own folder. Only Lua source runs: precompiled chunks are refused.
- Gone: `os.exit`, `os.execute`, `io.popen`, native modules (`package.loadlib`) and the `debug` library (except
  `debug.traceback`).

This protects the game from mistakes, not your PC from a malicious mod: a Lua mod can read, write and delete files
like any program. Install mods only from people you trust.

`lua.status` in the console lists every Lua mod and whether it runs or why it is paused; `lua.reload` checks the files
right away.

## The `emf` table

| Call | What it does |
|---|---|
| `emf.id` | your mod's id |
| `emf.dir` | your mod's folder (for your own data files) |
| `emf.log(...)` / `print(...)` | a line in `emf\emf_core.log` (and the console), prefixed `[id]` |
| `emf.notify(text)` | a note at the top of the screen for a few seconds |
| `emf.hud(id, text [, place [, rgba]])` | a line kept on screen; `""` removes it; place `"top_right"` (below the quest tracker), `"bottom_right"` or `"bottom_left"` (or 0, 1, 2), so a `select` setting's value fits; `rgba` a `color` setting's `"#rrggbb"`, `"#rrggbbaa"` (with alpha: a line can fade), a number like `0xFFD070FF` or the text `"0xFFD070FF"`; 0 (or none) means the usual colour |
| `emf.on_tick(fn)` | `fn(world)` after every world step (server, client) |
| `emf.on_event(name, fn [, "server"/"client"])` | `fn(world, event)` for each engine event `name`, with its fields as a table (see below); call it at the top of `main.lua` |
| `emf.command(name, help, fn [, {players = true}])` | a console command `<id>.<name>` (1.6: a `name` that is exactly your mod's id is the command `<id>` itself); `fn(world, args)` (`args` is a table of words); answer with `emf.reply(text)`. The full name is 1-32 letters, digits, `_ . -` (with `<id>.`) and ignores case; another name is not added: false and a log line, the rest of the mod runs. `{players = true}` (1.4): every player may run it from the game's chat, not only the `[chat] admins` ([server.md](server.md)); use it for commands that read or act on their sender (`emf.sender()`). A reload can add or take off the mark |
| `emf.local_player()` | your player's entity id |
| `emf.sender()` | inside a command: who typed it, `{chat, entity, account, admin}` (`chat` true: another player in the game's chat, `entity` 0 if unknown, `account` a text "0x" + 16 hex digits, `admin` (1.4) true when they may run every command: the console, the host, `[chat] admins`); the host's own chat and the console give `chat` false; nil outside commands |
| `emf.session()` | a number that changes when a world loads (entity ids from an older session mean nothing) |
| `emf.on_world_data(fn)` / `emf.save_world_data(text)` | your data for the world being played: `fn(world, text)` once after the world loaded / save text for it (see "Saving data") |
| `emf.setting(key)` | one of your settings from `mod.json`: a `bool` as true/false, `int` and whole `slider` as an integer, `float` and decimal `slider` as a number, `choice` as its index, `select` as the option's value (text), `color` as `"#rrggbb"`, `keybind` as the key's name (`""`: none), `text` as text; players change them in the Mods tab and EMF.exe |
| `emf.set_setting(key, value)` / `emf.on_setting_changed(fn)` | change one / `fn(key)` after each change |
| `emf.item(name)` / `emf.item_info(id)` | an item's id by its debug name (`items wood` in the console lists names) / `{id, name, max_stack, guid}`; inside callbacks only (they need a world), so look ids up in your first `on_tick` or command |
| `emf.give(world, player, item, count)` | drops items at a player; `item` is an id or a name |
| `emf.put(world, entity, item, count [, fn] [, {level = n, rarity = r}])` | puts items straight into an inventory; `fn(world, added)` one step later. The table (1.4, before or after `fn`): an item level for gear and a rarity, 0-4 or `"common"`, `"uncommon"`, `"rare"`, `"epic"`, `"legendary"` (without them: the game's defaults) |
| `emf.take(world, entity, item, count [, fn] [, {slot = id, exact = true}])` | takes items out (all or nothing); `fn(world, taken)`. The table (1.6): `slot` an InventorySlotId (the inventory entity in the low 32 bits, the slot index 0-7 above: `inv | (i << 32)`) to take from first; with `exact = true` only from that slot (never another inventory, such as worn equipment). Taken items your callback does not hand on (it raises an error first, or the script paused or reloaded before the answer) go back into `entity` |
| `emf.move(world, from, to, item, count [, opts] [, fn])` | (1.6) moves items without losing any: take, put, give back, drop; `opts`: `slot`, `exact`, `fallback`, `level`, `rarity`; `fn(world, moved, returned, dropped, taken)`. true when started (see "Moving items" below) |
| `emf.spawn(world, guid, x, y, z)` / `emf.destroy(world, id)` | spawns a template at a position in metres / removes an entity (host only) |
| `emf.kill(world, id)` | (1.4) kills a creature without a hit: its current health becomes 0 (1.5: through the game's attribute code) and the game's death program sends `EntityDiedEvent` the next step; the corpse keeps its loot. No attacker, no kill credit (`emf.damage` gives one); never a player; host only. true / false |
| `emf.health(world, id)` | (1.5) the current and maximum health: `local cur, max = emf.health(w, id)`; the game's attributes in a world you host, else the replicated `NetworkHealth`; nil when neither is there |
| `emf.attribute(world, id, attr)` | (1.5) an attribute as the game reads it: `attr` is `"health"`, `"max_health"`, `"stamina"`, `"max_stamina"`, `"mana"`, `"max_mana"` or an attribute id; nil when the entity has none. Host only |
| `emf.attribute_add(world, id, attr, delta)` | (1.5) adds `delta` (a whole number) through the game's own attribute code: damage stops at 0, a heal of `"health"`, `"stamina"` or `"mana"` at its maximum (other heals are refused). Not a hit: no kill credit, no hit reaction. Host only. true / false |
| `emf.damage(world, attacker, target, amount)` | (1.5) `amount` of true damage as a real game hit on the next step: a `HitEvent` (damage numbers, always a light reaction, aggro toward the attacker), the killing blow, kill credit and XP for `attacker` when it is a player (0 = no attacker, no aggro). The target must take hits (creatures and players) and not be immune. Host only. true / false |
| `emf.tell(world, id, text)` | (1.6) a chat line only player `id` sees, under their own name (in a game you host, a note for the host itself); `"\n"` splits lines, at most 10. Where players' chat is on: a dedicated server, or `[chat] players=1` in a game you host ([server.md](server.md)). A Lua mod may send 30 lines in 10 s. true when queued, false when refused (the log says why) |
| `emf.broadcast(text)` | (1.6) the same line for every connected player (a note for the host of a game you host). true / false |
| `emf.on_chat(fn)` | (1.6) `fn(world, account, entity, text)` for every chat line a player sends that is not an EMF command, on the server world after its next step; `account` as text ("0x" + 16 hex digits), `entity` 0 when unknown. Read-only: the line is shown as usual. false where players' chat is off |
| `emf.request_save()` | (1.6) asks the game to save the world now (the save its 30 s timer makes). Host or dedicated server. true when asked |
| `emf.teleport(world, id, x, y, z [, opts] [, fn])` | (1.7) moves an entity as fast travel does (see "Teleport" below) |
| `emf.time_of_day([world])` | (1.4) the time of day in hours (`19.5` = 19:30) from the world's clock, or nil when it is not known. Inside callbacks only (it needs a world: the callback's, or the one you pass); at the top of `main.lua` it is an error, as `emf.item` is |
| `emf.session_token()` | (1.4) a text that names this run of the game and its hosted session. Save it with entity ids: `emf.session()` restarts at 1 every time the game starts and the ids are renumbered, so only a matching token means the ids still name the same entities |
| `emf.on_key(key, fn [, "server"/"client"])` | `fn(world)` once per press of `key`: one of your `keybind` settings (the player's choice counts, also when changed while the game runs) or a key name (`"F7"`, `"G"`). It runs after the next step of the server world when your game hosts one, else of your client world (or of the kind given). Keys only count while the game window is in front and the console is closed |
| `emf.key_down(key)` | true while the key (or the key in a `keybind` setting) is held |
| `emf.wheel(title, labels, key, fn)` | the radial wheel; `key` is held to choose (`"G"`, `""` for click); `fn(world, choice)`, choice 1..n or nil |
| `emf.wheel_section(group, title, labels, fn)` | (1.9) your part of a wheel shared by several mods: publishes (replaces) your section of `group`; an empty list or nil removes it. `fn(world, index, label)` gets the index into the labels as they were when the wheel opened, and the label shown: if you published another list since, find the item by `label`. A pick from a wheel opened before your mod reloaded is dropped. Publish again when your labels change |
| `emf.wheel_group(group, title, key)` | (1.9) opens one wheel with every mod's section of `group` (each label as "Section: label" when more than one mod has a section); a pick runs only its owner's `fn`. True when it opened (false: no labels, or more than 64) |
| `emf.emits(names [, kind])` / `emf.emit(kind, name, fields)` | send engine events (see below) |

### Moving items

`emf.move(world, from, to, item, count [, opts] [, fn])` (1.6) moves items without losing any. It takes `count` from
`from` (all or nothing), puts them into `to`, gives back into `from` what does not fit, and drops what still does not fit
at `from`, else at `to`, else at `opts.fallback`, else at the nearest player (`LOST` in the log only when none of them
can take it).

- `opts`: `slot`, `exact` (as for `emf.take`), `fallback`, `level`, `rarity` (as for `emf.put`).
- `fn(world, moved, returned, dropped, taken)` runs at the end.
- It finishes in the Lua host even when your script errors or reloads meanwhile (only `fn` is skipped then), and does
  nothing with an answer from another session.
- At most 24 moves wait at once (false: try again later). It returns true when started.

### Teleport

`emf.teleport(world, id, x, y, z [, opts] [, fn])` (1.7, EMF API v20) moves a player, villager, animal or creature the
way the game's fast travel does. It returns true when queued, false when refused (the log says why).

- `opts`: `ground = true` (the game's ground search: the nearest walkable spot within about 10 m; when there is none
  the entity stands exactly at x, y, z), `yaw = degrees`, `spawn_point = id` (an entity with `PlayerSpawnPoint`: land
  on its pads as a respawn does; x, y, z may then be nil), `checkpoint = true`, `ready = true`.
- `fn(moved, x, y, z, info, world)` runs after the step that moved it, or when it failed. `x, y, z` is where the entity
  is now. `info` is `{status = "moved"/"failed"/"ready", distance, off_target, raw_target, reason, entity}`.
- With `ready = true`, `fn` runs a second time (`info.status == "ready"`) once a player can play again.
- When the session ends first (a reload of the world, back to the menu) or the script was reloaded meanwhile, `fn` is
  not called.

The rules (what is refused, waits, NPCs) are those of `teleport` in [reference.md](reference.md) (v20).

## Worlds and components

Callbacks get a `world` (valid only during that callback):

| Method | |
|---|---|
| `world:is_server()`, `world:is_client()` | which world: gameplay changes go to the server world (the host) |
| `world:tick()` | its tick counter |
| `world:entities()` | every entity id (a table) |
| `world:position(id)` | `x, y, z` in metres, or nil |
| `world:get(component, id)` | the entity's component, or nil |
| `world:template(component, id)` | (1.8) the entity's template half of a component: the data every entity of its template shares (for example `world:template("ModelResource", id).model`, the model GUID as text). Read-only: writing a field is an error. nil when the entity has none |
| `world:name(id)` | (1.6) a player's name, the one the game's chat shows (their platform name, not the character's), or nil (then say `"player #" .. id`); where players' chat is on, server world only |
| `world:account(id)` | a player entity's account as text ("0x" + 16 hex digits, as `emf.sender()` gives it), or nil: a key that stays the same for that player across sessions (entity ids do not); server world only |

`world:get` returns the component's data with its real field names (`sdk/include/emf/generated/components.hpp` lists
every component and its fields: search for `struct CurrentTransform`; `inspect <entity>` in the console shows what an
entity has):

```lua
local t = world:get("CurrentTransform", id)
emf.log(t.transform.position.x)          -- nested structs
local f = world:get("Faction", id)
f.faction = 11                           -- assign to change it (checked: 300 into a byte is an error)
local inv = world:get("Inventory", id)
emf.log(#inv.slots, inv.slots[1])        -- arrays: # and [1..n]
```

Ids, times and hashes (`EntityId`, `Time`, `HashKey32`) read and write as plain numbers. Reading a field the type
does not have is an error naming the type and the field (a typo would otherwise read nil); event tables check the same
way (`rawget(e, name)` reads one without the check). An unsigned 64-bit field at or above 2^63 reads as a float (so it
stays positive; floats hold such numbers only roughly, so a value within about 1024 of 2^64, like an all-ones
marker, reads as 2^64 and cannot be written back); NaN and infinity cannot be written into a float field.

A component can only be used during the callback that got it: keep ids, not components, between callbacks. Writing
changes the game's own data; what the game makes of it (and what it sends to other players) is the same as for a C++
mod: test in a spare world.

## Engine events

`emf.on_event` gives you each event as a table of its fields:

```lua
emf.on_event("EntityDiedEvent", function(world, e)
  emf.log("died:", e.deadEntityId)
end)
```

Common events: `HitEvent` (`rootSourceId` hit `targetId`), `EntityDiedEvent`, `PickupEvent`, `CraftRecipeEvent`,
`LevelUpEvent`, `ConsumeItemEvent` (`components.hpp` has all 129 with their fields, at its end the list `kEvents`); the console's `event_log` mod (C++) logs many of them, and `docs/engine_parts/re_events.md` explains
them. Call `emf.on_event` at the top of `main.lua`: an event that was not asked for when the game started needs a
restart (a reload tells you so; the rest of the mod runs). Event and component names are the short type names
(`HitEvent`); the game's full names (`keen::ecs::HitEvent`) work too. An event whose size in the game is not the size in
`components.hpp` (after a game update) is not delivered; the log says so once.

To make something happen, send the game's own events: declare them at the top of `main.lua`, then send them from any
callback. The host sends them on the next step of that world:

```lua
emf.emits({"NpcCommandEvent"})
emf.command("come", "<villager entity id>: that villager walks to you", function(world, args)
  local x, y, z = world:position(emf.local_player())
  emf.emit("server", "NpcCommandEvent", {
    type = 1, npcId = tonumber(args[1]),  -- 1 = GoToPosition (NpcCommandType in components.hpp)
    targetTransform = { position = { x = math.floor(x * 2^32), y = math.floor(y * 2^32), z = math.floor(z * 2^32) } },
  })
end)
```

(World positions inside the game's structs are fixed point: 1 m = 2^32. `world:position` already converts.)

## Settings

Declare settings in `mod.json` exactly as for C++ mods (see [reference.md](reference.md)): switches, numbers,
sliders with steps, lists, colours, key binds and text, under `group` headings, greyed out with `enabledWhen` while
another setting does not have some value. They show in the console's Mods tab, in EMF.exe and in `settings <id>`. Read
them with `emf.setting(key)`, every time you need them (the template reads them on each tick), and mark the ones you
apply at once `"live": true`; `emf.on_setting_changed` tells you when one changed. A setting added to or changed in
`mod.json` while the game runs shows in the Mods tab within a second (while a world is loaded); until then
`emf.setting` gives false, 0 or "" for a new one, so read settings when you use them rather than once at the top of
`main.lua`.

```lua
-- mod.json: {"key": "toggle_key", "type": "keybind", "default": "F7", "label": "Toggle", "live": true},
--           {"key": "tint", "type": "color", "default": "#ffd070", "live": true}
local on = true
emf.on_key("toggle_key", function(world) on = not on end)
emf.on_tick(function(world)
  if world:is_client() then emf.hud("line", on and "Hello" or "", "bottom_left", emf.setting("tint")) end
end)
```

## Saving data

A Lua mod keeps its variables while the game runs and loses them on reload or restart. Data that belongs to a world
(a score, a list of places) goes into the world's data; data for every world into a setting or a file of your own.

**Per world** (the `kill_counter` example keeps its count this way):

```lua
emf.on_world_data(function(world, text)   -- after the world loaded; "" the first time
  kills = tonumber(text:match("kills=(%d+)")) or 0
end)

-- whenever it changed:
emf.save_world_data("kills=" .. kills)
```

- `fn` runs once per world load, in the game that hosts the world (yours, or the dedicated server), on the server
  world's step, and again after your mod reloads. Registered later (say in a command, after the world loaded), it gets
  the world's current text on the next server step.
- `save_world_data` returns true when saved; false before a world has loaded, in someone else's game (also after you
  hosted a world earlier), and after your mod reloads until `fn` has run again, so nothing is ever written into the
  wrong world's file or over its data. It writes only when the text changed.
- The data is text; choose your own format. It is kept in `emf\config\<your mod id>\<world slot>.txt`, as for C++
  mods (`emf/storage.hpp`).

**Your own files:** write them into your own folder:

```lua
local f = io.open(emf.dir .. "\\scores.txt", "w")
f:write(kills, "\n")
f:close()
```

Use `emf.dir`: a path without a folder lands in the game's folder. File names are UTF-8 text (as `emf.dir` is), so
folders and files with letters such as æ, ø or å work in `io.open`, `require`, `loadfile` and `os.remove`. Give data
files another ending than `.lua`: saving a `.lua` file or `mod.json` reloads the mod.

## Lua or C++?

Lua is the quick way: no build, live reload, and a script error cannot crash the game. C++ (see [README.md](README.md)) reaches everything: mod programs,
hooks into engine functions (`hook_pattern`), template data, memory writes, and it runs faster in tight loops. Both can
live side by side, and a Lua mod can depend on a C++ mod in `mod.json`.
