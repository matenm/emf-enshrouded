#pragma once
// loot_scale: the factor applied to the game's "Loot Amount" world setting (unit-tested).
#include <cmath>

namespace lootscale {
constexpr double kMin = 0.1, kMax = 10.0;

// The player's factor, kept between kMin and kMax; anything that is not a number means 1 (no change).
inline double clamp_factor(double f) {
    if (std::isnan(f)) return 1.0;
    return f < kMin ? kMin : (f > kMax ? kMax : f);
}

// The value the game gets: its own setting (the world's "Loot Amount", 0.25..2) times the factor.
inline float scaled(float game_value, double factor) {
    return static_cast<float>(game_value * clamp_factor(factor));
}
}  // namespace lootscale
