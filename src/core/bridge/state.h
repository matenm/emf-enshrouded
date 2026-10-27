#pragma once
// The bridge's `state` and `mods` answers and its game events, as JSON. The core fills a StateSnapshot on the world's
// thread (or from values it tracks) and decodes events from their raw bytes; this file only writes them. Unknown
// values stay empty and come out as null, never guessed.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/mod_registry.h"
#include "core/overlay/hud_model.h"

namespace emf::core::bridge {

struct StateSnapshot {
    std::string world_status = "none";       // none | loading | running
    std::optional<std::string> world_kind;   // server (a world you host) | client (someone else's) | dedicated
    std::optional<uint32_t> world_tick;      // the world's step counter
    std::optional<double> ecs_time_s;        // the world's time
    std::optional<double> time_of_day_h;     // 0-24
    std::optional<uint32_t> world_slot;      // the save slot of the world last loaded or saved
    std::optional<std::string> world_name;   // the hosted world's display name (as the menu shows it), when known
    // The game's selected character (the one a running world was started with): storage local | steam | unknown, its
    // 1-based slot and name. Null on the dedicated server, which has no character of its own.
    struct Character {
        std::string storage = "unknown";
        std::optional<uint32_t> slot;
        std::optional<std::string> name;
    };
    std::optional<Character> character = Character{};
    std::optional<uint32_t> player_id;       // the local player's entity (none: "player": null)
    std::optional<std::array<double, 3>> position;            // metres
    struct Health {
        int64_t value = 0;
        std::optional<uint32_t> max;     // only when the source has one: never guessed
        std::string source;              // "attributes" | "NetworkHealth" | "server Health slot 0"
    };
    std::optional<Health> health;
    bool overlay_running = false;
    bool console_open = false;
    bool wheel_open = false;
    std::vector<HudLine> hud;
    std::vector<ModRecord> mods;
    size_t hooks_total = 0, hooks_emf = 0, hooks_mods = 0;
    struct Steps {
        uint64_t count = 0;                  // steps the bridge saw since it started
        std::optional<double> interval_ms;   // the recent time between steps
    };
    std::optional<Steps> server_steps, client_steps;
    std::optional<uint64_t> presents;        // frames through EMF's Vulkan layer (none: "frame": null)
    std::optional<double> frame_ms;
};

std::string state_json(const StateSnapshot& s);
// The player's health: in a world this game hosts, its current and max health attributes through the game's attribute
// registry (API v18); else NetworkHealth (value and max) when the player has one, which a client of someone else's game
// does; else the server's Health component's data slot 0 (the current health, no max); otherwise unknown.
std::optional<StateSnapshot::Health> pick_health(std::optional<std::pair<uint32_t, uint32_t>> network_health,
                                                 std::optional<std::pair<int32_t, int32_t>> attributes,
                                                 std::optional<int32_t> server_health_slot0);
// The mod list: id, name, version, status, why it does not load, source, authors, enabled by default.
std::string mods_json(const std::vector<ModRecord>& mods);

// ---- game events (`events`) ----
const std::vector<std::string>& game_event_names();  // pickup, kill, damage, save, load
// PickupEvent: the item (and its debug name when known), how many, who picked it up and from where.
std::string pickup_fields(uint32_t item_id, const std::string& item_name, uint32_t count, uint32_t by, uint32_t from,
                          const std::array<float, 3>& position);
std::string kill_fields(uint32_t entity);  // EntityDiedEvent
// HitEvent: target, root source, health and stamina change.
std::string damage_fields(uint32_t target, uint32_t source, int32_t health_change, int32_t stamina_change);
std::string slot_fields(uint16_t world_slot);  // on_save / on_load

}  // namespace emf::core::bridge
