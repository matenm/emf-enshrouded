// Enemy orders (API v39): an enemy sent at any entity (fake components). Live check 2026-11-12: the pick alone did not
// move a Vukah; the pick plus an alarmed aggro entry made it walk 15 m to a door and swing (the hits did 0 to it).
#include <doctest.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/api.h"
#include "core/api_sizes.h"
#include "core/enemy_orders.h"
#include "emf/emf_api.h"
#include "emf/generated/components.hpp"

using namespace emf::core;

namespace {
namespace gen = emf::gen::ecs;

constexpr uint32_t kEnemy = 70, kDoor = 900, kPlayer = 1, kOther = 71;

struct Fake {
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> comps;
    uint64_t now = 1000;
    std::vector<std::tuple<uint32_t, uint32_t, int32_t>> rules_added;
    std::vector<uint32_t> rules_removed;
    uint32_t next_rule = 5;
    bool rules_ok = true;
    std::map<uint32_t, bool> dead;

    template <class T>
    T& add(uint32_t entity) {
        auto& v = comps[{T::kHash, entity}];
        v.assign(sizeof(T), 0);
        return *reinterpret_cast<T*>(v.data());
    }
    template <class T>
    T& get(uint32_t entity) {
        return *reinterpret_cast<T*>(comps.at({T::kHash, entity}).data());
    }
    EnemyOrders::Deps deps() {
        EnemyOrders::Deps d;
        d.get = [this](EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size) -> void* {
            auto it = comps.find({hash, entity});
            if (it == comps.end()) return nullptr;
            if (size) *size = it->second.size();
            return it->second.data();
        };
        d.now_ms = [this] { return now; };
        d.add_rule = [this](uint32_t a, uint32_t t, int32_t add) -> uint32_t {
            if (!rules_ok) return 0;
            rules_added.push_back({a, t, add});
            return next_rule++;
        };
        d.remove_rule = [this](uint32_t id) { rules_removed.push_back(id); };
        d.dead = [this](EmfWorld*, uint32_t e) { return dead.count(e) && dead[e]; };
        return d;
    }
};

EmfWorld* W() { return reinterpret_cast<EmfWorld*>(0x1000); }

// A Vukah (faction 3) with its AI, and a door (a position, no health).
Fake world() {
    Fake f;
    f.add<gen::DynamicAggro>(kEnemy).allowTargetSwitches = true;
    f.add<gen::SelectedTargets>(kEnemy);
    f.add<gen::Faction>(kEnemy).faction = static_cast<emf::gen::FactionType>(3);
    f.add<gen::CurrentTransform>(kEnemy);
    f.add<gen::CurrentTransform>(kDoor);
    f.add<gen::CurrentTransform>(kPlayer);
    f.add<gen::PlayerState>(kPlayer);
    return f;
}

uint32_t pick(Fake& f) { return f.get<gen::SelectedTargets>(kEnemy).targets[0].targetEntityId.id; }
bool held(Fake& f) { return !f.get<gen::DynamicAggro>(kEnemy).allowTargetSwitches; }
// The aggro entry for `who`, or null.
const gen::AggroTarget* entry(Fake& f, uint32_t who) {
    for (const auto& t : f.get<gen::DynamicAggro>(kEnemy).targets)
        if (t.entityId.id == who) return &t;
    return nullptr;
}
}  // namespace

TEST_CASE("enemy orders: the target is the enemy's pick (switches off) and an alarmed aggro entry, at once and after every step") {
    Fake f = world();
    EnemyOrders o(f.deps());
    std::string why;
    const uint32_t id = o.add(W(), kEnemy, kDoor, 30, 0, 0, &why);
    REQUIRE(id != 0u);
    CHECK(pick(f) == kDoor);
    CHECK(f.get<gen::SelectedTargets>(kEnemy).targets[0].targetRootEntityId.id == kDoor);
    CHECK(held(f));
    const gen::AggroTarget* e = entry(f, kDoor);
    REQUIRE(e != nullptr);
    CHECK(e->state == gen::AggroTargetState::Alarmed);
    CHECK(e->aggro >= 100.0f);
    CHECK(o.target_of(W(), kEnemy) == kDoor);
    f.get<gen::SelectedTargets>(kEnemy).targets[0].targetEntityId.id = kPlayer;  // the game picked the player
    f.get<gen::DynamicAggro>(kEnemy).targets[0].aggro = 3;
    o.step(W());
    CHECK(pick(f) == kDoor);
    CHECK(entry(f, kDoor)->aggro >= 100.0f);
}

