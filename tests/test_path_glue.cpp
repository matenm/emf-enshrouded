// path_find (API v37): the emf_path program over fake game calls (create, result, destroy, defaults).
#include <doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "common/offsets.h"
#include "core/api.h"
#include "core/api_sizes.h"
#include "core/dispatcher.h"
#include "core/path_glue.h"
#include "emf/generated/components.hpp"
#include "fake_path_grid.h"
#include "fake_world.h"

using namespace emf;
using namespace emf::core;
using emf::test::FakeProgramCtx;
using emf::test::FakeWorld;

namespace {
struct GamePoint {
    float x, y, z;
    uint8_t move, pad[3];
};
uint32_t g_next_id = 0;
std::vector<uint32_t> g_owners, g_destroyed;
std::vector<float> g_targets;  // target x of each created query
std::map<uint32_t, std::vector<GamePoint>> g_found;
std::map<uint32_t, std::vector<uint32_t>> g_door_ids;  // the game's door entity per point (out +0x10 / +0x18)
bool g_get_faults = false;

std::vector<std::vector<uint8_t>> g_query_bytes;  // every created query as the game got it
uint32_t* f_create(void*, uint32_t* id, const void* q) {
    g_query_bytes.emplace_back(static_cast<const uint8_t*>(q), static_cast<const uint8_t*>(q) + kPathQueryBytes);
    uint32_t owner = 0;
    float tx = 0;
    std::memcpy(&owner, q, 4);
    std::memcpy(&tx, static_cast<const uint8_t*>(q) + 0x1c, 4);
    g_owners.push_back(owner);
    g_targets.push_back(tx);
    *id = ++g_next_id;
    return id;
}
bool f_destroy(void*, uint32_t, uint32_t id) {
    g_destroyed.push_back(id);
    return true;
}
bool f_get(void*, void* out, uint32_t id, bool dirty) {
    if (g_get_faults) *static_cast<volatile int*>(nullptr) = 1;
    CHECK_FALSE(dirty);
    auto it = g_found.find(id);
    if (it == g_found.end()) return false;
    auto* o = static_cast<uint8_t*>(out);
    const GamePoint* p = it->second.data();
    const uint64_t n = it->second.size();
    std::memcpy(o, &p, 8);
    std::memcpy(o + 8, &n, 8);
    if (auto d = g_door_ids.find(id); d != g_door_ids.end()) {
        const uint32_t* dp = d->second.data();
        const uint64_t dn = d->second.size();
        std::memcpy(o + 0x10, &dp, 8);
        std::memcpy(o + 0x18, &dn, 8);
    }
    o[0x24] = 1;  // PathFound
    return true;
}
void* f_defaults(void* q) {
    std::memset(q, 0, kPathQueryBytes);
    return q;
}

std::vector<std::string> g_answers;
void on_path(EmfWorld*, uint32_t id, const EmfPathResult* r, const EmfPathPoint* pts, void*) {
    g_answers.push_back(std::to_string(id) + ":" + std::to_string(r->status) + ":" + std::to_string(r->count) + ":" +
                        std::to_string(r->reaches) + (pts && r->count ? ":" + std::to_string(pts[r->count - 1].move) : ""));
}

struct Rig {
    FakeWorld world;
    FakeProgramCtx ctx;
    alignas(8) uint8_t cmdbuf[0x40] = {};
    ProgramSpec spec;
    explicit Rig(bool grid_layout = false) : ctx((world.put<uint8_t>(off::kKind, 0), world.p())) {
        void* cb = cmdbuf;
        std::memcpy(ctx.job + 0x08, &cb, 8);
        g_next_id = 0;
        g_query_bytes.clear();
        g_owners.clear();
        g_destroyed.clear();
        g_targets.clear();
        g_found.clear();
        g_door_ids.clear();
        g_answers.clear();
        g_get_faults = false;
        PathGlueDeps d;
        d.fns.create = f_create;
        d.fns.destroy = f_destroy;
        d.fns.get = f_get;
        d.fns.defaults = f_defaults;
        d.component = [](uint8_t*, uint32_t hash, uint32_t entity) -> const void* {
            static uint8_t xf[56];
            return hash == off::kHashCurrentTransform && entity == 7 ? xf : nullptr;
        };
        d.after_step = [](std::function<void(EmfWorld*)> fn) { fn(nullptr); };
        d.log = [](const std::string&) {};
        d.now_ms = [] { return uint64_t(1000); };
        d.grid_layout = grid_layout;
        spec = path_setup(d);
    }
    void step() { spec.fn(ctx.p(), spec.user); }
};

EmfPathQuery query(float tx) {
    EmfPathQuery q{};
    q.size = sizeof q;
    q.from[1] = 50;
    q.to[0] = tx;
    q.to[1] = 50;
    return q;
}
}  // namespace

