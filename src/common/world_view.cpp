#include "world_view.h"

#include <windows.h>

#include "fnv.h"
#include "offsets.h"
#include "seh.h"

namespace emf {
namespace {

template <class T>
T rd(const uint8_t* addr, T fallback = T{}) {
    T v;
    return seh_read(addr, &v, sizeof(T)) ? v : fallback;
}

// Header of a Keen map, read safely. Returns false if it looks unusable.
struct MapHeader {
    const uint8_t* bits = nullptr;
    uint64_t capacity = 0;
    const uint8_t* keys = nullptr;
    const uint8_t* values = nullptr;
    uint32_t count = 0;
};

bool read_map(const uint8_t* map, MapHeader& h) {
    h.bits = rd<const uint8_t*>(map + off::kMapBits);
    h.capacity = rd<uint64_t>(map + off::kMapCapacity);
    h.keys = rd<const uint8_t*>(map + off::kMapKeys);
    h.values = rd<const uint8_t*>(map + off::kMapValues);
    h.count = rd<uint32_t>(map + off::kMapCount);
    const bool pow2 = h.capacity && (h.capacity & (h.capacity - 1)) == 0;
    return h.count && pow2 && h.capacity <= off::kMapMaxCapacity && h.bits && h.keys;
}

bool slot_occupied(const MapHeader& h, uint64_t slot) {
    uint64_t word = 0;
    if (!seh_read(h.bits + (slot >> 6) * 8, &word, 8)) return false;
    return (word >> (slot & 63)) & 1;
}

bool call_guarded(GetComponentFn fn, uint8_t* world, uint32_t hash, uint32_t entity, uint64_t* out) {
    __try {
        fn(out, world, hash, entity);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* call_guarded_tpl(GetTemplateComponentFn fn, uint8_t* world, uint32_t hash, uint32_t entity) {
    __try {
        return fn(world, hash, entity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

}  // namespace

WorldView WorldView::from_storage(void* storage) {
    return WorldView(static_cast<uint8_t*>(storage) - off::kStorage);
}

uint8_t WorldView::state() const { return rd<uint8_t>(world_ + off::kState); }
uint8_t WorldView::kind() const { return rd<uint8_t>(world_ + off::kKind); }
uint32_t WorldView::tick() const { return rd<uint32_t>(world_ + off::kTick); }
int64_t WorldView::tick_length_ns() const { return rd<int64_t>(world_ + off::kTickLengthNs); }
uint32_t WorldView::entity_count() const { return rd<uint32_t>(world_ + off::kEntityMap + off::kMapCount); }
uint64_t WorldView::singleton_count() const { return rd<uint64_t>(world_ + off::kSingletonCount); }

std::optional<uint16_t> WorldView::map_u16(uint32_t map_offset, uint32_t key) const {
    MapHeader h;
    if (!world_ || !read_map(world_ + map_offset, h) || !h.values) return std::nullopt;
    uint64_t slot = key & (h.capacity - 1);
    for (uint64_t probes = 0; probes < h.capacity && slot_occupied(h, slot); ++probes) {
        if (rd<uint32_t>(h.keys + slot * 4) == key) {
            uint16_t v = 0;
            if (!seh_read(h.values + slot * 2, &v, 2)) return std::nullopt;
            return v;
        }
        slot = (slot + 1 < h.capacity) ? slot + 1 : 0;
    }
    return std::nullopt;
}

std::optional<bool> WorldView::has_dynamic_component(uint32_t hash) const {
    if (!world_) return std::nullopt;
    const uint8_t* reg = rd<const uint8_t*>(world_ + off::kStorage);
    if (!reg) return std::nullopt;
    MapHeader h;
    if (!read_map(reg + off::kRegistryDynamicMap, h) || !h.keys) return std::nullopt;
    uint64_t slot = hash & (h.capacity - 1);
    for (uint64_t probes = 0; probes < h.capacity && slot_occupied(h, slot); ++probes) {
        if (rd<uint32_t>(h.keys + slot * 4) == hash) return true;
        slot = (slot + 1 < h.capacity) ? slot + 1 : 0;
    }
    return false;
}

std::optional<uint32_t> WorldView::template_component_size(uint32_t hash) const {
    if (!world_) return std::nullopt;
    const uint8_t* reg = rd<const uint8_t*>(world_ + off::kStorage);
    if (!reg) return std::nullopt;
    MapHeader h;
    if (!read_map(reg + off::kRegistryTemplateMap, h) || !h.values) return std::nullopt;
    std::optional<uint16_t> index;
    uint64_t slot = hash & (h.capacity - 1);
    for (uint64_t probes = 0; probes < h.capacity && slot_occupied(h, slot); ++probes) {
        if (rd<uint32_t>(h.keys + slot * 4) == hash) {
            uint16_t v = 0;
            if (seh_read(h.values + slot * 2, &v, 2)) index = v;
            break;
        }
        slot = (slot + 1 < h.capacity) ? slot + 1 : 0;
    }
    if (!index) return std::nullopt;
    const uint8_t* entries = rd<const uint8_t*>(reg + off::kRegistryTypes);
    const uint64_t count = rd<uint64_t>(reg + off::kRegistryTypeCount);
    if (!entries || *index >= count) return std::nullopt;
    const uint8_t* desc = rd<const uint8_t*>(entries + uint64_t(*index) * off::kComponentTypeStride + off::kComponentTypeDesc);
    if (!desc || rd<uint32_t>(desc + off::kTypeDescHash) != hash) return std::nullopt;
    const uint32_t size = rd<uint32_t>(desc + off::kTypeDescSize);
    return size ? std::optional<uint32_t>(size) : std::nullopt;
}

void* WorldView::singleton(std::string_view name) const {
    const uint32_t hash = fnv1a32(name);
    const auto index = map_u16(off::kSingletonMap, hash);
    if (!index || *index >= singleton_count()) return nullptr;
    const uint8_t* entries = rd<const uint8_t*>(world_ + off::kSingletonArray);
    if (!entries) return nullptr;
    const uint8_t* entry = entries + uint64_t(*index) * off::kSingletonEntrySize;
    if (rd<uint32_t>(entry + off::kSingletonEntryHash) != hash) return nullptr;
    return rd<void*>(entry + off::kSingletonEntryObj);
}

std::vector<uint32_t> WorldView::entity_ids() const {
    std::vector<uint32_t> ids;
    MapHeader h;
    if (!read_map(world_ + off::kEntityMap, h)) return ids;
    std::vector<uint64_t> bits((h.capacity + 63) / 64);
    std::vector<uint32_t> keys(h.capacity);
    if (!seh_read(h.bits, bits.data(), bits.size() * 8)) return ids;
    if (!seh_read(h.keys, keys.data(), keys.size() * 4)) return ids;
    ids.reserve(h.count);
    for (uint64_t slot = 0; slot < h.capacity; ++slot)
        if ((bits[slot >> 6] >> (slot & 63)) & 1) ids.push_back(keys[slot]);
    return ids;
}

ComponentRef get_component(GetComponentFn fn, uint8_t* world, uint32_t hash, uint32_t entity) {
    if (!fn) return {};
    uint64_t out[2] = {0, 0};
    if (!call_guarded(fn, world, hash, entity, out)) return {};
    return {reinterpret_cast<void*>(out[0]), out[1]};
}

void* get_template_component(GetTemplateComponentFn fn, uint8_t* world, uint32_t hash, uint32_t entity) {
    if (!fn || !world) return nullptr;
    return call_guarded_tpl(fn, world, hash, entity);
}

}  // namespace emf
