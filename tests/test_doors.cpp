// Doors (API v38): the emf_doors program opens, closes and locks doors through the game's own writeOther (fakes).
#include <doctest.h>

#include <array>
#include <cstring>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "common/offsets.h"
#include "core/api.h"
#include "core/api_sizes.h"
#include "core/doors.h"
#include "core/system_programs.h"
#include "emf/generated/components.hpp"
#include "fake_world.h"

using namespace emf;
using namespace emf::core;
using emf::test::FakeProgramCtx;
using emf::test::FakeWorld;

namespace {
namespace gen = emf::gen::ecs;

constexpr uint32_t kDoor = 500, kSwitched = 501, kChest = 502, kGone = 503;
uint64_t acc(size_t i) { return 0xD0 + i; }

struct DoorBits {
    int64_t xf[7] = {};  // CurrentTransform: 32.32 fixed-point metres first
    gen::ServerToggleState toggle{};
    gen::DirectionalInteraction side{};
    gen::InteractionOfferRuntime offer{};
    gen::LogicGraphNodeConnected logic{};
};
std::map<uint32_t, DoorBits> g_doors;
std::map<std::pair<uint32_t, uint32_t>, void*> g_by_hash;  // (hash, entity)
std::map<uint64_t, uint32_t> g_acc_hash;                    // accessor -> component hash
int g_writes = 0;
alignas(8) uint8_t g_change_set[8];

void* f_read_other(void** out, void*, uint64_t a, uint32_t e) {
    const auto h = g_acc_hash.find(a);
    const auto it = h == g_acc_hash.end() ? g_by_hash.end() : g_by_hash.find({h->second, e});
    *out = it == g_by_hash.end() ? nullptr : it->second;
    return out;
}
void* f_write_other(void** out, void*, void*, uint64_t a, uint32_t e, uint8_t) {
    ++g_writes;
    return f_read_other(out, nullptr, a, e);
}
void* f_get_component(void* out, void*, uint32_t hash, uint32_t e) {
    auto o = static_cast<uint64_t*>(out);
    o[0] = o[1] = 0;
    const auto it = g_by_hash.find({hash, e});
    if (it != g_by_hash.end()) {
        o[0] = reinterpret_cast<uint64_t>(it->second);
        o[1] = hash == gen::ServerToggleState::kHash          ? 24
               : hash == gen::DirectionalInteraction::kHash   ? 160
               : hash == gen::InteractionOfferRuntime::kHash  ? 56
               : hash == gen::CurrentTransform::kHash         ? 56
                                                               : 8;
    }
    return out;
}
std::vector<std::string> g_write_names;  // the program's write list, in iterator order
void f_iter_begin(void* ctx, void* iter, uint32_t) {
    auto* it = static_cast<uint64_t*>(iter);
    it[0] = reinterpret_cast<uint64_t>(ctx);
    for (size_t i = 0; i < 3; ++i) it[1 + i] = acc(i);
}
uint64_t f_register(void*, const void*, char) { return 1; }

void add_door(uint32_t e, bool open = false, uint32_t writer_graph = 0) {
    DoorBits& d = g_doors[e];
    d.toggle.state = open;
    d.side.state = static_cast<decltype(d.side.state)>(open ? 2 : 0);
    d.logic.writerGraphId.id.value = writer_graph;
    d.xf[0] = int64_t(e) << 32;  // the door stands at (e, 20, 30) m
    d.xf[1] = int64_t(20) << 32;
    d.xf[2] = int64_t(30) << 32;
    g_by_hash[{gen::CurrentTransform::kHash, e}] = d.xf;
    g_by_hash[{gen::ServerToggleState::kHash, e}] = &d.toggle;
    g_by_hash[{gen::DirectionalInteraction::kHash, e}] = &d.side;
    g_by_hash[{gen::InteractionOfferRuntime::kHash, e}] = &d.offer;
    if (writer_graph) g_by_hash[{gen::LogicGraphNodeConnected::kHash, e}] = &d.logic;
}

struct Rig {
    FakeWorld w;
    SystemPrograms programs;
    DoorService service;
    FakeProgramCtx ctx;
    int user = 0;
    Rig() : service(deps()), ctx(w.p()) {
        programs.reset();
        g_doors.clear();
        g_by_hash.clear();
        g_acc_hash.clear();
        g_writes = 0;
        w.put<uint8_t>(off::kKind, 0);
        void* wctx = &user;
        std::memcpy(ctx.job + off::kJobWorldCtx, &wctx, 8);
        void* cs = g_change_set;
        std::memcpy(ctx.job + off::kJobChangeSet, &cs, 8);
        int64_t tick_ns = 123456789;
        std::memcpy(ctx.job + off::kJobTick, &tick_ns, 8);
        REQUIRE(programs.add(service.spec(), /*internal=*/true));
        REQUIRE(programs.register_into(w.p(), f_register) == 1);
        set_program_iter_begin(f_iter_begin);
        const ProgramSpec s = service.spec();
        for (size_t i = 0; i < s.other_write.size(); ++i) {
            const std::string& n = s.other_write[i];
            g_acc_hash[acc(i)] = n == "ServerToggleState"        ? gen::ServerToggleState::kHash
                                 : n == "DirectionalInteraction" ? gen::DirectionalInteraction::kHash
                                                                 : gen::InteractionOfferRuntime::kHash;
        }
        add_door(kDoor);
        add_door(kSwitched, false, 77);
        g_by_hash[{gen::InteractionOfferRuntime::kHash, kChest}] = &g_doors[kChest].offer;  // a chest: no toggle
    }
    ~Rig() {
        set_program_iter_begin(nullptr);
        programs.reset();
    }
    std::vector<std::string> saved;  // what the world file got
    bool save_ok = true;
    int list_calls = 0;
    bool doors_there = true;  // list_doors sees the doors
    std::vector<std::array<double, 3>> players;  // where the players stand
    DoorService::Deps deps() {
        DoorService::Deps d;
        d.read_other = f_read_other;
        d.write_other = f_write_other;
        d.get_component = f_get_component;
        d.log = [](const std::string&) {};
        d.save = [this](const std::string& text) {
            if (!save_ok) return false;
            saved.push_back(text);
            return true;
        };
        d.list_doors = [this](uint8_t*) {
            ++list_calls;
            std::vector<DoorSpot> out;
            if (!doors_there) return out;
            for (const auto& [e, bits] : g_doors)
                if (g_by_hash.count({gen::ServerToggleState::kHash, e}))
                    out.push_back({e, {double(bits.xf[0]) / 4294967296.0, double(bits.xf[1]) / 4294967296.0,
                                       double(bits.xf[2]) / 4294967296.0}});
            return out;
        };
        d.players = [this](uint8_t*) { return players; };
        return d;
    }
    uint8_t* world() { return w.p(); }
    void step() { system_program_desc(0).update(ctx.p()); }
    bool order(uint32_t e, DoorOrderKind k, uint8_t side = 0, std::string* why = nullptr) {
        std::string w2;
        return service.request(world(), e, k, side, why ? why : &w2);
    }
};
}  // namespace

