#pragma once
// Loot Magnet's game side, kept apart from loot_magnet.cpp so tests can run it over a fake API: what it reads from a
// dead entity's components, and the loot event it sends.
#include <cstdint>
#include <functional>
#include <string>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "emf/inventory.hpp"
#include "magnet_logic.h"

namespace magnet {

constexpr uint32_t kActiveNpcStateHash = 0x1a8b5909u;  // villagers' template data
constexpr double kFixed = 4294967296.0;                // positions: 1 m = 2^32

// Who an entity is: its template's Enemy01BehaviorState data (every entity of a template shares it), 0 if it has none
// (then identity is not compared). A reused id belongs to another template, or is alive.
inline uintptr_t template_of(emf::World w, uint32_t id) {
    const EmfApi* a = emf::api();
    if (!a || !a->get_template_component) return 0;
    return reinterpret_cast<uintptr_t>(a->get_template_component(w.raw(), emf::gen::ecs::Enemy01BehaviorState::kHash, id, nullptr));
}

// A dead entity as the magnet judges it. `name_of(item)` gives an item's debug name ("" when the game does not name it
// yet: a materials-only corpse then waits).
inline Body read_body(emf::World w, uint32_t id, const std::function<std::string(uint32_t)>& name_of) {
    namespace gen = emf::gen::ecs;
    Body b;
    b.id = id;
    auto* t = w.get<gen::CurrentTransform>(id);
    if (!t) return b;
    b.exists = true;
    b.pos = {t->transform.position.x / kFixed, t->transform.position.y / kFixed, t->transform.position.z / kFixed};
    b.player = w.get<gen::PlayerState>(id) != nullptr;
    auto* health = w.get<gen::Health>(id);
    b.has_health = health != nullptr;
    if (auto* n = w.get<gen::NetworkHealth>(id)) b.dead = n->health == 0;
    else b.dead = health && health->dataStorage[0] <= 0;
    b.has_inventory = w.get<gen::Inventory>(id) != nullptr;
    b.has_offer = w.get<gen::InteractionOfferRuntime>(id) != nullptr;
    if (auto* o = w.get<gen::OwnerRelationship>(id)) b.owner = o->ownerEntityId.id;
    if (auto* f = w.get<gen::Faction>(id)) b.faction = static_cast<int>(f->faction);
    b.tame = w.get<gen::IsTame>(id) != nullptr;
    const EmfApi* a = emf::api();
    b.villager = a && a->get_template_component && a->get_template_component(w.raw(), kActiveNpcStateHash, id, nullptr);
    b.tpl = template_of(w, id);
    for (const auto& s : emf::inventory_stacks(w, id)) b.items.push_back(name_of(s.item));
    return b;
}

// The game's own "loot in range" event: the corpse's stacks into the player (seen in game: a PickupEvent each).
inline void fill_loot_event(emf::gen::ecs::LootInteractionEvent& e, uint32_t player, uint32_t corpse) {
    e.guestId.id = player;
    e.hostId.id = corpse;
    e.offerId = 0;
    e.lootAll = true;
    e.bypassInteraction = true;
}

}  // namespace magnet
