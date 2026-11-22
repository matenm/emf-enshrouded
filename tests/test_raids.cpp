// Night Raids (mods/raids): the raid's state machine, the spawn ring, templates, levels and rewards, without the game.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../mods/raids/raid_logic.h"
#include "core/manifest.h"

using namespace raids;

namespace {
constexpr uint64_t kSec = 1000;
const Vec3 kAltar{100, 50, 200};

Settings settings() {
    Settings s;
    s.chance = 100;
    s.start_hour = 20.0;
    s.waves = 2;
    s.per_wave = 3;
    s.wave_gap_s = 30;
    s.warning_s = 20;
    s.base_radius = 30;
    return s;
}

// One occupied base: the altar with one level-7 player 5 m from it.
std::vector<Occupied> home(uint32_t level = 7) {
    Occupied o;
    o.base = {900, kAltar};
    o.players.push_back({1, {kAltar.x + 5, kAltar.y, kAltar.z}, level});
    return {o};
}

Tick at(uint64_t now_ms, double tod, std::vector<Occupied> occupied = home(), uint32_t roll = 0) {
    Tick t;
    t.now_ms = now_ms;
    t.tod = tod;
    t.world_on = true;
    t.session = 1;
    t.occupied = std::move(occupied);
    t.defenders = t.occupied.empty() ? 0 : static_cast<int>(t.occupied[0].players.size());
    t.roll = roll;
    return t;
}

template <class K>
const Action* find(const std::vector<Action>& a, K kind) {
    for (const auto& x : a)
        if (x.kind == kind) return &x;
    return nullptr;
}

// Runs the raid from nightfall to the first wave; returns the director in Fighting with wave 1's raiders spawned.
struct Fight {
    Director d{settings()};
    uint64_t now = 0;
    uint32_t next_id = 5000;
    std::vector<uint32_t> spawned;
    Fight() {
        d.tick(at(now, 19.9));
        now += kSec;
        const auto a = d.tick(at(now, 20.05));  // nightfall: the roll succeeds
        REQUIRE(find(a, Action::Notice));
        REQUIRE(d.phase() == Phase::Warning);
        now += 21 * kSec;
        spawn_from(d.tick(at(now, 20.3)));
        REQUIRE(d.phase() == Phase::Fighting);
    }
    void spawn_from(const std::vector<Action>& a) {
        const Action* s = find(a, Action::Spawn);
        REQUIRE(s);
        for (int i = 0; i < s->count; ++i) {
            spawned.push_back(next_id);
            d.on_spawned(next_id++);
        }
        d.on_wave_spawned(s->count);
    }
};
}  // namespace

TEST_CASE("raids: nightfall is the clock crossing the start hour (wrapping at midnight); night lasts until dawn") {
    CHECK(crossed(19.9, 20.05, 20.0));
    CHECK_FALSE(crossed(20.05, 20.1, 20.0));  // already past
    CHECK(crossed(12.0, 21.0, 20.0));          // the time command jumped over it
    CHECK(crossed(23.0, 0.5, 23.5));           // across midnight
    CHECK_FALSE(crossed(23.0, 0.5, 20.0));
    CHECK_FALSE(crossed(21.0, 7.0, 20.0));  // a jump to the morning is no nightfall
    CHECK(is_night(21.0, 20.0, 6.0));
    CHECK(is_night(3.0, 20.0, 6.0));
    CHECK_FALSE(is_night(6.0, 20.0, 6.0));
    CHECK_FALSE(is_night(12.0, 20.0, 6.0));
}

TEST_CASE("raids: once per night the chance is rolled; a raid needs a player at a base and the world switched on") {
    Settings s = settings();
    s.chance = 35;
    Director d(s);
    d.tick(at(0, 19.9));
    auto a = d.tick(at(kSec, 20.1, home(), 50));  // 50 >= 35: no raid this night
    CHECK(d.phase() == Phase::Idle);
    a = d.tick(at(2 * kSec, 20.2, home(), 0));  // the same night: no second roll
    CHECK(d.phase() == Phase::Idle);
    d.tick(at(3 * kSec, 12.0));  // the next day, then night
    d.tick(at(4 * kSec, 20.5, {}, 0));  // nobody at a base: no raid
    CHECK(d.phase() == Phase::Idle);
    Tick off = at(5 * kSec, 19.0);
    off.world_on = false;
    d.tick(off);
    off.now_ms = 6 * kSec;
    off.tod = 20.5;
    d.tick(off);  // switched off for this world
    CHECK(d.phase() == Phase::Idle);
    d.tick(at(7 * kSec, 12.0));
    d.tick(at(8 * kSec, 20.5, home(), 10));  // 10 < 35
    CHECK(d.phase() == Phase::Warning);
}

TEST_CASE("raids: a warning first, then wave after wave at the base, at the strongest player's level plus the difficulty") {
    Settings s = settings();
    s.level_offset = -2;
    Director d(s);
    d.tick(at(0, 19.9));
    auto a = d.tick(at(kSec, 20.05, home(9)));
    const Action* n = find(a, Action::Notice);
    REQUIRE(n);
    CHECK(n->text.find("20 s") != std::string::npos);
    a = d.tick(at(11 * kSec, 20.1, home(9)));
    const Action* hud = find(a, Action::Hud);
    REQUIRE(hud);
    CHECK(hud->text == "Raid in 10 s");
    a = d.tick(at(22 * kSec, 20.2, home(9)));
    const Action* sp = find(a, Action::Spawn);
    REQUIRE(sp);
    CHECK(sp->count == 3);
    CHECK(sp->level == 7u);  // 9 - 2
    CHECK(sp->altar == 900u);
    CHECK(sp->at.x == kAltar.x);
    CHECK_FALSE(sp->boss);
    CHECK(d.phase() == Phase::Fighting);
}

TEST_CASE("raids: the next wave comes after the gap, or soon after every raider of this wave is dead") {
    Fight f;
    CHECK(f.d.alive() == 3);
    f.now += 10 * kSec;
    CHECK_FALSE(find(f.d.tick(at(f.now, 20.4)), Action::Spawn));
    for (uint32_t id : f.spawned) f.d.on_died(id);  // all of wave 1 dead
    f.now += kSec;
    auto a = f.d.tick(at(f.now, 20.4));
    CHECK_FALSE(find(a, Action::Spawn));  // a breath first
    CHECK_FALSE(find(a, Action::Reward));  // not repelled: a wave is still to come
    f.now += 5 * kSec;
    f.spawn_from(f.d.tick(at(f.now, 20.5)));
    CHECK(f.d.wave() == 2);
    const auto after = f.d.tick(at(f.now + kSec, 20.5));
    const auto hud = find(after, Action::Hud);
    REQUIRE(hud);
    CHECK(hud->text == "Raid: wave 2/2 - 3 left");
}

TEST_CASE("raids: all raiders of the last wave dead: repelled, a reward at the base, then a quiet night") {
    Fight f;
    f.now += 31 * kSec;
    f.spawn_from(f.d.tick(at(f.now, 20.6)));  // wave 2 after the gap (wave 1 still alive)
    CHECK(f.d.alive() == 6);
    for (uint32_t id : f.spawned) {
        f.d.credit(id);  // a player's (or an ally's) hit
        f.d.on_died(id);
    }
    f.now += kSec;
    const auto a = f.d.tick(at(f.now, 20.7));
    const Action* r = find(a, Action::Reward);
    REQUIRE(r);
    CHECK(r->altar == 900u);
    CHECK(r->count == 6);  // raiders killed
    const Action* n = find(a, Action::Notice);
    REQUIRE(n);
    CHECK(n->text.find("repelled") != std::string::npos);
    CHECK(f.d.phase() == Phase::Idle);
    CHECK_FALSE(find(a, Action::Destroy));  // nothing alive to remove; the corpses keep their loot
}

TEST_CASE("raids: at dawn the survivors leave (destroyed) and no reward is given") {
    Fight f;
    f.d.on_died(f.spawned[0]);
    f.now += 10 * kSec;
    const auto a = f.d.tick(at(f.now, 6.01));
    const Action* x = find(a, Action::Destroy);
    REQUIRE(x);
    CHECK(x->ids == std::vector<uint32_t>{f.spawned[1], f.spawned[2]});
    CHECK_FALSE(find(a, Action::Reward));
    CHECK(find(a, Action::Notice)->text.find("dawn") != std::string::npos);
    CHECK(f.d.phase() == Phase::Idle);
    CHECK(f.d.alive() == 0);
}

TEST_CASE("raids: with nobody near the base for 60 s the raiders leave") {
    Fight f;
    Tick empty = at(f.now + 30 * kSec, 21.0, {});
    empty.defenders = 0;
    CHECK_FALSE(find(f.d.tick(empty), Action::Destroy));
    empty.now_ms = f.now + 91 * kSec;
    const auto a = f.d.tick(empty);
    REQUIRE(find(a, Action::Destroy));
    CHECK(f.d.phase() == Phase::Idle);
}

TEST_CASE("raids: a new session (world unload, reload) forgets the raid without destroying ids of the new world") {
    Fight f;
    Tick t = at(f.now + kSec, 21.0);
    t.session = 2;
    const auto a = f.d.tick(t);
    CHECK_FALSE(find(a, Action::Destroy));  // spawned entities are not saved: the old ids mean nothing now
    const Action* hud = find(a, Action::Hud);
    REQUIRE(hud);
    CHECK(hud->text.empty());
    CHECK(f.d.phase() == Phase::Idle);
    CHECK(f.d.alive() == 0);
}

