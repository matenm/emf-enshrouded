// loot_scale: the factor a player sets, applied to the game's "Loot Amount" world setting.
#include <doctest.h>

#include <cmath>

#include "../mods/loot_scale/loot_scale.h"

TEST_CASE("loot_scale: the factor stays between 0.1 and 10; nonsense falls back to 1") {
    CHECK(lootscale::clamp_factor(2.0) == doctest::Approx(2.0));
    CHECK(lootscale::clamp_factor(0.01) == doctest::Approx(0.1));
    CHECK(lootscale::clamp_factor(50.0) == doctest::Approx(10.0));
    CHECK(lootscale::clamp_factor(-3.0) == doctest::Approx(0.1));
    CHECK(lootscale::clamp_factor(std::nan("")) == doctest::Approx(1.0));
}

TEST_CASE("loot_scale: the world's own setting is multiplied (so 'Loot Amount' in the world settings still counts)") {
    CHECK(lootscale::scaled(1.0f, 2.0) == doctest::Approx(2.0f));
    CHECK(lootscale::scaled(1.5f, 2.0) == doctest::Approx(3.0f));   // the Relaxed preset's 1.5
    CHECK(lootscale::scaled(0.75f, 1.0) == doctest::Approx(0.75f)); // factor 1: the game's value unchanged
    CHECK(lootscale::scaled(1.0f, 100.0) == doctest::Approx(10.0f)); // clamped
}