TEST_CASE("enemy orders: an order ends after its time, with the switches back on and its entry and pick cleared") {
    Fake f = world();
    EnemyOrders o(f.deps());
    std::string why;
    REQUIRE(o.add(W(), kEnemy, kDoor, 30, 0, 0, &why) != 0u);
    f.now += 29000;
    o.step(W());
    CHECK(held(f));
    f.now += 2000;
    o.step(W());
    CHECK_FALSE(held(f));
    CHECK(pick(f) == 0u);
    CHECK(entry(f, kDoor) == nullptr);
    CHECK(o.count() == 0u);
}

TEST_CASE("enemy orders: an order ends when the target is gone or dead, or the enemy is gone; seconds 0 holds until then") {
    for (int c = 0; c < 3; ++c) {
        CAPTURE(c);
        Fake f = world();
        EnemyOrders o(f.deps());
        std::string why;
        REQUIRE(o.add(W(), kEnemy, kDoor, 0, 0, 0, &why) != 0u);
        f.now += 3600000;  // an hour: still held
        o.step(W());
        REQUIRE(o.count() == 1u);
        if (c == 0) f.comps.erase({gen::CurrentTransform::kHash, kDoor});
        if (c == 1) f.dead[kDoor] = true;
        if (c == 2) f.comps.erase({gen::DynamicAggro::kHash, kEnemy});
        o.step(W());
        CHECK(o.count() == 0u);
        if (c != 2) CHECK_FALSE(held(f));
    }
}

TEST_CASE("enemy orders: cancel ends one; a new order on the same enemy ends its old one; a world reset ends all") {
    Fake f = world();
    f.add<gen::CurrentTransform>(kOther);
    EnemyOrders o(f.deps());
    std::string why;
    const uint32_t a = o.add(W(), kEnemy, kDoor, 30, 0, 0, &why);
    const uint32_t b = o.add(W(), kEnemy, kOther, 30, 0, 0, &why);
    REQUIRE(b != 0u);
    CHECK(o.count() == 1u);
    CHECK(pick(f) == kOther);
    CHECK(entry(f, kDoor) == nullptr);  // the old order's entry went with it
    CHECK_FALSE(o.cancel(a));
    CHECK(o.cancel(b));
    CHECK_FALSE(held(f));
    REQUIRE(o.add(W(), kEnemy, kDoor, 30, 50, 0, &why) != 0u);
    o.world_reset();
    CHECK(o.count() == 0u);
    CHECK(f.rules_removed.size() == 1u);  // its damage rule went too
}

TEST_CASE("enemy orders: damage adds a hit rule for the enemy's hits on the target, removed when the order ends") {
    Fake f = world();
    EnemyOrders o(f.deps());
    std::string why;
    REQUIRE(o.add(W(), kEnemy, kDoor, 30, 50, 0, &why) != 0u);
    REQUIRE(f.rules_added.size() == 1u);
    CHECK(f.rules_added[0] == std::make_tuple(kEnemy, kDoor, 50));
    f.now += 31000;
    o.step(W());
    CHECK(f.rules_removed == std::vector<uint32_t>{5});
    f.rules_ok = false;  // hits are off on this build: refused, nothing written
    CHECK(o.add(W(), kEnemy, kDoor, 30, 50, 0, &why) == 0u);
    CHECK(why.find("damage") != std::string::npos);
    CHECK_FALSE(held(f));
}

TEST_CASE("enemy orders: a damage order without a hit rule says both causes: all 64 in use, or hit rules off") {
    Fake f = world();
    f.rules_ok = false;  // add_rule returns 0
    EnemyOrders o(f.deps());
    std::string why;
    CHECK(o.add(W(), kEnemy, kDoor, 30, 50, 0, &why) == 0u);
    CHECK(why == "no hit rule for its damage: all 64 shared ones are in use, or hit rules are off on this build");
    CHECK(why.size() < sizeof(EmfEnemyOrder{}.why));  // o->why holds it whole
    CHECK(o.add(W(), kEnemy, kDoor, 30, 0, 0, &why) != 0u);  // without damage it needs no rule
}

