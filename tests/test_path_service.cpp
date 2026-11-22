// path_find (API v37): the queries' life in EMF's path service, over a fake engine (the game's create / poll / destroy).
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "core/api_sizes.h"
#include "core/path_service.h"
#include "emf/emf_api.h"
#include "emf/generated/components.hpp"

using namespace emf::core;

namespace {
struct FakeEngine {
    uint64_t next = 100;
    std::map<uint64_t, PathAnswer> answers;  // set: the game finished that query
    std::vector<uint64_t> created, destroyed;
    std::vector<EmfPathQuery> queries;
    std::vector<uint32_t> owners;
    std::vector<uint32_t> gone;  // entities that no longer exist
    PathEngine engine() {
        PathEngine e;
        e.create = [this](const EmfPathQuery& q, uint32_t owner, const PathCreature*) {
            queries.push_back(q);
            owners.push_back(owner);
            created.push_back(next);
            return next++;
        };
        e.poll = [this](uint64_t id) {
            auto it = answers.find(id);
            return it == answers.end() ? PathAnswer{} : it->second;
        };
        e.destroy = [this](uint64_t id, uint32_t) { destroyed.push_back(id); };
        e.exists = [this](uint32_t entity) {
            for (uint32_t g : gone)
                if (g == entity) return false;
            return true;
        };
        return e;
    }
    void finish(uint64_t id, uint32_t game_result, std::vector<EmfPathPoint> pts) {
        PathAnswer a;
        a.done = true;
        a.game_result = game_result;
        a.points = std::move(pts);
        answers[id] = a;
    }
};

EmfPathQuery query(float tx = 10, float tz = 0) {
    EmfPathQuery q{};
    q.size = sizeof q;
    q.from[0] = 0;
    q.from[1] = 50;
    q.from[2] = 0;
    q.to[0] = tx;
    q.to[1] = 50;
    q.to[2] = tz;
    return q;
}

void cb_noop(EmfWorld*, uint32_t, const EmfPathResult*, const EmfPathPoint*, void*) {}
}  // namespace

TEST_CASE("path service: a query is created on the next step, polled until done, destroyed, and answered once") {
    PathService s;
    FakeEngine fe;
    std::string why;
    const uint32_t id = s.request(query(), 7, cb_noop, nullptr, 1000, &why);
    REQUIRE(id != 0);
    CHECK(s.open() == 1u);
    CHECK(s.step(fe.engine(), 1010).empty());  // created, no answer yet
    REQUIRE(fe.created.size() == 1u);
    CHECK(fe.owners[0] == 7u);
    CHECK(s.step(fe.engine(), 1020).empty());  // still searching
    fe.finish(fe.created[0], 1, {{3, 50, 0, 0}, {6, 51, 0, 2}, {10, 51, 0, 0}});
    const auto done = s.step(fe.engine(), 1030);
    REQUIRE(done.size() == 1u);
    CHECK(done[0].id == id);
    CHECK(done[0].cb == cb_noop);
    CHECK(done[0].result.size == sizeof(EmfPathResult));
    CHECK(done[0].result.status == EMF_PATH_FOUND);
    CHECK(done[0].result.reaches == 1);
    CHECK(done[0].result.count == 3u);
    CHECK(done[0].points[1].move == 2u);
    CHECK(done[0].result.length == doctest::Approx(3 + std::sqrt(9.0 + 1.0) + 4));  // from `from` along the points
    CHECK(fe.destroyed == fe.created);
    CHECK(s.open() == 0u);
    CHECK(s.step(fe.engine(), 1040).empty());  // answered once
}

TEST_CASE("path service: FOUND that stops short of the target (the closest-node fallback) does not reach it") {
    PathService s;
    FakeEngine fe;
    EmfPathQuery q = query(40, 0);
    q.closest = 1;
    REQUIRE(s.request(q, 7, cb_noop, nullptr, 0, nullptr));
    s.step(fe.engine(), 10);
    fe.finish(fe.created[0], 1, {{5, 50, 0, 0}, {20, 50, 0, 0}});  // ends at a wall 20 m short
    const auto done = s.step(fe.engine(), 20);
    REQUIRE(done.size() == 1u);
    CHECK(done[0].result.status == EMF_PATH_FOUND);
    CHECK(done[0].result.reaches == 0);
}