TEST_CASE("raids: adopt_session forgets the old raid at once (before any order or command can use its ids)") {
    Fight f;
    const auto a = f.d.adopt_session(2);
    REQUIRE(find(a, Action::Hud));
    CHECK_FALSE(find(a, Action::Destroy));
    CHECK(f.d.living().empty());
    CHECK_FALSE(f.d.is_raider(f.spawned[0]));
    CHECK(f.d.phase() == Phase::Idle);
    CHECK(f.d.adopt_session(2).empty());  // already this session
    Tick t = at(f.now + kSec, 21.0);
    t.session = 2;
    CHECK_FALSE(find(f.d.tick(t), Action::Hud));  // the tick has nothing left to forget
}

TEST_CASE("raids: stop (command, world switched off, mod switched off) destroys every living raider") {
    Fight f;
    f.d.on_died(f.spawned[1]);
    const auto a = f.d.stop("stopped");
    const Action* x = find(a, Action::Destroy);
    REQUIRE(x);
    CHECK(x->ids == std::vector<uint32_t>{f.spawned[0], f.spawned[2]});
    CHECK(find(a, Action::Hud)->text.empty());
    CHECK(f.d.phase() == Phase::Idle);
    CHECK(f.d.stop("again").empty());  // nothing left to do
    Fight g;
    Tick off = at(g.now + kSec, 21.0);
    off.world_on = false;
    REQUIRE(find(g.d.tick(off), Action::Destroy));
}

TEST_CASE("raids: start now (raids.start) skips the night and the roll; refused while a raid runs or nobody is at a base") {
    Director d(settings());
    std::string why;
    CHECK(d.start(at(0, 12.0, {}), 5, &why).empty());
    CHECK(why.find("base") != std::string::npos);
    auto a = d.start(at(0, 12.0), 5, &why);
    REQUIRE(find(a, Action::Notice));
    CHECK(d.phase() == Phase::Warning);
    CHECK(d.start(at(kSec, 12.0), 5, &why).empty());
    CHECK(why.find("already") != std::string::npos);
    a = d.tick(at(6 * kSec, 12.0));
    REQUIRE(find(a, Action::Spawn));  // in daylight too: a test raid does not end at once
    a = d.tick(at(7 * kSec, 12.1));
    CHECK(d.phase() == Phase::Fighting);
}

TEST_CASE("raids: at most 24 alive at once, and never more than the waves ask for") {
    Settings s = settings();
    s.waves = 5;
    s.per_wave = 12;
    s.wave_gap_s = 20;
    Director d(s);
    std::string why;
    d.start(at(0, 21.0), 0, &why);
    uint32_t id = 100;
    int total = 0;
    for (int step = 1; step < 200; ++step) {
        for (const auto& a : d.tick(at(step * kSec, 21.0))) {
            if (a.kind != Action::Spawn) continue;
            for (int i = 0; i < a.count; ++i) d.on_spawned(id++);
            d.on_wave_spawned(a.count);
            total += a.count;
        }
        CHECK(d.alive() <= 24);
    }
    CHECK(total == 24);  // nobody dies here: the cap holds the waves back
}

TEST_CASE("raids: a wave that could not spawn anyone ends the raid (logged), without a reward") {
    Director d(settings());
    std::string why;
    d.start(at(0, 21.0), 0, &why);
    const auto a = d.tick(at(kSec, 21.0));
    REQUIRE(find(a, Action::Spawn));
    d.on_wave_spawned(0);
    const auto b = d.tick(at(2 * kSec, 21.0));
    CHECK(d.phase() == Phase::Idle);
    CHECK_FALSE(find(b, Action::Reward));
}

TEST_CASE("raids: the cooldown skips that many nights after a raid") {
    Settings s = settings();
    s.cooldown_nights = 1;
    Director d(s);
    std::string why;
    d.start(at(0, 21.0), 0, &why);
    d.stop("x");
    d.tick(at(kSec, 12.0));
    d.tick(at(2 * kSec, 20.5));  // the first night after: skipped
    CHECK(d.phase() == Phase::Idle);
    d.tick(at(3 * kSec, 12.0));
    d.tick(at(4 * kSec, 20.5));
    CHECK(d.phase() == Phase::Warning);
}

TEST_CASE("raids: the boss wave adds a Vukah brawler to the last wave only") {
    Settings s = settings();
    s.boss = true;
    Director d(s);
    std::string why;
    d.start(at(0, 21.0), 0, &why);
    const auto first = d.tick(at(kSec, 21.0));
    const Action* w1 = find(first, Action::Spawn);
    REQUIRE(w1);
    CHECK_FALSE(w1->boss);
    d.on_wave_spawned(1);
    d.on_spawned(1);
    const auto second = d.tick(at(40 * kSec, 21.0));
    const Action* w2 = find(second, Action::Spawn);
    REQUIRE(w2);
    CHECK(w2->boss);
}

TEST_CASE("raids: only creatures you are ready for: the boss wave's brawler stays away below its level, unless the rule is off") {
    const uint32_t ready = boss_template().min_level;
    REQUIRE(ready > 1);
    CHECK(boss_template().known);
    const auto last_wave = [](Settings s, uint32_t level, std::vector<Action>* all) {
        s.boss = true;
        s.waves = 1;
        Director d(s);
        std::string why;
        d.start(at(0, 21.0, home(level)), 0, &why);
        *all = d.tick(at(kSec, 21.0, home(level)));
        const Action* a = find(*all, Action::Spawn);
        REQUIRE(a);
        return *a;
    };
    std::vector<Action> all;
    Settings s = settings();
    CHECK(s.progression);  // on by default
    CHECK(Settings{}.progression);
    Action a = last_wave(s, ready - 1, &all);
    CHECK_FALSE(a.boss);
    CHECK(a.count == s.per_wave);
    CHECK(a.player_level == ready - 1);
    bool told = false;
    for (const auto& x : all) told |= x.kind == Action::Log && x.text.find("boss stays away") != std::string::npos;
    CHECK(told);
    CHECK(last_wave(s, ready, &all).boss);
    s.progression = false;
    CHECK(last_wave(s, 1, &all).boss);  // the rule off: as before
    // the players' level decides, not the raiders' level: a higher difficulty does not let the boss in
    s.progression = true;
    s.level_offset = 6;
    a = last_wave(s, ready - 1, &all);
    CHECK_FALSE(a.boss);
    CHECK(a.level == ready + 5);
}

TEST_CASE("raids: only creatures you are ready for: never a template from above the strongest player's level") {
    const std::vector<Template> pool = {{"wolf", "a", 1, 99}, {"dragon", "b", 25, 99}};
    for (uint32_t rnd = 0; rnd < 8; ++rnd) {
        CHECK(choose_raider(pool, 8, 3, rnd, true).name == "wolf");
        CHECK(choose_raider(pool, 33, 30, rnd, true).name != "");  // both may come at 30
    }
    bool dragon = false;
    for (uint32_t rnd = 0; rnd < 8; ++rnd) dragon |= choose_raider(pool, 33, 30, rnd, true).name == "dragon";
    CHECK(dragon);
    const std::vector<Template> late = {{"dragon", "b", 25, 99}};
    CHECK(choose_raider(late, 3, 3, 0, false).name == "dragon");  // the rule off: the lowest band, as before
    CHECK(choose_raider(late, 3, 3, 0, true).guid == verified_pool()[0].guid);  // on: the tested Vukah instead
    // custom templates: "GUID@level" comes from that player level; a bare GUID's level is not known
    std::vector<std::string> bad;
    const auto custom = parse_custom_templates(
        "d5d05217-52dc-467a-9192-26b4f661d8d9@12, 9c8e2008-7259-4114-b6fc-615f0f181c04, d5d05217-52dc-467a-9192-26b4f661d8d9@x",
        &bad);
    REQUIRE(custom.size() == 2);
    CHECK(custom[0].guid == "d5d05217-52dc-467a-9192-26b4f661d8d9");
    CHECK(custom[0].min_level == 12u);
    CHECK(custom[0].known);
    CHECK(custom[1].min_level == 1u);
    CHECK_FALSE(custom[1].known);
    CHECK(bad == std::vector<std::string>{"d5d05217-52dc-467a-9192-26b4f661d8d9@x"});
    // a boss or mini-boss from a template whose level is not known is removed under the rule (EMF cannot tell whether
    // the players beat it); a known one, or any with the rule off, stays
    CHECK(remove_unready_boss(true, custom[1], true));
    CHECK_FALSE(remove_unready_boss(true, custom[1], false));
    CHECK_FALSE(remove_unready_boss(true, custom[0], true));
    CHECK_FALSE(remove_unready_boss(false, custom[1], true));
}

