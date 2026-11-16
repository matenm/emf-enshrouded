// Breaking blocks: the rules (block_rules.h).
#include "core/block_rules.h"

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

std::string break_check(const EmfBreakBlocks& r) {
    if (r.size < kBreakBlocksV44Size) return "the request's size is too small (set size = sizeof(EmfBreakBlocks))";
    if (r.flags & ~(EMF_BREAK_INSTANT | EMF_BREAK_DROPS)) return "unknown flags";
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
    bool any = false;
    for (int i = 0; i < 5; ++i) any = any || r.damage[i] != 0;
    for (int i = 0; i < 5; ++i) s.damage[i] = any ? r.damage[i] : kDefaultBreakDamage[i];
    for (uint32_t i = 0; i < r.material_count && r.materials; ++i) s.materials.push_back(r.materials[i]);
    return s;
}

std::vector<DropStack> break_drops(const float removed[256], const MaterialFacts facts[128], float carry[128]) {
    std::vector<DropStack> out;
    for (uint32_t id = 0; id < 128; ++id) {
        const float got = removed[128 + id];
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

}  // namespace emf::core