TEST_CASE("path service: the game's results become EMF's statuses; FOUND with no points is no path") {
    CHECK(path_status(1, 3) == EMF_PATH_FOUND);
    CHECK(path_status(1, 0) == EMF_PATH_NO_PATH);
    CHECK(path_status(2, 0) == EMF_PATH_NO_PATH);
    CHECK(path_status(3, 0) == EMF_PATH_NO_PATH);  // NoFreePath
    CHECK(path_status(4, 0) == EMF_PATH_NO_PATH);  // NoOccupiedPath
    CHECK(path_status(5, 0) == EMF_PATH_NO_START);
    CHECK(path_status(6, 0) == EMF_PATH_NO_TARGET);
    CHECK(path_status(7, 0) == EMF_PATH_TOO_LONG);
    CHECK(path_status(8, 0) == EMF_PATH_NO_MEMORY);
    CHECK(path_status(9, 0) == EMF_PATH_NO_MEMORY);
    CHECK(path_status(0, 0) == EMF_PATH_OTHER);
    CHECK(path_status(42, 0) == EMF_PATH_OTHER);
}

TEST_CASE("path service: refused at once: a small size, bad numbers, too many nodes, unknown moves, 32 open") {
    PathService s;
    std::string why;
    EmfPathQuery q = query();
    q.size = 8;
    CHECK(s.request(q, 7, cb_noop, nullptr, 0, &why) == 0u);
    q = query();
    q.to[1] = std::numeric_limits<float>::quiet_NaN();
    CHECK(s.request(q, 7, cb_noop, nullptr, 0, &why) == 0u);
    CHECK(why.find("numbers") != std::string::npos);
    q = query(1e7f, 0);
    CHECK(s.request(q, 7, cb_noop, nullptr, 0, &why) == 0u);
    q = query(150, 150);  // 212 m: the game answers nothing over 200 m without searching
    CHECK(s.request(q, 7, cb_noop, nullptr, 0, &why) == 0u);
    CHECK(why.find("200 m") != std::string::npos);
    CHECK(s.request(query(140, 140), 7, cb_noop, nullptr, 0, &why) != 0u);  // 198 m
    q = query();
    q.max_nodes = 20001;
    CHECK(s.request(q, 7, cb_noop, nullptr, 0, &why) == 0u);
    q = query();
    q.width = -1;
    CHECK(s.request(q, 7, cb_noop, nullptr, 0, &why) == 0u);
    q = query();
    q.moves = 0x8000;
    CHECK(s.request(q, 7, cb_noop, nullptr, 0, &why) == 0u);
    CHECK(s.request(query(), 0, cb_noop, nullptr, 0, &why) == 0u);  // no owner
    CHECK(why.find("owner") != std::string::npos);
    PathService full;
    for (size_t i = 0; i < kMaxOpenPaths; ++i) REQUIRE(full.request(query(), 7, cb_noop, nullptr, 0, &why) != 0u);
    CHECK(full.request(query(), 7, cb_noop, nullptr, 0, &why) == 0u);
    CHECK(why.find("32") != std::string::npos);
}

TEST_CASE("path service: at most 8 new queries a step; each id once") {
    PathService s;
    FakeEngine fe;
    std::vector<uint32_t> ids;
    for (int i = 0; i < 12; ++i) ids.push_back(s.request(query(), 7, cb_noop, nullptr, 0, nullptr));
    for (size_t i = 1; i < ids.size(); ++i) CHECK(ids[i] != ids[i - 1]);
    s.step(fe.engine(), 10);
    CHECK(fe.created.size() == kPathCreatesPerStep);
    s.step(fe.engine(), 20);
    CHECK(fe.created.size() == 12u);
}

