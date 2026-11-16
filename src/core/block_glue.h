#pragma once
// Breaking blocks (API v44): the game end of the block service, run by EMF's internal server program "emf_blocks"
// (private research wall_damage_2026-11-15.md sections 7 and 13). The program declares the singletons the game's
// building programs and impact_simulation write (so the scheduler never runs it alongside them), checks each iterator
// slot against the world's own singleton, and calls the game's building-mining kernel with an operation record and
// EMF's table: damage mode = pass 0 (add damage) then pass 1 (remove what reached full damage), instant = one pass 1
// without the damage store. Every game call is guarded; a fault turns breaking off until a restart.
#include <cstdint>
#include <functional>
#include <string>

#include "core/block_service.h"
#include "core/system_programs.h"
#include "emf/emf_api.h"

namespace emf::core {

constexpr const char* kBlocksProgramName = "emf_blocks";
constexpr size_t kBlockOpBytes = 0x160;

// The game functions (Sig::BlockKernel, VoxelRegionRead, BuildingConfigFind, VoxelDamageGet), research 13.
struct BlockFns {
    // kernel(op, undo, stats, &{table, 256}, &centre (16-byte aligned, w = 0), radius, mode 0 | 1): false = it did not
    // run (the snap query failed or the box is too big)
    bool (*kernel)(void* op, void* undo, void* stats, const void* table, const float* centre, float radius,
                   uint8_t mode) = nullptr;
    bool (*region_read)(const void* slice, void* editor) = nullptr;  // false: part of the box is not loaded
    const uint8_t* (*config_find)(void* registry, uint8_t building_id) = nullptr;
    uint8_t (*damage_get)(void* store, const uint32_t cell[3]) = nullptr;
    // the shipped world's column reader (ruin protection; private research ruin_blocks_2026-11-16.md 4.1): acquire fills
    // a zeroed 0xA0-byte handle; seek and decode give one 8x8x8 brick; release frees the column's resource
    void (*column_acquire)(void* handle, void* editor, uint32_t level, uint64_t column) = nullptr;
    bool (*brick_seek)(void* handle_at_0x30, uint32_t by) = nullptr;
    void (*brick_decode)(uint8_t result[8], void* out_slice, void* in_slice) = nullptr;
    void (*column_release)(void* loader, uint32_t id) = nullptr;
};

// What one step's program call holds.
struct BlockHandles {
    void* program_context = nullptr;
    void* voxel_data = nullptr;  // *(VoxelWorld + 0)
    void* editor = nullptr;      // *(VoxelWorld + 8)
    void* damage_store = nullptr;     // ServerVoxelDamageSystem
    void* snapping = nullptr;         // ecs::ServerSnappingSystem
    void* efficiency = nullptr;       // TerraformingEfficiencyRegistry
    void* blueprint_items = nullptr;  // VoxelBlueprintItemRegistry
    void* balancing = nullptr;        // BalancingRegistry
};

// The kernel's operation record: only the fields its building path reads (research 7.1); instant: no damage store.
void fill_block_op(uint8_t op[kBlockOpBytes], const BlockHandles& h, bool instant);

struct BlockGlueDeps {
    BlockFns fns;
    // Runs fn after the server world's next step, on its thread, with that world.
    std::function<void(std::function<void(EmfWorld*)>)> after_step;
    std::function<void(const std::string&)> log;
    std::function<uint64_t()> now_ms;
    // count x the item with this GUID as loose pickups at pos (on the server world's thread, after the step); the
    // number dropped
    std::function<uint32_t(EmfWorld* world, const uint8_t item_guid[16], uint32_t count, const float pos[3])> drop;
};

// Sets the feature up (again: everything before is forgotten); the program to add to the server world. Breaking is
// on when the kernel and the config lookup are there.
ProgramSpec blocks_setup(BlockGlueDeps deps);
// Queues a break: its id, or 0 and *why (off, not available on this game version, the request's own check, 64 waiting).
uint32_t blocks_request(const EmfBreakBlocks& r, EmfBreakCallback cb, void* user, std::string* why);
// "" while breaking is on; else why not.
std::string blocks_unavailable_why();
// A new server world: what waits is answered CANCELLED (after the world's first step).
void blocks_world_reset();
// After every server step: requests waiting while emf_blocks has not run for kBlockWatchdogMs end CANCELLED.
void blocks_server_tick(uint64_t now_ms);
// Turns the feature off (its program could not be added, IterBegin is missing, or tests end); why: what
// blocks_unavailable_why() says from then on.
void blocks_stop(const std::string& why = "off (the emf_blocks program could not be added)");

// block_at / find_wall (API v44), on the server world's thread between steps (inside a world callback): the voxel at a
// point, the first building block on a line. False and *why when refused ("" with false: find_wall's line is clear).
bool blocks_block_at(EmfWorld* w, float x, float y, float z, EmfBlockInfo* out, std::string* why);
bool blocks_find_wall(EmfWorld* w, const float from[3], const float to[3], EmfBlockInfo* out, std::string* why);

// The `blocks origin` probe (ruin protection): one cell now, the shipped world's cell there, and the verdict; on the
// server world's thread between steps (inside a world callback). False and *why when refused or not loaded.
bool blocks_origin(EmfWorld* w, float x, float y, float z, BlockOrigin* out, std::string* why);
// Tests: the column reader as if its signatures were missing.
void blocks_base_reader_off_for_test(bool off);

// The material facts of every building id through the config lookup (false: the lookup is missing or faulted).
bool blocks_read_facts(void* efficiency_registry, MaterialFacts out[128]);

}  // namespace emf::core
