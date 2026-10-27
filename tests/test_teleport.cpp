// teleport (API v20): the internal emf_teleport program pushing the game's add-Teleport record, the state checks, the
// watch after the push and the callbacks, over a fake QueuePush and a small model of the game's teleport program (#701).
#include <doctest.h>

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/offsets.h"
#include "common/signatures.h"
#include "core/system_programs.h"
#include "core/teleport.h"
#include "emf/generated/components.hpp"
#include "fake_world.h"

using namespace emf;
using namespace emf::core;
using emf::test::FakeProgramCtx;
using emf::test::FakeWorld;

namespace {
namespace ecs = emf::gen::ecs;
constexpr double kOne = 4294967296.0;

// ---- the fake game: entities with the components the checks and the watch read ----
struct Ent {
    ecs::CurrentTransform ct{};
    ecs::LastTransform lt{};
    ecs::TeleportCount tc{};
    ecs::Teleport tp{};
    ecs::NetworkHealth hp{};
    ecs::PlayerSpawnPoint sp{};
    std::vector<uint8_t> actor = std::vector<uint8_t>(sizeof(ecs::Actor), 0);
    bool has_ct = true, has_lt = true, has_tc = true, has_tp = false, has_hp = false, has_sp = false, has_actor = false;
    void set_pos(double x, double y, double z) {
        ct.transform.position = {to_fixed(x), to_fixed(y), to_fixed(z)};
        lt.transform = ct.transform;
    }
    double x() const { return from_fixed(ct.transform.position.x); }
    double y() const { return from_fixed(ct.transform.position.y); }
    double z() const { return from_fixed(ct.transform.position.z); }
    void set_ready(bool ready) {
        has_actor = true;
        uint64_t state = ready ? (1ull << kActorReadyBit) : 0;
        std::memcpy(actor.data() + kActorState, &state, 8);
    }
};
std::map<uint32_t, Ent> g_ents;
uint32_t g_size_off = 0;  // a hash whose size reads wrong (a "game update")

void* f_get(void* out, void*, uint32_t hash, uint32_t entity) {
    auto* o = static_cast<uint64_t*>(out);
    o[0] = o[1] = 0;
    auto it = g_ents.find(entity);
    if (it == g_ents.end()) return out;
    Ent& e = it->second;
    auto give = [&](bool has, const void* p, size_t n) {
        if (!has) return;
        o[0] = reinterpret_cast<uint64_t>(p);
        o[1] = hash == g_size_off ? n + 8 : n;
    };
    if (hash == ecs::CurrentTransform::kHash) give(e.has_ct, &e.ct, sizeof e.ct);
    if (hash == ecs::LastTransform::kHash) give(e.has_lt, &e.lt, sizeof e.lt);
    if (hash == ecs::TeleportCount::kHash) give(e.has_tc, &e.tc, sizeof e.tc);
    if (hash == ecs::Teleport::kHash) give(e.has_tp, &e.tp, sizeof e.tp);
    if (hash == ecs::NetworkHealth::kHash) give(e.has_hp, &e.hp, sizeof e.hp);
    if (hash == ecs::PlayerSpawnPoint::kHash) give(e.has_sp, &e.sp, sizeof e.sp);
    if (hash == ecs::Actor::kHash) give(e.has_actor, e.actor.data(), e.actor.size());
    return out;
}

// ---- the fake command buffer and QueuePush ----
struct Pushed {
    uint8_t kind;
    uint32_t size, entity;
    uint8_t* rec;
};
std::vector<Pushed> g_pushes;
alignas(16) uint8_t g_records[64 * 1024];
size_t g_used = 0;
bool g_full = false, g_crash = false;
uint8_t g_crash_on_kind = 0;  // fault only in a push of this kind
uint8_t g_cmdbuf[0x40];
uint8_t* f_queue_push(void* cb, void* buf, uint8_t kind, uint32_t size, uint32_t entity) {
    CHECK(cb == g_cmdbuf);
    CHECK(buf == g_cmdbuf + 0x10);
    if (g_crash || (g_crash_on_kind && kind == g_crash_on_kind)) *static_cast<volatile int*>(nullptr) = 1;
    if (g_full) return nullptr;
    uint8_t* rec = g_records + g_used;
    g_used += (size + 15) & ~size_t(15);
    std::memset(rec, 0, size);
    const uint16_t s16 = static_cast<uint16_t>(size);
    std::memcpy(rec, &s16, 2);
    rec[2] = kind;
    std::memcpy(rec + 8, &entity, 4);
    g_pushes.push_back({kind, size, entity, rec});
    return rec;
}

uint64_t f_register(void*, const void*, char) { return 1; }

// ---- the game's side of one step: #701 for entities that have Teleport, then emf_teleport, then the flush ----
bool g_search_waits = false;     // the ground search never answers (#701 retries forever)
double g_search_dx = 0;          // the ground search moves the target this far in x
double g_pad_dx = 3.0;           // a spawn point's landing pad lies this far from it in x
bool g_clear_ready_on_move = false;

void game_teleport_program() {
    for (auto& [id, e] : g_ents) {
        if (!e.has_tp) continue;
        if (e.tp.applied) {  // the tick after the move: the kind-4 removal
            e.has_tp = false;
            continue;
        }
        if (e.tp.searchBestSpawnPosition && g_search_waits) continue;
        ecs::Teleport& t = e.tp;
        emf::gen::WorldTransform target = t.targetTransform;
        if (t.targetEntityId.id && g_ents.count(t.targetEntityId.id) && g_ents[t.targetEntityId.id].has_sp) {
            target = g_ents[t.targetEntityId.id].ct.transform;
            target.position.x += to_fixed(g_pad_dx);
        } else if (t.searchBestSpawnPosition) {
            target.position.x += to_fixed(g_search_dx);
        }
        e.ct.transform = target;
        e.lt.transform = target;
        e.tc.counter = static_cast<uint8_t>(e.tc.counter + 1);
        t.applied = true;
        if (g_clear_ready_on_move) e.set_ready(false);
    }
}

void flush() {
    for (const auto& p : g_pushes) {
        uint32_t hash = 0;
        std::memcpy(&hash, p.rec + kRecHashes, 4);
        auto it = g_ents.find(p.entity);
        if (it == g_ents.end()) continue;
        if (p.kind == kRecAddComponents && hash == ecs::Teleport::kHash && !it->second.has_tp) {
            it->second.has_tp = true;
            std::memcpy(&it->second.tp, p.rec + kRecData, sizeof(ecs::Teleport));
        }
        if (p.kind == kRecRemoveComponents && hash == ecs::Teleport::kHash) it->second.has_tp = false;
    }
}

uint64_t g_now = 1000;
uint32_t g_session = 1;   // the hosted session number (deps.session)
bool g_health_known = false;  // deps.health answers (the health attribute) ...
std::map<uint32_t, int32_t> g_health;  // ... from here: entity -> current health
std::vector<std::string> g_log;
size_t count_logged(const std::string& needle) {
    size_t n = 0;
    for (const auto& l : g_log) n += l.find(needle) != std::string::npos;
    return n;
}

struct Fixture {
    FakeWorld w;
    SystemPrograms programs;
    TeleportService service;
    FakeProgramCtx ctx;
    std::vector<TeleportOutcome> outs;
    std::vector<size_t> pushes_per_step;
    bool program_runs = true;
    uint32_t tick = 100;
    Fixture() : service(deps()), ctx(w.p()) {
        programs.reset();
        g_ents.clear();
        g_pushes.clear();
        g_used = 0;
        g_full = g_crash = g_search_waits = g_clear_ready_on_move = false;
        g_crash_on_kind = 0;
        g_search_dx = 0;
        g_pad_dx = 3.0;
        g_size_off = 0;
        g_log.clear();
        g_now = 1000;
        g_session = 1;
        g_health_known = false;
        g_health.clear();
        std::memset(g_cmdbuf, 0, sizeof g_cmdbuf);
        const uint64_t cap = 1u << 20;
        std::memcpy(g_cmdbuf + kCmdBufCapacity, &cap, 8);
        uint8_t* cb = g_cmdbuf;
        std::memcpy(ctx.job + kJobCmdBuf, &cb, 8);
        w.put<uint8_t>(off::kKind, EMF_WORLD_SERVER);
        w.put<uint32_t>(off::kTick, tick);
        REQUIRE(programs.add(service.spec(), true));
        REQUIRE(programs.register_into(w.p(), f_register) == 1);
        Ent& p = g_ents[1];  // the player at (4000, 700, 1200), ready for gameplay
        p.set_pos(4000, 700, 1200);
        p.ct.transform.orientation = {0.0f, 0.6f, 0.0f, 0.8f};
        p.ct.transform.scale = {1.0f, 1.0f, 1.0f};
        p.has_hp = true;
        p.hp = {120, 120};
        p.set_ready(true);
    }
    ~Fixture() { programs.reset(); }
    TeleportService::Deps deps() {
        TeleportService::Deps d;
        d.queue_push = f_queue_push;
        d.get_component = f_get;
        d.log = [](const std::string& s) { g_log.push_back(s); };
        d.now_ms = [] { return g_now; };
        d.session = [] { return g_session; };
        d.health = [](uint8_t*, uint32_t e, int32_t* cur) {
            auto it = g_health.find(e);
            if (!g_health_known || it == g_health.end()) return false;
            *cur = it->second;
            return true;
        };
        return d;
    }
    EmfWorld* world() { return reinterpret_cast<EmfWorld*>(w.p()); }
    TeleportRequest req(uint32_t entity, double x, double y, double z, uint32_t flags = 0) {
        TeleportRequest r;
        r.world = world();
        r.entity = entity;
        r.x = x;
        r.y = y;
        r.z = z;
        r.flags = flags;
        r.done = [this](EmfWorld* wd, const TeleportOutcome& o) {
            CHECK(wd == world());
            outs.push_back(o);
        };
        return r;
    }
    bool ask(const TeleportRequest& r, bool on_world_thread = false, std::string* why = nullptr) {
        return service.request(r, on_world_thread, why);
    }
    // One server step (16.7 ms): the game's teleport program, emf_teleport, the flush, then EMF's post-step work.
    void step() {
        const size_t before = g_pushes.size();
        game_teleport_program();
        if (program_runs) system_program_desc(0).update(ctx.p());
        pushes_per_step.push_back(g_pushes.size() - before);
        flush();
        g_pushes.clear();
        g_used = 0;
        ++tick;
        w.put<uint32_t>(off::kTick, tick);
        g_now += 17;
        service.on_server_tick(world());
    }
    void steps(int n) {
        for (int i = 0; i < n; ++i) step();
    }
};

ecs::Teleport pushed_teleport(const Pushed& p) {
    ecs::Teleport t{};
    std::memcpy(&t, p.rec + kRecData, sizeof t);
    return t;
}
}  // namespace

