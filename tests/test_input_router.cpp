#include <windows.h>

#include <doctest.h>

#include <utility>
#include <vector>

#include "core/overlay/input_router.h"

using namespace emf::core;

namespace {
KeyEvent down(uint16_t scan, uint16_t vk = 0) { return KeyEvent{scan, vk, true, false}; }
KeyEvent up(uint16_t scan, uint16_t vk = 0) { return KeyEvent{scan, vk, false, false}; }
constexpr uint16_t kW = 0x11, kEsc = 0x01, kTilde = 0x29, kA = 0x1E;
}  // namespace

TEST_CASE("InputRouter closed: everything goes to the game; the toggle key opens the console") {
    InputRouter r;
    CHECK_FALSE(r.open());
    CHECK(r.key(down(kW)) == Route::Game);
    CHECK(r.key(up(kW)) == Route::Game);
    CHECK(r.character(L'w') == Route::Game);
    CHECK(r.mouse(MouseKind::Move) == Route::Game);
    CHECK(r.mouse(MouseKind::ButtonDown) == Route::Game);
    CHECK(r.mouse(MouseKind::Wheel) == Route::Game);
    CHECK(r.key(down(kTilde)) == Route::Toggle);
    CHECK(r.open());
}

TEST_CASE("InputRouter open: key presses go to the console, a release always reaches the game too") {
    InputRouter r;
    r.set_open(true);
    CHECK(r.key(down(kW)) == Route::Console);
    CHECK(r.key(up(kW)) == Route::Both);  // W held while opening: the game still sees it released
    CHECK(r.key(down(kA)) == Route::Console);
    CHECK(r.character(L'a') == Route::Console);
    CHECK(r.key(down(kEsc)) == Route::Toggle);  // Esc closes
    CHECK_FALSE(r.open());
    CHECK(r.key(up(kEsc)) == Route::Swallow);  // the game never saw Esc go down
}

TEST_CASE("InputRouter: the toggle's character is swallowed, but only right after the toggle") {
    InputRouter r;
    CHECK(r.key(down(kTilde)) == Route::Toggle);
    CHECK(r.character(L'§') == Route::Swallow);  // "§" from the toggle key itself
    CHECK(r.key(up(kTilde)) == Route::Swallow);
    CHECK(r.key(down(kA)) == Route::Console);
    CHECK(r.character(L'a') == Route::Console);
    // A raw-only keyboard sends no character for the toggle: the next real character must not be eaten.
    InputRouter raw;
    CHECK(raw.key(down(kTilde)) == Route::Toggle);
    CHECK(raw.key(down(kA)) == Route::Console);
    CHECK(raw.character(L'a') == Route::Console);
    // Esc's own character (0x1B) after closing does not reach the game either.
    InputRouter esc;
    esc.set_open(true);
    CHECK(esc.key(down(kEsc)) == Route::Toggle);
    CHECK(esc.character(L'\x1b') == Route::Swallow);
}

TEST_CASE("InputRouter open: mouse movement and presses are kept from the game, releases pass, the wheel scrolls") {
    InputRouter r;
    r.set_open(true);
    CHECK(r.mouse(MouseKind::Move) == Route::Swallow);
    CHECK(r.mouse(MouseKind::ButtonDown) == Route::Swallow);
    CHECK(r.mouse(MouseKind::ButtonUp) == Route::Game);
    CHECK(r.mouse(MouseKind::Wheel) == Route::Console);
}

TEST_CASE("InputRouter: another toggle key and the extended twin of a scan code") {
    InputRouter r(0x44);  // F10
    CHECK(r.key(down(kTilde)) == Route::Game);
    CHECK(r.key(down(0x44)) == Route::Toggle);
    InputRouter e;
    CHECK(e.key(KeyEvent{kTilde, 0, true, true}) == Route::Game);  // an E0-prefixed key with the same scan code
}

