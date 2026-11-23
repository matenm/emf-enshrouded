#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "common/fnv.h"
#include "core/api.h"
#include "core/builtin_commands.h"
#include "core/commands.h"
#include "core/daytime.h"
#include "core/game_text.h"
#include "core/emf_settings.h"
#include "core/inventory.h"
#include "core/items.h"
#include "core/manifest.h"
#include "core/path_glue.h"
#include "core/remove_building.h"
#include "core/spawn.h"
#include "emf/generated/components.hpp"
#include "fake_world.h"

using namespace emf;
using namespace emf::core;
using emf::test::FakeItemRegistry;
using emf::test::FakeWorld;
using emf::test::MapStore;
using emf::test::SingletonStore;

namespace {
std::vector<std::string> g_bout;
bool bsaid(const std::string& needle) {
    for (const auto& l : g_bout)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}
int64_t g_player_xf[7] = {int64_t(5) << 32, int64_t(905) << 32, int64_t(1171) << 32, 0, 0, 0, 0};
void* b_gc(void* out, void*, uint32_t hash, uint32_t entity) {
    auto o = static_cast<uint64_t*>(out);
    o[0] = o[1] = 0;
    if (hash == off::kHashCurrentTransform && entity == 42) {
        o[0] = reinterpret_cast<uint64_t>(g_player_xf);
        o[1] = 56;
    }
    static uint8_t other_player_state[96] = {};
    if (hash == emf::gen::ecs::PlayerState::kHash && entity == 50) {  // another player in the world
        o[0] = reinterpret_cast<uint64_t>(other_player_state);
        o[1] = 96;
    }
    return out;
}
uint8_t g_tpl_npc[456] = {};
void* b_tpl(uint8_t*, uint32_t hash, uint32_t entity) {
    return hash == 0x1a8b5909u /* ActiveNpcState */ && entity == 42 ? g_tpl_npc : nullptr;
}  // (the game's getTemplateComponent; the size comes from the registry, unknown in these fake worlds)
int g_bspawns = 0;
float g_bspawn_qy = 0, g_bspawn_qw = 1;  // the last spawn's rotation (CurrentTransform's quaternion y and w)
std::vector<uint32_t> g_pickup_amounts;
uint16_t g_pickup_fails = 0;  // != 0: a pickup of this amount is not created (the game's spawn gives no entity)
uint32_t b_spawn(void*, const void*, const SpawnSlice* comps) {
    ++g_bspawns;
    if (comps->count && comps->ptr[0].size == sizeof(WorldTransformData)) {
        WorldTransformData xf{};
        std::memcpy(&xf, comps->ptr[0].data, sizeof xf);
        g_bspawn_qy = xf.qy;
        g_bspawn_qw = xf.qw;
    }
    for (uint64_t i = 0; i < comps->count; ++i)
        if (comps->ptr[i].hash == kHashPickupItem) {
            uint16_t a = 0;
            std::memcpy(&a, static_cast<const uint8_t*>(comps->ptr[i].data) + 26, 2);
            if (g_pickup_fails && a == g_pickup_fails) return 0;
            g_pickup_amounts.push_back(a);
        }
    return 7000 + g_bspawns;
}
std::vector<uint32_t> g_destroyed;
uint64_t b_destroy(void*, uint32_t entity, char, uint64_t) {
    g_destroyed.push_back(entity);
    return 0;
}
int64_t g_set_tod = -1;
void b_set_tod(void* daytime, int64_t tod, int64_t) {
    g_set_tod = tod;
    std::memcpy(static_cast<uint8_t*>(daytime) + kDaytimeTimeOfDay, &tod, 8);
}

struct MemIni : IniBackend {
    std::map<std::string, std::string> values;
    std::optional<std::string> read(const std::string& mod, const std::string& key) override {
        auto it = values.find(mod + "." + key);
        if (it == values.end()) return std::nullopt;
        return it->second;
    }
    bool write(const std::string& mod, const std::string& key, const std::string& value) override {
        values[mod + "." + key] = value;
        return true;
    }
    bool erase(const std::string& mod, const std::string& key) override {
        values.erase(mod + "." + key);
        return true;
    }
};

struct BFixture {
    CoreState state;
    CommandCenter center;
    BuiltinContext ctx;
    ModRegistry registry;
    MemIni ini;
    std::unique_ptr<SettingsStore> settings;
    BFixture() {
        state.get_component = b_gc;
        state.get_template_component = b_tpl;
        state.log_line = [](const std::string&) {};
        state.now_ms = [] { return uint64_t(1000); };
        state.sigs[static_cast<size_t>(Sig::SpawnWrapper)] = Resolved{reinterpret_cast<uintptr_t>(&b_spawn), 1, false};
        state.sigs[static_cast<size_t>(Sig::SetTimeOfDay)] = Resolved{reinterpret_cast<uintptr_t>(&b_set_tod), 1, false};
        state.sigs[static_cast<size_t>(Sig::DestroyEntity)] = Resolved{reinterpret_cast<uintptr_t>(&b_destroy), 1, false};
        state.known_build = false;
        set_core_state(&state);
        ctx.api = api_table();
        ctx.center = &center;
        ctx.player = [] { return uint32_t(42); };
        ModRecord a;
        a.id = "sample_mod";
        a.name = "sample_mod";
        a.source = "sample_mod.dll";
        ModRecord b;
        b.id = "companion";
        b.name = "Companion";
        b.version = "1.2.0";
        b.source = "companion\\";
        b.has_manifest = true;
        ModRecord c;
        c.id = "big_stacks";
        c.name = "Big Stacks";
        c.version = "1.0.0";
        c.has_manifest = true;
        c.status = ModStatus::Disabled;
        c.reason = "off by default";
        registry.set({a, b, c});
        settings = std::make_unique<SettingsStore>(&ini);
        SettingDef place;
        place.key = "place";
        place.type = SettingType::Choice;
        place.choices = {"top right", "bottom right", "bottom left"};
        place.default_value = "1";
        place.label = "Corner";
        place.live = true;
        settings->declare("companion", place);
        SettingDef radius;
        radius.key = "radius";
        radius.type = SettingType::Int;
        radius.default_value = "10";
        radius.min = 2;
        radius.max = 50;
        radius.has_min = radius.has_max = true;
        settings->declare("companion", radius);
        ctx.registry = &registry;
        ctx.settings = settings.get();
        REQUIRE(add_builtin_commands(ctx));
        g_bout.clear();
        g_bspawns = 0;
        g_pickup_amounts.clear();
        g_set_tod = -1;
        g_destroyed.clear();
    }
    ~BFixture() { set_core_state(nullptr); }
    // Runs the queued lines inside a dispatcher callback on `world`, like the core does on the server tick.
    void run(FakeWorld& world) {
        struct Ctx {
            CommandCenter* c;
        } x{&center};
        state.dispatcher.post(EMF_WORLD_SERVER, [](EmfWorld* w, void* u) {
            static_cast<Ctx*>(u)->c->run_pending(w, [](const std::string& s) { g_bout.push_back(s); });
        }, &x);
        state.dispatcher.run(reinterpret_cast<EmfWorld*>(world.p()), EMF_WORLD_SERVER, [](const std::string&) {});
    }
};
struct ItemsWorld {
    FakeWorld w;
    SingletonStore s;
    FakeItemRegistry reg;
    ItemsWorld() {
        w.put<uint8_t>(off::kKind, 0);
        reg.add(0x11, 10, 250, "Material_T1_Wood_Logs");
        reg.add(0x22, 40, 1, "Torch");
        s.plant(w, "g38_items::ItemRegistry", reg.reg);
    }
};
}  // namespace

TEST_CASE("builtin commands: help and mods list what exists") {
    BFixture f;
    FakeWorld w;
    f.center.enqueue("help");
    f.center.enqueue("mods");
    f.run(w);
    CHECK(bsaid("give <item name> [count]"));
    CHECK(bsaid("time <hours 0-24>"));
    CHECK(bsaid("companion 1.2.0 (Companion): loaded"));
    CHECK(bsaid("sample_mod (sample_mod.dll): loaded"));
    CHECK(bsaid("big_stacks 1.0.0 (Big Stacks): disabled (off by default)"));
    CHECK(bsaid("3 mod(s): 2 loaded, 1 switched off, 0 not loaded"));
}

namespace {
void noop_cmd(EmfWorld*, int, const char* const*, void*) {}
}  // namespace

TEST_CASE("builtin commands: help is for every player; a player who is no admin sees only the commands they may run") {
    BFixture f;
    FakeWorld w;
    REQUIRE(f.center.add("raids.status", ": the raid in this world", noop_cmd, nullptr, nullptr, false, kCommandForPlayers));
    CHECK(f.center.for_players("help"));
    CommandSender guest;
    guest.from_chat = true;
    guest.account = 0xbb;
    guest.restricted = true;
    f.center.enqueue("help", GetTickCount64(), guest);
    f.run(w);
    CHECK(bsaid("raids.status : the raid in this world"));
    CHECK_FALSE(bsaid("give <item name>"));
    CHECK(bsaid("commands you may run here, page 1 of 1"));
    g_bout.clear();
    f.center.enqueue("help");  // the console: everything, the players' ones marked
    f.run(w);
    CHECK(bsaid("give <item name> [count]"));
    CHECK(bsaid("raids.status : the raid in this world (any player)"));
}

TEST_CASE("builtin commands: help in the chat comes in pages of 8 (/help 2 ...), not cut off at 10 lines") {
    BFixture f;
    FakeWorld w;
    for (int i = 0; i < 12; ++i)
        REQUIRE(f.center.add("mod.cmd" + std::to_string(10 + i), "", noop_cmd, nullptr, nullptr, false, kCommandForPlayers));
    CommandSender guest;
    guest.from_chat = true;
    guest.restricted = true;
    f.center.enqueue("help", GetTickCount64(), guest);
    f.run(w);
    CHECK(bsaid("commands you may run here, page 1 of 2 (/help 2 for more)"));
    CHECK(bsaid("help "));        // help itself is one of them
    CHECK(bsaid("mod.cmd16"));
    CHECK_FALSE(bsaid("mod.cmd17"));  // 8 per page: help + mod.cmd10..16
    g_bout.clear();
    f.center.enqueue("help 2", GetTickCount64(), guest);
    f.run(w);
    CHECK(bsaid("page 2 of 2"));
    CHECK(bsaid("mod.cmd17"));
    CHECK(bsaid("mod.cmd21"));
    CHECK_FALSE(bsaid("mod.cmd16"));
    g_bout.clear();
    f.center.enqueue("help 9", GetTickCount64(), guest);
    f.run(w);
    CHECK(bsaid("page 2 of 2"));  // past the end: the last page
    g_bout.clear();
    f.center.enqueue("help");  // the console lists everything at once
    f.run(w);
    CHECK(bsaid("mod.cmd21"));
    CHECK(bsaid("give <item name>"));
}

TEST_CASE("builtin commands: settings lists a mod's settings, set changes them") {
    BFixture f;
    FakeWorld w;
    f.center.enqueue("settings companion");
    f.center.enqueue("set companion place bottom left");
    f.center.enqueue("set companion radius 99");
    f.center.enqueue("set companion nope 1");
    f.center.enqueue("set companion enabled 0");
    f.center.enqueue("settings ghost");
    f.center.enqueue("settings sample_mod");
    f.run(w);
    CHECK(bsaid("place = 1 (bottom right)"));
    CHECK(bsaid("Corner"));
    CHECK(bsaid("choice: 0 top right, 1 bottom right, 2 bottom left"));
    CHECK(bsaid("radius = 10"));
    CHECK(bsaid("2 to 50"));
    CHECK(bsaid("companion.place = 2 (bottom left)"));
    CHECK(f.ini.values["companion.place"] == "2");
    CHECK(bsaid("must be between 2 and 50"));
    CHECK(bsaid("no setting \"nope\""));
    CHECK(bsaid("companion is switched off from the next game start"));
    CHECK(f.ini.values["companion.enabled"] == "0");
    CHECK(bsaid("no mod \"ghost\""));
    CHECK(bsaid("sample_mod has no settings"));
}

