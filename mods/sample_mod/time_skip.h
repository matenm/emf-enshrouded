#pragma once
// Step B write test: move the time of day forward through the game's own setTimeOfDay
// (the function the admin command and save-load use), then let the weather re-roll.
// Offsets: docs/engine_parts/world_building_daytime.md, re_time_spawn_weather.md.
#include <cstdint>
#include <cstring>

namespace sample {

using SetTimeOfDayFn = void (*)(void* daytime, int64_t tod_ns, int64_t ecs_now_ns);

constexpr int64_t kDayNs = 24ll * 3600ll * 1000000000ll;
constexpr size_t kDaytimeTimeOfDay = 0x48;  // i64 ns
constexpr size_t kWeatherMode = 0x08;       // u32: 0 = simulating (server), 1 = render side (client)
constexpr size_t kWeatherLayers = 0xC8;     // layer array
constexpr size_t kWeatherLayerCount = 0xD0; // u64
constexpr size_t kLayerStride = 0x18;       // layer +0x00 = i64 nextPhenomenonTime
constexpr uint64_t kMaxLayers = 64;

struct TimeSkipResult {
    int64_t old_tod = 0;
    int64_t new_tod = 0;
    int layers_reset = 0;
};

inline TimeSkipResult time_skip(uint8_t* daytime, uint8_t* weather, SetTimeOfDayFn set_tod, int64_t ecs_now,
                                int64_t add_ns) {
    TimeSkipResult r;
    std::memcpy(&r.old_tod, daytime + kDaytimeTimeOfDay, 8);
    r.new_tod = ((r.old_tod + add_ns) % kDayNs + kDayNs) % kDayNs;
    set_tod(daytime, r.new_tod, ecs_now);
    // setTimeOfDay restarts game time at day 0; layers waiting for a later time would stall.
    // Zeroing nextPhenomenonTime makes the next weather tick (same frame) roll new weather.
    if (!weather) return r;
    uint32_t mode = 0xFFFFFFFF;
    uint8_t* layers = nullptr;
    uint64_t count = 0;
    std::memcpy(&mode, weather + kWeatherMode, 4);
    std::memcpy(&layers, weather + kWeatherLayers, 8);
    std::memcpy(&count, weather + kWeatherLayerCount, 8);
    if (mode != 0 || !layers || count > kMaxLayers) return r;
    for (uint64_t i = 0; i < count; ++i) {
        const int64_t zero = 0;
        std::memcpy(layers + i * kLayerStride, &zero, 8);
        ++r.layers_reset;
    }
    return r;
}

}  // namespace sample
