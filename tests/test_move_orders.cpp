// Move orders (API v40): a creature sent to a point through the game's guard order on a waypoint entity (fake world).
// Live spike 2026-11-12: a guard order on an entity 25 m away walked a Vukah straight there (Guard state, ~5 m/s).
#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/api.h"
#include "common/offsets.h"
#include "core/api_sizes.h"
#include "core/dispatcher.h"
#include "core/enemy_orders.h"
#include "core/move_orders.h"
#include "emf/emf_api.h"
#include "emf/generated/components.hpp"
#include "fake_world.h"

using namespace emf::core;

namespace {
namespace gen = emf::gen::ecs;

constexpr uint32_t kEnemy = 70, kPlayer = 1, kBeast = 72;

struct Fake {
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> comps;
    uint64_t now = 1000;
    uint32_t next_spawn = 5000;
    std::vector<std::array<double, 3>> spawned;  // waypoints, in order
    std::vector<uint32_t> destroyed;
    std::vector<std::pair<uint32_t, uint32_t>> guards;   // (guarding, guarded): every guard order queued
    std::vector<std::pair<uint32_t, uint32_t>> pending;  // the queue the emf_moves program sends at the next step
    std::map<uint32_t, bool> dead;
    bool spawn_ok = true;

    template <class T>
    T& add(uint32_t e) {
        auto& v = comps[{T::kHash, e}];
        v.assign(sizeof(T), 0);
        return *reinterpret_cast<T*>(v.data());
    }
    template <class T>
    T& get(uint32_t e) {
        return *reinterpret_cast<T*>(comps.at({T::kHash, e}).data());
    }
    void place(uint32_t e, double x, double y, double z) {
        auto& v = comps[{gen::CurrentTransform::kHash, e}];
        v.assign(56, 0);
        const int64_t raw[3] = {std::llround(x * 4294967296.0), std::llround(y * 4294967296.0),
                                std::llround(z * 4294967296.0)};
        std::memcpy(v.data(), raw, sizeof raw);
    }
    MoveOrders::Deps deps() {
        MoveOrders::Deps d;
        d.get = [this](EmfWorld*, uint32_t hash, uint32_t e, uint64_t* size) -> void* {
            auto it = comps.find({hash, e});
            if (it == comps.end()) return nullptr;
            if (size) *size = it->second.size();
            return it->second.data();
        };
        d.now_ms = [this] { return now; };
        d.spawn = [this](EmfWorld*, double x, double y, double z) -> uint32_t {
            if (!spawn_ok) return 0;
            spawned.push_back({x, y, z});
            place(next_spawn, x, y, z);
            return next_spawn++;
        };
        d.destroy = [this](EmfWorld*, uint32_t e) {
            destroyed.push_back(e);
            comps.erase({gen::CurrentTransform::kHash, e});
        };
        d.guard = [this](uint32_t a, uint32_t b) {
            guards.push_back({a, b});
            pending.push_back({a, b});
        };
        d.unguard = [this](uint32_t a) {
            pending.erase(std::remove_if(pending.begin(), pending.end(), [a](const auto& g) { return g.first == a; }),
                          pending.end());
        };
        d.dead = [this](EmfWorld*, uint32_t e) { return dead.count(e) && dead[e]; };
        return d;
    }
};

EmfWorld* W() { return reinterpret_cast<EmfWorld*>(0x1000); }

Fake world() {
    Fake f;
    f.add<gen::DynamicEnemy01BehaviorState>(kEnemy);
    f.add<gen::DynamicAggro>(kEnemy);
    f.add<gen::SelectedTargets>(kEnemy);
    f.add<gen::Faction>(kEnemy).faction = static_cast<emf::gen::FactionType>(3);
    f.place(kEnemy, 0, 10, 0);
    f.place(kPlayer, 50, 10, 50);
    f.add<gen::PlayerState>(kPlayer);
    return f;
}

struct Done {
    uint32_t id = 0, status = 99;
    int calls = 0;
};
MoveOrders::Callback cb(Done& d) {
    return [&d](EmfWorld*, uint32_t id, uint32_t status) {
        d.id = id;
        d.status = status;
        ++d.calls;
    };
}
std::vector<std::array<double, 3>> pts(std::initializer_list<std::array<double, 3>> l) { return l; }
}  // namespace

TEST_CASE("move orders: a waypoint is spawned at the first point and the enemy is told to guard it") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    const uint32_t id = o.add(W(), kEnemy, pts({{20, 10, 0}}), 2.0, 60, 0, cb(d), &why);
    REQUIRE(id != 0u);
    REQUIRE(f.spawned.size() == 1u);
    CHECK(f.spawned[0][0] == doctest::Approx(20));
    REQUIRE(f.guards.size() == 1u);
    CHECK(f.guards[0] == std::make_pair(kEnemy, 5000u));
    CHECK(d.calls == 0);
}