TEST_CASE("builtin commands: reset puts one setting, or every setting of a mod (never its switch), back to its default") {
    BFixture f;
    FakeWorld w;
    f.center.enqueue("set companion radius 20");
    f.center.enqueue("set companion place bottom left");
    f.center.enqueue("set companion enabled 0");
    f.run(w);
    REQUIRE(f.ini.values["companion.radius"] == "20");
    f.center.enqueue("reset companion radius");
    f.run(w);
    CHECK(bsaid("companion.radius = 10 (the default)"));
    CHECK(f.ini.values.count("companion.radius") == 0);
    f.center.enqueue("reset companion");
    f.run(w);
    CHECK(bsaid("companion: 1 setting back to its default (place = 1 (bottom right))"));
    CHECK(f.ini.values.count("companion.place") == 0);
    CHECK(f.ini.values["companion.enabled"] == "0");  // its switch stays
    f.center.enqueue("reset companion");
    f.run(w);
    CHECK(bsaid("companion: every setting is at its default"));
    f.center.enqueue("reset companion nope");
    f.center.enqueue("reset ghost");
    f.center.enqueue("reset");
    f.run(w);
    CHECK(bsaid("no setting \"nope\""));
    CHECK(bsaid("no mod \"ghost\""));
    CHECK(bsaid("usage: reset <mod> [key]"));
}

TEST_CASE("builtin commands: set emf hud.size 120 changes EMF's own setting; settings emf lists its pages") {
    BFixture f;
    FakeWorld w;
    for (SettingDef d : emf_setting_defs({}))
        REQUIRE(f.settings->declare(kEmfSettingsId, std::move(d), nullptr, SettingsStore::Declare::Keep));
    f.center.enqueue("set emf hud.size 120");
    f.center.enqueue("set emf hud.size 999");
    f.center.enqueue("set emf overlay.enabled 0");
    f.center.enqueue("settings emf");
    f.run(w);
    CHECK(bsaid("emf.hud.size = 120"));
    CHECK(f.ini.values["emf.hud.size"] == "120");
    CHECK(bsaid("must be between 60 and 200"));
    CHECK(bsaid("no setting \"overlay.enabled\""));  // switching the overlay off stays in the ini (lockout)
    CHECK(bsaid("Text on screen"));
    CHECK(bsaid("Start-up"));
}

TEST_CASE("builtin commands: settings shows a select's options, steps, max length, groups and enabledWhen") {
    BFixture f;
    FakeWorld w;
    auto r = parse_manifest(R"({"id": "big_stacks", "version": "1", "settings": [
        {"key": "show", "type": "bool", "default": false, "label": "Show it"},
        {"key": "corner", "type": "select", "default": "tr", "group": "Display",
         "options": [{"value": "tr", "label": "Top right"}, {"value": "bl", "label": "Bottom left"}],
         "enabledWhen": {"setting": "show", "equals": true}},
        {"key": "speed", "type": "slider", "min": 0, "max": 2, "step": 0.5, "default": 1},
        {"key": "name", "type": "text", "maxLength": 12}]})");
    REQUIRE_MESSAGE(r.manifest, r.error);
    for (const auto& d : r.manifest->settings) REQUIRE(f.settings->declare("big_stacks", d));
    f.center.enqueue("settings big_stacks");
    f.center.enqueue("set big_stacks corner Bottom left");
    f.center.enqueue("set big_stacks speed 1.3");
    f.run(w);
    CHECK(bsaid("corner = tr (Top right)  [select: tr (Top right), bl (Bottom left)"));
    CHECK(bsaid("(Display)"));
    CHECK(bsaid("    only while Show it is on (greyed out in the Mods tab now)"));
    CHECK(bsaid("0 to 2, steps of 0.5, slider"));
    CHECK(bsaid("at most 12 characters"));
    CHECK(bsaid("big_stacks.corner = bl (Bottom left)"));
    CHECK(bsaid("big_stacks.speed = 1.5"));
}

TEST_CASE("builtin commands: items filters by name") {
    BFixture f;
    ItemsWorld iw;
    f.center.enqueue("items wood");
    f.run(iw.w);
    CHECK(bsaid("Material_T1_Wood_Logs"));
    CHECK_FALSE(bsaid("Torch"));
    CHECK(bsaid("1 of 2 items"));
}

TEST_CASE("builtin commands: give drops the item at the player; bad input gets a usage line") {
    BFixture f;
    ItemsWorld iw;
    f.center.enqueue("give Material_T1_Wood_Logs 3");
    f.center.enqueue("give torch");
    f.center.enqueue("give Nothing 1");
    f.center.enqueue("give Torch many");
    f.center.enqueue("give");
    f.run(iw.w);
    CHECK(g_pickup_amounts == std::vector<uint32_t>{3, 1});
    CHECK(bsaid("gave 3 x Material_T1_Wood_Logs"));
    CHECK(bsaid("unknown item 'Nothing'"));
    CHECK(bsaid("usage: give <item name> [count]"));
}

namespace {
struct FriendlyItemsWorld : ItemsWorld {
    FriendlyItemsWorld() {
        reg.add(0x13, 50, 250, "Material_T1_Wood_Logs_Dead");
        reg.add(0x14, 60, 20, "Material_T0_simple_FishingBait");
        reg.add(0x15, 70, 20, "Food_T2_Dog_Bait");
        reg.add(0x16, 80, 1, "Tool_Torch01");  // real names that end in a number
        reg.add(0x17, 90, 1, "Weapon_T1_1H_Sword_01");
        reg.add(0x18, 100, 1, "Weapon_T1_2H_Bow_01");
    }
};
}  // namespace

TEST_CASE("builtin commands: give and stack take item names as people type them: spaces, any case, part of a name") {
    BFixture f;
    FriendlyItemsWorld iw;
    f.state.tracker.on_tick(reinterpret_cast<EmfWorld*>(iw.w.p()), EMF_WORLD_SERVER, 1000);
    f.center.enqueue("give fishing bait 4");
    f.center.enqueue("give \"Wood Logs\" 2");  // quotes work too; not ..._Dead, which has a word more
    f.center.enqueue("give bait");             // two baits: none is given, both are named
    f.center.enqueue("give hardwood 2");
    f.center.enqueue("stack dead logs 500");
    f.center.enqueue("give tool torch 01 5");  // the count is the number at the end
    f.run(iw.w);
    CHECK(g_pickup_amounts == std::vector<uint32_t>{4, 2, 5});
    CHECK(bsaid("gave 5 x Tool_Torch01"));
    CHECK(bsaid("gave 4 x Material_T0_simple_FishingBait"));
    CHECK(bsaid("gave 2 x Material_T1_Wood_Logs to"));
    CHECK(bsaid("'bait' could be 2 items: Food_T2_Dog_Bait, Material_T0_simple_FishingBait"));
    CHECK(bsaid("unknown item 'hardwood'"));
    CHECK(bsaid("Material_T1_Wood_Logs_Dead: stack 250 -> 500"));
}

TEST_CASE("builtin commands: items finds items by words in any order, best match first") {
    BFixture f;
    FriendlyItemsWorld iw;
    f.center.enqueue("items logs wood");
    f.run(iw.w);
    CHECK(bsaid("2 of 8 items"));
    const auto at = [](const std::string& start) {
        for (size_t i = 0; i < g_bout.size(); ++i)
            if (g_bout[i].find(start) != std::string::npos) return i;
        return g_bout.size();
    };
    CHECK(at("Material_T1_Wood_Logs (") < at("Material_T1_Wood_Logs_Dead ("));
    CHECK(at("Material_T1_Wood_Logs_Dead (") < g_bout.size());
    CHECK(at("Torch") == g_bout.size());
}

TEST_CASE("builtin commands: put and take take item names as people type them; the numbers come after the name") {
    BFixture f;
    FriendlyItemsWorld iw;
    InventoryService::Deps deps;
    deps.after_step = [](std::function<void(EmfWorld*)>) {};
    deps.fns.remove_items = reinterpret_cast<RemoveItemsFn>(0x1);  // present (not called: no program runs here)
    InventoryService service(std::move(deps));
    f.state.inventory = &service;
    f.center.enqueue("put Fishing Bait 3 77");
    f.center.enqueue("take dog bait 2");
    f.run(iw.w);
    REQUIRE(service.pending() == 2);
    CHECK(service.pending_at(0).item == 0x14);
    CHECK(service.pending_at(0).count == 3);
    CHECK(service.pending_at(0).entity == 77);
    CHECK(service.pending_at(1).item == 0x15);
    CHECK(service.pending_at(1).count == 2);
    service.world_reset();
    f.state.inventory = nullptr;
}

TEST_CASE("builtin commands: take and put never read a number of the item's name as the entity") {
    BFixture f;
    FriendlyItemsWorld iw;
    InventoryService::Deps deps;
    deps.after_step = [](std::function<void(EmfWorld*)>) {};
    deps.fns.remove_items = reinterpret_cast<RemoveItemsFn>(0x1);  // present (not called: no program runs here)
    InventoryService service(std::move(deps));
    f.state.inventory = &service;
    f.center.enqueue("take tool torch 01 5");         // 5 torches from you, not 1 from entity 5
    f.center.enqueue("put weapon t1 1h sword 01 3");  // 3 swords into your bag, not 1 into entity 3
    f.center.enqueue("give weapon t1 2h bow 01");
    f.run(iw.w);
    REQUIRE(service.pending() == 2);
    CHECK(service.pending_at(0).item == 0x16);
    CHECK(service.pending_at(0).count == 5);
    CHECK(service.pending_at(0).entity == 42);
    CHECK(service.pending_at(1).item == 0x17);
    CHECK(service.pending_at(1).count == 3);
    CHECK(service.pending_at(1).entity == 42);
    CHECK(g_pickup_amounts == std::vector<uint32_t>{1});
    CHECK(bsaid("gave 1 x Weapon_T1_2H_Bow_01"));
    service.world_reset();
    f.state.inventory = nullptr;
}

TEST_CASE("builtin commands: words that can be read as two different items give nothing and say how to type each") {
    BFixture f;
    ItemsWorld iw;
    iw.reg.add(0x31, 90, 10, "Torch_2");
    f.center.enqueue("give torch 2");  // 1 x Torch_2, or 2 x Torch
    f.run(iw.w);
    CHECK(g_pickup_amounts.empty());
    CHECK(bsaid("'torch 2' can be read more than one way; type one of: give Torch_2 | give Torch 2"));
}

TEST_CASE("builtin commands: spawn places count copies near the player") {
    BFixture f;
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 0);
    f.center.enqueue("spawn 9543cfb0-b4d7-408e-905c-ccb79a15136d 2");
    f.center.enqueue("spawn 9543cfb0-b4d7-408e-905c-ccb79a15136d two");
    f.center.enqueue("spawn 9543cfb0-b4d7-408e-905c-ccb79a15136d 99");
    f.run(w);
    CHECK(g_bspawns == 2);
    CHECK(bsaid("spawned 2"));
    CHECK(bsaid("usage: spawn <item name | template guid> [count 1-20] [at <x> <y> <z>] [yaw <degrees>]"));
}

TEST_CASE("builtin commands: spawn at a place: no player needed (a dedicated server's console)") {
    BFixture f;
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 0);
    f.center.enqueue("spawn 9543cfb0-b4d7-408e-905c-ccb79a15136d at 3738.5 843.75 1428");
    f.center.enqueue("spawn 9543cfb0-b4d7-408e-905c-ccb79a15136d 3 at -10 0 5.5");
    f.center.enqueue("spawn 9543cfb0-b4d7-408e-905c-ccb79a15136d at 1 2");      // no z
    f.center.enqueue("spawn 9543cfb0-b4d7-408e-905c-ccb79a15136d 2 near 1 2 3");  // not "at"
    f.center.enqueue("spawn 9543cfb0-b4d7-408e-905c-ccb79a15136d at 1 two 3");
    f.run(w);
    CHECK(g_bspawns == 4);
    CHECK(bsaid("spawned 1"));
    CHECK(bsaid("spawned 3"));
    CHECK(bsaid("usage: spawn <item name | template guid> [count 1-20] [at <x> <y> <z>] [yaw <degrees>]"));
    CHECK_FALSE(bsaid("player position unknown"));
}

TEST_CASE("builtin commands: spawn ... yaw <degrees> spawns it turned (v47 spawn_facing), anywhere among the words") {
    BFixture f;
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 0);
    f.center.enqueue("spawn 9543cfb0-b4d7-408e-905c-ccb79a15136d at 1 2 3 yaw 90");
    f.run(w);
    CHECK(g_bspawns == 1);
    CHECK(bsaid("spawned 1"));
    CHECK(g_bspawn_qy == doctest::Approx(0.70710678f));
    CHECK(g_bspawn_qw == doctest::Approx(0.70710678f));
    f.center.enqueue("spawn 9543cfb0-b4d7-408e-905c-ccb79a15136d yaw -90 at 1 2 3");
    f.run(w);
    CHECK(g_bspawns == 2);
    CHECK(g_bspawn_qy == doctest::Approx(-0.70710678f));
    f.center.enqueue("spawn 9543cfb0-b4d7-408e-905c-ccb79a15136d at 1 2 3 yaw north");
    f.center.enqueue("spawn 9543cfb0-b4d7-408e-905c-ccb79a15136d at 1 2 3 yaw");
    f.run(w);
    CHECK(g_bspawns == 2);
    CHECK(bsaid("usage: spawn <item name | template guid> [count 1-20] [at <x> <y> <z>] [yaw <degrees>]"));
}

