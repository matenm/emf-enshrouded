// Mod profiles over a real (fake) game folder: saving and applying through set_mod_enabled with every kind of mod
// (EMF mods' ini, EML folder and zip renames, dinput8-chain.ini lines in UTF-16 and ANSI, read-only ShroudForge and
// Greeneye mods), missing and extra mods, settings, a locked file, importing; and EMF.exe's bridge commands.
#include <windows.h>

#include <doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "app/bridge.h"
#include "app/game_folder.h"
#include "app/profiles.h"
#include "app/text.h"
#include "app_test_files.h"
#include "core/json.h"
#include "core/profiles.h"

using namespace emf::app;
using app_test::TempGame;
namespace json = emf::core::json;
namespace core = emf::core;

namespace {

const char* kTools =
    "shroudforge | ShroudForge | text:shroudforge | beside:shroudforge | same-loader:dbghelp.dll\n"
    "eml | EML (Enshrouded Mod Loader) | text:EML_LOG_FILE_PATH\n"
    "greeneye | Greeneye Loader | text:greeneye | beside:greeneye\n";

const char* kClock = R"({"id":"clock","name":"Clock","version":"1.1.0","nexus_id":1234,
  "settings":[{"key":"size","type":"int","default":10,"min":1,"max":20},
              {"key":"place","type":"choice","default":"bottom right","choices":["top right","bottom right"]}]})";

std::string utf16(const std::wstring& w) {
    std::string b = "\xFF\xFE";
    b.append(reinterpret_cast<const char*>(w.data()), w.size() * sizeof(wchar_t));
    return b;
}

void install_emf(const TempGame& g) {
    g.write(L"dinput8.dll", "MZ EMF-LOADER-7d3c9f fake");
    g.write(L"emf\\emf_core.dll", std::string("MZ emf_core 0.3.0\0 more", 23));
    g.write(L"emf\\known_tools.txt", kTools);
}

// A folder with EMF, EML (as dbghelp.dll), an EMF mod with settings, an EML folder mod and a zip mod, and two DLLs in
// a UTF-16 dinput8-chain.ini.
void every_kind(const TempGame& g) {
    install_emf(g);
    g.write(L"dbghelp.dll", "EML_LOG_FILE_PATH");
    g.write(L"emf\\mods\\clock\\mod.json", kClock);
    g.write(L"emf\\mods\\clock\\clock.dll", "dll");
    g.write(L"mods\\foo\\mod.json", R"({"id":"foo","name":"Foo","version":"1.0.0","nexus_id":170})");
    g.write(L"mods\\pack.zip", "PK fake zip");
    g.write(L"dinput8-chain.ini", utf16(L"[Chain]\r\ncreative.dll\r\nmöd.dll\r\n"));
    g.write(L"creative.dll", "x");
    g.write(L"möd.dll", "x");
}

const ModEntry* find(const FolderState& s, const std::string& source, const std::string& id) {
    for (const auto& m : s.mods)
        if (m.source == source && m.id == id) return &m;
    return nullptr;
}

bool is_on(const std::wstring& game, const std::string& source, const std::string& id) {
    const FolderState s = scan_game_folder(game);
    const ModEntry* m = find(s, source, id);
    const std::string what = source + ":" + id;
    REQUIRE_MESSAGE(m, what);
    return m->enabled;
}

std::vector<std::string> ids(const std::vector<core::ApplyItem>& items) {
    std::vector<std::string> out;
    for (const auto& i : items) out.push_back(i.source + ":" + i.id);
    return out;
}

core::ProfileSave save(const TempGame& g, const std::string& name, bool settings = false, bool replace = false) {
    return save_folder_profile(g.dir, scan_game_folder(g.dir), name, settings, replace, "2026-10-26 20:00", "0.3.0");
}

ProfileApply apply(const TempGame& g, const std::string& slug) {
    FolderSwitcher sw(g.dir);
    return apply_saved_profile(g.dir, scan_game_folder(g.dir), slug, sw);
}

}  // namespace

