// Breaking blocks: the rules (block_rules.h).
#include "core/block_rules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/api_sizes.h"

namespace emf::core {

namespace {

bool allowed(const BreakSpec& s, uint32_t id) {
    if (s.materials.empty()) return true;
    for (uint8_t m : s.materials)
        if (m == id) return true;
    return false;
}

bool any_item(const uint8_t guid[16]) {
    for (int i = 0; i < 16; ++i)
        if (guid[i]) return true;
    return false;
}

std::string fmt(const char* f, double a, double b = 0) {
    char buf[120];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

}  // namespace

void build_break_table(const MaterialFacts facts[128], const BreakSpec& s, float mining_factor, float table[256]) {
    for (int m = 0; m < 256; ++m) table[m] = 0.0f;
    for (uint32_t id = 0; id < 128; ++id) {
        const MaterialFacts& f = facts[id];
        if (!f.known || f.hardness >= 5 || !allowed(s, id)) continue;
        if (s.instant) {
            table[128 + id] = 1.0f;
            continue;
        }
        if (f.health <= 0) continue;
        const float dmg = static_cast<float>(s.damage[f.hardness]) * mining_factor;
        const float hits = dmg > 0.0f ? static_cast<float>(f.health) / dmg : 0.0f;
        // the kernel adds floor(255 / hits) bytes a hit: past 255 hits a hit damages nothing (and would still count)
        if (hits > 0.0f && hits <= 255.0f) table[128 + id] = hits;
    }
}

std::vector<Sphere> hole_spheres(float x, float y, float z, float radius, float height) {
    std::vector<Sphere> out;
    if (height <= 0.0f) {
        out.push_back({x, y, z, radius});
        return out;
    }
    const int n = 1 + static_cast<int>(std::ceil(height / radius - 1e-4f));
    for (int i = 0; i < n; ++i) out.push_back({x, y + height * static_cast<float>(i) / static_cast<float>(n - 1), z, radius});
    return out;
}

int world_verdict(uint16_t cur, bool base_known, uint16_t base) {
    const uint8_t m = static_cast<uint8_t>(cur & 0xFF);
    if (m < 0x80) return 0;
    if (!base_known) return -1;
    return static_cast<uint8_t>(base & 0xFF) == m ? 1 : 0;
}

float shrink_off_world(float cx, float cy, float cz, float r,
                       const std::function<int(int32_t, int32_t, int32_t)>& world_cell) {
    const double c[3] = {cx, cy, cz};
    const double reach = r + kShrinkMargin;  // the kernel tests in float: a cell just past r may still pass it
    int32_t lo[3], hi[3];
    for (int a = 0; a < 3; ++a) {
        lo[a] = static_cast<int32_t>(std::floor((c[a] - reach) * 2.0));
        hi[a] = static_cast<int32_t>(std::floor((c[a] + reach) * 2.0));
    }
    double nearest = reach + 1.0;  // none within reach
    for (int32_t k = lo[2]; k <= hi[2]; ++k)
        for (int32_t j = lo[1]; j <= hi[1]; ++j)
            for (int32_t i = lo[0]; i <= hi[0]; ++i) {
                const double dx = (i + 0.5) / 2.0 - c[0], dy = (j + 0.5) / 2.0 - c[1], dz = (k + 0.5) / 2.0 - c[2];
                const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d > reach || d >= nearest) continue;
                if (world_cell(i, j, k) != 0) nearest = d;  // the world's own, or unknown
            }
    if (nearest > reach) return r;
    const double left = std::min(static_cast<double>(r), nearest - kShrinkMargin);
    return left < kBlockMinRadius ? 0.0f : static_cast<float>(left);
}

std::string block_origin_line(const BlockOrigin& o) {
    char b[200];
    char shipped[48];
    if (o.base_known)
        std::snprintf(shipped, sizeof shipped, "0x%04x", o.base);
    else
        std::snprintf(shipped, sizeof shipped, "not known (its column is not loaded)");
    const char* verdict = o.world == 1    ? "the world's own (a ruin: never broken)"
                          : o.world == -1 ? "not known: counts as the world's own (never broken)"
                          : (o.cur & 0xFF) >= 0x80 ? "a player's"
                                                   : "no building block";
    std::snprintf(b, sizeof b, "origin at %.2f %.2f %.2f: now 0x%04x, shipped %s: %s", o.x, o.y, o.z, o.cur, shipped,
                  verdict);
    return b;
}

std::string break_check(const EmfBreakBlocks& r) {
    if (r.size < kBreakBlocksV44Size) return "the request's size is too small (set size = sizeof(EmfBreakBlocks))";
    if (r.flags & ~(EMF_BREAK_INSTANT | EMF_BREAK_DROPS | EMF_BREAK_WORLD | EMF_BREAK_TERRAIN)) return "unknown flags";
    if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.z)) return "the point is not a number";
    if (!std::isfinite(r.radius) || !std::isfinite(r.height)) return "radius or height is not a number";
    const float radius = request_radius(r);
    if (radius < kBlockMinRadius || radius > kBlockMaxRadius)
        return fmt("radius %.2f m: it must be 0.25 to 3.5 m (0 = 1 m)", radius);
    if (r.height < 0.0f || r.height > kBlockMaxHeight) return fmt("height %.2f m: it must be 0 to 4 m", r.height);
    if (hole_spheres(r.x, r.y, r.z, radius, r.height).size() > kBlockMaxSpheres)
        return "at most 8 spheres: height / radius at most 7";
    for (int i = 0; i < 5; ++i)
        if (r.damage[i] < 0 || r.damage[i] > kBlockMaxDamage) return "damage must be 0 to 100000";
    if (r.material_count > kBlockMaxMaterials) return "at most 128 materials";
    if (r.material_count && !r.materials) return "material_count without a materials list";
    for (uint32_t i = 0; i < r.material_count; ++i)
        if (r.materials[i] > 127) return fmt("material %.0f: building materials are 0 to 127", r.materials[i]);
    return "";
}

