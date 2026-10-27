// The richer settings in mod.json: slider (min, max, step), select (options), keybind, color, text with
// maxLength, group headings and enabledWhen; checked the same way by the manifest, the settings store (the Mods tab,
// the console's `set`, the API) and EMF.exe.
#include <doctest.h>

#include <map>
#include <string>
#include <vector>

#include "core/manifest.h"
#include "core/settings.h"

using namespace emf::core;

namespace {
struct MemIni : IniBackend {
    std::map<std::string, std::string> values;
    std::optional<std::string> read(const std::string& mod, const std::string& key) override {
        auto it = values.find(mod + "." + key);
        if (it == values.end()) return std::nullopt;
        return it->second;
    }
    bool write(const std::string& mod, const std::string& key, const std::string& value) override {
        values[mod + "." + key] = value;
        return true;
    }
};

// The one setting of a manifest with `setting_json` as its only setting.
SettingDef one(const std::string& setting_json) {
    auto r = parse_manifest(R"({"id": "m", "version": "1", "settings": [)" + setting_json + "]}");
    REQUIRE_MESSAGE(r.manifest, r.error);
    REQUIRE(r.manifest->settings.size() == 1);
    return r.manifest->settings[0];
}
std::string refused(const std::string& settings_json) {
    auto r = parse_manifest(R"({"id": "m", "version": "1", "settings": [)" + settings_json + "]}");
    CHECK_MESSAGE(!r.manifest, settings_json.c_str());
    return r.error;
}
// A mistake in a display field of an old kind: the manifest (and the mod) loads, and the last setting is refused when
// it is declared. Gives why ("" when it was declared).
std::string refused_setting(const std::string& settings_json) {
    auto r = parse_manifest(R"({"id": "m", "version": "1", "settings": [)" + settings_json + "]}");
    REQUIRE_MESSAGE(r.manifest, r.error);
    MemIni ini;
    SettingsStore store(&ini);
    std::string why;
    for (const auto& d : r.manifest->settings) {
        why.clear();
        if (store.declare("m", d, &why)) why.clear();
    }
    CHECK_MESSAGE(!why.empty(), settings_json.c_str());
    return why;
}
std::string value(const SettingDef& d, const std::string& text, bool clamp = false) {
    std::string why;
    auto v = setting_value(d, text, clamp, &why);
    return v ? *v : "refused: " + why;
}
}  // namespace

TEST_CASE("setting controls: a slider with a whole step (and whole min, max, default) holds whole numbers; otherwise decimals") {
    SettingDef d = one(R"({"key": "radius", "type": "slider", "min": 2, "max": 50, "step": 2, "default": 10})");
    CHECK(d.type == SettingType::Int);
    CHECK(d.slider);
    CHECK(d.step == 2.0);
    CHECK(d.default_value == "10");
    d = one(R"({"key": "speed", "type": "slider", "min": 0.5, "max": 3, "step": 0.25, "default": 1})");
    CHECK(d.type == SettingType::Float);
    CHECK(d.default_value == "1");
    d = one(R"({"key": "plain", "type": "slider", "min": 0, "max": 10})");
    CHECK(d.type == SettingType::Float);  // no step: decimals (whole numbers need a whole step)
    CHECK(d.default_value == "0");  // no default: min
    d = one(R"({"key": "half", "type": "slider", "min": 0, "max": 1, "default": 0.5})");
    CHECK(d.type == SettingType::Float);  // a decimal default makes it decimals
    CHECK(refused(R"({"key": "a", "type": "slider", "max": 5})").find("min") != std::string::npos);
    CHECK(refused(R"({"key": "a", "type": "slider", "min": 0, "max": 5, "step": 10})").find("step") != std::string::npos);
    CHECK(refused(R"({"key": "a", "type": "slider", "min": 0, "max": 5, "step": -1})").find("step") != std::string::npos);
    CHECK(refused(R"({"key": "a", "type": "slider", "min": 0, "max": 10, "step": 2, "default": 3})").find("steps of 2") !=
          std::string::npos);
    CHECK(refused_setting(R"({"key": "a", "type": "text", "step": 1})").find("step") != std::string::npos);
    CHECK(refused_setting(R"({"key": "a", "type": "int", "step": 0.5})").find("whole") != std::string::npos);
}

