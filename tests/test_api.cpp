#include <windows.h>

#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/api.h"
#include "core/api_sizes.h"
#include "core/events.h"
#include "core/inventory.h"
#include "core/items.h"
#include "core/spawn.h"
#include "core/system_programs.h"
#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "fake_world.h"

using namespace emf;
using namespace emf::core;
using emf::test::FakeWorld;
using emf::test::MapStore;
using emf::test::SingletonStore;

namespace {
uint64_t g_now = 100000;
std::vector<std::string> g_lines;
int64_t g_transform[7] = {int64_t(5) << 32, 0, 0, 0, 0, 0, 0};

void* fake_gc(void* out, void*, uint32_t hash, uint32_t entity) {
    auto o = static_cast<uint64_t*>(out);
    o[0] = o[1] = 0;
    if (hash == off::kHashCurrentTransform && entity == 42) {
        o[0] = reinterpret_cast<uint64_t>(g_transform);
        o[1] = 56;
    }
    return out;
}

struct Fixture {
    CoreState state;
    Fixture() {
        state.get_component = fake_gc;
        state.log_line = [](const std::string& s) { g_lines.push_back(s); };
        state.now_ms = [] { return g_now; };
        state.sigs[static_cast<size_t>(Sig::SimStep)] = Resolved{0x140a1e2e0, 1, true};
        state.known_build = true;
        set_core_state(&state);
    }
    ~Fixture() { set_core_state(nullptr); }
};

EmfWorld* as_world(FakeWorld& w) { return reinterpret_cast<EmfWorld*>(w.p()); }

bool logged(const std::string& needle) {
    for (auto& l : g_lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

const uint8_t kGuid[16] = {1};
int g_spawn_calls = 0, g_destroy_calls = 0;
uint32_t api_fake_spawn(void*, const void*, const SpawnSlice*) {
    ++g_spawn_calls;
    return 6808;
}
uint64_t api_fake_destroy(void*, uint32_t, char, uint64_t) {
    ++g_destroy_calls;
    return 1;
}
}  // namespace

TEST_CASE("WorldTracker returns the latest world of a kind until it goes stale") {
    WorldTracker t(2000);
    FakeWorld a, b;
    CHECK(t.latest(EMF_WORLD_SERVER, 1000) == nullptr);
    t.on_tick(as_world(a), EMF_WORLD_SERVER, 1000);
    CHECK(t.latest(EMF_WORLD_SERVER, 2500) == as_world(a));
    t.on_tick(as_world(b), EMF_WORLD_SERVER, 2600);
    CHECK(t.latest(EMF_WORLD_SERVER, 2700) == as_world(b));
    CHECK(t.latest(EMF_WORLD_CLIENT, 2700) == nullptr);
    CHECK(t.latest(EMF_WORLD_SERVER, 5000) == nullptr);  // stale after 2 s
    CHECK(t.latest(7, 2700) == nullptr);                 // bad kind
}

TEST_CASE("WorldTracker: a reader whose clock is a little behind the ticking thread still sees the world") {
    WorldTracker t(2000);
    FakeWorld a;
    // Thread A read now = 1000, then thread B ticked the world at 1005 before A looked it up.
    t.on_tick(as_world(a), EMF_WORLD_SERVER, 1005);
    CHECK(t.latest(EMF_WORLD_SERVER, 1000) == as_world(a));
    // latest_within: an explicit age limit (the command router waits longer before calling the server world gone).
    CHECK(t.latest_within(EMF_WORLD_SERVER, 9000, 10000) == as_world(a));
    CHECK(t.latest_within(EMF_WORLD_SERVER, 12000, 10000) == nullptr);
    CHECK(t.latest_within(EMF_WORLD_CLIENT, 1000, 10000) == nullptr);
}

TEST_CASE("api_table has version, size and every function filled in") {
    const EmfApi* api = api_table();
    REQUIRE(api != nullptr);
    CHECK(api->version == EMF_API_VERSION);
    CHECK(api->size == sizeof(EmfApi));
    CHECK(api->log);
    CHECK(api->server_world);
    CHECK(api->client_world);
    CHECK(api->world_kind);
    CHECK(api->world_tick);
    CHECK(api->ecs_time_ns);
    CHECK(api->singleton);
    CHECK(api->local_player);
    CHECK(api->get_component);
    CHECK(api->for_each_entity);
    CHECK(api->on_tick);
    CHECK(api->run_on_world);
    CHECK(api->resolve);
    CHECK(api->hook);
}

TEST_CASE("api: worlds, kind, tick and ECS time come from the tracked worlds") {
    Fixture f;
    FakeWorld server;
    server.put<uint8_t>(off::kKind, 0);
    server.put<uint32_t>(off::kTick, 600);
    server.put<int64_t>(off::kTickPeriodNs, 16666667);
    f.state.tracker.on_tick(as_world(server), EMF_WORLD_SERVER, g_now);
    const EmfApi* api = api_table();
    REQUIRE(api->server_world() == as_world(server));
    CHECK(api->client_world() == nullptr);
    CHECK(api->world_kind(as_world(server)) == EMF_WORLD_SERVER);
    CHECK(api->world_tick(as_world(server)) == 600);
    CHECK(api->ecs_time_ns(as_world(server)) == 600ll * 16666667ll);
}

TEST_CASE("api: local_player reads LocalPlayerData+4 from the client world; get_component passes size") {
    Fixture f;
    FakeWorld client;
    client.put<uint8_t>(off::kKind, 2);
    uint8_t lpd[8] = {0, 0, 0, 0, 42, 0, 0, 0};
    SingletonStore ss;
    ss.plant(client, "LocalPlayerData", lpd);
    f.state.tracker.on_tick(as_world(client), EMF_WORLD_CLIENT, g_now);
    const EmfApi* api = api_table();
    CHECK(api->singleton(as_world(client), "LocalPlayerData") == lpd);
    REQUIRE(api->local_player() == 42);
    uint64_t size = 0;
    void* t = api->get_component(as_world(client), off::kHashCurrentTransform, 42, &size);
    CHECK(t == g_transform);
    CHECK(size == 56);
    CHECK(api->get_component(as_world(client), off::kHashCurrentTransform, 7, nullptr) == nullptr);
}

TEST_CASE("api: for_each_entity visits the entity map; resolve finds signatures by name") {
    Fixture f;
    FakeWorld w;
    MapStore ms;
    emf::test::plant_entities(w, ms, {2, 3});
    const EmfApi* api = api_table();
    std::vector<uint32_t> ids;
    api->for_each_entity(as_world(w), [](uint32_t id, void* u) { static_cast<std::vector<uint32_t>*>(u)->push_back(id); },
                         &ids);
    CHECK(ids.size() == 2);
    CHECK(api->resolve("SimStep") == reinterpret_cast<void*>(0x140a1e2e0));
    CHECK(api->resolve("GetComponent") == nullptr);  // not usable in this fixture
    CHECK(api->resolve("Nope") == nullptr);
}

TEST_CASE("api: log prefixes the mod name; run_on_world posts to the dispatcher") {
    Fixture f;
    g_lines.clear();
    const EmfApi* api = api_table();
    api->log("sample", "hello %d", 7);
    REQUIRE(g_lines.size() == 1);
    CHECK(g_lines[0] == "[sample] hello 7");
    static int ran = 0;
    api->run_on_world(EMF_WORLD_SERVER, [](EmfWorld*, void*) { ++ran; }, nullptr);
    f.state.dispatcher.run(nullptr, EMF_WORLD_SERVER, [](const std::string&) {});
    CHECK(ran == 1);
}

TEST_CASE("api functions are safe without a core state") {
    set_core_state(nullptr);
    const EmfApi* api = api_table();
    CHECK(api->server_world() == nullptr);
    CHECK(api->local_player() == 0);
    CHECK(api->resolve("SimStep") == nullptr);
    api->log("x", "no crash");
}

TEST_CASE("api v2: hook_program queues a request in the program registry") {
    Fixture f;
    f.state.programs = std::make_unique<ProgramRegistry>([](uintptr_t, void*, void**) { return true; });
    const EmfApi* api = api_table();
    CHECK(api->version >= 2);
    REQUIRE(api->hook_program != nullptr);
    CHECK(api->hook_program("player_state", [](void*, void*) {}, nullptr, nullptr));
    CHECK_FALSE(api->hook_program("", [](void*, void*) {}, nullptr, nullptr));
    f.state.programs.reset();
    CHECK_FALSE(api->hook_program("player_state", [](void*, void*) {}, nullptr, nullptr));
}

TEST_CASE("api v2: register_program queues a system program for future worlds") {
    Fixture f;
    f.state.system_programs.reset();
    const EmfApi* api = api_table();
    REQUIRE(api->register_program != nullptr);
    CHECK(api->register_program("mymod_tick", EMF_WORLD_SERVER, [](void*, void*) {}, nullptr));
    CHECK_FALSE(api->register_program("mymod_tick", EMF_WORLD_SERVER, [](void*, void*) {}, nullptr));  // duplicate
    CHECK_FALSE(api->register_program("bad_kind", EMF_WORLD_PREDICTION, [](void*, void*) {}, nullptr));
    CHECK(f.state.system_programs.count() == 1);
}

TEST_CASE("api v2: spawn and destroy refuse without resolved signatures or on a client world") {
    Fixture f;
    const EmfApi* api = api_table();
    REQUIRE(api->spawn != nullptr);
    REQUIRE(api->destroy != nullptr);
    FakeWorld server;
    server.put<uint8_t>(off::kKind, 0);
    const uint8_t guid[16] = {1};
    CHECK_FALSE(api->spawn(as_world(server), guid, 0, 0, 0));  // SpawnWrapper not resolved in the fixture
    CHECK_FALSE(api->destroy(as_world(server), 5));             // DestroyEntity not resolved in the fixture
    CHECK_FALSE(api->spawn(nullptr, guid, 0, 0, 0));
}

TEST_CASE("api v2: on_save / on_load subscribe to the core's save events") {
    Fixture f;
    const EmfApi* api = api_table();
    REQUIRE(api->on_save != nullptr);
    REQUIRE(api->on_load != nullptr);
    static int saves = 0, loads = 0;
    api->on_save([](uint16_t, void*) { ++saves; }, nullptr);
    api->on_load([](uint16_t, void*) { ++loads; }, nullptr);
    f.state.save_events.fire(1);
    f.state.load_events.fire(1);
    f.state.load_events.fire(1);
    CHECK(saves == 1);
    CHECK(loads == 2);
}

TEST_CASE("api v2: spawn and destroy only act on the world passed to the running callback") {
    Fixture f;
    f.state.sigs[static_cast<size_t>(Sig::SpawnWrapper)] = Resolved{reinterpret_cast<uintptr_t>(&api_fake_spawn), 1, true};
    f.state.sigs[static_cast<size_t>(Sig::DestroyEntity)] = Resolved{reinterpret_cast<uintptr_t>(&api_fake_destroy), 1, true};
    FakeWorld server, other;
    server.put<uint8_t>(off::kKind, 0);
    other.put<uint8_t>(off::kKind, 0);
    const EmfApi* api = api_table();
    g_lines.clear();
    g_spawn_calls = g_destroy_calls = 0;
    // outside any callback (e.g. another thread): refused, logged, the game is not called
    CHECK(api->spawn(as_world(server), kGuid, 1, 2, 3) == 0u);
    CHECK_FALSE(api->destroy(as_world(server), 5000));
    CHECK(g_spawn_calls == 0);
    CHECK(g_destroy_calls == 0);
    CHECK(logged("spawn refused"));
    CHECK(logged("destroy refused"));
    struct Ctx {
        const EmfApi* api;
        EmfWorld* other;
        uint32_t same = 0, cross = 1;
        bool destroyed = false;
    } ctx{api, as_world(other)};
    api->run_on_world(EMF_WORLD_SERVER, [](EmfWorld* w, void* u) {
        auto* c = static_cast<Ctx*>(u);
        c->same = c->api->spawn(w, kGuid, 1, 2, 3);
        c->cross = c->api->spawn(c->other, kGuid, 1, 2, 3);  // a world other than the one being dispatched
        c->destroyed = c->api->destroy(w, 5000);
    }, &ctx);
    f.state.dispatcher.run(as_world(server), EMF_WORLD_SERVER, [](const std::string&) {});
    CHECK(ctx.same == 6808u);
    CHECK(ctx.cross == 0u);
    CHECK(ctx.destroyed);
    CHECK(g_spawn_calls == 1);
    CHECK(g_destroy_calls == 1);
}

namespace {
// Entity 7 is a player (it has PlayerState); every other entity is not.
void* fake_gc_player(void* out, void*, uint32_t hash, uint32_t entity) {
    static uint8_t state[16] = {};
    auto o = static_cast<uint64_t*>(out);
    o[0] = o[1] = 0;
    if (hash == emf::gen::ecs::PlayerState::kHash && entity == 7) {
        o[0] = reinterpret_cast<uint64_t>(state);
        o[1] = sizeof state;
    }
    return out;
}
}  // namespace

TEST_CASE("api v2: destroy never removes a player (reference.md: \"never a player\"), whatever id a mod passes") {
    Fixture f;
    f.state.get_component = fake_gc_player;
    f.state.sigs[static_cast<size_t>(Sig::DestroyEntity)] = Resolved{reinterpret_cast<uintptr_t>(&api_fake_destroy), 1, true};
    FakeWorld server;
    server.put<uint8_t>(off::kKind, 0);
    g_lines.clear();
    g_destroy_calls = 0;
    struct Ctx {
        const EmfApi* api;
        bool player = true, other = false;
    } ctx{api_table()};
    ctx.api->run_on_world(EMF_WORLD_SERVER, [](EmfWorld* w, void* u) {
        auto* c = static_cast<Ctx*>(u);
        c->player = c->api->destroy(w, 7);
        c->other = c->api->destroy(w, 5000);
    }, &ctx);
    f.state.dispatcher.run(as_world(server), EMF_WORLD_SERVER, [](const std::string&) {});
    CHECK_FALSE(ctx.player);
    CHECK(ctx.other);
    CHECK(g_destroy_calls == 1);  // the game was asked once: for 5000
    CHECK(logged("destroy refused: entity 7 is a player"));
}

TEST_CASE("api v2: register_program is refused and logged once worlds have registered their programs") {
    Fixture f;
    f.state.system_programs.reset();
    g_lines.clear();
    f.state.system_programs.close_registration();
    const EmfApi* api = api_table();
    CHECK_FALSE(api->register_program("late_tick", EMF_WORLD_SERVER, [](void*, void*) {}, nullptr));
    CHECK(logged("register_program"));
    CHECK(logged("late_tick"));
    f.state.system_programs.reset();
}

// ---- v3: programs with events ----
namespace {
using emf::test::EventStore;
using emf::test::FakeProgramCtx;
using emf::test::FakeQueue;

uint64_t v3_register(void*, const void*, char) { return 1; }

alignas(8) uint8_t g_v3_event[64];
struct {
    void* buffer = nullptr;
    uint32_t hash = 0, size = 0;
    uint16_t id = 0;
    int calls = 0;
} g_v3_emit;
void* v3_emit(void* buffer, uint32_t hash, uint32_t size, uint16_t id) {
    g_v3_emit = {buffer, hash, size, id, g_v3_emit.calls + 1};
    std::memset(g_v3_event, 0, sizeof(g_v3_event));
    return g_v3_event;
}

FakeQueue* g_v3_queue = nullptr;
void v3_iter_begin(void* ctx, void* iter, uint32_t) {
    auto* it = static_cast<uint64_t*>(iter);
    it[0] = reinterpret_cast<uint64_t>(ctx);
    it[1] = reinterpret_cast<uint64_t>(g_v3_queue ? g_v3_queue->q : nullptr);  // A_Event (lowest type index)
}

struct V3Results {
    void* sent = nullptr;
    void* wrong_size = nullptr;
    void* not_out = nullptr;
    uint32_t read = 0, read_wrong_size = 0;
    std::vector<uint32_t> values;
} g_v3;
void v3_program(void* ctx, void*) {
    const EmfApi* api = api_table();
    g_v3.sent = api->emit_event(ctx, "C_Event", 32);
    g_v3.wrong_size = api->emit_event(ctx, "C_Event", 31);
    g_v3.not_out = api->emit_event(ctx, "A_Event", 24);
    g_v3.read = api->read_events(ctx, "A_Event", 24, [](const void* e, void*) {
        uint32_t v;
        std::memcpy(&v, static_cast<const uint8_t*>(e) + 8, 4);
        g_v3.values.push_back(v);
    }, nullptr);
    g_v3.read_wrong_size = api->read_events(ctx, "A_Event", 8, [](const void*, void*) {}, nullptr);
}
}  // namespace

TEST_CASE("api v3: version 3 with the event functions filled in") {
    const EmfApi* api = api_table();
    CHECK(api->version >= 3);
    CHECK(api->register_program_def);
    CHECK(api->emit_event);
    CHECK(api->read_events);
}

TEST_CASE("api v3: register_program_def queues a program with its event lists") {
    Fixture f;
    f.state.system_programs.reset();
    const EmfApi* api = api_table();
    const char* in[] = {"EntityDiedEvent", nullptr};
    const char* out[] = {"StartGuardingEvent", "StopGuardingEvent", nullptr};
    EmfProgramDef def{sizeof(EmfProgramDef), "guard", EMF_WORLD_SERVER, [](void*, void*) {}, nullptr, in, out};
    CHECK(api->register_program_def(&def));
    const ProgramDesc& d = system_program_desc(0);
    REQUIRE(d.eventsIn.n == 1);
    CHECK(std::string(d.eventsIn.p[0].s, d.eventsIn.p[0].n) == "EntityDiedEvent");
    CHECK(d.eventsOut.n == 2);
    EmfProgramDef no_events{sizeof(EmfProgramDef), "plain", EMF_WORLD_CLIENT, [](void*, void*) {}, nullptr, nullptr, nullptr};
    CHECK(api->register_program_def(&no_events));
    const char* bad[] = {"not valid", nullptr};
    EmfProgramDef bad_def{sizeof(EmfProgramDef), "bad", EMF_WORLD_SERVER, [](void*, void*) {}, nullptr, bad, nullptr};
    g_lines.clear();
    CHECK_FALSE(api->register_program_def(&bad_def));
    CHECK(logged("register_program_def('bad') refused"));
    EmfProgramDef short_def = def;
    short_def.size = 8;
    CHECK_FALSE(api->register_program_def(&short_def));
    CHECK_FALSE(api->register_program_def(nullptr));
    f.state.system_programs.reset();
}

TEST_CASE("api v3: emit_event and read_events outside a program call are refused and logged") {
    Fixture f;
    const EmfApi* api = api_table();
    g_lines.clear();
    int dummy = 0;
    CHECK(api->emit_event(&dummy, "StartGuardingEvent", 16) == nullptr);
    CHECK(logged("emit_event('StartGuardingEvent') refused"));
    CHECK(api->read_events(&dummy, "HitEvent", 248, [](const void*, void*) {}, nullptr) == 0);
    CHECK(logged("read_events('HitEvent') refused"));
    CHECK(api->emit_event(nullptr, nullptr, 0) == nullptr);
}

TEST_CASE("api v3: inside the program call emit_event sends declared events and read_events reads them") {
    Fixture f;
    f.state.system_programs.reset();
    f.state.sigs[static_cast<size_t>(Sig::EmitEvent)] = Resolved{reinterpret_cast<uintptr_t>(&v3_emit), 1, false};
    f.state.known_build = false;  // fake addresses: trust the unique match
    FakeWorld w;
    EventStore ev;
    ev.attach(w);
    ev.plant(w, "A_Event", 2, 24);
    ev.plant(w, "C_Event", 3, 32);
    FakeQueue queue(1, 3);
    queue.fill(3, 24);
    g_v3_queue = &queue;
    set_program_iter_begin(v3_iter_begin);
    const EmfApi* api = api_table();
    const char* in[] = {"A_Event", nullptr};
    const char* out[] = {"C_Event", nullptr};
    EmfProgramDef def{sizeof(EmfProgramDef), "v3prog", EMF_WORLD_SERVER, v3_program, nullptr, in, out};
    REQUIRE(api->register_program_def(&def));
    REQUIRE(f.state.system_programs.register_into(w.p(), v3_register) == 1);
    FakeProgramCtx ctx(w.p(), 42, 10, 100);
    g_v3 = {};
    g_v3_emit = {};
    g_lines.clear();
    system_program_desc(0).update(ctx.p());
    CHECK(g_v3.sent == g_v3_event);
    CHECK(g_v3_emit.calls == 1);  // the refused calls never reached the engine
    CHECK(g_v3_emit.hash == event_hash("C_Event"));
    CHECK(g_v3_emit.size == 32);
    CHECK(g_v3_emit.id == 42);
    CHECK(g_v3_emit.buffer == ctx.buffer_obj);
    int64_t stamp = 0;
    std::memcpy(&stamp, g_v3_event, 8);
    CHECK(stamp == 1000);
    CHECK(g_v3.wrong_size == nullptr);
    CHECK(logged("the game's 'C_Event' is 32 bytes, the mod expects 31"));
    CHECK(g_v3.not_out == nullptr);
    CHECK(logged("'A_Event' is not in the program's events_out"));
    CHECK(g_v3.read == 3);
    CHECK(g_v3.values == std::vector<uint32_t>{0, 1, 2});
    CHECK(g_v3.read_wrong_size == 0);
    set_program_iter_begin(nullptr);
    g_v3_queue = nullptr;
    f.state.system_programs.reset();
}

namespace {
struct V3bResults {
    void* revived = nullptr;
    int64_t revived_first8 = -1;
    uint32_t odd_read = 99;
} g_v3b;
void v3b_program(void* ctx, void*) {
    const EmfApi* api = api_table();
    g_v3b.revived = api->emit_event(ctx, "Revived_Event", 8);
    if (g_v3b.revived) std::memcpy(&g_v3b.revived_first8, g_v3b.revived, 8);
    g_v3b.odd_read = api->read_events(ctx, "Odd_Event", 12, [](const void*, void*) {}, nullptr);
}
}  // namespace

TEST_CASE("api v3: events without a timestamp are not stamped; odd-sized events are not read") {
    Fixture f;
    f.state.system_programs.reset();
    f.state.sigs[static_cast<size_t>(Sig::EmitEvent)] = Resolved{reinterpret_cast<uintptr_t>(&v3_emit), 1, false};
    f.state.known_build = false;
    FakeWorld w;
    EventStore ev;
    ev.attach(w);
    ev.plant(w, "Revived_Event", 2, 8, 0, false);
    ev.plant(w, "Odd_Event", 3, 12, 0, false);
    set_program_iter_begin(v3_iter_begin);
    const EmfApi* api = api_table();
    const char* in[] = {"Odd_Event", nullptr};
    const char* out[] = {"Revived_Event", nullptr};
    EmfProgramDef def{sizeof(EmfProgramDef), "v3b", EMF_WORLD_SERVER, v3b_program, nullptr, in, out};
    REQUIRE(api->register_program_def(&def));
    REQUIRE(f.state.system_programs.register_into(w.p(), v3_register) == 1);
    FakeProgramCtx ctx(w.p(), 7, 10, 100);
    g_v3b = {};
    g_lines.clear();
    system_program_desc(0).update(ctx.p());
    CHECK(g_v3b.revived == g_v3_event);
    CHECK(g_v3b.revived_first8 == 0);
    CHECK(g_v3b.odd_read == 0);
    CHECK(logged("read_events('Odd_Event') refused"));
    set_program_iter_begin(nullptr);
    f.state.system_programs.reset();
}

// ---- v4: on_event ----
TEST_CASE("api v4: version 4 with on_event") {
    const EmfApi* api = api_table();
    CHECK(api->version >= 4);
    CHECK(api->on_event);
}

TEST_CASE("api v4: on_event subscribes through the relay, and is refused (logged) once the worlds are set up") {
    Fixture f;
    f.state.relay.reset();
    const EmfApi* api = api_table();
    auto cb = [](EmfWorld*, const void*, uint32_t, void*) {};
    CHECK(api->on_event(EMF_WORLD_SERVER, "EntityDiedEvent", 16, cb, nullptr));
    CHECK(f.state.relay.events(EMF_WORLD_SERVER) == std::vector<std::string>{"EntityDiedEvent"});
    g_lines.clear();
    CHECK_FALSE(api->on_event(EMF_WORLD_PREDICTION, "HitEvent", 248, cb, nullptr));
    CHECK(logged("on_event('HitEvent') refused"));
    CHECK_FALSE(api->on_event(EMF_WORLD_SERVER, nullptr, 0, cb, nullptr));
    f.state.relay.close();
    g_lines.clear();
    CHECK_FALSE(api->on_event(EMF_WORLD_SERVER, "PickupEvent", 48, cb, nullptr));
    CHECK(logged("on_event('PickupEvent') refused: call it from emf_mod_load"));
    f.state.relay.reset();
}

// ---- v5: items ----
namespace {
using emf::test::FakeItemRegistry;

struct V5Captured {
    uint8_t guid[16];
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> comps;
    int calls = 0;
} g_v5;
uint32_t v5_spawn(void*, const void* guid, const SpawnSlice* comps) {
    std::memcpy(g_v5.guid, guid, 16);
    g_v5.comps.clear();
    for (uint64_t i = 0; i < comps->count; ++i) {
        const auto* b = static_cast<const uint8_t*>(comps->ptr[i].data);
        g_v5.comps.push_back({comps->ptr[i].hash, std::vector<uint8_t>(b, b + comps->ptr[i].size)});
    }
    ++g_v5.calls;
    return 7001;
}
struct ItemWorld {
    FakeWorld w;
    SingletonStore singleton;
    FakeItemRegistry reg;
    ItemWorld() {
        w.put<uint8_t>(off::kKind, 0);
        reg.add(0x11, 10, 20, "Wood_Log");
        reg.add(0x22, 40, 1, "Torch");
        singleton.plant(w, "g38_items::ItemRegistry", reg.reg);
    }
};
}  // namespace

TEST_CASE("api v5: version 5 with the item functions") {
    const EmfApi* api = api_table();
    CHECK(api->version >= 5);
    CHECK(api->for_each_item);
    CHECK(api->find_item);
    CHECK(api->item_info);
    CHECK(api->give_item);
    CHECK(api->spawn_ex);
}

TEST_CASE("api v5: list, find and describe items through the world's item registry") {
    Fixture f;
    ItemWorld iw;
    const EmfApi* api = api_table();
    std::vector<std::string> names;
    api->for_each_item(as_world(iw.w), [](const EmfItemInfo* i, void* u) {
        static_cast<std::vector<std::string>*>(u)->push_back(i->name);
    }, &names);
    CHECK(names.size() == 2);
    CHECK(api->find_item(as_world(iw.w), "Torch") == 0x22u);
    CHECK(api->find_item(as_world(iw.w), "torch") == 0x22u);
    CHECK(api->find_item(as_world(iw.w), "Nothing") == 0u);
    EmfItemInfo info{};
    info.size = sizeof(info);
    REQUIRE(api->item_info(as_world(iw.w), 0x11, &info));
    CHECK(std::string(info.name) == "Wood_Log");
    CHECK(info.max_stack == 20);
    CHECK(info.guid[0] == 10);
    CHECK_FALSE(api->item_info(as_world(iw.w), 0x99, &info));
    EmfItemInfo short_info{};
    short_info.size = 8;
    CHECK_FALSE(api->item_info(as_world(iw.w), 0x11, &short_info));
    FakeWorld bare;  // no registry singleton
    CHECK(api->find_item(as_world(bare), "Torch") == 0u);
}

TEST_CASE("api sizes: item_info takes the v5 size (the end of name, not sizeof) and writes nothing past it") {
    Fixture f;
    ItemWorld iw;
    const EmfApi* api = api_table();
    alignas(8) uint8_t buf[sizeof(EmfItemInfo) + 8];
    std::memset(buf, 0xAB, sizeof buf);
    auto* info = reinterpret_cast<EmfItemInfo*>(buf);
    info->size = kItemInfoV5Size;  // sizeof grows when a field is appended; a mod built before passes this
    REQUIRE(api->item_info(as_world(iw.w), 0x11, info));
    CHECK(std::string(info->name) == "Wood_Log");
    for (size_t i = kItemInfoV5Size; i < sizeof buf; ++i) CHECK(buf[i] == 0xAB);
    info->size = kItemInfoV5Size - 1;
    CHECK_FALSE(api->item_info(as_world(iw.w), 0x11, info));
}

TEST_CASE("api v5: give_item drops a pickup at the player, only inside a callback on that world") {
    Fixture f;
    f.state.sigs[static_cast<size_t>(Sig::SpawnWrapper)] = Resolved{reinterpret_cast<uintptr_t>(&v5_spawn), 1, false};
    f.state.known_build = false;
    ItemWorld iw;
    const EmfApi* api = api_table();
    g_lines.clear();
    g_v5 = {};
    CHECK(api->give_item(as_world(iw.w), 42, 0x22, 3) == 0u);  // not inside a callback
    CHECK(logged("give_item refused"));
    struct Ctx {
        const EmfApi* api;
        uint32_t given = 0, unknown = 1, no_player = 1;
    } ctx{api};
    api->run_on_world(EMF_WORLD_SERVER, [](EmfWorld* w, void* u) {
        auto* c = static_cast<Ctx*>(u);
        c->given = c->api->give_item(w, 42, 0x22, 3);       // entity 42 has a transform (5, 0, 0) in fake_gc
        c->unknown = c->api->give_item(w, 42, 0x99, 1);
        c->no_player = c->api->give_item(w, 7, 0x22, 1);
    }, &ctx);
    f.state.dispatcher.run(as_world(iw.w), EMF_WORLD_SERVER, [](const std::string&) {});
    CHECK(ctx.given == 7001u);
    CHECK(ctx.unknown == 0u);
    CHECK(ctx.no_player == 0u);
    CHECK(g_v5.calls == 1);
    REQUIRE(g_v5.comps.size() == 2);
    int64_t x = 0, y = 0;
    std::memcpy(&x, g_v5.comps[0].second.data(), 8);
    std::memcpy(&y, g_v5.comps[0].second.data() + 8, 8);
    CHECK(x == (int64_t(5) << 32));
    CHECK(y == (int64_t(1) << 32));  // dropped 1 m above the player
    CHECK(g_v5.comps[1].first == 0x08775a90u);
    uint16_t amount = 0;
    std::memcpy(&amount, g_v5.comps[1].second.data() + 26, 2);
    CHECK(amount == 3);
    CHECK(logged("give_item: unknown item"));
    CHECK(logged("give_item: player 7"));
}

TEST_CASE("api v5: spawn_ex passes extra component data inside a callback") {
    Fixture f;
    f.state.sigs[static_cast<size_t>(Sig::SpawnWrapper)] = Resolved{reinterpret_cast<uintptr_t>(&v5_spawn), 1, false};
    f.state.known_build = false;
    FakeWorld server;
    server.put<uint8_t>(off::kKind, 0);
    const EmfApi* api = api_table();
    g_v5 = {};
    static const uint32_t level = 9;
    static const EmfComponentInit inits[1] = {{0x1234u, 4, &level}};
    struct Ctx {
        const EmfApi* api;
        uint32_t id = 0;
    } ctx{api};
    api->run_on_world(EMF_WORLD_SERVER, [](EmfWorld* w, void* u) {
        auto* c = static_cast<Ctx*>(u);
        c->id = c->api->spawn_ex(w, kGuid, 1, 2, 3, inits, 1);
    }, &ctx);
    f.state.dispatcher.run(as_world(server), EMF_WORLD_SERVER, [](const std::string&) {});
    CHECK(ctx.id == 7001u);
    REQUIRE(g_v5.comps.size() == 2);
    CHECK(g_v5.comps[1].first == 0x1234u);
}

// ---- v6: commands ----
namespace {
std::vector<std::string> g_cmd_out;
void v6_cmd(EmfWorld*, int argc, const char* const* argv, void*) {
    api_table()->command_reply("%s got %d args", argv[0], argc - 1);
}
}  // namespace

TEST_CASE("api v6: register_command adds a command that replies through command_reply") {
    Fixture f;
    f.state.commands.reset();
    const EmfApi* api = api_table();
    CHECK(api->version >= 6);
    REQUIRE(api->register_command);
    REQUIRE(api->command_reply);
    CHECK(api->register_command("mymod.hello", ": say hello", v6_cmd, nullptr));
    g_lines.clear();
    CHECK_FALSE(api->register_command("MyMod.Hello", "", v6_cmd, nullptr));  // duplicate
    CHECK(logged("register_command('MyMod.Hello') refused"));
    CHECK_FALSE(api->register_command(nullptr, "", v6_cmd, nullptr));
    f.state.commands.enqueue("mymod.hello a b");
    g_cmd_out.clear();
    f.state.commands.run_pending(nullptr, [](const std::string& s) { g_cmd_out.push_back(s); });
    REQUIRE(g_cmd_out.size() == 2);
    CHECK(g_cmd_out[1] == "mymod.hello got 2 args");
    api->command_reply("outside %d", 1);  // no command running: dropped, no crash
    f.state.commands.reset();
}

// ---- v7: template components ----
namespace {
uint8_t g_v7_tpl[32] = {9};
void* fake_tpl(uint8_t*, uint32_t hash, uint32_t entity) { return hash == 0x1a8b5909u && entity == 42 ? g_v7_tpl : nullptr; }
}  // namespace

TEST_CASE("api v7: get_template_component forwards to the game's function; null world or no signature gives null") {
    Fixture f;
    const EmfApi* api = api_table();
    CHECK(api->version >= 7);
    CHECK(EMF_API_VERSION >= 7);
    REQUIRE(api->get_template_component);
    FakeWorld w;
    CHECK(api->get_template_component(as_world(w), 0x1a8b5909u, 42, nullptr) == nullptr);  // no function resolved yet
    f.state.get_template_component = fake_tpl;
    CHECK(api->get_template_component(as_world(w), 0x1a8b5909u, 42, nullptr) == g_v7_tpl);
    CHECK(api->get_template_component(as_world(w), 0x1a8b5909u, 43, nullptr) == nullptr);
    CHECK(api->get_template_component(nullptr, 0x1a8b5909u, 42, nullptr) == nullptr);
}

// ---- v7: item definition writes ----
TEST_CASE("api v7: item_data gives the ItemInfo and its size; set_item_max_stack changes every live world's registry") {
    Fixture f;
    ItemWorld server;
    ItemWorld client;
    client.w.put<uint8_t>(off::kKind, 2);
    const EmfApi* api = api_table();
    REQUIRE(api->item_data);
    REQUIRE(api->set_item_max_stack);
    CHECK_FALSE(api->set_item_max_stack(0x11, 999));  // no live world yet
    f.state.tracker.on_tick(as_world(server.w), EMF_WORLD_SERVER, g_now);
    f.state.tracker.on_tick(as_world(client.w), EMF_WORLD_CLIENT, g_now);
    uint64_t size = 0;
    void* info = api->item_data(as_world(server.w), 0x11, &size);
    REQUIRE(info != nullptr);
    CHECK(size == 0x828);
    CHECK(*static_cast<uint32_t*>(info) == 0x11);
    CHECK(api->item_data(as_world(server.w), 0x99, &size) == nullptr);
    CHECK(api->item_data(nullptr, 0x11, nullptr) == nullptr);
    CHECK(api->set_item_max_stack(0x11, 999));
    CHECK(item_by_id(server.reg.reg, 0x11)->max_stack == 999);
    CHECK(item_by_id(client.reg.reg, 0x11)->max_stack == 999);
    CHECK_FALSE(api->set_item_max_stack(0x11, 0));
    CHECK_FALSE(api->set_item_max_stack(0x99, 10));
}

TEST_CASE("api v7: set_item_max_stack writes an ItemInfo shared by both worlds once") {
    Fixture f;
    ItemWorld server;
    FakeWorld client;
    client.put<uint8_t>(off::kKind, 2);
    SingletonStore ss;
    ss.plant(client, "g38_items::ItemRegistry", server.reg.reg);  // same registry, same ItemInfo pointers
    f.state.tracker.on_tick(as_world(server.w), EMF_WORLD_SERVER, g_now);
    f.state.tracker.on_tick(as_world(client), EMF_WORLD_CLIENT, g_now);
    g_lines.clear();
    CHECK(api_table()->set_item_max_stack(0x22, 5));
    CHECK(item_by_id(server.reg.reg, 0x22)->max_stack == 5);
}

TEST_CASE("api v7: write_memory writes game data the game keeps read-only, and refuses code") {
    Fixture f;
    const EmfApi* api = api_table();
    REQUIRE(api->write_memory);
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    REQUIRE(page != nullptr);
    DWORD old = 0;
    VirtualProtect(page, 0x1000, PAGE_READONLY, &old);
    const float v = 6.0f;
    CHECK(api->write_memory(page + 8, &v, sizeof(v)));
    float back = 0;
    std::memcpy(&back, page + 8, 4);
    CHECK(back == 6.0f);
    VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old);
    CHECK_FALSE(api->write_memory(page + 8, &v, sizeof(v)));
    CHECK_FALSE(api->write_memory(nullptr, &v, sizeof(v)));
    VirtualFree(page, 0, MEM_RELEASE);
}

// ---- edge cases ----
TEST_CASE("api v7: set_item_max_stack works inside the server world's own callback even when its last tick is old") {
    Fixture f;
    ItemWorld server;
    f.state.tracker.on_tick(as_world(server.w), EMF_WORLD_SERVER, g_now);
    g_now += 5000;  // a long pass: the world has not stamped a tick for 5 s (its own thread is busy with us)
    struct Ctx {
        bool ok = false;
    } ctx;
    f.state.dispatcher.post(EMF_WORLD_SERVER, [](EmfWorld*, void* u) {
        static_cast<Ctx*>(u)->ok = api_table()->set_item_max_stack(0x11, 777);
    }, &ctx);
    f.state.dispatcher.run(as_world(server.w), EMF_WORLD_SERVER, [](const std::string&) {});
    g_now -= 5000;
    CHECK(ctx.ok);
    CHECK(item_by_id(server.reg.reg, 0x11)->max_stack == 777);
}

namespace {
// The game's component registry as getTemplateComponent's callers see it: storage[0] -> registry (world+0x928),
// template map at registry+0x30, registry+0x08 type entries*, registry+0x10 count (as 0x1409e6570 reads them),
// entry (0x100 B) +0x30 -> reflection descriptor.
struct TemplateRegistry {
    MapStore tpl;
    alignas(8) uint8_t types[3 * 0x100] = {};
    alignas(8) uint8_t desc[0x60] = {};
    void plant(FakeWorld& w, uint32_t hash, uint32_t size, uint16_t index) {
        uint8_t* reg = w.p() + 0x928;
        w.put<const void*>(off::kStorage, reg);
        tpl.attach(w, 0x958, true);
        tpl.put(w, 0x958, hash & 7, hash);
        tpl.vals2[hash & 7] = index;
        w.put<const void*>(0x928 + 0x08, types);
        w.put<uint64_t>(0x928 + 0x10, 3);
        const void* d = desc;
        std::memcpy(types + index * 0x100 + 0x30, &d, 8);
        std::memcpy(desc + off::kTypeDescSize, &size, 4);
        std::memcpy(desc + off::kTypeDescHash, &hash, 4);
    }
};
}  // namespace

TEST_CASE("api v7: get_template_component reports the game's own size for the component (0 when unknown)") {
    Fixture f;
    f.state.get_template_component = fake_tpl;
    FakeWorld w;
    const EmfApi* api = api_table();
    uint64_t size = 99;
    CHECK(api->get_template_component(as_world(w), 0x1a8b5909u, 42, &size) == g_v7_tpl);
    CHECK(size == 0);  // no registry planted: unknown
    TemplateRegistry r;
    r.plant(w, 0x1a8b5909u, 456, 2);
    CHECK(api->get_template_component(as_world(w), 0x1a8b5909u, 42, &size) == g_v7_tpl);
    CHECK(size == 456);
    CHECK(api->get_template_component(as_world(w), 0x1a8b5909u, 43, &size) == nullptr);
    CHECK(size == 0);
    CHECK(api->get_template_component(as_world(w), 0x1a8b5909u, 42, nullptr) == g_v7_tpl);  // size is optional
}

// ---- v8: singletons and other-entity reads in mod programs ----
namespace {
uint64_t v8_register(void*, const void*, char) { return 1; }
alignas(8) uint8_t g_v8_registry[0x40];
alignas(8) uint8_t g_v8_elsewhere[56];
uint64_t g_v8_accessor = 0xACC;
struct {
    void* world_ctx = nullptr;
    uint64_t accessor = 0;
    uint32_t entity = 0;
    int calls = 0;
    bool lie = false;   // return a pointer getComponent disagrees with
    bool hide = false;  // return nothing although getComponent has it (an entity created this tick)
} g_v8_ro;
void* v8_read_other(void** out, void* world_ctx, uint64_t accessor, uint32_t entity) {
    g_v8_ro.world_ctx = world_ctx;
    g_v8_ro.accessor = accessor;
    g_v8_ro.entity = entity;
    ++g_v8_ro.calls;
    *out = nullptr;
    if (accessor == 0xACC && entity == 42 && !g_v8_ro.hide)
        *out = g_v8_ro.lie ? g_v8_elsewhere : reinterpret_cast<void*>(g_transform);
    out[1] = *out ? reinterpret_cast<void*>(uintptr_t{56}) : nullptr;  // the engine writes {ptr, size}
    return out;
}
void* g_v8_iter_singleton = g_v8_registry;
void v8_iter_begin(void* ctx, void* iter, uint32_t) {
    auto* it = static_cast<uint64_t*>(iter);
    it[0] = reinterpret_cast<uint64_t>(ctx);
    it[1] = reinterpret_cast<uint64_t>(g_v8_iter_singleton);  // the one singleton
    it[2] = g_v8_accessor;                                    // the one other-read accessor
}
struct V8Results {
    void* singleton = nullptr;
    void* undeclared_singleton = reinterpret_cast<void*>(1);
    void* transform = nullptr;
    void* wrong_size = reinterpret_cast<void*>(1);
    void* lacking = reinterpret_cast<void*>(1);
    void* undeclared = reinterpret_cast<void*>(1);
    void* again = nullptr;
} g_v8;
void v8_program(void* ctx, void*) {
    const EmfApi* api = api_table();
    g_v8.singleton = api->program_singleton(ctx, "g38_items::ItemRegistry");
    g_v8.undeclared_singleton = api->program_singleton(ctx, "g38_daytime::Daytime");
    g_v8.transform = api->program_read_other(ctx, "CurrentTransform", 42, 56);
    g_v8.wrong_size = api->program_read_other(ctx, "CurrentTransform", 42, 55);
    g_v8.lacking = api->program_read_other(ctx, "CurrentTransform", 7, 56);
    g_v8.undeclared = api->program_read_other(ctx, "Health", 42, 0);
    g_v8.again = api->program_read_other(ctx, "CurrentTransform", 42, 0);
}
struct V8World {
    FakeWorld w;
    SingletonStore s;
    V8World() { s.plant(w, "g38_items::ItemRegistry", g_v8_registry); }
};
EmfProgramDef v8_def(const char* const* singletons, const char* const* other_read) {
    EmfProgramDef d{};
    d.size = sizeof(EmfProgramDef);
    d.name = "v8prog";
    d.world_kind = EMF_WORLD_SERVER;
    d.fn = v8_program;
    d.singletons = singletons;
    d.other_read = other_read;
    return d;
}
// Registers v8prog into a world and runs it once with job+0x30 = world_ctx.
void run_v8(Fixture& f, V8World& vw, void* world_ctx) {
    static const char* singletons[] = {"g38_items::ItemRegistry", nullptr};
    static const char* other[] = {"CurrentTransform", nullptr};
    const EmfProgramDef def = v8_def(singletons, other);
    REQUIRE(api_table()->register_program_def(&def));
    REQUIRE(f.state.system_programs.register_into(vw.w.p(), v8_register) == 1);
    FakeProgramCtx ctx(vw.w.p());
    std::memcpy(ctx.job + off::kJobWorldCtx, &world_ctx, 8);
    system_program_desc(0).update(ctx.p());
}
}  // namespace

TEST_CASE("api v8: version 8 with program_singleton and program_read_other") {
    const EmfApi* api = api_table();
    CHECK(api->version >= 8);
    CHECK(api->program_singleton);
    CHECK(api->program_read_other);
}

TEST_CASE("api v8: register_program_def takes singletons and other_read; a v3-sized definition still works") {
    Fixture f;
    f.state.system_programs.reset();
    const char* singletons[] = {"g38_items::ItemRegistry", nullptr};
    const char* other[] = {"PlayerInput", "CurrentTransform", nullptr};
    const EmfProgramDef def = v8_def(singletons, other);
    REQUIRE(api_table()->register_program_def(&def));
    const ProgramDesc& d = system_program_desc(0);
    REQUIRE(d.sysRead.n == 1);
    CHECK(std::string(d.sysRead.p[0].s, d.sysRead.p[0].n) == "g38_items::ItemRegistry");
    REQUIRE(d.otherRead.n == 2);
    CHECK(std::string(d.otherRead.p[0].s, d.otherRead.p[0].n) == "CurrentTransform");
    // A mod built against v3 passes the smaller size; fields after events_out are not read.
    EmfProgramDef old = v8_def(reinterpret_cast<const char* const*>(1), reinterpret_cast<const char* const*>(1));
    old.name = "old";
    old.size = static_cast<uint32_t>(offsetof(EmfProgramDef, singletons));
    CHECK(api_table()->register_program_def(&old));
    CHECK(system_program_desc(1).sysRead.n == 0);
    CHECK(system_program_desc(1).otherRead.n == 0);
    const char* unknown[] = {"NoSuchComponent", nullptr};
    EmfProgramDef bad = v8_def(nullptr, unknown);
    bad.name = "bad";
    g_lines.clear();
    CHECK_FALSE(api_table()->register_program_def(&bad));
    CHECK(logged("register_program_def('bad') refused"));
    f.state.system_programs.reset();
}

TEST_CASE("api v8: program_singleton and program_read_other inside the program; checked once against getComponent") {
    Fixture f;
    f.state.system_programs.reset();
    f.state.sigs[static_cast<size_t>(Sig::ReadOther)] = Resolved{reinterpret_cast<uintptr_t>(&v8_read_other), 1, false};
    f.state.known_build = false;
    set_program_iter_begin(v8_iter_begin);
    V8World vw;
    g_v8 = {};
    g_v8_ro = {};
    g_lines.clear();
    int world_ctx = 0;
    run_v8(f, vw, &world_ctx);
    CHECK(g_v8.singleton == g_v8_registry);
    CHECK(g_v8.undeclared_singleton == nullptr);
    CHECK(logged("program_singleton('g38_daytime::Daytime') refused"));
    CHECK(g_v8.transform == g_transform);
    CHECK(g_v8_ro.world_ctx == &world_ctx);  // *(job + 0x30)
    CHECK(g_v8_ro.accessor == 0xACC);
    CHECK(g_v8.wrong_size == nullptr);
    CHECK(logged("the game's 'CurrentTransform' is 56 bytes, the mod expects 55"));
    CHECK(g_v8.lacking == nullptr);  // entity 7 has no CurrentTransform
    CHECK(g_v8.undeclared == nullptr);
    CHECK(logged("program_read_other('Health') refused"));
    CHECK(g_v8.again == g_transform);
    set_program_iter_begin(nullptr);
    f.state.system_programs.reset();
}

TEST_CASE("api v8: an accessor that disagrees with getComponent is refused for good in that world") {
    Fixture f;
    f.state.system_programs.reset();
    f.state.sigs[static_cast<size_t>(Sig::ReadOther)] = Resolved{reinterpret_cast<uintptr_t>(&v8_read_other), 1, false};
    f.state.known_build = false;
    set_program_iter_begin(v8_iter_begin);
    V8World vw;
    g_v8 = {};
    g_v8_ro = {};
    g_v8_ro.lie = true;
    g_lines.clear();
    int world_ctx = 0;
    run_v8(f, vw, &world_ctx);
    CHECK(g_v8.transform == nullptr);
    CHECK(g_v8.again == nullptr);
    CHECK(logged("accessor for 'CurrentTransform' does not match getComponent"));
    g_v8_ro.lie = false;
    set_program_iter_begin(nullptr);
    f.state.system_programs.reset();
}

TEST_CASE("api v8: a singleton the iterator gives differently from the world's singleton map is refused") {
    Fixture f;
    f.state.system_programs.reset();
    f.state.sigs[static_cast<size_t>(Sig::ReadOther)] = Resolved{reinterpret_cast<uintptr_t>(&v8_read_other), 1, false};
    f.state.known_build = false;
    set_program_iter_begin(v8_iter_begin);
    V8World vw;
    g_v8 = {};
    g_lines.clear();
    g_v8_iter_singleton = g_v8_elsewhere;
    int world_ctx = 0;
    run_v8(f, vw, &world_ctx);
    CHECK(g_v8.singleton == nullptr);
    CHECK(logged("does not match the world's singleton"));
    g_v8_iter_singleton = g_v8_registry;
    set_program_iter_begin(nullptr);
    f.state.system_programs.reset();
}

TEST_CASE("api v8: program_singleton and program_read_other outside a program call are refused") {
    Fixture f;
    g_lines.clear();
    int dummy = 0;
    CHECK(api_table()->program_singleton(&dummy, "g38_items::ItemRegistry") == nullptr);
    CHECK(api_table()->program_read_other(&dummy, "CurrentTransform", 42, 56) == nullptr);
    CHECK(logged("program_read_other('CurrentTransform') refused"));
    CHECK(api_table()->program_read_other(nullptr, nullptr, 0, 0) == nullptr);
}

// ---- v8: put_items ----
namespace {
void v8_put_done(EmfWorld*, uint32_t, uint32_t, uint32_t, uint32_t, void*) {}
struct PutWorld {
    FakeWorld w;
    SingletonStore s;
    FakeItemRegistry reg;
    explicit PutWorld(uint8_t kind) {
        w.put<uint8_t>(off::kKind, kind);
        reg.add(0x11, 10, 250, "Material_T1_Wood_Logs");
        s.plant(w, "g38_items::ItemRegistry", reg.reg);
    }
};
}  // namespace

TEST_CASE("api v8: put_items is in the table") {
    CHECK(api_table()->put_items);
}

TEST_CASE("api v8: put_items queues a request for the server world; bad input or no inventory feature is refused") {
    Fixture f;
    const EmfApi* api = api_table();
    PutWorld server(EMF_WORLD_SERVER), client(EMF_WORLD_CLIENT);
    g_lines.clear();
    CHECK_FALSE(api->put_items(as_world(server.w), 1, 0x11, 5, v8_put_done, nullptr));  // no service on this build
    CHECK(logged("put_items refused: the inventory transaction is not available"));

    InventoryService::Deps deps;
    deps.log = [](const std::string& s) { g_lines.push_back(s); };
    InventoryService service(std::move(deps));
    f.state.inventory = &service;
    g_lines.clear();
    CHECK(api->put_items(as_world(server.w), 1, 0x11, 5, v8_put_done, nullptr));
    CHECK(service.pending() == 1);
    CHECK_FALSE(api->put_items(as_world(client.w), 1, 0x11, 5, v8_put_done, nullptr));
    CHECK(logged("put_items refused: not a server world"));
    CHECK_FALSE(api->put_items(as_world(server.w), 1, 0x99, 5, v8_put_done, nullptr));
    CHECK(logged("put_items refused: unknown item"));
    CHECK_FALSE(api->put_items(as_world(server.w), 0, 0x11, 5, v8_put_done, nullptr));
    CHECK_FALSE(api->put_items(as_world(server.w), 1, 0x11, 0, v8_put_done, nullptr));
    CHECK_FALSE(api->put_items(nullptr, 1, 0x11, 5, v8_put_done, nullptr));
    CHECK(service.pending() == 1);
    f.state.inventory = nullptr;
}

TEST_CASE("api v8: a read where only one side has the component is inconclusive, not a mismatch") {
    Fixture f;
    f.state.system_programs.reset();
    f.state.sigs[static_cast<size_t>(Sig::ReadOther)] = Resolved{reinterpret_cast<uintptr_t>(&v8_read_other), 1, false};
    f.state.known_build = false;
    set_program_iter_begin(v8_iter_begin);
    V8World vw;
    g_v8 = {};
    g_v8_ro = {};
    g_v8_ro.hide = true;
    g_lines.clear();
    static const char* singletons[] = {"g38_items::ItemRegistry", nullptr};
    static const char* other[] = {"CurrentTransform", nullptr};
    const EmfProgramDef def = v8_def(singletons, other);
    REQUIRE(api_table()->register_program_def(&def));
    REQUIRE(f.state.system_programs.register_into(vw.w.p(), v8_register) == 1);
    FakeProgramCtx ctx(vw.w.p());
    int world_ctx = 0;
    void* wctx = &world_ctx;
    std::memcpy(ctx.job + off::kJobWorldCtx, &wctx, 8);
    system_program_desc(0).update(ctx.p());
    CHECK(g_v8.transform == nullptr);
    CHECK_FALSE(logged("does not match getComponent"));
    g_v8_ro.hide = false;  // next tick both agree: the accessor is good
    system_program_desc(0).update(ctx.p());
    CHECK(g_v8.transform == g_transform);
    set_program_iter_begin(nullptr);
    f.state.system_programs.reset();
}

TEST_CASE("api v9: server_session counts the hosted sessions the core has seen set up") {
    Fixture f;
    const EmfApi* api = api_table();
    CHECK(api->version >= 9);
    REQUIRE(api->server_session);
    CHECK(api->server_session() == 0);
    f.state.server_session = 2;  // the core adds one each time a server world is set up
    CHECK(api->server_session() == 2);
}

// ---- v10: radial wheel and notify ----
namespace {
struct {
    int calls = 0;
    WheelRequest last;
    bool accept = true;
} g_wheel_hook;
struct {
    EmfWorld* world = nullptr;
    int choice = -99;
    void* user = nullptr;
    int calls = 0;
} g_wheel_cb;
void wheel_cb(EmfWorld* w, int choice, void* user) {
    g_wheel_cb.world = w;
    g_wheel_cb.choice = choice;
    g_wheel_cb.user = user;
    ++g_wheel_cb.calls;
}
std::vector<std::string> g_notes;
}  // namespace

TEST_CASE("api v10: wheel_open validates, copies the entries and asks the overlay") {
    Fixture f;
    const EmfApi* api = api_table();
    CHECK(api->version >= 10);
    REQUIRE(api->wheel_open);
    g_wheel_hook = {};
    f.state.open_wheel = [](const WheelRequest& r) {
        ++g_wheel_hook.calls;
        g_wheel_hook.last = r;
        return g_wheel_hook.accept;
    };
    std::string a = "Mace", b = "Shield";
    const char* items[] = {a.c_str(), b.c_str()};
    int user = 0;
    EmfWheel w{};
    w.size = sizeof(EmfWheel);
    w.title = "Villager 1025";
    w.items = items;
    w.count = 2;
    w.hold_key = 0x4A;
    w.world_kind = EMF_WORLD_SERVER;
    w.cb = wheel_cb;
    w.user = &user;
    CHECK(api->wheel_open(&w));
    a = "changed after the call";  // the core keeps its own copy
    REQUIRE(g_wheel_hook.calls == 1);
    CHECK(g_wheel_hook.last.title == "Villager 1025");
    CHECK(g_wheel_hook.last.items == std::vector<std::string>{"Mace", "Shield"});
    CHECK(g_wheel_hook.last.hold_vk == 0x4A);
    CHECK(g_wheel_hook.last.world_kind == EMF_WORLD_SERVER);
    CHECK(g_wheel_hook.last.cb == wheel_cb);
    CHECK(g_wheel_hook.last.user == &user);

    g_wheel_hook.accept = false;  // the overlay refuses (console or another wheel open)
    CHECK_FALSE(api->wheel_open(&w));
    g_wheel_hook.accept = true;

    g_lines.clear();
    EmfWheel bad = w;
    bad.count = 0;
    CHECK_FALSE(api->wheel_open(&bad));
    bad = w;
    bad.count = 65;
    CHECK_FALSE(api->wheel_open(&bad));
    bad = w;
    bad.cb = nullptr;
    CHECK_FALSE(api->wheel_open(&bad));
    bad = w;
    bad.world_kind = EMF_WORLD_PREDICTION;
    CHECK_FALSE(api->wheel_open(&bad));
    bad = w;
    bad.size = 8;
    CHECK_FALSE(api->wheel_open(&bad));
    const char* holes[] = {"ok", nullptr};
    bad = w;
    bad.items = holes;
    CHECK_FALSE(api->wheel_open(&bad));
    CHECK_FALSE(api->wheel_open(nullptr));
    CHECK(logged("wheel_open refused"));
    CHECK(g_wheel_hook.calls == 2);  // none of the bad ones reached the overlay

    bad = w;
    bad.hold_key = VK_LBUTTON;  // a mouse button's release never reaches the wheel as a key
    CHECK_FALSE(api->wheel_open(&bad));
    bad.hold_key = VK_LSHIFT;  // raw input reports the generic VK_SHIFT, never VK_LSHIFT
    CHECK_FALSE(api->wheel_open(&bad));
    CHECK(logged("a keyboard key's generic virtual-key code"));
    bad.hold_key = VK_SHIFT;
    CHECK(api->wheel_open(&bad));

    f.state.open_wheel = nullptr;  // no overlay
    CHECK_FALSE(api->wheel_open(&w));
}

TEST_CASE("api v10: the wheel's choice reaches the mod after the next step of the world kind it asked for") {
    Fixture f;
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 0);
    g_wheel_cb = {};
    int user = 0;
    WheelRequest r;
    r.world_kind = EMF_WORLD_SERVER;
    r.cb = wheel_cb;
    r.user = &user;
    post_wheel_choice(r, 3);
    CHECK(g_wheel_cb.calls == 0);  // not before the step
    f.state.dispatcher.run(as_world(w), EMF_WORLD_SERVER, [](const std::string&) {});
    REQUIRE(g_wheel_cb.calls == 1);
    CHECK(g_wheel_cb.choice == 3);
    CHECK(g_wheel_cb.world == as_world(w));
    CHECK(g_wheel_cb.user == &user);
}

