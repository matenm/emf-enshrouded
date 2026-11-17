// Breaking blocks (API v44): the rules before the game is called (hits per material, never terrain, hole spheres,
// request checks, drops with a remainder).
#include <doctest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "core/api_sizes.h"
#include "core/block_rules.h"
#include "emf/emf_api.h"

using namespace emf::core;

namespace {

// Straw (id 2, Soft, 100), wood logs (id 4, ModeratelyHard, 250), stone (id 7, VeryHard, 1000), id 11 Unbreakable.
struct Facts {
    MaterialFacts f[128] = {};
    Facts() {
        f[2] = {true, 0, 100, 4, {}};
        f[4] = {true, 2, 250, 2, {}};
        f[7] = {true, 4, 1000, 8, {}};
        f[11] = {true, 5, 1000, 1, {}};
        f[2].item_guid[0] = 0x22;
        f[7].item_guid[0] = 0x77;
    }
};

BreakSpec damage_spec() {
    BreakSpec s;
    s.instant = false;
    for (int i = 0; i < 5; ++i) s.damage[i] = kDefaultBreakDamage[i];
    return s;
}

EmfBreakBlocks good_request() {
    EmfBreakBlocks r{};
    r.size = sizeof r;
    r.x = 10;
    r.y = 20;
    r.z = 30;
    r.radius = 1;
    return r;
}

}  // namespace

TEST_CASE("block rules: damage table: hits = health / (damage x mining factor), per hardness class") {
    Facts facts;
    float t[256];
    build_break_table(facts.f, damage_spec(), 1.0f, t);
    CHECK(t[128 + 7] == doctest::Approx(1000.0 / 50));  // the game's T4 pickaxe: 100/100/95/60/50
    CHECK(t[128 + 2] == doctest::Approx(100.0 / 100));
    CHECK(t[128 + 4] == doctest::Approx(250.0 / 95));
    build_break_table(facts.f, damage_spec(), 1.5f, t);
    CHECK(t[128 + 7] == doctest::Approx(1000.0 / 75));
}

TEST_CASE("block rules: terrain entries are always 0") {
    MaterialFacts all[128];
    for (auto& m : all) m = {true, 0, 100, 1, {}};
    float t[256];
    BreakSpec s = damage_spec();
    build_break_table(all, s, 1.0f, t);
    for (int m = 0; m < 128; ++m) CHECK(t[m] == 0.0f);
    s.instant = true;
    build_break_table(all, s, 1.0f, t);
    for (int m = 0; m < 128; ++m) CHECK(t[m] == 0.0f);
    for (int m = 128; m < 256; ++m) CHECK(t[m] == 1.0f);
}

TEST_CASE("block rules: Unbreakable and unknown materials never break, in either mode") {
    Facts facts;
    float t[256];
    BreakSpec s = damage_spec();
    build_break_table(facts.f, s, 1.0f, t);
    CHECK(t[128 + 11] == 0.0f);
    CHECK(t[128 + 3] == 0.0f);  // no config
    s.instant = true;
    build_break_table(facts.f, s, 1.0f, t);
    CHECK(t[128 + 11] == 0.0f);
    CHECK(t[128 + 3] == 0.0f);
}

TEST_CASE("block rules: a materials list limits the table") {
    Facts facts;
    float t[256];
    BreakSpec s = damage_spec();
    s.materials = {7};
    build_break_table(facts.f, s, 1.0f, t);
    CHECK(t[128 + 7] > 0.0f);
    CHECK(t[128 + 2] == 0.0f);
    CHECK(t[128 + 4] == 0.0f);
}

TEST_CASE("block rules: break mode = 1 per breakable material; a zero damage class is immune in damage mode") {
    Facts facts;
    float t[256];
    BreakSpec s = damage_spec();
    s.instant = true;
    build_break_table(facts.f, s, 1.0f, t);
    CHECK(t[128 + 2] == 1.0f);
    CHECK(t[128 + 4] == 1.0f);
    CHECK(t[128 + 7] == 1.0f);
    s.instant = false;
    s.damage[4] = 0;  // VeryHard
    build_break_table(facts.f, s, 1.0f, t);
    CHECK(t[128 + 7] == 0.0f);
    CHECK(t[128 + 2] > 0.0f);
}