TEST_CASE("enemy orders: the aggro entry reuses the target's, else a free slot, else the lowest aggro") {
    Fake f = world();
    auto& ag = f.get<gen::DynamicAggro>(kEnemy);
    for (int i = 0; i < 6; ++i) {  // full: six players' entries
        ag.targets[i].entityId.id = 100 + i;
        ag.targets[i].state = gen::AggroTargetState::Alarmed;
        ag.targets[i].aggro = 10.0f + i;
    }
    ag.targets[4].aggro = 1.0f;  // the lowest
    EnemyOrders o(f.deps());
    std::string why;
    REQUIRE(o.add(W(), kEnemy, kDoor, 30, 0, 0, &why) != 0u);
    CHECK(ag.targets[4].entityId.id == kDoor);
    CHECK(ag.targets[0].entityId.id == 100u);
    o.step(W());
    int n = 0;
    for (const auto& t : ag.targets) n += t.entityId.id == kDoor;
    CHECK(n == 1);  // reused, not a second slot
}

TEST_CASE("enemy orders: a cancel outside a world callback ends the order at the next step") {
    Fake f = world();
    EnemyOrders o(f.deps());
    std::string why;
    const uint32_t id = o.add(W(), kEnemy, kDoor, 30, 0, 0, &why);
    CHECK(o.cancel(id, false));
    CHECK(held(f));  // nothing written yet
    o.step(W());
    CHECK_FALSE(held(f));
    CHECK(o.count() == 0u);
}

TEST_CASE("api table (v39): the three enemy order functions") {
    const EmfApi* api = emf::core::api_table();
    CHECK(api->version >= 39u);  // the newest version is checked by its own test (test_move_orders.cpp: v40)
    CHECK(api->size >= kApiV39Size);
    CHECK(api->enemy_order != nullptr);
    CHECK(api->enemy_order_cancel != nullptr);
    CHECK(api->enemy_target != nullptr);
    EmfEnemyOrder tiny{};
    tiny.size = 8;  // an older or broken caller
    CHECK(api->enemy_order(nullptr, &tiny) == 0u);
}

TEST_CASE("enemy orders: refused: no enemy AI, a player, an ally without EMF_ORDER_ALLIES, no target, itself, a dead target, bad numbers") {
    Fake f = world();
    f.add<gen::DynamicAggro>(kOther);
    f.add<gen::SelectedTargets>(kOther);
    f.add<gen::Faction>(kOther).faction = static_cast<emf::gen::FactionType>(11);  // a tamed beast
    f.add<gen::CurrentTransform>(kOther);
    EnemyOrders o(f.deps());
    std::string why;
    CHECK(o.add(W(), kDoor, kEnemy, 30, 0, 0, &why) == 0u);  // a door has no enemy AI
    CHECK(why.find("enemy AI") != std::string::npos);
    CHECK(o.add(W(), kPlayer, kDoor, 30, 0, 0, &why) == 0u);
    CHECK(o.add(W(), kOther, kDoor, 30, 0, 0, &why) == 0u);
    CHECK(why.find("EMF_ORDER_ALLIES") != std::string::npos);
    CHECK(o.add(W(), kOther, kDoor, 30, 0, EMF_ORDER_ALLIES, &why) != 0u);
    CHECK(o.add(W(), kEnemy, 12345, 30, 0, 0, &why) == 0u);  // no such target
    CHECK(o.add(W(), kEnemy, kEnemy, 30, 0, 0, &why) == 0u);
    f.dead[kDoor] = true;
    CHECK(o.add(W(), kEnemy, kDoor, 30, 0, 0, &why) == 0u);
    f.dead[kDoor] = false;
    CHECK(o.add(W(), kEnemy, kDoor, -1, 0, 0, &why) == 0u);
    CHECK(o.add(W(), kEnemy, kDoor, 30, -5, 0, &why) == 0u);
    CHECK(o.add(W(), kEnemy, kDoor, 30, 0, 0x80, &why) == 0u);  // unknown flags
    CHECK_FALSE(held(f));
}