TEST_CASE("move orders: within arrive distance of the last point it is done: arrived; the waypoint goes") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    const uint32_t id = o.add(W(), kEnemy, pts({{20, 10, 0}}), 2.0, 60, 0, cb(d), &why);
    auto& ai = f.get<gen::DynamicEnemy01BehaviorState>(kEnemy);
    ai.guardEntityId.id = 5000;  // the game took the guard order
    ai.state = emf::gen::enemy01::State::Guard;
    f.place(kEnemy, 10, 10, 0);
    f.now += 1000;
    o.step(W());
    CHECK(d.calls == 0);
    f.place(kEnemy, 18.5, 10, 0);
    f.now += 1000;
    o.step(W());
    CHECK(d.calls == 1);
    CHECK(d.id == id);
    CHECK(d.status == EMF_MOVE_ARRIVED);
    CHECK(f.destroyed == std::vector<uint32_t>{5000});
    CHECK(o.count() == 0u);
    CHECK(ai.guardEntityId.id == 0u);  // the waypoint's id may soon be another entity's
    CHECK(ai.state == emf::gen::enemy01::State::Idle);
    CHECK(ai.homeZonePosition.x == doctest::Approx(20));  // it lives there now
}

TEST_CASE("move orders: a new move on a moving creature ends the old one (cancelled) and takes over") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d, e;
    std::string why;
    const uint32_t a = o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, 0, cb(d), &why);
    const uint32_t b = o.add(W(), kEnemy, pts({{0, 10, 40}}), 2.0, 60, 0, cb(e), &why);
    REQUIRE(b != 0u);
    CHECK(d.id == a);
    CHECK(d.status == EMF_MOVE_CANCELLED);
    CHECK(o.count() == 1u);
    CHECK(o.moving(W(), kEnemy) == b);
    CHECK(f.guards.back() == std::make_pair(kEnemy, 5001u));
}

TEST_CASE("move orders: several points: a new waypoint at each next point, and the guard order on it") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    REQUIRE(o.add(W(), kEnemy, pts({{10, 10, 0}, {10, 10, 10}, {0, 10, 10}}), 2.0, 60, 0, cb(d), &why) != 0u);
    f.place(kEnemy, 10, 10, 1);
    o.step(W());
    REQUIRE(f.spawned.size() == 2u);
    CHECK(f.spawned[1][2] == doctest::Approx(10));
    CHECK(f.guards.back() == std::make_pair(kEnemy, 5001u));
    CHECK(f.destroyed == std::vector<uint32_t>{5000});
    f.place(kEnemy, 10, 10, 10);
    o.step(W());
    f.place(kEnemy, 1, 10, 10);
    o.step(W());
    CHECK(d.status == EMF_MOVE_ARRIVED);
    CHECK(f.spawned.size() == 3u);
}

TEST_CASE("move orders: the guard order is sent again every 3 s (the game ends a guard on Stuck)") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    REQUIRE(o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, 0, cb(d), &why) != 0u);
    f.now += 1000;
    f.place(kEnemy, 5, 10, 0);
    o.step(W());
    CHECK(f.guards.size() == 1u);
    f.now += 2500;
    f.place(kEnemy, 10, 10, 0);
    o.step(W());
    CHECK(f.guards.size() == 2u);
}

TEST_CASE("move orders: no progress for 15 s is stuck; past its time is timeout; the enemy dead or gone is gone") {
    for (int c = 0; c < 3; ++c) {
        CAPTURE(c);
        Fake f = world();
        MoveOrders o(f.deps());
        Done d;
        std::string why;
        REQUIRE(o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, c == 1 ? 10 : 60, 0, cb(d), &why) != 0u);
        if (c == 0) {
            for (int i = 0; i < 16; ++i) {  // it stays where it is
                f.now += 1000;
                o.step(W());
            }
            CHECK(d.status == EMF_MOVE_STUCK);
        }
        if (c == 1) {
            for (int i = 0; i < 11; ++i) {  // it walks, slowly, but the 10 s run out
                f.now += 1000;
                f.place(kEnemy, 1.0 * (i + 1), 10, 0);
                o.step(W());
            }
            CHECK(d.status == EMF_MOVE_TIMEOUT);
        }
        if (c == 2) {
            f.dead[kEnemy] = true;
            o.step(W());
            CHECK(d.status == EMF_MOVE_GONE);
        }
        CHECK(d.calls == 1);
        CHECK(f.destroyed.size() == 1u);
    }
}

TEST_CASE("move orders: cancel calls back cancelled; a world reset ends all with cancelled (no world)") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d, e;
    std::string why;
    const uint32_t id = o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, 0, cb(d), &why);
    CHECK(o.cancel(id));
    CHECK(d.status == EMF_MOVE_CANCELLED);
    CHECK_FALSE(o.cancel(id));
    REQUIRE(o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, 0, cb(e), &why) != 0u);
    o.world_reset();
    CHECK(e.status == EMF_MOVE_CANCELLED);
    CHECK(o.count() == 0u);
}

TEST_CASE("move orders: peaceful clears its fights against players each step; a monster fight stays") {
    Fake f = world();
    f.add<gen::Faction>(71).faction = static_cast<emf::gen::FactionType>(10);  // a skeleton
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    REQUIRE(o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, EMF_MOVE_PEACEFUL, cb(d), &why) != 0u);
    auto& ag = f.get<gen::DynamicAggro>(kEnemy);
    ag.targets[0].entityId.id = kPlayer;
    ag.targets[0].state = gen::AggroTargetState::Alarmed;
    ag.targets[0].aggro = 20;
    ag.targets[1].entityId.id = 71;
    ag.targets[1].state = gen::AggroTargetState::Alarmed;
    ag.targets[1].aggro = 20;
    f.get<gen::SelectedTargets>(kEnemy).targets[0].targetEntityId.id = kPlayer;
    f.place(kEnemy, 5, 10, 0);
    o.step(W());
    CHECK(ag.targets[0].state == gen::AggroTargetState::Unknown);
    CHECK(ag.targets[0].aggro == 0.0f);
    CHECK(ag.targets[1].state == gen::AggroTargetState::Alarmed);
    CHECK(f.get<gen::SelectedTargets>(kEnemy).targets[0].targetEntityId.id == 0u);
}

