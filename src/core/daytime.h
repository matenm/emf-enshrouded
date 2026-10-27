#pragma once
// Time of day through the game's own setTimeOfDay (docs/engine_parts/world_building_daytime.md,
// re_time_spawn_weather.md). Same logic as the sample mod's time_skip, for the core's "time" command.
#include <cstddef>
#include <cstdint>

namespace emf::core {

using SetTimeOfDayFn = void (*)(void* daytime, int64_t tod_ns, int64_t ecs_now_ns);

constexpr int64_t kDayNs = 24ll * 3600ll * 1000000000ll;
constexpr size_t kDaytimeTimeOfDay = 0x48;   // g38_daytime::Daytime: i64 ns
constexpr size_t kWeatherMode = 0x08;        // WeatherSystem: u32, 0 = simulating (server)
constexpr size_t kWeatherLayers = 0xC8;      // layer array (stride 0x18, +0 = i64 nextPhenomenonTime)
constexpr size_t kWeatherLayerCount = 0xD0;  // u64
constexpr size_t kWeatherLayerStride = 0x18;
constexpr uint64_t kMaxWeatherLayers = 64;

struct TimeOfDayResult {
    bool ok = false;
    int64_t old_tod = 0;
    int64_t new_tod = 0;
    int layers_reset = 0;
};

// Sets the time of day to `hours` (0..24) and, with reset_weather, lets the server weather roll again (a write into the
// WeatherSystem's layer array: only on a known build, whose layout is checked). weather may be NULL.
TimeOfDayResult set_time_of_day(uint8_t* daytime, uint8_t* weather, SetTimeOfDayFn fn, int64_t ecs_now_ns, double hours,
                                bool reset_weather);

}  // namespace emf::core