TEST_CASE("api v10: notify formats the line and hands it to the overlay (and the log)") {
    Fixture f;
    g_notes.clear();
    g_lines.clear();
    f.state.notify = [](const std::string& s) { g_notes.push_back(s); };
    api_table()->notify("villager %u is waiting for %s", 1025u, "wood");
    REQUIRE(g_notes.size() == 1);
    CHECK(g_notes[0] == "villager 1025 is waiting for wood");
    f.state.notify = nullptr;
    api_table()->notify("no overlay: %d", 7);
    CHECK(logged("no overlay: 7"));
}

TEST_CASE("api v11: put_items_ex and take_items validate the request and queue it on the server world") {
    Fixture f;
    const EmfApi* api = api_table();
    CHECK(api->version >= 11);
    REQUIRE(api->put_items_ex);
    REQUIRE(api->take_items);
    PutWorld server(EMF_WORLD_SERVER), client(EMF_WORLD_CLIENT);
    InventoryService::Deps deps;
    deps.fns.remove_items = reinterpret_cast<RemoveItemsFn>(0x1);  // present (never called here)
    InventoryService service(std::move(deps));
    f.state.inventory = &service;
    EmfItemsRequest r{};
    r.size = sizeof(EmfItemsRequest);
    r.entity = 1;
    r.item = 0x11;
    r.count = 1;
    r.level = 9;
    r.rarity = 2;
    r.cb = v8_put_done;
    CHECK(api->put_items_ex(as_world(server.w), &r));
    CHECK(api->take_items(as_world(server.w), &r));
    CHECK(service.pending() == 2);
    g_lines.clear();
    CHECK_FALSE(api->take_items(as_world(client.w), &r));
    CHECK(logged("not a server world"));
    EmfItemsRequest tiny = r;
    tiny.size = 8;
    CHECK_FALSE(api->put_items_ex(as_world(server.w), &tiny));
    CHECK_FALSE(api->take_items(as_world(server.w), nullptr));
    EmfItemsRequest bad = r;
    bad.flags = EMF_ITEMS_RARITY;
    bad.rarity = 7;  // 0..4
    CHECK_FALSE(api->put_items_ex(as_world(server.w), &bad));
    bad = r;
    bad.count = 0;
    CHECK_FALSE(api->take_items(as_world(server.w), &bad));
    CHECK(service.pending() == 2);
    f.state.inventory = nullptr;
}

