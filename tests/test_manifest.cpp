#include <doctest.h>

#include <string>

#include "core/manifest.h"
#include "emf/emf_api.h"

using namespace emf::core;

namespace {
const char* kFull = R"({
  "id": "clock", "name": "Clock", "version": "1.1.0",
  "description": "The time of day on screen.", "authors": ["someone", "another"],
  "emf": 13,
  "depends": {"other_mod": ">=1.2", "third": "*"},
  "settings": [
    {"key": "place", "type": "choice", "default": 1, "choices": ["top right", "bottom right", "bottom left"],
     "label": "Corner", "help": "Where the clock sits.", "live": true},
    {"key": "h24", "type": "bool", "default": true, "label": "24-hour clock", "live": true},
    {"key": "radius", "type": "int", "default": 10, "min": 2, "max": 50},
    {"key": "multiplier", "type": "float", "default": 4.0, "min": 1, "max": 100},
    {"key": "key", "type": "key", "default": "L"},
    {"key": "guid", "type": "text", "default": "d5d05217-52dc-467a-9192-26b4f661d8d9"}
  ],
  "something_new": [1, 2]
})";
}  // namespace

TEST_CASE("manifest: a full manifest parses with every field") {
    auto r = parse_manifest(kFull);
    REQUIRE_MESSAGE(r.manifest, r.error);
    const Manifest& m = *r.manifest;
    CHECK(m.id == "clock");
    CHECK(m.name == "Clock");
    CHECK(m.version_text == "1.1.0");
    CHECK(compare(m.version, Version{1, 1, 0}) == 0);
    CHECK(m.description == "The time of day on screen.");
    REQUIRE(m.authors.size() == 2);
    CHECK(m.authors[1] == "another");
    CHECK(m.emf == 13);
    CHECK(m.dll == "clock.dll");  // default: <id>.dll
    CHECK(m.main.empty());
    CHECK(m.enabled_default);
    REQUIRE(m.depends.size() == 2);
    CHECK(m.depends[0].id == "other_mod");
    CHECK(m.depends[0].version.text() == ">=1.2");
    CHECK_FALSE(m.depends[0].optional);
    CHECK(m.depends[1].version.any());
    REQUIRE(m.settings.size() == 6);
    const SettingDef& place = m.settings[0];
    CHECK(place.key == "place");
    CHECK(place.type == SettingType::Choice);
    CHECK(place.default_value == "1");
    CHECK(place.choices.size() == 3);
    CHECK(place.label == "Corner");
    CHECK(place.help == "Where the clock sits.");
    CHECK(place.live);
    CHECK(m.settings[1].type == SettingType::Bool);
    CHECK(m.settings[1].default_value == "1");
    CHECK(m.settings[2].type == SettingType::Int);
    CHECK(m.settings[2].has_min);
    CHECK(m.settings[2].min == 2.0);
    CHECK(m.settings[2].max == 50.0);
    CHECK_FALSE(m.settings[2].live);
    CHECK(m.settings[2].label == "radius");  // default label: the key
    CHECK(m.settings[3].type == SettingType::Float);
    CHECK(m.settings[3].default_value == "4");
    CHECK(m.settings[4].type == SettingType::Key);
    CHECK(m.settings[4].default_value == "L");
    CHECK(m.settings[5].type == SettingType::Text);
}

TEST_CASE("manifest: defaults and the dll / main / enabled fields") {
    auto r = parse_manifest(R"({"id": "tiny", "version": "2"})");
    REQUIRE_MESSAGE(r.manifest, r.error);
    CHECK(r.manifest->name == "tiny");
    CHECK(compare(r.manifest->version, Version{2, 0, 0}) == 0);
    CHECK(r.manifest->emf == 0);
    CHECK(r.manifest->settings.empty());

    r = parse_manifest(R"({"id": "luamod", "version": "1.0", "main": "main.lua", "enabled": false})");
    REQUIRE(r.manifest);
    CHECK(r.manifest->main == "main.lua");
    CHECK(r.manifest->dll.empty());  // a script mod has no DLL
    CHECK_FALSE(r.manifest->enabled_default);

    r = parse_manifest(R"({"id": "custom", "version": "1.0", "dll": "bin/custom_x64.dll"})");
    REQUIRE(r.manifest);
    CHECK(r.manifest->dll == "bin/custom_x64.dll");

    r = parse_manifest(R"({"id": "c", "version": "1", "settings": [{"key": "mode", "type": "choice",
        "default": "fast", "choices": ["slow", "fast"]}]})");
    REQUIRE(r.manifest);
    CHECK(r.manifest->settings[0].default_value == "1");  // a choice default given by name becomes its index
}

