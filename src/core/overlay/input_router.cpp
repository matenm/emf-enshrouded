#include <string>
#include <cstdio>
#include "input_router.h"

namespace emf::core {

Route InputRouter::key(const KeyEvent& e) {
    const Route r = decide(e);
    const size_t i = index(e);
    if (!e.down && e.vkey) hidden_from_mods(e.vkey, false);
    passes_[i] = r == Route::Game || r == Route::Both;
    if (!e.down)
        game_down_[i] = false;
    else if (passes_[i])
        game_down_[i] = true;  // an auto-repeat kept from the game does not clear it
    return r;
}

Route InputRouter::decide(const KeyEvent& e) {
    const bool toggle = e.scan == toggle_ && !e.extended;
    const bool esc = e.scan == kEscScan && !e.extended;
    const bool held = held_ != 0 && e.scan == held_ && !e.extended;
    if (wheel_ && !open_) {  // a wheel captures: no console toggling, the overlay sees every key
        if (e.down) {
            if (held) return Route::Swallow;  // auto-repeat of the key that last toggled the console
            return Route::Console;
        }
        if (held) {
            held_ = 0;
            return Route::Swallow;
        }
        return game_down_[index(e)] ? Route::Both : Route::Console;  // the game gets the releases of keys it saw
    }
    if (window_ && !open_) {  // a mod's window captures; the console key still opens the console over it
        if (e.down) {
            if (held) return Route::Swallow;  // auto-repeat of the key that toggled or closed
            // an auto-repeat of a key held since before the window opened (the key that opened it): not a press; its
            // release reaches the game, which saw it go down
            if (game_down_[index(e)]) return Route::Swallow;
            const uint16_t skip = window_skip_vk_.load();
            if (skip && e.vkey == skip) {  // the press that opened the window, handled here only now
                window_skip_vk_ = 0;
                held_ = e.scan;  // its repeats and its release go nowhere
                return Route::Swallow;
            }
            const uint16_t close_vk = window_close_vk_.load();
            if (esc || (close_vk && e.vkey == close_vk)) {
                held_ = e.scan;  // its repeats and its release go nowhere
                // still down when the window has closed: a mod polling it (the one that opens the window on that
                // key) must not see a new press, so it reads as up until released
                if (!esc) hidden_vk_ = close_vk;
                return Route::CloseWindow;
            }
            if (toggle) {
                open_ = true;
                esc_to_console_ = false;
                swallow_char_ = true;
                held_ = e.scan;
                return Route::Toggle;
            }
            return Route::Console;
        }
        if (e.vkey && e.vkey == window_skip_vk_.load()) window_skip_vk_ = 0;  // released: its next press closes
        if (held) {
            held_ = 0;
            return Route::Swallow;
        }
        return game_down_[index(e)] ? Route::Both : Route::Console;
    }
    if (e.down) {
        if (held) {  // auto-repeat of the key that toggled: one press, one toggle
            swallow_char_ = true;  // and the repeat's character is not typed
            return Route::Swallow;
        }
        if (esc && open_ && !toggle && esc_to_console_.load()) {  // Esc hides the console's suggestions first
            swallow_char_ = true;  // its character (0x1B) is not typed
            held_ = e.scan;        // and its auto-repeats do not close the console after the list hid
            return Route::Console;
        }
        if (toggle && !open_) {
            if (bool (*typing)() = game_text_.load(); typing && typing()) {  // the game's chat has the keys: type it there
                swallow_char_ = false;
                return Route::Game;
            }
        }
        if (toggle || (open_ && esc)) {
            open_ = !open_.load();
            esc_to_console_ = false;  // until the console says again that its suggestions show
            swallow_char_ = true;
            held_ = e.scan;
            return Route::Toggle;
        }
        swallow_char_ = false;  // any other key: a character after this one is that key's, not the toggle's
        return open_ ? Route::Console : Route::Game;
    }
    if (held) held_ = 0;
    if (toggle && !held && game_down_[index(e)]) return Route::Game;  // it typed into the game's chat
    if (toggle || held) return Route::Swallow;  // the game never saw these go down
    return open_ ? Route::Both : Route::Game;
}

bool InputRouter::hidden_from_mods(int vk, bool down) {
    uint16_t h = hidden_vk_.load();
    if (h == 0 || vk != h) return false;
    if (down) return true;
    hidden_vk_.compare_exchange_strong(h, 0);
    return false;
}

Route InputRouter::legacy_key(const KeyEvent& e) {
    if (capturing() && e.down) return Route::Swallow;
    return passes_[index(e)] ? Route::Game : Route::Swallow;
}

Route InputRouter::character(wchar_t) {
    if ((wheel_ || window_) && !open_) return Route::Swallow;  // nothing types while a wheel or a window is open
    if (swallow_char_) {
        swallow_char_ = false;
        return Route::Swallow;
    }
    return open_ ? Route::Console : Route::Game;
}

void HeldKeys::track(UINT vk, const KeyEvent& e) {
    const BYTE state = e.down ? 0x80 : 0;
    if (vk < 256) keys_[vk] = state;
    switch (vk) {  // keep the generic and the left/right variants in step
        case VK_SHIFT: keys_[e.scan == 0x36 ? VK_RSHIFT : VK_LSHIFT] = state; break;
        case VK_CONTROL: keys_[e.extended ? VK_RCONTROL : VK_LCONTROL] = state; break;
        case VK_MENU: keys_[e.extended ? VK_RMENU : VK_LMENU] = state; break;
        default: break;
    }
    keys_[VK_SHIFT] = (keys_[VK_LSHIFT] | keys_[VK_RSHIFT]) & 0x80;
    keys_[VK_CONTROL] = (keys_[VK_LCONTROL] | keys_[VK_RCONTROL]) & 0x80;
    keys_[VK_MENU] = (keys_[VK_LMENU] | keys_[VK_RMENU]) & 0x80;
}

void HeldKeys::clear() {
    for (BYTE& k : keys_) k = 0;
}

void InputRouter::focus_lost() {
    held_ = 0;
    swallow_char_ = false;
}

Route InputRouter::mouse(MouseKind kind) const {
    if (!capturing()) return Route::Game;
    switch (kind) {
        case MouseKind::Wheel: return Route::Console;
        case MouseKind::ButtonUp: return Route::Game;
        default: return Route::Swallow;
    }
}

void flush_dead_key(ToUnicodeFn to_unicode, UINT vk, UINT scan, HKL layout) {
    // A pending dead key combines with a second press of itself and is gone; a key that is dead but was not pending
    // gets stored by the first call and cleared by the second. An ordinary key returns a character at once.
    const BYTE state[256] = {};
    wchar_t buf[8];
    for (int i = 0; i < 3; ++i)
        if (to_unicode(vk, scan, state, buf, 8, 0, layout) >= 0) break;
}

bool decode_raw_keyboard(const RAWKEYBOARD& rk, KeyEvent* out) {
    if (!out || rk.VKey == 0xFF) return false;
    out->scan = rk.MakeCode;
    out->vkey = rk.VKey;
    out->down = (rk.Flags & RI_KEY_BREAK) == 0;
    out->extended = (rk.Flags & RI_KEY_E0) != 0;
    return true;
}

bool caller_is_mod(uintptr_t return_address, const std::vector<std::pair<uintptr_t, uintptr_t>>& mod_ranges) {
    for (const auto& [begin, end] : mod_ranges)
        if (return_address >= begin && return_address < end) return true;
    return false;
}


std::wstring fallback_chars(uint32_t mapped, bool shift, bool caps, bool alt) {
    if (alt || (mapped & 0x80000000u)) return {};  // AltGr or Alt: the layout's own character; a dead key
    const wchar_t ch = static_cast<wchar_t>(mapped & 0xFFFF);
    if (ch < 0x20) return {};
    if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z')) {
        const wchar_t lower = static_cast<wchar_t>(ch | 0x20);
        return std::wstring(1, (shift != caps) ? static_cast<wchar_t>(lower - 0x20) : lower);
    }
    if (ch == L' ') return L" ";
    if (shift || caps) return {};  // the layout's own character, not guessed
    if (IsCharAlphaW(ch)) {  // another script's letter: the small one (VK_TO_CHAR gives the capital)
        wchar_t c = ch;
        CharLowerBuffW(&c, 1);
        return std::wstring(1, c);
    }
    return std::wstring(1, ch);
}