TEST_CASE("block rules: hole spheres") {
    const auto two = hole_spheres(0, 0.75f, 0, 1.0f, 0.7f);
    REQUIRE(two.size() == 2);
    CHECK(two[0].y == doctest::Approx(0.75));
    CHECK(two[1].y == doctest::Approx(1.45));
    CHECK(two[0].r == doctest::Approx(1.0));
    CHECK(hole_spheres(1, 2, 3, 1.75f, 0).size() == 1);
    const auto five = hole_spheres(0, 0, 0, 1.0f, 4.0f);
    REQUIRE(five.size() == 5);
    CHECK(five[4].y == doctest::Approx(4.0));
    CHECK(five[2].y == doctest::Approx(2.0));
}

TEST_CASE("block rules: break_check refusals") {
    CHECK(break_check(good_request()).empty());
    auto bad = [](auto change) {
        EmfBreakBlocks r = good_request();
        change(r);
        return break_check(r);
    };
    CHECK_FALSE(bad([](EmfBreakBlocks& r) { r.radius = 3.6f; }).empty());
    CHECK_FALSE(bad([](EmfBreakBlocks& r) { r.radius = 0.2f; }).empty());
    CHECK(bad([](EmfBreakBlocks& r) { r.radius = 0; }).empty());  // 0 = 1 m
    CHECK_FALSE(bad([](EmfBreakBlocks& r) { r.height = 4.1f; }).empty());
    CHECK_FALSE(bad([](EmfBreakBlocks& r) { r.height = -0.1f; }).empty());
    CHECK_FALSE(bad([](EmfBreakBlocks& r) {
                    r.radius = 0.5f;
                    r.height = 4.0f;  // 9 spheres
                }).empty());
    CHECK_FALSE(bad([](EmfBreakBlocks& r) { r.damage[0] = -1; }).empty());
    CHECK_FALSE(bad([](EmfBreakBlocks& r) { r.damage[4] = 100001; }).empty());
    static const uint8_t k128[1] = {128};
    CHECK_FALSE(bad([](EmfBreakBlocks& r) {
                    r.material_count = 1;
                    r.materials = k128;
                }).empty());
    static uint8_t many[129] = {};
    CHECK_FALSE(bad([](EmfBreakBlocks& r) {
                    r.material_count = 129;
                    r.materials = many;
                }).empty());
    CHECK_FALSE(bad([](EmfBreakBlocks& r) { r.material_count = 1; }).empty());  // no list
    CHECK_FALSE(bad([](EmfBreakBlocks& r) { r.flags = 0x10; }).empty());
    CHECK_FALSE(bad([](EmfBreakBlocks& r) { r.x = std::numeric_limits<float>::quiet_NaN(); }).empty());
    CHECK_FALSE(bad([](EmfBreakBlocks& r) { r.z = std::numeric_limits<float>::infinity(); }).empty());
    CHECK_FALSE(bad([](EmfBreakBlocks& r) { r.size = kBreakBlocksV44Size - 1; }).empty());
}

TEST_CASE("block rules: the spec a request makes (defaults: 1 m, about a T4 pickaxe)") {
    EmfBreakBlocks r = good_request();
    r.radius = 0;
    BreakSpec s = break_spec(r);
    CHECK_FALSE(s.instant);
    for (int i = 0; i < 5; ++i) CHECK(s.damage[i] == kDefaultBreakDamage[i]);
    CHECK(s.materials.empty());
    CHECK(request_radius(r) == doctest::Approx(1.0));
    static const uint8_t list[2] = {7, 4};
    r.flags = EMF_BREAK_INSTANT;
    r.damage[2] = 5;
    r.material_count = 2;
    r.materials = list;
    s = break_spec(r);
    CHECK(s.instant);
    CHECK(s.damage[0] == 0);
    CHECK(s.damage[2] == 5);
    CHECK(s.materials == std::vector<uint8_t>{7, 4});
}