// ---- pure helpers ----

TEST_CASE("teleport: positions are 32.32 fixed point rounded as the game does, round(m * 2^32)") {
    CHECK(to_fixed(1.0) == (int64_t(1) << 32));
    CHECK(to_fixed(-1.0) == -(int64_t(1) << 32));
    CHECK(to_fixed(0.5) == (int64_t(1) << 31));
    CHECK(to_fixed(2.5 / kOne) == 3);   // half away from zero, as trunc(m * 2^32 + 0.5)
    CHECK(to_fixed(2.4 / kOne) == 2);
    CHECK(to_fixed(3704.25) == static_cast<int64_t>(3704.25 * kOne));
    CHECK(from_fixed(to_fixed(4737.25)) == doctest::Approx(4737.25));
    CHECK(from_fixed(to_fixed(-12.5)) == doctest::Approx(-12.5));
}

TEST_CASE("teleport: the yaw quaternion turns about the up axis: (0, sin(yaw/2), 0, cos(yaw/2))") {
    const auto q0 = yaw_quaternion(0);
    CHECK(q0.x == 0.0f);
    CHECK(q0.y == doctest::Approx(0.0));
    CHECK(q0.z == 0.0f);
    CHECK(q0.w == doctest::Approx(1.0));
    const auto q180 = yaw_quaternion(180);
    CHECK(q180.y == doctest::Approx(1.0));
    CHECK(q180.w == doctest::Approx(0.0).epsilon(1e-6));
    const auto q90 = yaw_quaternion(90);
    CHECK(q90.y == doctest::Approx(std::sqrt(0.5)));
    CHECK(q90.w == doctest::Approx(std::sqrt(0.5)));
    const auto qm90 = yaw_quaternion(-90);
    CHECK(qm90.y == doctest::Approx(-std::sqrt(0.5)));
}