TEST_CASE("api v11: flags: rarity only with EMF_ITEMS_RARITY, exact slot reaches the service, unknown flags refused") {
    Fixture f;
    const EmfApi* api = api_table();
    PutWorld server(EMF_WORLD_SERVER);
    InventoryService::Deps deps;
    deps.fns.remove_items = reinterpret_cast<RemoveItemsFn>(0x1);
    InventoryService service(std::move(deps));
    f.state.inventory = &service;
    EmfItemsRequest r{};  // zero-initialised: every default
    r.size = sizeof(EmfItemsRequest);
    r.entity = 1;
    r.item = 0x11;
    r.count = 1;
    REQUIRE(api->put_items_ex(as_world(server.w), &r));
    CHECK(service.pending_at(0).rarity == 0xFF);  // rarity 0 without the flag: the game's default, not Common
    r.flags = EMF_ITEMS_RARITY;
    r.rarity = 3;
    REQUIRE(api->put_items_ex(as_world(server.w), &r));
    CHECK(service.pending_at(1).rarity == 3);
    r.rarity = 7;
    CHECK_FALSE(api->put_items_ex(as_world(server.w), &r));
    r = EmfItemsRequest{};
    r.size = sizeof(EmfItemsRequest);
    r.entity = 1;
    r.item = 0x11;
    r.count = 1;
    r.slot = (uint64_t(2) << 32) | 500;
    r.flags = EMF_ITEMS_EXACT_SLOT;
    REQUIRE(api->take_items(as_world(server.w), &r));
    CHECK(service.pending_at(2).exact_slot);
    CHECK(service.pending_at(2).slot == r.slot);
    r.flags = 0x80;  // a flag this core does not know
    CHECK_FALSE(api->take_items(as_world(server.w), &r));
    CHECK(service.pending() == 3);
    f.state.inventory = nullptr;
}

