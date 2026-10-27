# How Enshrouded and EMF work: a guide for modders

This page explains the model behind the API, so that the [reference](reference.md) and the
[cookbook](cookbook.md) make sense. Everything here was learnt by reverse engineering the game (build 1273043) and
checked in game unless it says otherwise; the details are in `docs/ENGINE_MAP.md` and
`docs/engine_parts/`.

## The engine in one page

Enshrouded runs on Keen Games' own engine. Gameplay is an **ECS** (entity component system):

- An **entity** is a number (`uint32_t`, e.g. `1` for the local player, `1025` for a villager).
- A **component** is a block of data an entity has (`CurrentTransform`, `Health`, `Inventory`, `Faction`...). There
  are about 660 component types. Their layouts come from the game's own type registry; EMF generates C++ structs for
  all of them in `sdk/include/emf/generated/components.hpp` (namespace `emf::gen::ecs`), each with its `kHash` (the
  id the game uses) and `kSize`.
- A **program** (the game's word for a system) is a function the engine runs every tick over the entities that have
  certain components: `growth` raises a plant's level, `npc_bed` links villagers to beds, and so on (about 800,
  listed in `docs/ecs_programs.md`).
- An **event** is a short message programs send each other (`HitEvent`, `EntityDiedEvent`, `PickupEvent`,
  `CraftRecipeEvent`, `LootInteractionEvent`...). Sending the game's own events is the safest way to make things
  happen: the game's programs then do the work exactly as for a player.
- A **template** is the blueprint an entity was spawned from (a GUID, e.g. `Enemy_Vukah_Melee`). Many components are
  split in two: the template half (defaults shared by every entity of the template, read-only memory) and the
  per-entity half, stored as its own component named `Dynamic<Name>` (e.g. `DynamicActiveNpcState`).

## Worlds and threads

The game runs separate ECS **worlds**:

| World | Exists | What it is |
|---|---|---|
| server | on the host only | the authoritative simulation: gameplay changes must happen here |
| client | everywhere | what the local player sees; replicated from the server |
| prediction | sometimes | client-side prediction |

`emf::World` wraps a world pointer: `w.is_server()`, `w.is_client()`, `w.get<T>(entity)`,
`w.get_template<T>(entity)`, `w.for_each_entity(f)`. Entity ids are the same in the server and client worlds of a
host.

Each world steps on its own thread. EMF calls you **after a world's step, on that world's thread**, where reading and
writing components is safe:

| Callback | When | Typical use |
|---|---|---|
| `emf::on_tick(f)` | after every step of every world | per-frame logic; filter with `w.is_server()` |
| `emf::run_on_world(kind, f)` | once, after the next step of a world of `kind` | do something "on the game thread" from elsewhere |
| `emf::on_event<E>(kind, f)` | after the step in which the game sent event `E` | react to kills, hits, pickups |
| `emf::add_command(name, help, f)` | when someone types the command | developer tools |
| put / take results | after the server step that ran the transaction | item moves |
| `emf::wheel(..., f)` | after the step following the player's choice | menus |

Outside these (your own threads, `emf_mod_load`) do not touch components; use `run_on_world`.

**Mod programs** (`emf::add_program`) are different: they run *inside* the world's step, on a game worker thread, in
the slot of a real game program. That is where events are sent and read (`emf::emit<E>(ctx)`, `emf::read<E>(ctx, f)`)
and where other entities can be read through the game's own accessors (`emf::read_other<T>`). Keep program callbacks
short and hand work to/from `on_tick` through a mutex-protected queue: the shipped mods all follow that pattern
(see `mods/companion/companion.cpp`, `guard_program`).

## Components: reading and writing

```cpp
auto* hp = w.get<emf::gen::ecs::Health>(id);      // nullptr if the entity has none, or the size differs
auto* tpl = w.get_template<emf::gen::ecs::Enemy01BehaviorState>(id);  // template half: const, shared
```

`World::get<T>` checks the game's size of the component against your struct and returns null on a mismatch, so a
game update that changes a layout gives you null rather than garbage.

- **Per-entity components** can be written directly through the pointer while you are in a callback. The server's
  programs read them on their next pass. This is how the companion mod sets its `Faction` and `Level`, and how
  villager jobs re-arm a harvested plant (`Level`, `DynamicGrowth`). The write has no replication bookkeeping: what
  the game replicates to clients from it was not tested online.
- **Template data** is read-only memory shared by every entity of the template in both worlds. Change it only with
  `emf::write_value(&tpl->field, value)` (or `write_memory`); the change lasts until the game restarts, is never
  saved, and affects every entity of that template (including wild ones).

To find out what an entity has, use the console: `inspect <entity>` lists its components and sizes, `tpl <entity>
<component>` dumps template data, `near [radius]` lists what is around you, `inv <entity>` lists an inventory.

## Events and programs