TEST_CASE("path service: cancel before and after the game has it; the owner gone; no answer in 10 s") {
    PathService s;
    FakeEngine fe;
    const uint32_t a = s.request(query(), 7, cb_noop, nullptr, 0, nullptr);
    CHECK(s.cancel(a));
    auto done = s.step(fe.engine(), 10);
    REQUIRE(done.size() == 1u);
    CHECK(done[0].result.status == EMF_PATH_CANCELLED);
    CHECK(fe.created.empty());  // never sent
    CHECK_FALSE(s.cancel(a));   // gone

    const uint32_t b = s.request(query(), 7, cb_noop, nullptr, 20, nullptr);
    s.step(fe.engine(), 30);
    CHECK(s.cancel(b));
    done = s.step(fe.engine(), 40);
    REQUIRE(done.size() == 1u);
    CHECK(done[0].result.status == EMF_PATH_CANCELLED);
    CHECK(fe.destroyed == fe.created);

    const uint32_t c = s.request(query(), 9, cb_noop, nullptr, 50, nullptr);
    s.step(fe.engine(), 60);
    fe.gone.push_back(9);
    done = s.step(fe.engine(), 70);
    REQUIRE(done.size() == 1u);
    CHECK(done[0].id == c);
    CHECK(done[0].result.status == EMF_PATH_CANCELLED);

    const uint32_t d = s.request(query(), 7, cb_noop, nullptr, 100, nullptr);
    s.step(fe.engine(), 110);
    CHECK(s.step(fe.engine(), 100 + kPathTimeoutMs - 1).empty());
    done = s.step(fe.engine(), 100 + kPathTimeoutMs + 20);
    REQUIRE(done.size() == 1u);
    CHECK(done[0].id == d);
    CHECK(done[0].result.status == EMF_PATH_OTHER);
    CHECK(fe.destroyed.size() == fe.created.size());
}

TEST_CASE("path service: a new server world ends every query as cancelled, without calling the old world's game") {
    PathService s;
    FakeEngine fe;
    s.request(query(), 7, cb_noop, nullptr, 0, nullptr);
    s.request(query(), 7, cb_noop, nullptr, 0, nullptr);
    s.step(fe.engine(), 10);
    const auto done = s.world_reset();
    CHECK(done.size() == 2u);
    for (const auto& d : done) CHECK(d.result.status == EMF_PATH_CANCELLED);
    CHECK(fe.destroyed.empty());
    CHECK(s.open() == 0u);
}

TEST_CASE("path service: at most 512 points are kept; the count says how many were kept") {
    PathService s;
    FakeEngine fe;
    REQUIRE(s.request(query(190, 0), 7, cb_noop, nullptr, 0, nullptr));
    s.step(fe.engine(), 10);
    std::vector<EmfPathPoint> pts;
    for (int i = 1; i <= 700; ++i) pts.push_back({float(i) * 190.0f / 700.0f, 50, 0, 0});
    fe.finish(fe.created[0], 1, pts);
    const auto done = s.step(fe.engine(), 20);
    REQUIRE(done.size() == 1u);
    CHECK(done[0].result.count == kMaxPathPoints);
    CHECK(done[0].points.size() == kMaxPathPoints);
    CHECK(done[0].result.reaches == 1);  // judged on the game's last point, not the last kept one
}

TEST_CASE("path query bytes: EMF's fields over the game's defaults, at the offsets the game reads") {
    uint8_t b[kPathQueryBytes];
    for (auto& x : b) x = 0xAB;  // stands in for the game's defaults: what EMF does not set stays
    EmfPathQuery q = query(10, 4);
    q.from[1] = 51.5f;
    q.moves = EMF_PATH_JUMP_UP | EMF_PATH_JUMP_DOWN;  // walk is added
    q.max_jump_up = 4;
    q.max_jump_down = 6;
    q.width = 1.2f;   // 3 half metres (rounded up)
    q.height = 2.0f;  // 4
    q.closest = 1;
    q.max_nodes = 0;  // EMF's default
    fill_path_query(b, q, 77);
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, b + o, 4); return v; };
    auto f32 = [&](size_t o) { float v; std::memcpy(&v, b + o, 4); return v; };
    auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, b + o, 2); return v; };
    CHECK(u32(0x00) == 77u);
    CHECK(f32(0x10) == 0.0f);
    CHECK(f32(0x14) == 51.5f);
    CHECK(f32(0x1c) == 10.0f);
    CHECK(f32(0x24) == 4.0f);
    CHECK(b[0x36] == 2);  // UseClosestTargetNodeAsFallback
    CHECK(u32(0x48) == kPathDefaultNodes);
    CHECK(u16(0x54) == (EMF_PATH_WALK | EMF_PATH_JUMP_UP | EMF_PATH_JUMP_DOWN));
    CHECK(b[0x56] == 4);
    CHECK(b[0x57] == 6);
    CHECK(b[0xca] == 1);         // the time-slice flag
    CHECK(u32(0xf4) == 4u);      // start search radius vertical (half metres), as the game's own callers
    CHECK(u32(0xfc) == 4u);      // search radius vertical
    CHECK(u32(0x100) == 3u);     // min horizontal space
    CHECK(u32(0x104) == 4u);     // min vertical space
    CHECK(b[0x28] == 0xAB);      // priority: the game's default kept
    CHECK(b[0x2a] == 0xAB);      // unknown bytes kept
    CHECK(f32(0x6c) != 0.0f);    // the costs kept (0xABABABAB here)
    q.closest = 0;
    q.max_nodes = 9000;
    q.width = 0;
    fill_path_query(b, q, 77);
    CHECK(b[0x36] == 0);
    CHECK(u32(0x48) == 9000u);
    CHECK(u32(0x100) == 0u);
}