TEST_CASE("path glue: the emf_path program sends the query, reads the game's answer, destroys it, calls back") {
    Rig r;
    CHECK(r.spec.name == std::string(kPathProgramName));
    CHECK(r.spec.world_kind == EMF_WORLD_SERVER);
    REQUIRE(path_service() != nullptr);
    const uint32_t id = path_service()->request(query(12), 7, on_path, nullptr, 1000, nullptr, nullptr);
    REQUIRE(id != 0);
    r.step();
    REQUIRE(g_owners.size() == 1u);
    CHECK(g_owners[0] == 7u);
    CHECK(g_targets[0] == 12.0f);
    r.step();
    CHECK(g_answers.empty());
    g_found[1] = {{4, 50, 0, 0, {}}, {8, 51, 0, 2, {}}, {12, 51, 0, 5, {}}};
    r.step();
    REQUIRE(g_answers.size() == 1u);
    CHECK(g_answers[0] == std::to_string(id) + ":1:3:1:5");  // found, 3 points, reaches, the last one a door
    CHECK(g_destroyed == std::vector<uint32_t>{1});
}

TEST_CASE("path glue: an owner that is gone ends the query cancelled; a fault in a game call turns the feature off") {
    Rig r;
    const uint32_t a = path_service()->request(query(12), 9, on_path, nullptr, 1000, nullptr, nullptr);  // 9 has no transform
    r.step();
    REQUIRE(g_answers.size() == 1u);
    CHECK(g_answers[0] == std::to_string(a) + ":7:0:0");  // EMF_PATH_CANCELLED
    path_service()->request(query(12), 7, on_path, nullptr, 1000, nullptr, nullptr);
    r.step();
    g_get_faults = true;
    r.step();
    CHECK(path_service() == nullptr);
}

TEST_CASE("api path_find (v37): queued on a server world; refused on a client world, with no owner, or no query") {
    Rig r;
    CoreState state;
    state.log_line = [](const std::string&) {};
    set_core_state(&state);
    FakeWorld cw;
    cw.put<uint8_t>(off::kKind, 2);
    EmfWorld* server = reinterpret_cast<EmfWorld*>(r.world.p());
    EmfWorld* client = reinterpret_cast<EmfWorld*>(cw.p());
    EmfPathQuery q = query(12);
    q.owner = 7;
    const uint32_t id = api_table()->path_find(server, &q, on_path, nullptr);
    CHECK(id != 0u);
    CHECK(api_table()->path_find(client, &q, on_path, nullptr) == 0u);  // joined games have no path grid
    CHECK(api_table()->path_find(server, nullptr, on_path, nullptr) == 0u);
    q.owner = 0;  // the first player: none in this fake world
    CHECK(api_table()->path_find(server, &q, on_path, nullptr) == 0u);
    CHECK(api_table()->path_cancel(id));
    CHECK_FALSE(api_table()->path_cancel(id + 1000));
    set_core_state(nullptr);
}

TEST_CASE("path glue (v48): a creature's profile reaches the game's query") {
    Rig r;
    PathCreature c;
    c.forbidden = 388;
    c.up = 4;
    c.cost[3] = 4.0f;
    REQUIRE(path_service()->request(query(12), 7, on_path, nullptr, 1000, nullptr, &c) != 0u);
    REQUIRE(path_service()->request(query(12), 7, on_path, nullptr, 1000, nullptr, nullptr) != 0u);
    r.step();
    REQUIRE(g_query_bytes.size() == 2u);
    auto u32 = [](const std::vector<uint8_t>& b, size_t o) { uint32_t v; std::memcpy(&v, b.data() + o, 4); return v; };
    auto f32 = [](const std::vector<uint8_t>& b, size_t o) { float v; std::memcpy(&v, b.data() + o, 4); return v; };
    CHECK(u32(g_query_bytes[0], 0x60) == (388u | (1u << 19)));
    CHECK(u32(g_query_bytes[0], 0x104) == 4u);
    CHECK(u32(g_query_bytes[0], 0x10c) == 4u);
    CHECK(f32(g_query_bytes[0], 0x6c + 3 * 4) == 4.0f);
    CHECK(u32(g_query_bytes[1], 0x60) == (1u << 19));  // the plain query: the game's defaults (here 0) and EMF's rule
    CHECK(u32(g_query_bytes[1], 0x104) == 0u);
}