TEST_CASE("api sizes: wheel_open (v10) and put_items_ex / take_items (v11) take their version's size; less is refused") {
    Fixture f;
    const EmfApi* api = api_table();
    f.state.open_wheel = [](const WheelRequest&) { return true; };
    const char* items[] = {"a"};
    EmfWheel w{};
    w.size = kWheelV10Size;
    w.items = items;
    w.count = 1;
    w.world_kind = EMF_WORLD_SERVER;
    w.cb = wheel_cb;
    CHECK(api->wheel_open(&w));
    w.size = kWheelV10Size - 1;
    CHECK_FALSE(api->wheel_open(&w));

    PutWorld server(EMF_WORLD_SERVER);
    InventoryService::Deps deps;
    deps.fns.remove_items = reinterpret_cast<RemoveItemsFn>(0x1);
    InventoryService service(std::move(deps));
    f.state.inventory = &service;
    EmfItemsRequest r{};
    r.size = kItemsRequestV11Size;
    r.entity = 1;
    r.item = 0x11;
    r.count = 1;
    CHECK(api->put_items_ex(as_world(server.w), &r));
    CHECK(api->take_items(as_world(server.w), &r));
    r.size = kItemsRequestV11Size - 1;
    CHECK_FALSE(api->put_items_ex(as_world(server.w), &r));
    CHECK_FALSE(api->take_items(as_world(server.w), &r));
    CHECK(service.pending() == 2);
    f.state.inventory = nullptr;
}