// ---- the folder ----

TEST_CASE("app profiles: the folder as a profile sees it: every mod, read-only ones with the reason, Nexus ids") {
    TempGame g(L"prof_view");
    every_kind(g);
    g.write(L"greeneye\\gmod\\mod.json", R"({"name":"G Mod"})");
    const auto folder = profile_folder(scan_game_folder(g.dir));
    const auto at = [&](const std::string& source, const std::string& id) -> const core::FolderMod* {
        for (const auto& m : folder)
            if (m.source == source && m.id == id) return &m;
        return nullptr;
    };
    REQUIRE(at("emf", "clock"));
    CHECK(at("emf", "clock")->nexus_id == "1234");
    CHECK(at("emf", "clock")->can_switch);
    REQUIRE(at("emf", "clock")->settings.size() == 2);
    CHECK(at("emf", "clock")->settings[0].key == "size");
    CHECK(at("emf", "clock")->settings[0].value == "10");
    REQUIRE(at("eml", "foo"));
    CHECK(at("eml", "foo")->nexus_id == "170");
    REQUIRE(at("eml", "pack.zip"));
    REQUIRE(at("chain", "creative.dll"));
    REQUIRE(at("chain", "m\xC3\xB6" "d.dll"));
    REQUIRE(at("other", "greeneye:gmod"));
    CHECK_FALSE(at("other", "greeneye:gmod")->can_switch);
    CHECK_FALSE(at("other", "greeneye:gmod")->why_not.empty());
    CHECK_FALSE(at("other", "loader:dbghelp.dll"));  // a loader itself is not a mod a profile switches
    CHECK(folder.size() == 6);
}

TEST_CASE("app profiles: the core's scan is EMF.exe's without the install records") {
    TempGame g(L"prof_scan");
    every_kind(g);
    g.write(L"emf\\installed_mods\\nexus-170.json",
            R"({"key":"nexus-170","name":"Foo","nexus_id":"170","roots":["mods\\foo"],"files":[]})");
    const FolderState full = scan_game_folder(g.dir), bare = scan_mod_folder(g.dir);
    REQUIRE(full.mods.size() == bare.mods.size());
    for (size_t i = 0; i < full.mods.size(); ++i) {
        CHECK(full.mods[i].key == bare.mods[i].key);
        CHECK(full.mods[i].enabled == bare.mods[i].enabled);
        CHECK(bare.mods[i].install_key.empty());
    }
    CHECK(bare.installed.empty());
    CHECK(full.shroudforge_active == bare.shroudforge_active);
}

// ---- apply over every kind ----

