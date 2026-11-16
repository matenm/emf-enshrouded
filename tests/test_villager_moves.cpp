// Villager moves (API v45): a villager walked to points by re-sending the game's own GoToPosition (fake world).
// Live 2026-11-16: the game's villagers roam within 30 m of home (60 m across) and are teleported home beyond 51 m;
// points 20 m and 28 m from home were walked to and held by re-sending every 2 s; a point 40 m out failed (path result
// 7, MaxIterationsReached), the villager roamed near home.
#include <doctest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/villager_moves.h"
#include "emf/emf_api.h"
#include "emf/generated/components.hpp"

using namespace emf::core;

namespace {
namespace gen = emf::gen::ecs;
namespace npc = emf::gen::active_npc;

constexpr uint32_t kVillager = 80, kOther = 81, kPlayer = 1, kNpcId = 0x1234, kOtherNpcId = 0x5678;
EmfWorld* const kWorld = reinterpret_cast<EmfWorld*>(0x1000);

// active_npc::State
constexpr uint8_t kStart = 1, kFindBed = 2, kIdle = 3, kDecideTask = 4, kFindTarget = 5, kGoToTarget = 6, kRoam = 7,
                  kSleep = 8, kSitting = 9, kInteracting = 10, kSocializing = 11, kWaitPlayer = 13, kInsideWater = 16,
                  kFinishTask = 17;

struct Fake {
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> comps, tpls;
    uint64_t now = 1000;
    struct Send {
        uint32_t npc;
        double x, y, z;
    };
    std::vector<Send> sends;
    std::vector<uint32_t> dropped;
    struct Done {
        EmfWorld* world;
        uint32_t id, status;
    };
    std::vector<Done> done;

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
    template <class T>
    T& tpl(uint32_t e) {
        auto& v = tpls[{T::kHash, e}];
        if (v.size() != sizeof(T)) v.assign(sizeof(T), 0);
        return *reinterpret_cast<T*>(v.data());
    }
    void place(uint32_t e, double x, double y, double z) {
        auto& v = comps[{gen::CurrentTransform::kHash, e}];
        v.assign(56, 0);
        const int64_t raw[3] = {std::llround(x * 4294967296.0), std::llround(y * 4294967296.0),
                                std::llround(z * 4294967296.0)};
        std::memcpy(v.data(), raw, sizeof raw);
    }
    // A villager with home (0, 10, 0), standing at (2, 10, 0), Idle, a player 6 m away; the template's radii are the
    // live ones: roaming 30 m, targetRadius 20, bedRadius 50 (the leash 50 m; the game adds 1).
    void villager(uint32_t e = kVillager, uint32_t npc_id = kNpcId) {
        add<gen::DynamicActiveNpcState>(e).state = static_cast<npc::State>(kIdle);
        add<gen::NpcCommon>(e).closestPlayerDistance = 6.0f;
        add<gen::DynamicPathWalker>(e);
        auto& saved = add<gen::ActiveNpcStateSaved>(e);
        saved.spawnPosition = {0.0f, 10.0f, 0.0f};
        auto& t = tpl<gen::ActiveNpcState>(e);
        t.npcId.value = npc_id;
        t.roamingSetup.maxRadiusFromSpawn = 30.0f;
        t.searchTargetSetup.targetRadius = 20.0f;
        t.searchTargetSetup.bedRadius = 50.0f;
        place(e, 2, 10, 0);
    }
    std::array<double, 3> pos(uint32_t e = kVillager) {
        int64_t raw[3];
        std::memcpy(raw, comps.at({gen::CurrentTransform::kHash, e}).data(), sizeof raw);
        return {raw[0] / 4294967296.0, raw[1] / 4294967296.0, raw[2] / 4294967296.0};
    }
    // walks it there in steps of at most 2 m, one game step (100 ms) each: farther than 4 m in a step is a teleport
    void go(VillagerMoves& m, double x, double y, double z, uint32_t e = kVillager) {
        for (;;) {
            const auto p = pos(e);
            const double dx = x - p[0], dy = y - p[1], dz = z - p[2];
            const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < 1e-6) return;
            const double k = d > 2.0 ? 2.0 / d : 1.0;
            place(e, p[0] + dx * k, p[1] + dy * k, p[2] + dz * k);
            step(m);
        }
    }
    void state(uint8_t s, uint8_t prev = 0, uint8_t decision = 0, uint32_t e = kVillager) {
        auto& a = get<gen::DynamicActiveNpcState>(e);
        a.state = static_cast<npc::State>(s);
        a.previousState = static_cast<npc::State>(prev);
        a.decision = static_cast<npc::Decision>(decision);
    }
    // the game took our point: Roam toward it (NpcCommon.currentTargetPosition)
    void walking_to(double x, double y, double z, uint32_t e = kVillager) {
        state(kRoam, kIdle, 1, e);
        get<gen::NpcCommon>(e).currentTargetPosition = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    }
    void walker(uint8_t walker_state, uint8_t path_result = 0, uint32_t e = kVillager) {
        auto& w = get<gen::DynamicPathWalker>(e);
        w.state = static_cast<gen::PathWalkerState>(walker_state);
        w.lastPathfindingResult = static_cast<gen::PathfindingResult>(path_result);
    }
    VillagerMoves::Deps deps() {
        VillagerMoves::Deps d;
        d.get = [this](EmfWorld*, uint32_t hash, uint32_t e, uint64_t* size) -> void* {
            auto it = comps.find({hash, e});
            if (it == comps.end()) return nullptr;
            if (size) *size = it->second.size();
            return it->second.data();
        };
        d.tpl = [this](EmfWorld*, uint32_t hash, uint32_t e, uint64_t* size) -> const void* {
            auto it = tpls.find({hash, e});
            if (it == tpls.end()) return nullptr;
            if (size) *size = it->second.size();
            return it->second.data();
        };
        d.now_ms = [this] { return now; };
        d.go_to = [this](uint32_t n, double x, double y, double z) { sends.push_back({n, x, y, z}); };
        d.drop = [this](uint32_t n) { dropped.push_back(n); };
        return d;
    }
    VillagerMoves::Callback cb() {
        return [this](EmfWorld* w, uint32_t id, uint32_t status) { done.push_back({w, id, status}); };
    }
    void step(VillagerMoves& m, uint64_t ms = 100) {
        now += ms;
        m.step(kWorld);
    }
    void steps(VillagerMoves& m, uint64_t total_ms, uint64_t each = 100) {
        for (uint64_t t = 0; t < total_ms; t += each) step(m, each);
    }
};