TEST_CASE("decode_raw_keyboard: make/break, E0 keys, and the fake 0xFF key") {
    RAWKEYBOARD rk{};
    rk.MakeCode = 0x11;
    rk.VKey = 'W';
    rk.Flags = RI_KEY_MAKE;
    KeyEvent e{};
    REQUIRE(decode_raw_keyboard(rk, &e));
    CHECK(e.scan == 0x11);
    CHECK(e.vkey == 'W');
    CHECK(e.down);
    CHECK_FALSE(e.extended);
    rk.Flags = RI_KEY_BREAK | RI_KEY_E0;
    REQUIRE(decode_raw_keyboard(rk, &e));
    CHECK_FALSE(e.down);
    CHECK(e.extended);
    rk.VKey = 0xFF;  // part of an escaped sequence (e.g. Pause), not a key of its own
    CHECK_FALSE(decode_raw_keyboard(rk, &e));
}

TEST_CASE("caller_is_mod: a return address inside a loaded mod's image") {
    const std::vector<std::pair<uintptr_t, uintptr_t>> mods = {{0x10000, 0x20000}, {0x50000, 0x58000}};
    CHECK(caller_is_mod(0x10000, mods));
    CHECK(caller_is_mod(0x57fff, mods));
    CHECK_FALSE(caller_is_mod(0x20000, mods));
    CHECK_FALSE(caller_is_mod(0x40000, mods));
    CHECK_FALSE(caller_is_mod(0x10000, {}));
}

TEST_CASE("InputRouter: holding the toggle key toggles once (raw input repeats the make code)") {
    InputRouter r;
    CHECK(r.key(down(kTilde)) == Route::Toggle);
    CHECK(r.open());
    CHECK(r.key(down(kTilde)) == Route::Swallow);  // auto-repeat
    CHECK(r.key(down(kTilde)) == Route::Swallow);
    CHECK(r.open());
    CHECK(r.key(up(kTilde)) == Route::Swallow);
    CHECK(r.key(down(kTilde)) == Route::Toggle);  // a new press closes
    CHECK_FALSE(r.open());
}

TEST_CASE("InputRouter: Esc held after it closed the console stays away from the game until released") {
    InputRouter r;
    r.set_open(true);
    CHECK(r.key(down(kEsc)) == Route::Toggle);
    CHECK_FALSE(r.open());
    CHECK(r.key(down(kEsc)) == Route::Swallow);  // auto-repeat: not a pause-menu press
    CHECK(r.key(up(kEsc)) == Route::Swallow);
    CHECK(r.key(down(kEsc)) == Route::Game);  // a new press is the game's
}

TEST_CASE("InputRouter: key messages that follow raw input copy its decision") {
    InputRouter r;
    CHECK(r.legacy_key(down(kA)) == Route::Game);  // a key raw input never decided: the game's
    CHECK(r.key(down(kW)) == Route::Game);
    CHECK(r.legacy_key(down(kW)) == Route::Game);
    CHECK(r.key(up(kW)) == Route::Game);
    CHECK(r.legacy_key(up(kW)) == Route::Game);
    CHECK(r.key(down(kTilde)) == Route::Toggle);  // opens
    CHECK(r.legacy_key(down(kTilde)) == Route::Swallow);
    CHECK(r.key(down(kW)) == Route::Console);
    CHECK(r.legacy_key(down(kW)) == Route::Swallow);
    CHECK(r.key(up(kW)) == Route::Both);
    CHECK(r.legacy_key(up(kW)) == Route::Game);  // releases reach the game
    CHECK(r.key(down(kEsc)) == Route::Toggle);  // closes: its key message must not reach the game either
    CHECK_FALSE(r.open());
    CHECK(r.legacy_key(down(kEsc)) == Route::Swallow);
    CHECK(r.key(up(kEsc)) == Route::Swallow);
    CHECK(r.legacy_key(up(kEsc)) == Route::Swallow);
}

TEST_CASE("InputRouter: while open, key presses seen only as key messages stay with the console") {
    InputRouter r;
    r.set_open(true);
    CHECK(r.legacy_key(KeyEvent{0, VK_PACKET, true, false}) == Route::Swallow);  // injected Unicode (no raw scan)
    CHECK(r.legacy_key(KeyEvent{0, VK_PACKET, false, false}) == Route::Game);
}