TEST_CASE("move orders: a tamed beast needs EMF_MOVE_ALLIES, and guards its owner again after (arrived or not)") {
    Fake f = world();
    f.add<gen::DynamicEnemy01BehaviorState>(kBeast).guardEntityId.id = kPlayer;
    f.add<gen::DynamicAggro>(kBeast);
    f.add<gen::SelectedTargets>(kBeast);
    f.add<gen::Faction>(kBeast).faction = static_cast<emf::gen::FactionType>(11);
    f.place(kBeast, 0, 10, 0);
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    CHECK(o.add(W(), kBeast, pts({{10, 10, 0}}), 2.0, 60, 0, cb(d), &why) == 0u);
    CHECK(why.find("EMF_MOVE_ALLIES") != std::string::npos);
    const uint32_t id = o.add(W(), kBeast, pts({{10, 10, 0}}), 2.0, 60, EMF_MOVE_ALLIES, cb(d), &why);
    REQUIRE(id != 0u);
    f.place(kBeast, 9.5, 10, 0);
    o.step(W());
    CHECK(d.status == EMF_MOVE_ARRIVED);
    CHECK(f.guards.back() == std::make_pair(kBeast, kPlayer));  // back to its owner
}

TEST_CASE("move orders: refused: no enemy AI, a player, a boss, no points, too many, bad numbers, no waypoint") {
    Fake f = world();
    f.add<gen::Boss>(73);
    f.add<gen::DynamicEnemy01BehaviorState>(73);
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    CHECK(o.add(W(), 999, pts({{1, 1, 1}}), 2.0, 60, 0, cb(d), &why) == 0u);
    CHECK(why.find("enemy AI") != std::string::npos);
    CHECK(o.add(W(), kPlayer, pts({{1, 1, 1}}), 2.0, 60, 0, cb(d), &why) == 0u);
    CHECK(o.add(W(), 73, pts({{1, 1, 1}}), 2.0, 60, 0, cb(d), &why) == 0u);
    CHECK(why.find("boss") != std::string::npos);
    CHECK(o.add(W(), kEnemy, {}, 2.0, 60, 0, cb(d), &why) == 0u);
    std::vector<std::array<double, 3>> many(17, {1, 1, 1});
    CHECK(o.add(W(), kEnemy, many, 2.0, 60, 0, cb(d), &why) == 0u);
    CHECK(o.add(W(), kEnemy, pts({{NAN, 1, 1}}), 2.0, 60, 0, cb(d), &why) == 0u);
    CHECK(o.add(W(), kEnemy, pts({{1, 1, 1}}), 0.1, 60, 0, cb(d), &why) == 0u);
    CHECK(o.add(W(), kEnemy, pts({{1, 1, 1}}), 2.0, 99999, 0, cb(d), &why) == 0u);
    CHECK(o.add(W(), kEnemy, pts({{1, 1, 1}}), 2.0, 60, 0x80, cb(d), &why) == 0u);
    f.spawn_ok = false;
    CHECK(o.add(W(), kEnemy, pts({{1, 1, 1}}), 2.0, 60, 0, cb(d), &why) == 0u);
    CHECK(why.find("waypoint") != std::string::npos);
    CHECK(d.calls == 0);
}

TEST_CASE("api table (v40): the three move functions at the end") {
    const EmfApi* api = emf::core::api_table();
    CHECK(api->version >= 40u);  // the newest version is checked by its own test (v41, below)
    CHECK(api->size == kApiV40Size);
    CHECK(api->enemy_move != nullptr);
    CHECK(api->enemy_move_cancel != nullptr);
    CHECK(api->enemy_moving != nullptr);
    EmfMoveOrder tiny{};
    tiny.size = 8;  // an older or broken caller
    CHECK(api->enemy_move(nullptr, &tiny, nullptr, nullptr) == 0u);
    CHECK_FALSE(api->enemy_move_cancel(12345));
    CHECK(api->enemy_moving(nullptr, 70) == 0u);
}

TEST_CASE("move orders: a new enemy order on a moving creature ends its move; a move ends its enemy order") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    const uint32_t id = o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, 0, cb(d), &why);
    REQUIRE(id != 0u);
    CHECK(o.cancel_enemy(W(), kEnemy));  // what enemy_order calls first
    CHECK(d.status == EMF_MOVE_CANCELLED);
    CHECK_FALSE(o.cancel_enemy(W(), kEnemy));
}

TEST_CASE("move orders: a cancel from outside the server world ends the move at the next step") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    const uint32_t id = o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, 0, cb(d), &why);
    CHECK(o.cancel(id, false));
    CHECK(d.calls == 0);
    CHECK(f.destroyed.empty());  // nothing touches the world off its thread
    o.step(W());
    CHECK(d.status == EMF_MOVE_CANCELLED);
    CHECK(f.destroyed == std::vector<uint32_t>{5000});
    CHECK(o.count() == 0u);
}