TEST_CASE("setting controls: a value between steps moves to the nearest step; out of range is refused or clamped") {
    const SettingDef d = one(R"({"key": "speed", "type": "slider", "min": 0.5, "max": 3, "step": 0.25})");
    CHECK(value(d, "1.3") == "1.25");
    CHECK(value(d, "1.4") == "1.5");
    CHECK(value(d, "0.5") == "0.5");
    CHECK(value(d, "3") == "3");
    CHECK(value(d, "3.5").rfind("refused: must be between 0.5 and 3", 0) == 0);
    CHECK(value(d, "3.5", true) == "3");  // from the ini: clamped
    const SettingDef tenths = one(R"({"key": "t", "type": "slider", "min": 0, "max": 1, "step": 0.1})");
    CHECK(value(tenths, "0.3") == "0.3");  // not 0.30000000000000004
    CHECK(value(tenths, "0.71") == "0.7");
    const SettingDef evens = one(R"({"key": "e", "type": "slider", "min": 0, "max": 9, "step": 2})");
    CHECK(value(evens, "9") == "8");  // the top step that fits under max
    CHECK(value(evens, "3") == "4");
    CHECK(value(evens, "2.6").rfind("refused: must be a whole number", 0) == 0);  // as without a step
    CHECK(value(evens, "2.6", true) == "2");  // from the ini: to the nearest step
    const SettingDef big = one(R"({"key": "b", "type": "int", "step": 1, "default": 123456789012345})");
    CHECK(big.default_value == "123456789012345");
    CHECK(value(big, "123456789012345") == "123456789012345");  // whole numbers stay exact
    const SettingDef fine = one(R"({"key": "f", "type": "slider", "min": 0, "max": 1000000, "step": 0.5})");
    CHECK(value(fine, "123456.5") == "123456.5");
}

TEST_CASE("setting controls: a select stores the option's value; the default and edits may use value or label") {
    SettingDef d = one(R"({"key": "corner", "type": "select", "default": "Bottom left",
        "options": [{"value": "top_right", "label": "Top right"}, {"value": "bottom_right", "label": "Bottom right"},
                    {"value": "bottom_left", "label": "Bottom left"}]})");
    CHECK(d.type == SettingType::Select);
    REQUIRE(d.options.size() == 3);
    CHECK(d.options[0].label == "Top right");
    CHECK(d.default_value == "bottom_left");  // given by label, stored as the value
    CHECK(value(d, "TOP_RIGHT") == "top_right");
    CHECK(value(d, "bottom right") == "bottom_right");
    CHECK(value(d, "middle").find("choose one of: top_right (Top right), bottom_right (Bottom right)") != std::string::npos);
    d = one(R"({"key": "mode", "type": "select", "options": ["slow", "fast"]})");
    CHECK(d.default_value == "slow");  // no default: the first option
    CHECK(d.options[1].label == "fast");  // a plain text is value and label
    CHECK(refused(R"({"key": "a", "type": "select"})").find("options") != std::string::npos);
    CHECK(refused(R"({"key": "a", "type": "select", "options": ["x", "X"]})").find("twice") != std::string::npos);
    CHECK(refused(R"({"key": "a", "type": "select", "options": [""]})").find("empty") != std::string::npos);
    CHECK(refused(R"({"key": "a", "type": "select", "options": [" padded"]})").find("spaces") != std::string::npos);
    CHECK(refused(R"({"key": "a", "type": "select", "options": ["x"], "default": "y"})").find("default") != std::string::npos);
    CHECK(refused(R"({"key": "a", "type": "select", "choices": ["x"]})").find("options") != std::string::npos);
    CHECK(refused_setting(R"({"key": "a", "type": "int", "options": ["x"]})").find("select") != std::string::npos);
}

TEST_CASE("setting controls: a keybind takes the key names EMF's hotkeys read, or nothing") {
    const SettingDef d = one(R"({"key": "toggle", "type": "keybind", "default": "f7"})");
    CHECK(d.type == SettingType::Key);
    MemIni ini;
    SettingsStore store(&ini);
    REQUIRE(store.declare("m", d));
    CHECK(store.get("m", "toggle") == "F7");  // the store names the key
    CHECK(value(d, "g") == "G");
    CHECK(value(d, "numpad5") == "Numpad5");
    CHECK(value(d, "") == "");  // no key
    CHECK(value(d, "0xBA") == "0xba");  // a key without a name keeps its number
    CHECK(value(d, "Mouse1").rfind("refused: not a key", 0) == 0);
    // a default that is no key refuses that setting when it is declared; the manifest (and the mod) still load
    const SettingDef bad = one(R"({"key": "a", "type": "keybind", "default": "NotAKey"})");
    std::string why;
    CHECK_FALSE(store.declare("m", bad, &why));
    CHECK(why == "setting \"a\": the default is not a key");
}

