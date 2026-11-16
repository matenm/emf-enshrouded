#pragma once
// Loot Magnet 2.0's game side, kept apart from loot_magnet.cpp so tests run it over a fake API: what it reads from an
// entity's components (its own and its template's) into a sweep::Thing, which entities near the players it reads, and
// the accept event that rolls a chest's loot. The rules are sweep_logic.h.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "sweep_logic.h"

namespace sweep {

constexpr double kFixed = 4294967296.0;  // positions: 1 m = 2^32

// The entity's template half of a component (every entity of its template shares it), or null.
template <class T>
const T* template_of(emf::World w, uint32_t id) {
    const EmfApi* a = emf::api();
    if (!a || !a->get_template_component) return nullptr;
    return static_cast<const T*>(a->get_template_component(w.raw(), T::kHash, id, nullptr));
}

// The component on the entity itself or on its template.
template <class T>
bool has(emf::World w, uint32_t id) {
    return w.get<T>(id) != nullptr || template_of<T>(w, id) != nullptr;
}

inline Thing read_thing(emf::World w, uint32_t id, const std::function<std::string(uint32_t)>& name_of) {
    namespace gen = emf::gen::ecs;
    Thing t;
    t.id = id;
    if (auto* tr = w.get<gen::CurrentTransform>(id))
        t.pos = {tr->transform.position.x / kFixed, tr->transform.position.y / kFixed, tr->transform.position.z / kFixed};
    if (auto* o = w.get<gen::InteractionOfferRuntime>(id)) {
        t.offer = true;
        t.offer_id = o->offerId;
        t.offered = o->isOffered;
    }
    if (auto* p = w.get<gen::PickupItem>(id)) {
        t.pickup = true;
        t.item = p->itemId.value ? name_of(p->itemId.value) : std::string();
    }
    t.default_inv = has<gen::DefaultInventory>(w, id);
    t.setup = has<gen::InventorySetup>(w, id);
    t.destroy_on_loot = has<gen::DestroyOnLoot>(w, id);
    if (auto* a = w.get<gen::AreaLootOverride>(id))
        t.exclude = a->exclude ? 1 : 0;
    else if (auto* ta = template_of<gen::AreaLootOverride>(w, id))
        t.exclude = ta->exclude ? 1 : 0;
    t.actor = has<gen::Actor>(w, id);
    t.faction = has<gen::Faction>(w, id);
    t.random_loot = has<gen::RandomLoot>(w, id);
    t.random_container = has<gen::RandomLootContainer>(w, id);
    t.loot_on_destroy = has<gen::RandomLootOnDestroy>(w, id);
    if (auto* d = template_of<gen::Destructible>(w, id)) {
        t.destructible = true;
        t.max_health = d->maxHealth;
    } else if (auto* di = w.get<gen::Destructible>(id)) {
        t.destructible = true;
        t.max_health = di->maxHealth;
    }
    t.spawn_on_destroy = has<gen::SpawnEntityOnDestroy>(w, id);
    t.mining_node = has<gen::MiningNode>(w, id);
    t.chest_excluded = has<gen::UsedItem>(w, id) || has<gen::UpgradeStorage>(w, id) ||
                       has<gen::ItemCollectionComponent>(w, id) || has<gen::CreateItemLoot>(w, id) ||
                       t.destroy_on_loot || has<gen::InteractionLootRefill>(w, id) || has<gen::AnimalTarget>(w, id) ||
                       has<gen::InteractionKnowledgeLock>(w, id) || has<gen::LoreReading>(w, id);
    const gen::InteractionLock* lock = w.get<gen::InteractionLock>(id);
    if (!lock) lock = template_of<gen::InteractionLock>(w, id);
    if (lock) {
        t.lock = true;
        t.lock_item = lock->requiredItem.value;
    }
    if (auto* used = w.get<gen::DynamicInteractionLock>(id)) t.lock_item_used = used->itemUsed;
    if (auto* inv = w.get<gen::Inventory>(id)) {
        t.inventory = true;
        for (const auto& s : inv->slots)
            if (s.id.value) t.stacks.push_back(name_of(s.id.value));
    }
    return t;
}

// The entities within `reach` (horizontally) of any of `places` that the sweep may want: those with an interaction
// offer (pickups, plants, chests) and those whose template drops loot when destroyed (barrels, crates, urns). One walk
// of the world; every other entity costs two lookups.
inline std::vector<Thing> things_near(emf::World w, const std::vector<magnet::Vec3>& places, double reach,
                                      const std::function<std::string(uint32_t)>& name_of) {
    namespace gen = emf::gen::ecs;
    std::vector<Thing> out;
    if (places.empty()) return out;
    w.for_each_entity([&](uint32_t id) {
        if (!w.get<gen::InteractionOfferRuntime>(id) && !template_of<gen::RandomLootOnDestroy>(w, id)) return;
        auto* tr = w.get<gen::CurrentTransform>(id);
        if (!tr) return;
        const magnet::Vec3 p{tr->transform.position.x / kFixed, tr->transform.position.y / kFixed,
                             tr->transform.position.z / kFixed};
        bool in_reach = false;
        for (const auto& place : places) in_reach = in_reach || magnet::horizontal_distance(p, place) <= reach;
        if (in_reach) out.push_back(read_thing(w, id, name_of));
    });
    return out;
}

// The game's accept of an offered interaction, as a player's E sends it: on a chest it rolls the loot.
inline void fill_accept(emf::gen::ecs::AcceptOfferedInteractionEvent& e, uint32_t player, uint32_t host, uint32_t offer_id) {
    e.guestEntityId.id = player;
    e.hostEntityId.id = host;
    e.offerId = offer_id;
}

}  // namespace sweep
