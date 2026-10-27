#pragma once
// clock: the game's time of day (g38_daytime::Daytime, i64 ns since midnight) as text. No game here (unit-tested).
#include <cstdint>
#include <cstdio>
#include <string>

#include "emf/emf_api.h"

namespace clockmod {

constexpr int64_t kDayNs = 24ll * 3600ll * 1000000000ll;

// "14:05" (24 h) or "2:05 PM" (12 h); "" when the value is not a time of day.
inline std::string text(int64_t tod_ns, bool h24) {
    if (tod_ns < 0 || tod_ns >= kDayNs) return {};
    const int64_t minutes = tod_ns / (60ll * 1000000000ll);
    const int h = static_cast<int>(minutes / 60), m = static_cast<int>(minutes % 60);
    char b[16];
    if (h24)
        snprintf(b, sizeof(b), "%02d:%02d", h, m);
    else
        snprintf(b, sizeof(b), "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
    return b;
}

// The "place" setting (0 top right, 1 bottom right, 2 bottom left) as a HUD place; anything else: bottom right.
inline uint32_t place_from(int64_t setting) {
    return setting >= 0 && setting <= EMF_HUD_BOTTOM_LEFT ? static_cast<uint32_t>(setting) : EMF_HUD_BOTTOM_RIGHT;
}

}  // namespace clockmod