TEST_CASE("manifest: errors name the field") {
    auto err = [](const char* text) {
        auto r = parse_manifest(text);
        CHECK_FALSE(r.manifest);
        return r.error;
    };
    CHECK(err(R"({"version": "1"})").find("id") != std::string::npos);
    CHECK(err(R"({"id": "Bad Id", "version": "1"})").find("id") != std::string::npos);
    CHECK(err(R"({"id": "x"})").find("version") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "one"})").find("version") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "depends": {"y": ">>1"}})").find("depends") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "settings": [{"key": "a", "type": "colour"}]})").find("type") !=
          std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "settings": [{"key": "a", "type": "choice"}]})").find("choices") !=
          std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "settings": [{"key": "a", "type": "int", "default": 99, "max": 5}]})")
              .find("default") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "settings": [{"key": "a", "type": "int"}, {"key": "a", "type": "int"}]})")
              .find("twice") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "settings": [{"key": "enabled", "type": "bool"}]})").find("enabled") !=
          std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "dll": "a.dll", "main": "main.lua"})").find("main") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "dll": "../escape.dll"})").find("dll") != std::string::npos);
    CHECK(err("[1, 2]").find("object") != std::string::npos);
    CHECK(err(R"({"id": "x", )").find("line") != std::string::npos);  // JSON errors keep their position
}

TEST_CASE("manifest: a file over 256 KB, a UTF-16 file and out-of-range whole numbers are refused with a clear reason") {
    auto err = [](const std::string& text) {
        auto r = parse_manifest(text);
        CHECK_FALSE(r.manifest);
        return r.error;
    };
    const std::string big = R"({"id": "x", "version": "1", "description": ")" + std::string(300 * 1024, 'a') + "\"}";
    CHECK(err(big).find("256 KB") != std::string::npos);

    const std::string ok = R"({"id": "x", "version": "1"})";
    auto utf16 = [&](bool bom, bool big_endian) {
        std::string s;
        if (bom) s += big_endian ? "\xFE\xFF" : "\xFF\xFE";
        for (char c : ok) {
            if (big_endian) s += '\0';
            s += c;
            if (!big_endian) s += '\0';
        }
        return s;
    };
    CHECK(err(utf16(true, false)).find("save mod.json as UTF-8") != std::string::npos);
    CHECK(err(utf16(true, true)).find("save mod.json as UTF-8") != std::string::npos);
    CHECK(err(utf16(false, false)).find("save mod.json as UTF-8") != std::string::npos);

    CHECK(err(R"({"id": "x", "version": "1", "emf": 1e300})").find("\"emf\"") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "emf": -1})").find("\"emf\"") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "emf": 13.5})").find("\"emf\"") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "emf": "13"})").find("\"emf\"") != std::string::npos);
    auto r = parse_manifest(R"({"id": "x", "version": "1", "emf": 16})");
    REQUIRE(r.manifest);
    CHECK(r.manifest->emf == 16);

    CHECK(err(R"({"id": "x", "version": "1", "settings": [{"key": "a", "type": "int", "default": 1e300}]})")
              .find("default") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "settings": [{"key": "a", "type": "int", "max": 1e300}]})")
              .find("max") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "settings": [{"key": "a", "type": "int", "min": -1e300}]})")
              .find("min") != std::string::npos);
    CHECK(parse_manifest(R"({"id": "x", "version": "1", "settings": [{"key": "a", "type": "float", "max": 1e300}]})")
              .manifest);  // floats may be large
}

TEST_CASE("manifest: ids and setting keys") {
    CHECK(valid_mod_id("clock"));
    CHECK(valid_mod_id("my_mod-2.x"));
    CHECK_FALSE(valid_mod_id(""));
    CHECK_FALSE(valid_mod_id("Clock"));  // lower case only
    CHECK_FALSE(valid_mod_id("a b"));
    CHECK_FALSE(valid_mod_id(std::string(49, 'a')));
    CHECK(valid_setting_key("seconds_per_item"));
    CHECK(valid_setting_key("Key.Mode-2"));
    CHECK_FALSE(valid_setting_key("a b"));
    CHECK_FALSE(valid_setting_key(""));
}