VillagerMoves::Order order(std::vector<VillagerMoves::Point> points, uint32_t v = kVillager) {
    VillagerMoves::Order o;
    o.villager = v;
    o.points = std::move(points);
    return o;
}

std::string refused(Fake& f, VillagerMoves& m, const VillagerMoves::Order& o) {
    std::string why;
    CHECK(m.add(kWorld, o, f.cb(), &why) == 0);
    return why;
}
bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
}  // namespace

TEST_CASE("villager moves: refused: no villager, no template, a player, set to stay, a point beyond the leash or outside the walk area, bad numbers") {
    Fake f;
    f.villager();
    VillagerMoves m(f.deps());
    CHECK(has(refused(f, m, order({{10, 10, 0}}, 0)), "no villager"));
    f.add<gen::PlayerState>(kPlayer);
    f.place(kPlayer, 0, 10, 0);
    CHECK(has(refused(f, m, order({{10, 10, 0}}, kPlayer)), "player"));
    f.place(90, 0, 10, 0);
    CHECK(has(refused(f, m, order({{10, 10, 0}}, 90)), "no villager"));  // no DynamicActiveNpcState
    {
        Fake g;
        g.villager();
        g.tpls.clear();
        VillagerMoves n(g.deps());
        CHECK(has(refused(g, n, order({{10, 10, 0}})), "template"));
        g.villager();
        g.tpl<gen::ActiveNpcState>(kVillager).npcId.value = 0;
        CHECK(has(refused(g, n, order({{10, 10, 0}})), "template"));
        g.villager();
        g.tpl<gen::ActiveNpcState>(kVillager).roamingSetup.maxRadiusFromSpawn = 0;
        CHECK(has(refused(g, n, order({{10, 10, 0}})), "walk area"));
    }
    auto& saved = f.get<gen::ActiveNpcStateSaved>(kVillager);
    saved.overrideRoamingRange = true;
    saved.roamingRange = 0;
    CHECK(has(refused(f, m, order({{10, 10, 0}})), "told it to stay"));
    saved.roamingRange = 10;  // the player's own range: 10 m
    CHECK(has(refused(f, m, order({{12, 10, 0}})), "walk area"));
    std::string why;
    CHECK(m.add(kWorld, order({{10.5, 10, 0}}), f.cb(), &why) != 0);  // within 10 + 1
    CHECK(m.cancel_villager(kWorld, kVillager));
    saved.overrideRoamingRange = false;
    CHECK(has(refused(f, m, order({{0, 10, 32}})), "walk area"));  // 30 + 1 < 32 across
    CHECK(has(refused(f, m, order({{0, 62, 10}})), "teleport it home"));  // 53 m in 3D: beyond the 50 m leash
    CHECK(m.add(kWorld, order({{0, 10, 30.5}}), f.cb(), &why) != 0);
    CHECK(m.cancel_villager(kWorld, kVillager));
    CHECK(has(refused(f, m, order({})), "1 to 16"));
    CHECK(has(refused(f, m, order(std::vector<VillagerMoves::Point>(17, {1, 10, 1}))), "1 to 16"));
    CHECK(has(refused(f, m, order({{std::nan(""), 10, 0}})), "number"));
    auto o = order({{10, 10, 0}});
    o.arrive = 0.2;
    CHECK(has(refused(f, m, o), "arrive"));
    o = order({{10, 10, 0}});
    o.seconds = 4000;
    CHECK(has(refused(f, m, o), "seconds"));
    o = order({{10, 10, 0}});
    o.stuck_seconds = 1;
    CHECK(has(refused(f, m, o), "stuck"));
    o = order({{10, 10, 0}});
    o.hold_seconds = 3601;
    CHECK(has(refused(f, m, o), "hold"));
    f.comps.erase({gen::CurrentTransform::kHash, kVillager});
    CHECK(has(refused(f, m, order({{10, 10, 0}})), "position"));
}