// Live 2026-11-12: a Vukah guarding a waypoint 27 m away walked there and stopped 2.4 m short (its guard distance,
// currentGuardTargetDistance, was 3.0), then went Idle: with arrive 2 the move ended stuck.
TEST_CASE("move orders: a point counts as reached within the creature's guard distance + 1 m (it stops that far short)") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    REQUIRE(o.add(W(), kEnemy, pts({{20, 10, 0}}), 2.0, 60, 0, cb(d), &why) != 0u);
    auto& ai = f.get<gen::DynamicEnemy01BehaviorState>(kEnemy);
    ai.currentGuardTargetDistance = 3.0f;
    ai.guardEntityId.id = 5000;  // it guards the waypoint (the game took the order)
    f.place(kEnemy, 15.5, 10, 0);  // 4.5 m: not yet
    o.step(W());
    CHECK(d.calls == 0);
    f.place(kEnemy, 16.6, 10.3, 0.8);  // 3.5 m
    o.step(W());
    CHECK(d.status == EMF_MOVE_ARRIVED);
}

// Live 2026-11-12: a roaming Vukah given the guard order walked on to its roam spot (7 s the wrong way) and took the
// guard only once it was Idle; in Idle it took it at once.
TEST_CASE("move orders: a calm creature (roaming, at home, patrolling) is put in Idle with the guard order; a fight stays") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    auto& ai = f.get<gen::DynamicEnemy01BehaviorState>(kEnemy);
    ai.state = emf::gen::enemy01::State::Roaming;
    REQUIRE(o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, 0, cb(d), &why) != 0u);
    CHECK(ai.state == emf::gen::enemy01::State::Idle);
    ai.state = emf::gen::enemy01::State::WaitAtHome;
    f.now += 3500;
    o.step(W());  // the guard order again: Idle again
    CHECK(ai.state == emf::gen::enemy01::State::Idle);
    ai.state = emf::gen::enemy01::State::Combat;
    f.now += 3500;
    o.step(W());
    CHECK(ai.state == emf::gen::enemy01::State::Combat);
}

// Live 2026-11-12: a Vukah fighting the player was moved peacefully: its fight was cleared but it stayed in Combat with
// nothing picked and stood there.
TEST_CASE("move orders: peaceful: a creature left in Combat with nothing to fight goes to Idle (then takes the guard)") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    auto& ai = f.get<gen::DynamicEnemy01BehaviorState>(kEnemy);
    ai.state = emf::gen::enemy01::State::Combat;
    f.get<gen::SelectedTargets>(kEnemy).targets[0].targetEntityId.id = kPlayer;
    REQUIRE(o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, EMF_MOVE_PEACEFUL, cb(d), &why) != 0u);
    CHECK(ai.state == emf::gen::enemy01::State::Idle);
    ai.state = emf::gen::enemy01::State::Combat;  // the game started the fight again
    f.get<gen::SelectedTargets>(kEnemy).targets[0].targetEntityId.id = kPlayer;
    o.step(W());
    CHECK(ai.state == emf::gen::enemy01::State::Idle);
    ai.state = emf::gen::enemy01::State::Combat;  // a fight with a monster stays
    f.add<gen::Faction>(71).faction = static_cast<emf::gen::FactionType>(10);
    f.get<gen::SelectedTargets>(kEnemy).targets[0].targetEntityId.id = 71;
    o.step(W());
    CHECK(ai.state == emf::gen::enemy01::State::Combat);
}

// ---- review (2026-11-12) ----

TEST_CASE("move orders (review I1): a move that ends before its guard order went out takes the order back") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    const uint32_t id = o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, 0, cb(d), &why);
    REQUIRE(f.pending.size() == 1u);
    CHECK(o.cancel(id));
    CHECK(f.pending.empty());  // no guard on the destroyed waypoint (its id may soon be another entity's)
    // already there when asked: it arrives at the first step, before the program ran
    REQUIRE(o.add(W(), kEnemy, pts({{0.5, 10, 0}}), 2.0, 60, 0, cb(d), &why) != 0u);
    o.step(W());
    CHECK(d.status == EMF_MOVE_ARRIVED);
    CHECK(f.pending.empty());
}

TEST_CASE("move orders (review I2): a move started from the last one's callback keeps the beast's owner") {
    Fake f = world();
    auto& ai = f.add<gen::DynamicEnemy01BehaviorState>(kBeast);
    ai.guardEntityId.id = kPlayer;
    f.add<gen::DynamicAggro>(kBeast);
    f.add<gen::SelectedTargets>(kBeast);
    f.add<gen::Faction>(kBeast).faction = static_cast<emf::gen::FactionType>(11);
    f.place(kBeast, 0, 10, 0);
    MoveOrders o(f.deps());
    std::string why;
    uint32_t second_status = 99;
    auto second = [&](EmfWorld*, uint32_t, uint32_t status) { second_status = status; };
    auto first = [&](EmfWorld* w, uint32_t, uint32_t status) {
        if (status == EMF_MOVE_ARRIVED) o.add(w, kBeast, pts({{20, 10, 0}}), 2.0, 60, EMF_MOVE_ALLIES, second, &why);
    };
    REQUIRE(o.add(W(), kBeast, pts({{10, 10, 0}}), 2.0, 60, EMF_MOVE_ALLIES, first, &why) != 0u);
    ai.guardEntityId.id = 5000;  // the game took the guard order
    f.place(kBeast, 9.5, 10, 0);
    o.step(W());  // arrived; the callback starts the second move
    REQUIRE(o.count() == 1u);
    ai.guardEntityId.id = 5001;
    f.place(kBeast, 19.5, 10, 0);
    o.step(W());
    CHECK(second_status == EMF_MOVE_ARRIVED);
    CHECK(f.pending.back() == std::make_pair(kBeast, kPlayer));  // back to its owner, not a dead waypoint
    CHECK(ai.guardEntityId.id == kPlayer);
    CHECK(ai.homeZonePosition.x == doctest::Approx(0));  // a beast with an owner does not move home
}