TEST_CASE("api: unconfirmed tick fields give 0 and are reported once per core (not once per process)") {
    for (int round = 0; round < 2; ++round) {
        Fixture f;
        g_lines.clear();
        f.state.tick_offsets_ok = false;
        FakeWorld server;
        server.put<uint8_t>(off::kKind, 0);
        server.put<uint32_t>(off::kTick, 600);
        const EmfApi* api = api_table();
        CHECK(api->world_tick(as_world(server)) == 0);
        CHECK(api->ecs_time_ns(as_world(server)) == 0);
        size_t said = 0;
        for (const auto& l : g_lines) said += l.find("world_tick / ecs_time_ns give 0") != std::string::npos;
        CHECK(said == 1);
    }
}

TEST_CASE("api: repeated put_items refusals are logged at most every 10 s") {
    Fixture f;
    PutWorld client(EMF_WORLD_CLIENT);
    InventoryService service(InventoryService::Deps{});
    f.state.inventory = &service;
    const EmfApi* api = api_table();
    g_lines.clear();
    auto said = [] {
        size_t n = 0;
        for (const auto& l : g_lines) n += l.find("not a server world") != std::string::npos;
        return n;
    };
    CHECK_FALSE(api->put_items(as_world(client.w), 1, 0x11, 1, nullptr, nullptr));
    CHECK_FALSE(api->put_items(as_world(client.w), 1, 0x11, 1, nullptr, nullptr));
    CHECK(said() == 1);
    g_now += 10001;
    CHECK_FALSE(api->put_items(as_world(client.w), 1, 0x11, 1, nullptr, nullptr));
    CHECK(said() == 2);
    f.state.inventory = nullptr;
}