TEST_CASE("builtin commands: time sets the time of day; pos and entities report") {
    BFixture f;
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 0);
    SingletonStore s;
    alignas(8) static uint8_t daytime[0x100] = {};
    s.plant(w, "g38_daytime::Daytime", daytime);
    MapStore ents;
    emf::test::plant_entities(w, ents, {1, 2, 42});
    f.center.enqueue("time 12");
    f.center.enqueue("time 25");
    f.center.enqueue("pos");
    f.center.enqueue("entities");
    f.run(w);
    CHECK(g_set_tod == 12ll * 3600ll * 1000000000ll);
    CHECK(bsaid("time of day is now 12.00 h"));
    CHECK(bsaid("usage: time <hours 0-24>"));
    CHECK(bsaid("player 42 at (5.0, 905.0, 1171.0)"));
    CHECK(bsaid("3 entities"));
}

TEST_CASE("daytime: set_time_of_day passes the absolute time and resets the weather layers") {
    alignas(8) uint8_t daytime[0x100] = {};
    alignas(8) uint8_t weather[0x100] = {};
    alignas(8) int64_t layers[3 * 3] = {5, 0, 0, 6, 0, 0, 7, 0, 0};
    void* lp = layers;
    const uint64_t n = 3;
    std::memcpy(weather + kWeatherLayers, &lp, 8);
    std::memcpy(weather + kWeatherLayerCount, &n, 8);
    g_set_tod = -1;
    auto r = set_time_of_day(daytime, weather, b_set_tod, 1000, 6.5, false);  // an unknown build: weather left alone
    CHECK(r.ok);
    CHECK(r.layers_reset == 0);
    CHECK(layers[0] == 5);
    r = set_time_of_day(daytime, weather, b_set_tod, 1000, 6.5, true);
    CHECK(g_set_tod == static_cast<int64_t>(6.5 * 3600e9));
    CHECK(r.layers_reset == 3);
    CHECK(layers[0] == 0);
    CHECK(layers[6] == 0);
}

TEST_CASE("builtin commands: time on an unknown build skips the weather; unconfirmed tick fields refuse it") {
    BFixture f;  // the fixture is an unknown build
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 0);
    SingletonStore s;
    alignas(8) static uint8_t daytime[0x100] = {};
    s.plant(w, "g38_daytime::Daytime", daytime);
    f.center.enqueue("time 9");
    f.run(w);
    CHECK(bsaid("weather not reset (a game build EMF does not know)"));
    g_bout.clear();
    g_set_tod = -1;
    f.state.tick_offsets_ok = false;
    f.center.enqueue("time 10");
    f.run(w);
    CHECK(g_set_tod == -1);
    CHECK(bsaid("time: the world's tick fields are not confirmed on this game build"));
    w.put<int64_t>(off::kTickPeriodNs, 16666667);
    w.put<uint32_t>(off::kTick, 600);
    auto* ew = reinterpret_cast<EmfWorld*>(w.p());
    CHECK(f.ctx.api->ecs_time_ns(ew) == 0);  // world time from unconfirmed fields: not given out
    CHECK(f.ctx.api->world_tick(ew) == 0);
    f.state.tick_offsets_ok = true;
    CHECK(f.ctx.api->ecs_time_ns(ew) == 16666667ll * 600);
    CHECK(f.ctx.api->world_tick(ew) == 600);
}

// ---- inspect, tpl, stack ----
TEST_CASE("builtin commands: inspect lists an entity's components (dynamic and template); unknown entity says so") {
    BFixture f;
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 0);
    f.center.enqueue("inspect 42");
    f.center.enqueue("inspect 999");
    f.center.enqueue("inspect");
    f.run(w);
    CHECK(bsaid("CurrentTransform (56 B)"));
    CHECK(bsaid("ActiveNpcState (template, size unknown)"));
    CHECK(bsaid("entity 42: 2 components (1 template)"));
    CHECK(bsaid("entity 999: no components"));
    CHECK(bsaid("usage: inspect <entity>"));
}

TEST_CASE("builtin commands: tpl dumps template bytes; unknown component, missing data and bad input reply") {
    BFixture f;
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 0);
    for (int i = 0; i < 456; ++i) g_tpl_npc[i] = static_cast<uint8_t>(i);
    f.center.enqueue("tpl 42 activenpcstate 20");
    f.center.enqueue("tpl 42 ActiveNpcState");
    f.center.enqueue("tpl 42 NoSuchThing");
    f.center.enqueue("tpl 42 CurrentTransform");
    f.center.enqueue("tpl 42 ActiveNpcState 999");
    f.center.enqueue("tpl x");
    f.run(w);
    CHECK(bsaid("+0x000: 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f"));
    CHECK(bsaid("+0x010: 10 11 12 13"));
    CHECK(bsaid("+0x010: 10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f"));
    CHECK(bsaid("unknown component 'NoSuchThing'"));
    CHECK(bsaid("entity 42 has no template data for CurrentTransform"));
    CHECK(bsaid("usage: tpl <entity> <component> [bytes 1-256] [offset]"));
}

TEST_CASE("builtin commands: stack changes an item's stack size; bad input gets a usage line") {
    BFixture f;
    ItemsWorld iw;
    f.state.tracker.on_tick(reinterpret_cast<EmfWorld*>(iw.w.p()), EMF_WORLD_SERVER, 1000);
    f.center.enqueue("stack Material_T1_Wood_Logs 1000");
    f.center.enqueue("stack Torch 0");
    f.center.enqueue("stack Nothing 5");
    f.center.enqueue("stack Torch");
    f.run(iw.w);
    CHECK(bsaid("Material_T1_Wood_Logs: stack 250 -> 1000"));
    CHECK(emf::core::item_by_id(iw.reg.reg, 0x11)->max_stack == 1000);
    CHECK(bsaid("usage: stack <item name> <max 1-65535>"));
    CHECK(bsaid("unknown item 'Nothing'"));
}

TEST_CASE("builtin commands: tpl takes an offset; tplset writes one value into template data and reports old and new") {
    BFixture f;
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 0);
    for (int i = 0; i < 456; ++i) g_tpl_npc[i] = static_cast<uint8_t>(i);
    f.center.enqueue("tpl 42 ActiveNpcState 4 0x10");
    f.center.enqueue("tplset 42 ActiveNpcState 0x20 f32 6.5");
    f.center.enqueue("tplset 42 ActiveNpcState 0x30 u8 255");
    f.center.enqueue("tplset 42 ActiveNpcState 0x1c6 u32 1");  // runs past the end (456 B)
    f.center.enqueue("tplset 42 ActiveNpcState 0x20 f64 1");
    f.center.enqueue("tplset 42 ActiveNpcState 0x20 u8 300");
    f.center.enqueue("tplset 42 CurrentTransform 0 u8 1");
    f.run(w);
    CHECK(bsaid("+0x010: 10 11 12 13"));
    float v = 0;
    std::memcpy(&v, g_tpl_npc + 0x20, 4);
    CHECK(v == 6.5f);
    CHECK(bsaid("ActiveNpcState+0x020 of entity 42: f32"));
    CHECK(bsaid("-> 6.5"));
    CHECK(g_tpl_npc[0x30] == 255);
    CHECK(bsaid("offset out of range"));
    CHECK(bsaid("usage: tplset <entity> <component> <offset> <f32|i32|u32|u16|u8> <value>"));
    CHECK(bsaid("bad value '300' for u8"));
    CHECK(bsaid("entity 42 has no template data for CurrentTransform"));
}

TEST_CASE("builtin commands: destroy removes an entity, never a player or one that is not there") {
    BFixture f;
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 0);
    MapStore ents;
    emf::test::plant_entities(w, ents, {7, 42, 50});
    f.center.enqueue("destroy 7");
    f.center.enqueue("destroy 42");   // the local player
    f.center.enqueue("destroy 50");   // another player (has PlayerState)
    f.center.enqueue("destroy 999");  // no such entity
    f.center.enqueue("destroy 0");    // entity 0 is nobody
    f.center.enqueue("destroy");
    f.center.enqueue("destroy x");
    f.run(w);
    CHECK(g_destroyed == std::vector<uint32_t>{7});
    CHECK(bsaid("destroyed entity 7"));
    CHECK(bsaid("entity 42 is a player: not destroyed"));
    CHECK(bsaid("entity 50 is a player: not destroyed"));
    CHECK(bsaid("no entity 999 in this world"));
    size_t usage = 0;  // "destroy", "destroy x" and "destroy 0" (entity 0 is nobody)
    for (const auto& l : g_bout) usage += l.find("usage: destroy <entity>") != std::string::npos;
    CHECK(usage == 3);
    CHECK(bsaid("usage: destroy <entity>"));
}

TEST_CASE("builtin commands: put queues items for your inventory and replies when the server tick ran them") {
    BFixture f;
    ItemsWorld iw;
    std::vector<std::function<void(EmfWorld*)>> after;
    InventoryService::Deps deps;
    deps.after_step = [&after](std::function<void(EmfWorld*)> fn) { after.push_back(std::move(fn)); };
    InventoryService service(std::move(deps));
    f.state.inventory = &service;
    f.center.enqueue("put Material_T1_Wood_Logs 3");
    f.center.enqueue("put Torch 1 77");
    f.center.enqueue("put Nothing_Like_This");
    f.center.enqueue("put Torch 0");
    f.center.enqueue("put");
    f.run(iw.w);
    CHECK(service.pending() == 2);
    CHECK(bsaid("unknown item 'Nothing_Like_This'"));
    CHECK(bsaid("usage: put <item name> [count 1-65535] [entity]"));
    service.world_reset();  // (no program in this test) the requests end with 0 after the next step
    g_bout.clear();
    for (auto& j : after) j(reinterpret_cast<EmfWorld*>(iw.w.p()));  // the results go where the commands' replies went
    REQUIRE(g_bout.size() == 2);
    CHECK(g_bout[0] == "put 3 x Material_T1_Wood_Logs into entity 42: 0 added (a new world was set up)");  // the reason too
    CHECK(g_bout[1] == "put 1 x Torch into entity 77: 0 added (a new world was set up)");
    f.state.inventory = nullptr;
}

TEST_CASE("builtin commands: put and take typed in the chat answer their result to that player, after the command") {
    BFixture f;
    ItemsWorld iw;
    std::vector<std::function<void(EmfWorld*)>> after;
    InventoryService::Deps deps;
    deps.after_step = [&after](std::function<void(EmfWorld*)> fn) { after.push_back(std::move(fn)); };
    deps.fns.remove_items = reinterpret_cast<RemoveItemsFn>(0x1);  // present (not called: no program runs here)
    InventoryService service(std::move(deps));
    f.state.inventory = &service;
    CommandSender player;
    player.from_chat = true;
    player.account = 0xAA;
    player.handle = 0x42;
    std::vector<std::string> chat;
    const ReplyFn to_chat = [&chat](const std::string& s) { chat.push_back(s); };
    f.center.enqueue("put Torch 2 77", GetTickCount64(), player, to_chat);
    f.center.enqueue("take Torch 1 77", GetTickCount64(), player, to_chat);
    f.center.enqueue("put Torch 3 77");  // typed into the console: not the chat player's business
    f.run(iw.w);
    REQUIRE(service.pending() == 3);
    service.world_reset();
    g_bout.clear();
    chat.clear();
    for (auto& j : after) j(reinterpret_cast<EmfWorld*>(iw.w.p()));  // one step later: run_pending returned long ago
    CHECK(chat == std::vector<std::string>{"put 2 x Torch into entity 77: 0 added (a new world was set up)",
                                           "take 1 x Torch from entity 77: 0 taken (a new world was set up)"});
    CHECK(bsaid("put 3 x Torch into entity 77: 0 added"));  // the console's own result
    CHECK(bsaid("put 2 x Torch into entity 77: 0 added"));  // and, like a chat command's other lines, the chat's too
    f.state.inventory = nullptr;
}

TEST_CASE("builtin commands: put says so when the inventory feature is not available") {
    BFixture f;
    ItemsWorld iw;
    f.center.enqueue("put Torch");
    f.run(iw.w);
    CHECK(bsaid("put refused (see the log)"));
}

