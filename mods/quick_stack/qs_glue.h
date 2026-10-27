#pragma once
// Quick Stack / Chest Machines' game side, kept apart from quick_stack.cpp so tests can run it over a fake API: what
// counts as a chest, what a player carries, a chest's stacks and free slots, and the game's recipes by what they make.
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "emf/inventory.hpp"
#include "emf/query.hpp"
#include "quickstack.h"

namespace quickstack {

constexpr uint32_t kActiveNpcStateTemplateHash = 0x1a8b5909u;  // villagers' template data

// A storage entity: an initialized inventory that is not a player, a villager or an animal, and not a tombstone or a
// loot container (in game a tombstone has an initialized InventorySetup too, with CreateItemLoot, DestroyOnLoot and a
// LifeTime: it vanishes when emptied or when its time runs out, so it must never receive items).
inline bool is_chest(emf::World w, uint32_t id) {
    auto* setup = w.get<emf::gen::ecs::InventorySetup>(id);
    return setup && setup->isInitialized && !w.get<emf::gen::ecs::PlayerState>(id) &&
           !w.get<emf::gen::ecs::AnimalSaved>(id) && !w.get<emf::gen::ecs::DestroyOnLoot>(id) &&
           !w.get<emf::gen::ecs::LifeTime>(id) && !w.get<emf::gen::ecs::CreateItemLoot>(id) &&
           !emf::api()->get_template_component(w.raw(), kActiveNpcStateTemplateHash, id, nullptr);
}

// What the player carries in its Generic inventories (backpack and action bar): the action bar is kept with
// `keep_bar`, items the filters name (by their debug name from `name_of`) are kept too.
inline std::vector<Carried> carried(emf::World w, uint32_t player, bool keep_bar, const Filters& f,
                                    const std::function<std::string(uint32_t)>& name_of) {
    std::vector<Carried> out;
    auto* setup = w.get<emf::gen::ecs::InventorySetup>(player);
    if (!setup) return out;
    std::set<uint32_t> bar;
    if (auto* sel = w.get<emf::gen::ecs::SlotSelection>(player))
        for (const auto& e : sel->actionbarEntityIds)
            if (e.id) bar.insert(e.id);
    const bool filters = f.keep_ammo || f.keep_food || !f.exclude.empty();
    for (int i = 0; i < 16; ++i) {
        const uint32_t inv_id = setup->linksEntities[i].id;
        if (!inv_id || setup->linksCategories[i] != emf::gen::InventoryCategory::Generic) continue;
        auto* inv = w.get<emf::gen::ecs::Inventory>(inv_id);
        if (!inv) continue;
        const bool kept = keep_bar && bar.count(inv_id);
        for (uint8_t s = 0; s < 8; ++s) {
            const auto& st = inv->slots[s];
            if (!st.id.value || !st.data.count) continue;
            const bool by_name = filters && filtered(name_of(st.id.value), f);
            out.push_back({emf::slot_id(inv_id, s), st.id.value, st.data.count, st.data.pide.id != 0, kept || by_name});
        }
    }
    return out;
}

// Every stack of a chest (its own Inventory and the inventories it links), gear marked.
inline std::vector<Carried> chest_stacks(emf::World w, uint32_t chest) {
    std::vector<Carried> out;
    for (const auto& s : emf::inventory_stacks(w, chest))
        if (s.item && s.count) out.push_back({emf::slot_id(s.inventory, s.slot), s.item, s.count, s.data_entity != 0});
    return out;
}

// Empty slots in a chest's own Inventory and its linked Generic inventories, never more than its usable slots allow
// (InventorySetup.availableSlotCount: a chest's linked inventories can hold more slots than it may use).
inline uint32_t free_slots(emf::World w, uint32_t chest) {
    uint32_t empty = 0, used = 0;
    auto count = [&](uint32_t id) {
        auto* inv = w.get<emf::gen::ecs::Inventory>(id);
        if (!inv) return;
        for (const auto& st : inv->slots) {
            if (!st.id.value || !st.data.count) ++empty;
            else ++used;
        }
    };
    count(chest);
    auto* setup = w.get<emf::gen::ecs::InventorySetup>(chest);
    if (setup)
        for (int i = 0; i < 16; ++i) {
            const uint32_t id = setup->linksEntities[i].id;
            if (id && id != chest && setup->linksCategories[i] == emf::gen::InventoryCategory::Generic) count(id);
        }
    if (setup && setup->availableSlotCount) {
        const uint32_t usable = setup->availableSlotCount > used ? setup->availableSlotCount - used : 0;
        if (usable < empty) empty = usable;
    }
    return empty;
}

// How many more of `item` fit into a chest: its free slots in full stacks, and what its part-filled stacks of that
// item can still take. `max_stack` 0 = not known: no limit (UINT64_MAX). `biggest` (optional): the biggest plain stack
// of the item in it (0 for none); one above `max_stack` means the size has changed since it was read.
inline uint64_t room_for(emf::World w, uint32_t chest, uint32_t item, uint32_t max_stack, uint32_t* biggest = nullptr) {
    if (biggest) *biggest = 0;
    if (!max_stack) return UINT64_MAX;
    uint64_t room = static_cast<uint64_t>(free_slots(w, chest)) * max_stack;
    for (const auto& s : emf::inventory_stacks(w, chest)) {
        if (s.item != item || s.data_entity) continue;
        if (biggest && s.count > *biggest) *biggest = s.count;
        if (s.count < max_stack) room += max_stack - s.count;
    }
    return room;
}

// ---- marks and their chests ----
struct Resolved {
    Mark mark = Mark::None;
    Vec3 pos;
    uint32_t id = 0;  // the chest there in this session, 0 while its place is not loaded
};
struct DroppedMark {
    Mark mark = Mark::None;
    Vec3 pos;
    std::string why;
};

// Matches every mark to the chest at its place (one pass over the world) and drops the marks that lost their chest:
//   - another chest at the place than the one the mark found earlier this session (MarkBinder): the marked chest was
//     taken down and another put in its spot, which must not become a recycler unasked;
//   - no chest there while a player is within `gone_radius` of the place, on looks spanning 30 s and not in the first
//     30 s after a load (MissBook): the chest was taken down (a place far from every player may just not be loaded).
// The dropped ones are taken out of `marks` and listed in `dropped` (the caller saves, logs and tells the host).
inline std::vector<Resolved> resolve_marks(emf::World w, Marks& marks, MarkBinder& binder, MissBook& miss, uint64_t now,
                                           double gone_radius, std::vector<DroppedMark>* dropped) {
    std::vector<Resolved> out;
    for (const auto& m : marks.all()) out.push_back({m.mark, m.pos, 0});
    if (!out.empty())
        w.for_each_entity([&](uint32_t id) {
            const auto p = emf::position(w, id);
            if (!p) return;
            const Vec3 at{p->x, p->y, p->z};
            for (auto& r : out)
                if (!r.id && dist(at, r.pos) <= kPlaceTolerance && is_chest(w, id)) r.id = id;
        });
    std::vector<Vec3> players;
    for (uint32_t id : emf::players(w))
        if (const auto p = emf::position(w, id)) players.push_back({p->x, p->y, p->z});
    std::vector<Resolved> kept;
    for (const auto& r : out) {
        std::string why;
        if (r.id) {
            miss.found(r.pos);
            if (binder.seen(r.pos, r.id) == MarkBinder::Swapped)
                why = "another chest stands at its place now (the marked one was taken down)";
        } else {
            bool near_one = false;
            for (const auto& p : players) near_one = near_one || dist(p, r.pos) <= gone_radius;
            if (miss.missing(r.pos, near_one, now)) why = "it is gone (missed for 30 s with a player near)";
        }
        if (why.empty()) {
            kept.push_back(r);
            continue;
        }
        marks.set(r.pos, Mark::None);
        miss.found(r.pos);
        binder.forget(r.pos);
        if (dropped) dropped->push_back({r.mark, r.pos, why});
    }
    return kept;
}

// ---- the game's recipes ----
using CopyFn = bool (*)(void* dst, const void* src, size_t n);
inline bool plain_copy(void* dst, const void* src, size_t n) {
    if (!src) return false;
    std::memcpy(dst, src, n);
    return true;
}
inline bool guarded_copy(void* dst, const void* src, size_t n) {
    if (!src) return false;
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
        return false;
    }
}
namespace detail {
template <class T>
bool read(CopyFn copy, const void* base, size_t off, T* out) {
    return base && copy(out, static_cast<const uint8_t*>(base) + off, sizeof(T));
}
}  // namespace detail