TEST_CASE("api v12: hud_set keeps a line on screen by id; bad input is refused") {
    Fixture f;
    const EmfApi* api = api_table();
    CHECK(api->version >= 12);
    REQUIRE(api->hud_set);
    CHECK(api->hud_set("clock.time", "14:05", EMF_HUD_TOP_RIGHT, 0));
    REQUIRE(f.state.hud.lines().size() == 1);
    CHECK(f.state.hud.lines()[0].text == "14:05");
    CHECK(api->hud_set("clock.time", nullptr, EMF_HUD_TOP_RIGHT, 0));  // null text removes it
    CHECK(f.state.hud.empty());
    g_lines.clear();
    CHECK_FALSE(api->hud_set("bad id", "x", EMF_HUD_TOP_RIGHT, 0));
    CHECK_FALSE(api->hud_set("ok.id", "x", 7, 0));
    CHECK_FALSE(api->hud_set(nullptr, "x", 0, 0));
    CHECK(logged("hud_set refused"));
}

// ---- v16: other-entity writes that reach the players and the save ----
namespace {
struct {
    void* storage = nullptr;
    void* change_set = nullptr;
    uint64_t accessor = 0;
    uint32_t entity = 0;
    uint8_t variant = 0;
    int calls = 0;
} g_v16_wo;
void* v16_write_other(void** out, void* storage, void* change_set, uint64_t accessor, uint32_t entity, uint8_t variant) {
    g_v16_wo.storage = storage;
    g_v16_wo.change_set = change_set;
    g_v16_wo.accessor = accessor;
    g_v16_wo.entity = entity;
    g_v16_wo.variant = variant;
    ++g_v16_wo.calls;
    *out = (accessor == 0xACE && entity == 42) ? reinterpret_cast<void*>(g_transform) : nullptr;
    out[1] = *out ? reinterpret_cast<void*>(uintptr_t{56}) : nullptr;  // {ptr, size}, 16 bytes like the engine
    return out;
}
// readOther for the write accessor: the side-effect-free first check (lie: a pointer getComponent disagrees with)
bool g_v16_read_lie = false;
int g_v16_reads = 0;
uint32_t g_v16_read_only = 0;  // set: readOther also gives a component for this entity (getComponent does not)
void* v16_read_other(void** out, void*, uint64_t accessor, uint32_t entity) {
    ++g_v16_reads;
    *out = nullptr;
    if ((accessor == 0xACE || accessor == 0xACC) && (entity == 42 || (g_v16_read_only && entity == g_v16_read_only)))
        *out = g_v16_read_lie ? g_v8_elsewhere : reinterpret_cast<void*>(g_transform);
    out[1] = *out ? reinterpret_cast<void*>(uintptr_t{56}) : nullptr;
    return out;
}
void v16_iter_begin(void* ctx, void* iter, uint32_t) {
    auto* it = static_cast<uint64_t*>(iter);
    it[0] = reinterpret_cast<uint64_t>(ctx);
    it[1] = 0xACC;  // other-read accessor (Velocity)
    it[2] = 0xACE;  // other-write accessor (CurrentTransform)
}
struct {
    void* written = nullptr;
    void* wrong_size = reinterpret_cast<void*>(1);
    void* lacking = reinterpret_cast<void*>(1);
    void* undeclared = reinterpret_cast<void*>(1);
    void* read_only = reinterpret_cast<void*>(1);
} g_v16;
void v16_program(void* ctx, void*) {
    const EmfApi* api = api_table();
    g_v16.written = api->program_write_other(ctx, "CurrentTransform", 42, 56);
    g_v16.wrong_size = api->program_write_other(ctx, "CurrentTransform", 42, 55);
    g_v16.lacking = api->program_write_other(ctx, "CurrentTransform", 7, 56);
    g_v16.undeclared = api->program_write_other(ctx, "Health", 42, 0);
    g_v16.read_only = api->program_write_other(ctx, "Velocity", 42, 0);  // declared for reading only
}
}  // namespace