TEST_CASE("path answer text: what the console's path command prints") {
    EmfPathResult r{};
    r.size = sizeof r;
    r.status = EMF_PATH_FOUND;
    r.reaches = 1;
    r.count = 3;
    r.length = 52.24f;
    const EmfPathPoint pts[3] = {{1, 2, 3, 0}, {4, 5, 6, 2}, {10, 50, 4, 5}};
    CHECK(path_answer_text(7, r, pts) ==
          "path 7: found, reaches the target, 3 points, 52.2 m, ends at (10.0, 50.0, 4.0); 1 jump, 1 door");
    r.reaches = 0;
    CHECK(path_answer_text(7, r, pts).find("found, stops short of the target") != std::string::npos);
    r = EmfPathResult{};
    r.status = EMF_PATH_NO_START;
    CHECK(path_answer_text(8, r, nullptr) == "path 8: no path grid at the start (not loaded, or inside something)");
    r.status = EMF_PATH_CANCELLED;
    CHECK(path_answer_text(9, r, nullptr) == "path 9: cancelled");
}

TEST_CASE("path answer text (0.6.11): a door point names its door entity") {
    EmfPathResult r{};
    r.size = sizeof r;
    r.status = EMF_PATH_FOUND;
    r.reaches = 1;
    r.count = 3;
    r.length = 52.24f;
    const EmfPathPoint pts[3] = {{1, 2, 3, 0}, {4, 5, 6, 5}, {10, 50, 4, 5}};
    const uint32_t doors[3] = {0, 812, 0};  // the game names no door at the last point
    r.doors = doors;
    CHECK(path_answer_text(7, r, pts) ==
          "path 7: found, reaches the target, 3 points, 52.2 m, ends at (10.0, 50.0, 4.0); 0 jumps, 2 doors "
          "(812 at point 2)");
    r.size = kPathResultV37Size;  // a result without the door array: not read
    CHECK(path_answer_text(7, r, pts).find("812") == std::string::npos);
}

TEST_CASE("path service (review M2): a cancel while the step talks to the game still ends the query cancelled") {
    PathService s;
    FakeEngine fe;
    const uint32_t id = s.request(query(), 7, cb_noop, nullptr, 0, nullptr);
    s.step(fe.engine(), 10);
    fe.finish(fe.created[0], 1, {{10, 50, 0, 0}});
    PathEngine e = fe.engine();
    const auto poll = e.poll;
    e.poll = [&](uint64_t gid) {
        s.cancel(id);  // another thread, in the middle of the step
        return poll(gid);
    };
    const auto done = s.step(e, 20);
    REQUIRE(done.size() == 1u);
    CHECK(done[0].result.status == EMF_PATH_CANCELLED);
    CHECK(done[0].result.count == 0u);
}

// A modder's base (0.6.9, 2026-11-14): a closed gate with a floor piece under it was walked through (no doors, no jumps),
// while the raiders were stopped there. The game lets a walk or jump move enter a ClosedDoor node (type 19) that counts
// as ground unless a node rule forbids it; the door move ignores node rules. So EMF always forbids type 19.
TEST_CASE("path query bytes (0.6.11): closed doors are forbidden for every move but the door move (node rule bit 19)") {
    uint8_t b[kPathQueryBytes] = {};
    const uint32_t game_rule = 1u << 3;  // a forbidden rule of the game's defaults stays
    std::memcpy(b + 0x60, &game_rule, 4);
    EmfPathQuery q = query(10, 4);
    fill_path_query(b, q, 77);
    uint32_t forbidden = 0, required = 0;
    std::memcpy(&forbidden, b + 0x60, 4);
    std::memcpy(&required, b + 0x5c, 4);
    CHECK(forbidden == (game_rule | (1u << 19)));
    CHECK(required == 0u);
    q.moves = EMF_PATH_OPEN_DOOR;  // with the door move a closed door is still crossed (as a DOOR point)
    fill_path_query(b, q, 77);
    std::memcpy(&forbidden, b + 0x60, 4);
    CHECK(forbidden == (game_rule | (1u << 19)));
}

