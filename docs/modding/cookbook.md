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

Spawned entities are **not saved** with the world (villagers excepted; props spawned inside a base's build zone are saved like placed objects). Props that need a surface (feeders, some
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
writes `Faction` 11 (PlayerAlly) right after spawning an enemy body. To send it at something now, write its pick every
server step: `SelectedTargets.targets[0]` (`targetEntityId` and `targetRootEntityId`) = the target, and
`DynamicAggro.allowTargetSwitches` = false; give the switches back (true) when the order ends. Beastmaster's Attack does
this: the beast is in Combat with that target the same step. `AddThreatTargetEvent` does not pick a target: the game
adds its value to the target's `Threat` attribute, which only players have.

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

## Hits: god mode, PvP off in a zone, a damage meter, scaling by player count
*Lua 1.23 (EMF API v31); the server world: a game you host or a dedicated server. Rules change hits before they land
with no script call per hit; `emf.on_hit` sees hits after they landed. See lua.md, "Hits".*

```lua
-- God mode for one player: a hit cannot take their last point of health (or use cancel = true to drop every hit).
-- Keep the id and call emf.hit_rule_remove(id) when it ends or the player leaves (their entity id is reused). All
-- rules of this script go when it reloads or pauses.
local function god_mode(player) return emf.hit_rule{to = player, min_health = 1} end

-- God mode for all damage: kinds = "all" adds fall, shroud, cold and drowning to the entity hits.
local function god_mode_all(player) return emf.hit_rule{kinds = "all", to = player, min_health = 1} end

-- No fall damage for players (a cancelled fall gives the health back; the hard-landing stagger still plays).
emf.hit_rule{kinds = "fall", to = "players", cancel = true}

-- Half shroud damage for everyone: scale is exact on shroud, cold and drowning (1-point ticks are halved over time).
emf.hit_rule{kinds = "shroud", scale = 0.5}

-- Who is on the server: the player events (the host's own player too). A player's rules go when they leave: the game
-- gives their entity id to the next player who joins.
local online, count = {}, 0
local rules = {}   -- player id -> their PvP-off rule id
emf.on_player_joined(function(world, p) if not online[p.player] then count = count + 1 end; online[p.player] = true end)
emf.on_player_left(function(world, p)
  if online[p.player] then count = count - 1 end
  online[p.player] = nil
  if rules[p.player] then emf.hit_rule_remove(rules[p.player]); rules[p.player] = nil end
end)

-- PvP off inside a zone (a square here): a rule per player while they are inside, removed when they leave it.
emf.on_tick(function(world)
  if not world:is_server() then return end
  for id in pairs(online) do
    local x, y, z = world:position(id)
    local inside = x and math.abs(x - 100) < 50 and math.abs(z - 200) < 50
    if inside and not rules[id] then
      rules[id] = emf.hit_rule{from = "players", to = id, cancel = true}
    elseif not inside and rules[id] then
      emf.hit_rule_remove(rules[id])
      rules[id] = nil
    end
  end
end)
-- A rule matches damage hits only (no healing = true), so a friend's heal still works.

-- A damage meter: who dealt how much (health damage after the hit; blocked hits have amount 0).
local dealt = {}
emf.on_hit(function(world, hit)
  if hit.amount > 0 and hit.attacker ~= 0 then
    dealt[hit.attacker] = (dealt[hit.attacker] or 0) + hit.amount
  end
end)

-- Creatures hit players softer when more players are on the server: replace the rule when the count changes.
local scale_rule, scaled_for = nil, 0
emf.on_tick(function(world)
  if not world:is_server() or count == scaled_for then return end
  if scale_rule then emf.hit_rule_remove(scale_rule) end
  scale_rule = nil
  if count > 1 then  -- a rule with scale 1 does nothing and is refused
    scale_rule = emf.hit_rule{from = "creatures", to = "players", scale = 1 / count}
  end
  scaled_for = count
end)
```

In C++ the same is `emf::add_hit_rule` with an `EmfHitRule` from `emf::hit_rule()`, and `on_hit_before` changes a
hit inline (be fast; reference.md, "v31").

## Buffs: a shrine that gives the Elixir Buff, a cleanse command
*Lua 1.24 (EMF API v32); giving and removing work on the server world: a game you host or a dedicated server. The result
comes after the next step. See lua.md, "Buffs".*

```lua
-- Store ids, not names: names change with the language. Find ids with the console command buffs.list.
local ELIXIR   = 0xFF24634F   -- Elixir Buff (buff, 30 minutes by default)
local POISONED = 0x369C3618
local WET      = 0xAC13C6C3   -- the game gives Wet again at once in rain

-- A shrine: E on an entity you name gives the player the Elixir Buff for 10 minutes. Offer again after a load.
local function shrine(world, entity)
  emf.offer(world, entity, {verb = "Touch"}, function(world, player, entity)
    emf.apply_buff(world, player, ELIXIR, {seconds = 600}, function(world, r)
      if r.kind == "applied" then
        emf.tell(world, player, "The shrine gives you the Elixir Buff for 10 minutes.", {system = true})
      else
        emf.tell(world, player, "The shrine does nothing: " .. r.why, {system = true})   -- the game refused it
      end
    end)
  end)
end
emf.command("shrine", "<entity id>: make an entity a shrine", function(world, args)
  if not world:is_server() then emf.reply("run this on the server world"); return end
  shrine(world, tonumber(args[2]))
end)

-- /cleanse: removes Poisoned and Wet from whoever types it. A remove that finds nothing is refused ("the target does
-- not have it"): that is normal, so only the ones that worked say anything.
emf.command("cleanse", ": remove Poisoned and Wet from you", function(world, args)
  local me = emf.sender().entity
  if not me or me == 0 then emf.reply("no player"); return end
  for _, type in ipairs({POISONED, WET}) do
    emf.remove_buff(world, me, type, function(world, r)
      if r.kind == "removed" then
        emf.tell(world, me, "Cleansed.", {system = true})
      end
    end)
  end
end, {players = true})

-- What does a player have? (any world; seconds_left is nil in the step a buff is given)
local function list(world, player)
  for _, b in ipairs(emf.buffs(world, player)) do
    emf.log(string.format("0x%08X %s %s left", b.type, b.slot, b.seconds_left and ("%.0f s"):format(b.seconds_left) or "?"))
  end
end

-- Hear the players' buff changes (compared after every server step).
emf.on_buff(function(world, ev)
  if ev.kind == "removed" and ev.type == ELIXIR and not ev.cleansed and not ev.replaced then
    emf.tell(world, ev.target, "The Elixir wore off.", {system = true})
  end
end)
```

C++: `api()->buff_apply(w, target, type, duration_ns, source, item, done, user)` and `buff_remove`, with `emf::has_v32()`
(reference.md, "v32: buffs").

## Markers: a spot for every player, a marker over a bounty target

*Checked in game: the camera, the projection and the map screen (plans of 2026-11-11); the recipes are the API's own
use.*

A rally point every player sees, sent from the server (players need EMF on their game, not your mod):

```lua
emf.command("rally", "set a rally point where you stand (admin)", function(world)
  local me = emf.sender().entity
  local x, y, z = world:position(me)
  emf.send_marker(nil, "rally", {x = x, y = y, z = z, height = 2, text = "Rally", colour = 0x40c0ffff,
                                 game_map = true, edge_arrow = true})
end)
emf.command("rally.clear", "take the rally point away (admin)", function()
  emf.unsend_marker(nil, "rally")
end)
```

A marker that follows a creature, for the player who took the bounty only, gone when it dies:

```lua
local bounty = {}  -- creature -> player
function give_bounty(world, player, creature)
  bounty[creature] = player
  local x, y, z = world:position(creature)
  emf.send_marker(player, "bounty." .. creature, {entity = creature, x = x, y = y, z = z, height = 2.5,
                                                   text = "Bounty", shape = "circle", colour = 0xff4040ff,
                                                   edge_arrow = true, game_map = true})
end
emf.on_hit(function(world, hit)
  local player = bounty[hit.target]
  if hit.killed and player then
    emf.unsend_marker(player, "bounty." .. hit.target)
    bounty[hit.target] = nil
  end
end)
```

## Ping a spot for everyone

Players hold Shift and the left mouse button on their map; every player gets a "Ping" marker there for 30 seconds. It
needs EMF on the players' games (not your mod), Lua 1.26 and a server world (a game you host or a dedicated server).

```lua
local pings = {}  -- id -> time it ends
local n = 0

emf.map_action("ping", {label = "Ping here", hold_ms = 600}, function(world, player, x, z, admin)
  emf.ground_at(world, x, z, function(world, ok, gx, gy, gz, why)
    n = n + 1
    local id = "ping" .. n
    emf.send_marker(nil, id, {x = x, z = z, y = ok and gy or 0, text = "Ping", game_map = true, edge_arrow = true})
    pings[id] = os.time() + 30
  end)
end)

emf.on_tick(function(world)
  if not world:is_server() then return end
  local now = os.time()
  for id, ends in pairs(pings) do
    if now >= ends then
      emf.unsend_marker(nil, id)
      pings[id] = nil
    end
  end
end)
```

When the ground is not found, the marker is placed at height 0 here; you could skip it instead. `emf.ground_at` is
described in lua.md, "The map (1.26)".

## A minimap of your own

A canvas in a corner, the game's own map in it centred on the player, and an arrow turned by the heading. Lua 1.27, on
the client only. The sample `mods/minimap` adds zoom keys, settings, the game's map markers and a round frame.

```lua
local S = 200  -- canvas size, in units of a 1080-pixel-high screen
emf.canvas("mini", {corner = "top_right", x = 16, y = 16, w = S, h = S})

emf.on_tick(function(world)
  if not world:is_client() then return end
  local v = emf.player_view()
  if not v then return end
  local c, a = S / 2, math.rad(v.heading)
  local fx, fy = math.sin(a), -math.cos(a)  -- forward on the canvas (0 = north = up)
  emf.canvas_draw("mini", {
    {"rect", 0, 0, S, S, colour = 0x14181ce6},  -- under the map: seen only when the game cannot draw it
    {"map", v.x, v.z, 2},                       -- 2 metres per canvas unit, north up
    {"tri", c + fx * 9, c + fy * 9, c - fx * 6 - fy * 6, c - fy * 6 + fx * 6, c - fx * 6 + fy * 6, c - fy * 6 - fx * 6,
     colour = 0xffe08aff},
    {"rect", 0, 0, S, S, width = 3, colour = 0x0a0a0aff},  -- a frame
  })
end)
```

The map is north up, so only the arrow turns. `emf.fog_at(x, z)` and `emf.map_markers()` give the explored share and the
game's own markers; see lua.md, "Canvases and the minimap (1.27)".

## Removing placed objects (a raid that takes buildings apart)

Three ways to take an object a player placed out of the world. Pick by what the players should see.

- `emf.damage` on an object with `Destructible`: the game breaks it the way enemies do, with loot and fade-out. Use it
  for the raid's own attacks.
- `emf.remove_building` (Lua 1.28): the object goes the way a pick-up does and its item comes back, to a player or to
  the ground. Use it to clean up after a raid or to move a base. It refuses a Flame Altar and a chest that holds items
  (unless `force = true`, which loses the items).
- `emf.destroy`: the object just vanishes, with no item back and no loot. A container's contents go with it. Never use
  it on a Flame Altar: it is the base, and every object of it would be lost at the next save.

```lua
-- a raid's cleanup: take the objects a mod listed down, their items dropped for the owners to pick up
local function take_down(world, ids)
  local n = 0
  for _, id in ipairs(ids) do
    local ok, why = emf.remove_building(world, id, {refund = "drop"})
    if ok then n = n + 1 else emf.log("skipped", id, why) end  -- players, gone ids, altars, full chests
  end
  return n
end
```

## Can a raider get in? (paths)

Before a raid sends a raider at the base, ask the game's path grid whether it can get there. The grid knows terrain,
buildings and doors (Lua 1.29).

- Always pass `closest = true` and check `result.reaches`. Without it a target on a rock or in the air ends `too_long`.
  With it the path ends at the nearest reachable point, and `reaches` is false: that raider is blocked.
- The game's own enemies treat `found` as success and walk to a wall. Check `reaches` yourself.
- The grid exists only near players, and each query is a search shared with every creature. Ask once per raider, not
  every step.

```lua
-- send only the raiders that can get in; the rest wait
local function try_raider(world, raider, raider_pos, altar_pos)
  emf.path_find(world, raider_pos, altar_pos, {closest = true, doors = true, nodes = 5000},
    function(w, result, points)
      if result.reaches then
        start_march(w, raider, points)   -- your own code: walk the points
      else
        emf.log("raider", raider, "is blocked:", result.status, result.length)
      end
    end)
end
```

## A raider that breaks the wall in front of it (blocks)

A raider held up by a wall: find the wall on the line from its chest toward its goal, and crack it every 1.5 s while it
stands there (Night Raids' "Raiders break walls" does this, with a hole low enough to walk through).

```lua
local function crack_wall(world, raider, goal)
  local x, y, z = world:position(raider)
  local dx, dz = goal.x - x, goal.z - z
  local len = math.sqrt(dx * dx + dz * dz)
  if len < 0.1 then return end
  local chest = {x, y + 1.2, z}
  local ahead = {x + dx / len * 4, y + 1.2, z + dz / len * 4}  -- 4 m toward the goal
  local wall = emf.find_wall(world, chest, ahead)
  if not wall then return end                                    -- nothing to break: it is held up by something else
  -- a hole from the floor up: spheres of 1 m from 0.75 m over its feet, 0.7 m higher
  emf.break_blocks(world, wall.x, y + 0.75, wall.z, {radius = 1, height = 0.7}, function(w, r)
    if r.removed > 0 then emf.log("the wall gives way: " .. r.removed .. " blocks") end
  end)
end
```

## A gate that opens for a raid (doors)

A raid mod can open a gate when the attack starts and keep the treasure room shut (Lua 1.30, host or dedicated server).

- `emf.door(world, id)` first: nil means it is not a door, and `logic = true` means a switch drives it (EMF leaves it alone).
- `emf.door_open` and `emf.door_close` swing the door at the next step, and the game's path grid follows, so enemies'
  paths change. `emf.door_lock` takes away the E prompt; from EMF 0.6.8 the lock is kept with the world.
- They return false when refused: check the result, the reason is in the log (not a door, a switch drives it, already
  so, 256 door orders waiting, a lock when 1024 doors are locked already).

```lua
local function open_gate(world, gate, vault_door)
  local state, flags = emf.door(world, gate)
  if state == "closed" and not flags.logic then
    emf.door_open(world, gate, "front")
  end
  emf.door_lock(world, vault_door, true)   -- players cannot open the treasure room with E
end
```

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