TEST_CASE("raids: a bare custom GUID found to be a boss is skipped from then on: Vukah warriors come in its place") {
    const std::string boss = "9c8e2008-7259-4114-b6fc-615f0f181c04", other = "d5d05217-52dc-467a-9192-26b4f661d8d9";
    const std::set<std::string> found = {boss};
    const std::vector<Template> only = {{"custom", boss, 1, 99, false}};
    for (uint32_t rnd = 0; rnd < 6; ++rnd) {
        CHECK(choose_raider(only, 10, 10, rnd, true, &found).guid == verified_pool()[0].guid);
        CHECK(choose_raider({{"custom", boss, 1, 99, false}, {"custom", other, 1, 99, false}}, 10, 10, rnd, true, &found).guid ==
              other);
    }
    CHECK(choose_raider(only, 10, 10, 0, true, nullptr).guid == boss);  // not found yet: it may come (then it is checked)
    CHECK(choose_raider({{"custom", boss, 5, 99, true}}, 10, 10, 0, true, &found).guid == boss);  // GUID@5: allowed
    CHECK(choose_raider(only, 10, 10, 0, false, &found).guid == boss);  // the rule off: as before
}

TEST_CASE("raids: custom templates take spaces around the @") {
    std::vector<std::string> bad;
    const auto custom = parse_custom_templates("d5d05217-52dc-467a-9192-26b4f661d8d9 @ 12,9c8e2008-7259-4114-b6fc-615f0f181c04@ 3", &bad);
    REQUIRE(custom.size() == 2);
    CHECK(custom[0].guid == "d5d05217-52dc-467a-9192-26b4f661d8d9");
    CHECK(custom[0].min_level == 12u);
    CHECK(custom[1].min_level == 3u);
    CHECK(bad.empty());
}

// ---- where raiders appear ----
TEST_CASE("raids: inside a base is within its radius (horizontally, the radius rule)") {
    const std::vector<Base> bases = {{1, {0, 0, 0}}, {2, {500, 0, 0}}};
    CHECK(inside_any_base({10, 40, 10}, bases, 30));  // height does not matter
    CHECK_FALSE(inside_any_base({40, 0, 0}, bases, 30));
    CHECK(inside_any_base({480, 0, 0}, bases, 30));
}

TEST_CASE("raids: spawn points lie 40-60 m from the altar, outside every base; walked spots first, else the altar's height") {
    const std::vector<Base> bases = {{1, kAltar}};
    Breadcrumbs crumbs;
    const Vec3 walked{kAltar.x + 45, kAltar.y + 7, kAltar.z};  // a hillside the player walked over
    crumbs.add(walked, 0);
    crumbs.add({kAltar.x + 10, kAltar.y, kAltar.z}, 3000);  // inside the base: never used
    const Ring ring = ring_for(30);
    CHECK(ring.min == 40.0);
    CHECK(ring.max == 60.0);
    auto pts = spawn_points(kAltar, bases, crumbs, ring, 30, 4, 12345);
    REQUIRE(pts.size() == 4);
    for (const auto& p : pts) {
        const double d = horizontal_distance(p, kAltar);
        CHECK(d >= 36.0);  // the walked spot plus a few metres of spread, never into the base
        CHECK(d <= 64.0);
        CHECK_FALSE(inside_any_base(p, bases, 30));
        CHECK(p.y == doctest::Approx(walked.y + 1.0));  // on the walked spot's height, a metre up
    }
    // nothing walked out there: ring points at the altar's height + 2 m (seen in game: they land and walk)
    Breadcrumbs none;
    pts = spawn_points(kAltar, bases, none, ring, 30, 3, 7);
    REQUIRE(pts.size() == 3);
    for (const auto& p : pts) {
        CHECK(horizontal_distance(p, kAltar) >= 36.0);
        CHECK(p.y == doctest::Approx(kAltar.y + 2.0));
        CHECK_FALSE(inside_any_base(p, bases, 30));
    }
}

TEST_CASE("raids: a bigger base radius pushes the ring out; a neighbouring base blocks its side of the ring") {
    CHECK(ring_for(50).min == 55.0);
    CHECK(ring_for(50).max == 75.0);
    // a second base 50 m east covers the east side: points never land in it
    const std::vector<Base> bases = {{1, kAltar}, {2, {kAltar.x + 50, kAltar.y, kAltar.z}}};
    Breadcrumbs none;
    for (uint32_t seed = 0; seed < 40; ++seed) {
        const auto pts = spawn_points(kAltar, bases, none, ring_for(30), 30, 4, seed);
        REQUIRE_FALSE(pts.empty());
        for (const auto& p : pts) CHECK_FALSE(inside_any_base(p, bases, 30));
    }
    // a ring entirely covered by bases: nowhere to spawn
    std::vector<Base> crowded = {{1, kAltar}};
    for (int i = 0; i < 16; ++i) {
        const double a = i * 3.14159265358979 / 8;
        crowded.push_back({uint32_t(10 + i), {kAltar.x + 50 * std::cos(a), kAltar.y, kAltar.z + 50 * std::sin(a)}});
    }
    CHECK(spawn_points(kAltar, crowded, none, ring_for(30), 30, 4, 1).empty());
}

TEST_CASE("raids: breadcrumbs keep one spot per few metres, the newest 512") {
    Breadcrumbs c;
    c.add({0, 0, 0}, 0);
    c.add({1, 0, 1}, 100);  // too close to the last one
    CHECK(c.size() == 1);
    c.add({10, 0, 0}, 200);
    CHECK(c.size() == 2);
    for (int i = 0; i < 600; ++i) c.add({100.0 + i * 5, 0, 0}, 1000 + i);
    CHECK(c.size() == 512);
}

// ---- who comes, how strong, and what they leave ----
TEST_CASE("raids: the template for a level comes from the verified pool by its band; custom GUIDs replace the pool") {
    const auto& pool = verified_pool();
    REQUIRE_FALSE(pool.empty());
    for (const auto& t : pool) {
        uint8_t g[16];
        CHECK(parse_guid_text(t.guid, g));
    }
    CHECK(pick_template(pool, 1, 0).guid == pool[0].guid);
    const std::vector<Template> banded = {{"low", "a", 1, 10}, {"mid", "b", 8, 20}, {"high", "c", 18, 99}};
    CHECK(pick_template(banded, 5, 0).name == "low");
    CHECK(pick_template(banded, 9, 0).name == "low");
    CHECK(pick_template(banded, 9, 1).name == "mid");
    CHECK(pick_template(banded, 30, 7).name == "high");
    const std::vector<Template> above = {{"mid", "b", 8, 20}, {"high", "c", 18, 99}};
    CHECK(pick_template(above, 2, 0).name == "mid");  // below every band: the lowest one
    std::vector<std::string> bad;
    const auto custom = parse_custom_templates(
        " d5d05217-52dc-467a-9192-26b4f661d8d9, nonsense ,9c8e2008-7259-4114-b6fc-615f0f181c04", &bad);
    REQUIRE(custom.size() == 2);
    CHECK(custom[1].guid == "9c8e2008-7259-4114-b6fc-615f0f181c04");
    CHECK(bad == std::vector<std::string>{"nonsense"});
    CHECK(boss_template().guid == std::string("9c8e2008-7259-4114-b6fc-615f0f181c04"));  // seen in game: it wakes
}

TEST_CASE("raids: the raiders' level is the strongest player's plus the difficulty, 1-99") {
    CHECK(raider_level(10, 0) == 10u);
    CHECK(raider_level(10, -5) == 5u);
    CHECK(raider_level(2, -5) == 1u);
    CHECK(raider_level(98, 6) == 99u);
    CHECK(raider_level(0, 0) == 1u);  // unknown level
    CHECK(difficulty_offset("easy") == -5);
    CHECK(difficulty_offset("normal") == 0);
    CHECK(difficulty_offset("hard") == 3);
    CHECK(difficulty_offset("deadly") == 6);
    CHECK(difficulty_offset("?") == 0);
    std::vector<Occupied> o = home(7);
    o[0].players.push_back({2, kAltar, 12});
    CHECK(strongest_level(o[0]) == 12u);
}

TEST_CASE("raids: rewards: none, materials by the raiders killed, or materials plus a rare weapon at the player's level") {
    CHECK(reward_items(RewardKind::None, 6, 10, 0).empty());
    const auto m = reward_items(RewardKind::Materials, 6, 10, 0);
    REQUIRE(m.size() == 2);
    CHECK(m[0].item == "Material_T1_Token_WeaponUpgrade_Rune01");
    CHECK(m[0].count == 6u);
    CHECK(m[1].count == 6u);
    CHECK(reward_items(RewardKind::Materials, 1, 10, 0)[0].count == 2u);  // at least 2
    const auto r = reward_items(RewardKind::Rare, 6, 14, 3);
    REQUIRE(r.size() == 3);
    CHECK(r[2].item.rfind("Weapon_", 0) == 0);
    CHECK(r[2].level == 14u);
    CHECK(r[2].rarity == 2);  // Rare
    CHECK(reward_kind("rare") == RewardKind::Rare);
    CHECK(reward_kind("none") == RewardKind::None);
    CHECK(reward_kind("x") == RewardKind::Materials);
}

TEST_CASE("raids: which base when several have players: the busiest, or one at random") {
    std::vector<Occupied> o = home(5);
    Occupied two;
    two.base = {901, {0, 0, 0}};
    two.players = {{2, {0, 0, 0}, 3}, {3, {1, 0, 0}, 3}};
    o.push_back(two);
    CHECK(pick_base(o, BasePick::Busiest, 0)->base.altar == 901u);
    CHECK(pick_base(o, BasePick::Random, 0)->base.altar == 900u);
    CHECK(pick_base(o, BasePick::Random, 1)->base.altar == 901u);
    CHECK(pick_base({}, BasePick::Busiest, 0) == nullptr);
}