TEST_CASE("setting controls: a color is #rrggbb (#rgb and any case are read; the # may be left out)") {
    const SettingDef d = one(R"({"key": "tint", "type": "color", "default": "#FFD070"})");
    CHECK(d.type == SettingType::Color);
    CHECK(d.default_value == "#ffd070");
    CHECK(value(d, "#abc") == "#aabbcc");
    CHECK(value(d, "00FF80") == "#00ff80");
    CHECK(value(d, "red").rfind("refused: a colour like #ff8800", 0) == 0);
    CHECK(value(d, "#12345").rfind("refused", 0) == 0);
    CHECK(one(R"({"key": "c", "type": "color"})").default_value == "#ffffff");
    CHECK(refused(R"({"key": "a", "type": "color", "default": "blue"})").find("colour") != std::string::npos);
    CHECK(setting_type_name(SettingType::Color) == std::string("color"));
    CHECK(setting_type_name(SettingType::Select) == std::string("select"));
}

TEST_CASE("setting controls: text with maxLength counts characters, not bytes") {
    const SettingDef d = one(R"({"key": "name", "type": "text", "maxLength": 5, "default": "Hej"})");
    CHECK(d.max_length == 5);
    CHECK(value(d, "12345") == "12345");
    CHECK(value(d, "123456") == "refused: at most 5 characters");
    CHECK(value(d, "\xC3\xA6\xC3\xB8\xC3\xA5\xC3\xA6\xC3\xB8") == "\xC3\xA6\xC3\xB8\xC3\xA5\xC3\xA6\xC3\xB8");  // 5 letters, 10 bytes
    CHECK(refused_setting(R"({"key": "a", "type": "text", "maxLength": 3, "default": "four"})").find("maxLength") != std::string::npos);
    CHECK(refused_setting(R"({"key": "a", "type": "text", "maxLength": 0})").find("maxLength") != std::string::npos);
    CHECK(refused_setting(R"({"key": "a", "type": "text", "maxLength": 300})").find("maxLength") != std::string::npos);
    CHECK(refused_setting(R"({"key": "a", "type": "int", "maxLength": 3})").find("text") != std::string::npos);
}

TEST_CASE("setting controls: groups put settings under headings: ungrouped first, then each group in order") {
    auto r = parse_manifest(R"({"id": "m", "version": "1", "settings": [
        {"key": "a", "type": "bool", "group": "Display"},
        {"key": "b", "type": "bool"},
        {"key": "c", "type": "bool", "group": "Keys"},
        {"key": "d", "type": "bool", "group": "Display"},
        {"key": "e", "type": "bool"}]})");
    REQUIRE_MESSAGE(r.manifest, r.error);
    CHECK(r.manifest->settings[0].group == "Display");
    const auto order = setting_display_order(r.manifest->settings);
    CHECK(order == std::vector<size_t>{1, 4, 0, 3, 2});  // b e | Display: a d | Keys: c
    CHECK(refused_setting(R"({"key": "a", "type": "bool", "group": 3})").find("group") != std::string::npos);
    CHECK(refused_setting(R"({"key": "a", "type": "bool", "group": ")" + std::string(65, 'g') + R"("})").find("group") !=
          std::string::npos);
}

