#pragma once
// Breaking blocks (API v44): what a voxel is, and the first building block on a line (research section 1: a voxel is
// u16, low byte material, high byte fill; 0.5 m cells at floor(metres x 2); materials 128..255 are building blocks).
#include <cstdint>
#include <functional>
#include <optional>

#include "core/block_rules.h"
#include "emf/emf_api.h"

namespace emf::core {

constexpr float kFindWallMaxLength = 64.0f;  // metres of a line find_wall walks at most

// Reads the voxel of one cell; false: that place is not loaded (or the read failed).
using VoxelReader = std::function<bool(int32_t vx, int32_t vy, int32_t vz, uint16_t* voxel)>;

// The voxel as a mod sees it (size set; x, y, z and damage left for the caller: damage -1): no material = empty;
// 128..255 = building whatever the high byte (blocks a player places read 0x00..; material = id 0..127, hardness and
// health from facts when known); 1..127 = terrain when it has fill, else empty.
EmfBlockInfo decode_voxel(uint16_t v, const MaterialFacts facts[128]);

// The first building cell on the line from `from` to `to` (metres; at most kFindWallMaxLength of it), visiting every
// cell the line crosses in order (terrain is passed over), with the cell's centre in x, y, z. Nothing when the line is
// clear or a place on it is not loaded.
std::optional<EmfBlockInfo> first_building_on_line(const float from[3], const float to[3], const VoxelReader& read,
                                                   const MaterialFacts facts[128]);

}  // namespace emf::core