float request_radius(const EmfBreakBlocks& r) { return r.radius == 0.0f ? 1.0f : r.radius; }

BreakSpec break_spec(const EmfBreakBlocks& r) {
    BreakSpec s;
    s.instant = (r.flags & EMF_BREAK_INSTANT) != 0;
    s.world = (r.flags & EMF_BREAK_WORLD) != 0;
    s.terrain = (r.flags & EMF_BREAK_TERRAIN) != 0;
    bool any = false;
    for (int i = 0; i < 5; ++i) any = any || r.damage[i] != 0;
    for (int i = 0; i < 5; ++i) s.damage[i] = any ? r.damage[i] : kDefaultBreakDamage[i];
    for (uint32_t i = 0; i < r.material_count && r.materials; ++i) s.materials.push_back(r.materials[i]);
    return s;
}

namespace {
std::vector<DropStack> drops_of(const float removed[128], const MaterialFacts facts[128], float carry[128]);
}

std::vector<DropStack> break_drops(const float removed[256], const MaterialFacts facts[128], float carry[128]) {
    return drops_of(removed + 128, facts, carry);
}

std::vector<DropStack> terrain_drops(const float removed[256], const MaterialFacts terrain_facts[128], float carry[128]) {
    return drops_of(removed, terrain_facts, carry);
}

namespace {
std::vector<DropStack> drops_of(const float removed[128], const MaterialFacts facts[128], float carry[128]) {
    std::vector<DropStack> out;
    for (uint32_t id = 0; id < 128; ++id) {
        const float got = removed[id];
        if (got <= 0.0f) continue;
        const MaterialFacts& f = facts[id];
        if (!f.known || !f.loot_rate || !any_item(f.item_guid)) continue;
        const float total = got + carry[id];
        const auto count = static_cast<uint32_t>(std::floor(total / static_cast<float>(f.loot_rate) + 1e-4f));
        carry[id] = total - static_cast<float>(count) * static_cast<float>(f.loot_rate);
        if (carry[id] < 0.0f) carry[id] = 0.0f;
        if (count) out.push_back({id, count});
    }
    return out;
}
}  // namespace

// ---- terrain a player placed (API v47) ----

int terrain_verdict(uint16_t cur, bool base_known, uint16_t base) {
    const uint32_t m = cur & 0xFFu;
    if (m == 0 || m >= 0x80 || (cur >> 8) == 0) return 0;  // no terrain now
    if (!base_known) return -1;
    const uint32_t bm = base & 0xFFu;
    const bool ground = bm != 0 && bm < 0x80 && (base >> 8) != 0;
    return ground ? 1 : 0;
}

