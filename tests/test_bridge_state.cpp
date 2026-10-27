// The bridge's state, mods and game event JSON: known values as numbers and text, unknown ones as null.
#include <doctest.h>

#include <string>

#include "core/bridge/state.h"
#include "core/json.h"

using namespace emf::core;
using namespace emf::core::bridge;

namespace {
json::Value parse(const std::string& s) {
    auto r = json::parse(s);
    REQUIRE_MESSAGE(r.value, s);
    return *r.value;
}
bool is_null(const json::Value& v, const char* k) {
    const json::Value* m = v.get(k);
    return m && m->is(json::Value::Type::Null);
}
}  // namespace

TEST_CASE("bridge state: nothing known is null, not guessed") {
    StateSnapshot s;
    const auto v = parse(state_json(s));
    const json::Value* world = v.get("world");
    REQUIRE(world);
    CHECK(world->get("status")->s == "none");
    CHECK(is_null(*world, "kind"));
    CHECK(is_null(*world, "tick"));
    CHECK(is_null(*world, "time_of_day_h"));
    CHECK(is_null(*world, "slot"));
    CHECK(is_null(v, "player"));
    CHECK(is_null(v, "frame"));
    CHECK(is_null(*v.get("ticks"), "server"));
    CHECK(v.get("overlay")->get("running")->b == false);
    CHECK(v.get("mods")->arr.empty());
}

TEST_CASE("bridge state: a running world with the player, the overlay, mods, hooks, steps and frames") {
    StateSnapshot s;
    s.world_status = "running";
    s.world_kind = "server";
    s.world_tick = 1234;
    s.ecs_time_s = 20.5;
    s.time_of_day_h = 13.25;
    s.world_slot = 1;
    s.player_id = 7;
    s.position = std::array<double, 3>{1.5, -2.0, 300.25};
    s.overlay_running = true;
    s.console_open = true;
    s.hud.push_back(HudLine{"clock", "13:15", HudPlace::BottomRight, 0});
    ModRecord m;
    m.id = "clock";
    m.name = "Clock";
    m.version = "1.1.0";
    m.status = ModStatus::Disabled;
    m.reason = "switched off";
    m.source = "clock\\";
    s.mods.push_back(m);
    s.hooks_total = 11;
    s.hooks_emf = 9;
    s.hooks_mods = 2;
    s.server_steps = StateSnapshot::Steps{600, 16.7};
    s.client_steps = StateSnapshot::Steps{590, std::nullopt};
    s.presents = 5000;
    s.frame_ms = 16.6;
    const auto v = parse(state_json(s));
    const json::Value& world = *v.get("world");
    CHECK(world.get("status")->s == "running");
    CHECK(world.get("kind")->s == "server");
    CHECK(world.get("tick")->n == 1234);
    CHECK(world.get("ecs_time_s")->n == doctest::Approx(20.5));
    CHECK(world.get("time_of_day_h")->n == doctest::Approx(13.25));
    CHECK(world.get("slot")->n == 1);
    const json::Value& player = *v.get("player");
    CHECK(player.get("id")->n == 7);
    CHECK(player.get("position")->get("x")->n == doctest::Approx(1.5));
    CHECK(player.get("position")->get("z")->n == doctest::Approx(300.25));
    CHECK(is_null(player, "health"));  // not readable here: null
    const json::Value& overlay = *v.get("overlay");
    CHECK(overlay.get("console_open")->b);
    CHECK_FALSE(overlay.get("wheel_open")->b);
    REQUIRE(overlay.get("hud")->arr.size() == 1);
    CHECK(overlay.get("hud")->arr[0].get("text")->s == "13:15");
    CHECK(overlay.get("hud")->arr[0].get("place")->n == 1);
    REQUIRE(v.get("mods")->arr.size() == 1);
    CHECK(v.get("mods")->arr[0].get("status")->s == "disabled");
    CHECK(v.get("mods")->arr[0].get("reason")->s == "switched off");
    CHECK(v.get("hooks")->get("mods")->n == 2);
    CHECK(v.get("ticks")->get("server")->get("steps")->n == 600);
    CHECK(v.get("ticks")->get("server")->get("interval_ms")->n == doctest::Approx(16.7));
    CHECK(is_null(*v.get("ticks")->get("client"), "interval_ms"));
    CHECK(v.get("frame")->get("presents")->n == 5000);
}

TEST_CASE("bridge state: a player whose position is unknown keeps its id; health when known") {
    StateSnapshot s;
    s.player_id = 3;
    s.health = StateSnapshot::Health{80, 120u, "NetworkHealth"};
    const auto v = parse(state_json(s));
    CHECK(v.get("player")->get("id")->n == 3);
    CHECK(is_null(*v.get("player"), "position"));
    CHECK(v.get("player")->get("health")->get("value")->n == 80);
    CHECK(v.get("player")->get("health")->get("max")->n == 120);
    CHECK(v.get("player")->get("health")->get("source")->s == "NetworkHealth");
}

