#include "state.h"

#include <cstdio>

#include "json_out.h"

namespace emf::core::bridge {

namespace {

template <class T>
std::string opt_num(const std::optional<T>& v) {
    return v ? json_number(static_cast<double>(*v)) : "null";
}

std::string xyz(double x, double y, double z) {
    return JsonObj().num("x", x).num("y", y).num("z", z).text();
}

std::string steps_json(const std::optional<StateSnapshot::Steps>& s) {
    if (!s) return "null";
    return JsonObj().num("steps", static_cast<double>(s->count)).raw("interval_ms", opt_num(s->interval_ms)).text();
}

}  // namespace

std::string mods_json(const std::vector<ModRecord>& mods) {
    std::vector<std::string> items;
    for (const auto& m : mods) {
        JsonObj o;
        o.str("id", m.id).str("name", m.name).str("version", m.version).str("status", mod_status_name(m.status));
        o.str("reason", m.reason).str("source", m.source).str("authors", m.authors);
        o.boolean("enabled_default", m.enabled_default);
        items.push_back(o.text());
    }
    return json_list(items);
}

std::string state_json(const StateSnapshot& s) {
    JsonObj world;
    world.str("status", s.world_status);
    world.raw("kind", s.world_kind ? json_quote(*s.world_kind) : "null");
    world.raw("tick", opt_num(s.world_tick)).raw("ecs_time_s", opt_num(s.ecs_time_s));
    world.raw("time_of_day_h", opt_num(s.time_of_day_h)).raw("slot", opt_num(s.world_slot));
    world.raw("name", s.world_name ? json_quote(*s.world_name) : "null");

    std::string player = "null";
    if (s.player_id) {
        JsonObj p;
        p.num("id", *s.player_id);
        p.raw("position", s.position ? xyz((*s.position)[0], (*s.position)[1], (*s.position)[2]) : "null");
        std::string health = "null";
        if (s.health) {
            JsonObj h;
            h.num("value", static_cast<double>(s.health->value)).raw("max", opt_num(s.health->max));
            health = h.str("source", s.health->source).text();
        }
        p.raw("health", health);
        player = p.text();
    }

    std::vector<std::string> hud;
    for (const auto& l : s.hud)
        hud.push_back(JsonObj().str("id", l.id).str("text", l.text).num("place", static_cast<int>(l.place)).text());
    JsonObj overlay;
    overlay.boolean("running", s.overlay_running).boolean("console_open", s.console_open);
    overlay.boolean("wheel_open", s.wheel_open).raw("hud", json_list(hud));

    JsonObj hooks;
    hooks.num("total", static_cast<double>(s.hooks_total)).num("emf", static_cast<double>(s.hooks_emf));
    hooks.num("mods", static_cast<double>(s.hooks_mods));

    JsonObj ticks;
    ticks.raw("server", steps_json(s.server_steps)).raw("client", steps_json(s.client_steps));

    std::string frame = "null";
    if (s.presents)
        frame = JsonObj().num("presents", static_cast<double>(*s.presents)).raw("frame_ms", opt_num(s.frame_ms)).text();

    std::string character = "null";
    if (s.character) {
        JsonObj c;
        c.str("storage", s.character->storage).raw("slot", opt_num(s.character->slot));
        character = c.raw("name", s.character->name ? json_quote(*s.character->name) : "null").text();
    }

    JsonObj o;
    o.raw("world", world.text()).raw("character", character).raw("player", player).raw("overlay", overlay.text());
    o.raw("mods", mods_json(s.mods));
    o.raw("hooks", hooks.text()).raw("ticks", ticks.text()).raw("frame", frame);
    return o.text();
}

std::optional<StateSnapshot::Health> pick_health(std::optional<std::pair<uint32_t, uint32_t>> network_health,
                                                 std::optional<std::pair<int32_t, int32_t>> attributes,
                                                 std::optional<int32_t> server_health_slot0) {
    if (attributes && attributes->second >= 0)
        return StateSnapshot::Health{attributes->first, static_cast<uint32_t>(attributes->second), "attributes"};
    if (network_health) return StateSnapshot::Health{network_health->first, network_health->second, "NetworkHealth"};
    if (server_health_slot0) return StateSnapshot::Health{*server_health_slot0, std::nullopt, "server Health slot 0"};
    return std::nullopt;
}

const std::vector<std::string>& game_event_names() {
    static const std::vector<std::string> k = {"pickup", "kill", "damage", "save", "load"};
    return k;
}

std::string pickup_fields(uint32_t item_id, const std::string& item_name, uint32_t count, uint32_t by, uint32_t from,
                          const std::array<float, 3>& position) {
    char id[16];
    std::snprintf(id, sizeof id, "0x%08x", item_id);
    JsonObj o;
    o.raw("item", item_name.empty() ? "null" : json_quote(item_name)).str("item_id", id).num("count", count);
    o.num("by", by).num("from", from).raw("position", xyz(position[0], position[1], position[2]));
    return o.text();
}

std::string kill_fields(uint32_t entity) { return JsonObj().num("entity", entity).text(); }

std::string damage_fields(uint32_t target, uint32_t source, int32_t health_change, int32_t stamina_change) {
    return JsonObj().num("target", target).num("source", source).num("health", health_change).num("stamina", stamina_change).text();
}

std::string slot_fields(uint16_t world_slot) { return JsonObj().num("slot", world_slot).text(); }

}  // namespace emf::core::bridge
