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
    g.seen(kScreenInGame, 900);  // armed (below)
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
    g.seen(kScreenInGame, 90);  // armed: a reading said InGame once
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

TEST_CASE("hud_screen_offset_at: the offset from the signature's bytes, only when its compare is with TextChat") {
    // movzx r15d, byte [rdi+0x1cd8]; cmp r15b, 0x2b; mov [rbp+...], r15b ...
    uint8_t code[] = {0x44, 0x0F, 0xB6, 0xBF, 0xD8, 0x1C, 0x00, 0x00, 0x41, 0x80, 0xFF, 0x2B, 0x44, 0x88, 0xBD};
    CHECK(hud_screen_offset_at(code) == 0x1cd8u);
    code[11] = 0x2c;  // compares with another screen: not the read we know
    CHECK(hud_screen_offset_at(code) == 0u);
    code[11] = 0x2b;
    code[4] = 0x00;  // a zero offset
    code[5] = 0x00;
    CHECK(hud_screen_offset_at(code) == 0u);
    code[4] = 0x00;
    code[5] = 0x00;
    code[6] = 0x01;  // beyond 64 KB: UiPlayData is not that large
    CHECK(hud_screen_offset_at(code) == 0u);
    CHECK(hud_screen_offset_at(nullptr) == 0u);
    code[4] = 0xD8;
    code[5] = 0x1C;
    code[6] = 0x00;
    REQUIRE(hud_screen_offset_at(code) == 0x1cd8u);
    code[3] = 0xBE;  // movzx r15d, byte [rsi+...]: another base register, not the read we know
    CHECK(hud_screen_offset_at(code) == 0u);
    code[3] = 0xBF;
    code[10] = 0xFE;  // cmp r14b: another register
    CHECK(hud_screen_offset_at(code) == 0u);
}

TEST_CASE("game_screen_line: the screen by the game's own name, and whether mods' keys read as up") {
    CHECK(game_screen_line(kScreenTextChat, 0x1cd8, false, true) ==
          "screen: 43 TextChat: mods' keys read as up (the game's text box has them)");
    CHECK(game_screen_line(kScreenRename, 0x1cd8, false, true) ==
          "screen: 47 Rename: mods' keys read as up (the game's text box has them)");
    CHECK(game_screen_line(kScreenInGame, 0x1cd8, false, true) == "screen: 1 InGame: mods' keys as usual");
    CHECK(game_screen_line(9, 0x1cd8, false, true) == "screen: 9 Backpack: mods' keys as usual");
    CHECK(game_screen_line(53, 0x1cd8, false, true) == "screen: 53 YcPlayground: mods' keys as usual");
    CHECK(game_screen_line(200, 0x1cd8, false, true) == "screen: 200: mods' keys as usual");
    CHECK(game_screen_line(std::nullopt, 0x1cd8, false, true) == "screen: not known (no game world on this client right now)");
    CHECK(game_screen_line(std::nullopt, 0, false, true) ==
          "screen: not known: the chat key gate is off (the HudScreenRead signature is not usable on this game build)");
    CHECK(game_screen_line(std::nullopt, 0, true, true) == "screen: the dedicated server has no game screen");
    // the key hooks are not in (the overlay is off): the screen is read, but nothing is paused
    CHECK(game_screen_line(kScreenTextChat, 0x1cd8, false, false) ==
          "screen: 43 TextChat: mods' keys are not paused (EMF's key hooks are not installed: the overlay is off)");
}

// The hook read the clock before the reading: a reading a moment newer than the caller's clock said "not known".
TEST_CASE("game screen: a reading newer than the caller's clock counts") {
    GameScreen g;
    g.seen(kScreenInGame, 900);
    g.seen(kScreenTextChat, 1000);
    REQUIRE(g.screen(999).has_value());
    CHECK(g.typing(999));
}

// A key seen up while typing stayed marked until the first read after the chat closed.
TEST_CASE("typing gate: a key seen up while typing is not held after the close") {
    TypingKeyGate gate;
    CHECK(gate.hide('K', true, true));
    CHECK(gate.hide('K', false, true));  // let go while still typing
    CHECK_FALSE(gate.any());
    CHECK_FALSE(gate.hide('K', true, false));  // a press after the close is a press
}

// A mod that never asked about the key again (a changed keybind) left the mark, and the gate armed, for good.
TEST_CASE("typing gate: the window's key release clears the mark, Shift, Ctrl and Alt by either side too") {
    TypingKeyGate gate;
    CHECK(gate.hide('K', true, true));
    gate.released('K');
    CHECK_FALSE(gate.any());
    CHECK(gate.hide(VK_SHIFT, true, true));
    CHECK(gate.hide(VK_CONTROL, true, true));
    CHECK(gate.hide(VK_MENU, true, true));
    gate.released(VK_LSHIFT);
    gate.released(VK_RCONTROL);
    gate.released(VK_LMENU);
    CHECK_FALSE(gate.any());
    CHECK(gate.hide(VK_LSHIFT, true, true));
    gate.released(VK_SHIFT);
    CHECK_FALSE(gate.any());
    gate.released(0);  // out of range: nothing
    gate.released(300);
}

TEST_CASE("typing gates: GetAsyncKeyState's and GetKeyState's marks are kept apart; a release clears both") {
    CHECK(&typing_gate(true) != &typing_gate(false));
    CHECK(typing_gate(true).hide('J', true, true));
    CHECK_FALSE(typing_gate(false).any());
    CHECK(typing_gate(false).hide('J', true, true));
    typing_key_released('J');
    CHECK_FALSE(typing_gate(true).any());
    CHECK_FALSE(typing_gate(false).any());
}

// A fail-safe: if a game build's byte at the signature's offset meant something else and read a steady 43 (TextChat),
// every mod key and the console key would pause for good. The gate arms only after a reading said InGame once.
TEST_CASE("game screen: keys pause only after a reading said InGame once") {
    GameScreen g;
    g.seen(kScreenTextChat, 1000);
    CHECK_FALSE(g.typing(1000));  // never in game yet: not armed
    g.seen(kScreenInGame, 1100);
    g.seen(kScreenTextChat, 1200);
    CHECK(g.typing(1200));
    g.seen(kScreenSocial, 5000);  // armed for good after that
    CHECK(g.typing(5000));
}