namespace {
// Runs fn as a server world callback (as_entity is refused anywhere else).
struct InCallback {
    std::function<void(EmfWorld*)> fn;
};
void run_in_callback(EmfWorld* w, std::function<void(EmfWorld*)> fn) {
    Dispatcher d;
    InCallback c{std::move(fn)};
    d.post(EMF_WORLD_SERVER, [](EmfWorld* world, void* u) { static_cast<InCallback*>(u)->fn(world); }, &c);
    d.run(w, EMF_WORLD_SERVER, [](const std::string&) {});
}
bool g_has_finder = false, g_has_walker = false;
void* as_gc(void* out, void*, uint32_t hash, uint32_t entity) {
    static uint8_t finder[272];
    auto o = static_cast<uint64_t*>(out);
    o[0] = 0;
    o[1] = 0;
    if (hash == emf::gen::ecs::DynamicPathFinder::kHash && entity == 7 && g_has_finder) {
        o[0] = reinterpret_cast<uint64_t>(finder);
        o[1] = sizeof finder;
    }
    return out;
}
void* as_tpl(uint8_t*, uint32_t hash, uint32_t entity) {
    static emf::gen::ecs::PathWalker walker{};
    return hash == emf::gen::ecs::PathWalker::kHash && entity == 7 && g_has_walker ? &walker : nullptr;
}
}  // namespace

TEST_CASE("api path_find (v48): as_entity only from a world callback on the server world; its refusals reach the log") {
    Rig r;
    CoreState state;
    std::vector<std::string> lines;
    state.log_line = [&](const std::string& l) { lines.push_back(l); };
    state.get_component = as_gc;
    state.get_template_component = as_tpl;
    set_core_state(&state);
    g_has_finder = g_has_walker = false;
    auto said = [&](const std::string& part) {
        return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(part) != std::string::npos; });
    };
    EmfWorld* server = reinterpret_cast<EmfWorld*>(r.world.p());
    EmfPathQuery q = query(12);
    q.as_entity = 7;
    CHECK(api_table()->path_find(server, &q, on_path, nullptr) == 0u);  // not inside a world callback
    CHECK(said("path_find refused: as_entity: call it from a world callback (on_tick, run_on_world, on_event) on the "
               "server world passed to it"));
    uint32_t id = 1;
    run_in_callback(server, [&](EmfWorld* w) { id = api_table()->path_find(w, &q, on_path, nullptr); });
    CHECK(id == 0u);
    CHECK(said("path_find refused: as_entity 7 has no DynamicPathFinder"));
    g_has_finder = true;
    run_in_callback(server, [&](EmfWorld* w) { id = api_table()->path_find(w, &q, on_path, nullptr); });
    CHECK(id == 0u);
    CHECK(said("path_find refused: as_entity 7 has no PathWalker template"));
    state.get_template_component = nullptr;  // a build where template data cannot be read
    run_in_callback(server, [&](EmfWorld* w) { id = api_table()->path_find(w, &q, on_path, nullptr); });
    CHECK(id == 0u);
    CHECK(said("path_find refused: as_entity 7: template data cannot be read on this game build"));
    q.as_entity = 0;  // without it, any thread as before
    q.owner = 7;
    CHECK(api_table()->path_find(server, &q, on_path, nullptr) != 0u);
    set_core_state(nullptr);
}

TEST_CASE("api table (v37): path_find and path_cancel are there") {
    const EmfApi* api = api_table();
    CHECK(api->version >= 37u);  // the newest version is checked by its own test (test_doors.cpp: v38)
    CHECK(api->size >= kApiV37Size);
    CHECK(api->path_find != nullptr);
    CHECK(api->path_cancel != nullptr);
}

TEST_CASE("path glue: when the emf_path program has not run for 5 s, open queries are answered (cancelled)") {
    Rig r;
    const uint32_t id = path_service()->request(query(12), 7, on_path, nullptr, 1000, nullptr, nullptr);
    path_server_tick(2000);  // the program never ran in this world: give it time
    CHECK(g_answers.empty());
    for (uint64_t t = 0; t + 2 < kPathWatchdogSteps; ++t) path_server_tick(2000 + kPathWatchdogMs + t);
    CHECK(g_answers.empty());  // 5 s, but not yet 150 steps
    path_server_tick(2000 + kPathWatchdogMs + kPathWatchdogSteps);
    REQUIRE(g_answers.size() == 1u);
    CHECK(g_answers[0] == std::to_string(id) + ":7:0:0");
    r.step();  // running again: nothing left
    CHECK(g_answers.size() == 1u);
}