TEST_CASE("InputRouter: after focus loss a held toggle key does not block the next press") {
    InputRouter r;
    CHECK(r.key(down(kTilde)) == Route::Toggle);  // opened, then alt-tab while holding: the release never comes
    r.focus_lost();
    CHECK(r.key(down(kTilde)) == Route::Toggle);  // the next press closes
    CHECK_FALSE(r.open());
}

TEST_CASE("HeldKeys: keys held when the window lost the keyboard do not stay held (their releases went elsewhere)") {
    HeldKeys k;
    k.track(VK_CONTROL, KeyEvent{0x1D, VK_CONTROL, true, false});  // left Ctrl down (Ctrl+Alt+Del, Ctrl+Esc...)
    k.track(VK_MENU, KeyEvent{0x38, VK_MENU, true, false});        // Alt down (alt-tab)
    k.track('A', KeyEvent{0x1E, 'A', true, false});
    CHECK(k.down(VK_CONTROL));
    CHECK(k.down(VK_LCONTROL));
    CHECK(k.down(VK_MENU));
    k.track('A', KeyEvent{0x1E, 'A', false, false});
    CHECK_FALSE(k.down('A'));
    k.clear();  // WM_KILLFOCUS
    CHECK_FALSE(k.down(VK_CONTROL));
    CHECK_FALSE(k.down(VK_LCONTROL));
    CHECK_FALSE(k.down(VK_MENU));
    CHECK_FALSE(k.down(VK_LMENU));
    k.track(VK_SHIFT, KeyEvent{0x36, VK_SHIFT, true, false});  // right Shift
    CHECK(k.down(VK_RSHIFT));
    CHECK(k.down(VK_SHIFT));
    CHECK_FALSE(k.down(VK_LSHIFT));
}

TEST_CASE("InputRouter: the toggle key's auto-repeat characters are not typed into the console") {
    InputRouter r;
    CHECK(r.key(down(kTilde)) == Route::Toggle);
    CHECK(r.character(L'`') == Route::Swallow);
    CHECK(r.key(down(kTilde)) == Route::Swallow);  // repeat
    CHECK(r.character(L'`') == Route::Swallow);    // its character
    CHECK(r.key(down(kA)) == Route::Console);      // typing while still holding the toggle key
    CHECK(r.character(L'a') == Route::Console);
}

namespace {
// A keyboard layout double: the toggle key (VK 0xDC) is a dead key; a pending dead key composes with the next key.
int g_calls = 0;
bool g_pending = false;
int WINAPI fake_to_unicode(UINT vk, UINT, const BYTE*, LPWSTR buf, int, UINT, HKL) {
    ++g_calls;
    if (vk == 0xDC) {
        if (g_pending) {
            g_pending = false;
            buf[0] = L'^';
            buf[1] = L'^';
            return 2;
        }
        g_pending = true;
        return -1;
    }
    if (g_pending) {
        g_pending = false;
        buf[0] = L'^';
        buf[1] = L'h';
        return 2;
    }
    buf[0] = L'h';
    return 1;
}
}  // namespace

TEST_CASE("flush_dead_key: a dead toggle key left pending is cleared, so the next key types plainly") {
    g_pending = true;  // the game's TranslateMessage stored the toggle's dead key
    g_calls = 0;
    flush_dead_key(&fake_to_unicode, 0xDC, 0x29, nullptr);
    CHECK_FALSE(g_pending);
    wchar_t buf[4] = {};
    CHECK(fake_to_unicode('H', 0x23, nullptr, buf, 4, 0, nullptr) == 1);
    CHECK(buf[0] == L'h');
}

TEST_CASE("flush_dead_key: a toggle key that is not pending (or not dead) leaves nothing behind") {
    g_pending = false;  // nothing stored: the first call stores it, the loop clears it again
    flush_dead_key(&fake_to_unicode, 0xDC, 0x29, nullptr);
    CHECK_FALSE(g_pending);
    g_calls = 0;
    flush_dead_key(&fake_to_unicode, 'H', 0x23, nullptr);  // an ordinary key: one call, no state
    CHECK(g_calls == 1);
    CHECK_FALSE(g_pending);
}