TEST_CASE("move orders (review I2): a guard on an entity that is gone is not given back") {
    Fake f = world();
    auto& ai = f.get<gen::DynamicEnemy01BehaviorState>(kEnemy);
    ai.guardEntityId.id = 4321;  // stale: nothing there
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    REQUIRE(o.add(W(), kEnemy, pts({{10, 10, 0}}), 2.0, 60, 0, cb(d), &why) != 0u);
    ai.guardEntityId.id = 5000;
    f.place(kEnemy, 9.5, 10, 0);
    o.step(W());
    CHECK(d.status == EMF_MOVE_ARRIVED);
    for (const auto& g : f.guards) CHECK(g.second != 4321u);
    CHECK(ai.guardEntityId.id == 0u);
    CHECK(ai.homeZonePosition.x == doctest::Approx(10));
}

TEST_CASE("move orders (review I3): the guard distance counts only once it guards the waypoint, and only up to 8 m") {
    Fake f = world();
    auto& ai = f.add<gen::DynamicEnemy01BehaviorState>(kBeast);
    ai.guardEntityId.id = kPlayer;
    ai.currentGuardTargetDistance = 5.0f;  // its distance to its owner
    f.add<gen::Faction>(kBeast).faction = static_cast<emf::gen::FactionType>(11);
    f.place(kBeast, 0, 10, 0);
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    REQUIRE(o.add(W(), kBeast, pts({{6, 10, 0}}), 2.0, 60, EMF_MOVE_ALLIES, cb(d), &why) != 0u);
    o.step(W());
    CHECK(d.calls == 0);  // 6 m away: not there yet
    ai.guardEntityId.id = 5000;
    ai.currentGuardTargetDistance = 15.0f;  // not believed
    o.step(W());
    CHECK(d.calls == 0);
    ai.currentGuardTargetDistance = 5.0f;
    o.step(W());
    CHECK(d.status == EMF_MOVE_ARRIVED);
}

TEST_CASE("move orders (review I4): in a fight (an attack, a hit, circling) no guard order is sent and the stuck clock waits") {
    Fake f = world();
    f.add<gen::Faction>(71).faction = static_cast<emf::gen::FactionType>(10);
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    REQUIRE(o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, 0, cb(d), &why) != 0u);
    auto& ai = f.get<gen::DynamicEnemy01BehaviorState>(kEnemy);
    f.get<gen::SelectedTargets>(kEnemy).targets[0].targetEntityId.id = 71;
    const size_t sent = f.guards.size();
    const emf::gen::enemy01::State fight[] = {emf::gen::enemy01::State::Combat, emf::gen::enemy01::State::Hit,
                                             emf::gen::enemy01::State::ControlledByAttack,
                                             emf::gen::enemy01::State::Circling};
    for (int i = 0; i < 20; ++i) {  // 20 s of fighting where it stands
        ai.state = fight[i % 4];
        f.now += 1000;
        o.step(W());
    }
    CHECK(f.guards.size() == sent);
    CHECK(d.calls == 0);
    ai.state = emf::gen::enemy01::State::Idle;  // the fight is over: the guard order again 3 s after it
    f.get<gen::SelectedTargets>(kEnemy).targets[0].targetEntityId.id = 0;
    f.now += 1000;
    o.step(W());
    CHECK(f.guards.size() == sent);
    f.now += 2500;
    o.step(W());
    CHECK(f.guards.size() == sent + 1);
}

TEST_CASE("move orders (review I5): a waypoint that vanished is placed again (not destroyed twice, the clock not reset)") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    REQUIRE(o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 0, 0, cb(d), &why) != 0u);
    f.place(kEnemy, 5, 10, 0);  // progress
    f.now += 1000;
    o.step(W());
    f.comps.erase({gen::CurrentTransform::kHash, 5000});  // something removed the waypoint
    f.now += 3000;
    o.step(W());
    REQUIRE(f.spawned.size() == 2u);
    CHECK(f.destroyed.empty());
    f.place(5000, 900, 10, 900);  // the game gave id 5000 to another entity: not ours
    for (int i = 0; i < 4; ++i) {  // it vanishes again and again
        f.comps.erase({gen::CurrentTransform::kHash, f.next_spawn - 1});
        f.now += 3000;
        o.step(W());
    }
    CHECK(d.status == EMF_MOVE_STUCK);  // placed again at most 3 times
    CHECK(std::find(f.destroyed.begin(), f.destroyed.end(), 5000u) == f.destroyed.end());
}

TEST_CASE("move orders (review I5): a vanishing waypoint does not hold off stuck") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    REQUIRE(o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 0, 0, cb(d), &why) != 0u);
    f.now += 4000;
    o.step(W());
    f.comps.erase({gen::CurrentTransform::kHash, 5000});
    f.now += 4000;
    o.step(W());  // placed again at 9 s; no progress since 1 s
    f.now += 8000;
    o.step(W());  // 17 s
    CHECK(d.status == EMF_MOVE_STUCK);
}