TEST_CASE("raids: occupied bases are the altars with players within the base radius") {
    const std::vector<Base> bases = {{900, kAltar}, {901, {0, 0, 0}}};
    const std::vector<Player> players = {{1, {kAltar.x + 20, kAltar.y, kAltar.z}, 5}, {2, {300, 0, 300}, 9}};
    const auto o = occupied_bases(bases, players, 30);
    REQUIRE(o.size() == 1);
    CHECK(o[0].base.altar == 900u);
    REQUIRE(o[0].players.size() == 1);
    CHECK(o[0].players[0].id == 1u);
    CHECK(players_near(kAltar, players, 80) == 1);
}

TEST_CASE("raids: whom a raider fights: the nearest living player, or with allies on also a companion or summon") {
    const Vec3 r{0, 0, 0};
    const std::vector<Foe> foes = {
        {1, {12, 0, 0}, true, true},      // a player 12 m away
        {5000, {4, 0, 3}, false, true},   // the companion 5 m away
        {2, {3, 0, 0}, true, false},      // a dead player, nearer: never (seen in game: the raider then wandered off)
        {5001, {2, 0, 0}, false, false},  // a dead summon
    };
    CHECK(pick_target(r, foes, 25, Targets::Players) == 1u);
    CHECK(pick_target(r, foes, 25, Targets::Allies) == 5000u);
    CHECK(pick_target(r, foes, 10, Targets::Players) == 0u);  // the only living player is out of reach
    CHECK(pick_target(r, foes, 10, Targets::Allies) == 5000u);
    CHECK(pick_target(r, foes, 4, Targets::Allies) == 0u);
    CHECK(pick_target(r, {}, 25, Targets::Allies) == 0u);
    const std::vector<Foe> close = {{1, {3, 0, 0}, true, true}, {5000, {6, 0, 0}, false, true}};
    CHECK(pick_target(r, close, 25, Targets::Allies) == 1u);  // the player is nearer
    CHECK(targets_mode("players") == Targets::Players);
    CHECK(targets_mode("allies") == Targets::Allies);
    CHECK(targets_mode("") == Targets::Allies);  // the default
}

TEST_CASE("raids: a raider keeps its target while it lives and is in reach (no flipping every 3 s)") {
    const Vec3 r{0, 0, 0};
    const std::vector<Foe> foes = {{1, {6, 0, 0}, true, true}, {5000, {3, 0, 0}, false, true}};
    CHECK(pick_target(r, foes, 25, Targets::Allies) == 5000u);
    CHECK(pick_target(r, foes, 25, Targets::Allies, 1) == 1u);       // it fights the player: it goes on with the player
    CHECK(pick_target(r, foes, 25, Targets::Allies, 5000) == 5000u);
    CHECK(pick_target(r, foes, 5, Targets::Allies, 1) == 5000u);     // the player got out of reach
    CHECK(pick_target(r, foes, 25, Targets::Players, 5000) == 1u);   // allies are not wanted now
    CHECK(pick_target(r, foes, 25, Targets::Allies, 777) == 5000u);  // its last target is gone
    const std::vector<Foe> dead = {{1, {6, 0, 0}, true, false}, {5000, {3, 0, 0}, false, true}};
    CHECK(pick_target(r, dead, 25, Targets::Allies, 1) == 5000u);    // never a dead one
}

TEST_CASE("raids: presets: gentle, standard and hard set how often and how hard; custom keeps the settings one by one") {
    Settings s = settings();
    RewardKind reward = RewardKind::Rare;
    CHECK_FALSE(apply_preset("custom", s, reward));  // the settings as they are
    CHECK_FALSE(apply_preset("nonsense", s, reward));
    CHECK(s.chance == 100);
    CHECK(reward == RewardKind::Rare);
    REQUIRE(apply_preset("standard", s, reward));  // the mod's defaults
    const Settings def;
    CHECK(s.chance == def.chance);
    CHECK(s.waves == def.waves);
    CHECK(s.per_wave == def.per_wave);
    CHECK(s.wave_gap_s == def.wave_gap_s);
    CHECK(s.warning_s == def.warning_s);
    CHECK(s.level_offset == def.level_offset);
    CHECK(s.cooldown_nights == def.cooldown_nights);
    CHECK(s.boss == def.boss);
    CHECK(reward == RewardKind::Materials);
    CHECK(s.start_hour == 20.0);  // what a preset does not set stays
    CHECK(s.base_radius == 30);
    Settings g = settings(), h = settings();
    RewardKind gr, hr;
    REQUIRE(apply_preset("gentle", g, gr));
    REQUIRE(apply_preset("hard", h, hr));
    CHECK(g.chance < s.chance);
    CHECK(h.chance > s.chance);
    CHECK(g.waves * g.per_wave < s.waves * s.per_wave);
    CHECK(h.waves * h.per_wave > s.waves * s.per_wave);
    CHECK(g.level_offset < 0);
    CHECK(h.level_offset > 0);
    CHECK(g.warning_s > s.warning_s);
    CHECK_FALSE(g.boss);
    CHECK(h.boss);
    CHECK(hr == RewardKind::Rare);
    for (const char* name : {"gentle", "standard", "hard"}) {
        const auto p = preset(name);
        REQUIRE(p);
        CHECK(p->waves * p->per_wave + (p->boss ? 1 : 0) <= s.max_alive);  // a whole raid fits under the cap
    }
}

TEST_CASE("raids: mod.json: a preset's settings are greyed out unless it is custom, and their defaults are the standard preset") {
    std::ifstream f(std::string(EMF_SOURCE_DIR) + "/mods/raids/mod.json", std::ios::binary);
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const auto r = emf::core::parse_manifest(ss.str());
    REQUIRE_MESSAGE(r.manifest, r.error);
    const auto def = [&](const std::string& key) -> const emf::core::SettingDef* {
        for (const auto& d : r.manifest->settings)
            if (d.key == key) return &d;
        return nullptr;
    };
    const auto* p = def("preset");
    REQUIRE(p);
    CHECK(p->default_value == "custom");  // an update never overrides settings a player chose
    std::vector<std::string> values;
    for (const auto& o : p->options) values.push_back(o.value);
    CHECK(values == std::vector<std::string>{"custom", "gentle", "standard", "hard"});
    // the greyed-out settings keep showing the player's own values, so each preset's label spells out its numbers
    CHECK(p->help.find("decides") != std::string::npos);
    for (const auto& o : p->options) {
        const auto pr = preset(o.value);
        if (!pr) continue;
        CAPTURE(o.label);
        CHECK(o.label.find(std::to_string(pr->chance) + "%") != std::string::npos);
        CHECK(o.label.find(std::to_string(pr->waves) + (pr->waves == 1 ? " wave of " : " waves of ") +
                           std::to_string(pr->per_wave)) != std::string::npos);
        CHECK(o.label.find(std::to_string(pr->warning_s) + " s") != std::string::npos);
        if (pr->waves > 1) CHECK(o.label.find(std::to_string(pr->wave_gap_s) + " s apart") != std::string::npos);
        const int off = difficulty_offset(pr->difficulty);
        const std::string level = off == 0 ? "your level"
                                           : std::to_string(off < 0 ? -off : off) + (off < 0 ? " levels below" : " levels above");
        CHECK(o.label.find(level) != std::string::npos);
        CHECK((o.label.find("boss") != std::string::npos) == pr->boss);
        CHECK((o.label.find("rare") != std::string::npos) == (pr->reward == "rare"));
        CHECK((o.label.find("quiet night") != std::string::npos) == (pr->cooldown_nights > 0));
    }
    const auto standard = preset("standard");
    REQUIRE(standard);
    const std::map<std::string, std::string> want = {
        {"chance", std::to_string(standard->chance)},
        {"warning_seconds", std::to_string(standard->warning_s)},
        {"cooldown_nights", std::to_string(standard->cooldown_nights)},
        {"waves", std::to_string(standard->waves)},
        {"per_wave", std::to_string(standard->per_wave)},
        {"wave_gap_seconds", std::to_string(standard->wave_gap_s)},
        {"difficulty", standard->difficulty},
        {"boss", standard->boss ? "1" : "0"},
        {"reward", standard->reward}};
    const auto* rule = def("progression");  // the progression rule: on by default, its help names the brawler's level
    REQUIRE(rule);
    CHECK(rule->default_value == "1");
    CHECK(rule->help.find("level " + std::to_string(boss_template().min_level)) != std::string::npos);
    CHECK(want.size() == preset_keys().size());
    for (const auto& key : preset_keys()) {
        CAPTURE(key);
        const auto* d = def(key);
        REQUIRE(d);
        CHECK(d->when_key == "preset");
        CHECK(d->when_value == "custom");
        CHECK(d->default_value == want.at(key));
    }
}

TEST_CASE("raids: the per-world switch is kept in the world's file as on=1 / on=0 (off when missing)") {
    CHECK_FALSE(parse_world_on(""));
    CHECK(parse_world_on("on=1\n"));
    CHECK_FALSE(parse_world_on("on=0\n"));
    CHECK(parse_world_on("raids=3\non=1"));
    CHECK(world_text(true) == "on=1\n");
}