TEST_CASE("path service (0.6.11): the door entities are kept with the points (at most 512); none when no point is a door") {
    PathService s;
    FakeEngine fe;
    REQUIRE(s.request(query(10, 0), 7, cb_noop, nullptr, 0, nullptr));
    s.step(fe.engine(), 10);
    PathAnswer a;
    a.done = true;
    a.game_result = 1;
    a.points.assign(600, EmfPathPoint{1, 50, 0, 0});
    a.doors.assign(600, 0);
    a.doors[3] = 44;
    a.doors[599] = 45;  // past what is kept
    fe.answers[fe.created[0]] = a;
    auto done = s.step(fe.engine(), 20);
    REQUIRE(done.size() == 1u);
    REQUIRE(done[0].doors.size() == kMaxPathPoints);
    CHECK(done[0].doors[3] == 44u);
    const EmfPathResult v = done[0].view();
    CHECK(v.doors == done[0].doors.data());
    CHECK(v.count == kMaxPathPoints);

    REQUIRE(s.request(query(10, 0), 7, cb_noop, nullptr, 0, nullptr));
    s.step(fe.engine(), 30);
    a.points.assign(2, EmfPathPoint{1, 50, 0, 0});
    a.doors.assign(2, 0);  // a door array with no door in it
    fe.answers[fe.created[1]] = a;
    done = s.step(fe.engine(), 40);
    REQUIRE(done.size() == 1u);
    CHECK(done[0].doors.empty());
    CHECK(done[0].view().doors == nullptr);
}

// ---- v48: planning as a given creature (#44) ----
// The values below are a Vukah warrior's and brawler's as the game build 1287369 holds them (PathWalker radius 0 and
// 0.5, height 2; node rules forbid tar, deadly fog and lava; default moves walk, the door move only with canOpenDoors).
namespace {
namespace g = emf::gen::ecs;
g::PathWalker walker_template(float radius = 0.5f, float height = 2.0f) {
    g::PathWalker w{};
    w.horizontalRadius = radius;
    w.height = height;
    w.pathNodeRequirements.requiredTypes = 0x1;
    w.pathNodeRequirements.forbiddenTypes = 388;
    return w;
}
g::PathFinder finder_template() {
    g::PathFinder t{};
    t.horizontalStartSearchRadius.halfMeters = 3;
    t.verticalStartSearchRadius.halfMeters = 6;
    t.settings.maxHeightDifferenceSettings.terrain = 0.75f;
    t.settings.maxHeightDifferenceSettings.building = 0.5f;
    t.settings.maxHeightDifferenceSettings.collider = 0.25f;
    return t;
}
// 17 factors 1.0, 1.1, ... 2.6 in their order
void fill_factors(g::PathNodeCostFactors* f) {
    float* c = &f->none;
    for (int i = 0; i < 17; ++i) c[i] = 1.0f + 0.1f * static_cast<float>(i);
}
g::Enemy01BehaviorState enemy_template(bool doors) {
    g::Enemy01BehaviorState e{};
    e.movementSetup.defaultMoveTypes = EMF_PATH_WALK;
    e.movementSetup.extendedMoveTypes = 0x1c;  // its jumps in Guard and fights: not taken
    e.openDoorSetup.canOpenDoors = doors;
    fill_factors(&e.pathNodeSetup.defaultCostFactors);
    e.pathNodeSetup.roamingCostFactors.road = 0.3f;  // not taken
    return e;
}
struct Parts {
    g::PathWalker walker = walker_template();
    g::PathFinder finder = finder_template();
    g::Enemy01BehaviorState enemy = enemy_template(true);
    PathCreatureParts parts(bool with_finder = true, bool with_enemy = true) const {
        PathCreatureParts p;
        p.walker = &walker;
        p.walker_size = sizeof walker;
        if (with_finder) {
            p.finder = &finder;
            p.finder_size = sizeof finder;
        }
        if (with_enemy) {
            p.enemy = &enemy;
            p.enemy_size = sizeof enemy;
        }
        return p;
    }
};
PathCreature creature() {
    static const Parts parts;
    PathCreature c;
    REQUIRE(read_path_creature(parts.parts(), &c));
    return c;
}
}  // namespace