TEST_CASE("move orders (review M2): a refused move (no waypoint) sends no guard order") {
    Fake f = world();
    f.get<gen::DynamicEnemy01BehaviorState>(kEnemy).guardEntityId.id = kPlayer;
    f.spawn_ok = false;
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    CHECK(o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, 0, cb(d), &why) == 0u);
    CHECK(f.pending.empty());
}

namespace {
struct ApiRig {
    Fake f = world();
    emf::test::FakeWorld fw;
    CoreState state;
    std::unique_ptr<EnemyOrders> orders;
    std::unique_ptr<MoveOrders> moves;
    Dispatcher disp;
    ApiRig() {
        fw.put<uint8_t>(emf::off::kKind, 0);  // a server world
        f.add<gen::DynamicAggro>(kEnemy);
        f.add<gen::SelectedTargets>(kEnemy);
        f.place(900, 30, 10, 0);  // a gate
        state.log_line = [](const std::string&) {};
        EnemyOrders::Deps od;
        od.get = f.deps().get;
        od.now_ms = [this] { return f.now; };
        od.dead = [](EmfWorld*, uint32_t) { return false; };
        orders = std::make_unique<EnemyOrders>(od);
        moves = std::make_unique<MoveOrders>(f.deps());
        state.orders = orders.get();
        state.moves = moves.get();
        set_core_state(&state);
    }
    ~ApiRig() { set_core_state(nullptr); }
    EmfWorld* w() { return reinterpret_cast<EmfWorld*>(fw.p()); }
    template <class Fn>
    void in_world(Fn fn) {  // a server world callback, as a mod's on_tick would be
        static Fn* held;
        held = &fn;
        disp.post(EMF_WORLD_SERVER, [](EmfWorld* world, void*) { (*held)(world); }, nullptr);
        disp.run(w(), EMF_WORLD_SERVER, [](const std::string&) {});
    }
};
}  // namespace

TEST_CASE("api (review I6): a refused enemy order leaves the creature's move; an accepted one ends it, and back") {
    ApiRig r;
    std::string why;
    REQUIRE(r.moves->add(r.w(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, 0, nullptr, &why) != 0u);
    r.in_world([&](EmfWorld* world) {
        EmfEnemyOrder o{};
        o.size = sizeof o;
        o.enemy = kEnemy;
        o.target = 4444;  // no such target: refused
        CHECK(api_table()->enemy_order(world, &o) == 0u);
        CHECK(r.moves->moving(world, kEnemy) != 0u);  // the move goes on
        o.target = 900;
        CHECK(api_table()->enemy_order(world, &o) != 0u);
        CHECK(r.moves->moving(world, kEnemy) == 0u);  // the order ended it
        EmfPathPoint p{40, 10, 0, 0};
        EmfMoveOrder m{};
        m.size = sizeof m;
        m.enemy = kEnemy;
        m.count = 1;
        m.points = &p;
        m.arrive = 0.1f;  // refused
        CHECK(api_table()->enemy_move(world, &m, nullptr, nullptr) == 0u);
        CHECK(r.orders->order_of(world, kEnemy) != 0u);  // the order goes on
        m.arrive = 0;
        CHECK(api_table()->enemy_move(world, &m, nullptr, nullptr) != 0u);
        CHECK(r.orders->order_of(world, kEnemy) == 0u);  // the move ended it
    });
}

// Live 2026-11-14 (a modder's raids: every freshly spawned raider stood still, stuck after 15 s): a fresh Vukah sent
// 30 m took the guard target (guardEntityId = the waypoint) but stayed Roaming, or Idle when held there, and never
// walked; with its state set to Guard it walked and arrived in 5 s.
TEST_CASE("move orders: a calm creature that took the waypoint as its guard target is put in Guard (a fresh spawn)") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    REQUIRE(o.add(W(), kEnemy, pts({{30, 10, 0}}), 2.0, 60, 0, cb(d), &why) != 0u);
    auto& ai = f.get<gen::DynamicEnemy01BehaviorState>(kEnemy);
    ai.state = emf::gen::enemy01::State::Roaming;  // the guard order landed, the state did not follow
    ai.guardEntityId.id = 5000;
    o.step(W());
    CHECK(ai.state == emf::gen::enemy01::State::Guard);
    ai.state = emf::gen::enemy01::State::Idle;
    f.now += 3000;  // a nudge at most every 3 s
    o.step(W());
    CHECK(ai.state == emf::gen::enemy01::State::Guard);
    ai.guardEntityId.id = 4444;  // another guard target (not ours yet): left to the guard order
    ai.state = emf::gen::enemy01::State::Idle;
    f.now += 3000;
    o.step(W());
    CHECK(ai.state == emf::gen::enemy01::State::Idle);
    ai.guardEntityId.id = 5000;  // ours, but in a fight: the fight goes on
    ai.state = emf::gen::enemy01::State::Combat;
    f.get<gen::SelectedTargets>(kEnemy).targets[0].targetEntityId.id = 71;
    o.step(W());
    CHECK(ai.state == emf::gen::enemy01::State::Combat);
}

