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

   (`"lua": ">=1.1"` is the Lua host version the mod needs: [Lua versions](#lua-versions) says which version added
   what.)

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

## Lua versions

Each version of the `lua` mod (the Lua host) adds to the one before. Put the newest version whose additions your mod
uses into `depends`: `"lua": ">=1.21"` for `emf.language`. A mod that uses none of them can depend on `">=1.0"`. The
`lua` mod itself needs EMF API v20 (its `mod.json`), so it runs on no older core; the last column names the EMF API
version the additions need (empty: the v20 the `lua` mod needs is enough). Ranges are explained in
[guide.md](guide.md), "Dependencies and version ranges".

| Version | What it adds | EMF API |
|---|---|---|
| 1.0 | the first Lua host: the `emf` table without the additions below | |
| 1.1 | `emf.on_world_data`, `emf.save_world_data`, `world:account` | |
| 1.2 | full type names such as `keen::ecs::HitEvent`; `emf.command` returns true or false | |
| 1.3 | `emf.on_key`, `emf.key_down`; `emf.hud` takes a corner name and a `#rrggbb` colour | |
| 1.4 | `emf.time_of_day`, `emf.kill`, a level and rarity for `emf.put`, commands for every player (`emf.command(..., {players = true})`) | |
| 1.5 | `emf.health`, `emf.attribute`, `emf.attribute_add`, `emf.damage` | v18 |
| 1.6 | `emf.tell`, `emf.broadcast`, `world:name`, `emf.on_chat`, `emf.request_save`; `emf.command(emf.id, ...)` makes the bare command `/<id>` | v19 |
| 1.7 | `emf.teleport` | v20 |
| 1.8 | `world:template` | |
| 1.9 | wheel groups (`emf.wheel_section`, `emf.wheel_group`) | |
| 1.10 | `emf.world_settings`, `emf.on_world_setting_changed` | v21 |
| 1.11 | a player's XP and level: `emf.xp_required`, `emf.level_cap`, `emf.set_level`, the attribute names `"xp_pending"`, `"xp"`, `"xp_required"`, `"level"` | v22 |
| 1.12 | `emf.add_component`, `emf.remove_component`, components spawned with `emf.spawn` | v23 |
| 1.13 | mod windows (`emf.window`, `emf.window_update`, `emf.window_close`) | v24 |
| 1.14 | E prompts (`emf.offer`, `emf.spawn_offer`, `emf.offer_remove`) | v25 |
| 1.15 | pictures in windows (`emf.image`, the `image`, `icon`, `fps` and `height` row fields) | v26 |
| 1.16 | never used: the version went from 1.15 to 1.17 | |
| 1.17 | the message bus between mods (`emf.bus.subscribe`, `.publish`, `.unsubscribe`) | |
| 1.18 | `emf.json` (tables to JSON text and back, for messages on the bus) | |
| 1.19 | mod messages (`emf.net`) | v27 |
| 1.20 | player events (`emf.on_player_joined`, `_left`, `_died`) | v28 |
| 1.21 | the game's item names and language (`emf.item_name`, `emf.language`) | v29 |
| 1.22 | `world:character_name`, the `{system = true}` option of `emf.tell` and `emf.broadcast` | v30 |
| 1.23 | hits (`emf.on_hit`, `emf.hit_rule`, `emf.hit_rule_remove`) | v31 |
| 1.24 | buffs (`emf.buff_types`, `emf.buff_type`, `emf.buffs`, `emf.apply_buff`, `emf.remove_buff`, `emf.on_buff`) | v32 |
| 1.25 | markers (`emf.marker`, `emf.send_marker` and more) | v33 |
| 1.26 | the map (`emf.map_cursor`, `emf.on_map_click`, `emf.map_action`, `emf.remove_map_action`, `emf.ground_at`) | v34 |
| 1.27 | canvases and a minimap's parts (`emf.player_view`, `emf.fog_at`, `emf.map_markers`, `emf.canvas`, `emf.canvas_draw`, the marker key `minimap`, a `fog` argument for `emf.ground_at`) | v35 |
| 1.28 | `emf.remove_building` | v36 |
| 1.29 | `emf.path_find`, `emf.path_cancel` | v37 |
| 1.30 | `emf.door`, `emf.door_open`, `emf.door_close`, `emf.door_lock` | v38 |
| 1.31 | a `water_y` argument for `emf.ground_at` | v34, from EMF 0.6.8 |
| 1.32 | enemy orders (`emf.enemy_order`, `emf.enemy_order_cancel`, `emf.enemy_target`) | v39 |
| 1.33 | move orders (`emf.enemy_move`, `emf.enemy_move_cancel`, `emf.enemy_moving`) | v40 |
| 1.34 | the move options `stuck` and `peaceful = "all"` | v41, from EMF 0.6.10 |
| 1.35 | `door` on a path's door points | v42, from EMF 0.6.11 |
| 1.36 | `world:permissions` | v43 |
| 1.37 | `emf.break_blocks`, `emf.block_at`, `emf.find_wall` | v44 |
| 1.38 | the world's own blocks (ruins) spared: a block's `world`, the break option `world` | v45 |

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
| `emf.hud(id, text [, place [, rgba]])` | a line kept on screen; `""` removes it; place `"top_right"` (below the quest tracker), `"bottom_right"` or `"bottom_left"` (or 0, 1, 2), so a `select` setting's value fits; `rgba` a `color` setting's `"#rrggbb"`, `"#rrggbbaa"` (with alpha: a line can fade), a number like `0xFFD070FF` or the text `"0xFFD070FF"`; 0 (or none) means the usual colour; the player can move, scale and hide each place in the HUD editor |
| `emf.on_tick(fn)` | `fn(world)` after every world step (server, client). In a game you host, a client step leaves your mod out while it is still busy in the server step (at most 30 steps in a row), so the game's frame seldom waits for your server work (it still does when your mod has a client bus callback with messages due, a client `on_event`, or after 30 steps); keep heavy work in the server world |
| `emf.bus.subscribe(topic, fn [, world])` | (1.17) listen for a topic in `"server"` (default) or `"client"` world; `fn(world, source_mod_id, payload)` runs on that world's next step when a publication arrives; true when registered |
| `emf.bus.publish(topic, payload [, world])` | (1.17) queue a copy of a text/binary payload for `"server"` (default) or `"client"`; true when queued, false if the topic, payload or queue limit is invalid (logged once per rejection streak) |
| `emf.bus.unsubscribe(topic)` | (1.17) remove all this mod's callbacks for a topic in both worlds; true if any were removed, false if none existed |
| `emf.json.encode(value)` | (1.18) a Lua value as JSON text; nil plus a message when it cannot be written (see "JSON (1.18)") |
| `emf.json.decode(text)` | (1.18) JSON text as a Lua value; nil plus a message when the text is not valid JSON |
| `emf.json.null` | (1.18) the value `decode` gives for JSON `null` and `encode` writes as `null` (a Lua table cannot hold nil) |
| `emf.on_event(name, fn [, "server"/"client"])` | `fn(world, event)` for each engine event `name`, with its fields as a table (see below); call it at the top of `main.lua` |
| `emf.command(name, help, fn [, {players = true}])` | a console command `<id>.<name>` (1.6: a `name` that is exactly your mod's id is the command `<id>` itself); `fn(world, args)` (`args` is a table of words); answer with `emf.reply(text)`. The full name is 1-32 letters, digits, `_ . -` (with `<id>.`) and ignores case; another name is not added: false and a log line, the rest of the mod runs. `{players = true}` (1.4): every player may run it from the game's chat, not only the `[chat] admins` ([server.md](server.md)); use it for commands that read or act on their sender (`emf.sender()`). A reload can add or take off the mark. Write `help` as `<args>: what it does` (`"<name>: call a beast"`, `": your life skills"`): the console shows it as players type and offers `a\|b` choices and item names for an `<item>` argument |
| `emf.local_player()` | your player's entity id |
| `emf.sender()` | inside a command: who typed it, `{chat, entity, account, admin}` (`chat` true: another player in the game's chat, `entity` 0 if unknown, `account` a text "0x" + 16 hex digits, `admin` (1.4) true when they may run every command: the console, the host, `[chat] admins`, the game's admins with `[chat] game_admins=1`); the host's own chat and the console give `chat` false; nil outside commands |
| `emf.session()` | a number that changes when a world loads (entity ids from an older session mean nothing) |
| `emf.on_world_data(fn)` / `emf.save_world_data(text)` / `emf.player_data(a)` / `emf.save_player_data(a, text)` | your data for the world being played: `fn(world, text)` once after the world loaded / save text for it; (1.20) a text per player (see "Saving data") |
| `emf.setting(key)` | one of your settings from `mod.json`: a `bool` as true/false, `int` and whole `slider` as an integer, `float` and decimal `slider` as a number, `choice` as its index, `select` as the option's value (text), `color` as `"#rrggbb"`, `keybind` as the key's name (`""`: none), `text` as text; players change them in the Mods tab and EMF.exe |
| `emf.set_setting(key, value)` / `emf.on_setting_changed(fn)` | change one / `fn(key)` after each change |
| `emf.item(name)` / `emf.item_info(id)` | an item's id by its debug name (`items wood` in the console lists names) / `{id, name, max_stack, guid}`; inside callbacks only (they need a world), so look ids up in your first `on_tick` or command |
| `emf.item_name(item [, "game"])` / `emf.language()` | (1.21) the name the game shows for an item (an id or a debug name), in the player's language: `name, true`; `debug_name, false` where the game has none (the dedicated server, before its text is known, an older core); `nil, false` for an unknown item. For EMF's screens (windows, wheels, HUD lines) a name EMF's fonts cannot draw comes in English (Japanese, Korean, Chinese and Thai until EMF found a font for them, README_EMF.md); `"game"` gives the player's language always (for the chat). Inside callbacks only / `"en_us"`, `"de_de"`, ... or nil (reference.md "v29"); the `kill_counter` example shows its line in the player's language with it |
| `emf.give(world, player, item, count)` | drops items at a player; `item` is an id or a name |
| `emf.put(world, entity, item, count [, fn] [, {level = n, rarity = r}])` | puts items straight into an inventory; `fn(world, added)` one step later. The table (1.4, before or after `fn`): an item level for gear and a rarity, 0-4 or `"common"`, `"uncommon"`, `"rare"`, `"epic"`, `"legendary"` (without them: the game's defaults) |
| `emf.take(world, entity, item, count [, fn] [, {slot = id, exact = true}])` | takes items out (all or nothing); `fn(world, taken)`. The table (1.6): `slot` an InventorySlotId (the inventory entity in the low 32 bits, the slot index 0-7 above: `inv | (i << 32)`) to take from first; with `exact = true` only from that slot (never another inventory, such as worn equipment). Taken items your callback does not hand on (it raises an error first, or the script paused or reloaded before the answer) go back into `entity` |
| `emf.move(world, from, to, item, count [, opts] [, fn])` | (1.6) moves items without losing any: take, put, give back, drop; `opts`: `slot`, `exact`, `fallback`, `level`, `rarity`; `fn(world, moved, returned, dropped, taken)`. true when started (see "Moving items" below) |
| `emf.spawn(world, guid, x, y, z [, components])` / `emf.destroy(world, id)` | spawns a template at a position in metres / removes an entity (host only). 1.12: `components` = `{Name = {field = value}, ...}` are spawned with the entity (at most 15, by their real field names, absent fields 0; not `CurrentTransform`): `emf.spawn(w, guid, x, y, z, {ScaleComponent = {uniformScale = 1.25}})` makes a creature drawn 1.25x as big (a `ScaleComponent` from 0.1 to 10). An unknown name or field is an error. On a placed object `destroy` gives no item back, drops no loot and plays no pick-up sound; a container's contents go with it. Never destroy a Flame Altar (the base). For a pick-up use `emf.remove_building` (1.28); for loot use `emf.damage` on a `Destructible` object |
| `emf.remove_building(world, id [, opts])` | 1.28: removes a placed object like a pick-up, item back; `true, item_id` or `false, reason`; options below |
| `emf.path_find(world, from, to [, opts], fn)` | 1.29: asks the game's path grid for a path, `fn(world, result, points)` comes later; id or nil; options below |
| `emf.path_cancel(id)` | 1.29: cancels an open path query; true when it was open |
| `emf.door(world, id)` | 1.30: `"open"` or `"closed"` plus `{locked = bool, logic = bool}`, or nil when not a door (see "Doors" below) |
| `emf.door_open(world, id [, "front" \| "back"])` / `emf.door_close(world, id)` | 1.30: opens / closes a door at the next step; true when it will change, false when refused |
| `emf.door_lock(world, id, locked)` | 1.30: no E prompt on the door while locked (kept with the world from EMF 0.6.8); true / false |
| `emf.enemy_order(world, enemy, target [, opts])` | 1.32: the enemy goes for the target (a door, a gate, a creature, a player); the order's id, or nil and why (see "Enemy orders" below) |
| `emf.enemy_order_cancel(id)` / `emf.enemy_target(world, enemy)` | 1.32: end an order / what the enemy goes for now (or nil) |
| `emf.enemy_move(world, enemy, points [, opts] [, fn])` | 1.33: the creature walks to a point or through a list of points; the move's id, or nil and why (see "Move orders" below) |
| `emf.enemy_move_cancel(id)` / `emf.enemy_moving(world, enemy)` | 1.33: end a move / its move's id now (or nil) |
| `emf.break_blocks(world, x, y, z [, opts], fn)` | 1.37: damages or breaks the player-built blocks around a point, never the ground and (1.38) never the world's own ruins; `fn(world, result)` after the step; id or nil (see "Breaking blocks" below) |
| `emf.block_at(world, x, y, z)` / `emf.find_wall(world, from, to)` | 1.37: what is at a point / the first building block on a line, as a table, or nil |
| `emf.kill(world, id)` | (1.4) kills a creature without a hit: its current health becomes 0 (1.5: through the game's attribute code) and the game's death program sends `EntityDiedEvent` the next step; the corpse keeps its loot. No attacker, no kill credit (`emf.damage` gives one); never a player; host only. true / false |
| `emf.health(world, id)` | (1.5) the current and maximum health: `local cur, max = emf.health(w, id)`; the game's attributes in a world you host, else the replicated `NetworkHealth`; nil when neither is there |
| `emf.attribute(world, id, attr)` | (1.5) an attribute as the game reads it: `attr` is `"health"`, `"max_health"`, `"stamina"`, `"max_stamina"`, `"mana"`, `"max_mana"`, a player's `"xp_pending"`, `"xp"`, `"xp_required"` and `"level"` (1.11, EMF API v22), or an attribute id; nil when the entity has none. Give XP with `emf.attribute_add(w, player, "xp_pending", n)`: the game levels the player up itself (the other three are read-only). Host only |
| `emf.attribute_add(world, id, attr, delta)` | (1.5) adds `delta` (a whole number) through the game's own attribute code: damage stops at 0, a heal of `"health"`, `"stamina"` or `"mana"` at its maximum (other heals are refused). Not a hit: no kill credit, no hit reaction. Host only. true / false |
| `emf.damage(world, attacker, target, amount)` | (1.5) `amount` of true damage as a real game hit on the next step: a `HitEvent` (damage numbers, always a light reaction, aggro toward the attacker), the killing blow, kill credit and XP for `attacker` when it is a player (0 = no attacker, no aggro). The target must take hits (creatures and players, and since 1.23 placed objects: see "Hits" below) and not be immune. Host only. true / false |
| `emf.xp_required(world, level)` | (1.11) the XP `level` needs before the next one (the game's own curve); nil outside 1..cap or when unknown. Host only |
| `emf.level_cap(world)` | (1.11) the highest level a player can reach; nil when unknown. Host only |
| `emf.set_level(world, player, level [, xp])` | (1.11) sets a player's level (1..cap) with `xp` (default 0) towards the next one: raises or lowers at once, without level-up events (to level up the game's way, give XP with `emf.attribute_add(w, player, "xp_pending", n)`). The skill points follow the level; after lowering, a skill tree that costs more points than the level gives is reset by the game at the next load. Saved with the character. Host only. true (done or queued) / false |
| `emf.add_component(world, id, name [, data] [, opts] [, fn])` | (1.12) adds component `name` to entity `id` through the game's own command, with `data` = `{field = value}` by the real field names (absent fields are 0; pass `nil` for `data` to give `opts` or `fn` without it). In this version only `ScaleComponent` (`uniformScale` from 0.1 to 10) and `InteractionPause`; the components the world save keeps are refused until checked in game ([reference.md](reference.md), "v23"). A `ScaleComponent` added this way is not drawn (the game reads the size when it makes the creature): spawn with it instead (`emf.spawn`'s `components`). Only when absent: one already there is never overwritten. Never a player. Applied two server steps later; `fn(world, status, why)` then runs once: `status` is `"done"`, `"already"` or `"failed"` (not called when the world ends first). An unknown name or field is an error. Host only. true (queued) / false (refused: see the log; `fn` is not called) |
| `emf.remove_component(world, id, name [, opts] [, fn])` | (1.12) removes component `name` from entity `id`: one this mod added in this session (`ScaleComponent`, `InteractionPause`). `fn` as above (`"already"`: it was not there). Host only. true / false |
| `emf.tell(world, id, text [, opts])` | (1.6) a chat line only player `id` sees, under their own name (in a game you host, a note for the host itself); (1.22) `opts` `{system = true}`: shown with no name, like the game's "joined" lines, also with text chat off (a label only as a prefix: `"[Raid] ..."`; an older core shows it under the player's name); `"\n"` splits lines, at most 10. Where players' chat is on: a dedicated server, or `[chat] players=1` in a game you host ([server.md](server.md)). A Lua mod may send 30 lines in 10 s. true when queued, false when refused (the log says why) |
| `emf.broadcast(text [, opts])` | (1.6) the same line for every connected player (a note for the host of a game you host); (1.22) `opts` as `emf.tell`. true / false |
| `emf.on_chat(fn)` | (1.6) `fn(world, account, entity, text)` for every chat line a player sends that is not an EMF command, on the server world after its next step; `account` as text ("0x" + 16 hex digits), `entity` 0 when unknown. Read-only: the line is shown as usual. false where players' chat is off |
| `emf.request_save()` | (1.6) asks the game to save the world now (the save its 30 s timer makes). Host or dedicated server. true when asked |
| `emf.teleport(world, id, x, y, z [, opts] [, fn])` | (1.7) moves an entity as fast travel does (see "Teleport" below) |
| `emf.world_settings.get(key)`, `.set(key, value [, persist])`, `.reset([key] [, persist])`, `.list()` | (1.10) the running world's settings: day length, loot, XP, enemy damage... (see "World settings" below). Host or dedicated server |
| `emf.on_world_setting_changed(fn)` | (1.10) `fn(key, value)` once per world setting the server wrote, whoever changed it, on the server world after that step. false on an older core |
| `emf.time_of_day([world])` | (1.4) the time of day in hours (`19.5` = 19:30) from the world's clock, or nil when it is not known. Inside callbacks only (it needs a world: the callback's, or the one you pass); at the top of `main.lua` it is an error, as `emf.item` is |
| `emf.session_token()` | (1.4) a text that names this run of the game and its hosted session. Save it with entity ids: `emf.session()` restarts at 1 every time the game starts and the ids are renumbered, so only a matching token means the ids still name the same entities |
| `emf.on_key(key, fn [, "server"/"client"])` | `fn(world)` once per press of `key`: one of your `keybind` settings (the player's choice counts, also when changed while the game runs) or a key name (`"F7"`, `"G"`). It runs after the next step of the server world when your game hosts one, else of your client world (or of the kind given). Keys only count while the game window is in front and the console is closed |
| `emf.key_down(key)` | true while the key (or the key in a `keybind` setting) is held |
| `emf.wheel(title, labels, key, fn)` | the radial wheel; `key` is held to choose (`"G"`, `""` for click); `fn(world, choice)`, choice 1..n or nil |
| `emf.wheel_section(group, title, labels, fn)` | (1.9) your part of a wheel shared by several mods: publishes (replaces) your section of `group`; an empty list or nil removes it. `fn(world, index, label)` gets the index into the labels as they were when the wheel opened, and the label shown: if you published another list since, find the item by `label`. A pick from a wheel opened before your mod reloaded is dropped. Publish again when your labels change |
| `emf.wheel_group(group, title, key)` | (1.9) opens one wheel with every mod's section of `group` (each label as "Section: label" when more than one mod has a section); a pick runs only its owner's `fn`. True when it opened (false: no labels, or more than 64) |
| `emf.on_hit(fn [, {kinds = ...}])` | (1.23) `fn(world, hit)` after the server applied each landing hit on an entity (the game's `HitEvent`), on the server world; with `kinds` also fall, shroud, cold and drowning damage; see "Hits" below. Joined players' games see none. true / false (false on an older core) |
| `emf.hit_rule{from, to, crit, backstab, weakspot, types, attack, healing, kinds, scale, add, min_health, cancel}` | (1.23) a rule the core applies to every matching hit before it lands: change its amount or cancel it. Returns its id, or nil when refused (the log says why: 64 rules, the hook not usable on this game build). See "Hits" below |
| `emf.hit_rule_remove(id)` | (1.23) removes a rule of this script. true only for the script's own rule |
| `emf.buff_types(world [, slot])` | (1.24) the game's buff types as a list of type tables; `slot` is `"food"`, `"buff"`, `"debuff"`, `"rested"` or `"terrain"`. Any world. See "Buffs" below |
| `emf.buff_type(world, id)` | (1.24) one type table, or nil |
| `emf.buffs(world, entity)` | (1.24) the entity's active buffs as a list of tables, in slot order. Any world |
| `emf.apply_buff(world, target, type [, {seconds, source, item}] [, fn(world, result)])` | (1.24) gives a buff through the game's own buff code in the next step; `fn` gets the result after it. Server world only. true when queued, false when refused at once (logged) |
| `emf.remove_buff(world, target, type [, fn(world, result)])` | (1.24) removes a buff the game's way (a cleanse: gone after the next step). Server world only. true / false as `emf.apply_buff` |
| `emf.on_buff(fn)` | (1.24) `fn(world, ev)` after the server step for every change of a player's buffs: applied, refreshed, removed. false on an older core |
| `emf.marker(id, spec)` / `emf.remove_marker(id)` / `emf.clear_markers()` | (1.25) a marker over the 3D view and on the map on this game. See "Markers" below |
| `emf.send_marker(player, id, spec)` / `emf.unsend_marker(player, id)` | (1.25) server: a marker on a player's game (nil: every player's); players need EMF |
| `emf.world_to_screen(x, y, z)` / `emf.marker_types()` | (1.25) a world point in display pixels (or nil) / the game's map icon types seen in this world |
| `emf.map_cursor()` / `emf.on_map_click(fn)` | (1.26) the world spot under the mouse on the open map (or nil) / a function for clicks on the map. See "The map" below |
| `emf.map_action(id, opts, fn)` / `emf.remove_map_action(id)` | (1.26) server: something players do on their map by holding Shift and the left mouse button; players need EMF |
| `emf.ground_at(world, x, z, fn)` | (1.26) server world: the ground height under a spot, answered a moment later |
| `emf.player_view()` / `emf.fog_at(x, z)` / `emf.map_markers()` | (1.27) the local player's position and heading / the fog of war at a point / the game's own map markers. See "Canvases and the minimap" below |
| `emf.canvas(id, spec)` / `emf.canvas_draw(id, list)` | (1.27) a drawing panel EMF draws over the HUD / its drawing |
| `emf.on_player_joined(fn)`, `emf.on_player_left(fn)`, `emf.on_player_died(fn)` | (1.20) `fn(world, p)` on the server world, `p` = `{player, account, name, host, killer}` (`killer`: died only, the last hitter within 10 s or 0). All join 1 s after the world data; died needs the call in the first run. False on an older core (reference.md "v28") |
| `emf.net.channel(name, def)` | (1.19) a message channel `"<mod id>/<name>"`: `def` = `{version, to = "server" \| "players" \| "both", max, per_10s, on_server = fn(world, from, data), on_player = fn(world, from, data)}`; `from` = `{player, account, admin, is_local, version}`. A reload binds the new functions; a new version needs a restart. In process, and to joined players and the server over the chat (reference.md "v27") |
| `emf.net.send(name, data)`, `.send_to(world, player, name, data)`, `.send_all(world, name, data)`, `.peer(world, player, name)` | (1.19) to the server (true: sent) / to one player's game / to all with the channel (count) / that game's channel version (`peer(nil, 0, name)`: the server's; 0 = none) |
| `emf.offered(world, id)` | (1.15) true while your script's offer on `id` is in EMF's hands (not removed, not dropped by EMF); after a drop, `emf.offer` again takes it back |
| `emf.image(path)` | (1.15) a picture for windows: a .png or a folder of .png frames inside your mod's folder (`"pics/wolf.png"`, `"pics/wolf_turn"`), or `"portrait:<key>"` (EMF's portrait cache); the handle for a row's `image` or `icon`, or 0 (refused: logged; a core before v26), which a row takes as no picture and keeps its place. A path that leaves your mod's folder is an error. reference.md "v26" |
| `emf.window(id, def, fn)` | (1.13) opens a window EMF's overlay draws (one at a time; it takes the mouse and keyboard; Esc, its `close_key` or its X close it); true when it opened. `id` gets your mod's id in front (`"stable"` is `"beastmaster.stable"`). `def`: `title`, `width` (in text heights, about 28 by default), `place` (`"centre"`, `"left"`, `"right"`), `close_key` (a key name, for example `emf.setting("stable_key")`), `world` (`"server"` when you host, else `"client"`: where `fn` runs), `rows` = a list, top to bottom, each one of `{text = ...}`, `{heading = ...}`, `{button = key, text = ...}`, `{row = key, text = "Name (detail)"}`, `{slot = key, text = ..., count = n}`, `{bar = text, value = 0..1}`, `{separator = true}`, `{image = emf.image(...), height = 6, fps = 12, key = ...}` (1.15), with `icon = emf.image(...)` and `fps` on slots and rows (1.15), and any of `same_line`, `selected`, `disabled`, `dim`, `empty`, `locked` (true), `tooltip` and `color` (`"#rrggbb"`). `fn(world, ev)`: `ev.kind` is `"click"`, `"right_click"` or, last, `"closed"` (with `ev.reason` `"player"`, `"mod"` or `"core"`); `ev.key` is the clicked row's key. A reload closes the script's window. An unknown row kind or field is an error. false (logged) while another window or a wheel is open, with no world of its kind, without the overlay (a dedicated server). `close_key = ""` (a key setting left unset) is no close key |
| `emf.window_update(id, def)` | (1.13) new contents for your open window (`fn` stays); false when it is not open (the player closed it): it never reopens one |
| `emf.window_close(id)` | (1.13) closes your window (`fn` gets `"closed"`, reason `"mod"`); false when it is not open |
| `emf.offer(world, id [, opts] [, fn])` | (1.14) the game's own "E <verb>" prompt on entity `id` of the server world you host; `fn(world, player, entity)` runs when any player presses E there. `opts`: `verb` (`"Use"` (default), `"Open"`, `"Talk"`, `"Read"`, `"Sit"`, `"Sleep"`, `"Pet"`, `"Touch"`, `"Activate"`, `"Press"`, `"Enter"`, `"Inspect"`, `"Take"`, `"Collect"`, `"Rest"`, `"Release"` or a loca id), `text` (your own words, shown once a content mod adds them with `add_prompt_words`, content.md; else "E" with no word), `sequence` (`"touch"`, `"button"`), `range`, `low_priority`, `build_mode`. false for a player or an entity with the game's or another mod's offer. Offer again after a load; a reload takes the script's offers off. Host only (reference.md "v25") |
| `emf.spawn_offer(world, guid, x, y, z [, opts] [, fn])` | (1.14) spawns a template with an offer on it (`emf.offer`'s options); the entity id, or nil. The prop may stay in the save without its offer: after a load offer it again, don't spawn another |
| `emf.offer_remove(world, id)` | (1.14) takes your script's offer off the entity; false when it has none of yours |
| `emf.emits(names [, kind])` / `emf.emit(kind, name, fields)` | send engine events (see below) |

### Messages between Lua mods (1.17)

The `emf.bus` table lets separate Lua mods exchange **copies** of messages without sharing their Lua state.
This works on dedicated servers as well as in a hosted game. It does not provide network transport between the
server and clients: `"server"` and `"client"` choose which local world step delivers a message. The default is
`"server"`. A bus-using mod needs `"depends": {"lua": ">=1.17"}` in `mod.json`.

```lua
-- In a second Lua mod (the receiver):
emf.bus.subscribe("example.event", function(world, sender, payload)
  emf.log("received from", sender, payload)
end) -- defaults to the server world

-- In the sender mod:
local ok = emf.bus.publish("example.event", "example-payload")
if not ok then emf.log("message was refused") end
```

`topic` is 1-96 **bytes** (ASCII control characters, including NUL, are refused); `payload` is a Lua string of up to **65,536
bytes** and may contain `\0`. A topic may have several callbacks in one mod. Calling `unsubscribe(topic)` removes
all of them, including server and client callbacks. The callback's `sender` is the source mod's id, not a player id.

Each world delivers accepted messages in **publish order**, on its **next world step**. A publication made *inside*
a bus callback is held for the *following* step, never delivered recursively in the same step. A mod **does receive
its own messages** if subscribed to that topic. During the initial load, a message published by one mod's `main.lua`
can reach another mod even if that receiver's `main.lua` is loaded later, before the first world step. Subscriptions
added at runtime do not consume older publications.

Each publisher may have at most **256** pending messages; all publishers together may have at most **4,096**.
A rejected publication returns `false` and EMF logs the reason once until that mod next publishes successfully.
Messages are held in memory, not saved to disk. On reload, stop or Lua error, a mod's subscriptions
and its pending outgoing messages are cleared. New subscriptions cannot receive messages queued for the previous
load; messages still intended for other subscribed mods are preserved. Handle a refusal by retrying later or
coalescing updates rather than publishing in a tight loop.

### Services on the bus

One mod can offer a *service* that other mods call with request and reply messages (a world database, navigation, ...).
[services.md](services.md) is the short guide: topic names, the JSON envelope, request ids, timeouts, announcing a
service, and the examples `bus_service` and `bus_client`.

### JSON (1.18)

Bus payloads are strings, so a mod that sends a table turns it into text first. `emf.json` does that the same way
for every mod. It needs `"depends": {"lua": ">=1.18"}`.

```lua
local text = emf.json.encode({op = "get", args = {key = "apple"}, tags = {"a", "b"}})
-- {"args":{"key":"apple"},"op":"get","tags":["a","b"]}
local value, err = emf.json.decode(text)
if not value then emf.log("bad json:", err) end
```

Bad input never raises a Lua error: `encode` and `decode` give `nil` and a message, so check the result (a call with
a missing or wrong-typed argument is still a usage error, as for other `emf` functions).

- **Tables**: keys exactly `1..n` (n at least 1) make a JSON array. Any other table is an object and all its keys must
  be strings; a table with other keys (`{1, 2, x = 3}`, a sparse list, a number or table as key) is an error that names
  the problem. An empty table is `{}`, so an empty list decodes and encodes as an object: test `next(t) == nil`.
  Object keys are written in sorted order, so the same table always gives the same text.
- **Numbers**: a Lua integer is written as an integer. A float is written in the shortest form that reads back as the
  same number, and a whole float keeps a `.0` (`3.0`), so it stays a float. NaN and infinity are errors. When reading,
  a number without a fraction or exponent that fits 64 bits is an integer, any other number a float.
- **Null**: `emf.json.null` stands for JSON `null` (in either direction). Compare with `==`. A nil value in a table
  does not exist in Lua, so use `emf.json.null` when "no value" must be sent.
- **Strings** are UTF-8 and are checked: a string with invalid UTF-8 is an error in both directions. Quotes,
  backslashes and control characters are escaped; `\uXXXX` escapes, including surrogate pairs, are decoded; a lone
  surrogate is an error. Other text (`/`, accents, emoji) is written as it is.
- **Also**: `emf.json.null` is truthy (`if value then` is true for null: compare with `== emf.json.null`). Metatables
  are ignored (`__index`, `__pairs` have no effect; only the table's own entries are written). A repeated key in
  the text keeps its last value. A UTF-8 byte order mark at the start of the text is an error. `-0` decodes as the
  integer `0`. `decode` accepts a number as its argument (Lua converts it to text: `decode(5)` is `5`); other
  non-text arguments are a usage error.
- **Booleans** as usual. Functions, threads, userdata (except `emf.json.null`) and `nil` are errors.
- **Limits**: 64 levels of nesting (this also stops a table that contains itself), 1 MiB of input text, 1 MiB of
  output text. The bus carries at most 64 KiB, so encode first and check that `emf.bus.publish` returned true.

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

### World settings

`emf.world_settings` (1.10, EMF API v21) reads and changes the running world's settings, the values of
`enshrouded_server.json`'s `"gameSettings"`. Only where a world is hosted (the host or a dedicated server).

- `get(key)`: the value now, or nil. A key is the setting's name as `enshrouded_server.json` has it
  (`"playerStaminaFactor"`) or its short name (`"loot"`, `"day_minutes"`); durations are in minutes, choices by
  number, on/off as 1/0.
- `set(key, value [, persist])`: true when queued for the next server step (every player gets it); `false, why` when
  refused (`"loot: allowed 0.25 to 2"`, `"unknown setting: x"`, `"no world is hosted here"`). `persist = true`
  keeps it for this world (applied again after each load). A boolean value counts as 1 or 0.
- `reset([key] [, persist])`: back to the world's own value; no key: every setting. `persist` also forgets the kept
  value (`reset(true)` for all).
- `list()`: `{{key, alias, group, type, min, max, value, world, changed, needs_client}, ...}`, empty without a hosted
  world. `type` is `"float"`, `"bool"`, `"enum"`, `"minutes"` or `"whole"`; `world` is the world's own value.

The settings and their ranges are in [world_settings.md](world_settings.md). A mod that changes them should say so
in its README: players expect the world to keep the settings they chose.

## Hits (1.23)

`emf.on_hit(fn)` sees each hit the server world applies to an entity, after it landed. `emf.hit_rule{...}` changes
hits before they land, with no script call in between (a script never runs before a hit lands: the script lock is shared
by the world threads, and a call per hit would slow fights). Both work on the server world only (a game you host or the
dedicated server; see [server.md](server.md)).

`fn(world, hit)` gets a table:

| Field | What |
|---|---|
| `attacker`, `source` | the root source (the shooter of an arrow, the caster of a spell; 0 = none) and the direct source (a projectile, a trap) |
| `target`, `part` | the actor hit and the entity hit (the same for most hits) |
| `amount` | health damage; negative for healing. A blocked or parried hit has 0 |
| `stamina` | stamina damage of a blocked hit |
| `crit`, `blocked`, `parried`, `backstab`, `weakspot`, `healing`, `killed` | booleans (`killed`: the killing blow; for fall, shroud, cold and drowning: it left no health) |
| `types` | list of damage type names: `"cut"`, `"pierce"`, `"blunt"`, `"fire"`, `"ice"`, `"shroud"`, `"lightning"`, `"poison"`, `"wood"`, `"stone"`, `"metal"`, `"explosion"` |
| `attack`, `attacks` | the attack's kind: `"melee"`, `"ranged"`, `"magic"`, `"tool"`, `"other"`, `"aoe"` or `"skillshot"` (nil for a `emf.damage` hit); `attacks` lists every kind the hit has (most have one) |
| `weapon_item`, `weapon_category`, `program`, `impact`, `reaction` | the weapon's item id (0 = none or not known), the game's weapon category hash, the attack's impact program, the swing or projectile (hits of one share it), the hit reaction type |
| `health_before`, `health_after` | the target's health around the hit; nil when not known |
| `kind` | `"hit"` (an entity hit), `"fall"`, `"shroud"`, `"cold"` or `"drowning"`: see "Fall, shroud, cold and drowning" below |
| `instant` | true when the game set health to 0 at once: deadly shroud, or a creature (not a player) out of air |
| `from_mod` | true for a hit made by `emf.damage` |
| `position` | `{x = , y = , z = }` in metres |

A rule takes: `from` and `to` (an entity id, or `"players"`, `"creatures"`, `"objects"`; absent = anyone; an entity id
as `to` matches the actor hit and the part hit), `crit`, `backstab`, `weakspot` (true = only such hits), `types` (a
type name or a list of names, any of them), `attack` (a name or a list of names), `healing` (true = healing hits only; without it
a rule matches damage hits only, so "no PvP" does not stop a friend's heal), then what it does: `cancel` (the hit is
dropped: no damage, number, reaction, aggro, XP credit or destruction), `scale` (amount times scale; a blocked hit's
stamina too), `add` (added after the scale), `min_health` (the target keeps at least that much health: 1 = it cannot
die from hits). A damage hit never becomes a heal and a heal never becomes damage. Rules apply in id order; cancel
wins. An unknown field or a bad name is an error. A script's rules are removed when it reloads, unloads or pauses.

### Fall, shroud, cold and drowning

`kinds` lets a rule or a callback take part in the damage the game does not make as hits. A kind name is `"hit"`,
`"fall"`, `"shroud"`, `"cold"` or `"drowning"`; `kinds` is one name, a list of names, or `"all"`. An unknown name is an
error.

- `emf.hit_rule{kinds = "fall", to = "players", cancel = true}`. Without `kinds` a rule is for entity hits only, so every
  rule keeps its meaning.
- `emf.on_hit(fn, {kinds = {"fall", "drowning"}})`. Without the option `fn` gets entity hits only: a damage meter does
  not get 60 shroud ticks a second unasked.
- `hit.kind` tells them apart. For these kinds `attacker` and `source` are 0, `target` = `part` = the entity, there are no
  `types` or `attack`, and `health_before` and `health_after` are known. `from` (a player or creature) never matches
  them, nor `crit`, `types` or `attack`.
- Fall is once per landing. Shroud, cold and drowning come every server step (about 60 a second) while the shroud timer is
  out, body heat is 0 while freezing, or oxygen is 0. `scale` is exact there (0.5 halves 1-point ticks: EMF carries the
  fraction). `add` is per event, so about 60 times a second on those three.
- `min_health` works on every kind. A cancelled hit never reaches `on_hit` (entity hits too); a cancel gives back no mana or
  stamina the attacker spent. `add` on a healing rule is added to a negative amount: a positive `add` makes the heal smaller. A cancelled fall gives the health back, but the
  game's hard-landing stagger still plays.

What never becomes a hit (no hook sees it): regeneration, direct attribute writes, voxel damage (terrain and walls). A
blocked or parried hit changes stamina, not health.

`emf.damage` also hits placed objects (an entity with `Destructible` and no `CombatFlags`, or with `handleHits` on): the
game gives it health on the first hit and removes it at 0. A placed object's health is not saved: a reload heals it.

Recipes: [cookbook.md](cookbook.md), "Hits".

## Buffs (1.24)

The game's buffs and debuffs (potions, food, Wet, Poisoned, auras). Listing and reading work on any world. Giving and
removing work on the server world only (a game you host or the dedicated server): EMF asks the game's own buff code in
the next step, so every game rule runs (slots, giving again, faction exclusions, the food time factor, effects, the HUD
icon, saving). The answer comes after that step. A joined player's game cannot give or remove. Store type ids, not
names: names repeat ("Burning" is ten types) and change with the language, and the dedicated server has no names. Find
ids with the console command `buffs.list` or `emf.buff_types`. Details: [reference.md](reference.md), "v32: buffs".

`emf.buff_types(world [, slot])` and `emf.buff_type(world, id)` give type tables:

| Field | What |
|---|---|
| `id` | the buff type id |
| `slot` | `"food"`, `"buff"`, `"debuff"`, `"rested"` or `"terrain"` |
| `name`, `description` | the game's text in the player's language; nil on a dedicated server |
| `hidden` | true when the game does not show it |
| `saved` | true when it stays in the world save (potions, Wet, food) |
| `ends_on_death` | true when it ends with its target (EMF does not check whether the target is alive when it gives one) |
| `seconds` | its default time |
| `apply` | giving it again: `"once"` (refused), `"reset"` (its timer starts again), `"replace"` (a new buff) |
| `follow_up` | the type given when it ends (0 = none) |

`emf.buffs(world, entity)` gives a list of the entity's active buffs, in slot order (at most 40):

| Field | What |
|---|---|
| `type`, `name`, `slot` | as above |
| `entity` | the buff's own entity |
| `level` | from its source's level, 0 = none |
| `seconds` | its whole time; nil when not known yet |
| `seconds_left` | the time left; nil when not known yet (the step it was given) |
| `cleansed` | true while it is being removed |

`emf.apply_buff(world, target, type [, opts] [, fn])` and `emf.remove_buff(world, target, type [, fn])` return true when
queued, false when refused at once (the log says why: not the server world, an unknown type, 64 requests waiting, not
available on this game build). `opts`: `seconds` (0 or absent = the type's default), `source` (the entity the buff comes
from, default the target; damage over time scales from the source's stats, so give a creature), `item` (the item type
id a food buff takes its stats and time from, as `emf.item` gives: not an item entity; an unknown id is refused; without
it a food buff likely has no stats). An unknown option is an error.
`fn(world, result)` is called after the step with a result table. At most 16 requests are handled per step.

`result` (and the table `emf.on_buff` gets):

| Field | What |
|---|---|
| `kind` | `"applied"`, `"refreshed"`, `"removed"` or `"refused"` (results are `"applied"`, `"removed"` or `"refused"`) |
| `target`, `type`, `name`, `slot` | the entity, the type, its text (nil on a dedicated server) and its slot |
| `buff_entity` | the buff's own entity (0 when refused) |
| `cleansed` | `"removed"`: true when it was cleansed (always for `emf.remove_buff`; the game's own cleanses when EMF saw the mark); false when it ended or was replaced |
| `replaced` | `"removed"`: true when a new buff of the same type took its place (given again, a replace type); that `"applied"` comes right after |
| `seconds_left` | the time left; nil when not known |
| `why` | `"refused"` only: the reason, for example "the target does not have it" |

Refusal reasons: unknown buff type; the target has no buff container; the type is never given to the target's faction;
the game did not give it (the target's faction could not be read); no free slot of that group; already there and this type
cannot be given again; the target does not have it (remove; also when it is already being removed); not the server world; giving not available on this game build; 64 requests waiting.

`emf.on_buff(fn)`: `fn(world, ev)` after the server step for every change of a **player's** buffs, whoever made it (a
mod, a potion, food, rain, the end of the time). It compares after each step with the step before (a player who just
joined is compared within a quarter second). The first sight of a player (a new world, a join) says nothing.
`"refreshed"`: its end moved later by more than a second. Returns true, or false on an older core.

Recipes: [cookbook.md](cookbook.md), "Buffs".

## Markers (1.25)

Markers over the 3D view and on the game's map, drawn by EMF on each player's game in the game's style. `emf.marker` is
for the game your mod runs in; on a server (a game you host or the dedicated server) `emf.send_marker` puts them on a
player's game, or on everyone's: players need EMF on their game, not your mod. The ids are your mod's own (EMF keeps
them apart from other mods'); your markers go when your mod unloads or reloads, also from the players' games you sent
them to. Details: [reference.md](reference.md), "v33: markers".

```lua
emf.marker("home", {x = 4120, y = 96, z = 7333, text = "Home", game_map = true})
emf.send_marker(player, "pet", {entity = pet, height = 2.2, text = "Bjorn", edge_arrow = true, game_map = true})
emf.send_marker(nil, "raid", {x = 900, y = 40, z = 2100, text = "Raid!", colour = 0xff4040ff, max_distance = 600})
emf.unsend_marker(player, "pet")
```

| Function | What |
|---|---|
| `emf.marker(id, spec)` | sets or updates a marker on this game: true, false where nothing draws (the dedicated server) or when refused (logged) |
| `emf.remove_marker(id)` / `emf.clear_markers()` | removes one (true when it was there) / all of yours (how many) |
| `emf.send_marker(player or nil, id, spec)` | server: on that player's game (their entity), or every player's (nil); how many players it went to |
| `emf.unsend_marker(player or nil, id)` | takes a sent one off again |
| `emf.world_to_screen(x, y, z)` | `px, py` in display pixels, or nil (behind the camera, no frame yet) |
| `emf.marker_types()` | the game's map icon type ids its own map entries used in this world (for `game_icon`) |

`spec` keys (anything else is an error):

| Key | What |
|---|---|
| `x`, `y`, `z` | the position in world metres (with `entity`: where it shows until the game knows the entity) |
| `entity` | follow this entity |
| `height` | metres above the position |
| `text` | the label (cut at 64 bytes) |
| `colour` | `0xRRGGBBAA` (default: the HUD's accent colour) |
| `shape` | `"diamond"` (default), `"circle"`, `"square"` or `"none"` (text only) |
| `icon` | a picture from `emf.image` instead of the shape (only with `emf.marker`) |
| `world` | over the 3D view (default true) |
| `distance` | the distance under the text (default true) |
| `game_map` | on the map (default false) |
| `game_icon` | with `game_map`: one of the game's map icons (a type from `emf.marker_types`) instead of the shape |
| `edge_arrow` | an arrow at the screen's edge when it is behind you or off screen (default false) |
| `max_distance` | fades out toward it and is hidden beyond (default: no limit; the map shows it anyway) |
| `minimap` | (1.27) a canvas's `"markers"` item draws it (default false) |

## The map (1.26)

Players can act on the game's world map. `emf.map_cursor` and `emf.on_map_click` are for the game your mod runs in. A
map action is for a server (a game you host or the dedicated server): players hold Shift and the left mouse button on
their map, and your function gets the spot. Players need EMF on their game, not your mod. Details:
[reference.md](reference.md), "v34".

```lua
emf.map_action("height", {label = "Ground height", hold_ms = 800}, function(world, player, x, z, admin)
  emf.ground_at(world, x, z, function(world, ok, gx, gy, gz, why, hop_y)
    if ok then
      emf.tell(world, player, string.format("The ground at %d, %d is %.1f m up.", gx, gz, gy))
    else
      emf.log("no ground at", x, z, why)
    end
  end)
end)
```

| Function | What |
|---|---|
| `emf.map_cursor()` | `x, z` in world metres under the mouse while the map is open, else nil |
| `emf.on_map_click(fn)` | `fn(world, click)` on the client world's next step after a click on the map (Lua never runs on the game window's thread); `click` = `{button = "left" \| "right", shift, ctrl, alt, x, z}`. true when the core listens |
| `emf.map_action(id, opts, fn)` | server: adds a map action. `opts` is nil or `{label = text, hold_ms = number}` (an unknown key is an error). `fn(world, player, x, z, admin)` runs on the server world; `player` is their entity, `admin` is true for the host or an admin. The core id is `"<mod id>:<id>"`. true when taken |
| `emf.remove_map_action(id)` | true when your script had that action |
| `emf.ground_at(world, x, z, fn)` | server world: the ground under the spot. `fn(world, ok, x, y, z, why, hop_y, fog, water_y)` once, a few seconds at most; `hop_y` is a number only when the place is not loaded (nobody is near; teleport a player to that height over it, then ask again), else nil. `fog` (1.27) is the shroud a player standing there would be in: `"none"`, `"removed"`, `"shroud"`, `"deadly"` (a player there dies) or `"unknown"`. `water_y` (1.31): when the spot is under water at least 1 m deep, the water's surface (y stays the ground under it, and `fog` is the worse of the surface and the bed); else nil. true when asked |

Only the first action announced to the players is what Shift + hold does. Your map actions go when your script reloads,
pauses or unloads; your click functions stop then too.

## Canvases and the minimap (1.27)

A canvas is a drawing panel your mod owns. You place it, then hand EMF a list of shapes, and EMF draws it over the HUD
every frame. These functions are for the game your mod runs in (the client); on a dedicated server they do nothing.
Details: [reference.md](reference.md), "v35". The sample minimap in `mods/minimap/` is the full example.

| Function | What |
|---|---|
| `emf.player_view()` | `{x, y, z, heading, camera_yaw, world_size}`, or nil when there is no player yet. Degrees: 0 north (+z), 90 east (+x). `camera_yaw` is nil until a frame of the 3D view was seen. |
| `emf.fog_at(x, z)` | 0 (unexplored) .. 1 (explored) on this game's map, or nil. A copy refreshed twice a second. |
| `emf.map_markers()` | a list of `{type, x, y, z, shown}`: the game's own map markers (altars, shrines, places found). `shown` is true when the game's map shows it. |
| `emf.canvas(id, spec)` | places a canvas. `spec` = `{corner = "top_left" \| "top_right" \| "bottom_left" \| "bottom_right", x, y, w, h, shape = "rect" \| "circle", hidden}`. `x`, `y` are measured from the corner, inward; all sizes are in units of a 1080-pixel-high screen; `w`, `h` are 8 to 1080. `spec` nil removes the canvas. Your canvases go when the script reloads, pauses or unloads (the core id is `"<mod id>:<id>"`). |
| `emf.canvas_draw(id, list)` | the canvas's drawing, replacing the last one. At most 2048 items. |

A list item is a table: the kind first, then numbers, then named options. Positions are in canvas units: 0, 0 is the
top left, `w`, `h` the bottom right. Colours are `0xRRGGBBAA`. An unknown kind is an error.

| Kind | Positional numbers | Named |
|---|---|---|
| `"rect"` | x, y, w, h | `colour`, `width` (0 or none: filled) |
| `"circle"` | cx, cy, r | `colour`, `width` |
| `"line"` | x1, y1, x2, y2 | `colour`, `width` |
| `"tri"` | x1, y1, x2, y2, x3, y3 (filled) | `colour` |
| `"text"` | x, y (the centre), then the text | `colour`, `size` |
| `"image"` | picture (from `emf.image`), x, y, w, h | `colour` (tint), `turn` (degrees) |
| `"fog"` | cx, cz, metres per unit | `colour` (the unexplored colour) |
| `"markers"` | cx, cz, metres per unit | |
| `"map"` | cx, cz, metres per unit | |

`fog`, `markers` and `map` take the world x, z at the canvas centre and how many metres one canvas unit is. `"fog"`
draws the fog of war as cells. `"markers"` draws the markers made with `minimap = true` (see "Markers"). `"map"` is
the game's own world map, drawn by the game, so it looks like the map screen (relief, shroud, fog of war). It is north
up: turn your own arrow and icons.

Draw order: the game draws `"map"` before EMF draws anything else. Items before your last `"map"` are under it and are
drawn only when the game cannot draw the map. Put the background first, then `"map"`, then the rest. The game clips its
map to the canvas's rectangle, also for a round canvas. Canvases are hidden where markers are: on the map screen, in
menus and in the console.

```lua
emf.canvas("mini", {corner = "bottom_right", x = 16, y = 64, w = 200, h = 200})
emf.on_tick(function(world)
  if not world:is_client() then return end
  local v = emf.player_view()
  if not v then return end
  emf.canvas_draw("mini", {
    {"rect", 0, 0, 200, 200, colour = 0x14181ce6},
    {"map", v.x, v.z, 2},
    {"circle", 100, 100, 4, colour = 0xffe08aff},
  })
end)
```

## Removing placed objects (1.28)

An object a player placed can be removed the way a pick-up does, with its item given back (`emf.remove_building(world, id [, opts])`, in the table above). Host only, like
`emf.destroy` (the server world, inside a world callback). On a core older than API v36 it returns `false, "this EMF core is
too old (remove_building needs API v36)"`. `item_id` is nil when nothing went back.

| `opts` field | What |
|---|---|
| `refund` | `false` (default): nothing back. `"drop"`: a loose pickup at the object. A player's entity id: into that player's inventory (a drop at the object when it does not fit). Any other value is an error. |
| `force` | `true`: remove it even when it holds items: they drop at its spot as loose pickups (item, count, level and rarity; 0.6.8). Default false. |

Any other key is an error, so a typo cannot quietly lose the item.

It refuses, and changes nothing, for a player, an id with no entity (or no position), a Flame Altar (even with
`force`: use `emf.destroy` if you mean it), an object no player placed (use `emf.destroy`), and an object holding items
unless `force`. It plays no pick-up sound.
Details: [reference.md](reference.md), "v36".

```lua
emf.command("takedown", "<entity>: remove a placed object, the item to you", function(world, args)
  local id = tonumber(args[1])
  if not id then emf.reply("usage: takedown <entity>") return end
  local s = emf.sender()
  local ok, item = emf.remove_building(world, id, {refund = (s and s.entity ~= 0) and s.entity or "drop"})
  emf.reply(ok and ("removed, item " .. tostring(item)) or ("refused: " .. item))
end)
```

## Paths (1.29)

`emf.path_find(world, from, to [, opts], fn)` asks the game's own path grid for a path. The grid knows terrain,
buildings, placed objects and doors. It moves nothing. It returns an id, or nil (the reason is in the log). Host only,
like `emf.spawn`: a joined game has no path grid. `from` and `to` are `{x, y, z}` or `{x = , y = , z = }`, in metres, at
most 200 m apart. On a core older than API v37 it is nil.

The answer comes once, after a server step or more, to `fn(world, result, points)`:

- `result.status`: `"found"`, `"no_path"`, `"too_long"`, `"no_start"`, `"no_target"`, `"no_memory"`, `"cancelled"` or `"other"`;
- `result.reaches`: true when the last point is within 1.5 m across and 3 m up or down of `to`;
- `result.length`: metres from `from` along the points; `result.count`: the points (at most 512);
- `points`: a list of `{x, y, z, move}`; `move` is 0 walk, 2 jump up, 3 jump down, 4 jump across, 5 a door. A door
  point also has `door`, the door's entity (1.35, EMF API v42): the door this path opens there.

`emf.path_cancel(id)` returns true when the query was open. The callback still comes, with `"cancelled"`. A script that
is paused or reloaded before the answer hears nothing. Bad option values, or a position without three numbers, are Lua
errors.

| `opts` field | What |
|---|---|
| `jumps` | `true`: the path may jump up, down and across |
| `doors` | `true`: the path may open doors. A closed door is crossed only this way (EMF 0.6.11; before, a plain walk could cross a closed door, whose spot counts as ground in the grid), at a point with `move` 5. The whole box of a closed door's collider, on its lowest 1 m layer, is closed to walking: a door standing at an angle closes the floor beside it too. This is path_find's view; creatures walk with the game's own rules |
| `swim` | `true`: the path may swim |
| `closest` | `true`: when the target cannot be reached, end at the closest reachable point |
| `width`, `height` | the room the unit needs, in metres (0 to 64; default: the game's). `width` works as a radius: 0.5 needs 0.5 m free all round, 1.5 a clear 3 m circle (an open gate rarely has that). The grid keeps at most about 2 m across and 3.5 m up, so more finds no start. Jumps ignore both in flight. Try 0.5 and 2 for a humanoid, 1 and 3 for a big one. With `doors = true` pass only `height`: a modder saw any width take the door point away (the spots next to a gate have little room) |
| `max_jump_up`, `max_jump_down` | metres (0 to 255; 0 = no limit) |
| `nodes` | how many nodes the search may spend (0 to 20000; default 2000) |
| `owner` | an entity the query belongs to (default: the first player) |

Use `closest = true` and check `reaches`. A short path can mean blocked or ran out: without `closest`, `"no_path"` means
the target is shut off for these moves, while `"too_long"` (all `nodes` spent) and `"no_memory"` (the game's fixed
search memory full, usual above a few thousand nodes) mean the search ran out. With `closest` all three come back
`"found"` with `reaches` false: ask again from the path's last point, and treat it as blocked when the answers stop
getting nearer. Long paths need more `nodes`: 2000 got about 36 m of a 141 m route, 20000 got about 90 m in about 1.3 s.
The grid exists only where the world is loaded, near players. Each query is a search in the game's own jobs, shared with
every walking creature: do not ask every step. Details: [reference.md](reference.md), "v37".

```lua
-- can this raider get to the altar?
emf.path_find(world, raider_pos, altar_pos, {closest = true, doors = true, nodes = 5000},
  function(w, result, points)
    if result.reaches then
      emf.log("raider can reach the altar in", result.length, "m")
    else
      emf.log("blocked:", result.status, "path stops after", result.length, "m")
    end
  end)
```

## Doors (1.30)

`emf.door(world, id)` returns `"open"` or `"closed"` plus a table `{locked = bool, logic = bool}`, or nil when the entity
is not a door (player doors and gates). `logic` is true when a switch drives the door: EMF does not touch it.

- `emf.door_open(world, id [, "front" | "back"])` and `emf.door_close(world, id)` change the door at the next server
  step; players see it swing, and the game's collider and path grid follow it, so enemies' paths change. No sound plays.
- `emf.door_lock(world, id, true | false)`: while locked, no player gets the E prompt on it. From EMF 0.6.8 the lock is
  kept with the world (by the door's place): after a load the door there is locked again. A locked door that breaks
  or is taken down while a player is near loses its lock (a new door there is not locked). `emf.door_open` and
  `emf.door_close` still work on a locked door.

The three actions return true when the door will change at the next step, and false when refused (not the server world,
not a door, a switch drives it, already so, 256 door orders waiting already, or doors are off on this game build; a
lock also when 1024 doors are locked already; the reason is in the log). Any other side word is a Lua error. Host only, like
`emf.spawn`. On a core older than API v38: nil and false.

```lua
-- when the attack starts, open the gate; the treasure room stays shut
local function raid_started(world)   -- your own raid code calls this
  emf.door_open(world, gate_id)
  emf.door_lock(world, treasure_door_id, true)
end
```

## Enemy orders (1.32)

`emf.enemy_order(world, enemy, target [, {seconds = n, damage = n, allies = true}])` sends a creature with an enemy AI
at any entity with a position: a player, a creature, a door, a gate, a placed object. While the order holds, EMF writes
the target as its pick and puts it in its aggro list, at once and after every server step, and puts an enemy at rest
(idle, roaming, at home, asleep) in its fight state, so it walks up and attacks. Seen in game: a Vukah walked 30 m to a
door, and one hit with `damage = 200` broke it.

- `seconds`: how long (default 0: until the target is gone or dead, the enemy is gone, or you cancel it).
- `damage`: the game's own hits on a placed door do 0 (seen); `damage` is added to each of the enemy's hits on the
  target (a hit rule), so a door can break.
- `allies = true`: the creature may be a player's ally (a tamed beast); without it such a creature is refused.
- Another key, a value of the wrong type (`allies = "no"`, `damage = 2.5`), or `damage` outside 0..100000 is a Lua
  error.

It returns the order's id, or nil and why (not the server world, the enemy as its own target, no enemy AI, a player, a
boss, no such target, a dead target, `seconds` below 0, more than 256 orders, or `damage` with no hit rule free: all 64,
shared by every mod, are in use, or hit rules are off on this build). An order also ends when its enemy dies or becomes
a player's ally (tamed), and when your script reloads or pauses. A new order on the same enemy ends the old one. When an
order ends, the game picks for the enemy again. Nothing is saved. `emf.enemy_order_cancel(id)` ends one;
`emf.enemy_target(world, enemy)` is what it goes for now (its pick), or nil. Host only, like `emf.spawn`. On a core
older than API v39: nil and a reason.

```lua
-- a raid: the raider breaks the gate, then the game takes over
local id, why = emf.enemy_order(world, raider, gate, {seconds = 60, damage = 40})
if not id then emf.log("no order: " .. why) end
```

## Move orders (1.33)

`emf.enemy_move(world, enemy, points [, {arrive = m, seconds = n, peaceful = true, allies = true}] [, fn])` walks a
creature with the enemy AI (an enemy, or a tamed beast with `allies = true`) to a point `{x, y, z}` or through a list of
up to 16 of them (the points `emf.path_find` gives fit; thin a longer path to 16). It walks the game's own way: EMF puts a hidden waypoint at each
point in turn and gives the creature the game's guard order on it, so it finds its path and jumps where its kind can. Seen
in game: a Vukah walked 36 m in 10 s. A creature stops about 3 m short of a guarded spot (its guard distance), so a point
counts as reached within that distance + 1 m, or within `arrive` if that is larger.

- `arrive`: metres (0.5 to 50, default 2).
- `seconds`: a time limit (default 0: none; at most 3600).
- `peaceful = true` (or `"players"`): it ignores players on the way (its fights with players are cleared each step).
  `peaceful = "all"` (1.34): it ignores everyone, animals and players' beasts too: it only walks (a thief running off).
  Without it, it may fight on the way (the game's way) and goes on afterwards.
- `stuck` (1.34): seconds without a metre closer (while not fighting) before it ends `"stuck"` (2 to 120; default 15).
- `allies = true`: the creature may be a player's ally (a tamed beast).
- Another key, or a value of the wrong type (`peaceful = 1`), is a Lua error.

`fn(world, id, status)` comes once when the move ends; `status` is `"arrived"`, `"stuck"` (no metre closer for 15 s,
or `stuck`, while not fighting: a wall, no path, a point it cannot reach), `"timeout"`, `"gone"` (it died or went) or `"cancelled"`
(`emf.enemy_move_cancel`, a new move or enemy order on it; `world` is nil when the world closed). A cancel from a
server callback ends it at once, else at the next step. It returns the move's
id, or nil and why (not the server world, no enemy AI, a player, a boss, a tamed beast without `allies`, bad points or
numbers, more than 256 moves). Two creatures standing on one spot, both with a move, block each other and neither walks: after
2 s without a metre closer EMF moves the later move's creature 1 m aside (EMF 0.6.12; at most twice). Spawn creatures
at least a metre apart anyway. Put a point in front of a closed door or gate, not on it: a modder saw a fresh move to a
point on a shut gate fail its first try for 18 of 28 raiders (the creature idles and roams for about 3 s, wanders off a
few metres, then walks to the gate), and 0 of 12 with the point 3 m in front of it. Afterwards a creature that guarded someone guards them again (a tamed beast: its owner);
any other one that arrived lives there (its home is the last point, it roams around it). A move ends when your script
reloads or pauses (`fn` is not called then). A move and an enemy order on the same creature end each other. Nothing is
saved. `emf.enemy_moving(world, enemy)` is its move's id, or nil. Host only. On a core older than API v40: nil and a reason.

```lua
-- walk a raider to the gap next to the gate, then break the gate
emf.enemy_move(world, raider, {gap}, {peaceful = true}, function(w, id, status)
  if status == "arrived" then emf.enemy_order(w, raider, gate, {seconds = 60, damage = 40}) end
end)
```

## Breaking blocks (1.37)

`emf.break_blocks(world, x, y, z [, opts], fn)` damages (like a pickaxe hit: cracks, the game's per-block health) or
breaks at once the building blocks in a sphere, or a column of spheres, around a point in metres. Never terrain: the
ground stays. A hole is a normal building change, sent to every player and saved like mining: the game keeps it inside
a flame altar's area and resets the world outside bases (after about 30 minutes or at a server restart), as it does for
a pickaxe. Host only (the server world). It returns an id, or nil (the reason is in the log). On a core older than
API v44 it is nil.

**The world's own blocks (1.38, API v45).** The world's ruins are building blocks too. EMF tells them from what players
built by comparing each block with the world as the game shipped it: a block of the shipped material in its shipped
place is the world's own. A break never touches those, nor a block whose origin is not known (its place is not
loaded): each sphere stops just short of the nearest one, and a sphere with less than 0.25 m left is skipped. So a hole
next to a ruin is smaller. A ruin block a player broke and put back in the same material counts as the world's again.
`world = true` breaks them too. On a 1.37 core (API v44) ruins break like any block.

| `opts` field | What |
|---|---|
| `radius` | metres, 0.25 to 3.5 (default 1) |
| `height` | metres, 0 to 4: more spheres up to this far above the point (a column; at most 8 spheres) |
| `mode` | `"damage"` (default) or `"break"` (at once: no health, no cracks) |
| `damage` | a number for all five hardness classes, or a list of five (Soft, SlightlyHard, ModeratelyHard, Hard, VeryHard), whole, 0 to 100000; default the game's T4 pickaxe (100, 100, 95, 60, 50) |
| `drops` | `true`: removed blocks drop what a pickaxe gives (each material's item, so many blocks per item) at the hole |
| `materials` | a list of building material ids 0 to 127 (`emf.block_at`'s `material`); default every one that can break |
| `world` | 1.38: `true` breaks the world's own blocks too (its ruins); default `false`: they are spared |

Any other key, or a value of the wrong type, is a Lua error. `fn(world, result)` comes once, after the server step that
ran it: `result.status` is `"done"`, `"cancelled"` (the world ended first) or `"failed"` (breaking is off after a
fault); `result.damaged`, `result.removed`, `result.dropped` are counts. A script paused or reloaded in between hears
nothing. With the default damage straw falls in 1 hit, wood logs in 2-3, planks in 5, stone in about 22; damage not
finished heals 20 s after the last hit; the game's Unbreakable materials never break. At most 8 spheres run per step for
all mods; the rest wait.

`emf.block_at(world, x, y, z)` and `emf.find_wall(world, from, to)` (positions `{x, y, z}` or `{x = , y = , z = }`; the
line at most 64 m) give `{kind = "empty" | "terrain" | "building", material, fill, hardness, health, damage, x, y, z,
world}` (1.38: `world` is `true` for the world's own block, a ruin as shipped, `false` for a player's, nil when not known
or not a building block)
(`hardness` 0 Soft .. 4 VeryHard, 5 Unbreakable; `damage` 0..255 so far, nil when unknown; `x, y, z` the block's
centre), or nil (the line is clear, the place is not loaded, not the server world, an older core). Inside a world
callback on the server world, like `world:get`.

```lua
-- a bomb that cracks soft walls around it, never the ground
emf.command("bomb", "<x> <y> <z>: crack the walls around a point", function(world, args)
  local x, y, z = tonumber(args[1]), tonumber(args[2]), tonumber(args[3])
  local id = emf.break_blocks(world, x, y, z, {radius = 2, damage = {400, 300, 200, 50, 20}, drops = true},
    function(w, r) emf.reply(r.status .. ": " .. r.removed .. " blocks gone, " .. r.dropped .. " items") end)
  if not id then emf.reply("refused (see the log)") end
end)
```

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
| `world:permissions(id)` | (1.36) the player's own game permissions: a table with `admin` (the game's admin: on a dedicated server the Admin group, in a game someone hosts the host and the players the host made admins), `access_inventories`, `edit_base`, `edit_world`, `extend_base`, `receive_exp` (booleans) and `mask` (EMF_PERM_* bits); nil for anything but a player of this server world, on a client world or an older core. Read it when you need it: a host can change it. EMF's own admins (`[chat] admins`) are another list (`from.admin` of a mod message, a command's sender) |
| `world:character_name(id)` | (1.22) the player's character name (the Social tab's), or nil when not known: it may already be there in `on_player_joined` or come a few seconds later, and it can change during a session, so read it when you need it and never keep it. Text a player's game set, not an identity: key data by `p.account`. Server world only; players' chat need not be on |
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

**Per player in a world** (`lua` 1.20): a text for each player, by their account (`world:account(id)`, or `p.account`
in `emf.on_player_joined`):

```lua
local visits = {}
emf.on_player_joined(function(world, p)
  local text = emf.player_data(p.account)        -- "" the first time
  visits[p.account] = (tonumber(text) or 0) + 1
  emf.save_player_data(p.account, tostring(visits[p.account]))
end)
```

- `emf.player_data(account)` gives the player's text, "" when none, and nil before the world's data came (the same
  moment `on_world_data` runs), where no world of yours runs, and while the world's players file cannot be read (not
  EMF's format: the log says so; another program had it open: it is read again). `emf.save_player_data(account, text)` saves it
  (`""` removes the player) under the same rules as `save_world_data`; it returns false over 16 KiB for one player or
  2 MiB for the world's file.
- An account is `"0x"` and up to 16 hex digits, as `world:account` gives it; anything else is an error. The all-zero
  account (the console's `emf.sender()`, the host's own `emf.net` messages) gives nil and false: use
  `world:account(emf.local_player())` for the host.
- Kept beside the world's data in `emf\config\<your mod id>\<world slot>.players.txt` (C++: `WorldFile::player` and
  `save_player`). Each save writes the whole file, so save when a player's text changed, not every step.

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