TEST_CASE("builtin commands: take removes items from an inventory (yours by default); put takes a level and rarity") {
    BFixture f;
    ItemsWorld iw;
    std::vector<std::function<void(EmfWorld*)>> after;
    InventoryService::Deps deps;
    deps.after_step = [&after](std::function<void(EmfWorld*)> fn) { after.push_back(std::move(fn)); };
    deps.fns.remove_items = reinterpret_cast<RemoveItemsFn>(0x1);  // present (not called: no program runs here)
    InventoryService service(std::move(deps));
    f.state.inventory = &service;
    f.center.enqueue("take Material_T1_Wood_Logs 3");
    f.center.enqueue("take Torch 1 77");
    f.center.enqueue("take");
    f.center.enqueue("put Torch 1 42 12 3");  // level 12, Epic
    f.center.enqueue("put Torch 1 42 12 9");  // bad rarity
    f.run(iw.w);
    CHECK(service.pending() == 3);
    CHECK(service.pending_at(2).rarity == 3);
    CHECK(service.pending_at(2).level == 12);
    CHECK(bsaid("usage: take <item name> [count 1-65535] [entity]"));
    CHECK(bsaid("usage: put <item name> [count 1-65535] [entity] [level] [rarity 0-4]"));
    service.world_reset();
    g_bout.clear();
    for (auto& j : after) j(reinterpret_cast<EmfWorld*>(iw.w.p()));
    REQUIRE(g_bout.size() == 3);
    CHECK(g_bout[0].find("take 3 x Material_T1_Wood_Logs from entity 42: 0 taken") == 0);
    f.state.inventory = nullptr;
}

// ---- near, inv ----
namespace {
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> g_comps;  // (hash, entity) -> bytes
void* map_gc(void* out, void*, uint32_t hash, uint32_t entity) {
    auto o = static_cast<uint64_t*>(out);
    o[0] = o[1] = 0;
    auto it = g_comps.find({hash, entity});
    if (it != g_comps.end()) {
        o[0] = reinterpret_cast<uint64_t>(it->second.data());
        o[1] = it->second.size();
    }
    return out;
}
template <class T>
T& plant_comp(uint32_t entity) {
    auto& v = g_comps[{T::kHash, entity}];
    v.assign(sizeof(T), 0);
    return *reinterpret_cast<T*>(v.data());
}
void place(uint32_t entity, double x, double y, double z) {
    auto& t = plant_comp<emf::gen::ecs::CurrentTransform>(entity);
    t.transform.position.x = static_cast<int64_t>(x * 4294967296.0);
    t.transform.position.y = static_cast<int64_t>(y * 4294967296.0);
    t.transform.position.z = static_cast<int64_t>(z * 4294967296.0);
}
size_t said_at(const std::string& part) {
    for (size_t i = 0; i < g_bout.size(); ++i)
        if (g_bout[i].find(part) != std::string::npos) return i;
    return std::string::npos;
}
}  // namespace

TEST_CASE("builtin commands: near lists the entities around you, nearest first, with what they are") {
    BFixture f;
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 0);
    MapStore ms;
    emf::test::plant_entities(w, ms, {42, 100, 101, 102});
    g_comps.clear();
    place(42, 10, 0, 10);   // the player
    place(100, 13, 0, 10);  // 3 m: a chest
    plant_comp<emf::gen::ecs::InventorySetup>(100);
    place(101, 40, 0, 10);  // 30 m: too far
    place(102, 10, 0, 11);  // 1 m: a creature
    plant_comp<emf::gen::ecs::Health>(102);
    f.state.get_component = map_gc;
    f.center.enqueue("near");
    f.center.enqueue("near 0");
    f.run(w);
    const size_t creature = said_at("  102: 1.0 m");
    const size_t chest = said_at("  100: 3.0 m");
    REQUIRE(creature != std::string::npos);
    REQUIRE(chest != std::string::npos);
    CHECK(creature < chest);  // nearest first
    CHECK(g_bout[creature].find("health") != std::string::npos);
    CHECK(g_bout[chest].find("inventory") != std::string::npos);
    CHECK(said_at("  101:") == std::string::npos);
    CHECK(bsaid("usage: near [radius 1-200]"));
}

TEST_CASE("builtin commands: inv lists every stack in an entity's inventories, with slot ids and gear level") {
    BFixture f;
    ItemsWorld iw;
    g_comps.clear();
    auto& setup = plant_comp<emf::gen::ecs::InventorySetup>(100);
    setup.linksEntities[0].id = 500;
    auto& inv = plant_comp<emf::gen::ecs::Inventory>(500);
    inv.slots[2].id.value = 0x11;
    inv.slots[2].data.count = 7;
    inv.slots[5].id.value = 0x22;
    inv.slots[5].data.count = 1;
    inv.slots[5].data.pide.id = 900;
    auto& state = plant_comp<emf::gen::ecs::ItemState>(900);
    state.itemLevelUi = 10;
    state.itemRarityUi = static_cast<decltype(state.itemRarityUi)>(3);
    f.state.get_component = map_gc;
    f.center.enqueue("inv 100");
    f.center.enqueue("inv 77");
    f.center.enqueue("inv");
    f.run(iw.w);
    CHECK(bsaid("  inventory 500 slot 2 (slot id 0x2000001f4): Material_T1_Wood_Logs x7"));
    CHECK(bsaid("  inventory 500 slot 5 (slot id 0x5000001f4): Torch x1, data entity 900, level 10, rarity 3"));
    CHECK(bsaid("entity 100: 2 stack(s)"));
    CHECK(bsaid("entity 77 has no inventory"));
    CHECK(bsaid("usage: inv <entity>"));
}

TEST_CASE("builtin commands: near and inv refuse bad input; near skips entities without a position") {
    BFixture f;
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 0);
    MapStore ms;
    emf::test::plant_entities(w, ms, {42, 103});
    g_comps.clear();
    place(42, 0, 0, 0);
    plant_comp<emf::gen::ecs::Health>(103);  // no CurrentTransform
    f.state.get_component = map_gc;
    for (const char* line : {"near abc", "near 5 6", "near 201", "inv 0", "inv abc", "inv 1 2"}) f.center.enqueue(line);
    f.center.enqueue("near 5");
    f.run(w);
    size_t near_usage = 0, inv_usage = 0;
    for (const auto& l : g_bout) {
        near_usage += l == "usage: near [radius 1-200]";
        inv_usage += l == "usage: inv <entity>";
    }
    CHECK(near_usage == 3);
    CHECK(inv_usage == 3);
    CHECK(said_at("  103:") == std::string::npos);
    CHECK(bsaid("1 entities within 5 m"));  // just the player
}

TEST_CASE("builtin commands: hud shows, moves and removes a line (for trying layouts)") {
    BFixture f;
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 0);
    f.center.enqueue("hud test.line 1 Hello there");
    f.center.enqueue("hud test.other Top right text");
    f.run(w);
    auto lines = f.state.hud.lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].id == "test.other");  // place 0 (top right) first
    CHECK(lines[0].text == "Top right text");
    CHECK(lines[1].text == "Hello there");
    CHECK(lines[1].place == emf::core::HudPlace::BottomRight);
    f.center.enqueue("hud test.line");  // no text: removed
    f.center.enqueue("hud");
    f.center.enqueue("hud bad!id 0 x");
    f.run(w);
    CHECK(f.state.hud.lines().size() == 1);
    CHECK(bsaid("usage: hud <id> [place 0-2] [text]: no text removes the line"));
    CHECK(bsaid("hud: refused (see the log)"));
}

TEST_CASE("builtin commands: hooks lists every hook with its owner; pattern finds bytes in the exe") {
    BFixture f;
    FakeWorld w;
    std::vector<uint8_t> image(0x1000, 0xCC);
    const auto base = reinterpret_cast<uintptr_t>(image.data());
    image[0x210] = 0x48;
    image[0x211] = 0x89;
    image[0x212] = 0x5C;
    f.state.exe.base = base;
    f.state.exe.size = image.size();
    f.state.exe.sections = {{".text", base + 0x200, 0x400}};
    f.state.hooks = std::make_unique<HookTable>([](uintptr_t, void*, void** o) {
        if (o) *o = nullptr;
        return true;
    });
    f.state.hooks->install("emf", "SimStep", base + 0x210, reinterpret_cast<void*>(0x5000), nullptr);
    f.state.hooks->install("mymod", "my_sim", base + 0x210, reinterpret_cast<void*>(0x6000), nullptr);
    f.state.hooks->install("solo", "alone", base + 0x300, reinterpret_cast<void*>(0x7000), nullptr);
    f.state.hooks->install("late", "late_sim", base + 0x210, reinterpret_cast<void*>(0x8000), nullptr);
    f.center.enqueue("hooks");
    f.center.enqueue("pattern 48 89 5C");
    f.center.enqueue("pattern DE AD");
    f.center.enqueue("pattern 48 ZZ");
    f.run(w);
    // the newest hook on a function runs first; its original calls the one before it, the oldest calls the game
    CHECK(bsaid("emf: SimStep at exe+0x210 (chained: runs 3rd of 3)"));
    CHECK(bsaid("mymod: my_sim at exe+0x210 (chained: runs 2nd of 3)"));
    CHECK(bsaid("late: late_sim at exe+0x210 (chained: runs 1st of 3)"));
    CHECK(std::find(g_bout.begin(), g_bout.end(), "solo: alone at exe+0x300") != g_bout.end());
    CHECK_FALSE(bsaid("runs first"));
    CHECK(bsaid("4 hook(s)"));
    CHECK(bsaid("found once in .text at exe+0x210"));
    CHECK(bsaid("not found"));
    CHECK(bsaid("not a pattern"));
}

// ---- the game's item names (API v29, game_text) ----
namespace {
struct NamesPool {
    alignas(16) uint8_t mem[0x128]{};
    NamesPool() {
        const uint32_t null_tag = 0x69691905u, one = 1, lang = 1;
        const uint64_t table = 0x7700;
        std::memcpy(mem + 0x18, &null_tag, 4);
        std::memcpy(mem + 0x88, &one, 4);
        std::memcpy(mem + 0xD8, &lang, 4);
        std::memcpy(mem + 0x100, &table, 8);
    }
};
uint32_t g_wood_tag = 0;
const std::string kWoodText = "Wood Logs";
emf::core::LocaResult* names_lookup(emf::core::LocaResult* out, void*, uint32_t tag) {
    std::memset(out, 0, sizeof *out);
    if (tag != g_wood_tag) {
        out->error = 0x1F;
        return out;
    }
    out->text = kWoodText.data();
    out->length = kWoodText.size();
    return out;
}
}  // namespace

TEST_CASE("builtin commands: with the game's names, items and give show them and take them typed") {
    BFixture f;
    ItemsWorld iw;
    uint8_t* wood = emf::core::item_data_ptr(iw.reg.reg, 0x11);
    REQUIRE(wood);
    uint8_t g[16];
    for (int i = 0; i < 16; ++i) g[i] = static_cast<uint8_t>(0x40 + i);
    std::memcpy(wood + 0x48, g, 16);
    g_wood_tag = emf::fnv1a32(g, 16);
    NamesPool pool;
    emf::core::LocaAccess a;
    a.pool = [&pool] { return pool.mem; };
    a.lookup = &names_lookup;
    emf::core::set_game_text_for_tests(std::move(a));
    g_pickup_amounts.clear();
    f.center.enqueue("items wood");
    f.center.enqueue("give wood logs 2");
    f.center.enqueue("give Wood Logs");
    f.center.enqueue("give torch");
    f.run(iw.w);
    CHECK(bsaid("Wood Logs: Material_T1_Wood_Logs (0x00000011, stack 250)"));
    CHECK(bsaid("gave 2 x Wood Logs (Material_T1_Wood_Logs) to player"));
    CHECK(bsaid("gave 1 x Wood Logs (Material_T1_Wood_Logs) to player"));
    CHECK(bsaid("gave 1 x Torch to player"));  // no shown name: the debug name alone
    CHECK(g_pickup_amounts == std::vector<uint32_t>{2, 1, 1});
    emf::core::set_game_text_for_tests(emf::core::LocaAccess{});
}