TEST_CASE("app profiles: save, switch everything off, apply: every kind is back, the chain file still UTF-16") {
    TempGame g(L"prof_all");
    every_kind(g);
    const std::string chain_before = g.read(L"dinput8-chain.ini");
    const core::ProfileSave s = save(g, "All on");
    REQUIRE_MESSAGE(s.ok, s.error);
    CHECK(s.slug == "all-on");
    CHECK(g.has(L"emf\\profiles\\all-on.json"));
    CHECK(core::ProfileStore(profiles_dir(g.dir)).current() == "all-on");
    for (const auto& [source, id] : std::vector<std::pair<std::string, std::string>>{
             {"emf", "clock"}, {"eml", "foo"}, {"eml", "pack.zip"}, {"chain", "creative.dll"}, {"chain", "m\xC3\xB6" "d.dll"}})
        REQUIRE_MESSAGE(set_mod_enabled(g.dir, source, id, false).ok, id);
    CHECK(g.has(L"mods\\.foo"));
    CHECK(g.has(L"mods\\.pack.zip"));
    CHECK(g.read(L"dinput8-chain.ini") == utf16(L"[Chain]\r\n; creative.dll\r\n; möd.dll\r\n"));
    CHECK(profiles_overview(g.dir, scan_game_folder(g.dir)).changed);

    const ProfileApply a = apply(g, "all-on");
    REQUIRE_MESSAGE(a.ok, a.error);
    CHECK(a.report.ok());
    CHECK(a.report.switched.size() == 5);
    CHECK(a.report.failed.empty());
    CHECK(g.read(L"emf\\config\\clock.ini").find("enabled=1") != std::string::npos);  // EMF's ini
    CHECK(g.has(L"mods\\foo"));
    CHECK_FALSE(g.has(L"mods\\.foo"));  // EML: the folder renamed back
    CHECK(g.has(L"mods\\pack.zip"));
    CHECK_FALSE(g.has(L"mods\\.pack.zip"));
    CHECK(g.read(L"dinput8-chain.ini") == chain_before);  // UTF-16 with its BOM, byte for byte
    const ProfilesOverview o = profiles_overview(g.dir, scan_game_folder(g.dir));
    CHECK(o.current == "all-on");
    CHECK(o.current_name == "All on");
    CHECK_FALSE(o.changed);

    // and the other way: a profile with them off
    REQUIRE(set_mod_enabled(g.dir, "eml", "foo", false).ok);
    REQUIRE(set_mod_enabled(g.dir, "chain", "creative.dll", false).ok);
    REQUIRE(save(g, "Lean").ok);
    REQUIRE(apply(g, "all-on").ok);
    CHECK(is_on(g.dir, "eml", "foo"));
    const ProfileApply lean = apply(g, "lean");
    REQUIRE(lean.ok);
    CHECK(ids(lean.report.switched) == std::vector<std::string>{"eml:foo", "chain:creative.dll"});
    CHECK_FALSE(is_on(g.dir, "eml", "foo"));
    CHECK_FALSE(is_on(g.dir, "chain", "creative.dll"));
    CHECK(is_on(g.dir, "chain", "m\xC3\xB6" "d.dll"));
}

TEST_CASE("app profiles: an ANSI dinput8-chain.ini stays ANSI when a profile switches its line") {
    TempGame g(L"prof_ansi");
    install_emf(g);
    // a DLL name with a letter of this PC's ANSI code page (é where it has one)
    std::wstring dll = L"tést.dll";
    char ansi[64] = {};
    BOOL lossy = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, dll.c_str(), -1, ansi, sizeof ansi, nullptr, &lossy);
    if (n <= 0 || lossy) {
        dll = L"ansi.dll";
        n = WideCharToMultiByte(CP_ACP, 0, dll.c_str(), -1, ansi, sizeof ansi, nullptr, nullptr);
    }
    const std::string name_ansi(ansi);
    const std::string original = "[Chain]\r\n" + name_ansi + "\r\n";
    g.write(L"dinput8-chain.ini", original);
    g.write(dll, "x");
    const std::string id = utf8(dll);
    REQUIRE(find(scan_game_folder(g.dir), "chain", id));
    REQUIRE(save(g, "On").ok);
    REQUIRE(set_mod_enabled(g.dir, "chain", id, false).ok);
    CHECK(g.read(L"dinput8-chain.ini") == "[Chain]\r\n; " + name_ansi + "\r\n");
    const ProfileApply a = apply(g, "on");
    REQUIRE(a.ok);
    CHECK(ids(a.report.switched) == std::vector<std::string>{"chain:" + id});
    CHECK(g.read(L"dinput8-chain.ini") == original);  // the ANSI bytes as they were
}