// ---- edge cases ----
TEST_CASE("raids: only deaths by a player's or an ally's hand are kills; falls and the like pay nothing") {
    Fight f;
    f.d.credit(f.spawned[0]);
    for (uint32_t id : f.spawned) f.d.on_died(id);
    CHECK(f.d.kills() == 1);
    f.now += 31 * kSec;
    f.spawn_from(f.d.tick(at(f.now, 20.6)));
    for (size_t i = 3; i < f.spawned.size(); ++i) f.d.on_died(f.spawned[i]);  // wave 2 fell off a cliff
    f.now += kSec;
    const auto a = f.d.tick(at(f.now, 20.7));
    const Action* r = find(a, Action::Reward);
    REQUIRE(r);
    CHECK(r->count == 1);
    // nobody's kill at all: no reward, the raiders are simply gone
    Fight g;
    for (uint32_t id : g.spawned) g.d.on_died(id);
    g.now += 31 * kSec;
    g.spawn_from(g.d.tick(at(g.now, 20.6)));
    for (size_t i = 3; i < g.spawned.size(); ++i) g.d.on_died(g.spawned[i]);
    g.now += kSec;
    const auto b = g.d.tick(at(g.now, 20.7));
    CHECK_FALSE(find(b, Action::Reward));
    CHECK(find(b, Action::Notice)->text.find("gone") != std::string::npos);
}

TEST_CASE("raids: sleeping through nightfall starts no raid at dawn") {
    Director d(settings());
    d.tick(at(0, 19.5));
    d.tick(at(kSec, 6.5));  // a bed: 19:30 -> 6:30 passes 20:00, but it is morning now
    CHECK(d.phase() == Phase::Idle);
    d.tick(at(2 * kSec, 19.9));
    d.tick(at(3 * kSec, 20.1));
    CHECK(d.phase() == Phase::Warning);  // the next real nightfall
}

TEST_CASE("raids: a raider that fell at its spawn, or made no way towards the altar for 60 s, is stuck") {
    StuckWatch s;
    const Vec3 spawn{kAltar.x + 50, kAltar.y + 2, kAltar.z};
    s.on_spawned(10, spawn, 0);
    s.on_spawned(11, spawn, 0);
    s.on_spawned(12, spawn, 0);
    CHECK_FALSE(s.stuck(10, {spawn.x, spawn.y - 1.8, spawn.z}, kAltar, 4 * kSec));  // landed (seen in game: 1.8 m)
    CHECK(s.stuck(11, {spawn.x, spawn.y - 14, spawn.z}, kAltar, 3 * kSec));        // fell into a hole or through the ground
    CHECK_FALSE(s.stuck(12, {spawn.x - 10, spawn.y, spawn.z}, kAltar, 30 * kSec));  // walking in
    CHECK_FALSE(s.stuck(12, {spawn.x - 20, spawn.y + 12, spawn.z}, kAltar, 50 * kSec));  // up a hill later: fine
    CHECK_FALSE(s.stuck(10, {spawn.x, spawn.y - 1.8, spawn.z}, kAltar, 59 * kSec));
    CHECK(s.stuck(10, {spawn.x - 1, spawn.y - 1.8, spawn.z}, kAltar, 61 * kSec));  // 60 s without 3 m of progress
    s.on_spawned(13, {kAltar.x + 5, kAltar.y, kAltar.z}, 0);
    CHECK_FALSE(s.stuck(13, {kAltar.x + 5, kAltar.y, kAltar.z}, kAltar, 120 * kSec));  // at the altar: it arrived
    s.forget(10);
    CHECK_FALSE(s.stuck(10, {0, 0, 0}, kAltar, 200 * kSec));  // unknown: not judged
}

TEST_CASE("raids: walked spots from a wider band are moved onto the ring, at their own height") {
    const std::vector<Base> bases = {{1, kAltar}};
    Breadcrumbs crumbs;
    crumbs.add({kAltar.x + 80, kAltar.y + 4, kAltar.z}, 0);  // walked 80 m out: outside the 40-60 m ring
    const auto pts = spawn_points(kAltar, bases, crumbs, ring_for(30), 30, 1, 3);
    REQUIRE(pts.size() == 1);
    CHECK(horizontal_distance(pts[0], kAltar) == doctest::Approx(60.0));  // moved in to the ring
    CHECK(pts[0].z == doctest::Approx(kAltar.z));                        // along its own direction
    CHECK(pts[0].y == doctest::Approx(kAltar.y + 5.0));                  // its height + 1 m
    Breadcrumbs inner;
    inner.add({kAltar.x, kAltar.y, kAltar.z + 33}, 0);  // 33 m: outside the base, inside the ring's band
    const auto p2 = spawn_points(kAltar, bases, inner, ring_for(30), 30, 1, 3);
    REQUIRE(p2.size() == 1);
    CHECK(horizontal_distance(p2[0], kAltar) == doctest::Approx(40.0));
}

// ---- walls (the "Raiders break walls" setting) ----

namespace {
WallSettings walls_on(double radius = 1.0, double scale = 1.0) {
    WallSettings s;
    s.on = true;
    s.hole_radius = radius;
    s.damage_scale = scale;
    return s;
}
std::vector<WallAction> of_kind(const std::vector<WallAction>& a, WallAction::Kind k) {
    std::vector<WallAction> out;
    for (const auto& x : a)
        if (x.kind == k) out.push_back(x);
    return out;
}
}  // namespace

TEST_CASE("walls: hole sizes and the difficulty scale") {
    CHECK(hole_radius_of("small") == 1.0);
    CHECK(hole_radius_of("medium") == 1.75);
    CHECK(hole_radius_of("large") == 2.5);
    CHECK(hole_radius_of("huge") == 1.0);
    CHECK(wall_damage_scale("easy") == 0.5);
    CHECK(wall_damage_scale("normal") == 1.0);
    CHECK(wall_damage_scale("hard") == 1.5);
    CHECK(wall_damage_scale("deadly") == 2.0);
    CHECK(wall_damage_scale("other") == 1.0);
}

TEST_CASE("walls: a small hole is a column from 0.75 m to 1.45 m over the feet; medium is one sphere") {
    const Hole small = hole_at({10, 99, 20}, 50, 1.0);
    CHECK(small.centre.x == 10);
    CHECK(small.centre.z == 20);
    CHECK(small.centre.y == doctest::Approx(50.75));
    CHECK(small.radius == 1.0);
    CHECK(small.height == doctest::Approx(0.7));
    const Hole medium = hole_at({0, 0, 0}, 0, 1.75);
    CHECK(medium.centre.y == doctest::Approx(1.3125));
    CHECK(medium.height == 0.0);
    CHECK(hole_at({0, 0, 0}, 0, 2.5).height == 0.0);
}

TEST_CASE("walls: gate damage: about 6 hits at normal, 12 easy, 3 deadly; unknown health 40") {
    CHECK(gate_damage(600, 1.0) == 100);
    CHECK(gate_damage(600, 0.5) == 50);
    CHECK(gate_damage(600, 2.0) == 200);
    CHECK(gate_damage(601, 1.0) == 101);  // rounded up
    CHECK(gate_damage(0, 1.0) == 40);
    CHECK(gate_damage(-5, 1.5) == 60);
    CHECK(gate_damage(1, 0.5) == 1);
    CHECK(gate_damage(2000000000, 2.0) == 100000);
}

TEST_CASE("walls: a raider looks after 10 s without 3 m of progress, only near the base, once per 10 s") {
    WallWork w;
    const Vec3 near{kAltar.x + 30, kAltar.y, kAltar.z};  // 30 m: inside base radius 30 + 20
    CHECK_FALSE(w.wants_look(1, near, kAltar, 30, 0));
    CHECK_FALSE(w.wants_look(1, near, kAltar, 30, 9 * kSec));
    CHECK(w.wants_look(1, {near.x - 1, near.y, near.z}, kAltar, 30, 10 * kSec));  // 1 m closer: no progress
    CHECK_FALSE(w.wants_look(1, near, kAltar, 30, 11 * kSec));                     // looked: not again so soon
    CHECK(w.wants_look(1, near, kAltar, 30, 20 * kSec));
    // progress resets the clock
    CHECK_FALSE(w.wants_look(2, near, kAltar, 30, 0));
    CHECK_FALSE(w.wants_look(2, {near.x - 4, near.y, near.z}, kAltar, 30, 8 * kSec));
    CHECK_FALSE(w.wants_look(2, {near.x - 4, near.y, near.z}, kAltar, 30, 17 * kSec));
    CHECK(w.wants_look(2, {near.x - 4, near.y, near.z}, kAltar, 30, 18 * kSec));
    // far from the base: never
    const Vec3 far{kAltar.x + 55, kAltar.y, kAltar.z};
    CHECK_FALSE(w.wants_look(3, far, kAltar, 30, 0));
    CHECK_FALSE(w.wants_look(3, far, kAltar, 30, 30 * kSec));
}