namespace {
// Text for a log line: printable ASCII as it is, anything else as a backslash-u escape.
std::string shown(const std::wstring& s) {
    std::string out;
    char b[8];
    for (wchar_t ch : s) {
        if (ch >= 0x20 && ch < 0x7F && ch != L'"' && ch != L'\\') {
            out += static_cast<char>(ch);
        } else {
            std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(ch));
            out += b;
        }
    }
    return out;
}
std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%02X", v);
    return b;
}
}  // namespace

std::string KeyDebugLog::opened() {
    if (!on()) return {};
    return "overlay keys: the console opened; logging its first " + std::to_string(max_) +
           " key presses ([overlay] debug_keys=1)";
}

std::string KeyDebugLog::press(std::string line) {
    if (!on()) return {};
    if (++presses_ > max_) {
        done_ = true;
        return "overlay keys: " + std::to_string(max_) + " key presses logged; no more until the game starts again";
    }
    return line;
}

std::string KeyDebugLog::raw_key(const RawKeyInfo& k) {
    if (!on()) return {};
    std::string held;
    for (const auto& [is, name] : {std::pair<bool, const char*>{k.shift, "Shift"}, {k.ctrl, "Ctrl"}, {k.alt, "Alt"},
                                    {k.caps, "Caps"}})
        if (is) held += (held.empty() ? "" : " ") + std::string(name);
    std::string line = "overlay keys: raw key " + hex(k.scan) + " (vk " + hex(k.vk) + (held.empty() ? "" : ", held: " + held) + "): ";
    if (k.shortcut) return press(line + "a shortcut (Ctrl held), not typed");
    line += "ToUnicodeEx " + std::to_string(k.n);
    if (k.n < 0) line += " (a dead key)";
    else if (!k.proton.empty()) line += " \"" + shown(k.proton) + "\"";
    else if (!k.fallback.empty()) line += ", the key's own letter \"" + shown(k.fallback) + "\"";
    line += "; typed now: ";
    if (!k.typed.empty()) line += "\"" + shown(k.typed) + "\"";
    else line += k.waits ? "nothing (held for a character message)" : "nothing";
    return press(line);
}

std::string KeyDebugLog::key_message(uint16_t scan, uint32_t vk, bool counts) {
    if (!on()) return {};
    const std::string line = "overlay keys: key message " + hex(scan) + " (vk " + hex(vk) + ")";
    return counts ? press(line) : line;
}

std::string KeyDebugLog::character(wchar_t ch, bool to_console) {
    if (!on()) return {};
    char b[16];
    std::snprintf(b, sizeof b, "U+%04X", static_cast<unsigned>(ch));
    return "overlay keys: character message \"" + shown(std::wstring(1, ch)) + "\" (" + b + "): " +
           (to_console ? "to the console" : "not for the console");
}

}  // namespace emf::core
