#pragma once
// Breaking blocks (API v44, private research wall_damage_2026-11-15.md sections 2.4 and 13): the rules before the game
// is called. EMF hands the game's building-mining kernel its own table of hits per voxel material: entries for
// terrain (materials 0..127) are always 0, so the kernel can never dig the ground; the game's Unbreakable materials
// and materials without a config stay 0 too.
#include <cstdint>
#include <string>
#include <vector>

#include "emf/emf_api.h"

namespace emf::core {

// What EMF knows of one building material (the game's BuildingTerraformingConfig), by building id 0..127 (the voxel's
// material byte minus 128).
struct MaterialFacts {
    bool known = false;      // the game has a config for it
    uint8_t hardness = 0;    // 0 Soft .. 4 VeryHard, 5 Unbreakable
    int32_t health = 0;      // healthPoints
    uint32_t loot_rate = 0;  // blocksPerLootItemExchangeRate: removed blocks per item a pickaxe gives
    uint8_t item_guid[16] = {};  // materialItem: the item it gives (all zero: none)
};

constexpr int32_t kDefaultBreakDamage[5] = EMF_BREAK_DEFAULT_DAMAGE;
constexpr float kBlockMinRadius = 0.25f, kBlockMaxRadius = 3.5f, kBlockMaxHeight = 4.0f;
constexpr size_t kBlockMaxSpheres = 8;
constexpr int32_t kBlockMaxDamage = 100000;
constexpr uint32_t kBlockMaxMaterials = 128;

struct BreakSpec {
    bool instant = false;
    int32_t damage[5] = {};          // per hardness class (damage mode)
    std::vector<uint8_t> materials;  // building ids; empty = every one that can break
};

// The kernel's table: hits to break per voxel material (0 = untouched). Damage mode: health / (damage[hardness] x
// mining_factor), left 0 past 255 hits (a hit would add no damage byte); break mode: 1 for each material that can break.
void build_break_table(const MaterialFacts facts[128], const BreakSpec& s, float mining_factor, float table[256]);

struct Sphere {
    float x = 0, y = 0, z = 0, r = 0;
};
// A column of spheres of `radius` from (x, y, z) up to `height` above it, at most `radius` apart: 1 + ceil(height /
// radius) of them (1 for height 0), evenly spaced.
std::vector<Sphere> hole_spheres(float x, float y, float z, float radius, float height);

// "" when the request can be queued, else why not (the text a mod sees in EmfBreakBlocks.why).
std::string break_check(const EmfBreakBlocks& r);
// The request's radius (0 = 1 m) and spec (all-zero damage = kDefaultBreakDamage). Only for a request break_check passed.
float request_radius(const EmfBreakBlocks& r);
BreakSpec break_spec(const EmfBreakBlocks& r);

struct DropStack {
    uint32_t building_id = 0;
    uint32_t count = 0;
};
// The items the removed blocks give: per building material, (removed + carry) / loot_rate items, the rest kept in
// carry for the next break (the game keeps such a remainder per player for its pickaxes). removed: per voxel material.
std::vector<DropStack> break_drops(const float removed[256], const MaterialFacts facts[128], float carry[128]);

}  // namespace emf::core
