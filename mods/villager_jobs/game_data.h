#pragma once
// villager_jobs: reading the game's recipe registry and inventories (docs/engine_parts/re_villager_jobs.md). Every read
// goes through a copy function, so the mod can pass a fault-guarded one and the tests fake memory.
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "jobs_logic.h"

namespace jobs {

using CopyFn = bool (*)(void* dst, const void* src, size_t n);
inline bool plain_copy(void* dst, const void* src, size_t n) {
    if (!src) return false;
    std::memcpy(dst, src, n);
    return true;
}

template <class T>
bool read(CopyFn copy, const void* base, size_t off, T* out) {
    return base && copy(out, static_cast<const uint8_t*>(base) + off, sizeof(T));
}

// g38_items::RecipeRegistry + 0xF8: Keen map (plain keys, linear probing) workshopId -> SmallDynamicArray<RecipeInfo*,
// 512> (0x1008-byte values, u16 count at +0x1000). RecipeInfo: +0x10 id, +0x28 inputs / +0x30 outputs (BlobArray:
// self-relative i32 offset, u32 count; elements 48 / 24 B with item +0x10, count +0x14), +0x38 duration (ns),
// +0x48 requiredPropsMask (u64). (+0x50 knowledgeRequirement is not read: its first u32 is non-zero for recipes the
// test villagers craft fine.)
inline std::vector<Recipe> workshop_recipes(const uint8_t* registry, uint32_t workshop, CopyFn copy) {
    std::vector<Recipe> out;
    if (!registry) return out;
    const uint8_t* map = registry + 0xF8;
    const uint64_t* bits = nullptr;
    const uint32_t* keys = nullptr;
    const uint8_t* values = nullptr;
    uint64_t cap = 0;
    if (!read(copy, map, 0x08, &bits) || !read(copy, map, 0x10, &cap) || !read(copy, map, 0x28, &keys) ||
        !read(copy, map, 0x40, &values) || !bits || !keys || !values || cap == 0 || (cap & (cap - 1)) || cap > (1u << 20))
        return out;
    const uint8_t* list = nullptr;
    uint64_t slot = workshop & (cap - 1);
    for (uint64_t probes = 0; probes < cap; ++probes) {
        uint64_t word = 0;
        if (!read(copy, bits, (slot >> 6) * 8, &word) || !((word >> (slot & 63)) & 1)) break;
        uint32_t key = 0;
        if (!read(copy, keys, slot * 4, &key)) break;
        if (key == workshop) {
            list = values + slot * 0x1008;
            break;
        }
        slot = (slot + 1) & (cap - 1);
    }
    uint16_t n = 0;
    if (!list || !read(copy, list, 0x1000, &n)) return out;
    for (uint16_t i = 0; i < n && i < 512; ++i) {
        const uint8_t* r = nullptr;
        if (!read(copy, list, i * 8u, &r) || !r) continue;
        Recipe rec;
        int32_t in_off = 0, out_off = 0;
        uint32_t in_n = 0, out_n = 0;
        if (!read(copy, r, 0x10, &rec.id) || !read(copy, r, 0x28, &in_off) || !read(copy, r, 0x2C, &in_n) ||
            !read(copy, r, 0x30, &out_off) || !read(copy, r, 0x34, &out_n) || !read(copy, r, 0x38, &rec.duration_ns) ||
            !read(copy, r, 0x48, &rec.required_props) || in_n > 32 || out_n > 32)
            continue;
        bool ok = true;
        for (uint32_t k = 0; k < in_n && ok; ++k) {
            Stack s;
            const uint8_t* e = r + 0x28 + in_off + k * 48;
            ok = read(copy, e, 0x10, &s.item) && read(copy, e, 0x14, &s.count);
            if (s.item == 0) rec.category_input = true;
            rec.inputs.push_back(s);
        }
        for (uint32_t k = 0; k < out_n && ok; ++k) {
            Stack s;
            const uint8_t* e = r + 0x30 + out_off + k * 24;
            ok = read(copy, e, 0x10, &s.item) && read(copy, e, 0x14, &s.count);
            rec.outputs.push_back(s);
        }
        if (ok) out.push_back(std::move(rec));
    }
    return out;
}

// A chest's items: its own Inventory (8 slots of {item, count, pide}) and those of the inventories its InventorySetup
// links (linksEntities[16] at +0), summed per item. `inventory_of` gives an entity's Inventory component or null.
// plain_only: leave out stacks with their own data entity (pide: gear with a level, perks, gem), which a take + put
// would turn into a plain copy.
inline Contents chest_contents(const uint8_t* setup, uint32_t chest,
                               const std::function<const uint8_t*(uint32_t)>& inventory_of, CopyFn copy,
                               bool plain_only = false) {
    Contents c;
    auto add = [&](uint32_t id) {
        const uint8_t* inv = inventory_of(id);
        if (!inv) return;
        for (int s = 0; s < 8; ++s) {
            uint32_t item = 0, count = 0, pide = 0;
            if (!read(copy, inv, s * 12, &item) || !read(copy, inv, s * 12 + 4, &count) || !item || !count) continue;
            if (plain_only && (!read(copy, inv, s * 12 + 8, &pide) || pide)) continue;
            c[item] += count;
        }
    };
    add(chest);
    for (int i = 0; i < 16 && setup; ++i) {
        uint32_t id = 0;
        if (read(copy, setup, i * 4, &id) && id && id != chest) add(id);
    }
    return c;
}

// "Material_T1_Wood_Logs" -> "Wood Logs": the category word and tier tokens (T0, T1-TX) go, unless only one word
// would be left ("Shield_T0_Wooden" -> "Shield Wooden").
inline std::string pretty_item(const std::string& name) {
    std::vector<std::string> words;
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('_', start);
        if (end == std::string::npos) end = name.size();
        words.push_back(name.substr(start, end - start));
        start = end + 1;
    }
    if (words.size() <= 1) return name;
    auto tier = [](const std::string& w) { return w.size() >= 2 && w[0] == 'T' && w[1] >= '0' && w[1] <= '9'; };
    std::vector<std::string> rest;
    for (size_t i = 1; i < words.size(); ++i)
        if (!tier(words[i]) && !words[i].empty()) rest.push_back(words[i]);
    if (rest.size() < 2) rest.insert(rest.begin(), words[0]);
    std::string out;
    for (const auto& w : rest) out += (out.empty() ? "" : " ") + w;
    return out;
}

}  // namespace jobs
