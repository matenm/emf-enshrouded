#pragma once
// Loot Magnet 2.0's game side, kept apart from loot_magnet.cpp so tests run it over a fake API: what it reads from an
// entity's components (its own and its template's) into a sweep::Thing, which entities near the players it reads, and
// the accept event that rolls a chest's loot. The rules are sweep_logic.h.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "magnet_glue.h"
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

// The template a bee hive spawns when destroyed (Base_Helper_Bee_Hive_LootDropper): the honey and wax drop, and the
// wasps come out. A game update that renames it makes the rule match nothing.
inline bool is_hive_dropper(const uint8_t guid[16]) {
    static uint8_t dropper[16];
    static const bool ok = emf::parse_guid("8abfb80f-14b1-47c3-a51d-fe83d956c9fe", dropper);
    return ok && std::memcmp(guid, dropper, 16) == 0;
}

// Which groups' parts to read: a chest's (its lock, what excludes it, what it holds) and a barrel's are read only when
// that group is on and the entity has random loot, so a pickup or a plant costs about a dozen lookups.
struct Want {
    bool smash = true, chests = true, hives = true;
};

// `category_of`: an item's ItemCategory (item_category below, cached by the mod); empty = not read (-1).
inline Thing read_thing(emf::World w, uint32_t id, const std::function<std::string(uint32_t)>& name_of, Want want = {},
                        const std::function<int(uint32_t)>& category_of = {}) {
    namespace gen = emf::gen::ecs;
    Thing t;
    t.id = id;
    if (auto* tr = w.get<gen::CurrentTransform>(id))
        t.pos = {tr->transform.position.x / kFixed, tr->transform.position.y / kFixed, tr->transform.position.z / kFixed};
    if (auto* o = w.get<gen::InteractionOfferRuntime>(id)) {
        t.offer = true;
        t.offer_id = o->offerId;
        t.offered = o->isOffered;
        t.disabled = o->isDisabled;
        t.last_accept_guest = o->lastAcceptData.guestId.id;
    }
    if (auto* ts = w.get<gen::ServerToggleState>(id)) t.toggle = ts->state && !ts->isExtinguished ? 1 : 0;
    if (auto* st = w.get<gen::SpawnTime>(id)) t.fingerprint = static_cast<uint64_t>(st->spawnTime.value);
    if (auto* p = w.get<gen::PickupItem>(id)) {
        t.pickup = true;
        t.item = p->itemId.value ? name_of(p->itemId.value) : std::string();
        t.item_id = p->itemId.value;
        t.pickup_amount = p->pickupAmount;
        if (t.item_id && category_of) t.item_category = category_of(t.item_id);
    }
    const gen::DefaultInventory* def = w.get<gen::DefaultInventory>(id);
    if (!def) def = template_of<gen::DefaultInventory>(w, id);
    t.default_inv = def != nullptr;
    if (def) std::memcpy(t.loot_ref.data(), def->defaultInventory.bytes, t.loot_ref.size());
    t.setup = has<gen::InventorySetup>(w, id);
    if (auto* d = w.get<gen::DestroyOnLoot>(id)) {
        t.destroy_on_loot = true;
        t.looted = d->shouldDestroy;
    } else {
        t.destroy_on_loot = template_of<gen::DestroyOnLoot>(w, id) != nullptr;
    }
    t.looted = t.looted || w.get<gen::DestroyOnLootActivated>(id) != nullptr;
    if (auto* a = w.get<gen::AreaLootOverride>(id))
        t.exclude = a->exclude ? 1 : 0;
    else if (auto* ta = template_of<gen::AreaLootOverride>(w, id))
        t.exclude = ta->exclude ? 1 : 0;
    t.actor = has<gen::Actor>(w, id);
    t.faction = has<gen::Faction>(w, id);
    t.spawn_on_destroy = has<gen::SpawnEntityOnDestroy>(w, id);
    if (want.hives) {
        if (auto* sp = w.get<gen::SpawnEntityOnDestroy>(id)) {
            t.bee_hive = is_hive_dropper(sp->spawnEntity.bytes);
            t.hive_triggered = sp->triggered;
        } else if (auto* ts = template_of<gen::SpawnEntityOnDestroy>(w, id)) {  // its template's only: not triggered yet
            t.bee_hive = is_hive_dropper(ts->spawnEntity.bytes);
        }
    }
    t.enemy_spawner = has<gen::EnemySettings>(w, id) || has<gen::RandomSpawner>(w, id) || has<gen::TriggerSpawner>(w, id) ||
                      has<gen::EnemySpawnerClient>(w, id);
    t.used_item = has<gen::UsedItem>(w, id);
    t.food_bait = has<gen::FoodBait>(w, id);
    if (auto* inv = w.get<gen::Inventory>(id)) {
        t.inventory = true;
        for (const auto& s : inv->slots)
            if (s.id.value) t.stacks.push_back(name_of(s.id.value));
    }
    if (t.default_inv && !t.pickup) t.destructible = has<gen::Destructible>(w, id);  // a plant (a creature's drop is not)
    if (t.bee_hive) {  // its health: the hit that breaks it
        const gen::Destructible* d = template_of<gen::Destructible>(w, id);
        if (!d) d = w.get<gen::Destructible>(id);
        t.destructible = d != nullptr;
        t.max_health = d ? d->maxHealth : 0;
    }
    t.random_loot = has<gen::RandomLoot>(w, id);
    t.random_container = t.random_loot && has<gen::RandomLootContainer>(w, id);
    if (!t.random_container || !(want.smash || want.chests)) return t;
    // a barrel's or a chest's parts
    t.loot_on_destroy = has<gen::RandomLootOnDestroy>(w, id);
    t.disable_loot_on_destroy = has<gen::DisableRandomLootOnDestroy>(w, id);
    if (auto* d = template_of<gen::Destructible>(w, id)) {
        t.destructible = true;
        t.max_health = d->maxHealth;
    } else if (auto* di = w.get<gen::Destructible>(id)) {
        t.destructible = true;
        t.max_health = di->maxHealth;
    }
    t.mining_node = has<gen::MiningNode>(w, id);
    if (!want.chests || !t.inventory) return t;
    t.chest_excluded = t.used_item || has<gen::UpgradeStorage>(w, id) || has<gen::ItemCollectionComponent>(w, id) ||
                       has<gen::CreateItemLoot>(w, id) || t.destroy_on_loot || has<gen::InteractionLootRefill>(w, id) ||
                       has<gen::AnimalTarget>(w, id) || has<gen::InteractionKnowledgeLock>(w, id) ||
                       has<gen::LoreReading>(w, id);
    const gen::InteractionLock* lock = w.get<gen::InteractionLock>(id);
    if (!lock) lock = template_of<gen::InteractionLock>(w, id);
    if (lock) {
        t.lock = true;
        t.lock_item = lock->requiredItem.value;
        for (uint8_t b : lock->requiredItemRef.bytes) t.lock_item_ref = t.lock_item_ref || b != 0;
    }
    if (auto* used = w.get<gen::DynamicInteractionLock>(id)) t.lock_item_used = used->itemUsed;
    return t;
}

