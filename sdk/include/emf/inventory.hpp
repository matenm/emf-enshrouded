#pragma once
// emf/inventory.hpp — read what an entity carries (header-only, on top of emf.hpp).
//
// An entity with an InventorySetup (player, villager, chest, farm animal, feeder) owns several 8-slot Inventory
// entities (backpack, action bar, equipment...), linked from InventorySetup.linksEntities. These helpers read them;
// to CHANGE an inventory use emf::put_items / put_items_ex / take_items (the game's own transaction).
//
//   for (const auto& s : emf::inventory_stacks(w, chest))
//       EMF_LOG("mymod", "slot %u of %u: item %08x x%u", s.slot, s.inventory, s.item, s.count);
//   uint32_t logs = emf::inventory_contents(w, chest)[logs_id];
//
// Call inside a world callback, on the world it passed you.
#include <cstdint>
#include <map>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"

namespace emf {

struct Stack {
    uint32_t inventory = 0;    // the 8-slot inventory entity
    uint8_t slot = 0;          // 0..7
    uint32_t item = 0;         // item id
    uint32_t count = 0;
    uint32_t data_entity = 0;  // a gear item's own data entity ("pide": level, perks, gem); 0 for plain stacks
};

// The InventorySlotId take_items wants for a slot (the inventory entity in bits 0-31, the slot in bits 32-39).
inline uint64_t slot_id(uint32_t inventory, uint8_t slot) { return (static_cast<uint64_t>(slot) << 32) | inventory; }

// Every non-empty stack of the entity's own Inventory and the inventories its InventorySetup links.
inline std::vector<Stack> inventory_stacks(World w, uint32_t entity) {
    std::vector<Stack> out;
    auto add = [&](uint32_t inv_id) {
        auto* inv = w.get<gen::ecs::Inventory>(inv_id);
        if (!inv) return;
        for (uint8_t s = 0; s < 8; ++s) {
            const auto& st = inv->slots[s];
            if (st.id.value) out.push_back({inv_id, s, st.id.value, st.data.count, st.data.pide.id});
        }
    };
    add(entity);
    if (auto* setup = w.get<gen::ecs::InventorySetup>(entity))
        for (const auto& link : setup->linksEntities)
            if (link.id && link.id != entity) add(link.id);
    return out;
}

// item id -> total count. plain_only: leave out gear stacks with their own data entity (a take + put of those makes
// a fresh copy without its perks).
inline std::map<uint32_t, uint32_t> inventory_contents(World w, uint32_t entity, bool plain_only = false) {
    std::map<uint32_t, uint32_t> out;
    for (const auto& s : inventory_stacks(w, entity))
        if (s.count && !(plain_only && s.data_entity)) out[s.item] += s.count;
    return out;
}

}  // namespace emf