// Seen live: an ordered Vukah that was Idle/Roaming did not move (pick and alarmed entry written); set to Combat it
// walked 30 m to the door and one hit (with 200 damage added) broke it.
TEST_CASE("enemy orders: an idle or roaming enemy is put in its Combat state; one in an attack, hit or falling is left as it is") {
    using S = emf::gen::enemy01::State;
    const S calm[] = {S::Idle, S::Roaming, S::ReturnHome, S::Investigate, S::ReactToNoise, S::Patrol, S::Guard, S::Sleep};
    for (S st : calm) {
        CAPTURE(static_cast<int>(st));
        Fake f = world();
        f.add<gen::DynamicEnemy01BehaviorState>(kEnemy).state = st;
        EnemyOrders o(f.deps());
        std::string why;
        REQUIRE(o.add(W(), kEnemy, kDoor, 30, 0, 0, &why) != 0u);
        CHECK(f.get<gen::DynamicEnemy01BehaviorState>(kEnemy).state == S::Combat);
        f.get<gen::DynamicEnemy01BehaviorState>(kEnemy).state = S::Roaming;  // the game drops it again
        o.step(W());
        CHECK(f.get<gen::DynamicEnemy01BehaviorState>(kEnemy).state == S::Combat);
    }
    const S busy[] = {S::ControlledByAttack, S::Hit, S::Falling, S::Dead, S::Stunned, S::Spawning};
    for (S st : busy) {
        CAPTURE(static_cast<int>(st));
        Fake f = world();
        f.add<gen::DynamicEnemy01BehaviorState>(kEnemy).state = st;
        EnemyOrders o(f.deps());
        std::string why;
        REQUIRE(o.add(W(), kEnemy, kDoor, 30, 0, 0, &why) != 0u);
        CHECK(f.get<gen::DynamicEnemy01BehaviorState>(kEnemy).state == st);
    }
}

// Review I2: a creature tamed during an order (Beastmaster: faction 11) would go on attacking its new owner.
TEST_CASE("enemy orders: an order ends when its enemy becomes a player's ally (without EMF_ORDER_ALLIES) or a player") {
    Fake f = world();
    EnemyOrders o(f.deps());
    std::string why;
    REQUIRE(o.add(W(), kEnemy, kPlayer, 0, 0, 0, &why) != 0u);
    f.get<gen::Faction>(kEnemy).faction = static_cast<emf::gen::FactionType>(11);  // tamed
    o.step(W());
    CHECK(o.count() == 0u);
    CHECK_FALSE(held(f));
    CHECK(pick(f) == 0u);
}

// Review I3: the enemy's death ends its order (its id may soon be another creature's).
TEST_CASE("enemy orders: an order ends when its enemy dies") {
    Fake f = world();
    EnemyOrders o(f.deps());
    std::string why;
    REQUIRE(o.add(W(), kEnemy, kDoor, 0, 50, 0, &why) != 0u);
    f.dead[kEnemy] = true;
    o.step(W());
    CHECK(o.count() == 0u);
    CHECK(f.rules_removed.size() == 1u);
}

// Issue #42: a refused order ends nothing. The docs say so; before the fix the old order was finished before the hit
// rule was asked for, so an enemy whose new order was refused (all 64 shared hit rules in use, or hit rules off) was
// left with no order at all.
TEST_CASE("enemy orders: a refused order leaves the enemy's old order running (it ends nothing)") {
    Fake f = world();
    f.add<gen::CurrentTransform>(kOther);
    EnemyOrders o(f.deps());
    std::string why;
    REQUIRE(o.add(W(), kEnemy, kDoor, 30, 0, 0, &why) != 0u);  // an order without damage: it needs no hit rule
    f.rules_ok = false;  // the 64 shared hit rules are all in use, or hit rules are off on this build
    CHECK(o.add(W(), kEnemy, kOther, 30, 50, 0, &why) == 0u);  // refused: no hit rule for its damage
    CHECK(why.find("damage") != std::string::npos);
    CHECK(o.count() == 1u);  // the old order still runs
    CHECK(o.target_of(W(), kEnemy) == kDoor);
    CHECK(pick(f) == kDoor);
    CHECK(held(f));
    CHECK(entry(f, kDoor) != nullptr);
    CHECK(f.rules_removed.empty());  // nothing ended: no rule was added or removed
    f.now += 31000;                  // and it still ends when its own time is up
    o.step(W());
    CHECK(o.count() == 0u);
    CHECK_FALSE(held(f));
}

