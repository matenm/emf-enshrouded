#pragma once
// A mod's key read (GetAsyncKeyState / GetKeyState from a mod's code), after the bridge's injected keys: the typing gate
// (game_screen.h) and the input router decide whether the key reads as up. input_hook.cpp's two hooks call it; tests
// give a stand-in router.
#include <windows.h>

#include "core/game_screen.h"

namespace emf::core {

// While the player types in the game's text box (`typing`) or the gate still holds a key up for mods, and while the
// router captures the keys (the console or a mod window has them) or hides some from mods, a mod's read may say the key
// is up (0). `read(vk)` reads the real state, at most once (its low bit, "pressed since the last call", is used up by a
// read); `from_mod()` says whether the caller is a mod, asked only when something filters. Router: capturing(),
// hiding(), hidden_from_mods(vk, down); may be null.
template <class Router, class Read, class FromMod>
SHORT filtered_key_state(int vk, bool typing, TypingKeyGate& gate, Router* router, Read&& read, FromMod&& from_mod) {
    const bool gated = typing || gate.any();
    const bool routed = router && (router->capturing() || router->hiding());
    if (!(gated || routed) || !from_mod()) return read(vk);
    if (routed && router->capturing()) return 0;
    const SHORT s = read(vk);
    const bool down = (s & 0x8000) != 0;
    if (gated && gate.hide(vk, down, typing)) return 0;
    if (routed && router->hidden_from_mods(vk, down)) return 0;
    return s;
}

}  // namespace emf::core
