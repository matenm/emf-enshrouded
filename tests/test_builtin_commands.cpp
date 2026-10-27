#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/api.h"
#include "core/builtin_commands.h"
#include "core/commands.h"
#include "core/daytime.h"
#include "core/inventory.h"
#include "core/items.h"
#include "core/manifest.h"
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
std::vector<uint32_t> g_pickup_amounts;
uint32_t b_spawn(void*, const void*, const SpawnSlice* comps) {
    ++g_bspawns;
    for (uint64_t i = 0; i < comps->count; ++i)
        if (comps->ptr[i].hash == kHashPickupItem) {
            uint16_t a = 0;
            std::memcpy(&a, static_cast<const uint8_t*>(comps->ptr[i].data) + 26, 2);
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

TEST_CASE("builtin commands: spawn places count copies near the player") {
    BFixture f;
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 0);
    f.center.enqueue("spawn 9543cfb0-b4d7-408e-905c-ccb79a15136d 2");
    f.center.enqueue("spawn not-a-guid");
    f.center.enqueue("spawn 9543cfb0-b4d7-408e-905c-ccb79a15136d 99");
    f.run(w);
    CHECK(g_bspawns == 2);
    CHECK(bsaid("spawned 2"));
    CHECK(bsaid("usage: spawn <template guid> [count 1-20]"));
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
