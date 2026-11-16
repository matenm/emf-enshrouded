// EMF's own settings (in-game look phase 2): the Settings tab's defs under the reserved id "emf", their pages, live or
// next start, defaults equal to what the overlay uses with an empty emf_core.ini, the font list, the reserved id.
#include <doctest.h>

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "core/emf_settings.h"
#include "core/manifest.h"
#include "core/overlay/look_settings.h"
#include "core/overlay/overlay.h"
#include "test_scratch.h"

using namespace emf::core;

namespace {
const SettingDef* find_def(const std::vector<SettingDef>& defs, const std::string& key) {
    for (const auto& d : defs)
        if (d.key == key) return &d;
    return nullptr;
}
}  // namespace

TEST_CASE("emf settings: every key of the spec, on its page, live or next start") {
    const auto defs = emf_setting_defs({});
    std::vector<std::string> keys;
    for (const auto& d : defs) keys.push_back(d.key);
    CHECK(keys == std::vector<std::string>{
                      "hud.style", "hud.font", "hud.size", "hud.colour", "hud.shadow", "hud.notes_place",
                      "hud.notes_seconds", "overlay.key", "overlay.mouse_speed", "overlay.chat", "overlay.font",
                      "overlay.tabs_font", "overlay.text_size", "overlay.height", "overlay.opacity",
                      "overlay.background", "overlay.accent", "overlay.colour_command", "overlay.colour_reply",
                      "overlay.colour_error", "overlay.colour_success", "overlay.suggest", "overlay.suggest_rows",
                      "hud_editor.enabled", "hud_editor.show_outlines", "hud_editor.allow_with_shroudhud",
                      "hud_editor.key_page", "hud_editor.key_reset", "hud_editor.active_layout",
                      "console.window", "backup.on_start", "backup.keep", "backup.min_minutes"});
    const std::vector<std::string> next_start = {"overlay.key",    "overlay.chat", "console.window",
                                                 "backup.on_start", "backup.keep", "backup.min_minutes"};
    for (const auto& d : defs) {
        CAPTURE(d.key);
        const bool next = std::find(next_start.begin(), next_start.end(), d.key) != next_start.end();
        CHECK(d.live == !next);
        const std::string page = d.key == "hud_editor.active_layout"       ? ""
                                 : d.key.rfind("hud_editor.", 0) == 0 ? kPageHud
                                 : d.key.rfind("hud.", 0) == 0       ? kPageText
                                 : d.key.rfind("overlay.", 0) == 0 ? kPageConsole
                                                                   : kPageStart;
        CHECK(d.group == page);
        CHECK_FALSE(d.label.empty());
        SettingDef copy = d;  // each one is a valid definition with a valid default
        CHECK(normalise_setting(copy).empty());
    }
    CHECK(find_def(defs, "overlay.enabled") == nullptr);  // lockout: stays in the ini and EMF.exe's Server tab
    CHECK(find_def(defs, "chat.admins") == nullptr);
    CHECK(find_def(defs, "overlay.key")->type == SettingType::ScanCode);
}

TEST_CASE("emf settings: the defaults are the ones the overlay uses when emf_core.ini is empty") {
    std::map<std::string, std::string> dflt;
    for (const auto& d : emf_setting_defs({})) dflt[d.key] = d.default_value;
    std::vector<std::string> problems;
    const LookSettings from_defaults = read_look_settings(
        [&](const char* section, const char* key) {
            auto it = dflt.find(std::string(section) + "." + key);
            return it == dflt.end() ? std::string() : it->second;
        },
        &problems);
    CHECK(problems.empty());
    CHECK(from_defaults == LookSettings{});
    CHECK(dflt["overlay.key"] == "0x29");
    CHECK(dflt["overlay.mouse_speed"] == "1");
    CHECK(dflt["overlay.suggest_rows"] == "8");
    CHECK(dflt["backup.keep"] == "10");
    CHECK(dflt["backup.min_minutes"] == "60");
}