TEST_CASE("doors: emf_doors writes the three door components through the game's accessors") {
    Rig r;
    const ProgramSpec s = r.service.spec();
    CHECK(s.name == std::string(kDoorsProgramName));
    CHECK(s.world_kind == EMF_WORLD_SERVER);
    CHECK(s.other_write.size() == 3u);
}

TEST_CASE("doors: the state is closed, open or not a door; a switch-driven door says so") {
    Rig r;
    CHECK(r.service.state(r.world(), kDoor) == EMF_DOOR_CLOSED);
    g_doors[kDoor].toggle.state = true;
    CHECK(r.service.state(r.world(), kDoor) == EMF_DOOR_OPEN);
    CHECK(r.service.state(r.world(), kChest) == EMF_DOOR_NONE);
    CHECK(r.service.state(r.world(), 999) == EMF_DOOR_NONE);
    CHECK(r.service.state(r.world(), kSwitched) == (EMF_DOOR_CLOSED | EMF_DOOR_LOGIC));
}

TEST_CASE("doors: open writes both states at the next step, through writeOther; close puts them back") {
    Rig r;
    REQUIRE(r.order(kDoor, DoorOrderKind::Open));
    CHECK_FALSE(g_doors[kDoor].toggle.state);  // nothing until the step
    r.step();
    CHECK(g_doors[kDoor].toggle.state);
    CHECK(static_cast<int>(g_doors[kDoor].side.state) == 2);  // back (the game's load default)
    CHECK(g_doors[kDoor].toggle.lastChange.value != 0);
    CHECK(g_writes >= 2);
    REQUIRE(r.order(kDoor, DoorOrderKind::Close));
    r.step();
    CHECK_FALSE(g_doors[kDoor].toggle.state);
    CHECK(static_cast<int>(g_doors[kDoor].side.state) == 0);
    REQUIRE(r.order(kDoor, DoorOrderKind::Open, 1));  // swung to the front
    r.step();
    CHECK(static_cast<int>(g_doors[kDoor].side.state) == 1);
}

