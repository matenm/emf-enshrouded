// Player events (API v28 on_player): the service that reads the server world's players each step and hands mods the
// changes, and the reader of the player list through the API table.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/api.h"
#include "core/api_sizes.h"
#include "core/player_events.h"
#include "emf/generated/components.hpp"
#include "lua_test_fake.h"

using emf::core::PlayerEvents;
using emf::core::RosterSeen;

namespace {
struct Got {
    uint32_t kind, player, killer;
    uint64_t account;
    bool host;
    std::string name;
};
std::vector<Got> g_got;
void record(EmfWorld*, const EmfPlayerEvent* e, void*) {
    g_got.push_back({e->kind, e->player, e->killer, e->account, e->host != 0, e->name});
}
void faulty(EmfWorld*, const EmfPlayerEvent*, void*) {}
std::vector<Got> g_got2;
void record2(EmfWorld*, const EmfPlayerEvent* e, void*) {
    g_got2.push_back({e->kind, e->player, e->killer, e->account, e->host != 0, e->name});
}

struct Svc {
    std::vector<RosterSeen> seen;
    uint64_t now = 0;
    std::vector<std::string> logs;
    bool fault_faulty = true;  // the guarded call answers false for `faulty` (as a crash in it would)
    PlayerEvents p;
    Svc()
        : p(PlayerEvents::Deps{
              [this](EmfWorld*) { return seen; },
              [this] { return now; },
              [this](const std::string& s) { logs.push_back(s); },
              [this](EmfPlayerCallback cb, EmfWorld* w, const EmfPlayerEvent* e, void* u) {
                  if (cb == faulty && fault_faulty) return false;
                  cb(w, e, u);
                  return true;
              },
              [](const void*) { return std::string("faulty.dll"); }}) {
        g_got.clear();
    }
    void step(uint32_t session = 1) { p.server_step(kServer, session); }
    // A world's save loaded and the next step came: everyone joins 1 s later.
    void load(uint32_t session = 1) {
        p.world_loaded();
        step(session);
        now += 1000;
    }
};
std::string kinds() {
    std::string s;
    for (const auto& e : g_got) {
        if (!s.empty()) s += ' ';
        s += (e.kind == EMF_PLAYER_JOINED ? "+" : e.kind == EMF_PLAYER_LEFT ? "-" : "x") + std::to_string(e.player);
    }
    return s;
}
}  // namespace

TEST_CASE("player events: a mod gets joined for everyone 1 s after the world loaded, then the changes") {
    Svc s;
    CHECK_FALSE(s.p.watch(nullptr, nullptr));
    REQUIRE(s.p.watch(record, nullptr));
    s.seen = {{1, 0xA, true, true, "Host"}, {2, 0xB, true, false, "Bea"}};
    s.step();
    CHECK(g_got.empty());  // no world loaded yet
    s.p.world_loaded();
    s.now = 5000;
    s.step();  // the first step after the load (however late it comes): 1 s from here
    s.now = 5999;
    s.step();
    CHECK(g_got.empty());  // the mods' world data first
    s.now = 6000;
    s.step();
    CHECK(kinds() == "+1 +2");
    REQUIRE(g_got.size() == 2);
    CHECK(g_got[0].host);
    CHECK(g_got[0].account == 0xA);
    CHECK(g_got[1].name == "Bea");
    g_got.clear();
    s.seen = {{1, 0xA, true, true, "Host"}};
    s.now = 7000;
    s.step();
    CHECK(kinds() == "-2");
}

TEST_CASE("player events: a new session waits for its own world's load") {
    Svc s;
    REQUIRE(s.p.watch(record, nullptr));
    s.seen = {{1, 0xA, true, true, "Host"}};
    s.load(1);
    s.step(1);
    REQUIRE(kinds() == "+1");
    g_got.clear();
    s.now = 5000;
    s.step(2);  // another world: nobody left, nobody joined until it loaded
    s.now = 9000;
    s.step(2);
    CHECK(g_got.empty());
    s.load(2);
    s.step(2);
    CHECK(kinds() == "+1");
}

TEST_CASE("player events: a callback that faults is switched off and named once; the others go on") {
    Svc s;
    REQUIRE(s.p.watch(faulty, nullptr));
    REQUIRE(s.p.watch(record, nullptr));
    s.seen = {{1, 0xA, true, true, ""}};
    s.load();
    s.step();
    CHECK(kinds() == "+1");
    s.seen = {};
    s.now += 1000;
    s.step();
    CHECK(kinds() == "+1 -1");
    size_t told = 0;
    for (const auto& l : s.logs) told += l.find("faulty.dll") != std::string::npos;
    CHECK(told == 1);
}

TEST_CASE("player events: a joined player's death comes with the next step, with the last hitter") {
    Svc s;
    REQUIRE(s.p.watch(record, nullptr));
    s.seen = {{1, 0xA, true, true, ""}, {2, 0xB, true, false, ""}};
    s.load();
    s.step();
    g_got.clear();
    s.now += 2000;
    s.p.hit(2, 812);
    s.p.died(2);
    s.p.died(900);  // not a player: nothing
    CHECK(g_got.empty());
    s.step();
    REQUIRE(g_got.size() == 1);
    CHECK(g_got[0].kind == EMF_PLAYER_DIED);
    CHECK(g_got[0].player == 2);
    CHECK(g_got[0].account == 0xB);
    CHECK(g_got[0].killer == 812);
}