TEST_CASE("bridge state: health from the server's Health slot 0 has no max") {
    StateSnapshot s;
    s.player_id = 3;
    s.health = StateSnapshot::Health{1498, std::nullopt, "server Health slot 0"};
    const auto v = parse(state_json(s));
    const json::Value& h = *v.get("player")->get("health");
    CHECK(h.get("value")->n == 1498);
    CHECK(is_null(h, "max"));
    CHECK(h.get("source")->s == "server Health slot 0");
}

TEST_CASE("bridge state: health prefers the attributes (with max), then NetworkHealth, then the server's Health slot 0") {
    CHECK_FALSE(pick_health(std::nullopt, std::nullopt, std::nullopt).has_value());
    const auto server = pick_health(std::nullopt, std::nullopt, 950);
    REQUIRE(server.has_value());
    CHECK(server->value == 950);
    CHECK_FALSE(server->max.has_value());
    CHECK(server->source == "server Health slot 0");
    const auto net = pick_health(std::make_pair(180u, 200u), std::nullopt, 950);
    REQUIRE(net.has_value());
    CHECK(net->value == 180);
    CHECK(net->max == 200u);
    CHECK(net->source == "NetworkHealth");
    const auto attrs = pick_health(std::make_pair(180u, 200u), std::make_pair(700, 1498), 950);
    REQUIRE(attrs.has_value());
    CHECK(attrs->value == 700);
    CHECK(attrs->max == 1498u);
    CHECK(attrs->source == "attributes");
}

TEST_CASE("bridge state: the world's name when it is known, else null") {
    StateSnapshot s;
    CHECK(is_null(*parse(state_json(s)).get("world"), "name"));
    s.world_status = "running";
    s.world_name = "My World 1";
    CHECK(parse(state_json(s)).get("world")->get("name")->s == "My World 1");
}

TEST_CASE("bridge state: the mod list") {
    ModRecord m;
    m.id = "life_skills";
    m.name = "Life Skills";
    m.version = "1.0.0";
    m.authors = "EMF";
    m.status = ModStatus::Script;
    m.source = "life_skills\\";
    m.enabled_default = true;
    const auto v = parse(mods_json({m}));
    REQUIRE(v.arr.size() == 1);
    CHECK(v.arr[0].get("id")->s == "life_skills");
    CHECK(v.arr[0].get("status")->s == "script");
    CHECK(v.arr[0].get("source")->s == "life_skills\\");
    CHECK(v.arr[0].get("enabled_default")->b);
}

TEST_CASE("bridge events: fields of pickup, kill, damage, save and load") {
    CHECK(game_event_names() == std::vector<std::string>{"pickup", "kill", "damage", "save", "load"});
    auto v = parse(pickup_fields(0x7c31fc55u, "Material_T1_Wood_Logs", 5, 12, 34, {1.0f, 2.0f, 3.0f}));
    CHECK(v.get("item")->s == "Material_T1_Wood_Logs");
    CHECK(v.get("item_id")->s == "0x7c31fc55");
    CHECK(v.get("count")->n == 5);
    CHECK(v.get("by")->n == 12);
    CHECK(v.get("from")->n == 34);
    CHECK(v.get("position")->get("y")->n == doctest::Approx(2.0));
    v = parse(pickup_fields(0x1u, "", 1, 2, 0, {0, 0, 0}));
    CHECK(is_null(v, "item"));  // no name known
    v = parse(kill_fields(99));
    CHECK(v.get("entity")->n == 99);
    v = parse(damage_fields(5, 6, -12, -3));
    CHECK(v.get("target")->n == 5);
    CHECK(v.get("source")->n == 6);
    CHECK(v.get("health")->n == -12);
    CHECK(v.get("stamina")->n == -3);
    v = parse(slot_fields(2));
    CHECK(v.get("slot")->n == 2);
}

TEST_CASE("bridge state: the character (storage, slot, name); unknown by default; null on the dedicated server") {
    StateSnapshot s;
    json::Value v = parse(state_json(s));
    const json::Value* c = v.get("character");
    REQUIRE(c);
    CHECK(c->get("storage")->s == "unknown");
    CHECK(is_null(*c, "slot"));
    CHECK(is_null(*c, "name"));
    s.character = StateSnapshot::Character{"local", 1u, std::string("Character 2")};
    v = parse(state_json(s));
    c = v.get("character");
    CHECK(c->get("storage")->s == "local");
    CHECK(c->get("slot")->n == 1);
    CHECK(c->get("name")->s == "Character 2");
    s.character.reset();  // the dedicated server has no character of its own
    CHECK(is_null(parse(state_json(s)), "character"));
}
