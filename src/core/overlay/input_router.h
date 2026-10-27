#pragma once
// Which keyboard/mouse input goes to the in-game console and which to the game. Pure logic (unit-tested); the window
// hook in input_hook.cpp applies it.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

namespace emf::core {

struct KeyEvent {
    uint16_t scan = 0;  // keyboard scan code (make code)
    uint16_t vkey = 0;  // virtual key, if known
    bool down = false;
    bool extended = false;  // E0-prefixed
};

enum class Route : uint8_t {
    Game,     // pass to the game only
    Console,  // to the console only
    Both,     // to the console and the game (key releases while open)
    Toggle,   // the console opened or closed; nobody else sees it
    Swallow,  // nobody sees it
};

enum class MouseKind : uint8_t { Move, ButtonDown, ButtonUp, Wheel };

class InputRouter {
public:
    static constexpr uint16_t kEscScan = 0x01;
    explicit InputRouter(uint16_t toggle_scan = 0x29) : toggle_(toggle_scan) {
        for (bool& p : passes_) p = true;
        for (bool& d : game_down_) d = false;
    }
    bool open() const { return open_.load(); }  // the console is open (read from any thread)
    void set_open(bool open) { open_ = open; }
    // A mod's radial wheel captures the input (the console stays closed): keys go to the overlay, releases of keys
    // the game saw go down reach the game too, the mouse is kept from the game.
    bool wheel() const { return wheel_.load(); }
    void set_wheel(bool on) { wheel_ = on; }
    // The console or a wheel holds the input (the game gets no presses and no mouse movement).
    bool capturing() const { return open_.load() || wheel_.load(); }
    // A key from raw input (WM_INPUT), or from key messages when the window gets no raw keyboard input.
    Route key(const KeyEvent& e);
    // A key message (WM_KEYDOWN/UP) that follows the same key's raw input: it copies the raw decision (Game or
    // Swallow), so the key that closes the console does not reach the game as a key message either.
    Route legacy_key(const KeyEvent& e);
    Route character(wchar_t ch);
    Route mouse(MouseKind kind) const;
    void focus_lost();  // the window lost the keyboard: a held toggle key's release will not come
    // A wheel closed while its hold key is still down (Esc, a click): mods polling that key must not see a new press,
    // so it stays hidden from their GetAsyncKeyState/GetKeyState until it is released. Any thread.
    void hide_until_released(uint16_t vk) { hidden_vk_ = vk; }
    bool hiding() const { return hidden_vk_.load() != 0; }
    // True if a mod asking about `vk` (physically `down`) is to be told it is up. A release ends the hiding.
    bool hidden_from_mods(int vk, bool down);
    uint16_t toggle_scan() const { return toggle_; }

private:
    Route decide(const KeyEvent& e);
    static size_t index(const KeyEvent& e) { return (e.scan & 0xFFu) | (e.extended ? 0x100u : 0u); }
    uint16_t toggle_;
    std::atomic<bool> open_{false};
    std::atomic<bool> wheel_{false};
    std::atomic<uint16_t> hidden_vk_{0};
    bool swallow_char_ = false;  // the character right after a toggle belongs to the toggle key
    uint16_t held_ = 0;          // the key that toggled, until it is released (its auto-repeats are swallowed)
    bool passes_[512];           // per key: did the last raw decision let the game see it
    bool game_down_[512];        // per key: the game saw it go down and not up again (auto-repeats do not change it)
};

// The keyboard as raw input showed it (the state ToUnicodeEx needs to turn a raw key into characters): which keys are
// down, with Shift/Ctrl/Alt kept in step with their left and right keys. Window thread only.
class HeldKeys {
public:
    void track(UINT vk, const KeyEvent& e);  // vk: e.vkey, or mapped from the scan code when raw input gave none
    // The window lost the keyboard (alt-tab, another window): releases go elsewhere, so nothing counts as held.
    void clear();
    bool down(UINT vk) const { return vk < 256 && (keys_[vk] & 0x80) != 0; }
    const BYTE* state() const { return keys_; }

private:
    BYTE keys_[256] = {};
};

// The toggle key may be a dead key (German ^, US-International `): the game's TranslateMessage then keeps it pending,
// and the next key typed into the console would compose with it ("^help", "ê"). Called on the window thread after
// the toggle's key message, this clears it. `to_unicode` is ToUnicodeEx (a parameter so it can be tested).
using ToUnicodeFn = int(WINAPI*)(UINT vk, UINT scan, const BYTE* state, LPWSTR buf, int n, UINT flags, HKL layout);
void flush_dead_key(ToUnicodeFn to_unicode, UINT vk, UINT scan, HKL layout);

// RAWKEYBOARD -> KeyEvent; false for the fake 0xFF keys of escaped sequences.
bool decode_raw_keyboard(const RAWKEYBOARD& rk, KeyEvent* out);

// True if `return_address` lies in one of the [begin, end) module ranges (the loaded mods).
bool caller_is_mod(uintptr_t return_address, const std::vector<std::pair<uintptr_t, uintptr_t>>& mod_ranges);

}  // namespace emf::core