TEST_CASE("app profiles: read-only mods are listed and reported, never switched (ShroudForge, Greeneye)") {
    TempGame g(L"prof_ro");
    install_emf(g);
    g.mkdir(L"shroudforge");
    g.write(L"dbghelp.dll", "... ShroudForge ... EML_LOG_FILE_PATH");
    g.write(L"mods\\a\\mod.json", R"({"id":"a","version":"1.0.0"})");
    g.write(L"mods\\a\\extended.mod.json", R"({"schemaVersion":1,"enabled":true})");
    g.write(L"greeneye\\gmod\\mod.json", R"({"name":"G Mod"})");
    REQUIRE(scan_game_folder(g.dir).shroudforge_active);
    REQUIRE(save(g, "SF").ok);
    const core::Profile p = *core::ProfileStore(profiles_dir(g.dir)).read("sf").profile;
    bool a_read_only = false;
    for (const auto& m : p.mods)
        if (m.source == "eml" && m.id == "a") a_read_only = m.read_only;
    CHECK(a_read_only);
    // ShroudForge switches the package off; the profile says on: reported, not touched
    g.write(L"mods\\a\\extended.mod.json", R"({"schemaVersion":1,"enabled":false})");
    const ProfileApply r = apply(g, "sf");
    REQUIRE(r.ok);
    CHECK(r.report.switched.empty());
    CHECK(r.report.failed.empty());
    CHECK(r.report.read_only.empty());
    REQUIRE(ids(r.report.recorded_read_only) == std::vector<std::string>{"eml:a"});  // recorded read-only: never switched
    CHECK(r.report.recorded_read_only[0].why.find("read-only") != std::string::npos);
    // one the profile recorded as switchable is reported with ShroudForge's own way
    g.write(L"emf\\profiles\\other.json", R"({"name":"Other","mods":[{"source":"eml","id":"a","on":true}]})");
    const ProfileApply o = apply(g, "other");
    REQUIRE(ids(o.report.read_only) == std::vector<std::string>{"eml:a"});
    CHECK(o.report.read_only[0].why.find("ShroudForge") != std::string::npos);
    CHECK(g.read(L"mods\\a\\extended.mod.json").find("false") != std::string::npos);
    CHECK(g.has(L"mods\\a"));
    CHECK_FALSE(profiles_overview(g.dir, scan_game_folder(g.dir)).changed);  // read-only ones do not count
}

TEST_CASE("app profiles: missing mods are reported with their Nexus ids; mods not in the profile stay as they are") {
    TempGame g(L"prof_missing");
    every_kind(g);
    REQUIRE(save(g, "Full").ok);
    // foo is uninstalled, a new mod comes
    std::filesystem::remove_all(std::filesystem::path(g.dir + L"\\mods\\foo"));
    g.write(L"mods\\newmod\\mod.json", R"({"id":"newmod","name":"New Mod","version":"1.0.0"})");
    REQUIRE(set_mod_enabled(g.dir, "eml", "newmod", false).ok);
    REQUIRE(set_mod_enabled(g.dir, "emf", "clock", false).ok);
    CHECK(profiles_overview(g.dir, scan_game_folder(g.dir)).changed);
    const ProfileApply a = apply(g, "full");
    REQUIRE(a.ok);
    CHECK(a.report.ok());  // missing is never fatal
    CHECK(ids(a.report.switched) == std::vector<std::string>{"emf:clock"});
    REQUIRE(ids(a.report.missing) == std::vector<std::string>{"eml:foo"});
    CHECK(a.report.missing[0].nexus_id == "170");
    CHECK(a.report.missing[0].name == "Foo");
    REQUIRE(ids(a.report.not_in_profile) == std::vector<std::string>{"eml:newmod"});
    CHECK_FALSE(is_on(g.dir, "eml", "newmod"));  // left as it was
    CHECK(g.has(L"mods\\.newmod"));
    CHECK_FALSE(profiles_overview(g.dir, scan_game_folder(g.dir)).changed);  // newmod is not in it: no say on it
}