TEST_CASE("path glue: path_stop turns the feature off (the program could not be added)") {
    Rig r;
    REQUIRE(path_service() != nullptr);
    path_stop();
    CHECK(path_service() == nullptr);
}

namespace {
void on_path_faults(EmfWorld*, uint32_t, const EmfPathResult*, const EmfPathPoint*, void*) {
    *static_cast<volatile int*>(nullptr) = 1;  // a mod's bad callback
}
bool g_create_faults = false;
int g_create_calls = 0;
uint32_t* f_create_maybe_fault(void* cb, uint32_t* id, const void* q) {
    ++g_create_calls;
    if (g_create_faults) *static_cast<volatile int*>(nullptr) = 1;
    return f_create(cb, id, q);
}
}  // namespace

TEST_CASE("path glue (review I1): a callback that faults does not cost the other answers of that step") {
    Rig r;
    path_service()->request(query(12), 7, on_path_faults, nullptr, 1000, nullptr, nullptr);
    const uint32_t b = path_service()->request(query(14), 7, on_path, nullptr, 1000, nullptr, nullptr);
    r.step();
    g_found[1] = {{12, 50, 0, 0, {}}};
    g_found[2] = {{14, 50, 0, 0, {}}};
    r.step();
    REQUIRE(g_answers.size() == 1u);
    CHECK(g_answers[0].rfind(std::to_string(b) + ":1:", 0) == 0);
}

TEST_CASE("path glue (review I2): a fault in a game call stops that step; the open queries are answered at once") {
    Rig r;
    PathGlueDeps d;  // the Rig's deps with a create that faults
    d.fns.create = f_create_maybe_fault;
    d.fns.destroy = f_destroy;
    d.fns.get = f_get;
    d.fns.defaults = f_defaults;
    d.component = [](uint8_t*, uint32_t, uint32_t) -> const void* { static uint8_t xf[56]; return xf; };
    d.after_step = [](std::function<void(EmfWorld*)> fn) { fn(nullptr); };
    d.now_ms = [] { return uint64_t(1000); };
    r.spec = path_setup(d);
    for (int i = 0; i < 3; ++i) path_service()->request(query(12), 7, on_path, nullptr, 1000, nullptr, nullptr);
    g_create_faults = true;
    g_create_calls = 0;
    r.step();
    g_create_faults = false;
    CHECK(g_create_calls == 1);       // no second create after the fault
    CHECK(g_answers.size() == 3u);    // all three answered in that step
    CHECK(path_service() == nullptr);
}

TEST_CASE("path glue (review): one long step is not a stalled program; steps without a run are") {
    Rig r;
    path_service()->request(query(12), 7, on_path, nullptr, 1000, nullptr, nullptr);
    r.step();                                  // ran (the clock says 1000)
    path_server_tick(1000 + kPathWatchdogMs + 5000);  // one step that took long
    CHECK(g_answers.empty());
    for (uint64_t t = 0; t < kPathWatchdogSteps; ++t) path_server_tick(30000 + t);  // the program no longer runs
    CHECK(g_answers.size() == 1u);
}

TEST_CASE("path glue (review M1): no command buffer: the program does not count as running") {
    Rig r;
    std::memset(r.ctx.job + 0x08, 0, 8);
    path_service()->request(query(12), 7, on_path, nullptr, 1000, nullptr, nullptr);
    for (uint64_t t = 0; t <= kPathWatchdogSteps; ++t) {  // the program is called every step, but cannot work
        r.step();
        path_server_tick(t ? 1000 + kPathWatchdogMs + t : 1000);
    }
    CHECK(g_answers.size() == 1u);
}

TEST_CASE("api path_find (review M5): a larger query (a later version's) is read up to what v37 knows") {
    Rig r;
    CoreState state;
    state.log_line = [](const std::string&) {};
    set_core_state(&state);
    struct Bigger {
        EmfPathQuery q;
        uint32_t later_field;
    } big{};
    big.q = query(12);
    big.q.owner = 7;
    big.q.size = sizeof big;
    big.later_field = 99;
    CHECK(api_table()->path_find(reinterpret_cast<EmfWorld*>(r.world.p()), &big.q, on_path, nullptr) != 0u);
    set_core_state(nullptr);
}

