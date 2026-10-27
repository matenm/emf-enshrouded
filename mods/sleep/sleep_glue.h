#pragma once
// Sleep Through the Day's game side, kept apart from sleep.cpp so tests can run it over a fake API: who sleeps (as the
// game's own night skip counts), and the day clock read and moved.
//
// server_nighttime_skip (build 1273043: 0x140343fb0, disassembled) walks the 16 BasicPlayerInfos entries
// (0xA0 bytes, the player's entity id at +0); a player whose entity has an Actor counts, and sleeps when the Actor's
// state has flag 11 (0x800): Actor.currentState (+0xBF8), or while an action sequence plays (playbackContext.state
// bit 0) (currentState | setStateMask) & ~unsetStateMask (the game's "actor has state" at 0x14021aaa0). A "PlayerState
// bit 11" is this Actor flag (the NPC sleep state sets the same 0x800, docs/engine_parts/ai.md).
#include <cstdint>
#include <cstring>
#include <optional>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "sleep_logic.h"

namespace sleep {

constexpr int kPlayerSlots = 16;
constexpr size_t kPlayerInfoStride = 0xA0;
constexpr uint64_t kSleepFlag = 1ull << 11;

// g38_daytime::Daytime (0x70 bytes; offsets confirmed in build 1273043: isDaytime reads +0x48/+0x28/+0x30,
// setTimeScale writes +0x10/+0x18/+0x20 and bumps +0x24).
constexpr size_t kAnchorGameTime = 0x18;  // i64 ns
constexpr size_t kScale = 0x20;           // f32
constexpr size_t kChangeCounter = 0x24;   // u32: +1 on every re-anchor (the game sends the clock to players on a change)
constexpr size_t kDayStart = 0x28, kNightStart = 0x30, kDayDuration = 0x38, kNightDuration = 0x40, kTimeOfDay = 0x48;

namespace detail {
inline bool copy_guarded(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
        return false;
    }
}
}  // namespace detail

inline bool actor_sleeps(const emf::gen::ecs::Actor& a) {
    uint64_t state = a.currentState;
    const auto& p = a.currentAction.playbackContext;
    if (p.state & 1) state = (state | p.setStateMask) & ~p.unsetStateMask;
    return (state & kSleepFlag) != 0;
}

struct Count {
    int players = 0;
    int sleeping = 0;
};

inline Count count_sleepers(emf::World w) {
    Count n;
    const auto* infos = static_cast<const uint8_t*>(w.singleton("BasicPlayerInfos"));
    if (!infos) return n;
    for (int i = 0; i < kPlayerSlots; ++i) {
        uint32_t id = 0;
        if (!detail::copy_guarded(&id, infos + i * kPlayerInfoStride, 4) || !id) continue;
        auto* actor = w.get<emf::gen::ecs::Actor>(id);
        if (!actor) continue;
        ++n.players;
        if (actor_sleeps(*actor)) ++n.sleeping;
    }
    return n;
}

// sleep.pretend (host, for tests): every player counts as asleep, so the clock's path can be checked without a bed.
inline Count pretended(Count n, bool pretend) {
    if (pretend) n.sleeping = n.players;
    return n;
}

inline uint8_t* daytime_of(emf::World w) { return static_cast<uint8_t*>(w.singleton("g38_daytime::Daytime")); }

inline std::optional<Clock> read_clock(emf::World w) {
    const uint8_t* d = daytime_of(w);
    if (!d) return std::nullopt;
    Clock c;
    if (!detail::copy_guarded(&c.tod_ns, d + kTimeOfDay, 8) || !detail::copy_guarded(&c.day_start_ns, d + kDayStart, 8) ||
        !detail::copy_guarded(&c.night_start_ns, d + kNightStart, 8) ||
        !detail::copy_guarded(&c.day_dur_ns, d + kDayDuration, 8) ||
        !detail::copy_guarded(&c.night_dur_ns, d + kNightDuration, 8) || !detail::copy_guarded(&c.scale, d + kScale, 4))
        return std::nullopt;
    return c;
}

// Moves the clock's anchor game time forward by `ns`; with `bump`, also the change counter, as the game's own
// re-anchors do (so the players' clocks get the new time). Server world, between steps (on_tick), as the core's `time`
// command.
inline bool add_game_time(emf::World w, int64_t ns, bool bump = true) {
    uint8_t* d = daytime_of(w);
    if (!d || ns <= 0) return false;
    __try {
        auto* anchor = reinterpret_cast<volatile int64_t*>(d + kAnchorGameTime);
        auto* counter = reinterpret_cast<volatile uint32_t*>(d + kChangeCounter);
        *anchor = *anchor + ns;
        if (bump) *counter = *counter + 1;
        return true;
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
        return false;
    }
}

// The change counter goes up at most every 250 ms while the day races (if the game sends the clock to players on each
// change, that is 4 messages a second, not 60), and always on the step that reaches the evening.
class CounterPace {
public:
    static constexpr uint64_t kMs = 250;
    bool bump(uint64_t now_ms, bool force) {
        if (force || !last_ || now_ms - last_ >= kMs) {
            last_ = now_ms;
            return true;
        }
        return false;
    }

private:
    uint64_t last_ = 0;
};

}  // namespace sleep