TEST_CASE("teleport: the Teleport component keeps the current orientation and scale, or turns to the yaw") {
    emf::gen::WorldTransform cur{};
    cur.position = {1, 2, 3};
    cur.orientation = {0.1f, 0.2f, 0.3f, 0.9f};
    cur.scale = {1.5f, 1.25f, 0.75f};
    ecs::Teleport t = build_teleport(4000.5, 700.25, 1200.0, cur, false, 0, 0, false);
    CHECK(t.targetTransform.position.x == to_fixed(4000.5));
    CHECK(t.targetTransform.position.y == to_fixed(700.25));
    CHECK(t.targetTransform.position.z == to_fixed(1200.0));
    CHECK(t.targetTransform.orientation.x == 0.1f);
    CHECK(t.targetTransform.orientation.w == 0.9f);
    CHECK(t.targetTransform.scale.x == 1.5f);  // #701 copies scale too: never reset it
    CHECK(t.targetTransform.scale.y == 1.25f);
    CHECK(t.targetTransform.scale.z == 0.75f);
    CHECK(t.targetEntityId.id == 0u);
    CHECK_FALSE(t.applied);
    CHECK_FALSE(t.searchBestSpawnPosition);
    CHECK(t.delayEndTime.value == 0);
    t = build_teleport(1, 2, 3, cur, true, 180, 1025, true);
    CHECK(t.targetTransform.orientation.y == doctest::Approx(1.0));
    CHECK(t.targetTransform.orientation.x == 0.0f);
    CHECK(t.targetTransform.scale.x == 1.5f);
    CHECK(t.targetEntityId.id == 1025u);
    CHECK(t.searchBestSpawnPosition);
}

TEST_CASE("teleport: the records: kind 3 adds Teleport (hash, size 80, data at +0x6C), kind 4 removes it, 0x10 checkpoint") {
    emf::gen::WorldTransform cur{};
    const ecs::Teleport t = build_teleport(10, 20, 30, cur, false, 0, 7, true);
    const auto add = add_teleport_record(t);
    REQUIRE(add.size() == 0xBC);
    uint32_t hash = 0;
    uint16_t size = 0;
    std::memcpy(&hash, add.data() + 0x0C, 4);
    std::memcpy(&size, add.data() + 0x4C, 2);
    CHECK(hash == 0x23384d40u);
    CHECK(size == 80);
    for (size_t i = 0x10; i < 0x4C; ++i) CHECK(add[i] == 0);  // the 15 other hash slots stay 0 (no count field)
    for (size_t i = 0x4E; i < 0x6C; ++i) CHECK(add[i] == 0);
    CHECK(std::memcmp(add.data() + 0x6C, &t, 80) == 0);
    const auto rem = remove_teleport_record();
    REQUIRE(rem.size() == 0x4C);
    std::memcpy(&hash, rem.data() + 0x0C, 4);
    CHECK(hash == 0x23384d40u);
    const auto cp = checkpoint_record(3);
    REQUIRE(cp.size() == 0x10);
    uint32_t player = 0;
    std::memcpy(&player, cp.data() + 0x0C, 4);
    CHECK(player == 3u);
}

TEST_CASE("teleport: the numbers are checked first: entity 0, unknown flags, non-finite or out-of-world values") {
    TeleportRequest r;
    r.entity = 1;
    r.x = 4000;
    r.y = 700;
    r.z = 1200;
    CHECK(check_request_numbers(r).empty());
    auto bad = [&](auto change) {
        TeleportRequest c = r;
        change(c);
        return !check_request_numbers(c).empty();
    };
    CHECK(bad([](TeleportRequest& c) { c.entity = 0; }));
    CHECK(bad([](TeleportRequest& c) { c.flags = 0x100; }));
    CHECK(bad([](TeleportRequest& c) { c.x = std::nan(""); }));
    CHECK(bad([](TeleportRequest& c) { c.y = INFINITY; }));
    CHECK(bad([](TeleportRequest& c) { c.z = -INFINITY; }));
    CHECK(bad([](TeleportRequest& c) { c.x = -0.5; }));
    CHECK(bad([](TeleportRequest& c) { c.z = 10240.5; }));
    CHECK(bad([](TeleportRequest& c) { c.y = -1000.5; }));
    CHECK(bad([](TeleportRequest& c) { c.y = 5000.5; }));
    CHECK(bad([](TeleportRequest& c) {
        c.flags = kTpFaceYaw;
        c.yaw_deg = std::nanf("");
    }));
    CHECK(bad([](TeleportRequest& c) { c.spawn_point = 1; }));  // its own spawn point
    CHECK_FALSE(bad([](TeleportRequest& c) { c.flags = kTpKnownFlags; }));
    CHECK_FALSE(bad([](TeleportRequest& c) {  // a spawn point is the target: x/y/z are not used
        c.spawn_point = 1025;
        c.x = std::nan("");
    }));
    CHECK_FALSE(bad([](TeleportRequest& c) { c.x = 0; }));
    CHECK_FALSE(bad([](TeleportRequest& c) { c.z = 10240; }));
}

// ---- the state checks ----