namespace {
int g_destroy_refusals = 0;  // how many destroys the fake game refuses (a full command buffer)
bool f_destroy_maybe_full(void* cb, uint32_t entity, uint32_t id) {
    if (g_destroy_refusals > 0) {
        --g_destroy_refusals;
        return false;
    }
    return f_destroy(cb, entity, id);
}
}  // namespace

TEST_CASE("path glue (review M3): a destroy the game refused is tried again next step") {
    Rig r;
    PathGlueDeps d;
    d.fns.create = f_create;
    d.fns.destroy = f_destroy_maybe_full;
    d.fns.get = f_get;
    d.fns.defaults = f_defaults;
    d.component = [](uint8_t*, uint32_t, uint32_t) -> const void* { static uint8_t xf[56]; return xf; };
    d.after_step = [](std::function<void(EmfWorld*)> fn) { fn(nullptr); };
    d.now_ms = [] { return uint64_t(1000); };
    r.spec = path_setup(d);
    path_service()->request(query(12), 7, on_path, nullptr, 1000, nullptr, nullptr);
    r.step();
    g_found[1] = {{12, 50, 0, 0, {}}};
    g_destroy_refusals = 1;
    r.step();
    CHECK(g_answers.size() == 1u);
    CHECK(g_destroyed.empty());  // refused
    r.step();
    CHECK(g_destroyed == std::vector<uint32_t>{1});
}

TEST_CASE("path glue (review M3): queries the watchdog ends are destroyed in the game when the program runs again") {
    Rig r;
    path_service()->request(query(12), 7, on_path, nullptr, 1000, nullptr, nullptr);
    r.step();  // created (game id 1)
    REQUIRE(g_owners.size() == 1u);
    path_server_tick(1000);
    for (uint64_t t = 0; t < kPathWatchdogSteps; ++t) path_server_tick(1000 + kPathWatchdogMs + t);
    CHECK(g_answers.size() == 1u);
    CHECK(g_destroyed.empty());
    r.step();  // running again
    CHECK(g_destroyed == std::vector<uint32_t>{1});
}

TEST_CASE("path glue (review M3): a new world forgets the old world's game records (nothing to destroy there)") {
    Rig r;
    path_service()->request(query(12), 7, on_path, nullptr, 1000, nullptr, nullptr);
    r.step();
    path_world_reset();
    r.step();
    CHECK(g_destroyed.empty());
}

// 0.6.11 (a modder's raids): next to the points the game's answer holds the door entity of each point (out +0x10 pointer,
// +0x18 count; the path builder writes it for door points, path_walking reads it). EMF hands it on as
// EmfPathResult.doors: NULL when no point is a door, or when the game's count is not the points' count.
namespace {
std::vector<uint32_t> g_cb_doors;
bool g_cb_doors_null = true;
void on_path_doors(EmfWorld*, uint32_t, const EmfPathResult* r, const EmfPathPoint*, void*) {
    g_cb_doors.clear();
    g_cb_doors_null = r->size < offsetof(EmfPathResult, doors) + sizeof(r->doors) || !r->doors;
    if (!g_cb_doors_null) g_cb_doors.assign(r->doors, r->doors + r->count);
}
}  // namespace

TEST_CASE("path glue (0.6.11): the door entity of each point comes with the answer (EmfPathResult.doors)") {
    Rig r;
    REQUIRE(path_service()->request(query(12), 7, on_path_doors, nullptr, 1000, nullptr, nullptr) != 0u);
    r.step();
    g_found[1] = {{4, 50, 0, 0, {}}, {8, 50, 0, 5, {}}, {12, 50, 0, 0, {}}};
    g_door_ids[1] = {0, 812, 0};
    r.step();
    CHECK_FALSE(g_cb_doors_null);
    CHECK(g_cb_doors == std::vector<uint32_t>{0, 812, 0});

    REQUIRE(path_service()->request(query(12), 7, on_path_doors, nullptr, 1000, nullptr, nullptr) != 0u);
    r.step();
    g_found[2] = {{4, 50, 0, 0, {}}, {12, 50, 0, 0, {}}};
    g_door_ids[2] = {0, 0};  // no door point
    r.step();
    CHECK(g_cb_doors_null);

    REQUIRE(path_service()->request(query(12), 7, on_path_doors, nullptr, 1000, nullptr, nullptr) != 0u);
    r.step();
    g_found[3] = {{4, 50, 0, 5, {}}, {12, 50, 0, 0, {}}};
    g_door_ids[3] = {812};  // not one per point: not trusted
    r.step();
    CHECK(g_cb_doors_null);
}