namespace {
const std::string kWoodJa = "\xE4\xB8\xB8\xE5\xA4\xAA";  // 丸太
const std::string kWoodEn = "Wood Logs";
emf::core::LocaResult* ja_lookup(emf::core::LocaResult* out, void*, uint32_t tag) {
    std::memset(out, 0, sizeof *out);
    if (tag != g_wood_tag) {
        out->error = 0x1F;
        return out;
    }
    out->text = kWoodJa.data();
    out->length = kWoodJa.size();
    return out;
}
bool ja_slot_find(emf::core::LocaStringView* out, void*, uint32_t tag) {
    if (tag != g_wood_tag) return false;
    out->text = kWoodEn.data();
    out->length = kWoodEn.size();
    return true;
}
}  // namespace

TEST_CASE("builtin commands: in the game's chat the names are the player's language; on EMF's console English for Japanese") {  // review: Important 2
    BFixture f;
    ItemsWorld iw;
    uint8_t* wood = emf::core::item_data_ptr(iw.reg.reg, 0x11);
    REQUIRE(wood);
    uint8_t g[16];
    for (int i = 0; i < 16; ++i) g[i] = static_cast<uint8_t>(0x60 + i);
    std::memcpy(wood + 0x48, g, 16);
    g_wood_tag = emf::fnv1a32(g, 16);
    NamesPool pool;
    const uint32_t ja = 11;
    const uint64_t table = 0x7711, fallback = 0x7712;
    std::memcpy(pool.mem + 0xD8, &ja, 4);
    std::memcpy(pool.mem + 0x100, &table, 8);
    std::memcpy(pool.mem + 0xB0, &fallback, 8);
    emf::core::LocaAccess a;
    a.pool = [&pool] { return pool.mem; };
    a.lookup = &ja_lookup;
    a.slot_find = &ja_slot_find;
    emf::core::set_game_text_for_tests(std::move(a));
    g_pickup_amounts.clear();
    CommandSender admin;
    admin.from_chat = true;
    admin.account = 0xaa;
    f.center.enqueue("give " + kWoodJa + " 2", GetTickCount64(), admin);  // typed in Japanese, in the chat
    f.center.enqueue("give wood logs 1");                                   // the console
    f.run(iw.w);
    CHECK(bsaid("gave 2 x " + kWoodJa + " (Material_T1_Wood_Logs) to player"));
    CHECK(bsaid("gave 1 x Wood Logs (Material_T1_Wood_Logs) to player"));
    CHECK(g_pickup_amounts == std::vector<uint32_t>{2, 1});
    emf::core::set_game_text_for_tests(emf::core::LocaAccess{});
}

TEST_CASE("builtin commands: stack's reply names the item as the game shows it") {
    BFixture f;
    ItemsWorld iw;
    uint8_t* wood = emf::core::item_data_ptr(iw.reg.reg, 0x11);
    REQUIRE(wood);
    uint8_t g[16];
    for (int i = 0; i < 16; ++i) g[i] = static_cast<uint8_t>(0x70 + i);
    std::memcpy(wood + 0x48, g, 16);
    g_wood_tag = emf::fnv1a32(g, 16);
    NamesPool pool;
    const uint64_t table = 0x7720;
    std::memcpy(pool.mem + 0x100, &table, 8);
    emf::core::LocaAccess a;
    a.pool = [&pool] { return pool.mem; };
    a.lookup = &names_lookup;
    emf::core::set_game_text_for_tests(std::move(a));
    f.center.enqueue("stack wood logs 500");
    f.run(iw.w);
    CHECK(bsaid("Wood Logs (Material_T1_Wood_Logs): stack"));
    emf::core::set_game_text_for_tests(emf::core::LocaAccess{});
}

namespace {
std::map<uint32_t, std::string> g_bait_texts;  // name tag -> the name the game shows
emf::core::LocaResult* bait_lookup(emf::core::LocaResult* out, void*, uint32_t tag) {
    std::memset(out, 0, sizeof *out);
    const auto it = g_bait_texts.find(tag);
    if (it == g_bait_texts.end()) {
        out->error = 0x1F;
        return out;
    }
    out->text = it->second.data();
    out->length = it->second.size();
    return out;
}
}  // namespace

TEST_CASE("builtin commands: the candidates of an unclear item name are listed as the game shows them") {
    BFixture f;
    FriendlyItemsWorld iw;
    g_bait_texts.clear();
    const auto show = [&](uint32_t id, uint8_t seed, const std::string& text) {
        uint8_t* info = emf::core::item_data_ptr(iw.reg.reg, id);
        REQUIRE(info);
        uint8_t g[16];
        for (int i = 0; i < 16; ++i) g[i] = static_cast<uint8_t>(seed + i);
        std::memcpy(info + 0x48, g, 16);
        g_bait_texts[emf::fnv1a32(g, 16)] = text;
    };
    show(0x15, 0x80, "Dog Bait");
    show(0x14, 0xA0, "Simple Bait");
    NamesPool pool;
    const uint64_t table = 0x7730;
    std::memcpy(pool.mem + 0x100, &table, 8);
    emf::core::LocaAccess a;
    a.pool = [&pool] { return pool.mem; };
    a.lookup = &bait_lookup;
    emf::core::set_game_text_for_tests(std::move(a));
    f.center.enqueue("give bait");
    f.run(iw.w);
    CHECK(bsaid("'bait' could be 2 items: Dog Bait (Food_T2_Dog_Bait), Simple Bait (Material_T0_simple_FishingBait). "
                "Type more of the name."));
    CHECK(g_pickup_amounts.empty());
    emf::core::set_game_text_for_tests(emf::core::LocaAccess{});
}

namespace {
// v36 remove: 60 a placed object (item 0x11), 61 a placed chest holding items, 62 a world object (no UsedItem),
// 63 placed, for a refund to a player; 64 placed, holding an item the registry does not know; 65 a placed chest
// holding two stacks (3 x 0x11, 5 x 0x22); 50 another player, 42 the local player.
int64_t g_rm_xf[7] = {int64_t(100) << 32, int64_t(900) << 32, int64_t(200) << 32, 0, 0, 0, 0};
void* rm_gc(void* out, void* w, uint32_t hash, uint32_t entity) {
    namespace gen = emf::gen::ecs;
    b_gc(out, w, hash, entity);
    auto o = static_cast<uint64_t*>(out);
    const bool ours = entity >= 60 && entity <= 65;
    if (hash == off::kHashCurrentTransform && ours) {
        o[0] = reinterpret_cast<uint64_t>(g_rm_xf);
        o[1] = 56;
    }
    static gen::UsedItem used{};
    used.itemId.value = 0x11;
    if (hash == gen::UsedItem::kHash && ours && entity != 62) {
        o[0] = reinterpret_cast<uint64_t>(&used);
        o[1] = sizeof(used);
    }
    static gen::Inventory inv{};
    inv.slots[2].id.value = 0x22;
    inv.slots[2].data.count = 70000;  // a stack-size mod's stack: more than one pickup holds
    inv.slots[4].id.value = 0x22;
    inv.slots[4].data.count = 0;      // nothing: no pickup made from it
    if (hash == gen::Inventory::kHash && entity == 61) {
        o[0] = reinterpret_cast<uint64_t>(&inv);
        o[1] = sizeof(inv);
    }
    static gen::Inventory odd{};
    odd.slots[0].id.value = 0xdead;
    odd.slots[0].data.count = 1;
    if (hash == gen::Inventory::kHash && entity == 64) {
        o[0] = reinterpret_cast<uint64_t>(&odd);
        o[1] = sizeof(odd);
    }
    static gen::Inventory two{};
    two.slots[0].id.value = 0x11;
    two.slots[0].data.count = 3;
    two.slots[1].id.value = 0x22;
    two.slots[1].data.count = 5;
    if (hash == gen::Inventory::kHash && entity == 65) {
        o[0] = reinterpret_cast<uint64_t>(&two);
        o[1] = sizeof(two);
    }
    return out;
}
}  // namespace

TEST_CASE("builtin commands (v36): remove takes a placed object away like a pick-up, with the item back; refuses with the reason") {
    BFixture f;
    f.state.get_component = rm_gc;
    ItemsWorld iw;
    uint8_t* logs = nullptr;
    for (auto& b : iw.reg.infos) {
        uint32_t id = 0;
        std::memcpy(&id, b.get() + off::kItemInfoId, 4);
        if (id == 0x11) logs = b.get();
    }
    REQUIRE(logs);
    logs[off::kItemInfoPlacementConsume] = 1;  // placing it used it up: the game refunds it
    MapStore ents;
    emf::test::plant_entities(iw.w, ents, {42, 50, 60, 61, 62, 63, 64});
    f.center.enqueue("remove 60 drop");
    f.center.enqueue("remove 61");        // holds items
    f.center.enqueue("remove 61 force");  // with them: they drop
    f.center.enqueue("remove 64 force");  // an item that cannot be dropped: never lost
    f.center.enqueue("remove 62");        // no player placed it
    f.center.enqueue("remove 50");        // a player
    f.center.enqueue("remove 63 50");     // to player 50: no inventory transaction in this fake, so a drop
    f.center.enqueue("remove 63 42");     // 42 has no PlayerState in this fake: not a player
    f.center.enqueue("remove 60 nobody");
    f.center.enqueue("remove");
    f.run(iw.w);
    CHECK(g_destroyed == std::vector<uint32_t>{60, 61, 63});
    CHECK(bsaid("removed entity 60; the item came back as a drop"));
    CHECK(bsaid("entity 61 not removed: it holds items (2 stacks)"));
    CHECK(bsaid("removed entity 61"));
    CHECK(bsaid("what it held (1 stack) lies on the ground where it stood"));
    CHECK(bsaid("entity 64 not removed: what it holds cannot be dropped"));
    CHECK(bsaid("entity 62 not removed: not something a player placed"));
    CHECK(bsaid("entity 50 not removed: that is a player"));
    CHECK(bsaid("removed entity 63; the item came back to player 50"));
    CHECK(bsaid("entity 63 not removed: the refund's player is not a player"));
    CHECK(g_pickup_amounts == std::vector<uint32_t>{1, 65535, 4465, 1});  // 60's drop, 61's 70000, 63's (no inventory)
    CHECK(bsaid("usage: remove <entity> [me|drop|<player>] [force]"));
}

TEST_CASE("builtin commands (v36): remove ... force says what became of what it held: nothing, all of it on the ground, or the stacks that could not be dropped") {
    BFixture f;
    f.state.get_component = rm_gc;
    ItemsWorld iw;
    MapStore ents;
    emf::test::plant_entities(iw.w, ents, {42, 50, 60, 61, 63, 65});
    g_pickup_fails = 0;
    f.center.enqueue("remove 60 force");  // holds nothing
    f.run(iw.w);
    CHECK(bsaid("removed entity 60"));
    CHECK_FALSE(bsaid("on the ground"));
    CHECK_FALSE(bsaid("held"));
    g_bout.clear();
    f.center.enqueue("remove 61 force");  // one stack of 70000 (two pickups) and an empty slot
    f.run(iw.w);
    CHECK(bsaid("removed entity 61"));
    CHECK(bsaid("what it held (1 stack) lies on the ground where it stood"));
    g_bout.clear();
    g_pickup_fails = 5;                   // 65's second stack cannot be dropped
    f.center.enqueue("remove 65 force");
    f.run(iw.w);
    CHECK(bsaid("removed entity 65"));
    CHECK(bsaid("1 of the 2 stacks it held lies on the ground where it stood; the rest could not be dropped (see the "
                "log)"));
    CHECK_FALSE(bsaid("what it held (2 stacks) lies on the ground"));
    g_bout.clear();
    g_pickup_fails = 0;
    f.center.enqueue("remove 63 force");  // holds nothing: the last remove's stacks are not said again
    f.run(iw.w);
    CHECK(bsaid("removed entity 63"));
    CHECK_FALSE(bsaid("held"));
    CHECK(g_destroyed == std::vector<uint32_t>{60, 61, 65, 63});
}