TEST_CASE("app profiles: settings are saved only when asked, and applied back to emf\\config") {
    TempGame g(L"prof_settings");
    every_kind(g);
    REQUIRE(set_mod_setting(g.dir, "clock", "size", "15").ok);
    REQUIRE(save(g, "Plain").ok);
    REQUIRE(save(g, "With", true).ok);
    REQUIRE(core::ProfileStore(profiles_dir(g.dir)).read("with").profile->with_settings);
    CHECK_FALSE(core::ProfileStore(profiles_dir(g.dir)).read("plain").profile->with_settings);
    REQUIRE(set_mod_setting(g.dir, "clock", "size", "3").ok);
    REQUIRE(set_mod_setting(g.dir, "clock", "place", "0").ok);
    // a profile without settings leaves them
    ProfileApply a = apply(g, "plain");
    REQUIRE(a.ok);
    CHECK(a.report.settings_set == 0);
    CHECK(g.read(L"emf\\config\\clock.ini").find("size=3") != std::string::npos);
    CHECK_FALSE(profiles_overview(g.dir, scan_game_folder(g.dir)).changed);
    // the one with settings puts them back
    a = apply(g, "with");
    REQUIRE(a.ok);
    CHECK(a.report.settings_set == 2);
    CHECK(a.report.settings_failed.empty());
    const std::string ini = g.read(L"emf\\config\\clock.ini");
    CHECK(ini.find("size=15") != std::string::npos);
    CHECK(ini.find("place=1") != std::string::npos);
    CHECK_FALSE(profiles_overview(g.dir, scan_game_folder(g.dir)).changed);
    REQUIRE(set_mod_setting(g.dir, "clock", "size", "4").ok);
    CHECK(profiles_overview(g.dir, scan_game_folder(g.dir)).changed);
    // a saved value the mod no longer accepts (its range changed) is reported, the rest still applies
    g.write(L"emf\\mods\\clock\\mod.json", R"({"id":"clock","name":"Clock","version":"1.2.0",
      "settings":[{"key":"size","type":"int","default":2,"min":1,"max":5},
                  {"key":"place","type":"choice","default":"bottom right","choices":["top right","bottom right"]}]})");
    REQUIRE(set_mod_setting(g.dir, "clock", "place", "0").ok);
    a = apply(g, "with");
    REQUIRE(a.ok);
    REQUIRE(a.report.settings_failed.size() == 1);
    CHECK(a.report.settings_failed[0].id == "clock.size");
    CHECK(a.report.settings_set == 1);
    CHECK(g.read(L"emf\\config\\clock.ini").find("place=1") != std::string::npos);
}

