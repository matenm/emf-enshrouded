#pragma once
// Bulk Craft: reading the game's recipe registry (g38_items::RecipeRegistry; docs/engine_parts/inventory_crafting.md
// section 4). Every read goes through a copy function, so the mod passes a fault-guarded one and the tests fake memory.
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace bulkcraft {

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

struct Stack {
    uint32_t item = 0;
    uint32_t count = 0;
    uint32_t category = 0;  // a category input: item 0, this RecipeInputCategoryId, `count` of it
};

// One item of a recipe input category (RecipeInputCategoryItem, 8 B): the item, and how many of it one unit takes.
struct CategoryItem {
    uint32_t item = 0;
    float scaling = 1.0f;
};

// RecipeInfo (168 bytes; build 1273043): +0x10 id, +0x24 workshopId, +0x28 inputs / +0x30 outputs (BlobArray:
// self-relative i32 offset, u32 count; outputs 24 B with item +0x10, count +0x14; inputs 48 B
// (RecipeInputResource, types.json): itemStack {item +0x10, count +0x14} and inputItemCategory {category +0x28,
// count +0x2C}; a category input has item 0),
// +0x38 craftingDuration (ns), +0x48 requiredPropsMask, +0x50 knowledgeRequirement (KnowledgeQueryData 12 B: id,
// compareValue, compareOperator u8, type u8), +0x80 level, +0x84 comfortLevel, +0x88 serverProgressLevel,
// +0x8c requiredHappyNpcCount, +0x90 requiredEnergy, +0x94 waterAmount, +0x98 waterCharges, +0x9f isCookingRecipe,
// +0xa0 isUpgrade, +0xa1 requiresSheltered.
struct Recipe {
    uint32_t id = 0;
    uint32_t workshop = 0;
    std::vector<Stack> inputs;
    std::vector<Stack> outputs;
    bool category_input = false;  // an input any item of a category can fill (the crafting window's choice decides which)
    int64_t duration_ns = 0;
    uint64_t required_props = 0;
    uint32_t knowledge_id = 0;
    uint32_t knowledge_value = 0;
    uint8_t knowledge_op = 0;
    uint8_t knowledge_type = 0;
    uint32_t level = 0;
    uint32_t comfort = 0;
    uint32_t server_progress = 0;
    uint32_t happy_npcs = 0;
    uint32_t energy = 0;
    uint32_t water = 0;
    uint32_t water_charges = 0;
    bool cooking = false;
    bool upgrade = false;
    bool sheltered = false;
};

constexpr size_t kRecipeSize = 0xA8;

inline std::optional<Recipe> read_recipe(const uint8_t* r, CopyFn copy) {
    if (!r) return std::nullopt;
    Recipe rec;
    int32_t in_off = 0, out_off = 0;
    uint32_t in_n = 0, out_n = 0;
    if (!read(copy, r, 0x10, &rec.id) || !read(copy, r, 0x24, &rec.workshop) || !read(copy, r, 0x28, &in_off) ||
        !read(copy, r, 0x2C, &in_n) || !read(copy, r, 0x30, &out_off) || !read(copy, r, 0x34, &out_n) ||
        !read(copy, r, 0x38, &rec.duration_ns) || !read(copy, r, 0x48, &rec.required_props) ||
        !read(copy, r, 0x50, &rec.knowledge_id) || !read(copy, r, 0x54, &rec.knowledge_value) ||
        !read(copy, r, 0x58, &rec.knowledge_op) || !read(copy, r, 0x59, &rec.knowledge_type) ||
        !read(copy, r, 0x80, &rec.level) || !read(copy, r, 0x84, &rec.comfort) ||
        !read(copy, r, 0x88, &rec.server_progress) || !read(copy, r, 0x8C, &rec.happy_npcs) ||
        !read(copy, r, 0x90, &rec.energy) || !read(copy, r, 0x94, &rec.water) || !read(copy, r, 0x98, &rec.water_charges) ||
        !read(copy, r, 0x9F, &rec.cooking) || !read(copy, r, 0xA0, &rec.upgrade) || !read(copy, r, 0xA1, &rec.sheltered))
        return std::nullopt;
    if (in_n > 32 || out_n > 32 || rec.duration_ns < 0) return std::nullopt;
    for (uint32_t k = 0; k < in_n; ++k) {
        Stack s;
        const uint8_t* e = r + 0x28 + in_off + k * 48;
        if (!read(copy, e, 0x10, &s.item) || !read(copy, e, 0x14, &s.count)) return std::nullopt;
        if (s.item == 0) {
            if (!read(copy, e, 0x28, &s.category) || !read(copy, e, 0x2C, &s.count)) return std::nullopt;
            rec.category_input = true;
        }
        rec.inputs.push_back(s);
    }
    for (uint32_t k = 0; k < out_n; ++k) {
        Stack s;
        const uint8_t* e = r + 0x30 + out_off + k * 24;
        if (!read(copy, e, 0x10, &s.item) || !read(copy, e, 0x14, &s.count)) return std::nullopt;
        rec.outputs.push_back(s);
    }
    return rec;
}

namespace detail {
// A Keen hash map with plain u32 keys and linear probing: bitset of used slots at +0x08, capacity (a power of two) at
// +0x10, keys at +0x28, values at +0x40 (`value_size` bytes each). The address of the value of `key`, or null.
inline const uint8_t* map_find(const uint8_t* map, uint32_t key, size_t value_size, CopyFn copy) {
    const uint64_t* bits = nullptr;
    const uint32_t* keys = nullptr;
    const uint8_t* values = nullptr;
    uint64_t cap = 0;
    if (!read(copy, map, 0x08, &bits) || !read(copy, map, 0x10, &cap) || !read(copy, map, 0x28, &keys) ||
        !read(copy, map, 0x40, &values) || !bits || !keys || !values || cap == 0 || (cap & (cap - 1)) || cap > (1u << 20))
        return nullptr;
    uint64_t slot = key & (cap - 1);
    for (uint64_t probes = 0; probes < cap; ++probes) {
        uint64_t word = 0;
        if (!read(copy, bits, (slot >> 6) * 8, &word) || !((word >> (slot & 63)) & 1)) return nullptr;
        uint32_t k = 0;
        if (!read(copy, keys, slot * 4, &k)) return nullptr;
        if (k == key) return values + slot * value_size;
        slot = (slot + 1) & (cap - 1);
    }
    return nullptr;
}
}  // namespace detail

// The recipe with this id (the registry's map at +0x48: RecipeId -> RecipeInfo*), or nothing.
inline std::optional<Recipe> find_recipe(const uint8_t* registry, uint32_t id, CopyFn copy) {
    if (!registry || !id) return std::nullopt;
    const uint8_t* value = detail::map_find(registry + 0x48, id, 8, copy);
    const uint8_t* r = nullptr;
    if (!value || !read(copy, value, 0, &r) || !r) return std::nullopt;
    auto rec = read_recipe(r, copy);
    if (!rec || rec->id != id) return std::nullopt;
    return rec;
}

// Every recipe of a workshop (the registry's map at +0xF8: workshopId -> SmallDynamicArray<RecipeInfo*, 512>, values
// 0x1008 bytes with the u16 count at +0x1000). Workshop 0 is the player's own hand crafting.
inline std::vector<Recipe> workshop_recipes(const uint8_t* registry, uint32_t workshop, CopyFn copy) {
    std::vector<Recipe> out;
    if (!registry) return out;
    const uint8_t* list = detail::map_find(registry + 0xF8, workshop, 0x1008, copy);
    uint16_t n = 0;
    if (!list || !read(copy, list, 0x1000, &n)) return out;
    for (uint16_t i = 0; i < n && i < 512; ++i) {
        const uint8_t* r = nullptr;
        if (!read(copy, list, i * 8u, &r) || !r) continue;
        if (auto rec = read_recipe(r, copy)) out.push_back(std::move(*rec));
    }
    return out;
}

// The items of a recipe input category (the registry's map at +0xA0: RecipeInputCategoryId -> RecipeInputCategory*;
// RecipeInputCategory 12 B: id +0, items BlobArray at +4 of RecipeInputCategoryItem 8 B {itemId, scaling float}; layout
// from types.json, the map like the other two). Empty when unknown. Not seen in game.
inline std::vector<CategoryItem> category_items(const uint8_t* registry, uint32_t category, CopyFn copy) {
    std::vector<CategoryItem> out;
    if (!registry || !category) return out;
    const uint8_t* value = detail::map_find(registry + 0xA0, category, 8, copy);
    const uint8_t* c = nullptr;
    uint32_t id = 0, n = 0;
    int32_t off = 0;
    if (!value || !read(copy, value, 0, &c) || !c || !read(copy, c, 0, &id) || id != category ||
        !read(copy, c, 4, &off) || !read(copy, c, 8, &n) || n > 64)
        return out;
    for (uint32_t k = 0; k < n; ++k) {
        CategoryItem it;
        const uint8_t* e = c + 4 + off + k * 8;
        if (!read(copy, e, 0, &it.item) || !read(copy, e, 4, &it.scaling)) return {};
        out.push_back(it);
    }
    return out;
}

}  // namespace bulkcraft