// The entities within `reach` (horizontally) of any of `places` that the sweep may want: those with an interaction
// offer (pickups, plants, chests) and, with smashing on, those whose template drops loot when destroyed (barrels,
// crates, urns). One walk of the world: an entity's position first (out of reach: one lookup), then whether it is a
// crop's root (its stage is never taken: harvesting it would end the plant), then its offer.
inline std::vector<Thing> things_near(emf::World w, const std::vector<magnet::Vec3>& places, double reach,
                                      const std::function<std::string(uint32_t)>& name_of, Want want = {},
                                      const std::function<int(uint32_t)>& category_of = {}) {
    namespace gen = emf::gen::ecs;
    std::vector<Thing> out;
    std::vector<uint32_t> stages;
    if (places.empty()) return out;
    const double crop_reach = reach + 5.0;  // a root stands where its stage grows
    w.for_each_entity([&](uint32_t id) {
        auto* tr = w.get<gen::CurrentTransform>(id);
        if (!tr) return;
        const magnet::Vec3 p{tr->transform.position.x / kFixed, tr->transform.position.y / kFixed,
                             tr->transform.position.z / kFixed};
        double d = 1e300;
        for (const auto& place : places) d = (std::min)(d, magnet::horizontal_distance(p, place));
        if (d > crop_reach) return;
        if (w.get<gen::DynamicGrowth>(id)) {
            if (auto* up = w.get<gen::DynamicUpgrade>(id))
                if (up->entityId.id) stages.push_back(up->entityId.id);
            return;
        }
        if (d > reach) return;
        // a barrel's RandomLootOnDestroy is its own component on some, its template's on others (both seen)
        if (!w.get<gen::InteractionOfferRuntime>(id) && !(want.smash && has<gen::RandomLootOnDestroy>(w, id))) return;
        out.push_back(read_thing(w, id, name_of, want, category_of));
    });
    std::sort(stages.begin(), stages.end());
    for (auto& t : out) t.crop_stage = std::binary_search(stages.begin(), stages.end(), t.id);
    return out;
}