TEST_CASE("app profiles: one switch that fails (a file another program holds) is reported; the others apply") {
    TempGame g(L"prof_fail");
    every_kind(g);
    g.write(L"mods\\bar\\mod.json", R"({"id":"bar","version":"1.0.0"})");
    REQUIRE(save(g, "Base").ok);
    for (const auto& [source, id] : std::vector<std::pair<std::string, std::string>>{
             {"emf", "clock"}, {"eml", "foo"}, {"eml", "bar"}, {"chain", "creative.dll"}})
        REQUIRE(set_mod_enabled(g.dir, source, id, false).ok);
    // dinput8-chain.ini held open without sharing (another program took it after EMF looked at the folder)
    const FolderState before = scan_game_folder(g.dir);
    HANDLE lock = CreateFileW((g.dir + L"\\dinput8-chain.ini").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(lock != INVALID_HANDLE_VALUE);
    FolderSwitcher sw(g.dir);
    const ProfileApply a = apply_saved_profile(g.dir, before, "base", sw);
    CloseHandle(lock);
    REQUIRE(a.ok);
    CHECK_FALSE(a.report.ok());
    CHECK(ids(a.report.switched) == std::vector<std::string>{"emf:clock", "eml:bar", "eml:foo"});
    REQUIRE(ids(a.report.failed) == std::vector<std::string>{"chain:creative.dll"});
    CHECK_FALSE(a.report.failed[0].why.empty());
    CHECK(is_on(g.dir, "emf", "clock"));  // nothing was rolled back
    CHECK(is_on(g.dir, "eml", "bar"));
    const auto lines = core::apply_report_lines(a.report);
    CHECK(lines[0].find("1 failed") != std::string::npos);
}

TEST_CASE("app profiles: a write that fails (a read-only dinput8-chain.ini) is reported; the others still apply") {
    TempGame g(L"prof_wfail");
    every_kind(g);
    REQUIRE(save(g, "Base").ok);
    REQUIRE(set_mod_enabled(g.dir, "eml", "foo", false).ok);
    REQUIRE(set_mod_enabled(g.dir, "chain", "creative.dll", false).ok);
    const std::wstring chain = g.dir + L"\\dinput8-chain.ini";
    const std::string bytes = g.read(L"dinput8-chain.ini");
    REQUIRE(SetFileAttributesW(chain.c_str(), FILE_ATTRIBUTE_READONLY));
    const ProfileApply a = apply(g, "base");
    SetFileAttributesW(chain.c_str(), FILE_ATTRIBUTE_NORMAL);
    REQUIRE(a.ok);
    CHECK(ids(a.report.switched) == std::vector<std::string>{"eml:foo"});
    REQUIRE(ids(a.report.failed) == std::vector<std::string>{"chain:creative.dll"});
    CHECK(a.report.failed[0].why.find("dinput8-chain.ini") != std::string::npos);
    CHECK(g.read(L"dinput8-chain.ini") == bytes);  // the file as it was
    CHECK_FALSE(g.has(L"dinput8-chain.ini.emf-new"));
    CHECK(a.report.still_different() == 1);
}

// ---- edge cases ----

TEST_CASE("app profiles: entries saved read-only under ShroudForge are not switched once mods\\ goes to EML") {
    TempGame g(L"prof_ro2eml");
    install_emf(g);
    g.mkdir(L"shroudforge");
    g.write(L"dbghelp.dll", "... ShroudForge ... EML_LOG_FILE_PATH");
    g.write(L"mods\\plain\\mod.json", R"({"id":"plain","version":"1.0.0"})");  // off in ShroudForge (no extended.mod.json)
    REQUIRE(scan_game_folder(g.dir).shroudforge_active);
    REQUIRE(save(g, "Under SF").ok);
    // the player hands mods\ to EML: EMF could switch the folder now
    g.write(L"shroudforge\\config\\modloader-config.json", R"({"modloader":"eml"})");
    const FolderState now = scan_game_folder(g.dir);
    REQUIRE_FALSE(now.shroudforge_active);
    CHECK_FALSE(profiles_overview(g.dir, now).changed);
    const ProfileApply a = apply(g, "under-sf");
    REQUIRE(a.ok);
    CHECK(a.report.switched.empty());
    CHECK(ids(a.report.recorded_read_only) == std::vector<std::string>{"eml:plain"});
    CHECK(g.has(L"mods\\plain"));  // not renamed off
    CHECK_FALSE(g.has(L"mods\\.plain"));
}

TEST_CASE("app profiles: a broken mod.json and an EML folder without mod.json are not recorded, and never switched") {
    TempGame g(L"prof_forced");
    every_kind(g);
    g.write(L"emf\\mods\\broken\\mod.json", "{not json");
    g.write(L"mods\\bare\\extended.mod.json", R"({"enabled":true})");  // no mod.json: EML does not take it as a mod
    REQUIRE(save(g, "Base").ok);
    const core::Profile p = *core::ProfileStore(profiles_dir(g.dir)).read("base").profile;
    for (const auto& m : p.mods) {
        CHECK_MESSAGE(m.id != "broken", m.id);
        CHECK_MESSAGE(m.id != "bare", m.id);
    }
    // switched off by hand (the on and off forms of the bare folder): the profile has no say, nothing is renamed
    REQUIRE(set_mod_enabled(g.dir, "eml", "bare", false).ok);
    CHECK_FALSE(profiles_overview(g.dir, scan_game_folder(g.dir)).changed);
    const ProfileApply a = apply(g, "base");
    REQUIRE(a.ok);
    CHECK(a.report.switched.empty());
    CHECK(a.report.missing.empty());
    CHECK(g.has(L"mods\\.bare"));
}

TEST_CASE("app profiles: two copies of an EML mod count once: no \"(changed)\" right after Save; a switch reports them") {
    TempGame g(L"prof_dup");
    every_kind(g);
    g.write(L"mods\\.foo\\mod.json", R"({"id":"foo","version":"0.9.0"})");  // mods\foo and mods\.foo
    REQUIRE(save(g, "Dup").ok);
    CHECK_FALSE(profiles_overview(g.dir, scan_game_folder(g.dir)).changed);
    const core::Profile p = *core::ProfileStore(profiles_dir(g.dir)).read("dup").profile;
    size_t foos = 0;
    for (const auto& m : p.mods)
        if (m.source == "eml" && m.id == "foo") {
            ++foos;
            CHECK(m.on);  // EML loads mods\foo
            CHECK_FALSE(m.read_only);
        }
    CHECK(foos == 1);
    // a profile that wants it off cannot rename it while both copies are there: reported, nothing deleted
    g.write(L"emf\\profiles\\off.json", R"({"name":"Off","mods":[{"source":"eml","id":"foo","on":false}]})");
    const ProfileApply a = apply(g, "off");
    REQUIRE(a.ok);
    CHECK(a.report.failed.empty());
    REQUIRE(ids(a.report.read_only) == std::vector<std::string>{"eml:foo"});
    CHECK(a.report.read_only[0].why.find("two copies") != std::string::npos);
    CHECK(g.has(L"mods\\foo"));
    CHECK(g.has(L"mods\\.foo"));
}

TEST_CASE("app profiles: an unreadable dinput8-chain.ini stops Save and Switch instead of looking empty") {
    TempGame g(L"prof_unread");
    every_kind(g);
    REQUIRE(save(g, "Base").ok);
    HANDLE lock = CreateFileW((g.dir + L"\\dinput8-chain.ini").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(lock != INVALID_HANDLE_VALUE);
    const FolderState s = scan_game_folder(g.dir);
    CHECK_FALSE(s.read_error.empty());
    const core::ProfileSave sv = save_folder_profile(g.dir, s, "Other", false, false, "x", "y");
    FolderSwitcher sw(g.dir);
    const ProfileApply a = apply_saved_profile(g.dir, s, "base", sw);
    CloseHandle(lock);
    CHECK_FALSE(sv.ok);
    CHECK(sv.error.find("dinput8-chain.ini") != std::string::npos);
    CHECK_FALSE(g.has(L"emf\\profiles\\other.json"));
    CHECK_FALSE(a.ok);
    CHECK(a.error.find("dinput8-chain.ini") != std::string::npos);
    bool warned = false;
    for (const auto& w : s.warnings) warned = warned || w.text.find("dinput8-chain.ini") != std::string::npos;
    CHECK(warned);
}

TEST_CASE("app profiles: applying 150 EML and 50 EMF switches does not read the folder again for each one") {
    TempGame g(L"prof_cost");
    install_emf(g);
    g.write(L"dbghelp.dll", "EML_LOG_FILE_PATH");
    for (int i = 0; i < 150; ++i) g.write(L"mods\\m" + std::to_wstring(i) + L"\\mod.json", "{\"id\":\"m" + std::to_string(i) + "\"}");
    for (int i = 0; i < 50; ++i) {
        const std::wstring d = L"emf\\mods\\e" + std::to_wstring(i);
        g.write(d + L"\\mod.json", "{\"id\":\"e" + std::to_string(i) + "\",\"name\":\"E\",\"version\":\"1.0.0\"}");
        g.write(d + L"\\e" + std::to_wstring(i) + L".dll", "dll");
    }
    REQUIRE(save(g, "All on").ok);
    g.write(L"emf\\profiles\\off.json", [] {
        std::string t = "{\"name\":\"Off\",\"mods\":[";
        for (int i = 0; i < 150; ++i) t += std::string(i ? "," : "") + "{\"source\":\"eml\",\"id\":\"m" + std::to_string(i) + "\",\"on\":false}";
        for (int i = 0; i < 50; ++i) t += ",{\"source\":\"emf\",\"id\":\"e" + std::to_string(i) + "\",\"on\":false}";
        return t + "]}";
    }());
    const FolderState s = scan_mod_folder(g.dir, ScanDepth::Profiles);
    FolderSwitcher sw(g.dir, folder_facts(s));
    const size_t before = folder_reads();
    const ProfileApply a = apply_saved_profile(g.dir, s, "off", sw);
    CHECK(folder_reads() - before == 0);  // no scan and no manifest lookup per switch
    REQUIRE(a.ok);
    CHECK(a.report.switched.size() == 200);
    CHECK(a.report.failed.empty());
    CHECK(g.has(L"mods\\.m149"));
    CHECK(g.read(L"emf\\config\\e49.ini").find("enabled=0") != std::string::npos);
    // the same through the plain switcher reads the folder for each EML switch (what the facts save)
    FolderSwitcher plain(g.dir);
    const size_t b2 = folder_reads();
    REQUIRE(apply_saved_profile(g.dir, scan_mod_folder(g.dir, ScanDepth::Profiles), "all-on", plain).ok);
    CHECK(folder_reads() - b2 > 100);
}

TEST_CASE("app profiles: the light scan gives profiles the same mods and switches as the Mods tab's scan") {
    TempGame g(L"prof_light");
    every_kind(g);
    g.write(L"mods\\foo\\README.md", "# Foo");
    g.write(L"greeneye\\gmod\\mod.json", R"({"name":"G Mod"})");
    g.write(L"emf\\mods\\broken\\mod.json", "{not json");
    const FolderState full = scan_mod_folder(g.dir), light = scan_mod_folder(g.dir, ScanDepth::Profiles);
    const auto a = profile_folder(full), b = profile_folder(light);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].source == b[i].source);
        CHECK(a[i].id == b[i].id);
        CHECK(a[i].on == b[i].on);
        CHECK(a[i].can_switch == b[i].can_switch);
        CHECK(a[i].read_only == b[i].read_only);
        CHECK(a[i].forced == b[i].forced);
        CHECK(a[i].settings.size() == b[i].settings.size());
    }
    for (const auto& m : light.mods) CHECK(m.readme.empty());  // the light scan skips what profiles do not need
    CHECK(light.report.empty());
    CHECK(light.shroudforge_active == full.shroudforge_active);
}