TEST_CASE("villager moves: a move sends GoToPosition at once with the template npcId, then only when the order was dropped, at most once a second") {
    Fake f;
    f.villager();
    VillagerMoves m(f.deps());
    std::string why;
    const uint32_t id = m.add(kWorld, order({{20, 10, 0}}), f.cb(), &why);
    REQUIRE_MESSAGE(id != 0, why);
    CHECK(m.moving(kWorld, kVillager) == id);
    REQUIRE(f.sends.size() == 1);
    CHECK(f.sends[0].npc == kNpcId);  // the template's npcId, never the entity id
    CHECK(f.sends[0].x == doctest::Approx(20));
    CHECK(f.sends[0].y == doctest::Approx(10));
    f.walking_to(20, 10, 0);
    for (int i = 0; i < 30; ++i) {  // walks: Roam toward our point
        f.place(kVillager, 2 + i * 0.5, 10, 0);
        f.step(m);
    }
    CHECK(f.sends.size() == 1);
    f.state(kIdle, kRoam);  // dropped (interrupted): the last send was 3 s ago, so at once
    f.step(m);
    CHECK(f.sends.size() == 2);
    f.steps(m, 800);
    CHECK(f.sends.size() == 2);  // not again within a second
}

TEST_CASE("villager moves: a dropped order is sent again at once when the last send is a second old, never twice within a second") {
    Fake f;
    f.villager();
    VillagerMoves m(f.deps());
    std::string why;
    REQUIRE(m.add(kWorld, order({{20, 10, 0}}), f.cb(), &why) != 0);
    f.state(kIdle, kRoam);  // still Idle: the game has not taken it yet
    f.step(m, 100);
    f.step(m, 100);
    CHECK(f.sends.size() == 1);  // within a second of the first send
    f.steps(m, 900);
    CHECK(f.sends.size() == 2);
    f.steps(m, 500);
    CHECK(f.sends.size() == 2);
    f.steps(m, 600);
    CHECK(f.sends.size() == 3);
}