// Whether the player has an empty slot in their bag (the generic inventories their InventorySetup links: backpack and
// action bar), never more than their usable slots allow: a pickup comes only then, so it always fits whole.
inline bool has_room(emf::World w, uint32_t player) {
    namespace gen = emf::gen::ecs;
    auto* setup = w.get<gen::InventorySetup>(player);
    if (!setup) return false;
    uint32_t empty = 0, used = 0;
    for (int i = 0; i < 16; ++i) {
        const uint32_t id = setup->linksEntities[i].id;
        if (!id || setup->linksCategories[i] != emf::gen::InventoryCategory::Generic) continue;
        auto* inv = w.get<gen::Inventory>(id);
        if (!inv) continue;
        for (const auto& st : inv->slots) {
            if (!st.id.value || !st.data.count) ++empty;
            else ++used;
        }
    }
    if (setup->availableSlotCount) {  // linked inventories may hold more slots than the player may use
        const uint32_t usable = setup->availableSlotCount > used ? setup->availableSlotCount - used : 0;
        empty = (std::min)(empty, usable);
    }
    return empty > 0;
}

// How many of each item the player carries: their backpack and action bar (the generic inventories their InventorySetup
// links), their equipment (arrows in the quiver count) and their currency; never customization.
inline std::map<uint32_t, uint32_t> carried(emf::World w, uint32_t player) {
    namespace gen = emf::gen::ecs;
    using Cat = emf::gen::InventoryCategory;
    std::map<uint32_t, uint32_t> out;
    auto* setup = w.get<gen::InventorySetup>(player);
    if (!setup) return out;
    for (int i = 0; i < 16; ++i) {
        const uint32_t id = setup->linksEntities[i].id;
        const auto cat = setup->linksCategories[i];
        if (!id || (cat != Cat::Generic && cat != Cat::Equipment && cat != Cat::Currency)) continue;
        auto* inv = w.get<gen::Inventory>(id);
        if (!inv) continue;
        for (const auto& st : inv->slots)
            if (st.id.value && st.data.count) out[st.id.value] += st.data.count;
    }
    return out;
}

// An item's ItemCategory: the game's ItemInfo byte at +0xCC (docs/engine_parts/re_take_items.md); -1 when the item or
// its data is not there.
inline int item_category(emf::World w, uint32_t item) {
    const EmfApi* a = emf::api();
    if (!a || !a->item_data) return -1;
    uint64_t size = 0;
    const auto* p = static_cast<const uint8_t*>(a->item_data(w.raw(), item, &size));
    if (!p || size <= 0xcc) return -1;
    return p[0xcc];
}

// The game's accept of an offered interaction, as a player's E sends it: on a chest it rolls the loot.
inline void fill_accept(emf::gen::ecs::AcceptOfferedInteractionEvent& e, uint32_t player, uint32_t host, uint32_t offer_id) {
    e.guestEntityId.id = player;
    e.hostEntityId.id = host;
    e.offerId = offer_id;
}

// A loot event or a chest's accept for the program, which sends it on a game worker thread.
struct Request {
    uint32_t guest = 0, host = 0, session = 0;
    bool accept = false;    // the game's accept of the chest's offer (it rolls the loot), not the loot event
    uint32_t offer_id = 0;  // the offer it accepts
};
inline Request request_of(const Action& a, uint32_t session) {
    Request r;
    r.guest = a.player;
    r.host = a.entity;
    r.session = session;
    r.accept = a.act == Act::Accept;
    r.offer_id = a.offer_id;
    return r;
}
// From the program (on the game's worker thread): the request as the game's event.
inline void send(void* ctx, const Request& r) {
    if (r.accept) {
        if (auto* e = emf::emit<emf::gen::ecs::AcceptOfferedInteractionEvent>(ctx)) fill_accept(*e, r.guest, r.host, r.offer_id);
    } else if (auto* e = emf::emit<emf::gen::ecs::LootInteractionEvent>(ctx)) {
        magnet::fill_loot_event(*e, r.guest, r.host);
    }
}

}  // namespace sweep