TEST_CASE("api v16: program_write_other gives the component through the game's writeOther (marked for players and save)") {
    Fixture f;
    f.state.system_programs.reset();
    f.state.sigs[static_cast<size_t>(Sig::WriteOther)] = Resolved{reinterpret_cast<uintptr_t>(&v16_write_other), 1, false};
    f.state.sigs[static_cast<size_t>(Sig::ReadOther)] = Resolved{reinterpret_cast<uintptr_t>(&v16_read_other), 1, false};
    f.state.known_build = false;
    g_v16_read_lie = false;
    set_program_iter_begin(v16_iter_begin);
    REQUIRE(api_table()->version >= 16u);
    static const char* other_read[] = {"Velocity", nullptr};
    static const char* other_write[] = {"CurrentTransform", nullptr};
    EmfProgramDef def{};
    def.size = sizeof def;
    def.name = "v16prog";
    def.world_kind = EMF_WORLD_SERVER;
    def.fn = v16_program;
    def.other_read = other_read;
    def.other_write = other_write;
    REQUIRE(api_table()->register_program_def(&def));
    REQUIRE(system_program_desc(0).otherWrite.n == 1);
    V8World vw;
    REQUIRE(f.state.system_programs.register_into(vw.w.p(), v8_register) == 1);
    FakeProgramCtx ctx(vw.w.p());
    int storage = 0, change_set = 0;
    void* sp = &storage;
    void* cp = &change_set;
    const uint8_t variant = 3;
    std::memcpy(ctx.job + 0x30, &sp, 8);
    std::memcpy(ctx.job + 0x1C0, &cp, 8);
    std::memcpy(ctx.job + 0xBB, &variant, 1);
    g_v16 = {};
    g_v16_wo = {};
    g_lines.clear();
    system_program_desc(0).update(ctx.p());
    CHECK(g_v16.written == g_transform);
    CHECK(g_v16_wo.storage == &storage);        // *(job + 0x30)
    CHECK(g_v16_wo.change_set == &change_set);  // *(job + 0x1C0)
    CHECK(g_v16_wo.variant == 3);               // *(u8*)(job + 0xBB)
    CHECK(g_v16_wo.accessor == 0xACE);          // the write accessor comes after the read one
    CHECK(g_v16.wrong_size == nullptr);
    CHECK(logged("the game's 'CurrentTransform' is 56 bytes, the mod expects 55"));
    CHECK(g_v16.lacking == nullptr);
    CHECK(g_v16.undeclared == nullptr);
    CHECK(logged("program_write_other('Health') refused"));
    CHECK(g_v16.read_only == nullptr);
    CHECK(logged("program_write_other('Velocity') refused"));
    set_program_iter_begin(nullptr);
    f.state.system_programs.reset();

    emf::api_ref() = api_table();
    CHECK(emf::has_v16());
    EmfApi old = *api_table();
    old.size = static_cast<uint32_t>(offsetof(EmfApi, program_write_other));
    emf::api_ref() = &old;
    CHECK_FALSE(emf::has_v16());
    emf::api_ref() = nullptr;
}

TEST_CASE("api v16: a v8-sized program definition has no other_write; writes outside a program are refused") {
    Fixture f;
    f.state.system_programs.reset();
    static const char* other[] = {"CurrentTransform", nullptr};
    EmfProgramDef def{};
    def.size = static_cast<uint32_t>(offsetof(EmfProgramDef, other_write));
    def.name = "v8sized";
    def.world_kind = EMF_WORLD_SERVER;
    def.fn = v16_program;
    def.other_read = other;
    def.other_write = reinterpret_cast<const char* const*>(1);  // not read: beyond the definition's size
    REQUIRE(api_table()->register_program_def(&def));
    CHECK(system_program_desc(0).otherWrite.n == 0);
    int dummy = 0;
    CHECK(api_table()->program_write_other(&dummy, "CurrentTransform", 42, 56) == nullptr);
    CHECK(api_table()->program_write_other(nullptr, nullptr, 0, 0) == nullptr);
    f.state.system_programs.reset();
}