TEST_CASE("doors: refused at once: not a door, driven by a switch, already so, a bad side") {
    Rig r;
    std::string why;
    CHECK_FALSE(r.order(kChest, DoorOrderKind::Open, 0, &why));
    CHECK(why.find("not a door") != std::string::npos);
    CHECK_FALSE(r.order(kSwitched, DoorOrderKind::Open, 0, &why));
    CHECK(why.find("switch") != std::string::npos);
    CHECK_FALSE(r.order(kDoor, DoorOrderKind::Close, 0, &why));
    CHECK(why.find("already") != std::string::npos);
    CHECK_FALSE(r.order(kDoor, DoorOrderKind::Open, 3, &why));
    CHECK_FALSE(r.order(kSwitched, DoorOrderKind::Lock, 0, &why));
}

TEST_CASE("doors: a lock keeps isDisabled set every step (the game clearing it is undone); unlock clears it") {
    Rig r;
    REQUIRE(r.order(kDoor, DoorOrderKind::Lock));
    r.step();
    CHECK(g_doors[kDoor].offer.isDisabled);
    CHECK(r.service.state(r.world(), kDoor) == (EMF_DOOR_CLOSED | EMF_DOOR_LOCKED));
    g_doors[kDoor].offer.isDisabled = false;  // the game cleared it
    r.step();
    CHECK(g_doors[kDoor].offer.isDisabled);
    const int writes = g_writes;
    r.step();
    CHECK(g_writes == writes);  // already set: no write
    REQUIRE(r.order(kDoor, DoorOrderKind::Unlock));
    r.step();
    CHECK_FALSE(g_doors[kDoor].offer.isDisabled);
    CHECK(r.service.state(r.world(), kDoor) == EMF_DOOR_CLOSED);
}

TEST_CASE("doors: a door that is gone loses its lock; a new world drops orders and locks") {
    Rig r;
    add_door(kGone);
    REQUIRE(r.order(kGone, DoorOrderKind::Lock));
    r.step();
    CHECK(r.service.locked_count() == 1u);
    g_by_hash.erase({emf::gen::ecs::ServerToggleState::kHash, kGone});
    g_by_hash.erase({emf::gen::ecs::DirectionalInteraction::kHash, kGone});
    g_by_hash.erase({emf::gen::ecs::InteractionOfferRuntime::kHash, kGone});
    r.step();
    CHECK(r.service.locked_count() == 0u);
    REQUIRE(r.order(kDoor, DoorOrderKind::Lock));
    REQUIRE(r.order(kDoor, DoorOrderKind::Open));
    r.service.world_reset();
    r.step();
    CHECK_FALSE(g_doors[kDoor].toggle.state);
    CHECK_FALSE(g_doors[kDoor].offer.isDisabled);
}