TEST_CASE("villager moves: re-sends from Idle, DecideTask, FindTarget, Roam elsewhere, GoToTarget, WaitingCloseToPlayer and FinishTask; waits in Sitting, Interacting and NpcSocializing") {
    for (uint8_t s : {kIdle, kDecideTask, kFindTarget, kGoToTarget, kRoam, kWaitPlayer, kFinishTask}) {
        Fake f;
        f.villager();
        VillagerMoves m(f.deps());
        std::string why;
        REQUIRE(m.add(kWorld, order({{20, 10, 0}}), f.cb(), &why) != 0);
        f.state(s);
        if (s == kRoam) f.get<gen::NpcCommon>(kVillager).currentTargetPosition = {5, 10, 5};  // its own roam point
        f.steps(m, 1100);
        CHECK_MESSAGE(f.sends.size() == 2, "state ", int(s));
    }
    for (uint8_t s : {kSitting, kInteracting, kSocializing}) {
        Fake f;
        f.villager();
        VillagerMoves m(f.deps());
        std::string why;
        REQUIRE(m.add(kWorld, order({{20, 10, 0}}), f.cb(), &why) != 0);
        f.state(s);
        f.steps(m, 3000);
        CHECK_MESSAGE(f.sends.size() == 1, "state ", int(s));
    }
}

TEST_CASE("villager moves: paused with no player within 150 m, in Start, InsideWater, FindBed or Sleep, deciding to sleep, or while a player talks to it; seconds still run") {
    auto paused_case = [](auto pause) {
        Fake f;
        f.villager();
        VillagerMoves m(f.deps());
        std::string why;
        REQUIRE(m.add(kWorld, order({{20, 10, 0}}), f.cb(), &why) != 0);
        pause(f);
        f.steps(m, 60000, 500);
        CHECK(f.sends.size() == 1);    // nothing sent while paused
        CHECK(f.done.empty());         // the stuck clock waited
    };
    paused_case([](Fake& f) { f.get<gen::NpcCommon>(kVillager).closestPlayerDistance = 200.0f; });
    paused_case([](Fake& f) { f.state(kStart); });
    paused_case([](Fake& f) { f.state(kInsideWater); });
    paused_case([](Fake& f) { f.state(kFindBed); });
    paused_case([](Fake& f) { f.state(kSleep); });
    paused_case([](Fake& f) { f.state(kIdle, 0, 5 /* WantsToSleep */); });
    for (uint8_t bit : {1, 2, 8}) paused_case([bit](Fake& f) { f.get<gen::NpcCommon>(kVillager).interactionStates = bit; });
    // seconds run on while paused
    Fake f;
    f.villager();
    VillagerMoves m(f.deps());
    auto o = order({{20, 10, 0}});
    o.seconds = 30;
    std::string why;
    const uint32_t id = m.add(kWorld, o, f.cb(), &why);
    f.state(kSleep);
    f.steps(m, 31000, 500);
    REQUIRE(f.done.size() == 1);
    CHECK(f.done[0].id == id);
    CHECK(f.done[0].status == EMF_MOVE_TIMEOUT);
}

TEST_CASE("villager moves: arrived within arrive across and arrive + 2 up or down; also when Roam ended at the walker's TargetReached within arrive + 3; earlier points go on to the next at once") {
    Fake f;
    f.villager();
    VillagerMoves m(f.deps());
    std::string why;
    const uint32_t id = m.add(kWorld, order({{10, 10, 0}, {20, 10, 0}}), f.cb(), &why);
    REQUIRE(id != 0);
    f.walking_to(10, 10, 0);
    f.go(m, 8.5, 13.5, 0);  // 1.5 m across, 3.5 up: within 2 and 2 + 2
    CHECK(f.done.empty());
    REQUIRE(f.sends.size() == 2);  // on to the second point at once
    CHECK(f.sends[1].x == doctest::Approx(20));
    f.walking_to(20, 10, 0);
    f.go(m, 15, 10, 0);  // 5 m short: the walker reached the path's last node (up to 5 m from the point)
    f.walker(8 /* TargetReached */);
    f.state(kIdle, kRoam);
    f.step(m);
    REQUIRE(f.done.size() == 1);
    CHECK(f.done[0].id == id);
    CHECK(f.done[0].status == EMF_MOVE_ARRIVED);
    CHECK(f.done[0].world == kWorld);
    CHECK(m.moving(kWorld, kVillager) == 0);
    // farther than arrive + 3 with TargetReached is no arrival
    Fake g;
    g.villager();
    VillagerMoves n(g.deps());
    REQUIRE(n.add(kWorld, order({{20, 10, 0}}), g.cb(), &why) != 0);
    g.go(n, 14, 10, 0);
    g.walker(8);
    g.state(kIdle, kRoam);
    g.step(n);
    CHECK(g.done.empty());
}

