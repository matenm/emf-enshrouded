// Breaking blocks: reading voxels (voxel_read.h).
#include "core/voxel_read.h"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace emf::core {

EmfBlockInfo decode_voxel(uint16_t v, const MaterialFacts facts[128]) {
    EmfBlockInfo b{};
    b.size = sizeof b;
    b.damage = -1;
    const uint32_t m = v & 0xFFu, fill = v >> 8;
    b.fill = fill;
    // the game tells a building block by its material alone: blocks a player places read 0x00.. (high byte 0); the
    // high byte is a fill for terrain only
    if (!m || (m < 128 && !fill)) {
        b.kind = EMF_BLOCK_EMPTY;
        return b;
    }
    if (m >= 128) {
        b.kind = EMF_BLOCK_BUILDING;
        b.material = m - 128;
        const MaterialFacts& f = facts[b.material];
        if (f.known) {
            b.hardness = f.hardness;
            b.health = f.health;
        }
        return b;
    }
    b.kind = EMF_BLOCK_TERRAIN;
    b.material = m;
    return b;
}

std::optional<EmfBlockInfo> first_building_on_line(const float from[3], const float to[3], const VoxelReader& read,
                                                   const MaterialFacts facts[128]) {
    // in cell units (0.5 m)
    double p[3], d[3];
    for (int a = 0; a < 3; ++a) {
        p[a] = static_cast<double>(from[a]) * 2.0;
        d[a] = static_cast<double>(to[a]) * 2.0 - p[a];
    }
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const double max_cells = static_cast<double>(kFindWallMaxLength) * 2.0;
    if (len > max_cells)
        for (double& x : d) x *= max_cells / len;
    int64_t c[3], left[3];
    int step[3];
    double tmax[3], tdelta[3];
    constexpr double kInf = std::numeric_limits<double>::infinity();
    for (int a = 0; a < 3; ++a) {
        c[a] = static_cast<int64_t>(std::floor(p[a]));
        const auto end = static_cast<int64_t>(std::floor(p[a] + d[a]));
        left[a] = std::llabs(end - c[a]);
        if (d[a] == 0.0) {
            step[a] = 0;
            tmax[a] = tdelta[a] = kInf;
            continue;
        }
        step[a] = d[a] > 0 ? 1 : -1;
        const double boundary = d[a] > 0 ? static_cast<double>(c[a] + 1) : static_cast<double>(c[a]);
        tmax[a] = (boundary - p[a]) / d[a];
        tdelta[a] = 1.0 / std::fabs(d[a]);
    }
    for (;;) {
        uint16_t v = 0;
        if (!read(static_cast<int32_t>(c[0]), static_cast<int32_t>(c[1]), static_cast<int32_t>(c[2]), &v))
            return std::nullopt;
        EmfBlockInfo b = decode_voxel(v, facts);
        if (b.kind == EMF_BLOCK_BUILDING) {
            b.x = static_cast<float>((static_cast<double>(c[0]) + 0.5) / 2.0);
            b.y = static_cast<float>((static_cast<double>(c[1]) + 0.5) / 2.0);
            b.z = static_cast<float>((static_cast<double>(c[2]) + 0.5) / 2.0);
            return b;
        }
        int axis = -1;  // the next boundary along the line, among the axes with cells still to cross
        for (int a = 0; a < 3; ++a)
            if (left[a] > 0 && (axis < 0 || tmax[a] < tmax[axis])) axis = a;
        if (axis < 0) return std::nullopt;
        c[axis] += step[axis];
        tmax[axis] += tdelta[axis];
        --left[axis];
    }
}

}  // namespace emf::core
