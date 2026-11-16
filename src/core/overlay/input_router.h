#pragma once
// Which keyboard/mouse input goes to the in-game console and which to the game. Pure logic (unit-tested); the window
// hook in input_hook.cpp applies it.
#include <windows.h>

#include <string>

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
    CloseWindow,  // Esc or the close key of a mod's window (API v24): the overlay closes it; nobody else sees it
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
    void set_open(bool open) {
        open_ = open;
        esc_to_console_ = false;
    }
    // A mod's radial wheel captures the input (the console stays closed): keys go to the overlay, releases of keys
    // the game saw go down reach the game too, the mouse is kept from the game.
    bool wheel() const { return wheel_.load(); }
    void set_wheel(bool on) { wheel_ = on; }
    // A mod's window captures the input like a wheel; Esc and its close key (a generic virtual-key code, 0 = none)
    // close it on their press; the console key opens the console over it. `close_key_down`: the close key was down
    // when it opened (the key that opened it): that press, if the window thread had not handled it yet, does not close
    // it; the key's release ends this.
    bool window() const { return window_.load(); }
    void set_window(bool on, uint16_t close_vk = 0, bool close_key_down = false) {
        window_close_vk_ = on ? close_vk : 0;
        window_skip_vk_ = on && close_key_down ? close_vk : 0;
        window_ = on;
    }
    // The console, a wheel or a mod's window holds the input (the game gets no presses and no mouse movement).
    bool capturing() const { return open_.load() || wheel_.load() || window_.load(); }
    // While the console's suggestions show, Esc goes to the console (which hides them) instead of closing it. Set by
    // the render thread each frame the console draws; opening or closing the console clears it.
    void set_esc_to_console(bool on) { esc_to_console_ = on; }
    bool esc_to_console() const { return esc_to_console_.load(); }
    // Asked when the console key is pressed while the console is closed: true while the game's own text box (its chat)
    // has the keys, so the key types its character there instead of opening the console. Null: never.
    void set_game_text(bool (*typing)()) { game_text_ = typing; }
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
    std::atomic<bool> window_{false};
    std::atomic<uint16_t> window_close_vk_{0};
    std::atomic<uint16_t> window_skip_vk_{0};  // the close key's press that opened the window (see set_window)
    std::atomic<bool> esc_to_console_{false};
    std::atomic<uint16_t> hidden_vk_{0};
    std::atomic<bool (*)()> game_text_{nullptr};
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

// The characters for a key when ToUnicodeEx gave none (seen possible under Proton: a Linux player's console stayed
// empty): `mapped` is MapVirtualKeyEx(vk, MAPVK_VK_TO_CHAR), the character printed on the key. An English letter comes in
// the case Shift and Caps Lock make; any other letter, a digit or a symbol only without Shift and Caps Lock (they give
// the layout's own character, often not the capital: French Shift+e-acute is 2); nothing with Alt or AltGr held (AltGr+Q
// is @ on a German keyboard), for a dead key (the high bit), a control character or 0.
std::wstring fallback_chars(uint32_t mapped, bool shift, bool caps, bool alt = false);

// One raw key press for the console, for KeyDebugLog.
struct RawKeyInfo {
    uint16_t scan = 0;
    uint32_t vk = 0;
    bool shift = false, ctrl = false, alt = false, caps = false;  // held (Caps Lock: on)
    bool shortcut = false;  // Ctrl held without Alt: a shortcut, not typed
    int n = 0;              // ToUnicodeEx's result (< 0: a dead key)
    std::wstring proton, fallback, typed;  // ToUnicodeEx's characters, fallback_chars' when it gave none, typed now
    bool waits = false;     // nothing typed now: the characters are held for a character message
};

// [overlay] debug_keys=1: one log line per key event while the console is open, for the first `max` key presses of a game
// start (a raw key press, or a key message where no raw input comes), so a player's report shows where typed characters
// get lost. Off by default (what is typed must not end up in logs). Window thread only.
class KeyDebugLog {
public:
    explicit KeyDebugLog(size_t max = 20) : max_(max) {}
    void enable(bool on) { on_ = on; }
    bool on() const { return on_ && !done_; }
    std::string opened();
    std::string raw_key(const RawKeyInfo& k);  // a raw key press for the console (not its repeats)
    std::string key_message(uint16_t scan, uint32_t vk, bool counts = false);  // counts: no raw input, this is the press
    std::string character(wchar_t ch, bool to_console);

private:
    std::string press(std::string line);  // counts a press: the line, the last line, or "" once done
    size_t max_, presses_ = 0;
    bool on_ = false, done_ = false;
};

// Where the console's characters come from. With raw keys only, from raw input (ToUnicodeEx). Once the window also gets
// key messages, from its character messages (WM_CHAR, made by the game's TranslateMessage), as on Windows. Under Wine the
// key messages come but the character messages may not: after kMisses console key presses in a row whose characters
// came only from raw input, raw input types them from then on, the ones held while deciding included. Window thread only.
class CharSource {
public:
    static constexpr int kMisses = 3;
    void legacy_seen() {  // a key message (WM_KEYDOWN) arrived
        if (!switched_) from_raw_ = false;
    }
    bool from_raw() const { return from_raw_; }
    // A raw key press for the console and the characters ToUnicodeEx gives for it: what to type now. Control
    // characters (Enter's \r, Backspace's \b, Tab) are not typed text and do not count.
    std::wstring raw_down(uint16_t scan, const std::wstring& chars) {
        std::wstring text;
        for (wchar_t ch : chars)
            if (ch >= 0x20) text += ch;
        if (from_raw_) return text;
        if (text.empty()) return {};
        for (auto& p : pending_)
            if (p.first == scan) {  // its key repeats: the last characters stand
                p.second = text;
                return {};
            }
        pending_.push_back({scan, text});
        return {};
    }
    // A character message arrived from the window (for the console or not): the window makes them, nothing raw is held.
    void wm_char() {
        if (from_raw_) return;
        pending_.clear();
        misses_ = 0;
        held_.clear();
    }
    // The raw release of a key: what to type now (the held characters, when this press decides for raw input).
    std::wstring raw_up(uint16_t scan) {
        if (from_raw_) return {};
        for (size_t i = 0; i < pending_.size(); ++i) {
            if (pending_[i].first != scan) continue;
            held_ += pending_[i].second;
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            if (++misses_ < kMisses) return {};
            from_raw_ = switched_ = true;
            std::wstring out;
            out.swap(held_);
            for (const auto& p : pending_) out += p.second;  // keys still down type too
            pending_.clear();
            return out;
        }
        return {};
    }
    // The console closed: what was held while deciding is dropped, and the count starts again.
    void closed() {
        if (from_raw_) return;
        pending_.clear();
        held_.clear();
        misses_ = 0;
    }

private:
    bool from_raw_ = true, switched_ = false;
    std::vector<std::pair<uint16_t, std::wstring>> pending_;
    std::wstring held_;
    int misses_ = 0;
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