TEST_CASE("emf settings: dropped-in fonts become choices; the three EMF ships are not listed twice") {
    const std::wstring dir = test_scratch::temp_path(L"emf_fonts_test");
    CreateDirectoryW(dir.c_str(), nullptr);
    test_scratch::remove_at_exit(dir);
    for (const wchar_t* f : {L"Alegreya-Regular.ttf", L"MyFont.ttf", L"Other.OTF", L"notes.txt"})
        std::ofstream(dir + L"\\" + f) << "x";
    CHECK(emf_font_files(dir) == std::vector<std::string>{"MyFont.ttf", "Other.OTF"});
    const auto defs = emf_setting_defs(emf_font_files(dir));
    const SettingDef* hud_font = find_def(defs, "hud.font");
    REQUIRE(hud_font);
    REQUIRE(hud_font->options.size() == 4);
    CHECK(hud_font->options[2].value == "file:MyFont.ttf");
    CHECK(find_def(defs, "overlay.font")->options.back().value == "file:Other.OTF");
    CHECK(emf_font_files(dir + L"\\missing").empty());
}

TEST_CASE("emf settings: the console key is a scan code from 0x01 to 0x7f, kept as 0x29") {
    const SettingDef* key = find_def(emf_setting_defs({}), "overlay.key");
    REQUIRE(key);
    CHECK(setting_value(*key, "41", false, nullptr).value_or("") == "0x29");
    CHECK(setting_value(*key, "0x3B", false, nullptr).value_or("") == "0x3b");
    std::string why;
    CHECK_FALSE(setting_value(*key, "0x80", false, &why));
    CHECK(why.find("scan code") != std::string::npos);
    CHECK_FALSE(setting_value(*key, "F1", false, nullptr));
    CHECK_FALSE(setting_value(*key, "", false, nullptr));
    CHECK(setting_shown_value(*key, "0x29") == "the key under Esc (0x29)");
}

TEST_CASE("emf settings: any console key scan code is shown by the keyboard layout's name for it, else by its number") {
    const auto layout = [](unsigned sc) -> std::string {
        if (sc == 0x2b) return "\\";
        if (sc == 0x3b) return "F1";
        if (sc == 0x3a) return "Caps Lock";
        return "";  // the layout has no name for it
    };
    CHECK(scan_code_shown("0x29", layout) == "the key under Esc (0x29)");
    CHECK(scan_code_shown("0x2b", layout) == "the \\ key (0x2b)");
    CHECK(scan_code_shown("0x3b", layout) == "the F1 key (0x3b)");
    CHECK(scan_code_shown("0x3a", layout) == "the Caps Lock key (0x3a)");
    CHECK(scan_code_shown("0x59", layout) == "scan code 0x59");
    CHECK(scan_code_shown("junk", layout) == "scan code junk");  // not a scan code: as written
    // this PC's own layout (GetKeyNameTextW): every layout has a name for 0x2b (\ on US keyboards, # on UK and German)
    const SettingDef* key = find_def(emf_setting_defs({}), "overlay.key");
    REQUIRE(key);
    const std::string shown = setting_shown_value(*key, "0x2b");
    CHECK(shown.rfind("the ", 0) == 0);
    CHECK(shown.size() > std::string(" key (0x2b)").size());
    CHECK(shown.substr(shown.size() - 11) == " key (0x2b)");
}

TEST_CASE("emf settings: a mod cannot take the id emf") {
    const auto r = parse_manifest(R"({"id": "emf", "version": "1.0.0"})");
    CHECK_FALSE(r.manifest);
    CHECK(r.error.find("EMF's own") != std::string::npos);
    CHECK(parse_manifest(R"({"id": "emf_tools", "version": "1.0.0"})").manifest);
}