TEST_CASE("path creature (v48): its walking profile from its templates (PathWalker, PathFinder, its behaviour)") {
    const PathCreature c = creature();
    CHECK(c.moves == (EMF_PATH_WALK | EMF_PATH_OPEN_DOOR));  // default moves, the door move with canOpenDoors
    CHECK(c.across == 1u);  // the game's trunc(radius x 2) half metres
    CHECK(c.up == 4u);
    CHECK(c.required == 0x1u);
    CHECK(c.forbidden == 388u);
    CHECK(c.has_finder);
    CHECK(c.start_search[0] == 3u);
    CHECK(c.start_search[1] == 6u);
    CHECK(c.height_diff[0] == 0.75f);
    CHECK(c.height_diff[1] == 0.5f);
    CHECK(c.height_diff[2] == 0.25f);
    // the query's 20 costs per PathNodeType from the 17 default factors (none twice, noNpcMovement three times), as the
    // game's own program spreads them
    const float want[20] = {1.0f, 1.0f, 1.4f, 1.5f, 1.6f, 1.7f, 1.8f, 1.9f, 2.0f, 1.1f,
                            1.2f, 1.3f, 2.1f, 2.1f, 2.1f, 2.2f, 2.4f, 2.5f, 2.6f, 2.3f};
    for (int i = 0; i < 20; ++i) CHECK(c.cost[i] == doctest::Approx(want[i]));

    Parts p;
    p.walker = walker_template(0.4f, 1.9f);  // truncated, as the game does: 0 and 3
    p.enemy = enemy_template(false);
    PathCreature d;
    REQUIRE(read_path_creature(p.parts(), &d));
    CHECK(d.moves == EMF_PATH_WALK);
    CHECK(d.across == 0u);
    CHECK(d.up == 3u);

    PathCreature e;  // no behaviour template the profile knows, no PathFinder: walk, costs 1.0, the game's steps
    REQUIRE(read_path_creature(p.parts(false, false), &e));
    CHECK(e.moves == EMF_PATH_WALK);
    CHECK_FALSE(e.has_finder);
    for (float x : e.cost) CHECK(x == 1.0f);

    g::Wildlife wild{};  // wildlife: its default moves and costs
    wild.movementSetup.defaultMoveTypes = EMF_PATH_WALK | EMF_PATH_SWIM;
    fill_factors(&wild.pathNodeSetup.defaultCostFactors);
    PathCreatureParts wp = p.parts(false, false);
    wp.wildlife = &wild;
    wp.wildlife_size = sizeof wild;
    REQUIRE(read_path_creature(wp, &e));
    CHECK(e.moves == (EMF_PATH_WALK | EMF_PATH_SWIM));
    CHECK(e.cost[3] == doctest::Approx(1.5f));

    g::ActiveNpcState npc{};  // a villager: its template has no moves (walk), its default costs
    fill_factors(&npc.pathNodeSetup.defaultCostFactors);
    PathCreatureParts np = p.parts(false, false);
    np.npc = &npc;
    np.npc_size = sizeof npc;
    REQUIRE(read_path_creature(np, &e));
    CHECK(e.moves == EMF_PATH_WALK);
    CHECK(e.cost[9] == doctest::Approx(1.1f));

    PathCreatureParts bad = p.parts();  // another layout, or no PathWalker: refused
    bad.walker_size -= 4;
    CHECK_FALSE(read_path_creature(bad, &e));
    bad = p.parts();
    bad.enemy_size -= 4;
    CHECK_FALSE(read_path_creature(bad, &e));
    bad = p.parts();
    bad.finder_size -= 4;
    CHECK_FALSE(read_path_creature(bad, &e));
    bad = p.parts();
    bad.walker = nullptr;
    CHECK_FALSE(read_path_creature(bad, &e));
}