TEST_CASE("teleport: the state check refuses entities without CurrentTransform, LastTransform or TeleportCount, and dead ones") {
    Fixture f;
    auto check = [&](uint32_t e) {
        TeleportRequest r = f.req(e, 4010, 700, 1200);
        return check_teleport_state(f_get, f.w.p(), r);
    };
    StateCheck s = check(1);
    REQUIRE(s.verdict == StateCheck::Ok);
    CHECK(s.counter == 0);
    CHECK(s.target[0] == 4010.0);
    CHECK(s.current.orientation.y == 0.6f);
    CHECK(check(99).verdict == StateCheck::Refuse);  // no such entity
    CHECK(check(99).cause == "no CurrentTransform");
    g_ents[1].has_lt = false;
    s = check(1);
    CHECK(s.verdict == StateCheck::Refuse);
    CHECK(s.cause == "no LastTransform or TeleportCount");
    CHECK(s.why.find("LastTransform") != std::string::npos);
    g_ents[1].has_lt = true;
    g_ents[1].has_tc = false;
    CHECK(check(1).verdict == StateCheck::Refuse);
    g_ents[1].has_tc = true;
    g_ents[1].hp.health = 0;  // dead (a dead player moved into the terrain falls through the world)
    s = check(1);
    CHECK(s.verdict == StateCheck::Refuse);
    CHECK(s.cause == "dead");
    g_ents[1].hp.health = 1;
    CHECK(check(1).verdict == StateCheck::Ok);
    // a player in the server world has Health but no NetworkHealth: the health attribute decides (confirmed in game)
    g_ents[1].has_hp = false;
    int32_t attr = 0;
    HealthFn health = [&](uint8_t*, uint32_t e, int32_t* cur) {
        if (e != 1) return false;
        *cur = attr;
        return true;
    };
    s = check_teleport_state(f_get, f.w.p(), f.req(1, 4010, 700, 1200), health);
    CHECK(s.verdict == StateCheck::Refuse);
    CHECK(s.cause == "dead");
    attr = 60;
    CHECK(check_teleport_state(f_get, f.w.p(), f.req(1, 4010, 700, 1200), health).verdict == StateCheck::Ok);
    g_ents[1].has_hp = true;  // the attribute wins over NetworkHealth when it is known
    g_ents[1].hp.health = 0;
    CHECK(check_teleport_state(f_get, f.w.p(), f.req(1, 4010, 700, 1200), health).verdict == StateCheck::Ok);
    CHECK(check(1).verdict == StateCheck::Refuse);  // without the attribute: NetworkHealth 0 is dead
    g_ents[1].hp.health = 1;
    g_size_off = ecs::TeleportCount::kHash;  // a game update changed a size: refused, never misread
    CHECK(check(1).verdict == StateCheck::Refuse);
    CHECK(check(1).cause == "component sizes");
}

TEST_CASE("teleport: a pending game Teleport and a player not ready for gameplay wait; NPCs skip the ready check") {
    Fixture f;
    TeleportRequest r = f.req(1, 4010, 700, 1200);
    g_ents[1].has_tp = true;
    CHECK(check_teleport_state(f_get, f.w.p(), r).verdict == StateCheck::Wait);
    g_ents[1].has_tp = false;
    g_ents[1].set_ready(false);  // respawning, loading, or being teleported
    StateCheck s = check_teleport_state(f_get, f.w.p(), r);
    CHECK(s.verdict == StateCheck::Wait);
    CHECK(s.cause == "not ready for gameplay");
    Ent& villager = g_ents[1030];
    villager.set_pos(4005, 700, 1200);
    villager.set_ready(false);  // a villager's bit stays as it is: not checked
    r.entity = 1030;
    CHECK(check_teleport_state(f_get, f.w.p(), r).verdict == StateCheck::Ok);
}

TEST_CASE("teleport: a spawn point must have PlayerSpawnPoint and a position; it is the target") {
    Fixture f;
    TeleportRequest r = f.req(1, 0, 0, 0);
    r.spawn_point = 1025;
    StateCheck s = check_teleport_state(f_get, f.w.p(), r);
    CHECK(s.verdict == StateCheck::Refuse);
    CHECK(s.cause == "no spawn point");  // 1025 does not exist
    Ent& altar = g_ents[1025];
    altar.set_pos(3900, 690, 1150);
    altar.has_lt = altar.has_tc = false;
    s = check_teleport_state(f_get, f.w.p(), r);
    CHECK(s.verdict == StateCheck::Refuse);
    CHECK(s.cause == "no spawn point");
    altar.has_sp = true;
    s = check_teleport_state(f_get, f.w.p(), r);
    REQUIRE(s.verdict == StateCheck::Ok);
    CHECK(s.target[0] == 3900.0);
    CHECK(s.target[1] == 690.0);
    CHECK(s.target[2] == 1150.0);
}

// ---- the flow ----

TEST_CASE("teleport: the program declares nothing and runs on the server world only") {
    Fixture f;
    const ProgramDesc& d = system_program_desc(0);
    CHECK(std::string(d.name.s, d.name.n) == "emf_teleport");
    CHECK(d.otherRead.n == 0);
    CHECK(d.otherWrite.n == 0);
    CHECK(d.eventsIn.n == 0);
    CHECK(d.eventsOut.n == 0);
    CHECK((d.flags & kFlagClient) == 0);
    CHECK((d.flags & kFlagSystem) != 0);
    CHECK((d.flags & kFlagNotPredicted) != 0);
}

TEST_CASE("teleport: a request is checked after a step, pushed in the next, moved by the game in the one after; the "
          "callback gets the final position") {
    Fixture f;
    REQUIRE(f.ask(f.req(1, 4010, 705, 1200)));
    CHECK(f.service.busy(1));
    f.step();  // post-step: the state check
    CHECK(f.pushes_per_step.back() == 0);
    CHECK(f.outs.empty());
    f.step();  // emf_teleport pushes; the flush adds Teleport
    REQUIRE(f.pushes_per_step.back() == 1);
    CHECK(g_ents[1].has_tp);
    const ecs::Teleport t = g_ents[1].tp;
    CHECK(t.targetTransform.position.x == to_fixed(4010));
    CHECK(t.targetTransform.position.y == to_fixed(705));
    CHECK(t.targetTransform.orientation.y == 0.6f);  // the current orientation
    CHECK(t.targetTransform.scale.x == 1.0f);
    CHECK_FALSE(t.searchBestSpawnPosition);
    CHECK(f.outs.empty());  // not moved yet
    f.step();  // #701 moves it
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].status == kTpMoved);
    CHECK(f.outs[0].entity == 1u);
    CHECK(f.outs[0].pos[0] == doctest::Approx(4010));
    CHECK(f.outs[0].pos[1] == doctest::Approx(705));
    CHECK(f.outs[0].distance == doctest::Approx(0).epsilon(1e-6));
    CHECK(f.outs[0].flags == 0u);
    CHECK(f.outs[0].reason.empty());
    CHECK_FALSE(f.service.busy(1));
    CHECK(f.service.under_way() == 0);
    f.step();  // #701 removes Teleport; nothing else happens
    CHECK_FALSE(g_ents[1].has_tp);
    CHECK(f.outs.size() == 1);
}