// 0.6.10, a modder's raids: a raider at a wall should try another way in sooner than after 15 s, and a raider that stole a
// trophy should only run (not stop to fight an animal or a player's beast).
TEST_CASE("move orders (0.6.10): a stuck time of its own (2-120 s, 0 = 15)") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    REQUIRE(o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, 0, cb(d), &why, 5) != 0u);
    for (int i = 0; i < 4; ++i) {
        f.now += 1000;
        o.step(W());
    }
    CHECK(d.calls == 0);
    f.now += 1000;
    o.step(W());
    CHECK(d.status == EMF_MOVE_STUCK);
    CHECK(o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, 0, cb(d), &why, 1) == 0u);
    CHECK(why.find("stuck") != std::string::npos);
    CHECK(o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, 0, cb(d), &why, 121) == 0u);
}

TEST_CASE("move orders (0.6.10): EMF_MOVE_PEACEFUL_ALL clears every fight (monsters, animals, beasts), not only players'") {
    Fake f = world();
    f.add<gen::Faction>(71).faction = static_cast<emf::gen::FactionType>(6);  // a wild animal
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    REQUIRE(o.add(W(), kEnemy, pts({{40, 10, 0}}), 2.0, 60, EMF_MOVE_PEACEFUL_ALL, cb(d), &why) != 0u);
    auto& ag = f.get<gen::DynamicAggro>(kEnemy);
    ag.targets[0].entityId.id = 71;
    ag.targets[0].state = gen::AggroTargetState::Alarmed;
    ag.targets[0].aggro = 20;
    ag.targets[1].entityId.id = kPlayer;
    ag.targets[1].state = gen::AggroTargetState::Alarmed;
    f.get<gen::SelectedTargets>(kEnemy).targets[0].targetEntityId.id = 71;
    auto& ai = f.get<gen::DynamicEnemy01BehaviorState>(kEnemy);
    ai.state = emf::gen::enemy01::State::Combat;
    f.place(kEnemy, 5, 10, 0);
    o.step(W());
    CHECK(ag.targets[0].state == gen::AggroTargetState::Unknown);
    CHECK(ag.targets[1].state == gen::AggroTargetState::Unknown);
    CHECK(f.get<gen::SelectedTargets>(kEnemy).targets[0].targetEntityId.id == 0u);
    CHECK(ai.state != emf::gen::enemy01::State::Combat);
}

TEST_CASE("api (0.6.10): an EmfMoveOrder of 0.6.9's size is still taken; the new stuck_seconds is read when present") {
    ApiRig r;
    r.in_world([&](EmfWorld* world) {
        EmfPathPoint p{40, 10, 0, 0};
        EmfMoveOrder m{};
        m.size = kMoveOrderV40Size;  // a mod built with 0.6.8's header
        m.enemy = kEnemy;
        m.count = 1;
        m.points = &p;
        m.stuck_seconds = 1;  // past its size: not read
        CHECK(api_table()->enemy_move(world, &m, nullptr, nullptr) != 0u);
        m.size = sizeof m;
        m.stuck_seconds = 1;  // read now: out of range
        CHECK(api_table()->enemy_move(world, &m, nullptr, nullptr) == 0u);
        CHECK(std::string(m.why).find("stuck") != std::string::npos);
        m.stuck_seconds = 5;
        CHECK(api_table()->enemy_move(world, &m, nullptr, nullptr) != 0u);
    });
}

// Review (0.6.10): the Guard nudge only for a creature with nothing picked (one going for a target may fight on the
// way), and at most every 3 s (it must not fight another state writer every step).
TEST_CASE("move orders (review I1): no Guard nudge while it has a pick; at most every 3 s") {
    Fake f = world();
    MoveOrders o(f.deps());
    Done d;
    std::string why;
    REQUIRE(o.add(W(), kEnemy, pts({{30, 10, 0}}), 2.0, 60, 0, cb(d), &why) != 0u);
    auto& ai = f.get<gen::DynamicEnemy01BehaviorState>(kEnemy);
    ai.guardEntityId.id = 5000;
    ai.state = emf::gen::enemy01::State::Investigate;  // it spotted a player: going for it
    f.get<gen::SelectedTargets>(kEnemy).targets[0].targetEntityId.id = kPlayer;
    o.step(W());
    CHECK(ai.state == emf::gen::enemy01::State::Investigate);
    f.get<gen::SelectedTargets>(kEnemy).targets[0].targetEntityId.id = 0;
    ai.state = emf::gen::enemy01::State::Roaming;
    o.step(W());
    CHECK(ai.state == emf::gen::enemy01::State::Guard);  // nudged
    ai.state = emf::gen::enemy01::State::Sleep;  // something else wants it asleep
    f.now += 1000;
    o.step(W());
    CHECK(ai.state == emf::gen::enemy01::State::Sleep);  // not again within 3 s
    f.now += 2500;
    o.step(W());
    CHECK(ai.state == emf::gen::enemy01::State::Guard);
}

TEST_CASE("api table (v41): version 41 or newer (0.6.10's move options: stuck_seconds, EMF_MOVE_PEACEFUL_ALL); the table did not grow") {
    const EmfApi* api = emf::core::api_table();
    CHECK(api->version >= 41u);
    CHECK(api->size == kApiV40Size);
}

TEST_CASE("api table (v42): version 42 (0.6.11: EmfPathResult.doors); the table did not grow") {
    const EmfApi* api = emf::core::api_table();
    CHECK(api->version == 42u);
    CHECK(api->size == kApiV40Size);
    CHECK(EMF_API_VERSION == 42);
    CHECK(kPathResultV37Size == 20u);
    CHECK(sizeof(EmfPathResult) > kPathResultV37Size);
}