TEST_CASE("path query bytes (v48): as a creature, its walking fields at the game's offsets, the mod's query on top") {
    uint8_t b[kPathQueryBytes];
    for (auto& x : b) x = 0xAB;  // the game's defaults
    const PathCreature c = creature();
    EmfPathQuery q = query(10, 4);
    q.closest = 1;
    q.max_nodes = 5000;
    fill_path_query(b, q, 77, &c);
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, b + o, 4); return v; };
    auto f32 = [&](size_t o) { float v; std::memcpy(&v, b + o, 4); return v; };
    auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, b + o, 2); return v; };
    CHECK(u32(0x00) == 77u);  // the owner, from, to, closest and nodes stay the mod's
    CHECK(f32(0x1c) == 10.0f);
    CHECK(b[0x36] == 2);
    CHECK(u32(0x48) == 5000u);
    CHECK(u16(0x54) == (EMF_PATH_WALK | EMF_PATH_OPEN_DOOR));
    CHECK(b[0x56] == 0);  // no jump heights from the creature
    CHECK(b[0x57] == 0);
    CHECK(u32(0x5c) == 0x1u);
    CHECK(u32(0x60) == (388u | (1u << 19)));  // EMF's closed-door rule stays
    CHECK(u32(0x64) == 0x1u);                 // the target follows the path's rules
    CHECK(u32(0x68) == 388u);
    for (int i = 0; i < 20; ++i) CHECK(f32(0x6c + 4 * i) == c.cost[i]);
    CHECK(f32(0xbc) == 0.75f);
    CHECK(f32(0xc0) == 0.5f);
    CHECK(f32(0xc4) == 0.25f);
    CHECK(u32(0xf0) == 3u);  // start search radii: EMF's own (2 across, 4 up), or the creature's where larger
    CHECK(u32(0xf4) == 6u);
    CHECK(u32(0xf8) == 4u);  // search radii: EMF's
    CHECK(u32(0xfc) == 4u);
    CHECK(u32(0x100) == 0u);  // room across: none with the door move (a door node has no room; doors would vanish)
    CHECK(u32(0x104) == 4u);
    CHECK(u32(0x108) == 0u);  // the target's room: the path's
    CHECK(u32(0x10c) == 4u);
    CHECK(b[0x28] == 0xAB);   // a target's fields are left: priority, keep distance, leash, free-path checks, options
    CHECK(u32(0x58) == 0xABABABABu);
    CHECK(u32(0xe8) == 0xABABABABu);
    CHECK(b[0xc8] == 0xAB);
    CHECK(b[0xcc] == 0xAB);

    PathCreature walker = c;  // a creature without the door move keeps its room across
    walker.moves = EMF_PATH_WALK;
    fill_path_query(b, q, 77, &walker);
    CHECK(u32(0x100) == 1u);
    CHECK(u32(0x108) == 1u);

    q.moves = EMF_PATH_JUMP_UP;  // the mod's moves are added; its jump heights, width and height win when given
    q.max_jump_up = 1;
    q.width = 0.5f;
    q.height = 1.0f;
    fill_path_query(b, q, 77, &c);
    CHECK(u16(0x54) == (EMF_PATH_WALK | EMF_PATH_OPEN_DOOR | EMF_PATH_JUMP_UP));
    CHECK(b[0x56] == 1);
    CHECK(u32(0x100) == 1u);  // asked for: also with the door move
    CHECK(u32(0x104) == 2u);
    CHECK(u32(0x108) == 1u);
    CHECK(u32(0x10c) == 2u);
}

TEST_CASE("path creature of (v48): an entity's templates by hash; refused without a DynamicPathFinder or a PathWalker") {
    const Parts parts;
    const g::Wildlife wild{};
    bool has_finder_component = true, has_walker = true;
    uint64_t walker_size = sizeof parts.walker;
    std::vector<uint32_t> asked;
    const PathComponentFn get = [&](uint32_t hash, uint64_t* size) -> const void* {
        CHECK(hash == g::DynamicPathFinder::kHash);
        *size = 272;
        static const uint8_t finder[272] = {};
        return has_finder_component ? finder : nullptr;
    };
    const PathComponentFn tpl = [&](uint32_t hash, uint64_t* size) -> const void* {
        asked.push_back(hash);
        if (hash == g::PathWalker::kHash) {
            *size = walker_size;
            return has_walker ? &parts.walker : nullptr;
        }
        if (hash == g::PathFinder::kHash) {
            *size = sizeof parts.finder;
            return &parts.finder;
        }
        if (hash == g::Enemy01BehaviorState::kHash) {
            *size = sizeof parts.enemy;
            return &parts.enemy;
        }
        return nullptr;  // no wildlife, villager or animal template
    };
    PathCreature c;
    CHECK(path_creature_of(812, get, tpl, &c) == "");
    CHECK(c.moves == (EMF_PATH_WALK | EMF_PATH_OPEN_DOOR));
    CHECK(c.across == 1u);
    CHECK(c.has_finder);
    CHECK(std::find(asked.begin(), asked.end(), g::Wildlife::kHash) != asked.end());
    CHECK(std::find(asked.begin(), asked.end(), g::ActiveNpcState::kHash) != asked.end());
    CHECK(std::find(asked.begin(), asked.end(), g::Animal::kHash) != asked.end());
    has_walker = false;
    CHECK(path_creature_of(812, get, tpl, &c) ==
          "as_entity 812 has no PathWalker template (not a creature that walks by the path grid)");
    has_walker = true;
    has_finder_component = false;
    CHECK(path_creature_of(812, get, tpl, &c) ==
          "as_entity 812 has no DynamicPathFinder (not a creature the game plans paths for, or not in the server world)");
    has_finder_component = true;
    walker_size = 88;
    CHECK(path_creature_of(812, get, tpl, &c) ==
          "as_entity 812: its path templates have another layout on this game build (EMF needs an update)");
}

