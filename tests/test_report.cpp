#include <doctest.h>

#include <string>
#include <vector>

#include "fake_world.h"
#include "probe/report.h"

using namespace emf;
using namespace emf::probe;
using emf::test::FakeWorld;
using emf::test::MapStore;
using emf::test::SingletonStore;

namespace {
struct Lines {
    std::vector<std::string> v;
    LineSink sink() {
        return [this](const std::string& s) { v.push_back(s); };
    }
    bool has(const std::string& needle) const {
        for (auto& s : v)
            if (s.find(needle) != std::string::npos) return true;
        return false;
    }
    int count(const std::string& needle) const {
        int n = 0;
        for (auto& s : v) n += s.find(needle) != std::string::npos;
        return n;
    }
};

// Player transform: x = 2 m, y = 0, z = -1 m (32.32 fixed point).
int64_t g_player_pos[7] = {int64_t(2) << 32, 0, -(int64_t(1) << 32), 0, 0, 0, 0};
// DynamicActiveNpcState: state 7 (Roam), previous 3 (Idle), decision 1 (WantsToRoam).
uint8_t g_npc_state[136] = {7, 3, 1};

void* fake_gc(void* out, void*, uint32_t hash, uint32_t entity) {
    auto o = static_cast<uint64_t*>(out);
    o[0] = o[1] = 0;
    if (hash == off::kHashCurrentTransform && entity == 42) {
        o[0] = reinterpret_cast<uint64_t>(g_player_pos);
        o[1] = 56;
    }
    if (hash == off::kHashActiveNpcState && entity == 6) {
        o[0] = reinterpret_cast<uint64_t>(g_npc_state);
        o[1] = 136;
    }
    return out;
}

void make_running(FakeWorld& w, uint8_t kind, uint32_t tick) {
    w.put<uint8_t>(off::kState, 2);
    w.put<uint8_t>(off::kKind, kind);
    w.put<uint32_t>(off::kTick, tick);
}
}  // namespace

TEST_CASE("fixed_to_meters converts 32.32 fixed point") {
    CHECK(fixed_to_meters(int64_t(1) << 32) == doctest::Approx(1.0));
    CHECK(fixed_to_meters(-(int64_t(3) << 31)) == doctest::Approx(-1.5));
    CHECK(fixed_to_meters(0) == 0.0);
}

TEST_CASE("Reporter ignores a world that is not running yet") {
    FakeWorld w;
    w.put<uint8_t>(off::kKind, 2);  // state stays 0
    Reporter r(fake_gc);
    Lines l;
    r.on_tick(WorldView(w.p()), 10000, l.sink());
    CHECK(l.v.empty());
}

TEST_CASE("Reporter logs the client world and the local player's position") {
    FakeWorld w;
    make_running(w, 2, 4551);
    uint8_t lpd[8] = {0, 0, 0, 0, 42, 0, 0, 0};  // LocalPlayerData+4 = 42
    SingletonStore ss;
    ss.plant(w, "LocalPlayerData", lpd);
    Reporter r(fake_gc);
    Lines l;
    r.on_tick(WorldView(w.p()), 10000, l.sink());
    CHECK(l.has("world kind=c tick=4551"));
    CHECK(l.has("player id=42 pos=(2.00, 0.00, -1.00)"));
}

TEST_CASE("Reporter reports at most once per interval per world") {
    FakeWorld w;
    make_running(w, 2, 1);
    Reporter r(fake_gc, 2000);
    Lines l;
    r.on_tick(WorldView(w.p()), 10000, l.sink());
    size_t after_first = l.v.size();
    REQUIRE(after_first > 0);
    r.on_tick(WorldView(w.p()), 11000, l.sink());
    CHECK(l.v.size() == after_first);
    r.on_tick(WorldView(w.p()), 12000, l.sink());
    CHECK(l.v.size() > after_first);
}

TEST_CASE("Reporter lists villagers on the server world with the component size") {
    FakeWorld w;
    make_running(w, 0, 77);
    MapStore ms;
    emf::test::plant_entities(w, ms, {5, 6});
    Reporter r(fake_gc);
    Lines l;
    r.on_tick(WorldView(w.p()), 10000, l.sink());
    CHECK(l.has("world kind=s tick=77 entities=2"));
    CHECK(l.has("npc id=6 size=136 state=7 prev=3 decision=1"));
    CHECK(l.has("villagers: 1"));
}

TEST_CASE("Reporter warns once when only a client world ticks") {
    FakeWorld w;
    make_running(w, 2, 1);
    Reporter r(fake_gc);
    Lines l;
    for (uint64_t t = 0; t <= 30000; t += 500) r.on_tick(WorldView(w.p()), 100000 + t, l.sink());
    CHECK(l.count("no server world") == 1);
}

TEST_CASE("Reporter does not warn when a server world ticks too") {
    FakeWorld client, server;
    make_running(client, 2, 1);
    make_running(server, 0, 1);
    Reporter r(fake_gc);
    Lines l;
    for (uint64_t t = 0; t <= 30000; t += 500) {
        r.on_tick(WorldView(server.p()), 100000 + t, l.sink());
        r.on_tick(WorldView(client.p()), 100000 + t, l.sink());
    }
    CHECK(l.count("no server world") == 0);
}

TEST_CASE("npc_ctx_info reads the entity id and current state from the villager context") {
    // entity list: 16-byte entries; entity index is 1-based
    uint32_t entity_list[8] = {111, 0, 0, 0, 222, 0, 0, 0};
    alignas(8) uint8_t q[0x830] = {};
    const void* list_ptr = entity_list;
    std::memcpy(q + 0x828, &list_ptr, 8);
    alignas(8) uint8_t pc[16] = {};
    const void* q_ptr = q;
    std::memcpy(pc, &q_ptr, 8);
    uint32_t index = 2;
    std::memcpy(pc + 8, &index, 4);
    uint8_t dyn[136] = {9};  // state Sitting
    alignas(8) uint8_t ctx[0x1B8] = {};
    const void* pc_ptr = pc;
    const void* dyn_ptr = dyn;
    std::memcpy(ctx, &pc_ptr, 8);
    std::memcpy(ctx + 0x1B0, &dyn_ptr, 8);

    uint32_t id = 0;
    uint8_t state = 0;
    REQUIRE(npc_ctx_info(ctx, &id, &state));
    CHECK(id == 222);
    CHECK(state == 9);
    CHECK_FALSE(npc_ctx_info(nullptr, &id, &state));
}
