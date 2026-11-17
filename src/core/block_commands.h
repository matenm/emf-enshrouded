#pragma once
// Breaking blocks (API v44): the console commands `blocks` (what is where you look) and `break` (breaks the building
// blocks where you look). Admins only from the chat (no player flag); they need a world you host or the dedicated server.
#include <functional>
#include <string>

#include "core/commands.h"
#include "emf/emf_api.h"

namespace emf::core {

constexpr float kBlockLookRange = 12.0f;  // metres along the camera ray (the camera sits behind the player)

struct BlockCommandDeps {
    // The camera ray of the player the command means ("you": the console's host, a chat command's sender); false when
    // unknown. dir has unit length.
    std::function<bool(EmfWorld* w, float origin[3], float dir[3])> camera;
    // find_wall_ex (v47: flags EMF_FIND_TERRAIN also stops at terrain a player placed)
    std::function<bool(EmfWorld* w, const float from[3], const float to[3], uint32_t flags, EmfBlockInfo* out)> find_wall;
    std::function<bool(EmfWorld* w, float x, float y, float z, EmfBlockInfo* out)> block_at;
    std::function<uint32_t(EmfWorld* w, EmfBreakBlocks* req, EmfBreakCallback cb, void* user)> break_blocks;
    // `blocks origin`: the line for the cell at a point (block_origin_line), "" when nothing was read
    std::function<std::string(EmfWorld* w, float x, float y, float z)> origin;
};

bool add_block_commands(CommandCenter& commands, BlockCommandDeps deps);
// "building 7 (VeryHard, 1000 health, damage 34/255) at x y z" / "terrain 67 (VeryHard, 400 health), placed by a
// player at ..." / "nothing at ..."
std::string block_info_line(const EmfBlockInfo& b);

}  // namespace emf::core