namespace {
// health / ai: 70 a wild wolf in a fight (30 of 120 health), 71 no creature (no health, no AI), 72 just spawned (0 of 34).
std::map<std::pair<uint32_t, uint32_t>, int32_t> g_attr;
std::vector<std::pair<uint32_t, int32_t>> g_attr_adds;
bool fake_attr_get(EmfWorld*, uint32_t entity, uint32_t attr, int32_t* out) {
    auto it = g_attr.find({entity, attr});
    if (it == g_attr.end()) return false;
    *out = it->second;
    return true;
}
bool fake_attr_add(EmfWorld*, uint32_t entity, uint32_t attr, int32_t delta) {
    if (attr != EMF_ATTR_HEALTH) return false;
    g_attr_adds.push_back({entity, delta});
    return true;
}
void* ai_gc(void* out, void* w, uint32_t hash, uint32_t entity) {
    namespace gen = emf::gen::ecs;
    b_gc(out, w, hash, entity);
    auto o = static_cast<uint64_t*>(out);
    static gen::DynamicEnemy01BehaviorState ai{};
    ai.state = emf::gen::enemy01::State::Combat;
    ai.guardEntityId.id = 1;
    static gen::SelectedTargets st{};
    st.targets[0].targetEntityId.id = 80;
    static gen::DynamicAggro ag{};
    ag.targets[0].state = gen::AggroTargetState::Alarmed;
    ag.targets[0].entityId.id = 80;
    ag.targets[0].aggro = 12.5f;
    ag.targets[2].state = gen::AggroTargetState::Noticed;
    ag.targets[2].entityId.id = 81;
    ag.targets[2].aggro = 3.0f;
    static gen::Faction fa{};
    fa.faction = static_cast<emf::gen::FactionType>(6);
    auto give = [o](const void* p, size_t n) {
        o[0] = reinterpret_cast<uint64_t>(p);
        o[1] = n;
    };
    if (entity == 70) {
        if (hash == gen::DynamicEnemy01BehaviorState::kHash) give(&ai, sizeof(ai));
        if (hash == gen::SelectedTargets::kHash) give(&st, sizeof(st));
        if (hash == gen::DynamicAggro::kHash) give(&ag, sizeof(ag));
    }
    if ((entity == 70 || entity == 71) && hash == gen::Faction::kHash) give(&fa, sizeof(fa));
    return out;
}
}  // namespace

TEST_CASE("builtin commands: health reads a creature's health and sets it to a share of its maximum (to beat a beast down for taming)") {
    BFixture f;
    EmfApi api = *api_table();
    api.attribute_get = fake_attr_get;
    api.attribute_add = fake_attr_add;
    f.ctx.api = &api;
    g_attr = {{{70, EMF_ATTR_HEALTH}, 30}, {{70, EMF_ATTR_MAX_HEALTH}, 120}, {{72, EMF_ATTR_HEALTH}, 0},
              {{72, EMF_ATTR_MAX_HEALTH}, 34}};
    g_attr_adds.clear();
    ItemsWorld iw;
    MapStore ents;
    emf::test::plant_entities(iw.w, ents, {42, 70, 71, 72});
    f.center.enqueue("health 70");
    f.center.enqueue("health 70 20");
    f.center.enqueue("health 71");
    f.center.enqueue("health 72");
    f.center.enqueue("health 72 50");
    f.center.enqueue("health 70 0");
    f.center.enqueue("health 70 101");
    f.run(iw.w);
    CHECK(bsaid("entity 70: health 30 of 120 (25 %)"));
    CHECK(bsaid("entity 70: health 30 -> 24 of 120"));
    CHECK(bsaid("entity 71 has no health here"));
    CHECK(bsaid("entity 72: health 0 of 34: dead, or just spawned (try again in a second)"));
    CHECK(g_attr_adds == std::vector<std::pair<uint32_t, int32_t>>{{70, -6}});  // 72 is never set
    CHECK(bsaid("usage: health <entity> [percent 1-100]"));
}

TEST_CASE("builtin commands: ai shows a creature's AI state, whom it guards and fights, its aggro and faction") {
    BFixture f;
    f.state.get_component = ai_gc;
    ItemsWorld iw;
    MapStore ents;
    emf::test::plant_entities(iw.w, ents, {42, 70, 71});
    f.center.enqueue("ai 70");
    f.center.enqueue("ai 71");
    f.center.enqueue("ai");
    f.run(iw.w);
    CHECK(bsaid("entity 70: Combat (6), guards entity 1, fights entity 80, faction WildBeast (6)"));
    CHECK(bsaid("aggro: 80 alarmed (12.5), 81 noticed (3.0)"));
    CHECK(bsaid("entity 71 has no creature AI (faction WildBeast (6))"));
    CHECK(bsaid("usage: ai <entity>"));
}

namespace {
std::vector<EmfHitRule> g_god_rules;      // what hit_rule_add was given
std::vector<uint32_t> g_god_removed;      // the ids hit_rule_remove was given
bool g_god_refuse = false;                // hit_rule_add answers 0 (hits off on this game build, or 64 rules)
uint32_t fake_god_add(const EmfHitRule* r) {
    if (g_god_refuse) return 0;
    g_god_rules.push_back(*r);
    return 100 + static_cast<uint32_t>(g_god_rules.size());
}
std::vector<uint32_t> g_god_gone_rules;  // hit_rule_remove answers false for these (the rule was gone already)
bool fake_god_remove(uint32_t id) {
    g_god_removed.push_back(id);
    return std::find(g_god_gone_rules.begin(), g_god_gone_rules.end(), id) == g_god_gone_rules.end();
}
EmfPlayerCallback g_god_player_cb = nullptr;  // on_player's watcher
void* g_god_player_user = nullptr;
bool fake_god_on_player(EmfPlayerCallback cb, void* user) {
    g_god_player_cb = cb;
    g_god_player_user = user;
    return true;
}
}  // namespace

TEST_CASE("builtin commands: god keeps an entity (you by default) at 1 health or more with a hit rule; god alone lists") {
    BFixture f;
    EmfApi api = *api_table();
    api.attribute_get = fake_attr_get;
    api.hit_rule_add = fake_god_add;
    api.hit_rule_remove = fake_god_remove;
    f.ctx.api = &api;
    g_attr = {{{42, EMF_ATTR_HEALTH}, 100}, {{70, EMF_ATTR_HEALTH}, 30}};  // 42 is you; 71 has no health
    g_god_rules.clear();
    g_god_removed.clear();
    g_god_gone_rules.clear();
    g_god_refuse = false;
    ItemsWorld iw;
    MapStore ents;
    emf::test::plant_entities(iw.w, ents, {42, 70, 71});
    f.center.enqueue("god");
    f.center.enqueue("god on");      // you
    f.center.enqueue("god 70");      // on is the default
    f.center.enqueue("god me on");   // no second rule
    f.center.enqueue("god");
    f.run(iw.w);
    CHECK(bsaid("god: nobody has it"));
    CHECK(bsaid("entity 42: god on"));
    CHECK(bsaid("entity 70: god on"));
    CHECK(bsaid("entity 42 has god already"));
    CHECK(bsaid("god: entity 42 (you), entity 70"));
    REQUIRE(g_god_rules.size() == 2u);
    for (const auto& r : g_god_rules) {
        CHECK(r.size == sizeof(EmfHitRule));
        CHECK(r.attacker == 0u);
        CHECK(r.kinds == EMF_HIT_KINDS_ALL);  // hits, falls, the shroud, the cold, drowning
        CHECK(r.min_health == 1);
        CHECK(r.scale == 1.0f);
        CHECK(r.add == 0);
        CHECK(r.cancel == 0);
        CHECK(r.healing == 0);
    }
    CHECK(g_god_rules[0].target == 42u);
    CHECK(g_god_rules[1].target == 70u);

    g_bout.clear();
    f.center.enqueue("god 70 off");
    f.center.enqueue("god 70 off");
    f.center.enqueue("god");
    f.run(iw.w);
    CHECK(bsaid("entity 70: god off"));
    CHECK(bsaid("entity 70 has no god"));
    CHECK(bsaid("god: entity 42 (you)"));
    CHECK(g_god_removed == std::vector<uint32_t>{102});

    // bad input: nothing is added or removed
    g_bout.clear();
    f.center.enqueue("god 71");          // no health: not a creature or player, or not the host
    f.center.enqueue("god 70 maybe");
    f.center.enqueue("god x");
    f.center.enqueue("god 0");
    f.center.enqueue("god 70 on now");
    f.run(iw.w);
    CHECK(bsaid("entity 71 has no health here"));
    CHECK(std::count_if(g_bout.begin(), g_bout.end(),
                        [](const std::string& l) { return l == "usage: god [entity | me] [on|off]"; }) == 4);
    CHECK(g_god_rules.size() == 2u);
    CHECK(g_god_removed.size() == 1u);

    // hit rules off on this game build (add answers 0): the command says so
    g_bout.clear();
    g_god_refuse = true;
    f.center.enqueue("god 70");
    f.center.enqueue("god");
    f.run(iw.w);
    CHECK(bsaid("entity 70: no god: the hit rule was refused"));
    CHECK(bsaid("god: entity 42 (you)"));

    // a new world: the rules go (entity ids are used again there)
    g_bout.clear();
    builtin_world_reset(f.ctx);
    CHECK(g_god_removed == std::vector<uint32_t>{102, 101});
    f.center.enqueue("god");
    f.run(iw.w);
    CHECK(bsaid("god: nobody has it"));
}

TEST_CASE("builtin commands: god ends for an entity that is gone or a player who leaves; a dead entity gets none") {
    BFixture f;
    EmfApi api = *api_table();
    api.attribute_get = fake_attr_get;
    api.hit_rule_add = fake_god_add;
    api.hit_rule_remove = fake_god_remove;
    api.on_player = fake_god_on_player;
    f.ctx.api = &api;
    g_attr = {{{42, EMF_ATTR_HEALTH}, 100}, {{70, EMF_ATTR_HEALTH}, 30}, {{80, EMF_ATTR_HEALTH}, 50},
              {{72, EMF_ATTR_HEALTH}, 0}, {{72, EMF_ATTR_MAX_HEALTH}, 34}};
    g_god_rules.clear();
    g_god_removed.clear();
    g_god_gone_rules.clear();
    g_god_refuse = false;
    g_god_player_cb = nullptr;
    ItemsWorld iw;
    MapStore ents;
    emf::test::plant_entities(iw.w, ents, {42, 70, 72, 80});
    f.center.enqueue("god 72");  // dead (or a spawn not set up yet): no rule taken
    f.center.enqueue("god on");
    f.center.enqueue("god 70");
    f.center.enqueue("god 80");
    f.run(iw.w);
    CHECK(bsaid("entity 72: health 0 of 34: dead, or just spawned (try again in a second)"));
    REQUIRE(g_god_rules.size() == 3u);  // 42 -> 101, 70 -> 102, 80 -> 103
    REQUIRE(g_god_player_cb != nullptr);  // god watches players leaving

    // creature 70 was destroyed: its health reads no more; the list ends its god
    g_attr.erase({70, EMF_ATTR_HEALTH});
    g_bout.clear();
    f.center.enqueue("god");
    f.run(iw.w);
    CHECK(bsaid("entity 70 is gone (no health to read): its god ended"));
    CHECK(bsaid("god: entity 42 (you), entity 80"));
    CHECK(g_god_removed == std::vector<uint32_t>{102});

    // player 42 leaves: their god ends at once (the next player may get their entity id)
    EmfPlayerEvent left{};
    left.size = sizeof left;
    left.kind = EMF_PLAYER_LEFT;
    left.player = 42;
    g_god_player_cb(nullptr, &left, g_god_player_user);
    CHECK(g_god_removed == std::vector<uint32_t>{102, 101});
    g_bout.clear();
    f.center.enqueue("god");
    f.run(iw.w);
    CHECK(bsaid("god: entity 80"));

    // the rule was gone already (removed by someone else): the reply says so
    g_god_gone_rules = {103};
    g_bout.clear();
    f.center.enqueue("god 80 off");
    f.run(iw.w);
    CHECK(bsaid("entity 80: god off (its hit rule was gone already)"));
    CHECK(g_god_rules.size() == 3u);
}

TEST_CASE("builtin commands: god without a player of yours (a dedicated server's console) asks for an entity") {
    BFixture f;
    EmfApi api = *api_table();
    api.attribute_get = fake_attr_get;
    api.hit_rule_add = fake_god_add;
    api.hit_rule_remove = fake_god_remove;
    f.ctx.api = &api;
    f.ctx.player = [] { return uint32_t(0); };
    g_god_rules.clear();
    ItemsWorld iw;
    MapStore ents;
    emf::test::plant_entities(iw.w, ents, {42});
    f.center.enqueue("god on");
    f.run(iw.w);
    CHECK(bsaid("no player of yours here: name an entity (god <entity> [on|off])"));
    CHECK(g_god_rules.empty());
}

namespace {
std::vector<EmfEnemyOrder> g_orders;
std::vector<uint32_t> g_order_cancels;
uint32_t fake_order(EmfWorld*, EmfEnemyOrder* o) {
    if (o->enemy == 13) {
        std::snprintf(o->why, sizeof o->why, "it has no enemy AI to send");
        return 0;
    }
    g_orders.push_back(*o);
    return 40 + static_cast<uint32_t>(g_orders.size());
}
bool fake_order_cancel(uint32_t id) {
    g_order_cancels.push_back(id);
    return id == 41;
}
}  // namespace