TEST_CASE("manifest: versions compare numerically; constraints") {
    CHECK(parse_version("1.10.0"));
    CHECK_FALSE(parse_version(""));
    CHECK_FALSE(parse_version("1.2.3.4"));
    CHECK_FALSE(parse_version("v1"));
    CHECK_FALSE(parse_version("1..2"));
    CHECK(compare(*parse_version("1.10"), *parse_version("1.9.9")) > 0);
    CHECK(compare(*parse_version("1.2"), *parse_version("1.2.0")) == 0);
    CHECK(compare(*parse_version("0.9"), *parse_version("1")) < 0);

    auto c = parse_constraint(">=1.2");
    REQUIRE(c);
    CHECK(satisfies(*parse_version("1.2.0"), *c));
    CHECK(satisfies(*parse_version("1.10"), *c));
    CHECK_FALSE(satisfies(*parse_version("1.1.9"), *c));
    c = parse_constraint("=1.2.3");
    REQUIRE(c);
    CHECK(satisfies(*parse_version("1.2.3"), *c));
    CHECK_FALSE(satisfies(*parse_version("1.2.4"), *c));
    c = parse_constraint("*");
    REQUIRE(c);
    CHECK(satisfies(*parse_version("0.0.1"), *c));
    c = parse_constraint(" >= 2 ");
    REQUIRE(c);
    CHECK(c->text() == ">= 2");
    CHECK(c->describe() == "2.0.0 or newer");
    CHECK_FALSE(parse_constraint(">>1"));
    CHECK_FALSE(parse_constraint(""));
}

#include <fstream>
#include <sstream>

TEST_CASE("manifest: every shipped mod's mod.json parses, matches its folder and needs no newer API") {
    for (const char* id : {"companion", "villager_jobs", "quick_stack", "clock", "big_stacks", "sample_mod", "event_log",
                           "sim_rate"}) {
        std::ifstream f(std::string(EMF_SOURCE_DIR) + "/mods/" + id + "/mod.json", std::ios::binary);
        REQUIRE_MESSAGE(f.good(), id);
        std::stringstream ss;
        ss << f.rdbuf();
        auto r = parse_manifest(ss.str());
        const std::string why = std::string(id) + ": " + r.error;
        REQUIRE_MESSAGE(r.manifest, why);
        CHECK(r.manifest->id == id);
        CHECK(r.manifest->dll == std::string(id) + ".dll");
        CHECK(r.manifest->emf <= EMF_API_VERSION);
        // a fresh install shows only the clock and Quick Stack (L); every other mod is switched on by the player
        const bool off_by_default = std::string(id) == "big_stacks" || std::string(id) == "sim_rate" ||
                                    std::string(id) == "companion" || std::string(id) == "villager_jobs";
        CHECK(r.manifest->enabled_default == !off_by_default);
    }
}

TEST_CASE("manifest: depends as an object (a range, or {version, optional}) or as a list of ids and objects") {
    auto r = parse_manifest(R"({"id": "x", "version": "1", "depends": {
        "lua": "^1.3", "helper": {"version": "~2.1", "optional": true}, "other": {"optional": false}, "any": "*"}})");
    REQUIRE_MESSAGE(r.manifest, r.error);
    const auto& d = r.manifest->depends;
    REQUIRE(d.size() == 4);
    CHECK(d[0].id == "lua");
    CHECK(d[0].version.text() == "^1.3");
    CHECK_FALSE(d[0].optional);
    CHECK(d[1].id == "helper");
    CHECK(d[1].version.text() == "~2.1");
    CHECK(d[1].optional);
    CHECK(d[2].version.any());  // no version given: any
    CHECK_FALSE(d[2].optional);
    CHECK(d[3].version.any());

    r = parse_manifest(R"({"id": "x", "version": "1", "depends": [
        "plain", {"id": "helper", "version": ">=1.0.0, <2.0.0", "optional": true}, {"id": "lib"}]})");
    REQUIRE_MESSAGE(r.manifest, r.error);
    REQUIRE(r.manifest->depends.size() == 3);
    CHECK(r.manifest->depends[0].id == "plain");
    CHECK(r.manifest->depends[0].version.any());
    CHECK(r.manifest->depends[1].optional);
    CHECK(r.manifest->depends[1].version.describe() == "1.0.0 or newer, below 2.0.0");
    CHECK(r.manifest->depends[2].id == "lib");
}

