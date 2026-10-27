#include "daytime.h"

#include <windows.h>

#include <cmath>

#include "common/seh.h"

namespace emf::core {
namespace {

bool call_set(SetTimeOfDayFn fn, void* daytime, int64_t tod, int64_t now) {
    __try {
        fn(daytime, tod, now);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_zero(uint8_t* p) {
    __try {
        *reinterpret_cast<volatile int64_t*>(p) = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

TimeOfDayResult set_time_of_day(uint8_t* daytime, uint8_t* weather, SetTimeOfDayFn fn, int64_t ecs_now_ns, double hours,
                                bool reset_weather) {
    TimeOfDayResult r;
    if (!daytime || !fn || !(hours >= 0.0 && hours <= 24.0)) return r;
    seh_read(daytime + kDaytimeTimeOfDay, &r.old_tod, 8);
    r.new_tod = static_cast<int64_t>(std::llround(hours * 3600e9)) % kDayNs;
    if (!call_set(fn, daytime, r.new_tod, ecs_now_ns)) return r;
    r.ok = true;
    // setTimeOfDay restarts game time at day 0; weather layers waiting for a later time would stall.
    if (!weather || !reset_weather) return r;
    uint32_t mode = 0xFFFFFFFF;
    uint8_t* layers = nullptr;
    uint64_t count = 0;
    if (!seh_read(weather + kWeatherMode, &mode, 4) || !seh_read(weather + kWeatherLayers, &layers, 8) ||
        !seh_read(weather + kWeatherLayerCount, &count, 8))
        return r;
    if (mode != 0 || !layers || count > kMaxWeatherLayers) return r;
    for (uint64_t i = 0; i < count; ++i)
        if (write_zero(layers + i * kWeatherLayerStride)) ++r.layers_reset;
    return r;
}

}  // namespace emf::core