TEST_CASE("api doors (v38): door_state reads; door_set and door_lock queue for the next step; refusals say no") {
    Rig r;
    CoreState state;
    state.log_line = [](const std::string&) {};
    state.doors = &r.service;
    set_core_state(&state);
    EmfWorld* w = reinterpret_cast<EmfWorld*>(r.world());
    const EmfApi* api = api_table();
    CHECK(api->version >= 38u);
    CHECK(api->door_state(w, kDoor) == EMF_DOOR_CLOSED);
    CHECK(api->door_state(w, kChest) == EMF_DOOR_NONE);
    CHECK(api->door_set(w, kDoor, 1, 0));
    CHECK_FALSE(api->door_set(w, kChest, 1, 0));
    CHECK_FALSE(api->door_set(w, kDoor, 1, 7));  // a bad side
    CHECK(api->door_lock(w, kDoor, 1));
    r.step();
    CHECK(api->door_state(w, kDoor) == (EMF_DOOR_OPEN | EMF_DOOR_LOCKED));
    CHECK(g_doors[kDoor].offer.isDisabled);
    state.doors = nullptr;  // the feature off on this build
    CHECK(api->door_state(w, kDoor) == EMF_DOOR_NONE);
    CHECK_FALSE(api->door_set(w, kDoor, 0, 0));
    set_core_state(nullptr);
}

TEST_CASE("api table (v38): the three door functions") {
    const EmfApi* api = api_table();
    CHECK(api->version >= 38u);  // the newest version is checked by its own test (test_enemy_orders.cpp: v39)
    CHECK(api->size >= kApiV38Size);
    CHECK(api->door_state != nullptr);
    CHECK(api->door_set != nullptr);
    CHECK(api->door_lock != nullptr);
}

TEST_CASE("doors (review I1): a locked door that a toggle already under way flips is put back; EMF's own order moves it") {
    Rig r;
    REQUIRE(r.order(kDoor, DoorOrderKind::Lock));
    r.step();
    g_doors[kDoor].toggle.state = true;  // a player's toggle that was under way lands after the lock
    g_doors[kDoor].side.state = static_cast<decltype(g_doors[kDoor].side.state)>(2);
    r.step();
    CHECK_FALSE(g_doors[kDoor].toggle.state);
    CHECK(static_cast<int>(g_doors[kDoor].side.state) == 0);
    REQUIRE(r.order(kDoor, DoorOrderKind::Open));  // EMF opens the locked door: that is the state now held
    r.step();
    CHECK(g_doors[kDoor].toggle.state);
    r.step();
    CHECK(g_doors[kDoor].toggle.state);
}

TEST_CASE("doors (review M2): open then close in one callback: both are taken, the door ends closed") {
    Rig r;
    REQUIRE(r.order(kDoor, DoorOrderKind::Open));
    REQUIRE(r.order(kDoor, DoorOrderKind::Close));
    CHECK_FALSE(r.order(kDoor, DoorOrderKind::Close));  // already closed by the waiting order
    r.step();
    CHECK_FALSE(g_doors[kDoor].toggle.state);
}

TEST_CASE("doors (review M4): a locked door that a switch drives now can still be unlocked") {
    Rig r;
    add_door(kGone);
    REQUIRE(r.order(kGone, DoorOrderKind::Lock));
    r.step();
    g_doors[kGone].logic.writerGraphId.id.value = 9;
    g_by_hash[{emf::gen::ecs::LogicGraphNodeConnected::kHash, kGone}] = &g_doors[kGone].logic;
    CHECK(r.order(kGone, DoorOrderKind::Unlock));
    r.step();
    CHECK(r.service.locked_count() == 0u);
}

TEST_CASE("doors (review M6): a failed read does not count as a gone door; the lock stays") {
    Rig r;
    REQUIRE(r.order(kDoor, DoorOrderKind::Lock));
    r.step();
    for (auto& [a, h] : g_acc_hash)
        if (h == emf::gen::ecs::InteractionOfferRuntime::kHash) h = 0;  // the accessor reads nothing now
    r.step();
    CHECK(r.service.locked_count() == 1u);
}

// The owner: a lock should survive a save and a reload (EMF keeps locks per world, by the door's place: entity ids change).
TEST_CASE("door locks: a lock is saved with the world as the door's place; an unlock takes it out") {
    Rig r;
    REQUIRE(r.order(kDoor, DoorOrderKind::Lock));
    r.step();
    r.service.server_tick(r.world(), 1000);
    REQUIRE_FALSE(r.saved.empty());
    CHECK(r.saved.back().find("lock 500.00 20.00 30.00") != std::string::npos);
    REQUIRE(r.order(kDoor, DoorOrderKind::Unlock));
    r.step();
    r.service.server_tick(r.world(), 2000);
    CHECK(r.saved.back().find("lock 500.00") == std::string::npos);
    const size_t n = r.saved.size();
    r.service.server_tick(r.world(), 3000);
    CHECK(r.saved.size() == n);  // nothing changed: nothing saved
}