TEST_CASE("builtin commands: order sends an enemy at an entity (seconds, damage); order.stop ends it") {
    BFixture f;
    EmfApi api = *api_table();
    api.enemy_order = fake_order;
    api.enemy_order_cancel = fake_order_cancel;
    f.ctx.api = &api;
    g_orders.clear();
    g_order_cancels.clear();
    ItemsWorld iw;
    MapStore ents;
    emf::test::plant_entities(iw.w, ents, {42, 70, 900});
    f.center.enqueue("order 70 900");
    f.center.enqueue("order 70 900 30 50");
    f.center.enqueue("order 13 900");
    f.center.enqueue("order 70");
    f.center.enqueue("order.stop 41");
    f.center.enqueue("order.stop 7");
    f.run(iw.w);
    CHECK(bsaid("order 41: entity 70 goes for entity 900 until it ends (order.stop 41)"));
    CHECK(bsaid("order 42: entity 70 goes for entity 900 for 30 s, 50 damage added to each of its hits on it"));
    CHECK(bsaid("entity 13 not sent: it has no enemy AI to send"));
    CHECK(bsaid("usage: order <enemy> <target> [seconds] [damage]"));
    CHECK(bsaid("order 41 ended"));
    CHECK(bsaid("no order 7"));
    REQUIRE(g_orders.size() == 2u);
    CHECK(g_orders[0].seconds == 0.0f);
    CHECK(g_orders[1].seconds == 30.0f);
    CHECK(g_orders[1].damage == 50);
    CHECK(g_orders[1].flags == 0u);  // never a tamed beast (the spec), also from the console
}

namespace {
struct SeenMove {
    EmfMoveOrder o;
    std::vector<EmfPathPoint> pts;
    EmfMoveCallback cb;
    void* user;
};
std::vector<SeenMove> g_moves;
std::vector<uint32_t> g_move_cancels;
uint32_t fake_move(EmfWorld*, EmfMoveOrder* o, EmfMoveCallback cb, void* user) {
    if (o->enemy == 13) {
        std::snprintf(o->why, sizeof o->why, "it has no enemy AI to move");
        return 0;
    }
    g_moves.push_back({*o, {o->points, o->points + o->count}, cb, user});
    return 60 + static_cast<uint32_t>(g_moves.size());
}
bool fake_move_cancel(uint32_t id) {
    g_move_cancels.push_back(id);
    return id == 61;
}
}  // namespace

TEST_CASE("builtin commands: move walks an enemy to a point or to an entity (peaceful); its end is said; move.stop") {
    BFixture f;
    EmfApi api = *api_table();
    api.enemy_move = fake_move;
    api.enemy_move_cancel = fake_move_cancel;
    f.ctx.api = &api;
    g_moves.clear();
    g_move_cancels.clear();
    ItemsWorld iw;
    MapStore ents;
    emf::test::plant_entities(iw.w, ents, {42, 70, 900});
    f.center.enqueue("move 70 100 20 -30.5");
    f.center.enqueue("move 70 100 20 -30.5 peaceful");
    f.center.enqueue("move 13 1 2 3");
    f.center.enqueue("move 70");
    f.center.enqueue("move 70 1 2");
    f.center.enqueue("move.stop 61");
    f.center.enqueue("move.stop 7");
    f.run(iw.w);
    CHECK(bsaid("move 61: entity 70 walks to (100.0, 20.0, -30.5) (move.stop 61)"));
    CHECK(bsaid("move 62: entity 70 walks to (100.0, 20.0, -30.5), ignoring players (move.stop 62)"));
    CHECK(bsaid("entity 13 not moved: it has no enemy AI to move"));
    CHECK(bsaid("usage: move <enemy> <x> <y> <z> [peaceful] [arrive <metres>] | move <enemy> <entity | me> [peaceful] [arrive <metres>]"));
    CHECK(bsaid("move 61 ended"));
    CHECK(bsaid("no move 7"));
    REQUIRE(g_moves.size() == 2u);
    CHECK(g_moves[0].o.flags == 0u);  // never a tamed beast from the console (Beastmaster keeps its beasts)
    CHECK(g_moves[1].o.flags == EMF_MOVE_PEACEFUL);
    REQUIRE(g_moves[0].pts.size() == 1u);
    CHECK(g_moves[0].pts[0].z == doctest::Approx(-30.5f));
    REQUIRE(g_moves[0].cb != nullptr);
    g_moves[0].cb(nullptr, 61, EMF_MOVE_ARRIVED, g_moves[0].user);  // later, after a server step
    CHECK(bsaid("move 61: entity 70 arrived"));
    g_moves[1].cb(nullptr, 62, EMF_MOVE_STUCK, g_moves[1].user);
    CHECK(bsaid("move 62: entity 70 is stuck (no way on for 15 s)"));
}

TEST_CASE("builtin commands: move takes arrive <metres> (GitHub #44), in any order with peaceful") {
    BFixture f;
    EmfApi api = *api_table();
    api.enemy_move = fake_move;
    api.enemy_move_cancel = fake_move_cancel;
    f.ctx.api = &api;
    g_moves.clear();
    ItemsWorld iw;
    MapStore ents;
    emf::test::plant_entities(iw.w, ents, {42, 70, 900});
    f.center.enqueue("move 70 100 20 -30.5 arrive 0.5");
    f.center.enqueue("move 70 100 20 -30.5 arrive 1 peaceful");
    f.center.enqueue("move 70 100 20 -30.5 peaceful arrive 3");
    f.center.enqueue("move 70 1 2 3 arrive");      // no metres
    f.center.enqueue("move 70 1 2 3 arrive x");
    f.center.enqueue("move 70 1 2 3 arrive -1");
    f.run(iw.w);
    REQUIRE(g_moves.size() == 3u);
    CHECK(g_moves[0].o.arrive == doctest::Approx(0.5f));
    CHECK(g_moves[0].o.flags == 0u);
    CHECK(g_moves[1].o.arrive == doctest::Approx(1.0f));
    CHECK(g_moves[1].o.flags == EMF_MOVE_PEACEFUL);
    CHECK(g_moves[2].o.arrive == doctest::Approx(3.0f));
    CHECK(g_moves[2].o.flags == EMF_MOVE_PEACEFUL);
    CHECK(bsaid("move 61: entity 70 walks to (100.0, 20.0, -30.5), arriving within 0.5 m (move.stop 61)"));
    size_t usage = 0;
    for (const auto& l : g_bout) usage += l.find("usage: move <enemy>") != std::string::npos;
    CHECK(usage == 3u);
}

namespace {
std::vector<SeenMove> g_vmoves;
std::vector<uint32_t> g_vmove_cancels;
uint32_t fake_vmove(EmfWorld*, EmfMoveOrder* o, EmfMoveCallback cb, void* user) {
    if (o->enemy == 13) {
        std::snprintf(o->why, sizeof o->why, "a point is 40 m from the villager's home, outside its walk area (30 m around its home)");
        return 0;
    }
    g_vmoves.push_back({*o, {o->points, o->points + o->count}, cb, user});
    return 80 + static_cast<uint32_t>(g_vmoves.size());
}
bool fake_vmove_cancel(uint32_t id) {
    g_vmove_cancels.push_back(id);
    return id == 81;
}
}  // namespace

TEST_CASE("builtin commands: vmove walks a villager to a point or to an entity, with a hold of seconds or always; its end is said; vmove.stop") {
    BFixture f;
    EmfApi api = *api_table();
    api.villager_move = fake_vmove;
    api.villager_move_cancel = fake_vmove_cancel;
    f.ctx.api = &api;
    g_vmoves.clear();
    g_vmove_cancels.clear();
    ItemsWorld iw;
    MapStore ents;
    emf::test::plant_entities(iw.w, ents, {42, 70, 900});
    f.center.enqueue("vmove 70 100 20 -30.5");
    f.center.enqueue("vmove 70 100 20 -30.5 hold 30");
    f.center.enqueue("vmove 70 100 20 -30.5 hold always");
    f.center.enqueue("vmove 13 1 2 3");
    f.center.enqueue("vmove 70");
    f.center.enqueue("vmove 70 1 2 3 hold");
    f.center.enqueue("vmove.stop 81");
    f.center.enqueue("vmove.stop 7");
    f.run(iw.w);
    CHECK(bsaid("villager move 81: villager 70 walks to (100.0, 20.0, -30.5) (vmove.stop 81)"));
    CHECK(bsaid("villager move 82: villager 70 walks to (100.0, 20.0, -30.5), then holds 30 s (vmove.stop 82)"));
    CHECK(bsaid("villager move 83: villager 70 walks to (100.0, 20.0, -30.5), then holds until vmove.stop (vmove.stop 83)"));
    CHECK(bsaid("villager 13 not moved: a point is 40 m from the villager's home, outside its walk area (30 m around its home)"));
    CHECK(bsaid("usage: vmove <villager> <x> <y> <z> [hold <seconds | always>] | vmove <villager> <entity | me> [hold <seconds | always>]"));
    CHECK(bsaid("villager move 81 ended"));
    CHECK(bsaid("no villager move 7"));
    REQUIRE(g_vmoves.size() == 3u);
    CHECK(g_vmoves[0].o.flags == 0u);
    CHECK(g_vmoves[0].o.hold_seconds == 0.0f);
    CHECK(g_vmoves[1].o.hold_seconds == 30.0f);
    CHECK(g_vmoves[2].o.flags == EMF_MOVE_HOLD);
    REQUIRE(g_vmoves[0].cb != nullptr);
    g_vmoves[0].cb(nullptr, 81, EMF_MOVE_ARRIVED, g_vmoves[0].user);
    CHECK(bsaid("villager move 81: villager 70 arrived"));
    g_vmoves[1].cb(nullptr, 82, EMF_MOVE_ARRIVED, g_vmoves[1].user);
    CHECK(bsaid("villager move 82: villager 70 arrived and holds there"));
    g_vmoves[2].cb(nullptr, 83, EMF_MOVE_STUCK, g_vmoves[2].user);
    CHECK(bsaid("villager move 83: villager 70 is stuck (no way on, or the game sent it home)"));
}

TEST_CASE("api remove_building (v36): no request, a small size field, or a call outside a world callback is refused") {
    BFixture f;
    CHECK_FALSE(api_table()->remove_building(nullptr, nullptr));
    EmfRemoveBuilding r{};
    r.size = 8;
    CHECK_FALSE(api_table()->remove_building(nullptr, &r));
    r = EmfRemoveBuilding{};
    r.size = sizeof(r);
    r.entity = 60;
    r.refunded = 99;
    CHECK_FALSE(api_table()->remove_building(nullptr, &r));
    CHECK(std::string(r.why).find("world callback") != std::string::npos);
    CHECK(r.refunded == 0);
}

TEST_CASE("builtin commands: items shows the template an item places (spawn takes it), none for other items") {
    BFixture f;
    ItemsWorld iw;
    uint8_t* logs = nullptr;
    for (auto& b : iw.reg.infos) {
        uint32_t id = 0;
        std::memcpy(&id, b.get() + off::kItemInfoId, 4);
        if (id == 0x11) logs = b.get();
    }
    REQUIRE(logs);
    const uint8_t tpl[16] = {0x91, 0x9d, 0xf8, 0x54, 0x9c, 0xd5, 0xd8, 0x4f,  // 54f89d91-d59c-4fd8-b39c-ee30632bf57f
                             0xb3, 0x9c, 0xee, 0x30, 0x63, 0x2b, 0xf5, 0x7f};  // as the game stores it
    std::memcpy(logs + off::kItemInfoPlacedEntity, tpl, 16);
    f.center.enqueue("items");
    f.run(iw.w);
    CHECK(bsaid("Material_T1_Wood_Logs (0x00000011, stack 250, places 54f89d91-d59c-4fd8-b39c-ee30632bf57f)"));
    CHECK(bsaid("Torch (0x00000022, stack 1)"));
}

namespace {
// The remove test's world: entities 42, 50, 60-63 and item 0x11 consumed on placement.
struct RemoveWorld {
    ItemsWorld iw;
    MapStore ents;
    RemoveWorld() {
        for (auto& b : iw.reg.infos) {
            uint32_t id = 0;
            std::memcpy(&id, b.get() + off::kItemInfoId, 4);
            if (id == 0x11) b.get()[off::kItemInfoPlacementConsume] = 1;
        }
        emf::test::plant_entities(iw.w, ents, {42, 50, 60, 61, 62, 63});
    }
};
}  // namespace

