#pragma once
// big_stacks: the stack size rules, kept free of the game so they can be tested.
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace big_stacks {

// New maximum stack size: original x multiplier, at most `cap` (and 65535), never below the original. Items that do
// not stack (0 or 1) stay as they are.
inline uint16_t big_stack(uint16_t original, double multiplier, uint16_t cap) {
    if (original <= 1) return original;
    const double v = std::floor(original * multiplier + 0.5);
    const double limit = cap < 65535 ? cap : 65535;
    const double out = v > limit ? limit : v;
    return out <= original ? original : static_cast<uint16_t>(out);
}

// The mod works on the host's server world only: one thread (no locking needed), and set_item_max_stack updates the
// client world too. In someone else's game (no server world) it changes nothing.
inline bool processes_world(uint8_t kind) { return kind == 0; /* EMF_WORLD_SERVER */ }

// Remembers each item's original stack size. decide() returns the size to set, or 0 for "leave it": an item is raised
// only while it still has its original size (first sight, or after the game reloaded its data), so a second pass does
// not multiply again and a size someone else set (the stack command, another mod) is left alone.
class Originals {
public:
    uint16_t decide(uint32_t id, uint16_t current, double multiplier, uint16_t cap) {
        auto it = orig_.find(id);
        if (it == orig_.end()) it = orig_.emplace(id, current).first;
        if (current != it->second) return 0;
        const uint16_t target = big_stack(it->second, multiplier, cap);
        return target != current ? target : 0;
    }
    size_t size() const { return orig_.size(); }

private:
    std::unordered_map<uint32_t, uint16_t> orig_;
};

}  // namespace big_stacks