TEST_CASE("block rules: drops keep the remainder per material") {
    Facts facts;
    float carry[128] = {};
    float removed[256] = {};
    removed[128 + 2] = 10;  // straw: 4 blocks per item
    auto d = break_drops(removed, facts.f, carry);
    REQUIRE(d.size() == 1);
    CHECK(d[0].building_id == 2);
    CHECK(d[0].count == 2);
    CHECK(carry[2] == doctest::Approx(2.0));
    removed[128 + 2] = 2;
    d = break_drops(removed, facts.f, carry);
    REQUIRE(d.size() == 1);
    CHECK(d[0].count == 1);
    CHECK(carry[2] == doctest::Approx(0.0));
    std::memset(removed, 0, sizeof removed);
    removed[128 + 4] = 9;  // wood logs: rate 2, but no item GUID: nothing
    CHECK(break_drops(removed, facts.f, carry).empty());
    facts.f[7].loot_rate = 0;  // rate 0: nothing
    removed[128 + 7] = 50;
    CHECK(break_drops(removed, facts.f, carry).empty());
    removed[0] = 100;  // terrain counts never drop
    CHECK(break_drops(removed, facts.f, carry).empty());
}

TEST_CASE("block rules (review I2): a hit worth less than 1/255 of a block's health damages nothing, so its entry is 0") {
    MaterialFacts f[128] = {};
    f[9] = {true, 4, 5000, 1, {}};  // metal of 5000 health
    f[7] = {true, 4, 2550, 1, {}};  // exactly 255 hits at 10
    BreakSpec s;
    for (int i = 0; i < 5; ++i) s.damage[i] = 10;
    float t[256];
    build_break_table(f, s, 1.0f, t);
    CHECK(t[128 + 9] == 0.0f);                       // 500 hits a byte would need: floor(255 / 500) = 0
    CHECK(t[128 + 7] == doctest::Approx(255.0));     // one byte a hit still counts
    build_break_table(f, s, 0.75f, t);               // the Survival mining factor
    CHECK(t[128 + 7] == 0.0f);
}

TEST_CASE("block rules: world_verdict: a building cell whose material equals the shipped world's is the world's own; a new or another one is a player's; an unknown base is unknown; no building cell is 0") {
    CHECK(world_verdict(0x0000, true, 0x0000) == 0);   // empty
    CHECK(world_verdict(0xFF05, true, 0xFF05) == 0);   // terrain
    CHECK(world_verdict(0xFF8A, true, 0xFF8A) == 1);   // a ruin's stone as the game shipped it
    CHECK(world_verdict(0x7F8A, true, 0xFF8A) == 1);   // the same with other paint (decay, overgrowth: the high byte)
    CHECK(world_verdict(0x008A, true, 0xFF8A) == 1);   // a ruin's stone repainted unweathered (high byte 0)
    CHECK(world_verdict(0xFF8A, true, 0x0000) == 0);   // placed on air
    CHECK(world_verdict(0x008F, true, 0x0000) == 0);   // a block placed with the hammer (high byte 0) on air
    CHECK(world_verdict(0xFF8F, true, 0xFF8A) == 0);   // another material than the shipped one
    CHECK(world_verdict(0xFF8A, true, 0xFF05) == 0);   // built where terrain was
    CHECK(world_verdict(0xFF8A, false, 0) == -1);      // the base is not known (not loaded)
    CHECK(world_verdict(0xFF05, false, 0) == 0);       // terrain: no building block, whatever the base
}