TEST_CASE("teleport: called inside a callback on the server world, the state is checked at once and it pushes in the next step") {
    Fixture f;
    std::string why;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200), true, &why));
    f.step();
    CHECK(f.pushes_per_step.back() == 1);
    f.step();
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].status == kTpMoved);
    // a refusal there returns false at once (no callback)
    g_ents[5].set_pos(4000, 700, 1200);
    g_ents[5].has_tc = false;
    CHECK_FALSE(f.ask(f.req(5, 4010, 700, 1200), true, &why));
    CHECK(why.find("TeleportCount") != std::string::npos);
    f.steps(3);
    CHECK(f.outs.size() == 1);
}

TEST_CASE("teleport: refused at once: not the server world, bad numbers, a teleport of the entity under way, 64 under way") {
    Fixture f;
    std::string why;
    f.w.put<uint8_t>(off::kKind, EMF_WORLD_CLIENT);
    CHECK_FALSE(f.ask(f.req(1, 4010, 700, 1200), false, &why));
    CHECK(why.find("server world") != std::string::npos);
    f.w.put<uint8_t>(off::kKind, EMF_WORLD_SERVER);
    CHECK_FALSE(f.ask(f.req(1, std::nan(""), 700, 1200), false, &why));
    CHECK_FALSE(f.ask(f.req(1, 4010, 700, 99999), false, &why));
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200)));
    CHECK_FALSE(f.ask(f.req(1, 4020, 700, 1200), false, &why));  // the same tick: never two records for one entity
    CHECK(why.find("already under way") != std::string::npos);
    for (uint32_t e = 2; e <= 64; ++e) CHECK(f.ask(f.req(e, 4010, 700, 1200)));
    CHECK_FALSE(f.ask(f.req(65, 4010, 700, 1200), false, &why));
    CHECK(why.find("64") != std::string::npos);
    TeleportRequest none = f.req(1, 4010, 700, 1200);
    none.world = nullptr;
    CHECK_FALSE(f.ask(none));
}

TEST_CASE("teleport: two requests for one entity in one tick push one record (the second is refused)") {
    Fixture f;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200), true));
    CHECK_FALSE(f.ask(f.req(1, 4020, 700, 1200), true));
    f.step();
    CHECK(f.pushes_per_step.back() == 1);
    f.step();
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].pos[0] == doctest::Approx(4010));
    // after the move a new one is accepted
    CHECK(f.ask(f.req(1, 4020, 700, 1200), true));
}

TEST_CASE("teleport: a refusal found after a step reaches the callback; each cause is logged once per session") {
    Fixture f;
    for (uint32_t e : {7u, 8u}) {
        g_ents[e].set_pos(4000, 700, 1200);
        g_ents[e].has_lt = false;  // a barrel: no LastTransform / TeleportCount
        g_ents[e].has_tc = false;
        REQUIRE(f.ask(f.req(e, 4010, 700, 1200)));
    }
    f.step();
    REQUIRE(f.outs.size() == 2);
    CHECK(f.outs[0].status == kTpFailed);
    CHECK(f.outs[0].reason.find("cannot be teleported") != std::string::npos);
    CHECK(f.outs[1].status == kTpFailed);
    CHECK(count_logged("cannot be teleported") == 1);
    f.step();
    CHECK(f.pushes_per_step.back() == 0);  // nothing pushed for them
    // a dead player
    g_ents[1].hp.health = 0;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200)));
    f.step();
    REQUIRE(f.outs.size() == 3);
    CHECK(f.outs[2].status == kTpFailed);
    CHECK(f.outs[2].reason.find("dead") != std::string::npos);
    // a new session logs the same cause again
    f.service.world_reset();
    g_ents[9].set_pos(4000, 700, 1200);
    g_ents[9].has_tc = false;
    REQUIRE(f.ask(f.req(9, 4010, 700, 1200)));
    f.step();
    CHECK(count_logged("cannot be teleported") == 2);
}

TEST_CASE("teleport: a game teleport still under way is waited for (pushed after it ends), at most 5 s") {
    Fixture f;
    g_ents[1].has_tp = true;  // e.g. a respawn: the game's own Teleport, not applied yet
    g_ents[1].tp.applied = false;
    g_search_waits = true;
    g_ents[1].tp.searchBestSpawnPosition = true;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200)));
    f.steps(10);
    for (size_t n : f.pushes_per_step) CHECK(n == 0);
    CHECK(f.outs.empty());
    g_ents[1].has_tp = false;  // it ended
    f.steps(2);
    CHECK(f.pushes_per_step.back() == 1);
    g_search_waits = false;
    f.step();
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].status == kTpMoved);

    g_ents[1].set_ready(false);  // never ready again: refused after 5 s
    REQUIRE(f.ask(f.req(1, 4020, 700, 1200)));
    f.steps(299);
    CHECK(f.outs.size() == 1);
    f.steps(2);
    REQUIRE(f.outs.size() == 2);
    CHECK(f.outs[1].status == kTpFailed);
    CHECK(f.outs[1].reason.find("waited 5 s") != std::string::npos);
}

TEST_CASE("teleport: not moved within 600 ticks: given up, and the Teleport left on the entity is removed (kind 4)") {
    Fixture f;
    g_search_waits = true;  // the ground search never answers: #701 skips the entity every tick
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200, kTpFindGround), true));
    f.step();
    REQUIRE(f.pushes_per_step.back() == 1);
    f.steps(598);
    CHECK(f.outs.empty());
    CHECK(g_ents[1].has_tp);
    f.step();
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].status == kTpFailed);
    CHECK(f.outs[0].reason.find("not moved within 10 s") != std::string::npos);
    CHECK(f.outs[0].pos[0] == doctest::Approx(4000));  // where it still is
    CHECK(count_logged("it is removed") == 1);
    f.step();  // the program pushes the removal
    REQUIRE(f.pushes_per_step.back() == 1);
    CHECK_FALSE(g_ents[1].has_tp);  // the player is not left "not ready for gameplay"
    CHECK(count_logged("watchdog") == 1);
    f.steps(5);
    CHECK(f.outs.size() == 1);
}