TEST_CASE("setting controls: enabledWhen names another setting and one of its values (checked and normalised)") {
    auto r = parse_manifest(R"({"id": "m", "version": "1", "settings": [
        {"key": "text", "type": "text", "enabledWhen": {"setting": "show", "equals": true}},
        {"key": "show", "type": "bool", "default": true, "label": "Show the line"},
        {"key": "corner", "type": "select", "options": [{"value": "tr", "label": "Top right"}, "bl"]},
        {"key": "offset", "type": "int", "enabledWhen": {"setting": "corner", "equals": "Top right"}},
        {"key": "key", "type": "keybind", "default": "F7"},
        {"key": "repeat", "type": "bool", "enabledWhen": {"setting": "key", "equals": "f7"}}]})");
    REQUIRE_MESSAGE(r.manifest, r.error);
    const auto& s = r.manifest->settings;
    CHECK(s[0].when_key == "show");
    CHECK(s[0].when_value == "1");
    CHECK(s[3].when_value == "tr");  // a label becomes the value
    CHECK(s[5].when_value == "F7");
    std::map<std::string, std::string> now{{"show", "1"}, {"corner", "bl"}, {"key", "F7"}};
    const auto value_of = [&](const std::string& k) { return now[k]; };
    CHECK(setting_active(s[0], value_of));
    CHECK_FALSE(setting_active(s[3], value_of));
    CHECK(setting_active(s[1], value_of));  // no condition
    now["show"] = "0";
    CHECK_FALSE(setting_active(s[0], value_of));
    CHECK(setting_condition_text(s[0], s) == "only while Show the line is on");
    CHECK(setting_condition_text(s[3], s) == "only while corner is Top right");
    CHECK(setting_condition_text(s[1], s).empty());

    CHECK(refused_setting(R"({"key": "a", "type": "bool", "enabledWhen": {"setting": "nope", "equals": true}})").find("nope") !=
          std::string::npos);
    CHECK(refused_setting(R"({"key": "a", "type": "bool", "enabledWhen": {"setting": "a", "equals": true}})").find("itself") !=
          std::string::npos);
    CHECK(refused_setting(R"({"key": "a", "type": "bool"}, {"key": "b", "type": "int", "enabledWhen": {"setting": "a", "equals": "maybe"}})")
              .find("not a value of \"a\"") != std::string::npos);
    CHECK(refused_setting(R"({"key": "a", "type": "bool", "enabledWhen": "a"})").find("enabledWhen") != std::string::npos);
    CHECK(refused_setting(R"({"key": "a", "type": "bool", "enabledWhen": {"setting": "b"}})").find("equals") != std::string::npos);
}

TEST_CASE("setting controls: players see on/off, a choice's or option's label, or the value") {
    SettingDef b;
    b.type = SettingType::Bool;
    CHECK(setting_shown_value(b, "1") == "on");
    CHECK(setting_shown_value(b, "0") == "off");
    const SettingDef sel = one(R"({"key": "c", "type": "select", "options": [{"value": "tr", "label": "Top right"}]})");
    CHECK(setting_shown_value(sel, "tr") == "Top right");
    const SettingDef ch = one(R"({"key": "c", "type": "choice", "choices": ["a", "b"]})");
    CHECK(setting_shown_value(ch, "1") == "b");
    const SettingDef k = one(R"({"key": "k", "type": "keybind"})");
    CHECK(setting_shown_value(k, "") == "no key");
}

TEST_CASE("setting controls: the store checks every new control like the old ones, and fixes bad ini values") {
    MemIni ini;
    ini.values["m.speed"] = "7";         // above max: clamped
    ini.values["m.corner"] = "Top right";  // a label: stored as the value
    ini.values["m.tint"] = "#ABC";
    ini.values["m.name"] = "far too long";  // longer than maxLength: the default
    SettingsStore store(&ini);
    auto r = parse_manifest(R"({"id": "m", "version": "1", "settings": [
        {"key": "speed", "type": "slider", "min": 0, "max": 5, "step": 0.5, "default": 1},
        {"key": "corner", "type": "select", "options": [{"value": "tr", "label": "Top right"}, "bl"], "default": "bl"},
        {"key": "tint", "type": "color", "default": "#ffffff"},
        {"key": "name", "type": "text", "maxLength": 4, "default": "Bob"},
        {"key": "toggle", "type": "keybind", "default": "F7"}]})");
    REQUIRE_MESSAGE(r.manifest, r.error);
    for (const auto& d : r.manifest->settings) REQUIRE(store.declare("m", d));
    CHECK(store.get("m", "speed") == "5");
    CHECK(ini.values["m.speed"] == "5");  // written back
    CHECK(store.get("m", "corner") == "tr");
    CHECK(store.get("m", "tint") == "#aabbcc");
    CHECK(store.get("m", "name") == "Bob");
    CHECK(store.get("m", "toggle") == "F7");
    std::string why;
    CHECK(store.set("m", "speed", "2.3", &why) == SettingsStore::SetResult::Ok);
    CHECK(store.get("m", "speed") == "2.5");
    CHECK(store.get_float("m", "speed", 0) == 2.5);
    CHECK(store.set("m", "corner", "BL", &why) == SettingsStore::SetResult::Ok);
    CHECK(store.get("m", "corner") == "bl");
    CHECK(store.set("m", "corner", "middle", &why) == SettingsStore::SetResult::Invalid);
    CHECK(why.find("choose one of") == 0);
    CHECK(store.set("m", "tint", "#00FF80", &why) == SettingsStore::SetResult::Ok);
    CHECK(ini.values["m.tint"] == "#00ff80");
    CHECK(store.set("m", "name", "Alice", &why) == SettingsStore::SetResult::Invalid);
    CHECK(why == "at most 4 characters");
    CHECK(store.set("m", "toggle", "g", &why) == SettingsStore::SetResult::Ok);
    CHECK(store.get("m", "toggle") == "G");
}