TEST_CASE("player events: nothing is read while no mod watches") {
    Svc s;
    size_t reads = 0;
    PlayerEvents p(PlayerEvents::Deps{[&](EmfWorld*) {
                                          ++reads;
                                          return std::vector<RosterSeen>{};
                                      },
                                      [] { return uint64_t(5000); }, [](const std::string&) {},
                                      [](EmfPlayerCallback, EmfWorld*, const EmfPlayerEvent*, void*) { return true; },
                                      [](const void*) { return std::string(); }});
    p.world_loaded();
    p.server_step(kServer, 1);
    CHECK(reads == 0);
    CHECK_FALSE(p.watched());
}

TEST_CASE("player events: the reader lists the server world's players with account, readiness, host and name") {
    Rig r(L"player_events_reader");
    auto put = [&](uint32_t id, uint64_t account) {
        r.fake.components[{emf::gen::ecs::PlayerState::kHash, id}].assign(sizeof(emf::gen::ecs::PlayerState), 0);
        r.fake.components[{emf::gen::ecs::CurrentTransform::kHash, id}].assign(sizeof(emf::gen::ecs::CurrentTransform), 0);
        r.fake.player_info(id - 1, id, account);
    };
    put(1, 0xA);
    put(3, 0xC);
    put(4, 0);  // no account yet
    auto& in = r.fake.components[{emf::gen::ecs::PlayerInput::kHash, 3}];
    in.assign(sizeof(emf::gen::ecs::PlayerInput), 0);  // still loading: not ready
    r.fake.components[{emf::gen::ecs::PlayerState::kHash, 7}].assign(sizeof(emf::gen::ecs::PlayerState), 0);  // no position
    r.fake.names[3] = "Cy";
    r.api.local_player = [] { return 1u; };
    const auto seen = emf::core::read_players(&r.api, kServer);
    REQUIRE(seen.size() == 3);
    CHECK(seen[0].entity == 1);
    CHECK(seen[0].account == 0xA);
    CHECK(seen[0].host);
    CHECK(seen[0].ready);  // no PlayerInput: ready
    CHECK(seen[1].entity == 3);
    CHECK_FALSE(seen[1].ready);
    CHECK(seen[1].name == "Cy");
    CHECK_FALSE(seen[1].host);
    CHECK(seen[2].entity == 4);
    CHECK(seen[2].account == 0);
    in[offsetof(emf::gen::ecs::PlayerInput, fromClientIsReadyForGameplay)] = 1;
    CHECK(emf::core::read_players(&r.api, kServer)[1].ready);
}

TEST_CASE("player events: a 64-byte name (the longest a connection carries) is read whole") {
    Rig r(L"player_events_long_name");
    r.fake.components[{emf::gen::ecs::PlayerState::kHash, 2}].assign(sizeof(emf::gen::ecs::PlayerState), 0);
    r.fake.components[{emf::gen::ecs::CurrentTransform::kHash, 2}].assign(sizeof(emf::gen::ecs::CurrentTransform), 0);
    r.fake.player_info(1, 2, 0xB);
    std::string name(60, 'x');
    name += "\xc3\xa9\xc3\xa9";  // 64 bytes, the last four two letters
    REQUIRE(name.size() == 64);
    r.fake.names[2] = name;
    const auto seen = emf::core::read_players(&r.api, kServer);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].name == name);
}

TEST_CASE("player events: the core's API table is v28 with on_player at its end") {
    const EmfApi* api = emf::core::api_table();
    REQUIRE(api);
    CHECK(api->version >= 28u);
    CHECK(api->size >= emf::core::kApiV28Size);  // later versions append
    REQUIRE(api->on_player != nullptr);
    CHECK_FALSE(api->on_player(nullptr, nullptr));
}

TEST_CASE("player events: the killing hit that comes after the death in the same step names the killer") {
    Svc s;
    REQUIRE(s.p.watch(record, nullptr));
    s.seen = {{1, 0xA, true, true, ""}, {2, 0xB, true, false, ""}};
    s.load();
    s.step();
    g_got.clear();
    s.now += 500;
    s.p.hit(2, 700);  // an earlier hit
    s.p.died(2);      // the relay hands the step's deaths before its hits
    s.p.hit(2, 812);  // the killing hit
    s.step();
    REQUIRE(g_got.size() == 1);
    CHECK(g_got[0].killer == 812);
    g_got.clear();
    s.now += 3000;
    s.p.died(2);  // a fall soon after: no hit since
    s.step();
    REQUIRE(g_got.size() == 1);
    CHECK(g_got[0].killer == 0);
}

TEST_CASE("player events: a mod that starts watching after the players joined is told about them, to it alone") {
    Svc s;
    REQUIRE(s.p.watch(record, nullptr));
    s.seen = {{1, 0xA, true, true, "Host"}, {2, 0xB, true, false, "Bea"}};
    s.load();
    s.step();
    REQUIRE(kinds() == "+1 +2");
    g_got.clear();
    g_got2.clear();
    REQUIRE(s.p.watch(record2, nullptr));
    s.now += 100;
    s.step();
    CHECK(g_got.empty());
    REQUIRE(g_got2.size() == 2);
    CHECK(g_got2[0].kind == EMF_PLAYER_JOINED);
    CHECK(g_got2[0].host);
    CHECK(g_got2[1].player == 2);
    CHECK(g_got2[1].name == "Bea");
    s.now += 1000;
    s.step();
    CHECK(g_got2.size() == 2u);  // told once
}