TEST_CASE("teleport: FIND_GROUND reports where it landed: off target past 1 m, the raw target when it found nothing") {
    Fixture f;
    g_search_dx = 1.5;  // the search snapped 1.5 m sideways
    REQUIRE(f.ask(f.req(1, 4010, 703, 1200, kTpFindGround), true));
    f.steps(2);
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].status == kTpMoved);
    CHECK(f.outs[0].pos[0] == doctest::Approx(4011.5));
    CHECK(f.outs[0].distance == doctest::Approx(1.5));
    CHECK(f.outs[0].flags == kTpOffTarget);
    f.steps(1);
    g_search_dx = 0;  // nothing walkable within ~10 m: the game used the target as it was
    REQUIRE(f.ask(f.req(1, 4020, 740, 1200, kTpFindGround), true));
    f.steps(2);
    REQUIRE(f.outs.size() == 2);
    CHECK(f.outs[1].flags == kTpRawTarget);
    f.steps(1);
    g_search_dx = 0.5;  // a small snap: neither flag
    REQUIRE(f.ask(f.req(1, 4030, 701, 1200, kTpFindGround), true));
    f.steps(2);
    REQUIRE(f.outs.size() == 3);
    CHECK(f.outs[2].flags == 0u);
    f.steps(1);
    REQUIRE(f.ask(f.req(1, 4040, 700, 1200), true));  // without FIND_GROUND an exact landing is no "raw target"
    f.steps(2);
    REQUIRE(f.outs.size() == 4);
    CHECK(f.outs[3].flags == 0u);
}

TEST_CASE("teleport: to a spawn point the game lands it on a pad; the distance is from the spawn point") {
    Fixture f;
    Ent& altar = g_ents[1025];
    altar.set_pos(3900, 690, 1150);
    altar.has_lt = altar.has_tc = false;
    altar.has_sp = true;
    TeleportRequest r = f.req(1, 0, 0, 0);
    r.spawn_point = 1025;
    REQUIRE(f.ask(r, true));
    f.step();
    REQUIRE(f.pushes_per_step.back() == 1);
    CHECK(g_ents[1].tp.targetEntityId.id == 1025u);
    CHECK(g_ents[1].tp.targetTransform.position.x == to_fixed(3900));  // its position until the game picks the pad
    f.step();
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].status == kTpMoved);
    CHECK(f.outs[0].pos[0] == doctest::Approx(3903));
    CHECK(f.outs[0].distance == doctest::Approx(3));
    CHECK(f.outs[0].flags == 0u);  // on a pad: not off target
}

TEST_CASE("teleport: FACE_YAW turns it; CHECKPOINT adds a replication checkpoint for players only") {
    Fixture f;
    TeleportRequest r = f.req(1, 4010, 700, 1200, kTpFaceYaw | kTpCheckpoint);
    r.yaw_deg = 180;
    REQUIRE(f.ask(r, true));
    game_teleport_program();
    system_program_desc(0).update(f.ctx.p());
    REQUIRE(g_pushes.size() == 2);
    CHECK(g_pushes[0].kind == kRecAddComponents);
    CHECK(g_pushes[0].size == 0xBC);
    CHECK(g_pushes[0].entity == 1u);
    CHECK(pushed_teleport(g_pushes[0]).targetTransform.orientation.y == doctest::Approx(1.0));
    CHECK(g_pushes[1].kind == kRecCheckpoint);
    CHECK(g_pushes[1].size == 0x10);
    uint32_t player = 0;
    std::memcpy(&player, g_pushes[1].rec + 0x0C, 4);
    CHECK(player == 1u);
    g_pushes.clear();
    g_used = 0;
    Ent& v = g_ents[1030];  // a villager: no checkpoint
    v.set_pos(4005, 700, 1200);
    REQUIRE(f.ask(f.req(1030, 4006, 700, 1200, kTpCheckpoint), true));
    system_program_desc(0).update(f.ctx.p());
    REQUIRE(g_pushes.size() == 1);
    CHECK(g_pushes[0].kind == kRecAddComponents);
    CHECK(g_pushes[0].entity == 1030u);
}

TEST_CASE("teleport: a full command buffer is retried next step, and given up after 120 tries") {
    Fixture f;
    g_full = true;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200), true));
    f.steps(5);
    CHECK(f.outs.empty());
    g_full = false;
    f.step();
    CHECK(g_ents[1].has_tp);
    f.step();
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].status == kTpMoved);
    g_full = true;
    REQUIRE(f.ask(f.req(1, 4020, 700, 1200), true));  // waits a step for the last Teleport's removal
    f.steps(121);
    CHECK(f.outs.size() == 1);
    f.step();
    REQUIRE(f.outs.size() == 2);
    CHECK(f.outs[1].status == kTpFailed);
    CHECK(f.outs[1].reason.find("stayed full") != std::string::npos);
}

TEST_CASE("teleport: a fault in QueuePush turns teleport off: the request fails, later ones are refused") {
    Fixture f;
    g_crash = true;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200), true));
    f.step();
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].status == kTpFailed);
    CHECK(f.outs[0].reason.find("faulted") != std::string::npos);
    CHECK_FALSE(f.service.enabled());
    std::string why;
    CHECK_FALSE(f.ask(f.req(1, 4010, 700, 1200), true, &why));
    CHECK(why.find("off") != std::string::npos);
    CHECK(count_logged("QueuePush faulted") == 1);
}

TEST_CASE("teleport: a command buffer that does not look like one (a game update) turns teleport off") {
    Fixture f;
    const uint64_t zero = 0;
    std::memcpy(g_cmdbuf + kCmdBufCapacity, &zero, 8);
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200), true));
    f.step();
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].status == kTpFailed);
    CHECK(f.outs[0].reason.find("command buffer") != std::string::npos);
    CHECK_FALSE(f.service.enabled());
    // a job without a command buffer pointer: the same
    Fixture g;
    std::memset(g.ctx.job + kJobCmdBuf, 0, 8);
    REQUIRE(g.ask(g.req(1, 4010, 700, 1200), true));
    g.step();
    REQUIRE(g.outs.size() == 1);
    CHECK_FALSE(g.service.enabled());
}