// Review I6: the 256 check comes before the hit rule, so a refused order never holds one of the shared rules for a
// moment; a replacement needs no free slot, since the old order gives up its own.
TEST_CASE("enemy orders: at 256 orders a new enemy is refused before its hit rule; a replacement still starts") {
    Fake f = world();
    const uint32_t slots = static_cast<uint32_t>(kEnemyOrdersMax);
    for (uint32_t e = 1000; e < 1000 + slots; ++e) {
        f.add<gen::DynamicAggro>(e).allowTargetSwitches = true;
        f.add<gen::SelectedTargets>(e);
        f.add<gen::Faction>(e).faction = static_cast<emf::gen::FactionType>(3);
        f.add<gen::CurrentTransform>(e);
    }
    EnemyOrders o(f.deps());
    std::string why;
    for (uint32_t e = 1000; e < 1000 + slots; ++e) REQUIRE(o.add(W(), e, kDoor, 30, 0, 0, &why) != 0u);
    CHECK(o.count() == kEnemyOrdersMax);
    CHECK(f.rules_added.empty());

    // a 257th enemy: refused before its rule is asked for, and nothing is given back
    const uint32_t kLate = 4000, kLateTarget = 4001;
    f.add<gen::DynamicAggro>(kLate).allowTargetSwitches = true;
    f.add<gen::SelectedTargets>(kLate);
    f.add<gen::Faction>(kLate).faction = static_cast<emf::gen::FactionType>(3);
    f.add<gen::CurrentTransform>(kLate);
    f.add<gen::CurrentTransform>(kLateTarget);
    CHECK(o.add(W(), kLate, kLateTarget, 30, 50, 0, &why) == 0u);
    CHECK(why.find("256") != std::string::npos);
    CHECK(f.rules_added.empty());    // refused before the hit rule: it never held one
    CHECK(f.rules_removed.empty());  // and there was nothing to give back
    CHECK(o.count() == kEnemyOrdersMax);

    // a replacement of one already held still starts, with damage: its old order gives up its own slot
    const uint32_t replaced = 1000;
    CHECK(o.add(W(), replaced, kLateTarget, 30, 50, 0, &why) != 0u);
    CHECK(o.count() == kEnemyOrdersMax);  // replaced, not added
    CHECK(o.target_of(W(), replaced) == kLateTarget);
    CHECK(f.rules_added.size() == 1u);    // the new order's rule, and nothing else
}

// Review I5 (the spec: never a boss): a boss is refused; its leash and its own fight stay the game's.
TEST_CASE("enemy orders: a boss (Boss or BossHealthBar) is refused") {
    for (int c = 0; c < 2; ++c) {
        Fake f = world();
        if (c == 0) f.add<gen::Boss>(kEnemy);
        else f.add<gen::BossHealthBar>(kEnemy);
        EnemyOrders o(f.deps());
        std::string why;
        CHECK(o.add(W(), kEnemy, kDoor, 30, 0, 0, &why) == 0u);
        CHECK(why.find("boss") != std::string::npos);
    }
}

// Review minor (re-graded: a tamed beast sent with EMF_ORDER_ALLIES stood still after it, as Beastmaster saw): an order
// that ends with the enemy in Combat and nothing left to fight puts it back to Idle.
TEST_CASE("enemy orders: when an order ends, an enemy in Combat with nothing picked goes back to Idle") {
    using S = emf::gen::enemy01::State;
    Fake f = world();
    f.add<gen::DynamicEnemy01BehaviorState>(kEnemy).state = S::Roaming;
    EnemyOrders o(f.deps());
    std::string why;
    const uint32_t id = o.add(W(), kEnemy, kDoor, 30, 0, 0, &why);
    REQUIRE(f.get<gen::DynamicEnemy01BehaviorState>(kEnemy).state == S::Combat);
    REQUIRE(o.cancel(id));
    CHECK(f.get<gen::DynamicEnemy01BehaviorState>(kEnemy).state == S::Idle);
}

TEST_CASE("enemy orders: cancel_enemy ends the order on that enemy (a move order starts on it)") {
    Fake f = world();
    EnemyOrders o(f.deps());
    std::string why;
    REQUIRE(o.add(W(), kEnemy, kDoor, 0, 0, 0, &why) != 0u);
    CHECK(o.order_of(W(), kEnemy) != 0u);
    CHECK(o.cancel_enemy(W(), kEnemy));
    CHECK_FALSE(held(f));
    CHECK(o.count() == 0u);
    CHECK(o.order_of(W(), kEnemy) == 0u);
    CHECK_FALSE(o.cancel_enemy(W(), kEnemy));
}
