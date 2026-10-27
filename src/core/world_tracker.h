#pragma once
#include <windows.h>

#include <cstdint>

#include "emf/emf_api.h"

namespace emf::core {

// Remembers the most recent world pointer of each kind. A world that has not ticked for
// `stale_ms` is treated as gone (e.g. after leaving to the main menu).
class WorldTracker {
public:
    explicit WorldTracker(uint64_t stale_ms = 2000) : stale_ms_(stale_ms) {}
    void on_tick(EmfWorld* world, uint8_t kind, uint64_t now_ms);
    EmfWorld* latest(uint8_t kind, uint64_t now_ms) const;
    // Same with an explicit age limit. A tick stamped after now_ms (another thread read its clock later) is fresh.
    EmfWorld* latest_within(uint8_t kind, uint64_t now_ms, uint64_t max_age_ms) const;

private:
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    uint64_t stale_ms_;
    EmfWorld* world_[3] = {nullptr, nullptr, nullptr};
    uint64_t last_ms_[3] = {0, 0, 0};
};

}  // namespace emf::core