TEST_CASE("teleport: when emf_teleport does not run in the world, waiting requests fail after 2 s") {
    Fixture f;
    f.program_runs = false;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200), true));
    f.steps(100);  // 1.7 s
    CHECK(f.outs.empty());
    f.steps(30);
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].status == kTpFailed);
    CHECK(f.outs[0].reason.find("not running") != std::string::npos);
}

TEST_CASE("teleport: NOTIFY_READY: a second callback once the player is ready for gameplay again (or after 10 s)") {
    Fixture f;
    g_clear_ready_on_move = true;  // streaming the destination: not ready for a while
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200, kTpNotifyReady), true));
    f.steps(2);
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].status == kTpMoved);
    CHECK_FALSE(f.service.busy(1));  // moved: the entity may be teleported again
    f.steps(20);
    CHECK(f.outs.size() == 1);
    g_ents[1].set_ready(true);
    f.step();
    REQUIRE(f.outs.size() == 2);
    CHECK(f.outs[1].status == kTpReady);
    CHECK(f.outs[1].reason.empty());
    CHECK(f.outs[1].pos[0] == doctest::Approx(4010));
    // never ready: READY after 10 s, with the reason
    REQUIRE(f.ask(f.req(1, 4020, 700, 1200, kTpNotifyReady), true));
    f.steps(2);
    REQUIRE(f.outs.size() == 3);
    f.steps(599);
    CHECK(f.outs.size() == 3);
    f.steps(2);
    REQUIRE(f.outs.size() == 4);
    CHECK(f.outs[3].status == kTpReady);
    CHECK(f.outs[3].reason.find("still not ready") != std::string::npos);
    // NPCs: READY right after MOVED (they have no "ready" state)
    Ent& v = g_ents[1030];
    v.set_pos(4005, 700, 1200);
    REQUIRE(f.ask(f.req(1030, 4006, 700, 1200, kTpNotifyReady), true));
    f.steps(2);
    REQUIRE(f.outs.size() == 6);
    CHECK(f.outs[4].status == kTpMoved);
    CHECK(f.outs[5].status == kTpReady);
    CHECK(f.outs[5].pos[0] == doctest::Approx(4006));
}

TEST_CASE("teleport: an entity that disappears before it moved ends FAILED; a new world ends everything under way") {
    Fixture f;
    g_ents[1030].set_pos(4005, 700, 1200);
    g_search_waits = true;
    REQUIRE(f.ask(f.req(1030, 4006, 700, 1200, kTpFindGround), true));
    f.step();
    g_ents.erase(1030);
    f.step();
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].status == kTpFailed);
    CHECK(f.outs[0].reason.find("no longer exists") != std::string::npos);
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200)));
    f.service.world_reset();
    CHECK(f.service.under_way() == 0);
    REQUIRE(f.outs.size() == 2);  // answered at once, during the new world's setup (never on its ticks)
    CHECK(f.outs[1].status == kTpFailed);
    CHECK(f.outs[1].session_ended);
    CHECK(f.outs[1].reason.find("new world") != std::string::npos);
    f.step();
    CHECK(f.outs.size() == 2);
}

TEST_CASE("teleport: a callback may ask for the next teleport (e.g. /unstuck falling back to the spawn point)") {
    Fixture f;
    int chained = 0;
    TeleportRequest r = f.req(1, 4010, 700, 1200);
    r.done = [&](EmfWorld* w, const TeleportOutcome& o) {
        f.outs.push_back(o);
        if (chained++ == 0) {
            TeleportRequest next = f.req(1, 4100, 700, 1200);
            CHECK(f.service.request(next, true, nullptr));  // inside the callback: on the world's thread
            (void)w;
        }
    };
    REQUIRE(f.ask(r, true));
    f.steps(2);
    REQUIRE(f.outs.size() == 1);
    f.steps(2);  // it waits a step for the game to remove the first Teleport
    CHECK(f.outs.size() == 1);
    f.step();
    REQUIRE(f.outs.size() == 2);
    CHECK(f.outs[1].pos[0] == doctest::Approx(4100));
}

TEST_CASE("teleport: QueuePush is a signature with a checked RVA on both exes of 1273043; the layouts count as checked there only") {
    const SigDef* d = find_signature("QueuePush");
    REQUIRE(d != nullptr);
    CHECK(d->id == Sig::QueuePush);
    CHECK(d->rva[0] == kRvaUnchecked);  // 1269579: not at hand
    CHECK(d->rva[1] == 0x953670u);
    CHECK(d->rva[2] == 0x5f3bb0u);
    CHECK_FALSE(d->required);
    CHECK_FALSE(teleport_layouts_checked(0));
    CHECK(teleport_layouts_checked(1));
    CHECK(teleport_layouts_checked(2));
    CHECK_FALSE(teleport_layouts_checked(-1));
    CHECK_FALSE(teleport_layouts_checked(3));
}


// ---- edge cases ----

TEST_CASE("teleport: a new world answers everything under way at once, with the old world and session_ended") {
    Fixture f;
    g_search_waits = true;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200, kTpFindGround), true));
    f.step();  // pushed, waiting for the ground search
    g_ents[1030].set_pos(4005, 700, 1200);
    REQUIRE(f.ask(f.req(1030, 4006, 700, 1200)));  // still to be checked
    f.service.world_reset();  // synchronous: never on the new world's first tick, with its pointer
    REQUIRE(f.outs.size() == 2);
    for (const auto& o : f.outs) {
        CHECK(o.status == kTpFailed);
        CHECK(o.session_ended);
        CHECK(o.reason.find("new world") != std::string::npos);
    }
    f.step();
    CHECK(f.outs.size() == 2);  // answered exactly once
}

TEST_CASE("teleport: when the server world is gone (back to the menu), end_session answers each request once") {
    Fixture f;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200)));
    g_clear_ready_on_move = true;
    g_ents[1030].set_pos(4005, 700, 1200);
    REQUIRE(f.ask(f.req(1030, 4006, 700, 1200)));
    CHECK(f.service.end_session("the world ended") == 2);
    REQUIRE(f.outs.size() == 2);
    CHECK(f.outs[0].session_ended);
    CHECK(f.outs[0].reason == "the world ended");
    CHECK(f.service.under_way() == 0);
    CHECK(f.service.end_session("again") == 0);
    f.step();
    CHECK(f.outs.size() == 2);
}