TEST_CASE("door locks: after a reload the door at a saved place is locked again") {
    Rig r;
    r.service.load_saved("# EMF door locks\r\nlock 500.00 20.00 30.00\r\nlock 9000.00 20.00 30.00\r\n");
    r.service.server_tick(r.world(), 1000);
    r.step();
    CHECK((r.service.state(r.world(), kDoor) & EMF_DOOR_LOCKED) != 0u);
    CHECK(g_doors[kDoor].offer.isDisabled);
    CHECK(r.service.locked_count() == 1u);
    r.service.server_tick(r.world(), 2000);
    REQUIRE_FALSE(r.saved.empty());  // the found one is kept, the one not there yet too
    CHECK(r.saved.back().find("lock 500.00 20.00 30.00") != std::string::npos);
    CHECK(r.saved.back().find("lock 9000.00 20.00 30.00") != std::string::npos);
}

TEST_CASE("door locks: a saved place without its door yet is looked for again later; a world reset drops it") {
    Rig r;
    r.doors_there = false;  // not loaded yet
    r.service.load_saved("lock 500.00 20.00 30.00\n");
    r.service.server_tick(r.world(), 1000);
    CHECK(r.list_calls == 1);
    r.service.server_tick(r.world(), 1500);
    CHECK(r.list_calls == 1);  // not every step
    r.doors_there = true;
    r.service.server_tick(r.world(), 5000);
    CHECK(r.list_calls == 2);
    r.step();
    CHECK(r.service.locked_count() == 1u);
    r.service.world_reset();
    r.service.server_tick(r.world(), 9000);
    CHECK(r.service.locked_count() == 0u);
    CHECK(r.list_calls == 2);
}

TEST_CASE("door locks: after a minute of looks a saved place is still looked for every 10 s (a door that streams in late is locked soon)") {
    Rig r;
    r.doors_there = false;  // not loaded for a long while
    r.service.load_saved("lock 500.00 20.00 30.00\n");
    uint64_t t = 1000;
    for (int i = 0; i < 21; ++i, t += 3000) r.service.server_tick(r.world(), t);  // every 3 s for a minute
    CHECK(r.list_calls == 21);
    const uint64_t last = t - 3000;
    r.service.server_tick(r.world(), last + 9999);
    CHECK(r.list_calls == 21);
    r.doors_there = true;  // the door streams in
    r.service.server_tick(r.world(), last + 10000);
    CHECK(r.list_calls == 22);
    r.step();
    CHECK(r.service.locked_count() == 1u);
}

TEST_CASE("door locks: a save that fails is tried again at the next tick") {
    Rig r;
    r.save_ok = false;
    REQUIRE(r.order(kDoor, DoorOrderKind::Lock));
    r.step();
    r.service.server_tick(r.world(), 1000);
    CHECK(r.saved.empty());
    r.save_ok = true;
    r.service.server_tick(r.world(), 2000);
    CHECK(r.saved.size() == 1u);
}

namespace {
void remove_door(uint32_t e) {  // broken, or streamed out
    g_by_hash.erase({emf::gen::ecs::ServerToggleState::kHash, e});
    g_by_hash.erase({emf::gen::ecs::DirectionalInteraction::kHash, e});
    g_by_hash.erase({emf::gen::ecs::InteractionOfferRuntime::kHash, e});
}
}  // namespace

TEST_CASE("door locks (review I1): an unlock of a door at a saved place not found yet forgets that place") {
    Rig r;
    r.service.load_saved("lock 500.00 20.00 30.00\n");
    REQUIRE(r.order(kDoor, DoorOrderKind::Unlock));
    r.step();
    r.service.server_tick(r.world(), 1000);
    r.step();
    CHECK(r.service.locked_count() == 0u);  // not locked again by the look
    CHECK_FALSE(g_doors[kDoor].offer.isDisabled);
    REQUIRE_FALSE(r.saved.empty());
    CHECK(r.saved.back().find("lock 500.00") == std::string::npos);
}

