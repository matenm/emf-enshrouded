#pragma once
// sim_rate: steps per second from a counter sampled over a time span (unit-tested).
#include <cstdint>

namespace simrate {
// Steps per second (rounded) for `steps` counted over `ms` milliseconds; 0 for a span under 1 ms.
inline uint32_t per_second(uint64_t steps, uint64_t ms) {
    return ms == 0 ? 0u : static_cast<uint32_t>((steps * 1000ull + ms / 2) / ms);
}
}  // namespace simrate