namespace {
void v16_iter_begin_empty(void* ctx, void* iter, uint32_t) {
    auto* it = static_cast<uint64_t*>(iter);
    it[0] = reinterpret_cast<uint64_t>(ctx);
    it[1] = 0xACC;
    it[2] = 0;  // the engine gave no write accessor
}
}  // namespace

TEST_CASE("api v16: a write with no accessor in its iterator slot is refused and says so") {
    Fixture f;
    f.state.system_programs.reset();
    f.state.sigs[static_cast<size_t>(Sig::WriteOther)] = Resolved{reinterpret_cast<uintptr_t>(&v16_write_other), 1, false};
    f.state.sigs[static_cast<size_t>(Sig::ReadOther)] = Resolved{reinterpret_cast<uintptr_t>(&v16_read_other), 1, false};
    f.state.known_build = false;
    set_program_iter_begin(v16_iter_begin_empty);
    static const char* other_read[] = {"Velocity", nullptr};
    static const char* other_write[] = {"CurrentTransform", nullptr};
    EmfProgramDef def{};
    def.size = sizeof def;
    def.name = "v16empty";
    def.world_kind = EMF_WORLD_SERVER;
    def.fn = v16_program;
    def.other_read = other_read;
    def.other_write = other_write;
    REQUIRE(api_table()->register_program_def(&def));
    V8World vw;
    REQUIRE(f.state.system_programs.register_into(vw.w.p(), v8_register) == 1);
    FakeProgramCtx ctx(vw.w.p());
    g_v16 = {};
    g_v16_wo = {};
    g_lines.clear();
    system_program_desc(0).update(ctx.p());
    CHECK(g_v16.written == nullptr);
    CHECK(g_v16_wo.calls == 0);  // the game's function is not called without an accessor
    CHECK(logged("has no write accessor in slot 2"));
    set_program_iter_begin(nullptr);
    f.state.system_programs.reset();
}

namespace {
void* g_both_written = reinterpret_cast<void*>(1);
void v16_both_program(void* ctx, void*) {
    g_both_written = api_table()->program_write_other(ctx, "CurrentTransform", 42, 56);
}
void v16_iter_begin_both(void* ctx, void* iter, uint32_t) {
    auto* it = static_cast<uint64_t*>(iter);
    it[0] = reinterpret_cast<uint64_t>(ctx);
    it[1] = 0xACC;  // other-read accessor (CurrentTransform)
    it[2] = 0xACE;  // other-write accessor (CurrentTransform)
}
void run_v16(Fixture& f, const char* name, EmfProgramCallback fn, const char* const* other_read,
             const char* const* other_write) {
    EmfProgramDef def{};
    def.size = sizeof def;
    def.name = name;
    def.world_kind = EMF_WORLD_SERVER;
    def.fn = fn;
    def.other_read = other_read;
    def.other_write = other_write;
    REQUIRE(api_table()->register_program_def(&def));
    V8World vw;
    REQUIRE(f.state.system_programs.register_into(vw.w.p(), v8_register) == 1);
    FakeProgramCtx ctx(vw.w.p());
    int storage = 0, change_set = 0;
    void* sp = &storage;
    void* cp = &change_set;
    std::memcpy(ctx.job + 0x30, &sp, 8);
    std::memcpy(ctx.job + 0x1C0, &cp, 8);
    system_program_desc(0).update(ctx.p());
}
}  // namespace

TEST_CASE("api v16: a component in both other_read and other_write is written through its write accessor") {
    Fixture f;
    f.state.system_programs.reset();
    f.state.sigs[static_cast<size_t>(Sig::WriteOther)] = Resolved{reinterpret_cast<uintptr_t>(&v16_write_other), 1, false};
    f.state.sigs[static_cast<size_t>(Sig::ReadOther)] = Resolved{reinterpret_cast<uintptr_t>(&v16_read_other), 1, false};
    f.state.known_build = false;
    g_v16_read_lie = false;
    set_program_iter_begin(v16_iter_begin_both);
    static const char* both[] = {"CurrentTransform", nullptr};
    g_v16_wo = {};
    g_lines.clear();
    run_v16(f, "v16both", v16_both_program, both, both);
    CHECK(g_both_written == g_transform);
    CHECK(g_v16_wo.accessor == 0xACE);  // the write slot, not the read slot
    set_program_iter_begin(nullptr);
    f.state.system_programs.reset();
}

TEST_CASE("api v16: the first write is checked through readOther; an accessor that disagrees is never written through") {
    Fixture f;
    f.state.system_programs.reset();
    f.state.sigs[static_cast<size_t>(Sig::WriteOther)] = Resolved{reinterpret_cast<uintptr_t>(&v16_write_other), 1, false};
    f.state.sigs[static_cast<size_t>(Sig::ReadOther)] = Resolved{reinterpret_cast<uintptr_t>(&v16_read_other), 1, false};
    f.state.known_build = false;
    g_v16_read_lie = true;  // the write accessor points somewhere getComponent does not
    set_program_iter_begin(v16_iter_begin);
    static const char* other_read[] = {"Velocity", nullptr};
    static const char* other_write[] = {"CurrentTransform", nullptr};
    g_v16 = {};
    g_v16_wo = {};
    g_v16_reads = 0;
    g_lines.clear();
    run_v16(f, "v16lie", v16_program, other_read, other_write);
    CHECK(g_v16.written == nullptr);
    CHECK(g_v16_reads >= 1);
    CHECK(g_v16_wo.calls == 0);  // nothing was marked changed through a doubtful accessor
    CHECK(logged("does not match getComponent"));
    g_v16_read_lie = false;
    set_program_iter_begin(nullptr);
    f.state.system_programs.reset();
}

TEST_CASE("api v16: a job without a change set is refused before the game's writeOther runs") {
    Fixture f;
    f.state.system_programs.reset();
    f.state.sigs[static_cast<size_t>(Sig::WriteOther)] = Resolved{reinterpret_cast<uintptr_t>(&v16_write_other), 1, false};
    f.state.sigs[static_cast<size_t>(Sig::ReadOther)] = Resolved{reinterpret_cast<uintptr_t>(&v16_read_other), 1, false};
    f.state.known_build = false;
    g_v16_read_lie = false;
    set_program_iter_begin(v16_iter_begin);
    static const char* other_read[] = {"Velocity", nullptr};
    static const char* other_write[] = {"CurrentTransform", nullptr};
    EmfProgramDef def{};
    def.size = sizeof def;
    def.name = "v16nochange";
    def.world_kind = EMF_WORLD_SERVER;
    def.fn = v16_program;
    def.other_read = other_read;
    def.other_write = other_write;
    REQUIRE(api_table()->register_program_def(&def));
    V8World vw;
    REQUIRE(f.state.system_programs.register_into(vw.w.p(), v8_register) == 1);
    FakeProgramCtx ctx(vw.w.p());
    int storage = 0;
    void* sp = &storage;
    std::memcpy(ctx.job + 0x30, &sp, 8);  // storage, but the change set at +0x1C0 stays NULL
    g_v16 = {};
    g_v16_wo = {};
    g_lines.clear();
    system_program_desc(0).update(ctx.p());
    CHECK(g_v16.written == nullptr);
    CHECK(g_v16_wo.calls == 0);
    CHECK(logged("no storage or change set"));
    set_program_iter_begin(nullptr);
    f.state.system_programs.reset();
}

namespace {
struct {
    void* no_read = reinterpret_cast<void*>(1);
    void* no_component = reinterpret_cast<void*>(1);
    void* agreed = nullptr;
} g_v16_inc;
void v16_inconclusive_program(void* ctx, void*) {
    const EmfApi* api = api_table();
    g_v16_inc.no_read = api->program_write_other(ctx, "CurrentTransform", 7, 56);        // readOther gives NULL
    g_v16_inc.no_component = api->program_write_other(ctx, "CurrentTransform", 43, 56);  // getComponent gives NULL
    g_v16_inc.agreed = api->program_write_other(ctx, "CurrentTransform", 42, 56);        // both: checked, written
}
}  // namespace

TEST_CASE("api v16: an inconclusive first check (readOther or getComponent gives NULL) writes nothing, checks again") {
    Fixture f;
    f.state.system_programs.reset();
    f.state.sigs[static_cast<size_t>(Sig::WriteOther)] = Resolved{reinterpret_cast<uintptr_t>(&v16_write_other), 1, false};
    f.state.sigs[static_cast<size_t>(Sig::ReadOther)] = Resolved{reinterpret_cast<uintptr_t>(&v16_read_other), 1, false};
    f.state.known_build = false;
    g_v16_read_lie = false;
    g_v16_read_only = 43;
    set_program_iter_begin(v16_iter_begin);
    static const char* other_read[] = {"Velocity", nullptr};
    static const char* other_write[] = {"CurrentTransform", nullptr};
    g_v16_inc = {};
    g_v16_wo = {};
    g_lines.clear();
    run_v16(f, "v16inconclusive", v16_inconclusive_program, other_read, other_write);
    CHECK(g_v16_inc.no_read == nullptr);
    CHECK(g_v16_inc.no_component == nullptr);
    CHECK(logged("inconclusive check for entity 7"));
    CHECK(logged("inconclusive check for entity 43"));
    CHECK(g_v16_inc.agreed == g_transform);  // the accessor was still unchecked, so entity 42 checked it
    CHECK(g_v16_wo.calls == 1);              // the game's writeOther (marks changed) ran only after the check
    CHECK(g_v16_wo.entity == 42);
    g_v16_read_only = 0;
    set_program_iter_begin(nullptr);
    f.state.system_programs.reset();
}

TEST_CASE("api: a refusal logged once is logged again in a later hosted session (a new world)") {
    Fixture f;
    const EmfApi* api = api_table();
    int dummy = 0;
    g_lines.clear();
    auto said = [] {
        size_t n = 0;
        for (const auto& l : g_lines) n += l.find("program_write_other('SessionProbe') refused") != std::string::npos;
        return n;
    };
    CHECK(api->program_write_other(&dummy, "SessionProbe", 42, 56) == nullptr);  // outside a program
    CHECK(api->program_write_other(&dummy, "SessionProbe", 42, 56) == nullptr);
    CHECK(said() == 1);  // once in a session (per-tick mistakes would flood the log)
    ++f.state.server_session;  // a new server world was set up
    CHECK(api->program_write_other(&dummy, "SessionProbe", 42, 56) == nullptr);
    CHECK(said() == 2);
    CHECK(api->program_write_other(&dummy, "SessionProbe", 42, 56) == nullptr);
    CHECK(said() == 2);
}