TEST_CASE("walls: the nearer of a wall and a gate is chosen; walls near each other share a spot; at most 4 spots") {
    WallWork w;
    const auto s = walls_on();
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    const Vec3 wall{pos.x - 1, pos.y + 1.2, pos.z};
    const Gate gate{77, {pos.x - 3, pos.y, pos.z}, 600};
    const auto a = w.assign(1, pos, wall, gate, s, 0);
    REQUIRE(a);
    CHECK(w.working(1));
    auto spots = w.spots();
    REQUIRE(spots.size() == 1);
    CHECK_FALSE(spots[0].gate);
    const auto b = w.assign(2, {pos.x, pos.y, pos.z + 1}, Vec3{wall.x, wall.y, wall.z + 1.4}, std::nullopt, s, 0);
    CHECK(b == a);  // within the hole radius + 0.5: the same spot
    const auto g = w.assign(3, {pos.x - 2, pos.y, pos.z + 1}, Vec3{pos.x - 6, pos.y, pos.z + 10}, gate, s, 0);
    REQUIRE(g);
    CHECK(g != a);
    spots = w.spots();
    REQUIRE(spots.size() == 2);
    CHECK(spots[1].gate);
    CHECK(spots[1].door == 77);
    CHECK(spots[1].gate_damage == 100);
    const auto g2 = w.assign(4, pos, std::nullopt, gate, s, 0);
    CHECK(g2 == g);  // the same door: the same spot
    CHECK(w.assign(5, pos, Vec3{pos.x, pos.y, pos.z + 20}, std::nullopt, s, 0));
    CHECK(w.assign(6, pos, Vec3{pos.x, pos.y, pos.z + 30}, std::nullopt, s, 0));
    CHECK_FALSE(w.assign(7, pos, Vec3{pos.x, pos.y, pos.z + 40}, std::nullopt, s, 0));  // a fifth spot: no
    CHECK_FALSE(w.working(7));
    CHECK_FALSE(w.assign(8, pos, std::nullopt, std::nullopt, s, 0));  // nothing found
}

TEST_CASE("walls: a worker within 3 m that has not swung for 10 s: a hit every 1.5 s; two such workers double the damage scale") {
    WallWork w;
    const auto s = walls_on(1.0, 1.5);
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    const Vec3 wall{pos.x - 1, pos.y + 1.2, pos.z};
    const auto spot = w.assign(1, pos, wall, std::nullopt, s, 0);
    REQUIRE(spot);
    std::map<uint32_t, Vec3> raiders{{1, pos}};
    auto a = w.tick(raiders, s, 0);
    CHECK(of_kind(a, WallAction::Hit).empty());  // in range now: its swings are waited for
    REQUIRE(of_kind(a, WallAction::OrderWall).size() == 1);  // the new worker's order
    CHECK(of_kind(a, WallAction::OrderWall)[0].raider == 1);
    CHECK(of_kind(a, WallAction::OrderWall)[0].at.x == wall.x);
    a = w.tick(raiders, s, 10000);
    REQUIRE(of_kind(a, WallAction::Hit).size() == 1);
    CHECK(of_kind(a, WallAction::Hit)[0].damage_scale == doctest::Approx(1.5));
    CHECK(of_kind(a, WallAction::Hit)[0].hole.centre.y == doctest::Approx(pos.y + 0.75));
    CHECK(of_kind(w.tick(raiders, s, 11000), WallAction::Hit).empty());
    CHECK(of_kind(w.tick(raiders, s, 11500), WallAction::Hit).size() == 1);
    CHECK(w.assign(2, {pos.x, pos.y, pos.z + 0.5}, wall, std::nullopt, s, 11500) == spot);
    raiders[2] = {pos.x, pos.y, pos.z + 0.5};
    a = w.tick(raiders, s, 13000);
    REQUIRE(of_kind(a, WallAction::Hit).size() == 1);
    CHECK(of_kind(a, WallAction::Hit)[0].damage_scale == doctest::Approx(1.5));  // the second one's 10 s run
    CHECK(of_kind(a, WallAction::OrderWall).size() == 2);  // the new worker, and the first one's 3 s renewal
    a = w.tick(raiders, s, 23000);
    REQUIRE(of_kind(a, WallAction::Hit).size() == 1);
    CHECK(of_kind(a, WallAction::Hit)[0].damage_scale == doctest::Approx(3.0));
    raiders[1] = {pos.x + 10, pos.y, pos.z};  // walked off
    raiders[2] = {pos.x + 10, pos.y, pos.z};
    CHECK(of_kind(w.tick(raiders, s, 24500), WallAction::Hit).empty());
}

TEST_CASE("walls (1.3.0): a wall worker within 3 m of the wall point is told to swing; farther it is walked up") {
    WallWork w;
    const auto s = walls_on();
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    const Vec3 wall{pos.x - 4, pos.y + 1.2, pos.z};
    REQUIRE(w.assign(1, pos, wall, std::nullopt, s, 0));
    std::map<uint32_t, Vec3> raiders{{1, pos}};
    auto a = of_kind(w.tick(raiders, s, 0), WallAction::OrderWall);
    REQUIRE(a.size() == 1);
    CHECK_FALSE(a[0].swing);
    raiders[1] = {wall.x + 2.9, pos.y, pos.z};
    a = of_kind(w.tick(raiders, s, 1000), WallAction::OrderWall);  // walked up: told to swing at once
    REQUIRE(a.size() == 1);
    CHECK(a[0].swing);
    CHECK(a[0].at.x == wall.x);
    CHECK(of_kind(w.tick(raiders, s, 2000), WallAction::OrderWall).empty());
    REQUIRE(of_kind(w.tick(raiders, s, 4000), WallAction::OrderWall).size() == 1);  // renewed every 3 s
    raiders[1] = {wall.x + 4.0, pos.y, pos.z};  // pushed back: walked up again at once
    a = of_kind(w.tick(raiders, s, 4500), WallAction::OrderWall);
    REQUIRE(a.size() == 1);
    CHECK_FALSE(a[0].swing);
    raiders[1] = {wall.x + 1.0, pos.y + 3.5, pos.z};  // near across, but on the floor above
    CHECK_FALSE(of_kind(w.tick(raiders, s, 7500), WallAction::OrderWall)[0].swing);
}

TEST_CASE("walls (1.3.0): a wall spot keeps the side its first worker was held up on; its orders carry it") {
    WallWork w;
    const auto s = walls_on();
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    const Vec3 wall{pos.x - 1.5, pos.y + 1.2, pos.z};
    REQUIRE(w.assign(1, pos, wall, std::nullopt, s, 0));
    std::map<uint32_t, Vec3> raiders{{1, {pos.x - 1, pos.y, pos.z + 2}}};  // it drifted along the wall since
    const auto a = of_kind(w.tick(raiders, s, 0), WallAction::OrderWall);
    REQUIRE(a.size() == 1);
    CHECK(a[0].side.x == doctest::Approx(pos.x));
    CHECK(a[0].side.z == doctest::Approx(pos.z));
    REQUIRE(w.assign(2, {pos.x - 3, pos.y, pos.z}, wall, std::nullopt, s, 0));  // joins: the side stays the first one's
    raiders[2] = {pos.x - 3, pos.y, pos.z};
    for (const auto& o : of_kind(w.tick(raiders, s, 3000), WallAction::OrderWall)) CHECK(o.side.x == doctest::Approx(pos.x));
}

TEST_CASE("walls (1.3.0): a wall worker's swing near its spot is a hit at once with 2.5 x the damage scale") {
    WallWork w;
    const auto s = walls_on(1.0, 2.0);
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    const Vec3 wall{pos.x - 1.5, pos.y + 1.2, pos.z};
    REQUIRE(w.assign(1, pos, wall, std::nullopt, s, 0));
    auto a = w.swung(1, pos, s, 500);
    REQUIRE(a.size() == 1);
    CHECK(a[0].kind == WallAction::Hit);
    CHECK(a[0].damage_scale == doctest::Approx(5.0));
    CHECK(a[0].hole.centre.x == doctest::Approx(wall.x));
    CHECK(w.swung(1, {pos.x + 5, pos.y, pos.z}, s, 900).empty());  // swung 6.5 m off: at something else
    CHECK(w.swung(2, pos, s, 900).empty());                        // works no spot
    REQUIRE(w.assign(3, pos, std::nullopt, Gate{9, {pos.x, pos.y, pos.z + 2}, 600}, s, 0));
    CHECK(w.swung(3, pos, s, 900).empty());  // a gate worker's swing lands on the gate itself
    WallSettings off = s;
    off.on = false;
    CHECK(w.swung(1, pos, off, 1000).empty());
}

TEST_CASE("walls (1.3.0): the timed hits stop while a worker swings and come back 10 s after its last swing") {
    WallWork w;
    const auto s = walls_on();
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    const Vec3 wall{pos.x - 1.5, pos.y + 1.2, pos.z};
    REQUIRE(w.assign(1, pos, wall, std::nullopt, s, 0));
    std::map<uint32_t, Vec3> raiders{{1, pos}};
    for (uint64_t t = 0; t < 10000; t += 500) CHECK(of_kind(w.tick(raiders, s, t), WallAction::Hit).empty());
    CHECK(of_kind(w.tick(raiders, s, 10000), WallAction::Hit).size() == 1);  // no swing in 10 s: the timed hits
    CHECK(of_kind(w.tick(raiders, s, 11500), WallAction::Hit).size() == 1);
    CHECK(w.swung(1, pos, s, 12000).size() == 1);  // it swings now: its hits come from its swings
    for (uint64_t t = 12000; t < 22000; t += 500) CHECK(of_kind(w.tick(raiders, s, t), WallAction::Hit).empty());
    CHECK(of_kind(w.tick(raiders, s, 22000), WallAction::Hit).size() == 1);  // its last swing was 10 s ago
    raiders[1] = {pos.x + 10, pos.y, pos.z};  // out of range for 2 s or more: its 10 s start again when it is back
    w.tick(raiders, s, 23000);
    w.tick(raiders, s, 25000);
    raiders[1] = pos;
    CHECK(of_kind(w.tick(raiders, s, 26000), WallAction::Hit).empty());
    CHECK(of_kind(w.tick(raiders, s, 35500), WallAction::Hit).empty());
    CHECK(of_kind(w.tick(raiders, s, 36000), WallAction::Hit).size() == 1);
}