TEST_CASE("door locks (review I1): a locked door gone with a player near was broken: its place is forgotten") {
    Rig r;
    add_door(kGone);
    REQUIRE(r.order(kGone, DoorOrderKind::Lock));
    r.step();
    r.service.server_tick(r.world(), 1000);
    REQUIRE(r.saved.back().find("lock 503.00") != std::string::npos);
    r.players = {{503, 20, 40}};  // 10 m away
    remove_door(kGone);
    r.step();
    r.service.server_tick(r.world(), 2000);
    CHECK(r.service.locked_count() == 0u);
    CHECK(r.saved.back().find("lock 503.00") == std::string::npos);
    add_door(kGone);  // a new door in the same doorway is not locked
    r.service.server_tick(r.world(), 60000);
    r.step();
    CHECK(r.service.locked_count() == 0u);
}

TEST_CASE("door locks (review I1): a locked door gone with nobody near was streamed out: it is locked again when back") {
    Rig r;
    add_door(kGone);
    REQUIRE(r.order(kGone, DoorOrderKind::Lock));
    r.step();
    r.service.server_tick(r.world(), 1000);
    r.players = {{700, 20, 30}};  // 197 m away
    remove_door(kGone);
    r.step();
    r.service.server_tick(r.world(), 2000);
    CHECK(r.service.locked_count() == 0u);
    CHECK(r.service.saved_text().find("lock 503.00 20.00 30.00") != std::string::npos);  // still kept
    add_door(kGone);
    r.service.server_tick(r.world(), 60000);
    r.step();
    CHECK(r.service.locked_count() == 1u);
}

TEST_CASE("door locks (review I2): one look lists the doors once for all saved places") {
    Rig r;
    add_door(504);
    r.service.load_saved("lock 500.00 20.00 30.00\nlock 504.00 20.00 30.00\nlock 9000.00 20.00 30.00\n");
    r.service.server_tick(r.world(), 1000);
    CHECK(r.list_calls == 1);
    r.step();
    CHECK(r.service.locked_count() == 2u);
}

TEST_CASE("door locks (review M2): each saved place gets the nearest door, and a door only one place") {
    const std::vector<DoorSpot> doors{{1, {0, 0, 0}}, {2, {0.4, 0, 0}}};
    const std::vector<std::array<double, 3>> places{{0.35, 0, 0}, {0.05, 0, 0}, {5, 0, 0}, {0.3, 0, 0}};
    CHECK(match_doors(doors, places) == std::vector<uint32_t>{2, 1, 0, 0});
    CHECK(match_doors(doors, {{0, 0.6, 0}}) == std::vector<uint32_t>{0});  // half a metre, in height too
}

TEST_CASE("door locks (review M1): a place found while other locks are saved stays in the file") {
    Rig r;
    add_door(504);
    REQUIRE(r.order(504, DoorOrderKind::Lock));
    r.step();  // a change waits to be saved
    r.service.load_saved("lock 500.00 20.00 30.00\n");
    r.service.server_tick(r.world(), 1000);  // finds 500 and saves
    REQUIRE_FALSE(r.saved.empty());
    CHECK(r.saved.back().find("lock 500.00 20.00 30.00") != std::string::npos);
    CHECK(r.saved.back().find("lock 504.00 20.00 30.00") != std::string::npos);
    r.step();
    r.service.server_tick(r.world(), 2000);
    const std::string t = r.service.saved_text();
    CHECK(t.find("lock 500.00") == t.rfind("lock 500.00"));  // once
}

TEST_CASE("door locks (review M4): a door at a saved place that a switch drives now is not locked; its place goes") {
    Rig r;
    r.service.load_saved("lock 501.00 20.00 30.00\n");  // kSwitched
    r.service.server_tick(r.world(), 1000);
    r.step();
    CHECK(r.service.locked_count() == 0u);
    CHECK(r.service.saved_text().find("lock 501.00") == std::string::npos);
}
