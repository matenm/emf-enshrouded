# EMF cookbook

Short, complete recipes. Each one says how it was tested; the shipped mods (`mods/`) contain every pattern in a full,
working mod. All snippets assume:

```cpp
#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
namespace gen = emf::gen::ecs;
```

and run on the **host**. "In a world callback" means inside `on_tick`, `run_on_world`, `on_event`, a command, a
put/take result or a wheel choice.

---

## Spawn something next to the player
*Tested in game (rabbits, Vukah warriors, chests, villagers).*

```cpp
#include "emf/query.hpp"

emf::add_command("mymod.rabbit", ": a rabbit next to you", [](emf::World w, const std::vector<std::string>&) {
    uint8_t guid[16];
    emf::parse_guid("7e389a60-f74b-4133-a5aa-68e91623985c", guid);  // a template GUID (export/template_guids.csv)
    const auto p = emf::position(w, emf::local_player());
    if (!p) return;
    const uint32_t id = emf::api()->spawn(w.raw(), guid, p->x + 2.0, p->y, p->z);
    EMF_REPLY(id ? "spawned %u" : "refused (see the log)", id);
});
```

Spawned entities are **not saved** with the world (villagers excepted). Props that need a surface (feeders, some
furniture) vanish where they cannot attach.

## React to the game's events (a kill, a hit, a pickup)
*Tested in game (`event_log` mod, companion deaths, villager_jobs pickups).*

```cpp
// in emf_mod_load (event subscriptions must exist before the first world starts)
emf::on_event<gen::EntityDiedEvent>(EMF_WORLD_SERVER, [](emf::World w, const gen::EntityDiedEvent& e) {
    EMF_LOG("mymod", "entity %u died", e.deadEntityId.id);
});
emf::on_event<gen::PickupEvent>(EMF_WORLD_SERVER, [](emf::World, const gen::PickupEvent& e) {
    EMF_LOG("mymod", "%u got %u x item %08x from %u", e.pickedUpByEntityId.id, e.itemCount, e.itemId.value,
            e.pickedUpFromEntityId.id);
});
```

The callback runs after the step on the world's thread: you may read components and spawn there.

## Items: give, put into an inventory, gear with level and rarity, take
*Tested in game (player, villagers, chests, feeders, farm animals).*

```cpp
#include "emf/inventory.hpp"

// a loose pickup the game collects (popup, knowledge) - in a world callback
emf::give_item(w, emf::local_player(), "Material_T1_Wood_Logs", 5);

// straight into any inventory through the game's transaction; the result arrives after the next server step
emf::put_items(w, chest, "Material_T1_Wood_Logs", 20, [](emf::World, uint32_t added) {
    EMF_LOG("mymod", "%u fitted", added);
});

// gear with an item level and rarity (0 Common .. 4 Legendary; 0xFF = the game's default)
const uint32_t sword = emf::api()->find_item(w.raw(), "Weapon_T1_1H_SwordEpic_01");
emf::put_items_ex(w, emf::local_player(), sword, 1, 10, 3, [](emf::World, uint32_t added) {});

// take: all or nothing; here exactly the stack in a known slot (refused if it moved meanwhile)
for (const auto& s : emf::inventory_stacks(w, emf::local_player()))
    if (s.item == sword) {
        emf::take_items(w, emf::local_player(), sword, 1, emf::slot_id(s.inventory, s.slot),
                        [](emf::World, uint32_t taken) { /* 1 or 0 */ }, true);
        break;
    }
```

Taking a gear item destroys its data entity (perks, gem): putting it back makes a fresh copy. Move plain stacks freely;
think twice about gear.

## A console command with arguments
*Tested (every shipped mod has some).*

```cpp
emf::add_command("mymod.heal", "<amount>: heal yourself", [](emf::World w, const std::vector<std::string>& args) {
    if (args.size() != 2) {
        EMF_REPLY("usage: mymod.heal <amount>");
        return;
    }
    const int amount = std::atoi(args[1].c_str());
    // ... change a component of emf::local_player() ...
    EMF_REPLY("healed %d", amount);
});
```

Type it into the in-game console (key under Esc) or a line of `<game>\emf\commands.txt`.

## A command every player may run from the chat
*Unit-tested; the chat side was tested on a dedicated server for admin commands. Needs API v17.*

