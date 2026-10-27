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
    if (e.down) {
        if (held) {  // auto-repeat of the key that toggled: one press, one toggle
            swallow_char_ = true;  // and the repeat's character is not typed
            return Route::Swallow;
        }
        if (toggle || (open_ && esc)) {
            open_ = !open_.load();
            swallow_char_ = true;
            held_ = e.scan;
            return Route::Toggle;
        }
        swallow_char_ = false;  // any other key: a character after this one is that key's, not the toggle's
        return open_ ? Route::Console : Route::Game;
    }
    if (held) held_ = 0;
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
    if (wheel_ && !open_) return Route::Swallow;  // nothing types while a wheel is open
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

}  // namespace emf::core