TEST_CASE("path used text (v48): the console's second line, what the search planned with") {
    EmfPathResult r{};
    r.size = sizeof r;
    r.moves = EMF_PATH_WALK | EMF_PATH_OPEN_DOOR;
    r.jump_up = 2;  // no jump move: the heights are not shown
    r.jump_down = 3;
    r.height = 2.0f;
    CHECK(path_used_text(7, r) == "path 7: planned with walk, door; room 0.0 m across, 2.0 m up");
    r.moves = EMF_PATH_WALK | EMF_PATH_JUMP_UP | EMF_PATH_JUMP_DOWN;
    r.jump_up = 0;
    r.width = 0.5f;
    r.height = 0;
    CHECK(path_used_text(7, r) ==
          "path 7: planned with walk, jump up, jump down; jumps up any height, down at most 3 m; room 0.5 m across, 0.0 m up");
    r.size = kPathResultV42Size;  // an older result: nothing to say
    CHECK(path_used_text(7, r) == "");
}

TEST_CASE("path used (v48): the moves, jumps and room a query plans with, as the result reports them") {
    EmfPathQuery q = query();
    q.moves = EMF_PATH_JUMP_DOWN;
    q.max_jump_down = 5;
    q.width = 0.3f;  // rounded up to half metres, as the query holds it
    q.height = 2.0f;
    PathUsed u = path_used(q, nullptr);
    CHECK(u.moves == (EMF_PATH_WALK | EMF_PATH_JUMP_DOWN));
    CHECK(u.jump_up == 0);
    CHECK(u.jump_down == 5);
    CHECK(u.across == 1u);
    CHECK(u.up == 4u);
    const PathCreature c = creature();
    u = path_used(query(), &c);
    CHECK(u.moves == (EMF_PATH_WALK | EMF_PATH_OPEN_DOOR));
    CHECK(u.jump_up == 0);
    CHECK(u.across == 0u);  // the door move: no room across
    CHECK(u.up == 4u);
}

TEST_CASE("path service (v48): a creature's profile goes to the game with the query, the result says what was used") {
    PathService s;
    FakeEngine fe;
    std::vector<bool> with_creature;
    PathEngine e = fe.engine();
    const auto create = e.create;
    e.create = [&](const EmfPathQuery& q, uint32_t owner, const PathCreature* as) {
        with_creature.push_back(as && as->up == 4u);
        return create(q, owner, as);
    };
    const PathCreature c = creature();
    REQUIRE(s.request(query(), 7, cb_noop, nullptr, 0, nullptr, &c));
    REQUIRE(s.request(query(), 7, cb_noop, nullptr, 0, nullptr));
    s.step(e, 10);
    REQUIRE(with_creature.size() == 2u);
    CHECK(with_creature[0]);
    CHECK_FALSE(with_creature[1]);
    fe.finish(fe.created[0], 1, {{10, 50, 0, 0}});
    fe.finish(fe.created[1], 2, {});
    const auto done = s.step(e, 20);
    REQUIRE(done.size() == 2u);
    CHECK(done[0].result.size == sizeof(EmfPathResult));
    CHECK(done[0].result.moves == (EMF_PATH_WALK | EMF_PATH_OPEN_DOOR));
    CHECK(done[0].result.jump_up == 0);
    CHECK(done[0].result.width == 0.0f);
    CHECK(done[0].result.height == 2.0f);  // 4 half metres
    CHECK(done[1].result.moves == EMF_PATH_WALK);  // also on a query without a creature, and on no path
    CHECK(done[1].result.height == 0.0f);
}