TEST_CASE("manifest: a dependency EMF cannot read rejects the mod with a reason naming it") {
    auto err = [](const char* text) {
        auto r = parse_manifest(text);
        CHECK_FALSE(r.manifest);
        return r.error;
    };
    std::string e = err(R"({"id": "x", "version": "1", "depends": {"helper": "^1.2.x.4"}})");
    CHECK(e.find("\"depends\": \"helper\"") == 0);
    e = err(R"({"id": "x", "version": "1", "depends": {"helper": "1.2.3-beta"}})");
    CHECK(e.find("pre-release") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "depends": {"Bad Id": "*"}})").find("not a valid mod id") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "depends": {"helper": 2}})").find("helper") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "depends": {"helper": {"optional": "yes"}}})").find("optional") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "depends": [{"version": "1"}]})").find("depends") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "depends": ["a", "a"]})").find("twice") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "depends": {"x": "*"}})").find("itself") != std::string::npos);
    CHECK(err(R"({"id": "x", "version": "1", "depends": "lua"})").find("depends") != std::string::npos);
}

TEST_CASE("manifest: the shipped Lua mods and both templates parse (their dependencies and every setting kind)") {
    for (const char* path : {"/mods/kill_counter/mod.json", "/mods/life_skills/mod.json", "/mods/lua/mod.json",
                             "/template/mod.json", "/template_lua/mod.json"}) {
        std::ifstream f(std::string(EMF_SOURCE_DIR) + path, std::ios::binary);
        REQUIRE_MESSAGE(f.good(), path);
        std::stringstream ss;
        ss << f.rdbuf();
        auto r = parse_manifest(ss.str());
        const std::string why = std::string(path) + ": " + r.error;
        REQUIRE_MESSAGE(r.manifest, why);
        CHECK(r.manifest->emf <= EMF_API_VERSION);
    }
}

TEST_CASE("manifest: Night Raids, Loot Magnet and Wandering Champion parse with every setting clean") {
    struct Want {
        const char* id;
        const char* dll;  // "" for the Lua mod
        uint32_t emf;
        bool on;          // on by default (all three are installed switched off; Night Raids then also starts off in every world)
    };
    for (const Want& m : {Want{"raids", "raids.dll", 17, false}, Want{"loot_magnet", "loot_magnet.dll", 17, false},
                          Want{"champion", "", 14, false}}) {
        std::ifstream f(std::string(EMF_SOURCE_DIR) + "/mods/" + m.id + "/mod.json", std::ios::binary);
        REQUIRE_MESSAGE(f.good(), m.id);
        std::stringstream ss;
        ss << f.rdbuf();
        auto r = parse_manifest(ss.str());
        const std::string why = std::string(m.id) + ": " + r.error;
        REQUIRE_MESSAGE(r.manifest, why);
        CHECK(r.manifest->id == m.id);
        CHECK(r.manifest->dll == m.dll);
        CHECK(r.manifest->emf == m.emf);
        CHECK(r.manifest->enabled_default == m.on);
        CHECK(r.manifest->settings.size() >= 5);
        bool has_new_kind = false;
        for (const auto& s : r.manifest->settings) {
            CAPTURE(m.id);
            CAPTURE(s.key);
            CHECK(s.problem.empty());
            CHECK(s.warning.empty());
            has_new_kind |= s.strict;
        }
        CHECK(has_new_kind);  // they show off the richer kinds (slider, select, keybind, color)
    }
}

TEST_CASE("manifest: ShroudForge's \"dependencies\" is read as \"depends\"; both at once is refused") {
    auto r = parse_manifest(R"({"id": "x", "version": "1", "dependencies": [{"id": "helper-mod", "version": "^1.2.0"},
        {"id": "extra", "version": ">=2.0.0, <3.0.0", "optional": true}]})");
    REQUIRE_MESSAGE(r.manifest, r.error);
    REQUIRE(r.manifest->depends.size() == 2);
    CHECK(r.manifest->depends[0].id == "helper-mod");
    CHECK(r.manifest->depends[1].optional);
    r = parse_manifest(R"({"id": "x", "version": "1", "dependencies": []})");
    REQUIRE(r.manifest);
    CHECK(r.manifest->depends.empty());
    r = parse_manifest(R"({"id": "x", "version": "1", "depends": {"a": "*"}, "dependencies": [{"id": "b"}]})");
    CHECK_FALSE(r.manifest);
    CHECK(r.error == "give either \"depends\" or \"dependencies\" (ShroudForge's name for it), not both");
    r = parse_manifest(R"({"id": "x", "version": "1", "dependencies": {"a": 3}})");
    CHECK_FALSE(r.manifest);
    CHECK(r.error.find("\"dependencies\": \"a\"") == 0);  // errors name the field as the mod wrote it
}