namespace {
std::vector<SettingDef> defs_of(const std::string& settings_json) {
    auto r = parse_manifest(R"({"id": "m", "version": "1", "settings": [)" + settings_json + "]}");
    REQUIRE_MESSAGE(r.manifest, r.error);
    return r.manifest->settings;
}
}  // namespace

TEST_CASE("setting controls: a live re-declare (mod.json edited in game) never overwrites the saved value") {
    MemIni ini;
    ini.values["m.radius"] = "50";  // the player's saved values
    ini.values["m.mode"] = "fast";
    SettingsStore store(&ini);
    const char* start = R"({"key": "radius", "type": "int", "min": 0, "max": 100, "default": 10},
        {"key": "mode", "type": "select", "options": ["slow", "fast"], "default": "slow"})";
    for (const auto& d : defs_of(start)) REQUIRE(store.declare("m", d));
    CHECK(store.take_changes().empty());  // declaring is not a change

    // the editor saves half-way: the range narrowed, the option renamed
    for (const auto& d : defs_of(R"({"key": "radius", "type": "int", "min": 0, "max": 1, "default": 0},
        {"key": "mode", "type": "select", "options": ["slow", "fas"], "default": "slow"})"))
        REQUIRE(store.declare("m", d, nullptr, SettingsStore::Declare::Live));
    CHECK(store.get("m", "radius") == "1");    // clamped in memory
    CHECK(store.get("m", "mode") == "slow");   // not an option any more: the default, in memory
    CHECK(ini.values["m.radius"] == "50");     // the saved values stay
    CHECK(ini.values["m.mode"] == "fast");
    auto changes = store.take_changes();       // the mod hears that the values it reads moved
    CHECK(changes == std::vector<std::pair<std::string, std::string>>{{"m", "radius"}, {"m", "mode"}});

    // the edit is finished: the saved values come back
    for (const auto& d : defs_of(start)) REQUIRE(store.declare("m", d, nullptr, SettingsStore::Declare::Live));
    CHECK(store.get("m", "radius") == "50");
    CHECK(store.get("m", "mode") == "fast");
    CHECK(ini.values["m.radius"] == "50");
    changes = store.take_changes();
    CHECK(changes.size() == 2);
    for (const auto& d : defs_of(start)) REQUIRE(store.declare("m", d, nullptr, SettingsStore::Declare::Live));
    CHECK(store.take_changes().empty());  // nothing moved

    // a setting new in mod.json reads its saved value, if any, and is no change
    ini.values["m.extra"] = "7";
    for (const auto& d : defs_of(R"({"key": "extra", "type": "int", "default": 1})"))
        REQUIRE(store.declare("m", d, nullptr, SettingsStore::Declare::Live));
    CHECK(store.get("m", "extra") == "7");
    CHECK(store.take_changes().empty());
}

