#include "world_tracker.h"

namespace emf::core {

void WorldTracker::on_tick(EmfWorld* world, uint8_t kind, uint64_t now_ms) {
    if (kind > 2) return;
    AcquireSRWLockExclusive(&lock_);
    world_[kind] = world;
    last_ms_[kind] = now_ms;
    ReleaseSRWLockExclusive(&lock_);
}

EmfWorld* WorldTracker::latest(uint8_t kind, uint64_t now_ms) const { return latest_within(kind, now_ms, stale_ms_); }

EmfWorld* WorldTracker::latest_within(uint8_t kind, uint64_t now_ms, uint64_t max_age_ms) const {
    if (kind > 2) return nullptr;
    AcquireSRWLockShared(&lock_);
    EmfWorld* w = world_[kind];
    const uint64_t last = last_ms_[kind];
    ReleaseSRWLockShared(&lock_);
    const bool fresh = w && (now_ms <= last || now_ms - last <= max_age_ms);
    return fresh ? w : nullptr;
}

}  // namespace emf::core
