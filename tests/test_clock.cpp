#include <doctest.h>

#include "../mods/clock/clock_text.h"

TEST_CASE("clock: the game's time of day as text, 24 h or 12 h; nonsense gives nothing") {
    constexpr int64_t kHour = 3600ll * 1000000000ll;
    CHECK(clockmod::text(14 * kHour + 5 * 60 * 1000000000ll, true) == "14:05");
    CHECK(clockmod::text(0, true) == "00:00");
    CHECK(clockmod::text(14 * kHour + 5 * 60 * 1000000000ll, false) == "2:05 PM");
    CHECK(clockmod::text(0, false) == "12:00 AM");
    CHECK(clockmod::text(12 * kHour, false) == "12:00 PM");
    CHECK(clockmod::text(-1, true).empty());
    CHECK(clockmod::text(25 * kHour, true).empty());  // not a time of day: a layout change after a game update?
}

TEST_CASE("clock: the place setting maps to a HUD corner; anything else is the bottom right") {
    CHECK(clockmod::place_from(0) == EMF_HUD_TOP_RIGHT);
    CHECK(clockmod::place_from(1) == EMF_HUD_BOTTOM_RIGHT);
    CHECK(clockmod::place_from(2) == EMF_HUD_BOTTOM_LEFT);
    CHECK(clockmod::place_from(3) == EMF_HUD_BOTTOM_RIGHT);
    CHECK(clockmod::place_from(-1) == EMF_HUD_BOTTOM_RIGHT);
}