// A modder's raids (0.6.11) and a live check (2026-11-15): two creatures standing on one spot, each with a move, never
// walk (Idle <-> Guard every half second, 0 m); moved 1 m apart, both walked at once. After 2 s without a metre of
// progress, EMF moves the creature of the later move 1 m aside (the game's teleport, Deps::nudge).
namespace {
constexpr uint32_t kOther = 73;
void add_enemy(Fake& f, uint32_t e, double x, double y, double z) {
    f.add<gen::DynamicEnemy01BehaviorState>(e);
    f.add<gen::DynamicAggro>(e);
    f.add<gen::SelectedTargets>(e);
    f.add<gen::Faction>(e).faction = static_cast<emf::gen::FactionType>(3);
    f.place(e, x, y, z);
}
struct Nudges {
    std::vector<std::tuple<uint32_t, double, double, double>> list;
    MoveOrders::Deps deps(Fake& f) {
        MoveOrders::Deps d = f.deps();
        d.nudge = [this](EmfWorld*, uint32_t e, double x, double y, double z) {
            list.push_back({e, x, y, z});
            return true;
        };
        return d;
    }
};
}  // namespace

TEST_CASE("move orders (0.6.12): two creatures on one spot: the later move's creature is moved 1 m aside after 2 s") {
    Fake f = world();
    add_enemy(f, kOther, 0, 10, 0);  // the same spot as kEnemy
    Nudges n;
    MoveOrders o(n.deps(f));
    Done a, b;
    std::string why;
    REQUIRE(o.add(W(), kEnemy, pts({{0, 10, 80}}), 2.0, 60, 0, cb(a), &why) != 0u);
    REQUIRE(o.add(W(), kOther, pts({{0, 10, 80}}), 2.0, 60, 0, cb(b), &why) != 0u);
    o.step(W());
    f.now += 1500;
    o.step(W());
    CHECK(n.list.empty());  // not before 2 s
    f.now += 600;
    o.step(W());
    REQUIRE(n.list.size() == 1u);
    CHECK(std::get<0>(n.list[0]) == kOther);  // the later move's creature, never both
    const double dx = std::get<1>(n.list[0]), dz = std::get<3>(n.list[0]);
    CHECK(std::sqrt(dx * dx + dz * dz) == doctest::Approx(1.0).epsilon(0.05));
    CHECK(std::get<2>(n.list[0]) == doctest::Approx(10.0));
    f.now += 1000;
    o.step(W());
    CHECK(n.list.size() == 1u);  // not again within 3 s
    f.now += 2500;
    o.step(W());
    CHECK(n.list.size() == 2u);  // still stacked (the fake does not move it): once more
    f.now += 3500;
    o.step(W());
    CHECK(n.list.size() == 2u);  // at most twice per move
    CHECK(a.calls == 0);
    CHECK(b.calls == 0);
}

TEST_CASE("move orders (0.6.12): creatures half a metre apart, walking, or fighting are not moved aside") {
    SUBCASE("half a metre apart") {
        Fake f = world();
        add_enemy(f, kOther, 0.5, 10, 0);
        Nudges n;
        MoveOrders o(n.deps(f));
        Done a, b;
        std::string why;
        REQUIRE(o.add(W(), kEnemy, pts({{0, 10, 80}}), 2.0, 60, 0, cb(a), &why) != 0u);
        REQUIRE(o.add(W(), kOther, pts({{0, 10, 80}}), 2.0, 60, 0, cb(b), &why) != 0u);
        for (int i = 0; i < 4; ++i) {
            f.now += 1000;
            o.step(W());
        }
        CHECK(n.list.empty());
    }
    SUBCASE("on one spot but walking (a metre closer every second)") {
        Fake f = world();
        add_enemy(f, kOther, 0, 10, 0);
        Nudges n;
        MoveOrders o(n.deps(f));
        Done a, b;
        std::string why;
        REQUIRE(o.add(W(), kEnemy, pts({{0, 10, 80}}), 2.0, 60, 0, cb(a), &why) != 0u);
        REQUIRE(o.add(W(), kOther, pts({{0, 10, 80}}), 2.0, 60, 0, cb(b), &why) != 0u);
        for (int i = 1; i <= 4; ++i) {
            f.place(kEnemy, 0, 10, 1.5 * i);
            f.place(kOther, 0, 10, 1.5 * i);
            f.now += 1000;
            o.step(W());
        }
        CHECK(n.list.empty());
    }
    SUBCASE("on one spot, fighting") {
        Fake f = world();
        add_enemy(f, kOther, 0, 10, 0);
        Nudges n;
        MoveOrders o(n.deps(f));
        Done a, b;
        std::string why;
        REQUIRE(o.add(W(), kEnemy, pts({{0, 10, 80}}), 2.0, 60, 0, cb(a), &why) != 0u);
        REQUIRE(o.add(W(), kOther, pts({{0, 10, 80}}), 2.0, 60, 0, cb(b), &why) != 0u);
        f.get<gen::DynamicEnemy01BehaviorState>(kOther).state = emf::gen::enemy01::State::Combat;
        f.get<gen::SelectedTargets>(kOther).targets[0].targetEntityId.id = kPlayer;
        for (int i = 0; i < 4; ++i) {
            f.now += 1000;
            o.step(W());
        }
        CHECK(n.list.empty());
    }
}
