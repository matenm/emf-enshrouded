#include "keen_map.h"

#include <cstring>

namespace emf {
namespace {
template <class T>
T rd(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }
}  // namespace

uint64_t KeenMapView::capacity() const { return rd<uint64_t>(map_ + 0x10); }
uint32_t KeenMapView::count() const { return rd<uint32_t>(map_ + 0x54); }

bool KeenMapView::occupied(size_t slot) const {
    auto bits = rd<const uint64_t*>(map_ + 0x08);
    return (bits[slot >> 6] >> (slot & 63)) & 1;
}

uint32_t KeenMapView::key(size_t slot) const { return rd<const uint32_t*>(map_ + 0x28)[slot]; }

const uint8_t* KeenMapView::value_ptr(size_t slot, size_t stride) const {
    return rd<const uint8_t*>(map_ + 0x40) + slot * stride;
}

uint32_t KeenMapView::mix(uint32_t x) {
    x = ((x >> 16) ^ x) * 0x45d9f3bu;
    x = ((x >> 16) ^ x) * 0x45d9f3bu;
    return (x >> 16) ^ x;
}

std::optional<size_t> KeenMapView::find(uint32_t k, bool use_mix) const {
    const uint64_t cap = capacity();
    if (count() == 0 || cap == 0) return std::nullopt;
    size_t slot = (use_mix ? mix(k) : k) & (cap - 1);
    for (uint64_t probes = 0; probes < cap && occupied(slot); ++probes) {
        if (key(slot) == k) return slot;
        slot = (slot + 1 < cap) ? slot + 1 : 0;
    }
    return std::nullopt;
}

}  // namespace emf