// Every recipe of the game by each item it makes. g38_items::RecipeRegistry + 0x48 is a Keen map recipe id ->
// RecipeInfo* (bitset +0x08, capacity +0x10, keys +0x28, values +0x40; docs/engine_parts/inventory_crafting.md).
// RecipeInfo: +0x10 id, +0x28 inputs / +0x30 outputs (BlobArray: self-relative i32 offset, u32 count; elements 48 / 24
// bytes with the item at +0x10 and the count at +0x14; an input of item 0 is a category input), +0x9f isCookingRecipe,
// +0xa0 isUpgrade (the same reads as villager_jobs' game_data.h, which crafts with them).
inline std::map<uint32_t, std::vector<Recipe>> recipes_by_output(const uint8_t* registry, CopyFn copy) {
    std::map<uint32_t, std::vector<Recipe>> out;
    if (!registry) return out;
    const uint8_t* map = registry + 0x48;
    const uint64_t* bits = nullptr;
    const uint8_t* values = nullptr;
    uint64_t cap = 0;
    if (!detail::read(copy, map, 0x08, &bits) || !detail::read(copy, map, 0x10, &cap) ||
        !detail::read(copy, map, 0x40, &values) || !bits || !values || cap == 0 || (cap & (cap - 1)) || cap > (1u << 20))
        return out;
    for (uint64_t slot = 0; slot < cap; ++slot) {
        uint64_t word = 0;
        if (!detail::read(copy, bits, (slot >> 6) * 8, &word)) break;
        if (!((word >> (slot & 63)) & 1)) continue;
        const uint8_t* r = nullptr;
        if (!detail::read(copy, values, slot * 8, &r) || !r) continue;
        Recipe rec;
        int32_t in_off = 0, out_off = 0;
        uint32_t in_n = 0, out_n = 0;
        uint8_t cooking = 0, upgrade = 0;
        if (!detail::read(copy, r, 0x10, &rec.id) || !detail::read(copy, r, 0x28, &in_off) ||
            !detail::read(copy, r, 0x2C, &in_n) || !detail::read(copy, r, 0x30, &out_off) ||
            !detail::read(copy, r, 0x34, &out_n) || !detail::read(copy, r, 0x9f, &cooking) ||
            !detail::read(copy, r, 0xa0, &upgrade) || in_n > 32 || out_n > 32)
            continue;
        rec.cooking = cooking != 0;
        rec.upgrade = upgrade != 0;
        bool ok = true;
        for (uint32_t k = 0; k < in_n && ok; ++k) {
            Stack s;
            const uint8_t* e = r + 0x28 + in_off + k * 48;
            ok = detail::read(copy, e, 0x10, &s.item) && detail::read(copy, e, 0x14, &s.count);
            rec.inputs.push_back(s);
        }
        for (uint32_t k = 0; k < out_n && ok; ++k) {
            Stack s;
            const uint8_t* e = r + 0x30 + out_off + k * 24;
            ok = detail::read(copy, e, 0x10, &s.item) && detail::read(copy, e, 0x14, &s.count);
            rec.outputs.push_back(s);
        }
        if (!ok) continue;
        std::set<uint32_t> made;
        for (const auto& o : rec.outputs)
            if (o.item && made.insert(o.item).second) out[o.item].push_back(rec);
    }
    return out;
}

}  // namespace quickstack