// path.door (0.6.11): the console asks; emf_path reads the game's path grid (*(job + 0x28)) at its next run and the
// reply comes after that step, one or two lines.
namespace {
std::vector<std::string> g_door_lines;
std::function<void(const std::string&)> door_reply() {
    return [](const std::string& l) { g_door_lines.push_back(l); };
}
void set_grid(Rig& r, const uint8_t* grid) { std::memcpy(r.ctx.job + 0x28, &grid, 8); }
}  // namespace

TEST_CASE("path glue (path.door): asked, answered after the program's next run from the grid at job + 0x28") {
    g_door_lines.clear();
    Rig r(true);
    emf::test::FakeGrid g;
    g.standard_door(2);
    g.node(4, 2, 5, 0x98, 0x00);
    set_grid(r, g.grid.data());
    CHECK(path_door_ask(77, door_reply()) == "");
    CHECK(path_door_ask(5, door_reply()) == "");
    CHECK(g_door_lines.empty());  // nothing before the step
    r.step();
    REQUIRE(g_door_lines.size() == 3u);  // two lines for the door, one for the entity that is not one
    CHECK(g_door_lines[0].find("path.door 77: in the path grid's door table, closed (kind 2)") == 0);
    CHECK(g_door_lines[1].find("at the row's centre: node (") == 0);
    CHECK(g_door_lines[1].find("ClosedDoor (19)") != std::string::npos);
    CHECK(g_door_lines[2] == "path.door 5: not in the path grid's door table (2 doors registered)");
    r.step();
    CHECK(g_door_lines.size() == 3u);  // answered once
}

TEST_CASE("path glue (path.door): no grid in this storage; not available without the guards or with path_find off") {
    g_door_lines.clear();
    {
        Rig r(true);  // the job's grid stays 0 (not the server storage)
        CHECK(path_door_ask(77, door_reply()) == "");
        r.step();
        REQUIRE(g_door_lines.size() == 1u);
        CHECK(g_door_lines[0] == "path.door 77: no path grid in this world");
    }
    {
        Rig r(false);  // a PathGrid guard signature is not usable on this build
        CHECK(path_door_ask(77, door_reply()) == "path.door: not available on this game build");
    }
    {
        Rig r(true);
        path_stop();
        CHECK(path_door_ask(77, door_reply()) == "path.door: not available (path_find is off; see the log)");
    }
}

TEST_CASE("path glue (path.door): at most 16 wait; the watchdog answers them when emf_path does not run") {
    g_door_lines.clear();
    Rig r(true);
    for (uint32_t i = 0; i < 16; ++i) CHECK(path_door_ask(100 + i, door_reply()) == "");
    CHECK(path_door_ask(200, door_reply()) == "path.door: 16 are waiting for the path grid (try again in a moment)");
    path_server_tick(1000);
    for (uint64_t t = 0; t < kPathWatchdogSteps; ++t) path_server_tick(1000 + kPathWatchdogMs + t);
    REQUIRE(g_door_lines.size() == 16u);
    CHECK(g_door_lines[0] == "path.door 100: not answered (the emf_path program is not running in this world)");
    CHECK(path_door_ask(200, door_reply()) == "");  // room again
}

TEST_CASE("path glue (0.6.11 review M1): a door id on a point that is not a door point is not handed on") {
    Rig r;
    REQUIRE(path_service()->request(query(12), 7, on_path_doors, nullptr, 1000, nullptr, nullptr) != 0u);
    r.step();
    g_found[1] = {{4, 50, 0, 0, {}}, {8, 50, 0, 5, {}}, {12, 50, 0, 2, {}}};
    g_door_ids[1] = {33, 812, 34};  // an old array the game did not clear: only the door point counts
    r.step();
    CHECK(g_cb_doors == std::vector<uint32_t>{0, 812, 0});
}

TEST_CASE("path glue (0.6.11 review M2): path.door asks waiting when the world ends are answered, not read in the next world") {
    g_door_lines.clear();
    Rig r(true);
    CHECK(path_door_ask(77, door_reply()) == "");
    path_world_reset();
    REQUIRE(g_door_lines.size() == 1u);
    CHECK(g_door_lines[0] == "path.door 77: not answered (the world ended)");
    emf::test::FakeGrid g;
    g.standard_door(2);
    set_grid(r, g.grid.data());
    r.step();
    CHECK(g_door_lines.size() == 1u);  // nothing left for the next world's grid
}