TEST_CASE("villager moves: stuck after stuck_seconds without a metre closer, at once on a jump of more than 4 m in one step, or the second time the path is not found for the same point") {
    {
        Fake f;
        f.villager();
        VillagerMoves m(f.deps());
        auto o = order({{20, 10, 0}});
        o.stuck_seconds = 5;
        std::string why;
        REQUIRE(m.add(kWorld, o, f.cb(), &why) != 0);
        f.walking_to(20, 10, 0);
        f.place(kVillager, 5, 10, 0);  // 3 m closer
        f.step(m);
        f.steps(m, 4800);
        CHECK(f.done.empty());
        f.steps(m, 400);
        REQUIRE(f.done.size() == 1);
        CHECK(f.done[0].status == EMF_MOVE_STUCK);
        REQUIRE(!f.sends.empty());
        CHECK(f.sends.back().x == doctest::Approx(5));  // told to stop where it stands
    }
    {
        Fake f;
        f.villager();
        VillagerMoves m(f.deps());
        std::string why;
        REQUIRE(m.add(kWorld, order({{20, 10, 0}}), f.cb(), &why) != 0);
        f.walking_to(20, 10, 0);
        f.place(kVillager, 5, 10, 0);
        f.step(m);
        f.place(kVillager, 0, 10, 0);  // 5 m in one step: the game teleported it (home)
        f.step(m);
        REQUIRE(f.done.size() == 1);
        CHECK(f.done[0].status == EMF_MOVE_STUCK);
    }
    {
        // the live 40 m case: each send answered with path result 7 (MaxIterationsReached), the villager roams elsewhere
        Fake f;
        f.villager();
        f.get<gen::ActiveNpcStateSaved>(kVillager).overrideRoamingRange = true;
        f.get<gen::ActiveNpcStateSaved>(kVillager).roamingRange = 45;  // the area allows it; the path does not
        VillagerMoves m(f.deps());
        std::string why;
        REQUIRE(m.add(kWorld, order({{0, 10, 40}}), f.cb(), &why) != 0);
        f.walking_to(0, 10, 40);
        f.walker(2, 7);
        f.step(m);
        CHECK(f.done.empty());  // once
        f.state(kRoam);
        f.get<gen::NpcCommon>(kVillager).currentTargetPosition = {3, 10, 4};  // its own roam point instead
        f.walker(2, 0);
        f.steps(m, 1100);
        REQUIRE(f.sends.size() == 2);
        f.walking_to(0, 10, 40);
        f.walker(2, 7);
        f.step(m);
        REQUIRE(f.done.size() == 1);
        CHECK(f.done[0].status == EMF_MOVE_STUCK);
    }
}

TEST_CASE("villager moves: gone when the villager loses its AI or its position, or its npcId changes") {
    for (int how = 0; how < 3; ++how) {
        Fake f;
        f.villager();
        VillagerMoves m(f.deps());
        std::string why;
        REQUIRE(m.add(kWorld, order({{20, 10, 0}}), f.cb(), &why) != 0);
        if (how == 0) f.comps.erase({gen::DynamicActiveNpcState::kHash, kVillager});
        if (how == 1) f.comps.erase({gen::CurrentTransform::kHash, kVillager});
        if (how == 2) f.tpl<gen::ActiveNpcState>(kVillager).npcId.value = 0x9999;  // the id names another entity now
        const size_t sent = f.sends.size();
        f.step(m);
        REQUIRE(f.done.size() == 1);
        CHECK(f.done[0].status == EMF_MOVE_GONE);
        CHECK(f.sends.size() == sent);  // nothing sent to what is gone
    }
}

