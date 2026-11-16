#pragma once
// Breaking blocks (API v44): mods' break requests, queued and run by EMF's internal server program emf_blocks at the
// next step (block_glue.cpp gives it the game's calls; tests give fakes). At most kBlockMaxSpheres kernel calls a step,
// so a burst of breaks never changes more than the game's ring of changed boxes holds (64) in one step.
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/block_rules.h"
#include "emf/emf_api.h"

namespace emf::core {

constexpr size_t kBlockRequestsMax = 64;
constexpr uint64_t kBlockWatchdogMs = 3000;  // requests waiting and emf_blocks has not run this long: cancelled

// What one kernel call (both passes for a sphere) did; added to by each call.
struct KernelCount {
    uint32_t damaged = 0, removed = 0;
    float removed_per_material[256] = {};  // by voxel material
};

// The game's side, called only from inside emf_blocks on the server world.
struct BlockEngine {
    std::function<bool(MaterialFacts out[128])> facts;  // false: the materials cannot be read
    std::function<float()> mining_factor;               // the world's mining damage setting (1 = default)
    // Damage (or with instant break) the building blocks in one sphere; false: the call faulted.
    std::function<bool(const Sphere& s, const float table[256], bool instant, KernelCount* out)> kernel;
    // count x the item with this GUID as loose pickups at pos; the number dropped.
    std::function<uint32_t(const uint8_t item_guid[16], uint32_t count, const float pos[3])> drop;
    // Ruin protection: how far a sphere may reach (shrink_off_world over the world as it is); 0 skips it. Not asked
    // for a request with EMF_BREAK_WORLD. Unset: the full radius.
    std::function<float(const Sphere& s)> shrink;
};

// A finished request, for its callback (after the step).
struct BlockDone {
    uint32_t id = 0;
    EmfBreakCallback cb = nullptr;
    void* user = nullptr;
    EmfBreakResult result{};
};

class BlockService {
public:
    // Checks and queues a request: its id (> 0), or 0 and *why. Any thread.
    uint32_t request(const EmfBreakBlocks& r, EmfBreakCallback cb, void* user, std::string* why);
    // Inside emf_blocks: runs waiting requests in order while their spheres fit in this step's kBlockMaxSpheres.
    std::vector<BlockDone> step(const BlockEngine& e);
    // A new world (or the watchdog): every waiting request ends CANCELLED; the drop remainders are forgotten.
    std::vector<BlockDone> world_reset();
    // The watchdog alone: waiting requests end CANCELLED, the remainders stay (the same world).
    std::vector<BlockDone> cancel_all();
    size_t waiting() const;

private:
    struct Request {
        uint32_t id = 0;
        BreakSpec spec;
        std::vector<Sphere> spheres;
        bool drops = false;
        float mid[3] = {};  // where drops land: the column's middle
        EmfBreakCallback cb = nullptr;
        void* user = nullptr;
    };
    static BlockDone answer(const Request& r, uint32_t status, const KernelCount* k = nullptr, uint32_t dropped = 0);

    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<Request> waiting_;
    uint32_t next_id_ = 1;
    float carry_[128] = {};  // drop remainders (only touched inside the program)
};

}  // namespace emf::core