TEST_CASE("InputRouter wheel: keys go to the overlay, a key held before still releases to the game, no toggling") {
    InputRouter r;
    CHECK(r.key(down(kW)) == Route::Game);  // W held while the wheel opens
    r.set_wheel(true);
    CHECK(r.wheel());
    CHECK(r.capturing());
    CHECK_FALSE(r.open());                       // the console itself stays closed
    CHECK(r.key(down(kA)) == Route::Console);    // the overlay (the wheel) sees it, the game does not
    CHECK(r.key(up(kA)) == Route::Console);      // the game never saw A go down
    CHECK(r.key(up(kW)) == Route::Both);         // but W's release reaches the game
    CHECK(r.key(down(kTilde)) == Route::Console);  // the console key does not open the console over the wheel
    CHECK_FALSE(r.open());
    CHECK(r.key(up(kTilde)) == Route::Console);
    CHECK(r.key(down(kEsc)) == Route::Console);  // Esc is the wheel's (cancel), not the console's
    CHECK_FALSE(r.open());
    CHECK(r.character(L'a') == Route::Swallow);  // characters go nowhere while the wheel is open
    r.set_wheel(false);
    CHECK_FALSE(r.capturing());
    CHECK(r.key(down(kA)) == Route::Game);
    CHECK(r.character(L'a') == Route::Game);
}

TEST_CASE("InputRouter wheel: mouse movement and presses are kept from the game, releases pass, the wheel pages") {
    InputRouter r;
    r.set_wheel(true);
    CHECK(r.mouse(MouseKind::Move) == Route::Swallow);
    CHECK(r.mouse(MouseKind::ButtonDown) == Route::Swallow);
    CHECK(r.mouse(MouseKind::ButtonUp) == Route::Game);
    CHECK(r.mouse(MouseKind::Wheel) == Route::Console);
}

TEST_CASE("InputRouter wheel: key messages copy the raw decision; presses are swallowed while it captures") {
    InputRouter r;
    r.set_wheel(true);
    CHECK(r.key(down(kA)) == Route::Console);
    CHECK(r.legacy_key(down(kA)) == Route::Swallow);
    CHECK(r.key(up(kA)) == Route::Console);
    CHECK(r.legacy_key(up(kA)) == Route::Swallow);  // the game never saw A: no stray release message
}

TEST_CASE("InputRouter wheel: a key held before the wheel opened still releases to the game after auto-repeats") {
    InputRouter r;
    CHECK(r.key(down(kW)) == Route::Game);  // W held (walking) when a wheel opens
    r.set_wheel(true);
    CHECK(r.key(down(kW)) == Route::Console);  // its auto-repeat goes to the overlay
    CHECK(r.key(down(kW)) == Route::Console);
    CHECK(r.legacy_key(down(kW)) == Route::Swallow);
    CHECK(r.key(up(kW)) == Route::Both);  // the game saw W go down: it must see it go up
    CHECK(r.legacy_key(up(kW)) == Route::Game);
    CHECK(r.key(down(kA)) == Route::Console);  // pressed and repeated during the wheel: the game never saw it
    CHECK(r.key(down(kA)) == Route::Console);
    CHECK(r.key(up(kA)) == Route::Console);
}

TEST_CASE("InputRouter: a wheel's hold key still down after the wheel closed stays hidden from mods until released") {
    InputRouter r;
    CHECK_FALSE(r.hiding());
    r.hide_until_released('K');
    CHECK(r.hiding());
    CHECK(r.hidden_from_mods('K', true));         // still held: mods are told it is up (no new press)
    CHECK_FALSE(r.hidden_from_mods('J', true));   // other keys are not affected
    CHECK_FALSE(r.hidden_from_mods('K', false));  // released: up anyway, and the hiding ends
    CHECK_FALSE(r.hiding());
    CHECK_FALSE(r.hidden_from_mods('K', true));   // the next press is a real press

    r.hide_until_released('K');
    KeyEvent up_k{0x25, 'K', false, false};
    r.key(up_k);  // the release seen through raw input ends it too
    CHECK_FALSE(r.hiding());
}
