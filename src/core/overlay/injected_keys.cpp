#include "injected_keys.h"

#include "common/srw.h"

namespace emf::core {

bool InjectedKeys::add(uint32_t vk, uint64_t until) {
    if (vk == 0 || vk > 0xFE) return false;
    const SrwExclusive lock(lock_);
    for (const auto& k : keys_)
        if (k.vk == vk) return false;
    keys_.push_back(Key{vk, until, 0});
    active_.store(static_cast<int>(keys_.size()), std::memory_order_release);
    return true;
}

bool InjectedKeys::press(uint32_t vk, uint64_t now_ms, uint32_t hold_ms) { return add(vk, now_ms + hold_ms); }

bool InjectedKeys::down(uint32_t vk, uint64_t now_ms) { return add(vk, now_ms + kMaxDownMs); }

uint32_t InjectedKeys::up(uint32_t vk) {
    const SrwExclusive lock(lock_);
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i].vk != vk) continue;
        const uint32_t reads = keys_[i].reads;
        keys_.erase(keys_.begin() + static_cast<std::ptrdiff_t>(i));
        active_.store(static_cast<int>(keys_.size()), std::memory_order_release);
        return reads;
    }
    return 0;
}

std::vector<std::pair<uint32_t, uint32_t>> InjectedKeys::expire(uint64_t now_ms) {
    std::vector<std::pair<uint32_t, uint32_t>> ended;
    const SrwExclusive lock(lock_);
    for (size_t i = 0; i < keys_.size();) {
        if (now_ms < keys_[i].until) {
            ++i;
            continue;
        }
        ended.emplace_back(keys_[i].vk, keys_[i].reads);
        keys_.erase(keys_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    active_.store(static_cast<int>(keys_.size()), std::memory_order_release);
    return ended;
}

uint32_t InjectedKeys::read(uint32_t vk, uint64_t now_ms) {
    const SrwExclusive lock(lock_);
    for (auto& k : keys_)
        if (k.vk == vk && now_ms < k.until) return ++k.reads;
    return 0;
}

bool InjectedKeys::any_held(uint64_t now_ms) const {
    if (!any()) return false;
    const SrwShared lock(lock_);
    for (const auto& k : keys_)
        if (now_ms < k.until) return true;
    return false;
}

bool InjectedKeys::held(uint32_t vk, uint64_t now_ms) const {
    const SrwShared lock(lock_);
    for (const auto& k : keys_)
        if (k.vk == vk && now_ms < k.until) return true;
    return false;
}

std::optional<SHORT> injected_key_state(InjectedKeys* keys, int vk, bool caller_is_mod, uint64_t now_ms, bool async,
                                        bool game_really_in_front) {
    if (!keys || !keys->any() || !caller_is_mod || vk <= 0) return std::nullopt;
    if (const uint32_t reads = keys->read(static_cast<uint32_t>(vk), now_ms))
        return static_cast<SHORT>(async && reads == 1 ? 0x8001 : 0x8000);
    if (!game_really_in_front && keys->any_held(now_ms)) return static_cast<SHORT>(0);  // spoofed focus: no real keys
    return std::nullopt;
}

}  // namespace emf::core