TEST_CASE("block rules: shrink_off_world: a sphere stops just short of the nearest world (or unknown) building cell; none near: the radius stays; under 0.25 m: skipped") {
    // cells are 0.5 m; cell (i, j, k) has its centre at ((i + 0.5) / 2, ...)
    auto none = [](int32_t, int32_t, int32_t) { return 0; };
    CHECK(shrink_off_world(10.25f, 20.25f, 30.25f, 2.0f, none) == doctest::Approx(2.0f));
    // a ruin cell whose centre is 1 m from the sphere's centre: (22, 40, 60) -> (11.25, 20.25, 30.25)
    auto ruin = [](int32_t i, int32_t j, int32_t k) { return i == 22 && j == 40 && k == 60 ? 1 : 0; };
    const float r = shrink_off_world(10.25f, 20.25f, 30.25f, 2.0f, ruin);
    CHECK(r == doctest::Approx(0.99f));
    CHECK(r < 1.0f);  // the game's kernel breaks a cell whose centre is within the radius: strictly short of it
    // an unknown base counts as the world's own
    auto unknown = [](int32_t i, int32_t j, int32_t k) { return i == 22 && j == 40 && k == 60 ? -1 : 0; };
    CHECK(shrink_off_world(10.25f, 20.25f, 30.25f, 2.0f, unknown) == doctest::Approx(0.99f));
    // a ruin cell farther than the radius changes nothing
    CHECK(shrink_off_world(10.25f, 20.25f, 30.25f, 0.9f, ruin) == doctest::Approx(0.9f));
    // the ruin cell right next to the centre: less than 0.25 m left, the sphere is skipped
    CHECK(shrink_off_world(11.05f, 20.25f, 30.25f, 2.0f, ruin) == 0.0f);  // 0.2 m away
    // the nearest of several counts; a cell exactly at the radius counts too (the kernel's test includes it)
    auto two = [](int32_t i, int32_t j, int32_t k) {
        if (i == 22 && j == 40 && k == 60) return 1;   // 1 m
        if (i == 20 && j == 43 && k == 60) return 1;   // 1.5 m up
        return 0;
    };
    CHECK(shrink_off_world(10.25f, 20.25f, 30.25f, 3.0f, two) == doctest::Approx(0.99f));
    CHECK(shrink_off_world(10.25f, 20.25f, 30.25f, 1.0f, ruin) == doctest::Approx(0.99f));
    // negative coordinates
    auto neg = [](int32_t i, int32_t j, int32_t k) { return i == -3 && j == 0 && k == -3 ? 1 : 0; };  // (-1.25, 0.25, -1.25)
    CHECK(shrink_off_world(-0.25f, 0.25f, -1.25f, 2.0f, neg) == doctest::Approx(0.99f));
}

TEST_CASE("block rules: EMF_BREAK_WORLD (v45) is a known flag and asks to break the world's own blocks too") {
    EmfBreakBlocks r{};
    r.size = sizeof r;
    r.radius = 1;
    r.flags = EMF_BREAK_WORLD;
    CHECK(break_check(r).empty());
    CHECK(break_spec(r).world);
    r.flags = 0;
    CHECK_FALSE(break_spec(r).world);
    r.flags = 0x10;
    CHECK(break_check(r) == "unknown flags");
}

// ---- API v47: terrain blocks a player placed (Divusinski: walls of Stone or Granite stop every raid) ----

TEST_CASE("block rules: EMF_BREAK_TERRAIN (v47) is a known flag and asks for the terrain a player placed too") {
    EmfBreakBlocks r{};
    r.size = sizeof r;
    r.radius = 1;
    r.flags = EMF_BREAK_TERRAIN;
    CHECK(break_check(r).empty());
    CHECK(break_spec(r).terrain);
    r.flags = 0;
    CHECK_FALSE(break_spec(r).terrain);
}

TEST_CASE("block rules: terrain_verdict: terrain where the shipped world had no ground is a player's; ground stays the world's") {
    CHECK(terrain_verdict(0xFF43, true, 0x0000) == 0);   // Stone (67) placed on air
    CHECK(terrain_verdict(0xFF43, true, 0x0043) == 0);   // where the shipped cell had a material but no fill
    CHECK(terrain_verdict(0xFF43, true, 0xFF8A) == 0);   // where a ruin block was (a player replaced it)
    CHECK(terrain_verdict(0xFF43, true, 0xFF05) == 1);   // natural ground, as shipped or refilled with another material
    CHECK(terrain_verdict(0x3005, true, 0xFF05) == 1);   // natural ground a player dug into (less fill now)
    CHECK(terrain_verdict(0xFF05, true, 0x2005) == 1);   // a thin natural surface cell, filled up: still the ground
    CHECK(terrain_verdict(0xFF43, false, 0) == -1);      // the shipped world not known here
    CHECK(terrain_verdict(0x0000, true, 0x0000) == 0);   // empty: no terrain
    CHECK(terrain_verdict(0x0043, true, 0x0000) == 0);   // terrain material without fill: empty
    CHECK(terrain_verdict(0x008F, true, 0x0000) == 0);   // a building block: no terrain
    CHECK(terrain_verdict(0x008F, false, 0) == 0);
}