TEST_CASE("walls (1.3.0, review): a worker hovering at the 3 m line keeps its timed hits and its order (no flip each second)") {
    WallWork w;
    const auto s = walls_on();
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    const Vec3 wall{pos.x - 2.9, pos.y + 1.2, pos.z};
    REQUIRE(w.assign(1, pos, wall, std::nullopt, s, 0));
    std::map<uint32_t, Vec3> raiders{{1, pos}};
    size_t hits = 0, orders = 0;
    for (uint64_t t = 0; t <= 30000; t += 1000) {
        raiders[1] = {wall.x + ((t / 1000) % 2 ? 3.1 : 2.9), pos.y, pos.z};
        const auto a = w.tick(raiders, s, t);
        hits += of_kind(a, WallAction::Hit).size();
        for (const auto& o : of_kind(a, WallAction::OrderWall)) {
            ++orders;
            CHECK(o.swing);
        }
    }
    CHECK(hits >= 10);    // from 10 s on, every 1.5 s or so
    CHECK(orders <= 11);  // the first one and its 3 s renewals
}

TEST_CASE("walls (1.3.0, review): a worker out of range for a second keeps its 10 s; for 2 s or more they start again") {
    WallWork w;
    const auto s = walls_on();
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    const Vec3 wall{pos.x - 2.0, pos.y + 1.2, pos.z};
    REQUIRE(w.assign(1, pos, wall, std::nullopt, s, 0));
    std::map<uint32_t, Vec3> raiders{{1, pos}};
    w.tick(raiders, s, 0);
    raiders[1] = {wall.x + 6, pos.y, pos.z};  // pushed off for a second
    CHECK(of_kind(w.tick(raiders, s, 5000), WallAction::Hit).empty());
    raiders[1] = pos;
    CHECK(of_kind(w.tick(raiders, s, 10000), WallAction::Hit).size() == 1);  // its 10 s since 0 still count
}

TEST_CASE("walls (1.3.0, review): FightClock: a raider fought lately for 10 s after it attacked or was hit") {
    FightClock f;
    CHECK_FALSE(f.lately(1, 0));
    f.fought(1, 1000);
    CHECK(f.lately(1, 10999));
    CHECK_FALSE(f.lately(1, 11000));
    f.fought(1, 20000);
    f.forget(1);
    CHECK_FALSE(f.lately(1, 20500));
    f.fought(2, 0);
    f.clear();
    CHECK_FALSE(f.lately(2, 1));
}

TEST_CASE("walls (1.3.0): the swing target faces the raider; the walk-up point is 1.5 m in front of the wall on its side") {
    CHECK(yaw_toward({0, 0, 0}, {0, 0, 5}) == doctest::Approx(0.0));    // north
    CHECK(yaw_toward({0, 0, 0}, {5, 0, 0}) == doctest::Approx(90.0));   // east
    CHECK(yaw_toward({0, 0, 0}, {-5, 0, 0}) == doctest::Approx(-90.0)); // west
    CHECK(std::fabs(yaw_toward({0, 0, 0}, {0, 0, -5})) == doctest::Approx(180.0));
    const Vec3 f = front_point({10, 5, 10}, {15, 2, 10}, 1.5);
    CHECK(f.x == doctest::Approx(11.5));
    CHECK(f.y == doctest::Approx(2.0));  // at the raider's height
    CHECK(f.z == doctest::Approx(10.0));
    const Vec3 same = front_point({10, 5, 10}, {10, 2, 10}, 1.5);  // standing on the point: stays where it is
    CHECK(same.x == doctest::Approx(10.0));
    CHECK(same.z == doctest::Approx(10.0));
}

TEST_CASE("walls (1.3.0): SwingWatch: a change of the last attack time is a swing; the first reading and no reading are not") {
    SwingWatch sw;
    CHECK_FALSE(sw.swung(1, 1000));  // the first reading
    CHECK_FALSE(sw.swung(1, 1000));
    CHECK(sw.swung(1, 5000));
    CHECK_FALSE(sw.swung(1, std::nullopt));  // no AI state (another kind of creature): never a swing
    CHECK_FALSE(sw.swung(1, 5000));
    CHECK_FALSE(sw.swung(2, 7000));
    sw.forget(1);
    CHECK_FALSE(sw.swung(1, 9000));  // forgotten: a first reading again
    sw.clear();
    CHECK_FALSE(sw.swung(2, 8000));
}

TEST_CASE("walls: a spot not through after 2 minutes is given up and its workers released") {
    WallWork w;
    const auto s = walls_on();
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    w.assign(1, pos, Vec3{pos.x - 1, pos.y + 1.2, pos.z}, std::nullopt, s, 0);
    std::map<uint32_t, Vec3> raiders{{1, pos}};
    w.tick(raiders, s, 0);
    CHECK(of_kind(w.tick(raiders, s, 119 * kSec), WallAction::GiveUp).empty());
    const auto a = w.tick(raiders, s, 120 * kSec);
    CHECK(of_kind(a, WallAction::GiveUp).size() == 1);
    REQUIRE(of_kind(a, WallAction::Release).size() == 1);
    CHECK(of_kind(a, WallAction::Release)[0].raider == 1);
    CHECK(of_kind(a, WallAction::Hit).empty());
    CHECK_FALSE(w.working(1));
    CHECK(w.spots().empty());
}

TEST_CASE("walls: through: wall_clear releases the workers and ends the spot") {
    WallWork w;
    const auto s = walls_on();
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    const auto spot = w.assign(1, pos, Vec3{pos.x - 1, pos.y + 1.2, pos.z}, std::nullopt, s, 0);
    w.assign(2, pos, Vec3{pos.x - 1, pos.y + 1.2, pos.z}, std::nullopt, s, 0);
    const auto a = w.wall_clear(*spot);
    CHECK(of_kind(a, WallAction::Release).size() == 2);
    CHECK(of_kind(a, WallAction::Done).size() == 1);
    CHECK_FALSE(w.working(1));
    CHECK(w.spots().empty());
    CHECK(w.wall_clear(*spot).empty());
}

TEST_CASE("walls: holding while the spot took damage in the last 5 s (a swing comes about every 4 s), not after") {
    WallWork w;
    const auto s = walls_on();
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    const auto spot = w.assign(1, pos, Vec3{pos.x - 1, pos.y + 1.2, pos.z}, std::nullopt, s, 0);
    CHECK_FALSE(w.holding(1, 0));
    w.on_hit(*spot, 0, 0, 1000);  // nothing took damage
    CHECK_FALSE(w.holding(1, 1000));
    w.on_hit(*spot, 5, 0, 2000);
    CHECK(w.holding(1, 2000));
    CHECK(w.holding(1, 6900));
    CHECK_FALSE(w.holding(1, 7100));
    CHECK_FALSE(w.holding(2, 2000));  // not a worker
    const auto g = w.assign(3, pos, std::nullopt, Gate{9, {pos.x - 2, pos.y, pos.z}, 600}, s, 0);
    w.on_gate_health(*g, 600, 1000);  // the first reading is no damage
    CHECK_FALSE(w.holding(3, 1000));
    w.on_gate_health(*g, 500, 2000);
    CHECK(w.holding(3, 2000));
}

TEST_CASE("StuckWatch: hold restarts the 60 s") {
    StuckWatch s;
    const Vec3 spawn{kAltar.x + 50, kAltar.y, kAltar.z};
    s.on_spawned(1, spawn, 0);
    CHECK_FALSE(s.stuck(1, spawn, kAltar, 11 * kSec));  // first look
    s.hold(1, 50 * kSec);
    CHECK_FALSE(s.stuck(1, spawn, kAltar, 100 * kSec));
    CHECK(s.stuck(1, spawn, kAltar, 111 * kSec));
    s.hold(99, 0);  // unknown: nothing
}

TEST_CASE("walls (review focus 5): a forgotten raider leaves its spot; a spot without workers stops hitting") {
    WallWork w;
    const auto s = walls_on();
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    w.assign(1, pos, Vec3{pos.x - 1, pos.y + 1.2, pos.z}, std::nullopt, s, 0);
    w.assign(2, pos, Vec3{pos.x - 1, pos.y + 1.2, pos.z}, std::nullopt, s, 0);
    w.forget(1);
    CHECK_FALSE(w.working(1));
    std::map<uint32_t, Vec3> raiders{{2, pos}};
    w.tick(raiders, s, 0);
    CHECK(of_kind(w.tick(raiders, s, 10000), WallAction::Hit).size() == 1);
    raiders.clear();  // raider 2 is gone too (not among the living)
    const auto a = w.tick(raiders, s, 11500);
    CHECK(of_kind(a, WallAction::Hit).empty());
    CHECK(of_kind(a, WallAction::GiveUp).size() == 1);  // its swing target goes
    CHECK(w.spots().empty());
    // a gate whose only worker is released: the spot ends at the next tick
    w.assign(3, pos, std::nullopt, Gate{9, {pos.x - 2, pos.y, pos.z}, 600}, s, 2000);
    w.release(3);
    CHECK_FALSE(w.working(3));
    const auto b = w.tick({}, s, 2500);
    CHECK(of_kind(b, WallAction::GiveUp).size() == 1);
    CHECK(of_kind(b, WallAction::OrderGate).empty());
    CHECK(w.spots().empty());
}