namespace {
struct MapIni : IniBackend {
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
}  // namespace

TEST_CASE("look_from_store: the store's emf values make the look; unset keys are the defaults") {
    MapIni ini;
    ini.values["emf.hud.size"] = "140";
    SettingsStore s(&ini);
    for (SettingDef d : emf_setting_defs({}))
        REQUIRE(s.declare(kEmfSettingsId, std::move(d), nullptr, SettingsStore::Declare::Keep));
    REQUIRE(s.set(kEmfSettingsId, "overlay.accent", "#112233") == SettingsStore::SetResult::Ok);
    REQUIRE(s.set(kEmfSettingsId, "overlay.colour_error", "#010203") == SettingsStore::SetResult::Ok);
    std::vector<std::string> problems;
    const LookSettings look = look_from_store(s, &problems);
    CHECK(problems.empty());
    CHECK(look.hud_size == 140);
    CHECK(look.accent == 0x112233FFu);
    CHECK(look.colour_error == 0x010203FFu);
    CHECK(look.style == HudStyle::Game);
    CHECK(look.height == 50);
    SettingsStore empty(&ini);  // nothing declared (a core without the defs): the defaults
    CHECK(look_from_store(empty, nullptr) == LookSettings{});
}

TEST_CASE("look_from_store: only style=boxed in the file gives the boxed look (its own defaults), as at start") {
    MapIni ini;
    ini.values["emf.hud.style"] = "boxed";
    SettingsStore s(&ini);
    for (SettingDef d : emf_setting_defs({}))
        REQUIRE(s.declare(kEmfSettingsId, std::move(d), nullptr, SettingsStore::Declare::Keep));
    const LookSettings boxed = read_look_settings(
        [](const char* section, const char* key) {
            return std::string(section) == "hud" && std::string(key) == "style" ? std::string("boxed") : std::string();
        },
        nullptr);
    CHECK(look_from_store(s, nullptr) == boxed);
    CHECK(boxed.hud_font.kind == FontChoice::Kind::Console);  // boxed's own defaults
    REQUIRE(s.set(kEmfSettingsId, "hud.font", "serif") == SettingsStore::SetResult::Ok);  // chosen in the tab
    CHECK(look_from_store(s, nullptr).hud_font.kind == FontChoice::Kind::Serif);
}

TEST_CASE("emf settings: the console key is never Esc or Enter, also when typed into set or the ini") {
    const SettingDef* key = find_def(emf_setting_defs({}), "overlay.key");
    REQUIRE(key);
    std::string why;
    CHECK_FALSE(setting_value(*key, "0x01", false, &why));
    CHECK(why.find("Esc") != std::string::npos);
    CHECK_FALSE(setting_value(*key, "0x1c", false, nullptr));
    CHECK_FALSE(setting_value(*key, "0x1C", true, nullptr));  // from the ini: the default is used instead
}

TEST_CASE("overlay_config_with_store: the console starts with the values the Settings tab shows (one parser)") {
    MapIni ini;
    ini.values["emf.hud.size"] = "125";             // snapped to whole steps of 10, as the tab shows it
    ini.values["emf.overlay.mouse_speed"] = "20";   // kept inside 0.1-10
    ini.values["emf.overlay.chat"] = "false";
    ini.values["emf.overlay.key"] = "0x3B";
    ini.values["emf.overlay.suggest_rows"] = "5";
    SettingsStore s(&ini);
    for (SettingDef d : emf_setting_defs({}))
        REQUIRE(s.declare(kEmfSettingsId, std::move(d), nullptr, SettingsStore::Declare::Keep));
    OverlayConfig from_ini;  // what read_overlay_config would give (enabled stays its own)
    from_ini.enabled = false;
    const OverlayConfig c = overlay_config_with_store(from_ini, s);
    CHECK_FALSE(c.enabled);
    CHECK(c.look.hud_size == 130);
    CHECK(c.look == look_from_store(s, nullptr));
    CHECK(c.mouse_speed == doctest::Approx(10.0f));
    CHECK_FALSE(c.chat_commands);
    CHECK(c.toggle_scan == 0x3B);
    CHECK(c.suggest_rows == 5u);
    SettingsStore empty(&ini);  // no defs (a core without them): the ini's config as it was
    CHECK(overlay_config_with_store(from_ini, empty).look == from_ini.look);
}

TEST_CASE("emf settings: the HUD editor's keys default to F9 and Pause, and a store keeps them") {
    MapIni ini;
    SettingsStore s(&ini);
    for (SettingDef d : emf_setting_defs({}))
        REQUIRE(s.declare(kEmfSettingsId, std::move(d), nullptr, SettingsStore::Declare::Keep));
    CHECK(s.get(kEmfSettingsId, "hud_editor.key_page") == "F9");
    CHECK(s.get(kEmfSettingsId, "hud_editor.key_reset") == "Pause");
    CHECK(s.get(kEmfSettingsId, "hud_editor.enabled") == "1");
    CHECK(s.get(kEmfSettingsId, "hud_editor.allow_with_shroudhud") == "0");
    CHECK(s.get(kEmfSettingsId, "hud_editor.active_layout") == "Default");
    CHECK(s.set(kEmfSettingsId, "hud_editor.active_layout", std::string(41, 'a')) == SettingsStore::SetResult::Invalid);
}