TEST_CASE("block rules: the terrain table: hits per terrain material from its health and hardness, as for buildings") {
    MaterialFacts t[128] = {};
    t[5] = {true, 0, 50, 10, {}};    // dirt: Soft, 50
    t[67] = {true, 4, 400, 5, {}};   // Stone: VeryHard, 400
    t[90] = {true, 5, 400, 5, {}};   // Unbreakable
    t[91] = {true, 2, 0, 5, {}};     // no health known
    float table[128];
    BreakSpec s = damage_spec();  // T4 pickaxe: 100, 100, 95, 60, 50
    build_terrain_table(t, s, 1.0f, table);
    CHECK(table[0] == 0.0f);
    CHECK(table[5] == doctest::Approx(0.5f));   // 50 / 100
    CHECK(table[67] == doctest::Approx(8.0f));  // 400 / 50
    CHECK(table[90] == 0.0f);
    CHECK(table[91] == 0.0f);
    CHECK(table[6] == 0.0f);  // no config
    build_terrain_table(t, s, 2.0f, table);  // the world's mining damage x2
    CHECK(table[67] == doctest::Approx(4.0f));
    s.materials = {7};  // a building list never limits terrain
    build_terrain_table(t, s, 1.0f, table);
    CHECK(table[67] == doctest::Approx(8.0f));
    s.instant = true;
    build_terrain_table(t, s, 1.0f, table);
    CHECK(table[5] == 1.0f);
    CHECK(table[67] == 1.0f);
    CHECK(table[90] == 0.0f);  // Unbreakable never
}

TEST_CASE("block rules: EMF's terrain damage adds up per cell and heals 20 s after the cell's last hit") {
    TerrainDamage d;
    CHECK(d.add(1, 2, 3, 100, 1000) == 100);
    CHECK(d.add(1, 2, 3, 100, 2000) == 200);
    CHECK(d.add(1, 2, 3, 100, 3000) == 255);  // full: 255 at most
    CHECK(d.add(4, 2, 3, 10, 3000) == 10);
    CHECK(d.size() == 2);
    d.heal(3000 + TerrainDamage::kHealMs);  // exactly 20 s: kept
    CHECK(d.size() == 2);
    d.heal(3001 + TerrainDamage::kHealMs);
    CHECK(d.size() == 0);
    CHECK(d.add(1, 2, 3, 5, 30000) == 5);  // healed: starts again
    d.forget(1, 2, 3);
    CHECK(d.size() == 0);
    d.add(7, 7, 7, 1, 0);
    d.clear();
    CHECK(d.size() == 0);
}

TEST_CASE("block rules: removed terrain cells become runs: same column, consecutive heights, one material") {
    std::vector<TerrainCell> cells = {{1, 5, 1, 67}, {1, 3, 1, 67}, {1, 4, 1, 67}, {1, 7, 1, 67}, {2, 3, 1, 67},
                                      {1, 6, 1, 5}};
    const auto runs = column_runs(cells);
    REQUIRE(runs.size() == 4);
    CHECK((runs[0].x == 1 && runs[0].z == 1 && runs[0].y0 == 3 && runs[0].y1 == 5));  // 3, 4, 5
    CHECK((runs[1].x == 1 && runs[1].y0 == 6 && runs[1].y1 == 6));                    // dirt between
    CHECK((runs[2].x == 1 && runs[2].y0 == 7 && runs[2].y1 == 7));
    CHECK((runs[3].x == 2 && runs[3].y0 == 3 && runs[3].y1 == 3));
    CHECK(column_runs({}).empty());
}

namespace {
// A Stone wall a player built (x 2..3 m, y 0..2 m, z 0..1 m, cells 4..5 x 0..3 x 0..1) on natural dirt (y < 0) next to
// a natural hill of Stone (x >= 3 m): the shipped world has dirt below y 0 and the hill, nothing where the wall is.
TerrainCellRead world_at(int32_t i, int32_t j, int32_t k) {
    TerrainCellRead c;
    c.ok = true;
    c.base_known = true;
    const bool wall = i >= 4 && i <= 5 && j >= 0 && j <= 3 && k >= 0 && k <= 1;
    const bool hill = i >= 6 && j <= 3;
    if (j < 0) c.base = 0xFF05;
    else if (hill) c.base = 0xFF43;
    c.cur = wall ? 0xFF43 : c.base;
    return c;
}
float stone_table(float hits, float table[128]) {
    for (int m = 0; m < 128; ++m) table[m] = 0.0f;
    table[5] = hits;
    table[67] = hits;
    return hits;
}
}  // namespace