TEST_CASE("villager moves: cancel or a new move on the villager sends 'go to where you stand' and drops its queued sends; the callback runs after the move is gone and may start another") {
    Fake f;
    f.villager();
    VillagerMoves m(f.deps());
    std::string why;
    const uint32_t a = m.add(kWorld, order({{20, 10, 0}}), f.cb(), &why);
    f.place(kVillager, 5, 10, 1);
    CHECK(m.cancel(a));
    REQUIRE(f.done.size() == 1);
    CHECK(f.done[0].status == EMF_MOVE_CANCELLED);
    CHECK(f.dropped == std::vector<uint32_t>{kNpcId});
    REQUIRE(f.sends.size() == 2);
    CHECK(f.sends[1].x == doctest::Approx(5));  // where it stands
    CHECK(f.sends[1].z == doctest::Approx(1));
    CHECK_FALSE(m.cancel(a));
    CHECK(m.moving(kWorld, kVillager) == 0);
    // a new move on it ends the old one (cancelled) and sends only the new point
    const uint32_t b = m.add(kWorld, order({{20, 10, 0}}), f.cb(), &why);
    const size_t sent = f.sends.size();
    const uint32_t c = m.add(kWorld, order({{0, 10, 20}}), f.cb(), &why);
    REQUIRE(c != 0);
    REQUIRE(f.done.size() == 2);
    CHECK(f.done[1].id == b);
    CHECK(f.done[1].status == EMF_MOVE_CANCELLED);
    REQUIRE(f.sends.size() == sent + 1);
    CHECK(f.sends.back().z == doctest::Approx(20));
    CHECK(m.moving(kWorld, kVillager) == c);
    // a callback that starts another move
    uint32_t next = 0;
    const uint32_t d = m.add(kWorld, order({{20, 10, 0}}),
                             [&](EmfWorld* w, uint32_t, uint32_t) { next = m.add(w, order({{0, 10, 10}}), nullptr, nullptr); },
                             &why);
    CHECK(m.cancel(d));
    CHECK(next != 0);
    CHECK(m.moving(kWorld, kVillager) == next);
    // cancel later (not now): ends at the next step
    CHECK(m.cancel(next, false));
    CHECK(m.moving(kWorld, kVillager) == 0);
    f.step(m);
    CHECK(m.count() == 0);
}

TEST_CASE("villager moves: hold: the arrival callback comes once; it holds quietly, re-sent every 2 s; it ends quietly after hold_seconds, on cancel, stay set, its home moved, a teleport, or no way back") {
    auto arrive_and_hold = [](Fake& f, VillagerMoves& m, double hold_s, bool forever) {
        f.villager();
        auto o = order({{20, 10, 0}});
        o.hold_seconds = hold_s;
        o.hold_forever = forever;
        std::string why;
        const uint32_t id = m.add(kWorld, o, f.cb(), &why);
        REQUIRE_MESSAGE(id != 0, why);
        f.walking_to(20, 10, 0);
        f.go(m, 19.5, 10, 0);
        REQUIRE(f.done.size() == 1);
        CHECK(f.done[0].status == EMF_MOVE_ARRIVED);
        CHECK(m.moving(kWorld, kVillager) == id);  // holding
        return id;
    };
    {
        Fake f;
        VillagerMoves m(f.deps());
        arrive_and_hold(f, m, 30, false);
        f.state(kIdle, kRoam);  // the game ended its Roam there
        const size_t sent = f.sends.size();
        f.steps(m, 10000);
        CHECK(f.sends.size() >= sent + 4);  // every 2 s
        CHECK(f.sends.size() <= sent + 6);
        CHECK(f.sends.back().x == doctest::Approx(20));
        // a player close by: the hold waits (never pulls it away from the player)
        f.state(kWaitPlayer);
        const size_t before = f.sends.size();
        f.steps(m, 6000);
        CHECK(f.sends.size() == before);
        f.state(kIdle);
        f.steps(m, 15000);
        CHECK(m.moving(kWorld, kVillager) == 0);  // 30 s are over
        CHECK(f.done.size() == 1);                // quietly
    }
    {
        Fake f;
        VillagerMoves m(f.deps());
        const uint32_t id = arrive_and_hold(f, m, 0, true);
        f.state(kIdle, kRoam);
        f.steps(m, 600000, 1000);
        CHECK(m.moving(kWorld, kVillager) == id);  // until cancelled
        CHECK(m.cancel(id));
        CHECK(f.done.size() == 1);  // no second callback
        CHECK(m.moving(kWorld, kVillager) == 0);
    }
    for (int how = 0; how < 4; ++how) {
        Fake f;
        VillagerMoves m(f.deps());
        arrive_and_hold(f, m, 600, false);
        f.state(kIdle, kRoam);
        if (how == 0) {
            f.get<gen::ActiveNpcStateSaved>(kVillager).overrideRoamingRange = true;  // the player says stay
            f.get<gen::ActiveNpcStateSaved>(kVillager).roamingRange = 0;
        }
        if (how == 1) f.get<gen::ActiveNpcStateSaved>(kVillager).spawnPosition = {30, 10, 30};  // a new home
        if (how == 2) f.place(kVillager, 0, 10, 0);  // teleported (20 m in one step)
        if (how == 3) {  // led away and cannot get back for stuck_seconds (15)
            f.go(m, 12, 10, 0);
            f.state(kRoam);
            f.get<gen::NpcCommon>(kVillager).currentTargetPosition = {20, 10, 0};
            f.steps(m, 15500);
        }
        f.step(m);
        CHECK_MESSAGE(m.moving(kWorld, kVillager) == 0, "case ", how);
        CHECK_MESSAGE(f.done.size() == 1, "case ", how);
    }
    {
        // cancel inside the arrival callback ends the hold quietly
        Fake f;
        f.villager();
        VillagerMoves m(f.deps());
        auto o = order({{20, 10, 0}});
        o.hold_seconds = 60;
        int calls = 0;
        std::string why;
        uint32_t id = 0;
        id = m.add(kWorld, o, [&](EmfWorld*, uint32_t got, uint32_t) { ++calls; m.cancel(got); }, &why);
        f.go(m, 19.5, 10, 0);
        f.steps(m, 3000);
        CHECK(calls == 1);
        CHECK(m.moving(kWorld, kVillager) == 0);
        CHECK(id != 0);
    }
}