```cpp
#include "emf/query.hpp"

emf::add_command("mymod.where", ": where am I?", [](emf::World w, const std::vector<std::string>&) {
    EmfCommandSender s{};
    const uint32_t me = emf::command_sender(&s) && s.from_chat ? s.entity : emf::local_player();  // "you" = the sender
    if (const auto p = emf::position(w, me)) EMF_REPLY("you are at %.0f, %.0f, %.0f", p->x, p->y, p->z);
}, EMF_COMMAND_PLAYERS);
```

Any player types `/mymod.where` into the game's chat (a dedicated server, or a game you host with `[chat] players=1`);
the reply reaches only them. Without `EMF_COMMAND_PLAYERS` only the `[chat] admins` may. Mark only commands that read
or act on their sender. Lua: `emf.command("where", ": where am I?", fn, {players = true})`.

## A radial menu on a key
*Tested in game (companion order wheel, villager job wheel).*

```cpp
#include "emf/keys.hpp"

static emf::keys::Edge g_key;
emf::on_tick([](emf::World w) {
    if (!w.is_server() || !g_key.pressed(emf::keys::down('J'))) return;   // only with the game window in front
    emf::wheel("Pick one", {"Day", "Night", "Rabbit (two metres to your right)"}, 'J', EMF_WORLD_SERVER,
               [](emf::World w, int choice) {
                   if (choice < 0) return;  // cancelled
                   emf::api()->notify("you picked %d", choice);
               });
});
```

Holding J when the wheel opens: releasing J picks the highlighted entry. A quick tap: click an entry. Esc or a right
click cancels. Read a key from your ini with `emf::keys::parse_key(L"J")` (0 for mouse buttons and nonsense; an
optional second argument receives a printable name such as "key 0x71").