Two ways to use events:

1. **Watch** (`emf::on_event<E>`): a copy of every event of a type, after the step. Simple; read-only.
2. **Send / read in a program** (`emf::add_program(name, kind, {events in}, {events out}, f)`): your program declares
   the events it reads and sends; inside it, `emf::emit<E>(ctx)` returns a zeroed event to fill in, and the game's
   programs see it on the next tick.

Events that worked from a mod program in game: `StartGuardingEvent` / `StopGuardingEvent` (make an NPC guard an
entity), `AddThreatTargetEvent` (make an NPC attack), `NpcCommandEvent` (walk a villager / move its home),
`CraftRecipeEvent` (craft at a station into a chest), `LootInteractionEvent` with `bypassInteraction` (harvest a crop
or collect from an animal into a chest). See the cookbook for each.

## Items and inventories

- Items are identified by an `ItemId` (the FNV-1a hash of the item's GUID). `emf::item_id`, `find_item(name)`,
  `item_info`; `items <part of a name>` in the console lists them with their debug names (`Material_T1_Wood_Logs`).
- An entity with an initialized `InventorySetup` (players, villagers, chests, farm animals, feeders) owns several
  8-slot `Inventory` entities (backpack, action bar, equipment...). `emf/inventory.hpp` reads them.
- **Changing inventories goes through the game's own inventory transaction**, run by EMF's internal program on the
  next server step: `put_items` / `put_items_ex` (with item level and rarity) and `take_items` (all or nothing, a slot
  first or only). The result comes back in your callback. Stacking, the "+N" popup, knowledge unlocks and saving all
  behave as for the player.
- A gear item has its own **data entity** (its "pide": level, perks, gem). `take_items` makes the game destroy it, so a
  take followed by a put gives a fresh copy (same id; level and rarity if you pass them; perks re-rolled). Move only
  plain stacks between containers, or accept that.

## Sessions, saves and per-world data

- `emf::server_session()` is a number that changes every time a hosted world starts (a new game, a reload, another
  world). **A reload often reuses the world object's address**, so compare session numbers, never `EmfWorld*`
  pointers. Entity ids of the old session mean nothing in the new one.
- `on_load(slot)` runs after the host loaded a world save (with its slot number), `on_save(slot)` after it wrote one.
- The game saves player-built things and entities whose template is marked to be saved (villagers). **Entities a mod
  spawns are not saved** (chests, plants, animals, even with an added `ForceSave` component), except villagers.
- For your own per-world data use `emf::WorldFile` (`emf/storage.hpp`): it learns the slot, loads once per session,
  and saves atomically into `<game>\emf\config\<mod>\<slot>.txt`, only for the session whose file it read.

## Your mod's folder, manifest and settings

Ship your mod as a folder: `<game>\emf\mods\<id>\` with your DLL and a `mod.json` (the template does this). The
manifest says who the mod is (`id`, `name`, `version`, `authors`, `description`), what it needs (`emf`: the lowest API
version; `depends`: other mods and their versions, which then load before yours), and which settings it has. The
loader reads every manifest first, orders the mods, and tells the player in the console's Mods tab (and with `mods`)
what became of each one: loaded, switched off, or not loaded and why.

Settings are the part players see. Declare them in `mod.json` with a type (`bool`, `int`, `float`, `slider`, `choice`,
`select`, `keybind`, `color`, `text`), a default, a label and help, and ranges, steps, options or a length limit; put
them under `group` headings and grey one out with `enabledWhen` while another has some value (reference.md lists every
field). EMF then

- shows them in the Mods tab and in EMF.exe as switches, sliders, lists, colour pickers, key binds (click, press a key)
  and text fields, and with `settings <mod>` / `set`;
- stores them in `emf\config\<id>.ini`, section `[<id>]`, so a mod may also read its numbers and on/off values with
  `GetPrivateProfile*` (EMF writes a clamped or replaced value back, so both agree; choices are stored as their index
  and keys by name, which an old `parse_key` does not read: use the settings API for those, with the same defaults as
  the manifest);
- checks every value: an ini value out of range is clamped, a bad one falls back to the default, and a bad value typed
  in the console or the Mods tab is refused with a reason;
- tells you about changes: `emf::on_setting_changed(id, f)` runs after the next world step. Mark settings you apply at
  once `"live": true`; the others show "next start", and your mod reads them in `emf_mod_load`.

Read values with `emf::setting_int / setting_float / setting_bool / setting_text(id, key, fallback)`. Every mod also
has a switch, its `enabled` key: off means EMF does not load the DLL at the next start (`"enabled": false` in the
manifest makes a mod start off, e.g. one that changes saves). See the cookbook's "Give your mod settings".

## Dependencies and version ranges

When your mod uses another mod (calls its functions, needs its data, runs as its script), list it in `depends` with the
versions you work with:

```json
"depends": {
  "lua": ">=1.3",
  "helper": {"version": ">=2.1, <3.0"},
  "map_markers": {"version": "^1.0", "optional": true}
}
```

- **The range** follows SemVer, as npm and Cargo read it. `^1.3` takes 1.3.0 up to below 2.0.0: newer versions that
  keep the major number, which by SemVer's promise do not break you. `~1.3` is stricter (1.3.x only), `>=2.1, <3.0`
  spells it out, `=1.3.2` takes only that version, `*` any. A bare `1.3` means the same as `^1.3`. The full table is in
  [reference.md](reference.md).
- **Required** (the default): your mod loads after it, and does not load when it is missing, a version outside the
  range, switched off or failed. The Mods tab, the `mods` command and EMF.exe say which one and why: `needs helper
  >=2.1, <3.0 (2.1.0 or newer, below 3.0.0), found 2.0.4: too old (update helper)`.
- **Optional** (`"optional": true`): your mod loads with or without it, and whatever version it is while it is
  switched off. When it runs, it loads first and its version must fit. Check at run time whether it runs before you use it (in C++ `emf::mod_info("map_markers")`
  gives its status). Two mods that use each other only optionally both load.
- **A mod you write for others to depend on:** raise the major version (2.0.0) when you remove or change something
  they use, the minor version (1.4.0) when you add something, the patch (1.3.1) for fixes. Then `^` ranges keep
  working for everyone.

A Lua mod always needs the `lua` mod; EMF adds that dependency itself, so list it only to ask for a version (`"lua":
">=1.3"` for `emf.on_key`; an older EMF reads `>=` and says which `lua` it needs, where it would not read `^`).

## What only works near the player

The game simulates the area around players: plants grow, animals eat and produce, villagers walk and work only while
the area is loaded (villagers pause beyond about 150 m). Code that acts on far entities may find them missing.

## Safety rules (what EMF protects you from, and what it cannot)

- A crash inside one of your callbacks is caught (structured exception), logged with your DLL's name, and that callback
  is disabled. A C++ exception escaping a wheel, item or setting-change callback (the emf.hpp wrappers) is caught
  too.
- `spawn`, `destroy`, `give_item` refuse outside callbacks or on a client world.
- `World::get<T>` refuses a size mismatch; `emit<E>` refuses an event size mismatch; mod programs that read other
  entities are cross-checked against the engine's own lookup.
- What EMF cannot protect: writing a wrong value into a component, writing template data your code misunderstood,
  or destroying an entity the game still needs. Test in a separate world.

## C++ notes

- `emf/keys.hpp` and `emf/storage.hpp` include `<windows.h>`, which defines macros such as `min`, `max`, `near` and
  `far`: a variable named `near` or a call to `std::max(a, b)` then fails to compile. Define `NOMINMAX` (and, if you
  like, `WIN32_LEAN_AND_MEAN`) before including them, write `(std::max)(a, b)`, and avoid those names.
- Callbacks may be lambdas with captures (the wrappers copy them); keep the objects they refer to alive (globals or
  statics), since the game calls back long after `emf_mod_load` returned.

## Reaching engine code EMF has not named

EMF names 37 engine functions ("signatures") and builds its API on them. For anything else, find the function the
same way EMF does, by its bytes:

- `emf::find_pattern("48 8B 05 ?? ?? ?? ?? ...")` returns the address of the one place in the game exe where those
  bytes occur (`??` matches any byte), optionally plus an offset, and optionally following a relative address (a
  call's target, a global variable a `mov` reads). Patterns survive game updates as long as the bytes stay the same,
  which is much more often than addresses do.
- `emf::hook_pattern(name, bytes, detour, &original)` replaces the function with yours; call `original` to run the
  game's (or the next hook's) code. Several mods, and EMF itself, can hook the same function: the hooks chain, the
  newest runs first. `hooks` in the console lists every hook and who owns it; `pattern <bytes>` tests a pattern live.
- Keep your patterns in a `.patterns` file so `emf_sigcheck --patterns` can check them after every update without
  starting the game (see [updating.md](updating.md)), and check `emf::game_build()` if your mod depends on one build.

Finding good patterns needs a disassembler (Ghidra, IDA, x64dbg); `docs/ENGINE_MAP.md` lists what is already known
about the engine. A detour must have exactly the original function's signature and calling convention, and it runs on
whatever thread the game calls it from: touch ECS data only as the rules above allow (hand work to `run_on_world`).
`mods/sim_rate/` is a complete example: it hooks the simulation step, which EMF also hooks.

## Game updates

EMF knows each game build by its file hash and has the engine function addresses ("signatures") for it. On an unknown
build it searches for them by byte pattern and turns off what it cannot find, logging it. Your mod should check the
results of API calls (null pointers, false) rather than assume. See [updating.md](updating.md).
