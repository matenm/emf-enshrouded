#pragma once
// Keys the debug bridge presses for mods (mod_key). While one is held, mods' GetAsyncKeyState/GetKeyState calls see it
// down and their GetForegroundWindow calls see the game window (input_hook.cpp), so emf::keys::down() works without
// window focus. The game, the console and wheels never see these keys. Thread-safe; the hooks check any() first, one
// atomic load, so nothing changes while no key is held.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace emf::core {

class InjectedKeys {
public:
    static constexpr uint32_t kMaxDownMs = 60000;  // a "down" without "up" is released after this

    // Down now, up at now + hold_ms. False for vk 0 or above 0xFE, or a key already held.
    bool press(uint32_t vk, uint64_t now_ms, uint32_t hold_ms);
    // Down until up() (at most kMaxDownMs). False like press.
    bool down(uint32_t vk, uint64_t now_ms);
    // Releases vk; returns how often mods read it while it was held (0 if it was not held).
    uint32_t up(uint32_t vk);
    // Releases what is due; returns (vk, mod reads) for each key released.
    std::vector<std::pair<uint32_t, uint32_t>> expire(uint64_t now_ms);
    bool any() const { return active_.load(std::memory_order_acquire) != 0; }
    // Some key is held right now (its time not up): only then does the game window count as in front for mods.
    bool any_held(uint64_t now_ms) const;
    // A mod reads vk: 0 when it is not held, else how often mods read this press so far (1 = the first read).
    uint32_t read(uint32_t vk, uint64_t now_ms);
    bool held(uint32_t vk, uint64_t now_ms) const;

private:
    struct Key {
        uint32_t vk;
        uint64_t until;
        uint32_t reads;
    };
    bool add(uint32_t vk, uint64_t until);
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<Key> keys_;
    std::atomic<int> active_{0};
};

// What a hooked key-state call from a mod returns while the bridge holds keys: for an injected key, down
// (GetAsyncKeyState: also "pressed since the last call" on the press's first read); for any other key while a key is
// held and the game is not really in front (mods are told it is), up, so real keys typed into another window do not
// reach mods. nullopt: go on as usual (nothing held, not a mod, or the game really is in front).
std::optional<SHORT> injected_key_state(InjectedKeys* keys, int vk, bool caller_is_mod, uint64_t now_ms, bool async,
                                        bool game_really_in_front);

}  // namespace emf::core