TEST_CASE("villager moves: world reset ends walking moves cancelled with world null, holds quietly; at most 256 moves") {
    Fake f;
    f.villager(kVillager, kNpcId);
    f.villager(kOther, kOtherNpcId);
    VillagerMoves m(f.deps());
    std::string why;
    const uint32_t a = m.add(kWorld, order({{20, 10, 0}}, kVillager), f.cb(), &why);
    auto o = order({{3, 10, 0}}, kOther);
    o.hold_seconds = 60;
    REQUIRE(m.add(kWorld, o, f.cb(), &why) != 0);
    f.step(m);  // the other one arrives (it stands 1 m from its point) and holds
    REQUIRE(f.done.size() == 1);
    const size_t sent = f.sends.size();
    m.world_reset();
    REQUIRE(f.done.size() == 2);
    CHECK(f.done[1].id == a);
    CHECK(f.done[1].world == nullptr);
    CHECK(f.done[1].status == EMF_MOVE_CANCELLED);
    CHECK(m.count() == 0);
    CHECK(f.sends.size() == sent);  // nothing sent with a world that is gone
    // the limit
    Fake g;
    VillagerMoves n(g.deps());
    for (uint32_t i = 0; i < kVillagerMovesMax; ++i) {
        g.villager(1000 + i, 0x100000 + i);
        REQUIRE(n.add(kWorld, order({{10, 10, 0}}, 1000 + i), nullptr, &why) != 0);
    }
    g.villager(5000, 0x200000);
    CHECK(has(refused(g, n, order({{10, 10, 0}}, 5000)), "too many"));
    CHECK(n.add(kWorld, order({{12, 10, 0}}, 1000), nullptr, &why) != 0);  // a new move on a moving villager is fine
}

TEST_CASE("villager moves: the emf_moves queue keeps one GoToPosition per villager per step (the latest); dropped ones go; what was not sent goes first next step") {
    NpcSendQueue q;
    q.push(kNpcId, 1, 10, 1);
    q.push(kOtherNpcId, 2, 10, 2);
    q.push(kNpcId, 3, 10, 3);  // the latest point for that villager wins, in its old place
    auto all = q.take();
    REQUIRE(all.size() == 2);
    CHECK(all[0].npc == kNpcId);
    CHECK(all[0].x == doctest::Approx(3));
    CHECK(all[1].npc == kOtherNpcId);
    CHECK(q.size() == 0);
    q.push(kNpcId, 4, 10, 4);
    q.push(kOtherNpcId, 5, 10, 5);
    q.drop(kNpcId);
    all = q.take();
    REQUIRE(all.size() == 1);
    CHECK(all[0].npc == kOtherNpcId);
    // the program could send only part: the rest goes first, unless a newer send names the same villager
    q.push(kOtherNpcId, 6, 10, 6);
    q.put_back({{kNpcId, 7, 10, 7}, {kOtherNpcId, 5, 10, 5}});
    all = q.take();
    REQUIRE(all.size() == 2);
    CHECK(all[0].npc == kNpcId);
    CHECK(all[1].npc == kOtherNpcId);
    CHECK(all[1].x == doctest::Approx(6));
    for (uint32_t i = 0; i < kNpcQueueMax + 10; ++i) q.push(i + 1, 0, 0, 0);
    CHECK(q.size() == kNpcQueueMax);
}