TEST_CASE("teleport: a QueuePush fault keeps watching what EMF already pushed and names the entities it may strand") {
    Fixture f;
    g_search_waits = true;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200, kTpFindGround), true));
    f.step();  // player 1's Teleport is out; the ground search waits
    REQUIRE(g_ents[1].has_tp);
    g_ents[1030].set_pos(4005, 700, 1200);
    REQUIRE(f.ask(f.req(1030, 4006, 700, 1200), true));
    g_crash = true;
    f.step();  // 1030's push faults
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].entity == 1030u);
    CHECK(f.outs[0].status == kTpFailed);
    CHECK_FALSE(f.service.enabled());
    CHECK(count_logged("may stay frozen") == 1);
    CHECK(count_logged("rejoins: 1") == 1);
    g_crash = false;
    g_search_waits = false;  // the game moves player 1 after all: still reported
    f.step();
    REQUIRE(f.outs.size() == 2);
    CHECK(f.outs[1].entity == 1u);
    CHECK(f.outs[1].status == kTpMoved);
}

TEST_CASE("teleport: when emf_teleport stops running, the entities whose Teleport it cannot take back are logged once") {
    Fixture f;
    g_search_waits = true;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200, kTpFindGround), true));
    f.step();
    f.program_runs = false;
    f.steps(700);  // given up at 600; the removal cannot be pushed
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].status == kTpFailed);
    CHECK(g_ents[1].has_tp);
    CHECK(count_logged("not running") >= 1);
    CHECK(count_logged("cannot take back the Teleport it gave to entity 1") == 1);
}

TEST_CASE("teleport: after the move the watchdog never removes a game Teleport (a respawn near age 600)") {
    Fixture f;
    g_clear_ready_on_move = true;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200, kTpNotifyReady), true));
    f.steps(2);
    REQUIRE(f.outs.size() == 1);  // MOVED; waiting for READY
    f.steps(580);
    g_ents[1].has_tp = true;  // the player died and the game's respawn Teleport waits for its ground search
    g_ents[1].tp = {};
    g_ents[1].tp.searchBestSpawnPosition = true;
    g_search_waits = true;
    f.steps(40);
    CHECK(g_ents[1].has_tp);  // still the game's: no kind-4 record from EMF
    CHECK(count_logged("it is removed") == 0);
}

TEST_CASE("teleport: a request waiting to push is checked again every step: a death or a game teleport stops it") {
    Fixture f;
    g_full = true;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200), true));
    f.steps(3);
    CHECK(f.outs.empty());
    g_ents[1].hp.health = 0;  // died while the buffer was full
    f.step();
    REQUIRE(f.outs.size() == 1);
    CHECK(f.outs[0].status == kTpFailed);
    CHECK(f.outs[0].reason.find("dead") != std::string::npos);
    g_full = false;
    f.steps(3);
    CHECK_FALSE(g_ents[1].has_tp);  // never pushed
    // a game Teleport that appears meanwhile sends it back to waiting
    g_ents[1].hp.health = 100;
    g_full = true;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200), true));
    f.step();
    g_ents[1].has_tp = true;  // added at that step's flush: the post-step sees it
    g_ents[1].tp = {};
    g_ents[1].tp.searchBestSpawnPosition = true;
    g_search_waits = true;
    f.service.on_server_tick(f.world());
    g_full = false;
    const size_t before = f.pushes_per_step.size();
    f.steps(3);
    for (size_t i = before; i < f.pushes_per_step.size(); ++i) CHECK(f.pushes_per_step[i] == 0);  // nothing over the game's
}

TEST_CASE("teleport: a player whose health cannot be read is refused (unknown is not alive)") {
    Fixture f;
    g_ents[1].has_hp = false;  // no NetworkHealth and no health attribute answer
    std::string why;
    CHECK_FALSE(f.ask(f.req(1, 4010, 700, 1200), true, &why));
    CHECK(why.find("health could not be read") != std::string::npos);
    g_health_known = true;  // the attribute is known: alive
    g_health[1] = 80;
    CHECK(f.ask(f.req(1, 4010, 700, 1200), true, &why));
    g_ents[1030].set_pos(4005, 700, 1200);  // an NPC without health: allowed
    CHECK(f.ask(f.req(1030, 4006, 700, 1200), true, &why));
}

TEST_CASE("teleport: a removal waiting for room is dropped when the entity holds another Teleport or the session changed") {
    Fixture f;
    g_search_waits = true;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200, kTpFindGround), true));
    f.step();
    f.steps(599);  // given up: the removal is queued
    REQUIRE(f.outs.size() == 1);
    g_full = true;  // no room for it
    f.steps(3);
    CHECK(g_ents[1].has_tp);
    g_ents[1].tp.targetTransform.position.x = to_fixed(9.0);  // now the game's own Teleport (an id reused, a respawn)
    f.service.on_server_tick(f.world());                       // the post-step after the flush that added it
    g_full = false;
    f.steps(3);
    CHECK(g_ents[1].has_tp);  // EMF did not remove the game's
    CHECK(count_logged("not EMF's") >= 1);

    Fixture g;
    g_search_waits = true;
    REQUIRE(g.ask(g.req(1, 4010, 700, 1200, kTpFindGround), true));
    g.step();
    g.steps(599);
    g_full = true;
    g.steps(2);
    g_session = 2;  // a reload kept the world's address
    g_full = false;
    g.steps(3);
    CHECK(g_ents[1].has_tp);  // no removal into the new session
}

TEST_CASE("teleport: a fault in the checkpoint push turns teleport off like one in the add") {
    Fixture f;
    REQUIRE(f.ask(f.req(1, 4010, 700, 1200, kTpCheckpoint), true));
    g_crash_on_kind = kRecCheckpoint;
    f.step();
    CHECK_FALSE(f.service.enabled());
    CHECK(count_logged("QueuePush faulted") == 1);
}