TEST_CASE("setting controls: text that is not valid UTF-8 is refused (it would trip the in-game text box)") {
    const SettingDef d = one(R"({"key": "name", "type": "text", "maxLength": 4})");
    for (const char* bad : {"caf\xE9", "\x80\x80\x80\x80\x80\x80\x80\x80\x80", "\xC0\xAF", "\xE2\x82", "\xED\xA0\x80", "\xF5\x80\x80\x80"})
        CHECK_MESSAGE(value(d, bad).rfind("refused: not valid UTF-8", 0) == 0, bad);
    CHECK(value(d, "caf\xC3\xA9") == "caf\xC3\xA9");
    MemIni ini;
    ini.values["m.name"] = "\xE6\xF8\xE5";  // Latin-1 bytes in the ini: the default instead
    SettingsStore store(&ini);
    REQUIRE(store.declare("m", d));
    CHECK(store.get("m", "name").empty());
    std::string why;
    CHECK(store.set("m", "name", "\xFF\xFE", &why) == SettingsStore::SetResult::Invalid);  // e.g. from a C++ mod
    CHECK(refused_setting(R"({"key": "a", "type": "text", "default": ")" + std::string("\xE9") + R"("})").find("UTF-8") !=
          std::string::npos);
    CHECK(refused(R"({"key": "a", "type": "select", "options": [")" + std::string("\xE9") + R"("]})").find("UTF-8") !=
          std::string::npos);
    // labels, help and group headings only show: a bad byte becomes U+FFFD there
    const SettingDef l = one(R"({"key": "b", "type": "bool", "label": "Gr)" + std::string("\xF8") + R"(n", "group": "x)" +
                             std::string("\xFF") + R"("})");
    CHECK(l.label == "Gr\xEF\xBF\xBDn");
    CHECK(l.group == "x\xEF\xBF\xBD");
}

namespace {
std::vector<SettingDef> all(const std::string& settings_json) {
    auto r = parse_manifest(R"({"id": "m", "version": "1", "settings": [)" + settings_json + "]}");
    REQUIRE_MESSAGE(r.manifest, r.error);
    return r.manifest->settings;
}
// What declaring `d` says: "" when it is declared without a word, "refused: <why>" or "warning: <why>".
std::string declared(const SettingDef& d) {
    MemIni ini;
    SettingsStore store(&ini);
    std::string why;
    if (!store.declare("m", d, &why)) return "refused: " + why;
    return why.empty() ? "" : "warning: " + why;
}
}  // namespace

TEST_CASE("setting controls: null is the same as leaving a field out") {
    const SettingDef d = one(R"({"key": "a", "type": "int", "step": null, "group": null, "enabledWhen": null,
        "options": null, "maxLength": null, "control": null, "default": null, "label": null, "min": null})");
    CHECK(d.step == 0.0);
    CHECK(d.group.empty());
    CHECK(d.when_key.empty());
    CHECK(d.default_value == "0");
    CHECK(d.label == "a");
    CHECK(declared(d).empty());
    auto r = parse_manifest(R"({"id": "m", "version": "1", "depends": {"lua": {"version": null, "optional": null}}})");
    REQUIRE_MESSAGE(r.manifest, r.error);
    CHECK(r.manifest->depends[0].version.any());
}

TEST_CASE("setting controls: a bad display field on an old kind refuses only that setting; the mod still loads") {
    const auto s = all(R"({"key": "ok", "type": "bool"},
        {"key": "half_step", "type": "int", "min": 0, "max": 10, "step": 0.5, "default": 2},
        {"key": "off_step", "type": "int", "min": 0, "max": 100, "step": 5, "default": 7},
        {"key": "group_number", "type": "text", "group": 1},
        {"key": "bool_step", "type": "bool", "step": 1},
        {"key": "choice_options", "type": "choice", "choices": ["x", "y"], "options": ["x", "y"]},
        {"key": "long_text", "type": "text", "maxLength": 300},
        {"key": "when_missing", "type": "bool", "enabledWhen": {"setting": "zz", "equals": true}},
        {"key": "when_bad", "type": "float", "enabledWhen": "ok"},
        {"key": "knob", "type": "int", "min": 0, "max": 9, "control": "knob"})");
    REQUIRE(s.size() == 10);
    CHECK(declared(s[0]).empty());
    CHECK(declared(s[1]).find("refused: setting \"half_step\": \"step\" of a whole-number setting must be whole") == 0);
    CHECK(declared(s[2]).find("steps of 5") != std::string::npos);
    CHECK(declared(s[3]).find("\"group\"") != std::string::npos);
    CHECK(declared(s[4]).find("\"step\" is only for numbers") != std::string::npos);
    CHECK(declared(s[5]).find("\"options\" are for select settings") != std::string::npos);
    CHECK(declared(s[6]).find("maxLength") != std::string::npos);
    CHECK(declared(s[7]).find("\"zz\", which is not one of this mod's settings") != std::string::npos);
    CHECK(declared(s[8]).find("enabledWhen") != std::string::npos);
    CHECK(declared(s[9]).find("\"control\"") != std::string::npos);
    for (size_t i = 1; i < s.size(); ++i) CHECK_MESSAGE(declared(s[i]).rfind("refused: ", 0) == 0, s[i].key);
}

