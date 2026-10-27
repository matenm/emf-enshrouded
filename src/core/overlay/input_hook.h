#pragma once
// Puts the in-game console between the player's keyboard/mouse and the game: subclasses the game window's message
// procedure (raw input and legacy messages) and hides key state from mods' GetAsyncKeyState/GetKeyState while the
// console or a wheel holds the input. Applies InputRouter's decisions.
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "input_router.h"

namespace emf::core {

struct InputCallbacks {
    std::function<void(const KeyEvent&)> console_key;  // presses (and releases) for the console
    std::function<void(wchar_t)> console_char;         // typed characters for the console
    std::function<void(float)> console_wheel;          // wheel notches, + = away from the user
    std::function<void(bool open)> toggled;            // the console opened or closed
    // While a wheel captures (InputRouter::wheel): raw mouse movement, RI_MOUSE_* button flags and wheel notches.
    std::function<void(float dx, float dy, uint16_t buttons, float wheel)> overlay_mouse;
    std::function<void()> focus_lost;  // the game window lost the keyboard (alt-tab, another window)
};

// Starts a thread that waits for the game window (class "localWindowClass" of this process) and subclasses it, and
// hooks GetAsyncKeyState/GetKeyState. `router` must live for the process. False if the key-state hooks failed.
bool install_input_hooks(InputRouter* router, InputCallbacks callbacks);

// The game window is subclassed (keys, characters and mouse reach the overlay).
bool window_hooked();

// The loaded mods' image ranges [begin, end): their key-state calls see "not pressed" while the console is open.
void set_mod_ranges(std::vector<std::pair<uintptr_t, uintptr_t>> ranges);

class InjectedKeys;
enum class InjectedKeyHooks {
    None,       // the key-state hooks failed: mod_key cannot work
    NeedFocus,  // mods see the keys, but only while the game window is in front (GetForegroundWindow not hooked)
    Full,       // mods see the keys and the game window in front while one is held
};
// The debug bridge's keys for mods (mod_key, injected_keys.h): hooks GetAsyncKeyState/GetKeyState if the overlay did
// not, and GetForegroundWindow. `keys` must live for the process.
InjectedKeyHooks install_injected_keys(InjectedKeys* keys);

}  // namespace emf::core