TEST_CASE("walls: off in the settings: no action; spots end and their workers are released") {
    WallWork w;
    WallSettings off;
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    std::map<uint32_t, Vec3> raiders{{1, pos}};
    CHECK(w.tick(raiders, off, 0).empty());
    w.assign(1, pos, Vec3{pos.x - 1, pos.y + 1.2, pos.z}, std::nullopt, walls_on(), 0);
    const auto a = w.tick(raiders, off, 1000);
    CHECK(of_kind(a, WallAction::Release).size() == 1);
    CHECK(of_kind(a, WallAction::GiveUp).size() == 1);
    CHECK(of_kind(a, WallAction::Hit).empty());
    CHECK(w.spots().empty());
}

TEST_CASE("walls: the gate worker gets its order with the gate's per-hit damage, renewed every 3 s; no Hit for a gate") {
    WallWork w;
    const auto s = walls_on(1.0, 2.0);
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    w.assign(1, pos, std::nullopt, Gate{9, {pos.x - 2, pos.y, pos.z}, 600}, s, 0);
    std::map<uint32_t, Vec3> raiders{{1, pos}};
    auto a = w.tick(raiders, s, 0);
    CHECK(of_kind(a, WallAction::Hit).empty());
    REQUIRE(of_kind(a, WallAction::OrderGate).size() == 1);
    CHECK(of_kind(a, WallAction::OrderGate)[0].door == 9);
    CHECK(of_kind(a, WallAction::OrderGate)[0].gate_damage == 200);
    CHECK(of_kind(w.tick(raiders, s, 2000), WallAction::OrderGate).empty());
    CHECK(of_kind(w.tick(raiders, s, 3000), WallAction::OrderGate).size() == 1);
}


TEST_CASE("walls: the settings: hole size, drops and the difficulty scale (a preset's difficulty while it is chosen)") {
    const WallSettings a = wall_settings(true, "medium", true, "custom", "easy");
    CHECK(a.on);
    CHECK(a.hole_radius == 1.75);
    CHECK(a.drops);
    CHECK(a.damage_scale == 0.5);
    const WallSettings b = wall_settings(false, "large", false, "hard", "easy");  // the hard preset's difficulty
    CHECK_FALSE(b.on);
    CHECK(b.hole_radius == 2.5);
    CHECK_FALSE(b.drops);
    CHECK(b.damage_scale == 1.5);
    CHECK(wall_settings(true, "small", false, "gentle", "deadly").damage_scale == 0.5);
    CHECK(wall_settings(true, "?", false, "nonsense", "deadly").damage_scale == 2.0);  // not a preset: its own
}

TEST_CASE("walls (1.3.0): Raiders also break walls of terrain blocks: off by default, only with Raiders break walls") {
    CHECK_FALSE(wall_settings(true, "small", false, "custom", "normal").terrain);
    CHECK(wall_settings(true, "small", false, "custom", "normal", 1.0, true).terrain);
    CHECK_FALSE(wall_settings(false, "small", false, "custom", "normal", 1.0, true).terrain);
}

TEST_CASE("walls: Wall strength is x hits: the damage scale (walls and gates) is divided by it; 0.5..10") {
    CHECK(wall_settings(true, "small", false, "custom", "normal", 2.0).damage_scale == doctest::Approx(0.5));
    CHECK(wall_settings(true, "small", false, "custom", "hard", 3.0).damage_scale == doctest::Approx(0.5));
    CHECK(wall_settings(true, "small", false, "custom", "normal", 0.5).damage_scale == doctest::Approx(2.0));
    CHECK(wall_settings(true, "small", false, "hard", "easy", 1.5).damage_scale == doctest::Approx(1.0));  // preset's
    CHECK(wall_settings(true, "small", false, "custom", "normal", 0.0).damage_scale == doctest::Approx(2.0));   // 0.5
    CHECK(wall_settings(true, "small", false, "custom", "normal", -3.0).damage_scale == doctest::Approx(2.0));  // 0.5
    CHECK(wall_settings(true, "small", false, "custom", "normal", 40.0).damage_scale == doctest::Approx(0.1));  // 10
    CHECK(wall_settings(true, "small", false, "custom", "normal", std::nan("")).damage_scale == doctest::Approx(1.0));
    CHECK(gate_damage(600, 1.0 / 3.0) == 34);  // strength 3: a gate takes 18 hits, not 6
}

TEST_CASE("raids: mod.json: Raiders break walls (off), Hole size (small), Broken blocks drop items (off); presets leave them") {
    std::ifstream f(std::string(EMF_SOURCE_DIR) + "/mods/raids/mod.json", std::ios::binary);
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const auto r = emf::core::parse_manifest(ss.str());
    REQUIRE_MESSAGE(r.manifest, r.error);
    const auto def = [&](const std::string& key) -> const emf::core::SettingDef* {
        for (const auto& d : r.manifest->settings)
            if (d.key == key) return &d;
        return nullptr;
    };
    const auto* on = def("break_walls");
    REQUIRE(on);
    CHECK(on->label == "Raiders break walls");
    CHECK(on->default_value == "0");
    CHECK(on->when_key.empty());
    const auto* hole = def("hole_size");
    REQUIRE(hole);
    CHECK(hole->label == "Hole size");
    CHECK(hole->default_value == "small");
    std::vector<std::string> values;
    for (const auto& o : hole->options) values.push_back(o.value);
    CHECK(values == std::vector<std::string>{"small", "medium", "large"});
    CHECK(hole->when_key == "break_walls");
    const auto* drops = def("wall_drops");
    REQUIRE(drops);
    CHECK(drops->label == "Broken blocks drop items");
    CHECK(drops->default_value == "0");
    CHECK(drops->when_key == "break_walls");
    const auto* strength = def("wall_strength");
    REQUIRE(strength);
    CHECK(strength->label == "Wall strength (x hits)");
    CHECK(strength->default_value == "1");
    CHECK(strength->slider);
    CHECK(strength->min == 0.5);
    CHECK(strength->max == 10.0);
    CHECK(strength->step == 0.5);
    CHECK(strength->live);
    CHECK(strength->group == "Walls");
    CHECK(strength->when_key == "break_walls");
    for (const auto& key : preset_keys()) {
        CHECK(key != "break_walls");
        CHECK(key != "hole_size");
        CHECK(key != "wall_drops");
        CHECK(key != "wall_strength");
    }
    CHECK(emf::core::version_text(r.manifest->version) == "1.3.0");
}

TEST_CASE("walls (review I1): after a give-up the raider does not look again until it makes 3 m of progress") {
    WallWork w;
    const auto s = walls_on();
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    w.wants_look(1, pos, kAltar, 30, 0);
    w.assign(1, pos, Vec3{pos.x - 1, pos.y + 1.2, pos.z}, std::nullopt, s, 0);
    std::map<uint32_t, Vec3> raiders{{1, pos}};
    w.tick(raiders, s, 0);
    REQUIRE(of_kind(w.tick(raiders, s, 120 * kSec), WallAction::GiveUp).size() == 1);
    CHECK_FALSE(w.wants_look(1, pos, kAltar, 30, 121 * kSec));
    CHECK_FALSE(w.wants_look(1, pos, kAltar, 30, 200 * kSec));  // no progress: never again (today's 60 s rule removes it)
    const Vec3 closer{pos.x - 4, pos.y, pos.z};
    CHECK_FALSE(w.wants_look(1, closer, kAltar, 30, 201 * kSec));  // 4 m closer: it may look again later
    CHECK_FALSE(w.wants_look(1, closer, kAltar, 30, 205 * kSec));
    CHECK(w.wants_look(1, closer, kAltar, 30, 212 * kSec));       // held up again for 10 s at a new place
}

TEST_CASE("walls (review I3): the hole goes deeper: advance moves the spot's point and hole, at most 3 m from where it began") {
    WallWork w;
    const auto s = walls_on();
    const Vec3 pos{kAltar.x + 20, kAltar.y, kAltar.z};
    const Vec3 wall{pos.x - 1, pos.y + 1.2, pos.z};
    const auto spot = w.assign(1, pos, wall, std::nullopt, s, 0);
    REQUIRE(spot);
    CHECK(w.advance(*spot, Vec3{wall.x - 1.0, wall.y, wall.z}, pos.y, s));
    auto sp = w.spots();
    REQUIRE(sp.size() == 1);
    CHECK(sp[0].point.x == doctest::Approx(wall.x - 1.0));
    CHECK(sp[0].hole.centre.x == doctest::Approx(wall.x - 1.0));
    CHECK(sp[0].hole.centre.y == doctest::Approx(pos.y + 0.75));
    CHECK(w.advance(*spot, Vec3{wall.x - 3.0, wall.y, wall.z}, pos.y, s));
    CHECK_FALSE(w.advance(*spot, Vec3{wall.x - 3.5, wall.y, wall.z}, pos.y, s));  // past 3 m: a tunnel, not a hole
    CHECK(w.spots()[0].point.x == doctest::Approx(wall.x - 3.0));
    CHECK_FALSE(w.advance(99, wall, pos.y, s));
}