TEST_CASE("app profiles: an imported file is checked, with the mods this folder lacks and whether its name is taken") {
    TempGame g(L"prof_import");
    every_kind(g);
    g.write(L"shared\\friend.json", R"({"emf_profile":1,"name":"Friend's set","created":"2026-10-20 10:00",
      "mods":[{"source":"emf","id":"clock","on":true},
              {"source":"eml","id":"auto-loot-plus","on":true,"name":"Auto Loot Plus","nexus_id":"170"},
              {"source":"chain","id":"other.dll","on":true}]})");
    ProfileImport i = read_profile_file(g.dir + L"\\shared\\friend.json", g.dir, scan_game_folder(g.dir));
    REQUIRE_MESSAGE(i.profile, i.error);
    CHECK(i.slug == "friend-s-set");
    CHECK_FALSE(i.exists);
    REQUIRE(i.missing.size() == 2);
    CHECK(i.missing[0].id == "auto-loot-plus");
    CHECK(i.missing[0].nexus_id == "170");
    CHECK(i.missing[1].id == "other.dll");
    REQUIRE(save(g, "Friend's set").ok);
    i = read_profile_file(g.dir + L"\\shared\\friend.json", g.dir, scan_game_folder(g.dir));
    CHECK(i.exists);
    CHECK(i.existing_name == "Friend's set");
    g.write(L"shared\\bad.json", R"({"name":"x","mods":[{"source":"eml"}]})");
    i = read_profile_file(g.dir + L"\\shared\\bad.json", g.dir, scan_game_folder(g.dir));
    CHECK_FALSE(i.profile);
    CHECK(i.error.find("mod 1") != std::string::npos);
    i = read_profile_file(g.dir + L"\\shared\\none.json", g.dir, scan_game_folder(g.dir));
    CHECK_FALSE(i.profile);
}