TEST_CASE("block rules: a terrain sphere breaks only the terrain a player placed, never the natural ground or a hill") {
    float table[128];
    stone_table(1.0f, table);
    TerrainDamage d;
    // a sphere of 1.5 m at the wall's middle reaches the dirt below and the hill beside it
    const auto res = terrain_sphere({2.5f, 1.0f, 0.5f, 1.5f}, table, true, d, 1000, world_at);
    CHECK(res.removed == 16);  // the whole wall (2 x 4 x 2 cells): every centre within 1.5 m
    CHECK(res.damaged == 16);
    CHECK(res.removed_per_material[67] == doctest::Approx(16.0f));
    CHECK(res.removed_per_material[5] == 0.0f);
    uint32_t cells = 0;
    for (const auto& r : res.runs) {
        CHECK((r.x >= 4 && r.x <= 5 && r.z >= 0 && r.z <= 1));  // never the hill (x >= 6) nor the dirt (y < 0)
        CHECK(r.y0 >= 0);
        CHECK(r.y1 <= 3);
        cells += static_cast<uint32_t>(r.y1 - r.y0 + 1);
    }
    CHECK(cells == 16);
    CHECK(res.runs.size() == 4);  // one run per column
}

TEST_CASE("block rules: a terrain sphere in damage mode: cracks first, removed at full damage; not known or not loaded stays") {
    float table[128];
    stone_table(3.0f, table);  // 3 hits: 85 bytes each
    TerrainDamage d;
    const Sphere s{2.25f, 0.25f, 0.25f, 0.3f};  // one cell: (4, 0, 0)
    auto r1 = terrain_sphere(s, table, false, d, 1000, world_at);
    CHECK(r1.damaged == 1);
    CHECK(r1.removed == 0);
    CHECK(r1.runs.empty());
    auto r2 = terrain_sphere(s, table, false, d, 2000, world_at);
    CHECK(r2.removed == 0);
    auto r3 = terrain_sphere(s, table, false, d, 3000, world_at);
    CHECK(r3.removed == 1);
    REQUIRE(r3.runs.size() == 1);
    CHECK((r3.runs[0].x == 4 && r3.runs[0].y0 == 0 && r3.runs[0].z == 0));
    CHECK(d.size() == 0);  // a removed cell is forgotten
    // healed between hits: starts again
    TerrainDamage h;
    terrain_sphere(s, table, false, h, 1000, world_at);
    terrain_sphere(s, table, false, h, 2000, world_at);
    h.heal(2001 + TerrainDamage::kHealMs);
    CHECK(terrain_sphere(s, table, false, h, 25000, world_at).removed == 0);
    // the shipped world not known, the place not loaded, a material the table leaves out: untouched
    auto unknown = [](int32_t i, int32_t j, int32_t k) {
        TerrainCellRead c = world_at(i, j, k);
        c.base_known = false;
        return c;
    };
    CHECK(terrain_sphere(s, table, true, d, 4000, unknown).damaged == 0);
    auto unloaded = [](int32_t i, int32_t j, int32_t k) {
        TerrainCellRead c = world_at(i, j, k);
        c.ok = false;
        return c;
    };
    CHECK(terrain_sphere(s, table, true, d, 4000, unloaded).damaged == 0);
    float none[128] = {};
    CHECK(terrain_sphere(s, none, true, d, 4000, world_at).damaged == 0);
}

TEST_CASE("block rules: shrink_off_world keeps a margin past the radius (the kernel tests in float)") {
    auto ruin = [](int32_t i, int32_t j, int32_t k) { return i == 22 && j == 40 && k == 60 ? 1 : 0; };  // 1 m away
    CHECK(shrink_off_world(10.25f, 20.25f, 30.25f, 0.995f, ruin) == doctest::Approx(0.99f));
    CHECK(shrink_off_world(10.25f, 20.25f, 30.25f, 0.98f, ruin) == doctest::Approx(0.98f));
}
