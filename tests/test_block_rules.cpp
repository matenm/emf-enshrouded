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
    CHECK_FALSE(bad([](EmfBreakBlocks& r) { r.flags = 0x8; }).empty());
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
    CHECK(world_verdict(0xFF8A, true, 0x0000) == 0);   // placed on air
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
    r.flags = 0x8;
    CHECK(break_check(r) == "unknown flags");
}