void build_terrain_table(const MaterialFacts facts[128], const BreakSpec& s, float mining_factor, float table[128]) {
    for (int m = 0; m < 128; ++m) table[m] = 0.0f;
    for (uint32_t m = 1; m < 128; ++m) {
        const MaterialFacts& f = facts[m];
        if (!f.known || f.hardness >= 5) continue;
        if (s.instant) {
            table[m] = 1.0f;
            continue;
        }
        if (f.health <= 0) continue;
        const float dmg = static_cast<float>(s.damage[f.hardness]) * mining_factor;
        const float hits = dmg > 0.0f ? static_cast<float>(f.health) / dmg : 0.0f;
        if (hits > 0.0f && hits <= 255.0f) table[m] = hits;  // past 255 hits a hit would add no damage (as the kernel)
    }
}

uint8_t TerrainDamage::add(int32_t x, int32_t y, int32_t z, uint32_t bytes, uint64_t now_ms) {
    Entry& e = cells_[{x, y, z}];
    const uint32_t v = e.damage + bytes;
    e.damage = static_cast<uint8_t>(v > 255 ? 255 : v);
    e.last_ms = now_ms;
    return e.damage;
}

void TerrainDamage::forget(int32_t x, int32_t y, int32_t z) { cells_.erase({x, y, z}); }

void TerrainDamage::heal(uint64_t now_ms) {
    for (auto it = cells_.begin(); it != cells_.end();)
        it = now_ms - it->second.last_ms > kHealMs ? cells_.erase(it) : std::next(it);
}

std::vector<TerrainRun> column_runs(std::vector<TerrainCell> cells) {
    std::sort(cells.begin(), cells.end(), [](const TerrainCell& a, const TerrainCell& b) {
        return std::tie(a.x, a.z, a.y) < std::tie(b.x, b.z, b.y);
    });
    std::vector<TerrainRun> runs;
    for (const TerrainCell& c : cells) {
        if (!runs.empty()) {
            TerrainRun& r = runs.back();
            if (r.x == c.x && r.z == c.z && r.material == c.material && r.y1 + 1 == c.y) {
                r.y1 = c.y;
                continue;
            }
        }
        runs.push_back({c.x, c.z, c.y, c.y, c.material});
    }
    return runs;
}

TerrainPassResult terrain_sphere(const Sphere& s, const float table[128], bool instant, TerrainDamage& damage,
                                 uint64_t now_ms, const std::function<TerrainCellRead(int32_t, int32_t, int32_t)>& read) {
    TerrainPassResult res;
    if (!(s.r > 1e-20f)) return res;
    const double c[3] = {s.x, s.y, s.z};
    int32_t lo[3], hi[3];
    for (int a = 0; a < 3; ++a) {
        lo[a] = static_cast<int32_t>(std::floor((c[a] - s.r) * 2.0));
        hi[a] = static_cast<int32_t>(std::floor((c[a] + s.r) * 2.0));
    }
    const double r2 = static_cast<double>(s.r) * s.r;
    std::vector<TerrainCell> gone;
    for (int32_t k = lo[2]; k <= hi[2]; ++k)
        for (int32_t j = lo[1]; j <= hi[1]; ++j)
            for (int32_t i = lo[0]; i <= hi[0]; ++i) {
                const double dx = (i + 0.5) / 2.0 - c[0], dy = (j + 0.5) / 2.0 - c[1], dz = (k + 0.5) / 2.0 - c[2];
                if (dx * dx + dy * dy + dz * dz > r2) continue;
                const TerrainCellRead cell = read(i, j, k);
                if (!cell.ok) continue;
                const uint8_t m = static_cast<uint8_t>(cell.cur & 0xFF);
                if (m == 0 || m >= 0x80 || (cell.cur >> 8) == 0) continue;  // no terrain
                if (terrain_verdict(cell.cur, cell.base_known, cell.base) != 0) continue;  // the ground, or not known
                const float hits = table[m];
                if (!(hits > 0.0f)) continue;
                ++res.damaged;
                bool removed = instant;
                if (!instant) {
                    const uint32_t bytes = hits <= 1.0f ? 255u : static_cast<uint32_t>(255.0f / hits);
                    removed = damage.add(i, j, k, bytes, now_ms) >= 255;
                }
                if (!removed) continue;
                damage.forget(i, j, k);
                ++res.removed;
                res.removed_per_material[m] += 1.0f;
                gone.push_back({i, j, k, m});
            }
    res.runs = column_runs(std::move(gone));
    return res;
}

}  // namespace emf::core