## What is the player looking at?
*Tested in game (villager_jobs picks villagers and chests, the companion's attack order).*

```cpp
#include "emf/aim.hpp"
#include "emf/query.hpp"

const uint32_t player = emf::local_player();
const auto ray = emf::aim::camera_ray(w, player);  // server world: the host's camera
const auto from = emf::position(w, player);
if (ray && from) {
    std::vector<uint32_t> ids;
    std::vector<emf::aim::Vec3> points;
    for (const auto& n : emf::entities_near(w, *from, 40.0, [&](uint32_t id) { return w.get<gen::Health>(id); })) {
        ids.push_back(n.id);
        points.push_back({n.pos.x, n.pos.y + 0.8, n.pos.z});  // aim at the body, not the feet
    }
    if (auto i = emf::aim::pick(*ray, *from, points, {40.0, 1.5, 0.25})) EMF_LOG("mymod", "looking at %u", ids[*i]);
}
```

`pick` takes the candidate closest to the crosshair ray (within 1.5 m of it), preferring the nearer one on near ties.

## Remember things per world
*`emf::WorldFile` is unit-tested; the same logic runs the companion's and villager_jobs' saved state in game.*

```cpp
#include "emf/storage.hpp"

emf::WorldFile g_file("mymod");  // <game>\emf\config\mymod\<slot>.txt
int g_counter = 0;

// emf_mod_load:
g_file.attach();
emf::on_tick([](emf::World w) {
    if (!w.is_server()) return;
    g_file.poll([](const std::string& text) { g_counter = std::atoi(text.c_str()); });  // once per session
    if (g_file.ready()) g_file.save(std::to_string(g_counter));                        // writes only on change
});
```

It never writes the previous world's data into another world's file, and a crash never leaves the file empty.

## Give your mod settings
*The clock mod works this way (its corner and 12/24 h change at once from the Mods tab).*

`mod.json` next to your DLL:

```json
{
  "id": "mymod", "name": "My Mod", "version": "1.0.0", "emf": 13,
  "settings": [
    {"key": "radius", "type": "int", "default": 10, "min": 2, "max": 50, "label": "Radius (m)"},
    {"key": "key", "type": "key", "default": "J", "label": "Hotkey"},
    {"key": "corner", "type": "choice", "default": "bottom left",
     "choices": ["top right", "bottom right", "bottom left"], "label": "Corner", "live": true}
  ]
}
```

In the mod:

```cpp
std::atomic<int> g_radius{10};
std::atomic<uint32_t> g_corner{EMF_HUD_BOTTOM_LEFT};

void read_settings() {
    g_radius = static_cast<int>(emf::setting_int("mymod", "radius", 10));
    g_corner = static_cast<uint32_t>(emf::setting_int("mymod", "corner", 2));
}

// emf_mod_load:
read_settings();
emf::on_setting_changed("mymod", [](emf::World, const std::string&) { read_settings(); });  // after a world step
const std::string key = emf::setting_text("mymod", "key", "J");               // "J", "F2", "Space"...
const uint32_t vk = emf::keys::parse_key(std::wstring(key.begin(), key.end()));  // emf/keys.hpp
```

Players change them in the console's Mods tab (the key under Esc, then the Mods tab, with the mouse) or by typing
`set mymod radius 20`. Values reach you already checked and clamped. Settings without `"live": true` are shown with
"next start": read them once in `emf_mod_load`.

More kinds: `slider` (`min`, `max`, `step`), `select` (`options`, stored as the option's value: read it with
`emf::setting_text`), `color` (`"#rrggbb"`: `emf::setting_rgba` gives `0xRRGGBBAA` for `emf::hud`), `keybind` (as
`key`; the Mods tab and EMF.exe let the player press the key) and `text` with `maxLength`. `"group": "Display"` puts
settings under a heading, and `"enabledWhen": {"setting": "show", "equals": true}` greys one out while another is
off. The template (`template\`) uses each of them; [reference.md](reference.md) lists the fields.

## Make an NPC guard (follow) an entity
*Tested in game (the companion).*

```cpp
#include <mutex>
std::mutex g_m;
std::vector<std::pair<uint32_t, uint32_t>> g_guard;  // (guard, guarded) queued from on_tick

void my_program(void* ctx) {  // runs inside the server's step
    std::vector<std::pair<uint32_t, uint32_t>> todo;
    {
        std::lock_guard<std::mutex> l(g_m);
        todo.swap(g_guard);
    }
    for (auto [guard, guarded] : todo)
        if (auto* e = emf::emit<gen::StartGuardingEvent>(ctx)) {
            e->guardingEntityId.id = guard;
            e->guardedEntityId.id = guarded;
        }
}
// emf_mod_load:
emf::add_program("mymod_guard", EMF_WORLD_SERVER, {}, {"StartGuardingEvent"}, my_program);
```

The NPC follows its guarded entity and fights what threatens it. Its faction decides whom it attacks: the companion
writes `Faction` 11 (PlayerAlly) right after spawning an enemy body. `AddThreatTargetEvent {attackerId, targetId,
threatValue}` makes it attack something (send it again every few seconds).

## Walk a villager somewhere
*Tested in game (villager_jobs).*

```cpp
// npcId: the villager's ActiveNpcState template data, first 4 bytes (template hash 0x1a8b5909)
uint32_t npc = 0;
if (auto* t = static_cast<const uint8_t*>(emf::api()->get_template_component(w.raw(), 0x1a8b5909u, villager, nullptr)))
    std::memcpy(&npc, t, 4);
// in your program (events_out "NpcCommandEvent"):
if (auto* e = emf::emit<gen::NpcCommandEvent>(ctx)) {
    e->type = gen::NpcCommandType::GoToPosition;
    e->npcId.value = npc;
    e->targetTransform = target_transform;  // e.g. a copy of a chest's CurrentTransform::transform
}
```

The villager roams to near the target for a few seconds, then returns to its routine: repeat the order every few
seconds while it should stay. `NpcCommandType::Reset` moves its home there and teleports it.

## Craft at a villager, into a chest
*Tested in game (villager_jobs).*

```cpp
// in your program (events_out "CraftRecipeEvent"):
if (auto* e = emf::emit<gen::CraftRecipeEvent>(ctx)) {
    e->craftingOperatorId.id = chest;     // inputs come from, and the product goes into, this inventory
    e->craftingStationId.id = villager;   // the station: the villager's workshop (traders: their barter recipes)
    e->recipeId.value = recipe_id;
    e->recipeAmount = 1;
}
```

Nothing happens when the chest lacks inputs or room; watch `PickupEvent` (by = chest) to confirm. The console's
`job.recipes <villager> [chest]` lists recipe ids.

## Harvest a crop into a chest, and keep the plant growing
*Tested in game (villager_jobs farm job).*

```cpp
// in your program (events_out "LootInteractionEvent"): stage = the plant root's DynamicUpgrade.entityId
if (auto* e = emf::emit<gen::LootInteractionEvent>(ctx)) {
    e->guestId.id = chest;  // who receives the loot
    e->hostId.id = stage;
    e->offerId = 0;
    e->lootAll = true;
    e->bypassInteraction = true;
}
// emf_mod_load: when the harvest arrives, re-arm the root in that same step (otherwise the game removes the plant)
emf::on_event<gen::PickupEvent>(EMF_WORLD_SERVER, [](emf::World w, const gen::PickupEvent& e) {
    if (e.pickedUpFromEntityId.id != g_harvested_stage) return;
    if (auto* lv = w.get<gen::Level>(g_root)) lv->dataStorage[0] = 1;
    if (auto* g = w.get<gen::DynamicGrowth>(g_root)) g->nextLevelTime.value = 0;
});
```

A root is ripe when `Level.dataStorage[0]` equals the last level of its `Upgrade` template (see
`mods/villager_jobs/villager_jobs.cpp`, `last_upgrade_level`). The same loot event with `hostId = a farm animal` moves
what the animal produced into the chest; `put_items` fills a feeder.

## Keep some text on screen (a clock, a status line)
*Tested by unit tests; the `clock` mod uses it.*

```cpp
// any thread; the line stays until you change or remove it
emf::hud("mymod.status", "Villagers working: 3");                       // top right
emf::hud("mymod.warning", "Chest full!", EMF_HUD_BOTTOM_RIGHT, 0xff6060ffu);
emf::hud("mymod.warning", "");                                           // gone
```

Set a line only when its text changes (the overlay redraws it every frame anyway). The game's own HUD uses the top
left, the top centre and the bottom centre, and its quest tracker the top right: top-right lines start a quarter down
the screen, below it (a long list of tracked quests can still reach them). Bottom right was free in every test.

## Change a template value (for every entity of that template)
*Tested in game (companion follow distance, big_stacks).*

```cpp
if (auto* tpl = w.get_template<gen::Enemy01BehaviorState>(id))
    emf::write_value(&tpl->guardSetup.startDistance, 20.0f);  // read-only memory: never write through the pointer
```

Lasts until the game restarts; affects wild entities of the same template too. `tpl <entity> <component>` and
`tplset` in the console let you experiment first.

## Change stack sizes

```cpp
const uint32_t logs = emf::api()->find_item(w.raw(), "Material_T1_Wood_Logs");  // in a world callback
emf::set_item_max_stack(logs, 1000);
```

*Tested in game (big_stacks).* Saved stacks keep their size; a game without the mod may then see oversized stacks.

## Find things around a point

```cpp
#include "emf/query.hpp"
if (const auto p = emf::position(w, emf::local_player()))  // no position while the player is not in the world yet
    for (const auto& n : emf::entities_near(w, *p, 15.0, [&](uint32_t id) { return w.get<gen::AnimalSaved>(id) != nullptr; }))
        EMF_LOG("mymod", "an animal %u at %.1f m", n.id, n.distance);
```

The console's `near [radius]` shows the same with tags (player, villager, inventory, animal, plant, health).

## Run code before or after one of the game's programs

```cpp
// emf_mod_load; names from docs/ecs_programs.md. pre/post run on game worker threads inside the step.
emf::api()->hook_program("growth", [](void* ctx, void*) { /* before */ }, [](void* ctx, void*) { /* after */ }, nullptr);
```

*The mechanism is tested (EMF's own hooks); use it for timing, not for reading components (use a mod program).*

## Hook a game function by its bytes
*The `sim_rate` mod works this way (tested in game: it hooks the simulation step that EMF also hooks; both run).*

```cpp
using SimStepFn = void (*)(void* storage, char ticked);  // exactly the original's signature
SimStepFn g_original = nullptr;

void my_detour(void* storage, char ticked) {
    // ... before the game ...
    g_original(storage, ticked);  // the next hook in the chain, then the game
    // ... after ...
}

// emf_mod_load:
if (!emf::hook_pattern("mymod.sim_step", "40 55 53 56 41 55 48 8D AC 24 F8 FF FD FF B8 08 01 02 00 E8 ?? ?? ?? ??",
                       reinterpret_cast<void*>(&my_detour), reinterpret_cast<void**>(&g_original)))
    return false;  // not found or not hookable: the log says why
```

`mymod.patterns` next to the DLL:

```
sim_step = 40 55 53 56 41 55 48 8D AC 24 F8 FF FD FF B8 08 01 02 00 E8 ?? ?? ?? ??
```

After a game update: `emf_sigcheck --patterns mymod.patterns "<game>\enshrouded.exe"`. To read a global the code
uses, follow its RIP-relative address: `emf::find_pattern("48 8B 05 ?? ?? ?? ?? 48 85 C0", 3, true)` gives the
address of the variable `mov rax, [rip+...]` loads.
