#include "core/player_strings.h"

#include "common/fnv.h"
#include "common/offsets.h"
#include "common/seh.h"
#include "core/manifest.h"

namespace emf::core {

namespace {

template <class T>
bool rd(const uint8_t* addr, T* out) {
    return seh_read(addr, out, sizeof(T));
}

bool slot_occupied(const uint8_t* bits, uint64_t slot) {
    uint64_t word = 0;
    if (!rd(bits + (slot >> 6) * 8, &word)) return false;
    return (word >> (slot & 63)) & 1;
}

}  // namespace

std::optional<std::string> custom_player_string(const uint8_t* system, uint64_t id) {
    if (!system || id == 0) return std::nullopt;
    const uint8_t* map = system + kCustomStringsMap;
    const uint8_t *bits = nullptr, *keys = nullptr, *values = nullptr;
    uint64_t capacity = 0;
    uint32_t count = 0;
    if (!rd(map + off::kMapBits, &bits) || !rd(map + off::kMapCapacity, &capacity) || !rd(map + off::kMapKeys, &keys) ||
        !rd(map + off::kMapValues, &values) || !rd(map + off::kMapCount, &count))
        return std::nullopt;
    const bool pow2 = capacity && (capacity & (capacity - 1)) == 0;
    if (!count || !pow2 || capacity > off::kMapMaxCapacity || !bits || !keys || !values) return std::nullopt;
    uint64_t slot = static_cast<uint32_t>(id) & (capacity - 1);
    for (uint64_t probes = 0; probes < capacity && slot_occupied(bits, slot); ++probes) {
        uint64_t key = 0;
        if (!rd(keys + slot * 8, &key)) return std::nullopt;
        if (key == id) {
            const uint8_t* entry = nullptr;
            uint64_t entry_id = 0, len = 0;
            const char* text = nullptr;
            if (!rd(values + slot * 8, &entry) || !entry || !rd(entry + kEntryId, &entry_id) ||
                !rd(entry + kEntryText, &text) || !rd(entry + kEntryLen, &len))
                return std::nullopt;
            if (entry_id != id || len == 0 || len > kMaxCustomString || !text) return std::nullopt;
            std::string s(static_cast<size_t>(len), '\0');
            if (!seh_read(text, s.data(), s.size())) return std::nullopt;
            if (fnv1a64(s) != id || !display_name_ok(s)) return std::nullopt;
            return s;
        }
        slot = slot + 1 < capacity ? slot + 1 : 0;
    }
    return std::nullopt;
}

}  // namespace emf::core
