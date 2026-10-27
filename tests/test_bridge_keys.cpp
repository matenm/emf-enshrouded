// The debug bridge's mod_key: keys pressed for mods' key reads only, released on time, with a count of mod reads.
#include <windows.h>

#include <doctest.h>

#include "core/overlay/injected_keys.h"

using namespace emf::core;

TEST_CASE("injected keys: a press is held for its time, counts mods' reads, then ends") {
    InjectedKeys k;
    CHECK_FALSE(k.any());
    REQUIRE(k.press('L', 1000, 150));
    CHECK(k.any());
    CHECK(k.held('L', 1000));
    CHECK_FALSE(k.held('K', 1000));
    CHECK(k.read('L', 1050));
    CHECK(k.read('L', 1100));
    CHECK_FALSE(k.read('K', 1100));
    CHECK_FALSE(k.press('L', 1100, 150));  // already held
    CHECK(k.expire(1149).empty());
    CHECK_FALSE(k.read('L', 1150));  // its time is up even before expire runs
    const auto ended = k.expire(1150);
    REQUIRE(ended.size() == 1);
    CHECK(ended[0].first == 'L');
    CHECK(ended[0].second == 2);
    CHECK_FALSE(k.any());
}

TEST_CASE("injected keys: down holds until up, at most a minute; bad keys are refused") {
    InjectedKeys k;
    REQUIRE(k.down(VK_F6, 0));
    CHECK(k.read(VK_F6, 30000));
    CHECK(k.up(VK_F6) == 1);
    CHECK_FALSE(k.any());
    CHECK(k.up(VK_F6) == 0);
    REQUIRE(k.down(VK_F6, 0));
    CHECK(k.expire(InjectedKeys::kMaxDownMs - 1).empty());
    CHECK(k.expire(InjectedKeys::kMaxDownMs).size() == 1);
    CHECK_FALSE(k.press(0, 0, 100));
    CHECK_FALSE(k.down(0x1FF, 0));
}

TEST_CASE("injected keys: what the hooked key-state calls answer") {
    InjectedKeys k;
    CHECK_FALSE(injected_key_state(&k, 'L', true, 0, true, true));  // nothing held: as usual
    CHECK_FALSE(injected_key_state(nullptr, 'L', true, 0, true, true));
    REQUIRE(k.press('L', 0, 100));
    // GetAsyncKeyState: down and "pressed since the last call" on the first read of the press only
    CHECK(injected_key_state(&k, 'L', true, 10, true, true) == static_cast<SHORT>(0x8001));
    CHECK(injected_key_state(&k, 'L', true, 11, true, true) == static_cast<SHORT>(0x8000));
    CHECK(injected_key_state(&k, 'L', true, 12, false, true) == static_cast<SHORT>(0x8000));  // GetKeyState: down
    CHECK_FALSE(injected_key_state(&k, 'L', false, 10, true, true));  // the game itself never sees it
    CHECK_FALSE(injected_key_state(&k, 'K', true, 10, true, true));   // other keys, game in front: as usual
    CHECK_FALSE(injected_key_state(&k, 'L', true, 100, true, true));  // released
    k.expire(150);  // the pipe thread releases it
    REQUIRE(k.press('L', 200, 100));
    CHECK(injected_key_state(&k, 'L', true, 210, true, true) == static_cast<SHORT>(0x8001));  // a new press: again
}

TEST_CASE("injected keys: while a key is held for mods with another window in front, mods' other keys read up") {
    InjectedKeys k;
    REQUIRE(k.press('L', 0, 100));
    CHECK(k.any_held(50));
    // the game window is spoofed as in front for mods: the real keys typed elsewhere must not reach them
    CHECK(injected_key_state(&k, 'K', true, 50, true, false) == static_cast<SHORT>(0));
    CHECK(injected_key_state(&k, 'K', true, 50, false, false) == static_cast<SHORT>(0));
    CHECK_FALSE(injected_key_state(&k, 'K', false, 50, true, false));  // the game's own reads: as usual
    CHECK_FALSE(k.any_held(100));  // its time is up: no spoofing, even before expire() ran
    CHECK_FALSE(injected_key_state(&k, 'K', true, 100, true, false));
}
