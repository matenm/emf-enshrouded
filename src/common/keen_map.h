#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

namespace emf {

// Read-only view of Keen's open-addressing hash map (u32 keys, linear probing).
// Layout: +0x08 u64* occupied bitset, +0x10 u64 capacity (power of 2),
//         +0x28 u32* keys, +0x40 values, +0x54 u32 count.
class KeenMapView {
public:
    explicit KeenMapView(const uint8_t* map) : map_(map) {}

    uint64_t capacity() const;
    uint32_t count() const;
    bool occupied(size_t slot) const;
    uint32_t key(size_t slot) const;
    // Pointer to the value of `slot` when values are `stride` bytes apart.
    const uint8_t* value_ptr(size_t slot, size_t stride) const;
    template <class V>
    const V* value(size_t slot) const { return reinterpret_cast<const V*>(value_ptr(slot, sizeof(V))); }

    // Slot holding `key`, or nullopt. `mix` = entity-id style key mixing.
    std::optional<size_t> find(uint32_t key, bool mix) const;

    // The engine's entity-id mix: x=((x>>16)^x)*0x45d9f3b twice, then (x>>16)^x.
    static uint32_t mix(uint32_t x);

private:
    const uint8_t* map_;
};

}  // namespace emf
