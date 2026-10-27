// API v20 teleport through the C table (api_teleport -> TeleportService), the SDK wrapper (emf/teleport.hpp) and the
// core's goto command, with a fake QueuePush and a fake game that moves an entity the step after the push.
#include <windows.h>

#include <doctest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common/offsets.h"
#include "core/api.h"
#include "core/api_sizes.h"
#include "core/builtin_commands.h"
#include "core/commands.h"
#include "core/system_programs.h"
#include "core/teleport.h"
#include "emf/generated/components.hpp"
#include "emf/teleport.hpp"
#include "fake_world.h"

using namespace emf;
using namespace emf::core;
using emf::test::FakeProgramCtx;
using emf::test::FakeWorld;
namespace ecs = emf::gen::ecs;

namespace {
struct Body {
    ecs::CurrentTransform ct{};
    ecs::LastTransform lt{};
    ecs::TeleportCount tc{};
    ecs::Teleport tp{};
    ecs::PlayerState ps{};
    ecs::PlayerSpawnPoint sp{};
    ecs::SelectedTargets st{};
    bool has_tp = false, player = false, spawn = false, has_tc = true, has_st = false;
};
std::map<uint32_t, Body> g_bodies;
std::vector<std::string> g_lines;

void put(uint32_t e, double x, double y, double z) {
    Body& b = g_bodies[e];
    b.ct.transform.position = {to_fixed(x), to_fixed(y), to_fixed(z)};
    b.ct.transform.scale = {1, 1, 1};
    b.lt.transform = b.ct.transform;
}
void* gc(void* out, void*, uint32_t hash, uint32_t e) {
    auto* o = static_cast<uint64_t*>(out);
    o[0] = o[1] = 0;
    auto it = g_bodies.find(e);
    if (it == g_bodies.end()) return out;
    Body& b = it->second;
    auto give = [&](bool has, const void* p, size_t n) {
        if (has) {
            o[0] = reinterpret_cast<uint64_t>(p);
            o[1] = n;
        }
    };
    if (hash == ecs::CurrentTransform::kHash) give(true, &b.ct, sizeof b.ct);
    if (hash == ecs::LastTransform::kHash) give(!b.spawn, &b.lt, sizeof b.lt);
    if (hash == ecs::TeleportCount::kHash) give(!b.spawn && b.has_tc, &b.tc, sizeof b.tc);
    if (hash == ecs::Teleport::kHash) give(b.has_tp, &b.tp, sizeof b.tp);
    if (hash == ecs::PlayerState::kHash) give(b.player, &b.ps, sizeof b.ps);
    if (hash == ecs::PlayerSpawnPoint::kHash) give(b.spawn, &b.sp, sizeof b.sp);
    if (hash == ecs::SelectedTargets::kHash) give(b.has_st, &b.st, sizeof b.st);
    return out;
}

alignas(16) uint8_t g_recs[8192];
size_t g_used = 0;
struct Rec {
    uint8_t kind;
    uint32_t entity;
    uint8_t* p;
};
std::vector<Rec> g_recs_pushed;
uint8_t g_cmdbuf[0x40];
uint8_t* qpush(void*, void*, uint8_t kind, uint32_t size, uint32_t entity) {
    uint8_t* r = g_recs + g_used;
    g_used += (size + 15) & ~size_t(15);
    std::memset(r, 0, size);
    g_recs_pushed.push_back({kind, entity, r});
    return r;
}
uint64_t reg(void*, const void*, char) { return 1; }

struct Fixture {
    CoreState state;
    FakeWorld w;
    SystemPrograms programs;
    TeleportService service;
    FakeProgramCtx ctx;
    uint32_t tick = 500;
    Fixture() : service(deps()), ctx(w.p()) {
        g_bodies.clear();
        g_lines.clear();
        g_recs_pushed.clear();
        g_used = 0;
        programs.reset();
        state.get_component = gc;
        state.log_line = [](const std::string& s) { g_lines.push_back(s); };
        state.now_ms = [] { return uint64_t(1000); };
        state.teleport = &service;
        set_core_state(&state);
        std::memset(g_cmdbuf, 0, sizeof g_cmdbuf);
        const uint64_t cap = 1u << 20;
        std::memcpy(g_cmdbuf + kCmdBufCapacity, &cap, 8);
        uint8_t* cb = g_cmdbuf;
        std::memcpy(ctx.job + kJobCmdBuf, &cb, 8);
        w.put<uint8_t>(off::kKind, EMF_WORLD_SERVER);
        w.put<uint32_t>(off::kTick, tick);
        REQUIRE(programs.add(service.spec(), true));
        REQUIRE(programs.register_into(w.p(), reg) == 1);
        put(1, 4000, 700, 1200);
        g_bodies[1].player = true;
    }
    ~Fixture() {
        set_core_state(nullptr);
        programs.reset();
    }
    TeleportService::Deps deps() {
        TeleportService::Deps d;
        d.queue_push = qpush;
        d.get_component = gc;
        d.health = [](uint8_t*, uint32_t e, int32_t* cur) {  // players are alive (the health attribute)
            *cur = 100;
            return e >= 1 && e <= 1023;
        };
        d.log = [](const std::string& s) { g_lines.push_back(s); };
        d.now_ms = [] { return uint64_t(1000); };
        return d;
    }
    EmfWorld* world() { return reinterpret_cast<EmfWorld*>(w.p()); }
    // One server step: the game moves what has a Teleport, emf_teleport pushes, the flush, then the post-step work and
    // the posted callbacks (commands) on the server world.
    void step() {
        for (auto& [id, b] : g_bodies) {
            if (!b.has_tp) continue;
            if (b.tp.applied) {
                b.has_tp = false;
                continue;
            }
            emf::gen::WorldTransform t = b.tp.targetTransform;
            if (b.tp.targetEntityId.id && g_bodies[b.tp.targetEntityId.id].spawn) {
                t = g_bodies[b.tp.targetEntityId.id].ct.transform;
                t.position.z += to_fixed(2.0);  // its pad
            }
            b.ct.transform = t;
            b.tc.counter = static_cast<uint8_t>(b.tc.counter + 1);
            b.tp.applied = true;
        }
        system_program_desc(0).update(ctx.p());
        for (const auto& r : g_recs_pushed)
            if (r.kind == kRecAddComponents && !g_bodies[r.entity].has_tp) {
                g_bodies[r.entity].has_tp = true;
                std::memcpy(&g_bodies[r.entity].tp, r.p + kRecData, sizeof(ecs::Teleport));
            }
        g_recs_pushed.clear();
        g_used = 0;
        w.put<uint32_t>(off::kTick, ++tick);
        state.dispatcher.run(world(), EMF_WORLD_SERVER, [](const std::string&) {});
        service.on_server_tick(world());
    }
    template <class F>
    void in_callback(F f) {
        auto* p = new F(std::move(f));
        api_table()->run_on_world(EMF_WORLD_SERVER, [](EmfWorld* wd, void* u) {
            auto* fn = static_cast<F*>(u);
            (*fn)(wd);
            delete fn;
        }, p);
        step();
    }
};

struct Seen {
    std::vector<EmfTeleportResult> results;
    std::vector<std::string> reasons;
};
void on_result(EmfWorld*, const EmfTeleportResult* r, void* user) {
    auto* s = static_cast<Seen*>(user);
    s->results.push_back(*r);
    s->reasons.push_back(r->reason ? r->reason : "(null)");
}
EmfTeleport request(uint32_t entity, double x, double y, double z, Seen* seen, uint32_t flags = 0) {
    EmfTeleport t{};
    t.size = sizeof t;
    t.entity = entity;
    t.x = x;
    t.y = y;
    t.z = z;
    t.flags = flags;
    t.cb = seen ? on_result : nullptr;
    t.user = seen;
    return t;
}
bool logged(const std::string& needle) {
    for (const auto& l : g_lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

TEST_CASE("api v20: the table has teleport after v19's request_save; the flags are the core's") {
    const EmfApi* api = api_table();
    CHECK(api->version == 20u);
    CHECK(EMF_API_VERSION == 20);
    CHECK(api->teleport != nullptr);
    CHECK(offsetof(EmfApi, teleport) == kApiV19Size);
    CHECK(kApiV20Size == sizeof(EmfApi));
    CHECK(kTeleportV20Size == sizeof(EmfTeleport));
    CHECK(EMF_TELEPORT_FIND_GROUND == kTpFindGround);
    CHECK(EMF_TELEPORT_NOTIFY_READY == kTpNotifyReady);
    CHECK(EMF_TELEPORT_LAST != EMF_TELEPORT_OFF_TARGET);
    CHECK(EMF_TELEPORT_LAST != EMF_TELEPORT_RAW_TARGET);
}

TEST_CASE("api teleport: off on this build (no service) or a too small request: false, logged once") {
    CoreState state;
    state.log_line = [](const std::string& s) { g_lines.push_back(s); };
    state.teleport_off = "the QueuePush signature is not usable on this game build";
    g_lines.clear();
    set_core_state(&state);
    FakeWorld w;
    EmfTeleport t = request(1, 4000, 700, 1200, nullptr);
    CHECK_FALSE(api_table()->teleport(reinterpret_cast<EmfWorld*>(w.p()), &t));
    CHECK_FALSE(api_table()->teleport(reinterpret_cast<EmfWorld*>(w.p()), &t));
    size_t n = 0;
    for (const auto& l : g_lines) n += l.find("teleport is off: the QueuePush signature") != std::string::npos;
    CHECK(n == 1);
    t.size = 8;
    CHECK_FALSE(api_table()->teleport(reinterpret_cast<EmfWorld*>(w.p()), &t));
    CHECK_FALSE(api_table()->teleport(reinterpret_cast<EmfWorld*>(w.p()), nullptr));
    set_core_state(nullptr);
}

TEST_CASE("api teleport: from another thread, refused and moved results reach the C callback with the final position") {
    Fixture f;
    Seen seen;
    EmfTeleport t = request(1, 4010, 702, 1200, &seen);
    REQUIRE(api_table()->teleport(f.world(), &t));
    f.step();  // checked
    f.step();  // pushed
    CHECK(seen.results.empty());
    f.step();  // moved
    REQUIRE(seen.results.size() == 1);
    const EmfTeleportResult& r = seen.results[0];
    CHECK(r.size == sizeof(EmfTeleportResult));
    CHECK(r.entity == 1u);
    CHECK(r.status == EMF_TELEPORT_MOVED);
    CHECK(r.x == doctest::Approx(4010));
    CHECK(r.y == doctest::Approx(702));
    CHECK(r.flags == EMF_TELEPORT_LAST);
    CHECK(seen.reasons[0].empty());
    // a client world: false at once
    f.w.put<uint8_t>(off::kKind, EMF_WORLD_CLIENT);
    CHECK_FALSE(api_table()->teleport(f.world(), &t));
    f.w.put<uint8_t>(off::kKind, EMF_WORLD_SERVER);
    // an entity without TeleportCount, asked from another thread: FAILED in the callback
    put(9, 4000, 700, 1200);
    g_bodies[9].has_tc = false;
    EmfTeleport bad = request(9, 4010, 700, 1200, &seen);
    REQUIRE(api_table()->teleport(f.world(), &bad));
    f.step();
    REQUIRE(seen.results.size() == 2);
    CHECK(seen.results[1].status == EMF_TELEPORT_FAILED);
    CHECK(seen.results[1].flags == EMF_TELEPORT_LAST);
    CHECK(seen.reasons[1].find("TeleportCount") != std::string::npos);
}

TEST_CASE("api teleport: inside a server world callback the entity is checked at once; NOTIFY_READY marks the READY as last") {
    Fixture f;
    Seen seen;
    put(9, 4000, 700, 1200);
    g_bodies[9].has_tc = false;
    bool refused = false, queued = false;
    f.in_callback([&](EmfWorld* w) {
        EmfTeleport bad = request(9, 4010, 700, 1200, &seen);
        refused = !api_table()->teleport(w, &bad);
        EmfTeleport ok = request(1030, 4006, 700, 1200, &seen, EMF_TELEPORT_NOTIFY_READY);
        queued = api_table()->teleport(w, &ok);
    });
    CHECK(refused);
    CHECK_FALSE(queued);  // 1030 does not exist yet: refused at once too
    put(1030, 4005, 700, 1200);
    f.in_callback([&](EmfWorld* w) {
        EmfTeleport ok = request(1030, 4006, 700, 1200, &seen, EMF_TELEPORT_NOTIFY_READY);
        queued = api_table()->teleport(w, &ok);
    });
    REQUIRE(queued);
    f.step();  // pushed
    CHECK(seen.results.empty());
    f.step();  // moved: MOVED, then READY at once (not a player)
    REQUIRE(seen.results.size() == 2);
    CHECK(seen.results[0].status == EMF_TELEPORT_MOVED);
    CHECK((seen.results[0].flags & EMF_TELEPORT_LAST) == 0u);  // a READY follows
    CHECK(seen.results[1].status == EMF_TELEPORT_READY);
    CHECK((seen.results[1].flags & EMF_TELEPORT_LAST) != 0u);
    CHECK(logged("cannot be teleported"));
}

TEST_CASE("sdk teleport: emf::teleport frees its functor on the last call; spawn points come from SelectedTargets slot 9") {
    Fixture f;
    REQUIRE(emf::init(api_table()));
    put(1025, 3900, 690, 1150);
    g_bodies[1025].spawn = true;
    g_bodies[1025].sp.target = ecs::PlayerSpawnType::Base;
    put(1100, 3950, 690, 1150);
    g_bodies[1100].spawn = true;
    g_bodies[1100].sp.target = ecs::PlayerSpawnType::NoRespawn;  // never a respawn point
    emf::World w(f.world());
    CHECK(emf::player_spawn_point(w, 1) == 0u);  // no SelectedTargets
    g_bodies[1].has_st = true;
    g_bodies[1].st.targets[9].targetEntityId.id = 1025;
    CHECK(emf::player_spawn_point(w, 1) == 1025u);
    g_bodies[1].st.targets[9].targetEntityId.id = 1100;
    CHECK(emf::player_spawn_point(w, 1) == 0u);
    g_bodies[1].st.targets[9].targetEntityId.id = 4242;  // gone
    CHECK(emf::player_spawn_point(w, 1) == 0u);
    CHECK(emf::player_spawn_point(w, 2000) == 0u);  // not a player
    // (spawn_points_near walks the world's entity map with is_spawn_point as its filter)
    CHECK(emf::is_spawn_point(w, 1025));
    CHECK_FALSE(emf::is_spawn_point(w, 1100));
    CHECK_FALSE(emf::is_spawn_point(w, 1));

    int calls = 0;
    emf::TeleportResult last;
    REQUIRE(emf::teleport_to_spawn_point(w, 1, 1025, [&](emf::World, const emf::TeleportResult& r) {
        ++calls;
        last = r;
    }));
    f.step();  // checked (asked outside a callback)
    f.step();  // pushed
    CHECK(calls == 0);
    f.step();  // moved onto the pad
    REQUIRE(calls == 1);
    CHECK(last.moved());
    CHECK(last.last);
    CHECK(last.pos.z == doctest::Approx(1152));
    CHECK(last.distance == doctest::Approx(2));
    CHECK_FALSE(last.off_target);
    emf::api_ref() = nullptr;
}

TEST_CASE("goto: moves you to a position or beside an entity and replies with the result a few steps later") {
    Fixture f;
    CommandCenter center;
    BuiltinContext ctx;
    ctx.api = api_table();
    ctx.center = &center;
    ctx.player = [] { return uint32_t(1); };
    REQUIRE(add_builtin_commands(ctx));
    std::vector<std::string> out;
    auto reply = [&](const std::string& s) { out.push_back(s); };
    auto run = [&](const std::string& line) {  // run in a step's callbacks, as the core runs commands
        center.enqueue(line, 1000, {}, reply);
        f.in_callback([&](EmfWorld* w) { center.run_pending(w, reply, 1000); });
    };
    run("goto 4020 705 1210 ground");
    CHECK(out.back().find("goto: moving player 1 to (4020.0, 705.0, 1210.0) with the ground search") == 0);
    f.step();
    f.step();
    CHECK(out.back().find("now at (4020.0, 705.0, 1210.0)") != std::string::npos);
    CHECK(g_bodies[1].tp.searchBestSpawnPosition);
    put(1030, 4100, 700, 1300);
    f.step();  // the last Teleport goes
    run("goto 1030");
    f.step();
    f.step();
    CHECK(out.back().find("now at (4101.5, 700.0, 1300.0)") != std::string::npos);
    run("goto 1 2");
    CHECK(out.back().find("usage: goto") == 0);
    run("goto #3 4000 700 1200");
    CHECK(out.back().find("no player #3") != std::string::npos);
    run("goto 99999");
    CHECK(out.back().find("has no position") != std::string::npos);
    run("goto 4000 700 99999");
    CHECK(out.back().find("refused") != std::string::npos);
}

TEST_CASE("api teleport: an answer after the session ended (a new world, the menu) has SESSION_ENDED and LAST") {
    Fixture f;
    Seen seen;
    EmfTeleport t = request(1, 4010, 702, 1200, &seen, EMF_TELEPORT_NOTIFY_READY);
    REQUIRE(api_table()->teleport(f.world(), &t));
    f.service.world_reset();  // answered at once, with the world it was meant for
    REQUIRE(seen.results.size() == 1);
    CHECK(seen.results[0].status == EMF_TELEPORT_FAILED);
    CHECK((seen.results[0].flags & EMF_TELEPORT_SESSION_ENDED) != 0u);
    CHECK((seen.results[0].flags & EMF_TELEPORT_LAST) != 0u);  // even with NOTIFY_READY: nothing follows
    CHECK(seen.reasons[0].find("new world") != std::string::npos);
}
