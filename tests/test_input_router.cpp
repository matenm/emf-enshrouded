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

TEST_CASE("InputRouter: while the console's suggestions show, Esc goes to the console (it hides them) and stays open") {
    InputRouter r;
    r.set_open(true);
    r.set_esc_to_console(true);
    CHECK(r.key(down(kEsc)) == Route::Console);
    CHECK(r.open());
    CHECK(r.legacy_key(down(kEsc)) == Route::Swallow);  // its key message does not reach the game either
    r.set_esc_to_console(false);                         // the list hid
    CHECK(r.key(down(kEsc)) == Route::Swallow);          // auto-repeat of the same press: not a second Esc
    CHECK(r.open());
    CHECK(r.key(up(kEsc)) == Route::Swallow);  // the game never saw it go down
    CHECK(r.key(down(kEsc)) == Route::Toggle);  // the next Esc closes the console as before
    CHECK_FALSE(r.open());
}

TEST_CASE("InputRouter: opening or closing the console forgets that Esc was the suggestions'") {
    InputRouter r;
    r.set_open(true);
    r.set_esc_to_console(true);
    CHECK(r.key(down(kTilde)) == Route::Toggle);  // closed with the list still showing
    CHECK(r.key(up(kTilde)) == Route::Swallow);
    CHECK(r.key(down(kEsc)) == Route::Game);  // closed: Esc is the game's
    CHECK(r.key(up(kEsc)) == Route::Game);
    CHECK(r.key(down(kTilde)) == Route::Toggle);  // open again: no list has shown yet
    CHECK(r.key(up(kTilde)) == Route::Swallow);
    CHECK(r.key(down(kEsc)) == Route::Toggle);
    CHECK_FALSE(r.open());
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

TEST_CASE("InputRouter window: keys go to the overlay; Esc and the close key close it; the console opens over it") {
    constexpr uint16_t kB = 0x30;  // the B key's scan code; its virtual key is 'B'
    InputRouter r;
    CHECK(r.key(down(kW)) == Route::Game);  // W held while the window opens
    r.set_window(true, 'B');
    CHECK(r.window());
    CHECK(r.capturing());
    CHECK_FALSE(r.open());
    CHECK(r.key(down(kA)) == Route::Console);  // the overlay sees it, the game does not
    CHECK(r.key(up(kA)) == Route::Console);
    CHECK(r.key(up(kW)) == Route::Both);  // W's release still reaches the game
    CHECK(r.character(L'a') == Route::Swallow);  // nothing types into a window (phase 1)
    CHECK(r.key(down(kB, 'B')) == Route::CloseWindow);  // the close key, on its press
    CHECK(r.key(down(kB, 'B')) == Route::Swallow);      // its auto-repeat does nothing more
    CHECK(r.key(up(kB, 'B')) == Route::Swallow);        // and the game never saw it go down
    CHECK(r.key(down(kEsc)) == Route::CloseWindow);
    CHECK(r.key(up(kEsc)) == Route::Swallow);
    // the console key opens the console over the window; Esc closes the console first, then the window
    CHECK(r.key(down(kTilde)) == Route::Toggle);
    CHECK(r.open());
    CHECK(r.key(up(kTilde)) == Route::Swallow);
    CHECK(r.key(down(kA)) == Route::Console);
    CHECK(r.character(L'a') == Route::Console);  // typing goes to the console
    CHECK(r.key(down(kEsc)) == Route::Toggle);
    CHECK_FALSE(r.open());
    CHECK(r.key(up(kEsc)) == Route::Swallow);
    CHECK(r.key(down(kEsc)) == Route::CloseWindow);
    CHECK(r.key(up(kEsc)) == Route::Swallow);
    CHECK(r.mouse(MouseKind::Move) == Route::Swallow);
    CHECK(r.mouse(MouseKind::ButtonUp) == Route::Game);
    r.set_window(false);
    CHECK_FALSE(r.capturing());
    CHECK(r.key(down(kB, 'B')) == Route::Game);  // no window: B is the game's again
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

TEST_CASE("InputRouter window: the close key is hidden from mods until released (it never reopens the window)") {
    constexpr uint16_t kB = 0x30;
    InputRouter r;
    r.set_window(true, 'B');
    CHECK(r.key(down(kB, 'B')) == Route::CloseWindow);
    r.set_window(false);  // the overlay closed it
    CHECK(r.hidden_from_mods('B', true));  // still down: mods polling it see it up, not a new press
    CHECK(r.key(up(kB, 'B')) == Route::Swallow);
    CHECK_FALSE(r.hidden_from_mods('B', false));
    CHECK_FALSE(r.hidden_from_mods('B', true));  // a later press is a press again
}

TEST_CASE("InputRouter window: auto-repeats of the key that opened it are repeats, not a close; its release reaches the game") {
    constexpr uint16_t kB = 0x30;
    InputRouter r;
    CHECK(r.key(down(kB, 'B')) == Route::Game);  // B opened the window (a mod's on_key)
    r.set_window(true, 'B');
    CHECK(r.key(down(kB, 'B')) == Route::Swallow);  // held past the repeat delay: not the close key
    CHECK(r.key(down(kB, 'B')) == Route::Swallow);
    CHECK(r.window());
    CHECK(r.key(up(kB, 'B')) == Route::Both);  // the game saw it go down: it sees it go up
    CHECK(r.key(down(kB, 'B')) == Route::CloseWindow);  // the next press closes it
}

TEST_CASE("InputRouter window: opened while its close key is down, that press does not close it (it opened it)") {
    // the mod polled the key down and opened the window before the window thread handled the key's own press
    constexpr uint16_t kB = 0x30;
    InputRouter r;
    r.set_window(true, 'B', true);
    CHECK(r.key(down(kB, 'B')) == Route::Swallow);  // the press that opened it
    CHECK(r.window());
    CHECK(r.key(up(kB, 'B')) == Route::Swallow);    // the game never saw it go down
    CHECK(r.key(down(kB, 'B')) == Route::CloseWindow);  // the next press closes it
    CHECK(r.key(up(kB, 'B')) == Route::Swallow);
    r.set_window(true, 'B', false);  // opened with the key up: its first press closes it
    CHECK(r.key(down(kB, 'B')) == Route::CloseWindow);
}

TEST_CASE("InputRouter window: opened while its close key is down after the game saw the press; its release ends that") {
    constexpr uint16_t kB = 0x30;
    InputRouter r;
    CHECK(r.key(down(kB, 'B')) == Route::Game);  // the window thread handled the press first
    r.set_window(true, 'B', true);
    CHECK(r.key(up(kB, 'B')) == Route::Both);  // the game saw it go down
    CHECK(r.key(down(kB, 'B')) == Route::CloseWindow);  // so the next press closes it
}

TEST_CASE("CharSource: raw keys only (no key messages): characters come from raw input at once") {
    CharSource s;
    CHECK(s.from_raw());
    CHECK(s.raw_down(0x1E, L"a") == L"a");
    CHECK(s.raw_up(0x1E).empty());
}

TEST_CASE("CharSource: key messages with character messages (Windows): raw characters are never typed") {
    CharSource s;
    s.legacy_seen();
    CHECK_FALSE(s.from_raw());
    for (int i = 0; i < 6; ++i) {
        CHECK(s.raw_down(0x1E, L"a").empty());
        s.wm_char();  // the game's TranslateMessage made one
        CHECK(s.raw_up(0x1E).empty());
    }
    CHECK_FALSE(s.from_raw());
}

TEST_CASE("CharSource: key messages but no character messages (Wine): after 3 presses raw input types, the held ones too") {
    CharSource s;
    s.legacy_seen();
    CHECK(s.raw_down(0x23, L"h").empty());
    CHECK(s.raw_up(0x23).empty());
    CHECK(s.raw_down(0x12, L"e").empty());
    CHECK(s.raw_up(0x12).empty());
    CHECK(s.raw_down(0x26, L"l").empty());
    CHECK(s.raw_up(0x26) == L"hel");  // the third press without a character message: switch, and type what was held
    CHECK(s.from_raw());
    CHECK(s.raw_down(0x19, L"p") == L"p");
    s.wm_char();  // a late one changes nothing now
    CHECK(s.from_raw());
}

TEST_CASE("CharSource: one fast tap whose character message came late does not switch; a character message resets") {
    CharSource s;
    s.legacy_seen();
    CHECK(s.raw_down(0x1E, L"a").empty());
    CHECK(s.raw_up(0x1E).empty());  // its character message comes after the release
    s.wm_char();
    for (int i = 0; i < 2; ++i) {
        CHECK(s.raw_down(0x30, L"b").empty());
        CHECK(s.raw_up(0x30).empty());
    }
    CHECK_FALSE(s.from_raw());  // 2 misses after the reset, not 3
    s.wm_char();
    CHECK_FALSE(s.from_raw());
}

TEST_CASE("CharSource: keys without characters (Enter, arrows) do not count; key-up of another key does nothing") {
    CharSource s;
    s.legacy_seen();
    for (int i = 0; i < 5; ++i) {
        CHECK(s.raw_down(0x1C, L"\r").empty());  // Enter's character is a control one: it does not count
        CHECK(s.raw_up(0x1C).empty());
        CHECK(s.raw_down(0x0E, L"\b").empty());  // Backspace
        CHECK(s.raw_up(0x0E).empty());
    }
    CHECK_FALSE(s.from_raw());
    CHECK(s.raw_down(0x1E, L"a").empty());
    CHECK(s.raw_up(0x30).empty());  // another key's release
    CHECK_FALSE(s.from_raw());
}

TEST_CASE("CharSource (review): two keys held at once under Wine both count and type in order; closing the console clears what is held") {
    CharSource s;
    s.legacy_seen();
    CHECK(s.raw_down(0x23, L"h").empty());
    CHECK(s.raw_down(0x12, L"e").empty());  // e pressed before h is released
    CHECK(s.raw_up(0x23).empty());
    CHECK(s.raw_up(0x12).empty());
    CHECK(s.raw_down(0x26, L"l").empty());
    CHECK(s.raw_up(0x26) == L"hel");
    CharSource t;
    t.legacy_seen();
    t.raw_down(0x23, L"h");
    t.raw_up(0x23);
    t.raw_down(0x12, L"e");
    t.raw_up(0x12);
    t.closed();  // the console closed: what was held goes
    t.raw_down(0x26, L"l");
    CHECK(t.raw_up(0x26).empty());  // one miss since the close, not three
    CHECK_FALSE(t.from_raw());
}

// 0.6.9: a Linux player's console stayed empty on 0.6.8 and the switch to raw typing never came (no log line). Proton's
// ToUnicodeEx may give nothing for a raw key: then the letter printed on the key (MapVirtualKey VK_TO_CHAR) is typed.
TEST_CASE("fallback_chars: a letter in the case Shift and Caps Lock make, digits and space without Shift, dead keys never") {
    CHECK(fallback_chars('A', false, false) == L"a");
    CHECK(fallback_chars('A', true, false) == L"A");
    CHECK(fallback_chars('A', false, true) == L"A");
    CHECK(fallback_chars('A', true, true) == L"a");
    CHECK(fallback_chars('1', false, false) == L"1");
    CHECK(fallback_chars('1', true, false) == L"");  // Shift+1 is a layout's own symbol: not guessed
    CHECK(fallback_chars(' ', true, false) == L" ");
    CHECK(fallback_chars('-', false, false) == L"-");
    CHECK(fallback_chars(0x80000000u | '^', false, false) == L"");  // a dead key
    CHECK(fallback_chars(0, false, false) == L"");
    CHECK(fallback_chars(0x0D, false, false) == L"");  // a control character
}

// Review (0.6.9): AltGr (Ctrl+Alt) or Alt with a key: no guess (AltGr+Q is @ on a German keyboard, not q; a Ctrl+Alt
// hotkey must type nothing). A non-English letter only plain: Shift or Caps Lock may give another character on its
// layout (French Shift+e-acute is 2, Swiss Shift+u-umlaut is e-grave), so no capital is guessed.
TEST_CASE("fallback_chars (review): nothing with Alt or AltGr; a non-English letter only without Shift and Caps Lock") {
    CHECK(fallback_chars('Q', false, false, true) == L"");
    CHECK(fallback_chars('7', false, false, true) == L"");
    CHECK(fallback_chars(' ', false, false, true) == L"");
    CHECK(fallback_chars(0xDC, false, false) == L"\u00fc");  // a key printed U+00DC types the small letter
    CHECK(fallback_chars(0xDC, true, false) == L"");
    CHECK(fallback_chars(0xDC, false, true) == L"");
    CHECK(fallback_chars(0xDF, true, false) == L"");  // German sharp s with Shift is ?, not guessed
}

namespace {
RawKeyInfo key(uint16_t scan, uint32_t vk, int n, const wchar_t* proton, const wchar_t* fallback, const wchar_t* typed,
               bool waits) {
    RawKeyInfo k;
    k.scan = scan;
    k.vk = vk;
    k.n = n;
    k.proton = proton;
    k.fallback = fallback;
    k.typed = typed;
    k.waits = waits;
    return k;
}
}  // namespace

TEST_CASE("KeyDebugLog: off by default; on, one line per key event while the console is open, the first 20 presses only") {
    KeyDebugLog log;
    CHECK(log.raw_key(key(0x23, 0x48, 1, L"h", L"", L"", true)).empty());  // off
    log.enable(true);
    CHECK(log.opened() == "overlay keys: the console opened; logging its first 20 key presses ([overlay] debug_keys=1)");
    CHECK(log.raw_key(key(0x23, 0x48, 1, L"h", L"", L"", true)) ==
          "overlay keys: raw key 0x23 (vk 0x48): ToUnicodeEx 1 \"h\"; typed now: nothing (held for a character message)");
    CHECK(log.key_message(0x23, 0x48) == "overlay keys: key message 0x23 (vk 0x48)");
    CHECK(log.character(L'h', true) == "overlay keys: character message \"h\" (U+0068): to the console");
    RawKeyInfo q = key(0x10, 0x51, 0, L"", L"Q", L"Q", false);
    q.shift = true;
    q.caps = false;
    CHECK(log.raw_key(q) ==
          "overlay keys: raw key 0x10 (vk 0x51, held: Shift): ToUnicodeEx 0, the key's own letter \"Q\"; typed now: \"Q\"");
    CHECK(log.character(0x2603, false) == "overlay keys: character message \"\\u2603\" (U+2603): not for the console");
    CHECK(log.raw_key(key(0x1A, 0xBA, -1, L"", L"", L"", false)) ==
          "overlay keys: raw key 0x1A (vk 0xBA): ToUnicodeEx -1 (a dead key); typed now: nothing");
    RawKeyInfo c = key(0x2E, 0x43, 0, L"", L"", L"", false);
    c.ctrl = true;
    c.shortcut = true;
    CHECK(log.raw_key(c) == "overlay keys: raw key 0x2E (vk 0x43, held: Ctrl): a shortcut (Ctrl held), not typed");
    for (int i = 4; i < 20; ++i) CHECK_FALSE(log.raw_key(key(0x1E, 0x41, 1, L"a", L"", L"", true)).empty());
    CHECK(log.raw_key(key(0x1E, 0x41, 1, L"a", L"", L"", true)) ==
          "overlay keys: 20 key presses logged; no more until the game starts again");
    CHECK(log.raw_key(key(0x1E, 0x41, 1, L"a", L"", L"", true)).empty());
    CHECK(log.key_message(0x1E, 0x41).empty());
    CHECK(log.character(L'a', true).empty());
    CHECK(log.opened().empty());
}

TEST_CASE("KeyDebugLog: without raw input, key messages count as the presses") {
    KeyDebugLog log(2);
    log.enable(true);
    CHECK_FALSE(log.key_message(0x23, 0x48, true).empty());
    CHECK_FALSE(log.key_message(0x17, 0x49, true).empty());
    CHECK(log.key_message(0x17, 0x49, true) == "overlay keys: 2 key presses logged; no more until the game starts again");
}

namespace {
bool g_game_typing = false;
bool game_typing_for_test() { return g_game_typing; }
}  // namespace

// Typing the console key's character in the game's chat (an apostrophe on a Norwegian keyboard) opened EMF's console.
TEST_CASE("InputRouter: while the game's chat has the keys, the console key types into it; an open console still closes") {
    InputRouter r(0x2b);
    r.set_game_text(&game_typing_for_test);
    g_game_typing = true;
    CHECK(r.key(down(0x2b)) == Route::Game);
    CHECK_FALSE(r.open());
    CHECK(r.character(L'\'') == Route::Game);  // its character reaches the chat box
    CHECK(r.key(up(0x2b)) == Route::Game);
    g_game_typing = false;
    CHECK(r.key(down(0x2b)) == Route::Toggle);  // out of the chat: the console opens as before
    CHECK(r.open());
    CHECK(r.key(up(0x2b)) == Route::Swallow);
    g_game_typing = true;  // the console open (it has the keys, not the game's chat): its key closes it
    CHECK(r.key(down(0x2b)) == Route::Toggle);
    CHECK_FALSE(r.open());
    g_game_typing = false;
}

// Held in the chat while Enter closed it: the next auto-repeat opened the console, and the game never saw the release.
TEST_CASE("InputRouter: the console key held in the chat as it closes: its repeats and its release still go to the game") {
    InputRouter r(0x2b);
    r.set_game_text(&game_typing_for_test);
    g_game_typing = true;
    CHECK(r.key(down(0x2b)) == Route::Game);
    CHECK(r.legacy_key(down(0x2b)) == Route::Game);
    CHECK(r.key(down(0x2b)) == Route::Game);  // auto-repeat while typing
    g_game_typing = false;                     // Enter closed the chat
    CHECK(r.key(down(0x2b)) == Route::Game);  // a repeat, not a press: the console stays closed
    CHECK_FALSE(r.open());
    CHECK(r.key(up(0x2b)) == Route::Game);
    CHECK(r.legacy_key(up(0x2b)) == Route::Game);
    CHECK(r.key(down(0x2b)) == Route::Toggle);  // the next press opens it
    CHECK(r.open());
}
