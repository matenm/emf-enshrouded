// The game's current HUD screen (the chat key gate): which screens take the keys, how long a reading counts, and the
// keys mods are told are up while the player types and after.
#include <doctest.h>

#include <windows.h>

#include <cstdint>
#include <vector>

#include "core/game_screen.h"

using namespace emf::core;

TEST_CASE("game screen: the chat, the social pane's chat and the rename box take the keys; play and menus do not") {
    CHECK(screen_takes_text(kScreenTextChat));
    CHECK(screen_takes_text(kScreenSocial));
    CHECK(screen_takes_text(kScreenRename));
    CHECK(kScreenTextChat == 43);
    CHECK(kScreenSocial == 44);
    CHECK(kScreenRename == 47);
    for (uint8_t s : {0, 1, 2, 5, 9, 22, 42, 45, 46, 48, 53, 255}) {
        CAPTURE(int(s));
        CHECK_FALSE(screen_takes_text(s));
    }
}

TEST_CASE("game screen: a reading counts for 500 ms; nothing before the first") {
    GameScreen g;
    CHECK_FALSE(g.screen(1000).has_value());
    CHECK_FALSE(g.typing(1000));
    g.seen(kScreenTextChat, 1000);
    REQUIRE(g.screen(1000).has_value());
    CHECK(*g.screen(1000) == kScreenTextChat);
    CHECK(g.typing(1500));
    CHECK_FALSE(g.typing(1501));  // stale: the main menu, a load, the client world stopped
    CHECK_FALSE(g.screen(1501).has_value());
    g.seen(kScreenInGame, 2000);
    CHECK_FALSE(g.typing(2000));
    CHECK(*g.screen(2000) == kScreenInGame);
}

TEST_CASE("game screen: read takes the byte at the signature's offset, and nothing without one") {
    std::vector<uint8_t> uipd(0x2000, 0);
    uipd[0x1cd8] = kScreenRename;
    GameScreen g;
    g.read(uipd.data(), 100);  // no offset yet: the signature was not usable
    CHECK_FALSE(g.screen(100).has_value());
    g.set_offset(0x1cd8);
    CHECK(g.offset() == 0x1cd8);
    g.read(nullptr, 100);  // no UiPlayData (the main menu)
    CHECK_FALSE(g.screen(100).has_value());
    g.read(uipd.data(), 100);
    REQUIRE(g.screen(100).has_value());
    CHECK(*g.screen(100) == kScreenRename);
    CHECK(g.typing(100));
}

TEST_CASE("game screen: a read that faults keeps no value") {
    GameScreen g;
    g.set_offset(0x10);
    g.read(reinterpret_cast<const uint8_t*>(uintptr_t{8}), 100);  // unreadable memory
    CHECK_FALSE(g.screen(100).has_value());
}

TEST_CASE("typing gate: keys read as up while the player types, and a key held then stays up until released") {
    TypingKeyGate gate;
    CHECK_FALSE(gate.any());
    CHECK(gate.hide('K', true, true));    // typing K in the chat
    CHECK(gate.hide('W', false, true));   // every key reads as up while typing
    CHECK(gate.any());
    CHECK(gate.hide('K', true, false));   // the chat closed with K still held: not a new press
    CHECK(gate.any());
    CHECK_FALSE(gate.hide('K', false, false));  // released: it counts again
    CHECK_FALSE(gate.any());
    CHECK_FALSE(gate.hide('K', true, false));   // the next press is a press
}

TEST_CASE("typing gate: a key first pressed after the chat closed counts at once") {
    TypingKeyGate gate;
    CHECK(gate.hide(VK_RETURN, true, true));  // the Enter that sends the line
    CHECK_FALSE(gate.hide('K', true, false));  // K was not down while typing
    CHECK(gate.hide(VK_RETURN, true, false));  // Enter still held
    CHECK_FALSE(gate.hide(VK_RETURN, false, false));
}

TEST_CASE("typing gate: key codes outside 1-255 are never hidden after typing and never marked") {
    TypingKeyGate gate;
    CHECK(gate.hide(0, true, true));  // while typing everything reads as up
    CHECK(gate.hide(300, true, true));
    CHECK_FALSE(gate.any());
    CHECK_FALSE(gate.hide(300, true, false));
}
