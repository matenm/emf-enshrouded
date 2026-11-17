#pragma once
// Breaking blocks (API v44, private research wall_damage_2026-11-15.md sections 2.4 and 13): the rules before the game
// is called. EMF hands the game's building-mining kernel its own table of hits per voxel material: entries for
// terrain (materials 0..127) are always 0, so the kernel can never dig the ground; the game's Unbreakable materials
// and materials without a config stay 0 too.
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <tuple>
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
    bool world = false;              // EMF_BREAK_WORLD: the world's own blocks too (else the spheres stop short of them)
    bool terrain = false;            // EMF_BREAK_TERRAIN (v47): the terrain a player placed too
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
// Whose a voxel is (ruin protection): the game keeps the shipped world (its ruins in it) as a base layer under the
// saved changes, so a building cell whose material byte equals the base cell's is the world's own: 1. One that differs
// (built on air or terrain, another material) is a player's: 0. Base not known (not loaded, not readable): -1, which
// breaking treats as the world's own. A cell that is no building block (empty, terrain): 0. Only the low byte counts:
// decay and overgrowth paint change the high one.
int world_verdict(uint16_t cur, bool base_known, uint16_t base);

// Ruin protection for one break sphere: the game's kernel breaks every cell whose centre is within the radius and
// decides by material, not by cell, so EMF shrinks the radius instead: to just short (0.01 m) of the nearest cell that
// world_cell says is the world's own (1) or unknown (-1), among the cells within r. The radius to use; 0 when less
// than kBlockMinRadius is left (the sphere is skipped). world_cell(i, j, k): cell (i, j, k) of 0.5 m, its centre at
// ((i + 0.5) / 2, ...); 0 for a player's block, terrain and empty cells.
// Cells within r + kShrinkMargin are looked at (the kernel's float test may take a cell a hair past r).
constexpr double kShrinkMargin = 0.01;
float shrink_off_world(float cx, float cy, float cz, float r, const std::function<int(int32_t, int32_t, int32_t)>& world_cell);

// One cell read for the `blocks origin` probe: what it is now, what the shipped world has there, and the verdict.
struct BlockOrigin {
    uint16_t cur = 0;
    bool base_known = false;
    uint16_t base = 0;
    int world = 0;              // world_verdict
    float x = 0, y = 0, z = 0;  // the cell's centre, metres
};
// "origin at x y z: now 0xff87, shipped 0xff87: the world's own (a ruin: never broken)"
std::string block_origin_line(const BlockOrigin& o);

// The items the removed blocks give: per building material, (removed + carry) / loot_rate items, the rest kept in
// carry for the next break (the game keeps such a remainder per player for its pickaxes). removed: per voxel material.
std::vector<DropStack> break_drops(const float removed[256], const MaterialFacts facts[128], float carry[128]);

// ---- API v47: terrain a player placed (EMF_BREAK_TERRAIN, find_wall's EMF_FIND_TERRAIN) ----
// The game's terrain dig reduces fill in a sphere and would eat the natural ground around a placed wall, so EMF judges
// each cell and clears a player's cells itself with the game's box fill (the call its building kernel removes a block
// with). Terrain facts come from the game's TerrainTerraformingConfig (the building config's offsets), by terrain
// material 1..127; the game keeps no damage for terrain, so EMF keeps it (TerrainDamage).

// Whose a terrain cell is: the shipped world had ground there (terrain with fill): the world's own, 1. No ground there
// (empty, a terrain material without fill, a building cell a player replaced): a player placed it, 0. The shipped
// world not known there: -1, never broken. A cell that is no terrain now (empty, building): 0.
int terrain_verdict(uint16_t cur, bool base_known, uint16_t base);

// Hits to break per terrain material (index = material 1..127; 0 = untouched), as build_break_table: health /
// (damage[hardness] x mining_factor), 0 past 255 hits; instant: 1. Unbreakable never. A building materials list does not
// limit terrain.
void build_terrain_table(const MaterialFacts facts[128], const BreakSpec& s, float mining_factor, float table[128]);

// EMF's damage for terrain cells (0..255, removed at 255), healing like the game's building damage: a cell is forgotten
// kHealMs after its last hit.
class TerrainDamage {
public:
    static constexpr uint64_t kHealMs = 20000;
    uint8_t add(int32_t x, int32_t y, int32_t z, uint32_t bytes, uint64_t now_ms);  // the new value, at most 255
    void forget(int32_t x, int32_t y, int32_t z);
    void heal(uint64_t now_ms);
    void clear() { cells_.clear(); }
    size_t size() const { return cells_.size(); }

private:
    struct Entry {
        uint8_t damage = 0;
        uint64_t last_ms = 0;
    };
    std::map<std::tuple<int32_t, int32_t, int32_t>, Entry> cells_;
};

struct TerrainCell {
    int32_t x = 0, y = 0, z = 0;
    uint8_t material = 0;
};
// A column of removed cells, cleared with one box fill: cells (x, y0..y1, z), all of one material.
struct TerrainRun {
    int32_t x = 0, z = 0, y0 = 0, y1 = 0;
    uint8_t material = 0;
};
// Cells with the same x and z, consecutive y and one material become one run; ordered by x, z, y.
std::vector<TerrainRun> column_runs(std::vector<TerrainCell> cells);

// One cell as the glue reads it: ok false = not loaded (left).
struct TerrainCellRead {
    bool ok = false;
    uint16_t cur = 0;
    bool base_known = false;
    uint16_t base = 0;
};
struct TerrainPassResult {
    uint32_t damaged = 0, removed = 0;  // cells hit (the removed ones too), cells removed
    std::vector<TerrainRun> runs;       // what to clear
    float removed_per_material[128] = {};
};
// One sphere: every cell whose centre is within the radius (the building kernel's test) that is terrain a player placed
// (terrain_verdict 0) and whose table entry is not 0 takes a hit, floor(255 / hits) bytes (instant: removed at once); a
// cell at 255 is removed (and forgotten by the damage store). Nothing is changed here: the caller clears the runs.
TerrainPassResult terrain_sphere(const Sphere& s, const float table[128], bool instant, TerrainDamage& damage,
                                 uint64_t now_ms, const std::function<TerrainCellRead(int32_t, int32_t, int32_t)>& read);

}  // namespace emf::core
