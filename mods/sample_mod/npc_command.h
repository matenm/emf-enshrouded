#pragma once
// Villager "come here" for the sample mod: replays the engine's NpcCommandEvent GoToPosition handler
// (0x14044f040, type 1) on the villager worker context `ctx` (see docs/engine_parts/ai.md).
#include <atomic>
#include <cstdint>
#include <cstring>

namespace sample {

using NpcSetStateFn = void (*)(void* ctx, uint8_t state);
using MarkWrittenFn = void (*)(void* tracker, uint64_t a, uint64_t b, uint64_t c, uint32_t type_idx_u16, uint8_t variant);

// One write-component entry of the villager worker context (0x30 bytes).
struct WriteEntry {
    void* data;
    uint64_t a, b, c;  // passed through to markWritten
    uint16_t type_idx;
    uint8_t variant;
    uint8_t pad[5];
    void* tracker;     // NULL when change tracking is off
};
static_assert(sizeof(WriteEntry) == 0x30, "engine layout");

constexpr uint32_t kDynamicActiveNpcStateEntry = 0x1b0;
constexpr uint32_t kNpcCommonEntry = 0x300;
constexpr uint32_t kNpcCommonTargetPosition = 0x64;  // float3, meters
constexpr uint8_t kStateStart = 1;
constexpr uint8_t kStateRoam = 7;
constexpr uint8_t kDecisionWantsToRoam = 1;

inline void mark_entry(const WriteEntry& e, MarkWrittenFn mark) {
    if (e.tracker) mark(e.tracker, e.a, e.b, e.c, e.type_idx, e.variant);
}

// SetState(Roam), decision = WantsToRoam, NpcCommon.currentTargetPosition = target, marking both
// components written like the engine does. False (nothing changed) for a villager in state Start
// (the engine ignores commands then) or when a pointer is missing.
inline bool apply_go_to_position(uint8_t* ctx, const double target_m[3], NpcSetStateFn set_state, MarkWrittenFn mark) {
    if (!ctx || !target_m || !set_state || !mark) return false;
    WriteEntry state_entry, common_entry;
    std::memcpy(&state_entry, ctx + kDynamicActiveNpcStateEntry, sizeof(WriteEntry));
    std::memcpy(&common_entry, ctx + kNpcCommonEntry, sizeof(WriteEntry));
    auto* state = static_cast<uint8_t*>(state_entry.data);
    auto* common = static_cast<uint8_t*>(common_entry.data);
    if (!state || !common || state[0] == kStateStart) return false;
    set_state(ctx, kStateRoam);
    mark_entry(state_entry, mark);
    state[2] = kDecisionWantsToRoam;
    mark_entry(common_entry, mark);
    const float target[3] = {static_cast<float>(target_m[0]), static_cast<float>(target_m[1]),
                             static_cast<float>(target_m[2])};
    std::memcpy(common + kNpcCommonTargetPosition, target, sizeof(target));
    return true;
}

// A command posted by the server tick for one villager, taken by that villager's worker (any thread).
class PendingCommand {
public:
    static constexpr uint64_t kLifetimeMs = 5000;

    void post(uint32_t entity, const double target_m[3], uint64_t now_ms) {
        entity_.store(0, std::memory_order_relaxed);
        std::memcpy(target_, target_m, sizeof(target_));
        expires_ms_.store(now_ms + kLifetimeMs, std::memory_order_relaxed);
        entity_.store(entity, std::memory_order_release);
    }
    // True exactly once, for the posted entity, before the command expires; copies the target.
    bool take_if(uint32_t entity, uint64_t now_ms, double target_out[3]) {
        uint32_t expected = entity_.load(std::memory_order_acquire);
        if (!entity || expected != entity) return false;
        if (now_ms > expires_ms_.load(std::memory_order_relaxed)) {
            entity_.compare_exchange_strong(expected, 0);
            return false;
        }
        double t[3];
        std::memcpy(t, target_, sizeof(t));
        if (!entity_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) return false;
        std::memcpy(target_out, t, sizeof(t));
        return true;
    }
    bool armed() const { return entity_.load(std::memory_order_acquire) != 0; }
    // Disarms a command nobody took before its lifetime ended (call from the posting thread).
    void expire(uint64_t now_ms) {
        uint32_t current = entity_.load(std::memory_order_acquire);
        if (current && now_ms > expires_ms_.load(std::memory_order_relaxed))
            entity_.compare_exchange_strong(current, 0);
    }

private:
    std::atomic<uint32_t> entity_{0};
    std::atomic<uint64_t> expires_ms_{0};
    double target_[3] = {};
};

}  // namespace sample