TEST_CASE("setting controls: the new kinds stay strict: a bad field refuses the mod, with the reason") {
    CHECK(refused(R"({"key": "a", "type": "slider", "min": 0, "max": 1, "group": 1})").find("group") != std::string::npos);
    CHECK(refused(R"({"key": "a", "type": "select", "options": ["x"], "step": 1})").find("step") != std::string::npos);
    CHECK(refused(R"({"key": "a", "type": "keybind", "maxLength": 3})").find("maxLength") != std::string::npos);
    CHECK(refused(R"({"key": "a", "type": "color", "enabledWhen": {"setting": "zz", "equals": 1}})").find("zz") !=
          std::string::npos);
}

TEST_CASE("setting controls: \"control\": \"slider\" makes an int or float a slider with exactly its type") {
    SettingDef d = one(R"({"key": "a", "type": "int", "min": 0, "max": 100, "control": "slider"})");
    CHECK(d.type == SettingType::Int);
    CHECK(d.slider);
    d = one(R"({"key": "b", "type": "float", "min": 0, "max": 10, "step": 1, "control": "slider"})");
    CHECK(d.type == SettingType::Float);  // the type says so, whatever the step
    CHECK(d.slider);
    d = one(R"({"key": "c", "type": "slider", "min": 0, "max": 10, "step": 1})");
    CHECK(d.type == SettingType::Int);
    d = one(R"({"key": "d", "type": "slider", "min": 0.5, "max": 10, "step": 1})");
    CHECK(d.type == SettingType::Float);  // a whole step but not whole ends: decimals
    const SettingDef no_max = one(R"({"key": "e", "type": "int", "min": 0, "control": "slider"})");
    CHECK(declared(no_max).find("a slider needs \"min\" and \"max\"") != std::string::npos);
}

TEST_CASE("setting controls: enabledWhen's equals must be a value the setting really has (not one it is turned into)") {
    // on an old kind the condition only refuses that setting; on a new kind the mod
    auto s = all(R"({"key": "c", "type": "choice", "choices": ["a", "b", "c"]},
        {"key": "n", "type": "int", "min": 0, "max": 10, "step": 5},
        {"key": "f", "type": "float", "min": 0, "max": 1, "step": 0.1, "default": 0.3},
        {"key": "on_choice", "type": "bool", "enabledWhen": {"setting": "c", "equals": true}},
        {"key": "on_step", "type": "bool", "enabledWhen": {"setting": "n", "equals": 3}},
        {"key": "on_float", "type": "bool", "enabledWhen": {"setting": "f", "equals": "0.30"}},
        {"key": "off_grid", "type": "bool", "enabledWhen": {"setting": "f", "equals": 0.35}},
        {"key": "by_index", "type": "bool", "enabledWhen": {"setting": "c", "equals": 1}})");
    CHECK(declared(s[3]).find("true is not a value of \"c\"") != std::string::npos);
    CHECK(declared(s[4]).find("3 is not a value of \"n\"") != std::string::npos);
    CHECK(declared(s[5]).empty());
    CHECK(s[5].when_value == "0.3");  // the same number, written another way
    CHECK(declared(s[6]).rfind("refused: ", 0) == 0);
    CHECK(declared(s[7]).empty());  // a choice by its index
    CHECK(refused(R"({"key": "c", "type": "choice", "choices": ["a", "b"]},
        {"key": "x", "type": "select", "options": ["p"], "enabledWhen": {"setting": "c", "equals": true}})")
              .find("true is not a value of \"c\"") != std::string::npos);
}

TEST_CASE("setting controls: enabledWhen on a setting that is refused is dropped with a warning, never greyed out for good") {
    const auto s = all(R"({"key": "k", "type": "key", "default": "Mouse4"},
        {"key": "a", "type": "bool", "enabledWhen": {"setting": "k", "equals": "G"}},
        {"key": "t", "type": "text", "group": 3},
        {"key": "b", "type": "bool", "enabledWhen": {"setting": "t", "equals": "x"}})");
    CHECK(declared(s[0]) == "refused: setting \"k\": the default is not a key");
    CHECK(s[1].when_key.empty());
    CHECK(declared(s[1]).find("warning: setting \"a\": \"enabledWhen\" names \"k\", which is refused") == 0);
    CHECK(s[3].when_key.empty());
    CHECK(declared(s[3]).find("names \"t\", which is refused") != std::string::npos);
}