TEST_CASE("remove (review I1): a refusal repeated every step is logged once, not every time") {
    BFixture f;
    f.state.get_component = rm_gc;
    std::vector<std::string> lines;
    f.state.log_line = [&](const std::string& l) {
        if (l.find("remove_building refused") != std::string::npos) lines.push_back(l);
    };
    RemoveWorld rw;
    for (int i = 0; i < 5; ++i) f.center.enqueue("remove 61");  // holds items: refused each time
    f.run(rw.iw.w);
    CHECK(lines.size() == 1u);
    CHECK(bsaid("entity 61 not removed: it holds items"));  // the caller still gets the reason every time
}

TEST_CASE("remove (review I4): when the item cannot be given back on this build, nothing is removed") {
    BFixture f;
    f.state.get_component = rm_gc;
    f.state.sigs[static_cast<size_t>(Sig::SpawnWrapper)] = Resolved{};
    RemoveWorld rw;
    f.center.enqueue("remove 60 drop");
    f.center.enqueue("remove 63 50");
    f.center.enqueue("remove 63");  // no refund asked: removed
    f.run(rw.iw.w);
    CHECK(bsaid("entity 60 not removed: its item cannot be given back on this game build"));
    CHECK(bsaid("entity 63 not removed: its item cannot be given back on this game build"));
    CHECK(g_destroyed == std::vector<uint32_t>{63});
}

TEST_CASE("remove (review, not judged): the same object twice in one step pays its item once") {
    BFixture f;
    f.state.get_component = rm_gc;  // (the fake destroy leaves its components, as a deferred destroy might)
    RemoveWorld rw;
    f.center.enqueue("remove 60 drop");
    f.center.enqueue("remove 60 drop");
    f.run(rw.iw.w);
    CHECK(g_destroyed == std::vector<uint32_t>{60});
    CHECK(g_pickup_amounts == std::vector<uint32_t>{1});
    CHECK(bsaid("entity 60 not removed: it was removed already"));
}

TEST_CASE("builtin commands (v37): path checks its words; with the feature off it says why") {
    BFixture f;
    path_stop();
    f.state.get_component = rm_gc;  // entities 60-65 have positions
    RemoveWorld rw;
    f.center.enqueue("path 1 2");
    f.center.enqueue("path 1 2 x");
    f.center.enqueue("path 3740 845 1440 jumps closest");
    f.center.enqueue("path 3740 845 1440 closest 20001");  // more nodes than 20000
    f.center.enqueue("path 3740 845 1440 closest 0");
    f.center.enqueue("path 3740 845 1440 9000 closest");   // a node count: fine
    f.center.enqueue("path 3740 845 1440 doors jumps");    // 0.6.11: doors
    f.center.enqueue("path 3740 845 1440 as");             // 0.6.20: as <entity>
    f.center.enqueue("path 3740 845 1440 as x");
    f.center.enqueue("path 3740 845 1440 as 0");
    f.center.enqueue("path 3740 845 1440 doors as 60");    // from entity 60's position, planned as it
    f.center.enqueue("path 3740 845 1440 as 999");         // no such entity: no position
    f.center.enqueue("path 3740 845 1440 jumps doors closest 9000 as 60");  // every word at once
    f.center.enqueue("path 3740 845 1440 closest nodes 20000");  // the count after the word, as the usage line says it
    f.center.enqueue("path 3740 845 1440 nodes 9000 as 60");
    f.center.enqueue("path 3740 845 1440 nodes");                // no count
    f.center.enqueue("path 3740 845 1440 nodes 0");
    f.center.enqueue("path 3740 845 1440 nodes 9000 8000");      // two counts
    f.run(rw.iw.w);
    size_t usage = 0, refused = 0;
    for (const auto& l : g_bout) {
        usage += l.find("usage: path <x> <y> <z> [jumps] [doors] [closest] [nodes 1-20000] [as <entity>]") !=
                 std::string::npos;
        refused += l.find("path not asked") != std::string::npos;
    }
    CHECK(usage == 10u);
    CHECK(refused == 7u);
    CHECK(bsaid("path not asked (see the log: path_find refused"));
    CHECK(bsaid("path: entity 999 has no position in this world"));
}

TEST_CASE("builtin commands (path.door): checks its words; asks emf_path, or says the feature is not available") {
    BFixture f;
    path_stop();
    RemoveWorld rw;
    f.center.enqueue("path.door");
    f.center.enqueue("path.door x");
    f.center.enqueue("path.door 0");
    f.center.enqueue("path.door 60 61");
    f.center.enqueue("path.door 60");
    f.run(rw.iw.w);
    size_t usage = 0;
    for (const auto& l : g_bout) usage += l.find("usage: path.door <entity>") != std::string::npos;
    CHECK(usage == 4u);
    CHECK(bsaid("path.door: not available"));  // no path grid feature in this fake
    g_bout.clear();
    f.center.enqueue("help");
    f.run(rw.iw.w);
    CHECK(bsaid("path.door <entity>: what the game's path grid knows about a door"));
}

TEST_CASE("builtin commands (v38): door checks its words; with doors off it says why") {
    BFixture f;
    RemoveWorld rw;
    f.center.enqueue("door");
    f.center.enqueue("door 60 wobble");
    f.center.enqueue("door 60");
    f.center.enqueue("door 60 open back");
    f.run(rw.iw.w);
    size_t usage = 0;
    for (const auto& l : g_bout) usage += l.find("usage: door <entity> [open|close|lock|unlock] [front|back]") != std::string::npos;
    CHECK(usage == 2u);
    CHECK(bsaid("entity 60: not a door"));
    CHECK(bsaid("door 60: not changed (see the log: door_set refused"));
}

TEST_CASE("builtin commands: spawn takes a placeable item's name (the template it places) and a place written any way") {
    BFixture f;
    ItemsWorld iw;
    uint8_t* logs = nullptr;
    for (auto& b : iw.reg.infos) {
        uint32_t id = 0;
        std::memcpy(&id, b.get() + off::kItemInfoId, 4);
        if (id == 0x11) logs = b.get();
    }
    REQUIRE(logs);
    logs[off::kItemInfoPlacedEntity] = 0x5a;  // the logs place a template (a stand-in for a door's)
    f.center.enqueue("spawn wood logs");
    f.center.enqueue("spawn wood logs 2 at (3738.5, 843.75, 1428)");
    f.center.enqueue("spawn torch");             // places nothing
    f.center.enqueue("spawn wood logs at 1,2");  // no z
    f.run(iw.w);
    CHECK(g_bspawns == 3);
    CHECK(bsaid("spawned 1"));
    CHECK(bsaid("spawned 2"));
    CHECK(bsaid("Torch places nothing"));
    CHECK(bsaid("usage: spawn <item name | template guid> [count 1-20] [at <x> <y> <z>]"));
}

TEST_CASE("builtin commands: goto and path read coordinates written with commas and brackets, or a pasted pos line") {
    BFixture f;
    path_stop();
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 0);
    f.center.enqueue("path (3740, 845, 1440) closest");
    f.center.enqueue("path 3740,845,1440");
    f.center.enqueue("path player 1 at (3734.5, 843.7, 1418.0) m");
    f.run(w);
    size_t refused = 0;
    for (const auto& l : g_bout) refused += l.find("path not asked") != std::string::npos;
    CHECK(refused == 3u);  // read fine; asked (and refused here: path_find is off in this fake)
    CHECK_FALSE(bsaid("usage: path"));
}

TEST_CASE("builtin commands (v36): remove <id> me (or player) gives the item back to whoever typed it") {
    BFixture f;
    f.state.get_component = rm_gc;
    RemoveWorld rw;
    f.center.enqueue("remove 60 me");
    f.center.enqueue("remove 63 player");
    f.run(rw.iw.w);
    // the typing player is 42 in this fake, who has no PlayerState: refused for that reason, not as a usage error
    CHECK_FALSE(bsaid("usage: remove"));
    CHECK(bsaid("entity 60 not removed: the refund's player is not a player"));
    CHECK(bsaid("entity 63 not removed: the refund's player is not a player"));
}

// Review (contributed god command): not for players who are no admin; an admin's "you" is their own entity; destroy
// ends the god of what it destroys at once (a new entity may get its id before the next god).
TEST_CASE("builtin commands: god is not for players who are no admin; an admin's god is their own entity") {
    BFixture f;
    EmfApi api = *api_table();
    api.attribute_get = fake_attr_get;
    api.hit_rule_add = fake_god_add;
    api.hit_rule_remove = fake_god_remove;
    f.ctx.api = &api;
    g_attr = {{{42, EMF_ATTR_HEALTH}, 100}, {{70, EMF_ATTR_HEALTH}, 30}};
    g_god_rules.clear();
    g_god_removed.clear();
    g_god_gone_rules.clear();
    g_god_refuse = false;
    ItemsWorld iw;
    MapStore ents;
    emf::test::plant_entities(iw.w, ents, {42, 70});
    f.ctx.player = [] { return emf::core::command_player(42); };  // as core.cpp sets it: a chat sender's entity
    CHECK_FALSE(f.center.for_players("god"));
    CommandSender guest;
    guest.from_chat = true;
    guest.account = 0xbb;
    guest.entity = 70;
    guest.restricted = true;
    f.center.enqueue("god on", GetTickCount64(), guest);
    f.run(iw.w);
    CHECK(g_god_rules.empty());
    CommandSender admin;
    admin.from_chat = true;
    admin.account = 0xaa;
    admin.entity = 70;
    f.center.enqueue("god on", GetTickCount64(), admin);
    f.run(iw.w);
    REQUIRE(g_god_rules.size() == 1u);
    CHECK(g_god_rules[0].target == 70u);  // the admin, not the host (42)
    builtin_world_reset(f.ctx);
}

TEST_CASE("builtin commands: destroy ends the god of the entity it destroys at once") {
    BFixture f;
    EmfApi api = *api_table();
    api.attribute_get = fake_attr_get;
    api.hit_rule_add = fake_god_add;
    api.hit_rule_remove = fake_god_remove;
    api.destroy = [](EmfWorld*, uint32_t) { return true; };
    f.ctx.api = &api;
    g_attr = {{{42, EMF_ATTR_HEALTH}, 100}, {{70, EMF_ATTR_HEALTH}, 30}};
    g_god_rules.clear();
    g_god_removed.clear();
    g_god_gone_rules.clear();
    g_god_refuse = false;
    ItemsWorld iw;
    MapStore ents;
    emf::test::plant_entities(iw.w, ents, {42, 70});
    f.center.enqueue("god 70");
    f.center.enqueue("destroy 70");
    f.run(iw.w);
    CHECK(bsaid("destroyed entity 70"));
    CHECK(g_god_removed == std::vector<uint32_t>{101});  // at once, not at the next god
    g_bout.clear();
    f.center.enqueue("god");
    f.run(iw.w);
    CHECK(bsaid("god: nobody has it"));
}

TEST_CASE("builtin commands: keys lists every key in use by key, the ones two switched-on users share first") {
    BFixture f;
    FakeWorld w;
    SettingDef wheel;
    wheel.key = "key";
    wheel.type = SettingType::Key;
    wheel.default_value = "F6";
    wheel.label = "Order wheel key";
    REQUIRE(f.settings->declare("companion", wheel));
    SettingDef other = wheel;
    other.label = "Stack key";
    other.default_value = "f6";
    REQUIRE(f.settings->declare("big_stacks", other));  // switched off (its manifest's default), same key
    SettingDef bulk = wheel;
    bulk.label = "Craft key";
    bulk.default_value = "F8";
    REQUIRE(f.settings->declare("sample_mod", bulk));
    SettingDef dup = wheel;
    dup.key = "key2";
    dup.label = "Second key";
    dup.default_value = "F8";
    REQUIRE(f.settings->declare("companion", dup));
    f.center.enqueue("keys");
    f.run(w);
    CHECK(bsaid("F8 (used twice: one press does both): Companion (Second key), sample_mod (Craft key)"));
    CHECK(bsaid("F6: Big Stacks (Stack key, not running), Companion (Order wheel key)"));
}

TEST_CASE("builtin commands: keys does not call keys shared on purpose a clash") {
    BFixture f;
    FakeWorld w;
    SettingDef read;
    read.key = "read_key";
    read.type = SettingType::Key;
    read.default_value = "E";
    read.label = "Read key";
    read.shared = true;
    REQUIRE(f.settings->declare("companion", read));
    SettingDef window = read;
    window.label = "Window key";
    REQUIRE(f.settings->declare("sample_mod", window));
    f.center.enqueue("keys");
    f.run(w);
    CHECK(bsaid("E (shared on purpose): Companion (Read key), sample_mod (Window key)"));
}
