// Night Raids (mods/raids): the raid's state machine, the spawn ring, templates, levels and rewards, without the game.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "../mods/raids/raid_logic.h"

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
