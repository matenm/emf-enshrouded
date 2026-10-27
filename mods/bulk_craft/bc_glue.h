#pragma once
// Bulk Craft's game side, kept apart from bulk_craft.cpp so tests can run it over a fake API: which entities are
// stations it may use, the one a player looks at (or the nearest), their recipes, and what a player carries.
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <vector>

#include "bc_logic.h"
#include "bc_recipes.h"
#include "emf/aim.hpp"
#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "emf/inventory.hpp"
#include "emf/query.hpp"

namespace bulkcraft {

constexpr double kStationHeight = 0.8;  // metres above a station's feet the crosshair aims at
constexpr double kRayTolerance = 1.5;   // metres from the crosshair's ray
constexpr double kWalkAwaySlack = 3.0;  // metres beyond `range` before an order stops

inline bool guarded_copy(void* dst, const void* src, size_t n) {
    if (!src) return false;
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
        return false;
    }
}

inline bool is_player(emf::World w, uint32_t id) {
    return id >= 1 && id <= emf::kMaxPlayerEntity && w.get<emf::gen::ecs::PlayerState>(id) != nullptr;
}

inline std::optional<uint32_t> workshop_of(emf::World w, uint32_t id) {
    auto* c = w.get<emf::gen::ecs::Crafting>(id);
    if (!c) return std::nullopt;
    return c->workshopId.value;
}

// A station Bulk Craft may use: an entity with Crafting that is not a player, not a factory (those run their own
// timers), not the flame altar, and not a plain hand-crafting entity (a villager with workshop 0: that is hand
// crafting, done with the player as the station).
inline bool is_station(emf::World w, uint32_t id) {
    const auto ws = workshop_of(w, id);
    if (!ws || *ws == 0 || w.get<emf::gen::ecs::PlayerState>(id) || w.get<emf::gen::ecs::FactoryStation>(id)) return false;
    for (uint32_t a : kAltarWorkshops)
        if (*ws == a) return false;
    return true;
}

struct Station {
    uint32_t id = 0;
    uint32_t workshop = 0;
    double distance = 0;
};

// The station under the player's crosshair within `range` (when `crosshair` and the player's camera is known), else
// the nearest one within `range`; nothing when there is none (then only hand crafting).
inline std::optional<Station> station_for(emf::World w, uint32_t player, double range, bool crosshair) {
    const auto pp = emf::position(w, player);
    if (!pp) return std::nullopt;
    const auto close_by = emf::entities_near(w, *pp, range, [&](uint32_t id) { return id != player && is_station(w, id); });
    if (close_by.empty()) return std::nullopt;
    size_t pick = 0;
    if (crosshair)
        if (const auto ray = emf::aim::camera_ray(w, player)) {
            std::vector<emf::aim::Vec3> points;
            for (const auto& n : close_by) points.push_back({n.pos.x, n.pos.y + kStationHeight, n.pos.z});
            if (const auto i = emf::aim::pick(*ray, *pp, points, {range, kRayTolerance, 0.25})) pick = *i;
        }
    return Station{close_by[pick].id, *workshop_of(w, close_by[pick].id), close_by[pick].distance};
}

inline const uint8_t* registry(emf::World w) {
    return static_cast<const uint8_t*>(w.singleton("g38_items::RecipeRegistry"));
}
inline std::vector<Recipe> station_recipes(emf::World w, uint32_t workshop) {
    return workshop_recipes(registry(w), workshop, guarded_copy);
}
inline std::optional<Recipe> recipe_by_id(emf::World w, uint32_t id) { return find_recipe(registry(w), id, guarded_copy); }
inline std::vector<CategoryItem> category_items(emf::World w, uint32_t category) {
    return bulkcraft::category_items(registry(w), category, guarded_copy);
}
// For resolve(): the items of a category in this world's registry.
inline CategoryLookup category_lookup(emf::World w) {
    return [w](uint32_t category) { return category_items(w, category); };
}

// The recipes a player may bulk-craft where they stand: their learned ones the station makes, then their learned hand
// recipes, newest first within each, matching `filter`.
template <class NameOf>
std::vector<Recipe> candidates(emf::World w, const std::vector<uint32_t>& learned, const std::optional<Station>& st,
                               const std::string& filter, NameOf&& name_of) {
    std::vector<Recipe> out;
    if (st) out = learned_here(learned, station_recipes(w, st->workshop), filter, name_of);
    for (auto& r : learned_here(learned, station_recipes(w, 0), filter, name_of)) out.push_back(std::move(r));
    return out;
}

// What the player carries: item -> count over their inventories (the materials the game's craft takes from).
inline std::map<uint32_t, uint32_t> carried(emf::World w, uint32_t player) { return emf::inventory_contents(w, player); }

// Before each send: is the order still sound? `name_of(item)` gives an item's debug name for the note. Another account in
// the player's entity (they left, someone joined into the slot) stops it quietly: the newcomer is told nothing.
template <class NameOf>
Stop order_check(emf::World w, const Order& o, NameOf&& name_of) {
    if (!o.account || emf::player_account(w, o.player) != o.account) return Stop{"another player has this slot now", true};
    const auto pp = emf::position(w, o.player);
    if (!pp) return Stop{"you left"};
    if (o.station != o.player) {
        if (!w.get<emf::gen::ecs::Crafting>(o.station)) return Stop{"the station is gone"};
        const auto sp = emf::position(w, o.station);
        if (!sp || emf::distance(*pp, *sp) > o.settings.range + kWalkAwaySlack) return Stop{"you walked away from the station"};
    }
    if (const uint32_t missing = missing_input(o.recipe, carried(w, o.player)))
        return Stop{"out of " + pretty_item(name_of(missing))};
    return Stop{};
}

// Subscribes to the crafts (to learn them) and the pickups (to count them). False when the game refused either: then
// Bulk Craft cannot see crafts on this build and refuses orders (blind_text).
template <class OnCraft, class OnPickup>
bool watch_events(OnCraft on_craft, OnPickup on_pickup) {
    const bool crafts = emf::on_event<emf::gen::ecs::CraftRecipeEvent>(EMF_WORLD_SERVER, std::move(on_craft));
    const bool pickups = emf::on_event<emf::gen::ecs::PickupEvent>(EMF_WORLD_SERVER, std::move(on_pickup));
    if (!crafts) EMF_LOG("bulk_craft", "could not watch CraftRecipeEvent: Bulk Craft refuses orders");
    if (!pickups) EMF_LOG("bulk_craft", "could not watch PickupEvent: Bulk Craft refuses orders");
    return crafts && pickups;
}
inline std::string blind_text() {
    return "Bulk Craft can't see crafts on this build (the game refused its event watch; see the log): it makes nothing";
}

// The command came from the game's chat (a player, maybe an admin), not from the host's console or emf\commands.txt.
inline bool sender_from_chat() {
    EmfCommandSender s{};
    s.size = sizeof s;
    return emf::command_sender(&s) && s.from_chat;
}

}  // namespace bulkcraft
