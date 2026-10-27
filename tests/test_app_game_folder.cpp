// EMF.exe's mod list: what is installed in a game folder (EMF mods, EML or ShroudForge mods, DLL mods EMF starts,
// other loaders' mods), with warnings, and the switches and settings the app changes. Fake game folders in %TEMP%.
#include <windows.h>

#include <doctest.h>

#include <algorithm>
#include <fstream>

#include <string>
#include <vector>

#include "app/game_folder.h"
#include "core/mod_registry.h"
#include "core/mods.h"
#include "core/settings.h"
#include "app/text.h"
#include "app_test_files.h"
#include "test_scratch.h"

using namespace emf::app;
using app_test::TempGame;

namespace {

const char* kTools =
    "shroudforge | ShroudForge | text:shroudforge | beside:shroudforge | same-loader:dbghelp.dll\n"
    "eml | EML (Enshrouded Mod Loader) | text:EML_LOG_FILE_PATH\n"
    "modloader-proxy | ModLoader Proxy | text:chainloader.ini | beside:chainloader.ini\n"
    "shroudtopia | Shroudtopia (or its Shroudweave fork) | text:shroudtopia | beside:shroudtopia.json\n"
    "greeneye | Greeneye Loader | text:greeneye | beside:greeneye\n";

const char* kClock = R"({
  "id": "clock", "name": "Clock", "version": "1.1.0", "description": "The time of day.",
  "authors": ["EMF", "Someone"], "nexus_id": 1234, "game_build": "123456",
  "settings": [
    {"key": "place", "type": "choice", "default": "bottom right",
     "choices": ["top right", "bottom right", "bottom left"], "label": "Corner", "help": "Where it is."},
    {"key": "size", "type": "int", "default": 10, "min": 1, "max": 20, "live": true}
  ]
})";

// EMF installed: its loader as dinput8.dll and its core (the build writes "emf_core <version>" into the DLL).
void install_emf(const TempGame& g) {
    g.write(L"dinput8.dll", "MZ EMF-LOADER-7d3c9f fake");
    g.write(L"emf\\emf_core.dll", std::string("MZ emf_core 0.9.1\0 more", 23));
    g.write(L"emf\\known_tools.txt", kTools);
}

const ModEntry* find(const FolderState& s, const std::string& source, const std::string& id) {
    for (const auto& m : s.mods)
        if (m.source == source && m.id == id) return &m;
    return nullptr;
}

bool has_warning(const FolderState& s, const std::string& level, const std::string& part) {
    for (const auto& w : s.warnings)
        if (w.level == level && w.text.find(part) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---- the folder ----

TEST_CASE("app folder: a folder without the game is reported, not scanned") {
    TempGame g(L"nogame", false);
    const FolderState s = scan_game_folder(g.dir);
    CHECK_FALSE(s.game_found);
    CHECK(s.mods.empty());
    CHECK(has_warning(s, "error", "enshrouded.exe"));
    CHECK_FALSE(is_game_folder(g.dir));
}

TEST_CASE("app folder: a dedicated server folder counts as a game folder") {
    TempGame g(L"server", false);
    g.write(L"enshrouded_server.exe", "fake");
    const FolderState s = scan_game_folder(g.dir);
    CHECK(s.game_found);
    CHECK(s.server);
    CHECK(is_game_folder(g.dir));
}

TEST_CASE("app folder: EMF installed with its version, or a warning that it is not") {
    TempGame g(L"emf");
    FolderState s = scan_game_folder(g.dir);
    CHECK_FALSE(s.emf_installed);
    CHECK(has_warning(s, "warn", "EMF is not installed"));
    install_emf(g);
    s = scan_game_folder(g.dir);
    CHECK(s.emf_installed);
    CHECK(s.emf_version == "0.9.1");
    CHECK_FALSE(has_warning(s, "warn", "EMF is not installed"));
}

// ---- EMF mods ----

TEST_CASE("app EMF mods: mod.json gives name, version, authors, description, Nexus id, README and settings") {
    TempGame g(L"emfmods");
    install_emf(g);
    g.write(L"emf\\mods\\clock\\mod.json", kClock);
    g.write(L"emf\\mods\\clock\\clock.dll", "dll");
    g.write(L"emf\\mods\\clock\\README.md", "# Clock\nShows the time.");
    const FolderState s = scan_game_folder(g.dir);
    const ModEntry* m = find(s, "emf", "clock");
    REQUIRE(m);
    CHECK(m->name == "Clock");
    CHECK(m->version == "1.1.0");
    CHECK(m->authors == std::vector<std::string>{"EMF", "Someone"});
    CHECK(m->description == "The time of day.");
    CHECK(m->loaded_by == "EMF");
    CHECK(m->nexus_id == "1234");
    CHECK(m->game_build == "123456");
    CHECK(m->readme == "# Clock\nShows the time.");
    CHECK(m->enabled);
    CHECK(m->can_toggle);
    CHECK(m->error.empty());
    REQUIRE(m->settings.size() == 2);
    CHECK(m->settings[0].def.key == "place");
    CHECK(m->settings[0].def.label == "Corner");
    CHECK(m->settings[0].value == "1");  // choices are stored by index
    CHECK(m->settings[1].value == "10");
    CHECK(m->settings[1].def.live);
}

TEST_CASE("app EMF mods: the switch and settings come from emf\\config\\<id>.ini; looking writes nothing") {
    TempGame g(L"emfini");
    install_emf(g);
    g.write(L"emf\\mods\\clock\\mod.json", kClock);
    g.write(L"emf\\mods\\clock\\clock.dll", "dll");
    const std::string ini = "[clock]\r\nsize=50\r\nenabled=0\r\n";
    g.write(L"emf\\config\\clock.ini", ini);
    const FolderState s = scan_game_folder(g.dir);
    const ModEntry* m = find(s, "emf", "clock");
    REQUIRE(m);
    CHECK_FALSE(m->enabled);
    CHECK(m->settings[1].value == "20");  // clamped to max, as EMF's core does
    CHECK(g.read(L"emf\\config\\clock.ini") == ini);
}

TEST_CASE("app EMF mods: \"enabled\": false in mod.json is off until the ini says otherwise") {
    TempGame g(L"emfoff");
    install_emf(g);
    g.write(L"emf\\mods\\sim\\mod.json", R"({"id":"sim","version":"1.0.0","enabled":false})");
    g.write(L"emf\\mods\\sim\\sim.dll", "dll");
    CHECK_FALSE(find(scan_game_folder(g.dir), "emf", "sim")->enabled);
    g.write(L"emf\\config\\sim.ini", "[sim]\r\nenabled=1\r\n");
    CHECK(find(scan_game_folder(g.dir), "emf", "sim")->enabled);
}

TEST_CASE("app EMF mods: an unreadable mod.json or a missing DLL is shown with the error, not hidden") {
    TempGame g(L"emfbad");
    install_emf(g);
    g.write(L"emf\\mods\\broken\\mod.json", "{ not json");
    g.write(L"emf\\mods\\nodll\\mod.json", R"({"id":"nodll","version":"1.0.0"})");
    const FolderState s = scan_game_folder(g.dir);
    const ModEntry* b = find(s, "emf", "broken");
    REQUIRE(b);
    CHECK(b->error.find("not valid JSON") != std::string::npos);
    CHECK_FALSE(b->can_toggle);
    const ModEntry* n = find(s, "emf", "nodll");
    REQUIRE(n);
    CHECK(n->error.find("nodll.dll not found") != std::string::npos);
    CHECK(has_warning(s, "warn", "broken"));
}

TEST_CASE("app EMF mods: a loose DLL in emf\\mods is a mod named after its file") {
    TempGame g(L"emfloose");
    install_emf(g);
    g.write(L"emf\\mods\\Loose Mod.dll", "dll");
    const FolderState s = scan_game_folder(g.dir);
    const ModEntry* m = find(s, "emf", "loose_mod");
    REQUIRE(m);
    CHECK(m->name == "Loose Mod");
    CHECK(m->can_toggle);
}

// ---- EML and ShroudForge mods ----

TEST_CASE("app EML mods: folders and zips in mods\\; a leading '.' means off (EML skips those)") {
    TempGame g(L"eml");
    g.write(L"dbghelp.dll", "EML_LOG_FILE_PATH");
    g.write(L"emf\\known_tools.txt", kTools);
    g.write(L"mods\\foo\\mod.json",
            R"({"id":"foo","name":"Foo","version":"0.1.0","authors":["A"],"description":"d","dependencies":["bar"]})");
    g.write(L"mods\\.bar\\mod.json", R"({"id":"bar","name":"Bar","version":"1.0.0"})");
    g.write(L"mods\\pack.zip", "PK");
    g.write(L"mods\\.old.zip", "PK");
    const FolderState s = scan_game_folder(g.dir);
    const ModEntry* foo = find(s, "eml", "foo");
    REQUIRE(foo);
    CHECK(foo->name == "Foo");
    CHECK(foo->version == "0.1.0");
    CHECK(foo->authors == std::vector<std::string>{"A"});
    CHECK(foo->depends == std::vector<std::string>{"bar"});
    CHECK(foo->loaded_by == "EML");
    CHECK(foo->enabled);
    CHECK(foo->can_toggle);
    const ModEntry* bar = find(s, "eml", "bar");
    REQUIRE(bar);
    CHECK_FALSE(bar->enabled);
    REQUIRE(find(s, "eml", "pack.zip"));
    CHECK(find(s, "eml", "pack.zip")->enabled);
    REQUIRE(find(s, "eml", "old.zip"));
    CHECK_FALSE(find(s, "eml", "old.zip")->enabled);
}

TEST_CASE("app EML mods: a broken mod.json or a folder without one is shown with the error") {
    TempGame g(L"emlbad");
    g.write(L"mods\\bad\\mod.json", "{");
    g.write(L"mods\\nested\\nested\\mod.json", R"({"id":"nested","version":"1.0.0"})");
    const FolderState s = scan_game_folder(g.dir);
    REQUIRE(find(s, "eml", "bad"));
    CHECK_FALSE(find(s, "eml", "bad")->error.empty());
    REQUIRE(find(s, "eml", "nested"));
    CHECK(find(s, "eml", "nested")->error.find("no mod.json") != std::string::npos);
}

TEST_CASE("app EML mods: a '.' folder without mod.json (.git, an editor's folder) is not a mod") {
    TempGame g(L"emldot");
    g.write(L"mods\\.git\\config", "x");
    g.write(L"mods\\.foo\\mod.json", R"({"id":"foo","version":"1.0.0"})");
    const FolderState s = scan_game_folder(g.dir);
    CHECK_FALSE(find(s, "eml", "git"));
    CHECK(find(s, "eml", "foo"));
}

TEST_CASE("app EML mods: EML mods without EML or ShroudForge next to the game get a warning") {
    TempGame g(L"emlnoloader");
    g.write(L"emf\\known_tools.txt", kTools);
    g.write(L"mods\\foo\\mod.json", R"({"id":"foo","version":"1.0.0"})");
    CHECK(has_warning(scan_game_folder(g.dir), "warn", "EML"));
}

TEST_CASE("app ShroudForge: when it is the active loader, its packages' switch is extended.mod.json (read-only)") {
    TempGame g(L"sf");
    g.write(L"emf\\known_tools.txt", kTools);
    g.mkdir(L"shroudforge");
    g.write(L"dbghelp.dll", "... ShroudForge ... EML_LOG_FILE_PATH");
    g.write(L"mods\\a\\mod.json", R"({"id":"a","version":"1.0.0"})");
    g.write(L"mods\\a\\extended.mod.json", R"({"schemaVersion":1,"enabled":true})");
    g.write(L"mods\\b\\mod.json", R"({"id":"b","version":"1.0.0"})");  // plain EML mod: off in ShroudForge
    const FolderState s = scan_game_folder(g.dir);
    CHECK(s.shroudforge_active);
    const ModEntry* a = find(s, "eml", "a");
    REQUIRE(a);
    CHECK(a->loaded_by == "ShroudForge");
    CHECK(a->enabled);
    CHECK_FALSE(a->can_toggle);
    CHECK(a->toggle_note.find("F9") != std::string::npos);
    REQUIRE(find(s, "eml", "b"));
    CHECK_FALSE(find(s, "eml", "b")->enabled);
}

TEST_CASE("app ShroudForge: a package where ShroudForge is not active is an EML mod") {
    TempGame g(L"sfoff");
    g.write(L"dbghelp.dll", "EML_LOG_FILE_PATH");
    g.write(L"emf\\known_tools.txt", kTools);
    g.write(L"mods\\a\\mod.json", R"({"id":"a","version":"1.0.0"})");
    g.write(L"mods\\a\\extended.mod.json", R"({"enabled":false})");
    const FolderState s = scan_game_folder(g.dir);
    CHECK_FALSE(s.shroudforge_active);
    const ModEntry* a = find(s, "eml", "a");
    REQUIRE(a);
    CHECK(a->loaded_by == "EML");
    CHECK(a->enabled);
    CHECK(a->can_toggle);
    CHECK(a->note.find("ShroudForge") != std::string::npos);
}

// ---- DLL mods EMF starts (dinput8-chain.ini) ----

TEST_CASE("app chain: dinput8-chain.ini entries with their status from emf\\chain_status.txt") {
    TempGame g(L"chain");
    install_emf(g);
    g.write(L"dinput8-chain.ini", "[Chain]\r\ncreative.dll\r\n; statsliders.dll\r\ngone.dll\r\n; just a comment\r\nnew.dll\r\n");
    g.write(L"creative.dll", "creative");
    g.write(L"statsliders.dll", "sliders");
    g.write(L"new.dll", "new");
    g.write(L"emf\\chain_status.txt", "creative.dll\tstarted\r\ngone.dll\tmissing\r\n");
    const FolderState s = scan_game_folder(g.dir);
    const ModEntry* c = find(s, "chain", "creative.dll");
    REQUIRE(c);
    CHECK(c->enabled);
    CHECK(c->can_toggle);
    CHECK(c->status == "started");
    CHECK(c->loaded_by == "EMF");
    const ModEntry* off = find(s, "chain", "statsliders.dll");
    REQUIRE(off);
    CHECK_FALSE(off->enabled);
    CHECK(off->can_toggle);
    const ModEntry* gone = find(s, "chain", "gone.dll");
    REQUIRE(gone);
    CHECK(gone->status == "missing");
    CHECK_FALSE(gone->error.empty());
    CHECK(find(s, "chain", "new.dll")->status.empty());  // listed after the last game start
    int chained = 0;
    for (const auto& m : s.mods) chained += m.source == "chain";
    CHECK(chained == 4);  // "; just a comment" is no entry
    CHECK(has_warning(s, "warn", "gone.dll"));
}

TEST_CASE("app chain: a DLL that failed to load gets Windows' own words for the error, with its name") {
    TempGame g(L"chainfailed");
    install_emf(g);
    g.write(L"dinput8-chain.ini", "[Chain]\r\nbad.dll\r\n");
    g.write(L"bad.dll", "not a dll");
    g.write(L"emf\\chain_status.txt", "bad.dll\tfailed 193\r\n");
    const FolderState s = scan_game_folder(g.dir);
    const ModEntry* m = find(s, "chain", "bad.dll");
    REQUIRE(m);
    CHECK(m->status == "failed 193");
    std::string why = emf::app::windows_error_text(193);  // "%1 is not a valid Win32 application. (Windows error 193)"
    if (const size_t at = why.find("%1"); at != std::string::npos) why.replace(at, 2, "bad.dll");
    CHECK(m->error == "did not load at the last game start: " + why);
    CHECK(m->error.find("(Windows error 193)") != std::string::npos);
}

TEST_CASE("app chain: a tool's second copy that the installer kept aside (emf\\moved.txt) is shown as not started") {
    TempGame g(L"keptaside");
    install_emf(g);
    g.write(L"dinput8-chain.ini", "[Chain]\r\n");
    g.write(L"shroudforge.dll", "shroudforge");
    g.write(L"dbghelp.dll", "shroudforge");  // the same loader runs through its other proxy name (same-loader)
    g.write(L"emf\\moved.txt", "moved\tshroudforge.dll\r\n");
    const FolderState s = scan_game_folder(g.dir);
    const ModEntry* m = find(s, "chain", "shroudforge.dll");
    REQUIRE(m);
    CHECK(m->status == "kept aside");
    CHECK_FALSE(m->enabled);
    CHECK_FALSE(m->can_toggle);
}

TEST_CASE("app chain: chainloader.ini is ModLoader Proxy's list, shown read-only with a note") {
    TempGame g(L"mlproxy");
    install_emf(g);
    g.write(L"chainloader.ini", "[Settings]\r\nDelayMs=0\r\nForwardTo=fwd.dll\r\n[Load]\r\natlas.dll\r\n");
    const FolderState s = scan_game_folder(g.dir);
    CHECK_FALSE(find(s, "chain", "atlas.dll"));
    bool listed = false;
    for (const auto& m : s.mods)
        if (m.source == "other" && m.name.find("atlas") != std::string::npos) {
            listed = true;
            CHECK(m.loaded_by == "ModLoader Proxy");
            CHECK_FALSE(m.can_toggle);
        }
    CHECK(listed);
    for (const auto& m : s.mods) CHECK(m.name.find("fwd") == std::string::npos);  // a setting, not its list
    CHECK(has_warning(s, "info", "chainloader.ini"));
}

TEST_CASE("app chain: switching a line off puts \"; \" in front; on takes it away; every other byte stays") {
    const std::string text = "\xEF\xBB\xBF" "a.dll\r\n[Chain]\r\n  b.dll ; Creative\r\nmod3 = C.DLL\nlast.dll";
    const auto off_a = switch_chain_line(text, L"A.dll", false);
    REQUIRE(off_a);
    CHECK(*off_a == "\xEF\xBB\xBF" "; a.dll\r\n[Chain]\r\n  b.dll ; Creative\r\nmod3 = C.DLL\nlast.dll");
    const auto off_b = switch_chain_line(*off_a, L"b.dll", false);
    REQUIRE(off_b);
    CHECK(*off_b == "\xEF\xBB\xBF" "; a.dll\r\n[Chain]\r\n;   b.dll ; Creative\r\nmod3 = C.DLL\nlast.dll");
    const auto off_c = switch_chain_line(*off_b, L"c.dll", false);
    REQUIRE(off_c);
    const auto off_last = switch_chain_line(*off_c, L"last.dll", false);
    REQUIRE(off_last);
    CHECK(*off_last == "\xEF\xBB\xBF" "; a.dll\r\n[Chain]\r\n;   b.dll ; Creative\r\n; mod3 = C.DLL\n; last.dll");
    CHECK(switched_off_chain_entries(*off_last) ==
          std::vector<std::wstring>{L"a.dll", L"b.dll", L"C.DLL", L"last.dll"});
    // and back, to the same bytes
    std::string back = *off_last;
    for (const wchar_t* n : {L"a.dll", L"b.dll", L"c.dll", L"last.dll"}) {
        const auto on = switch_chain_line(back, n, true);
        REQUIRE(on);
        back = *on;
    }
    CHECK(back == text);
}

TEST_CASE("app chain: nothing to switch gives nullopt; comments that are not switched-off entries are left alone") {
    CHECK_FALSE(switch_chain_line("a.dll\r\n", L"b.dll", false));
    CHECK_FALSE(switch_chain_line("a.dll\r\n", L"a.dll", true));  // already on
    CHECK_FALSE(switch_chain_line("; a.dll\r\n", L"a.dll", false));
    CHECK(switched_off_chain_entries("; settings below\r\n# x.dll\r\n;y.dll\r\n; z.dll\r\n; z.dll\r\n") ==
          std::vector<std::wstring>{L"y.dll", L"z.dll"});  // ";y.dll" is off too, as the installer reads it
}

// ---- other loaders ----

TEST_CASE("app other loaders: proxy DLLs named after their tool; Greeneye and Shroudtopia mods read-only") {
    TempGame g(L"others");
    install_emf(g);
    g.write(L"dbghelp.dll", "EML_LOG_FILE_PATH");
    g.write(L"dbgcore.dll", "greeneye loader");
    g.write(L"winmm.dll", "shroudtopia");
    g.write(L"greeneye\\continue\\mod.json",
            R"({"id":"continue","name":"Continue","version":"1.2","dll":"continue.dll","side":"client","api":1,)"
            R"("description":"Continue from the menu.","author":"G"})");
    g.write(L"greeneye\\continue\\continue.dll", "dll");
    g.write(L"shroudtopia.json", R"({"mods":{"Minimap":{"active":true},"FirstPerson":{"active":false}}})");
    g.write(L"mods\\Minimap\\minimap.dll", "dll");
    g.write(L"mods\\loose.dll", "dll");
    const FolderState s = scan_game_folder(g.dir);

    bool eml_loader = false;
    for (const auto& m : s.mods)
        if (m.source == "other" && m.is_loader && m.name == "EML (Enshrouded Mod Loader)") {
            eml_loader = true;
            CHECK(m.note.find("dbghelp.dll") != std::string::npos);
        }
    CHECK(eml_loader);
    const ModEntry* cont = nullptr;
    const ModEntry* minimap = nullptr;
    const ModEntry* fp = nullptr;
    const ModEntry* loose = nullptr;
    for (const auto& m : s.mods) {
        if (m.source != "other" || m.is_loader) continue;
        if (m.name == "Continue") cont = &m;
        if (m.name == "Minimap") minimap = &m;
        if (m.name == "FirstPerson") fp = &m;
        if (m.name == "loose") loose = &m;
    }
    REQUIRE(cont);
    CHECK(cont->loaded_by == "Greeneye Loader");
    CHECK(cont->version == "1.2");
    CHECK(cont->authors == std::vector<std::string>{"G"});
    CHECK_FALSE(cont->can_toggle);
    REQUIRE(minimap);
    CHECK(minimap->loaded_by.find("Shroudtopia") != std::string::npos);
    CHECK(minimap->enabled);
    REQUIRE(fp);
    CHECK_FALSE(fp->enabled);
    REQUIRE(loose);
    CHECK_FALSE(find(s, "eml", "Minimap"));  // a DLL folder without mod.json is Shroudtopia's, not a broken EML mod
}

TEST_CASE("app warnings: the start-up report, and a loader whose file another tool took") {
    TempGame g(L"report");
    install_emf(g);
    g.mkdir(L"shroudforge");
    g.write(L"dbghelp.dll", "EML_LOG_FILE_PATH");  // EML's DLL took ShroudForge's file name
    const FolderState s = scan_game_folder(g.dir);
    REQUIRE_FALSE(s.report.empty());
    CHECK(s.report[0].find("dinput8.dll = EMF") != std::string::npos);
    CHECK(has_warning(s, "warn", "ShroudForge is installed"));
}

// ---- changes ----

TEST_CASE("app switch: an EMF mod's switch is \"enabled\" in emf\\config\\<id>.ini") {
    TempGame g(L"setemf");
    install_emf(g);
    g.write(L"emf\\mods\\clock\\mod.json", kClock);
    g.write(L"emf\\mods\\clock\\clock.dll", "dll");
    Change c = set_mod_enabled(g.dir, "emf", "clock", false);
    CHECK(c.ok);
    CHECK(g.read(L"emf\\config\\clock.ini").find("enabled=0") != std::string::npos);
    CHECK_FALSE(find(scan_game_folder(g.dir), "emf", "clock")->enabled);
    CHECK(set_mod_enabled(g.dir, "emf", "clock", true).ok);
    CHECK(find(scan_game_folder(g.dir), "emf", "clock")->enabled);
    c = set_mod_enabled(g.dir, "emf", "nothere", false);
    CHECK_FALSE(c.ok);
    CHECK_FALSE(c.error.empty());
}

TEST_CASE("app switch: an EML mod is renamed to and from a leading '.'; nothing is deleted") {
    TempGame g(L"seteml");
    g.write(L"mods\\foo\\mod.json", R"({"id":"foo","version":"1.0.0"})");
    g.write(L"mods\\foo\\src\\mod.lua", "-- lua");
    g.write(L"mods\\pack.zip", "PK");
    CHECK(set_mod_enabled(g.dir, "eml", "foo", false).ok);
    CHECK_FALSE(g.has(L"mods\\foo"));
    CHECK(g.read(L"mods\\.foo\\src\\mod.lua") == "-- lua");
    CHECK(set_mod_enabled(g.dir, "eml", "foo", true).ok);
    CHECK(g.read(L"mods\\foo\\src\\mod.lua") == "-- lua");
    CHECK(set_mod_enabled(g.dir, "eml", "pack.zip", false).ok);
    CHECK(g.has(L"mods\\.pack.zip"));
    CHECK(set_mod_enabled(g.dir, "eml", "foo", true).ok);  // already on: nothing to do
}

TEST_CASE("app switch: an EML rename never replaces a folder that is already there") {
    TempGame g(L"emlclash");
    g.write(L"mods\\foo\\mod.json", R"({"id":"foo","version":"2.0.0"})");
    g.write(L"mods\\.foo\\mod.json", R"({"id":"foo","version":"1.0.0"})");
    const Change c = set_mod_enabled(g.dir, "eml", "foo", false);
    CHECK_FALSE(c.ok);
    CHECK(c.error.find(".foo") != std::string::npos);
    CHECK(g.read(L"mods\\foo\\mod.json").find("2.0.0") != std::string::npos);
    CHECK(g.read(L"mods\\.foo\\mod.json").find("1.0.0") != std::string::npos);
}

TEST_CASE("app switch: names that leave mods\\ are refused, and so are ShroudForge packages and other loaders") {
    TempGame g(L"emlrefuse");
    g.write(L"mods\\foo\\mod.json", R"({"id":"foo","version":"1.0.0"})");
    for (const char* bad : {"..", ".", "", "a\\b", "a/b", "..\\emf", "c:x"}) CHECK_FALSE(set_mod_enabled(g.dir, "eml", bad, false).ok);
    CHECK_FALSE(set_mod_enabled(g.dir, "other", "anything", false).ok);
    g.write(L"emf\\known_tools.txt", kTools);
    g.mkdir(L"shroudforge");
    g.write(L"dbghelp.dll", "shroudforge");
    const Change c = set_mod_enabled(g.dir, "eml", "foo", false);
    CHECK_FALSE(c.ok);
    CHECK(c.error.find("F9") != std::string::npos);
    CHECK(g.has(L"mods\\foo"));
}

TEST_CASE("app switch: a chained DLL is commented out in dinput8-chain.ini and back, byte for byte") {
    TempGame g(L"setchain");
    install_emf(g);
    const std::string text = "[Chain]\r\ncreative.dll\r\nother.dll\r\n";
    g.write(L"dinput8-chain.ini", text);
    g.write(L"creative.dll", "x");
    CHECK(set_mod_enabled(g.dir, "chain", "creative.dll", false).ok);
    CHECK(g.read(L"dinput8-chain.ini") == "[Chain]\r\n; creative.dll\r\nother.dll\r\n");
    CHECK_FALSE(find(scan_game_folder(g.dir), "chain", "creative.dll")->enabled);
    CHECK(set_mod_enabled(g.dir, "chain", "creative.dll", true).ok);
    CHECK(g.read(L"dinput8-chain.ini") == text);
    CHECK_FALSE(set_mod_enabled(g.dir, "chain", "nothere.dll", false).ok);
}

TEST_CASE("app switch: a chain file too large to read whole is refused, not cut short") {
    TempGame g(L"chainbig");
    install_emf(g);
    std::string text = "[Chain]\r\ncreative.dll\r\n";
    text += "; " + std::string(2u << 20, 'x') + "\r\n";  // 2 MiB of comment after the entry
    g.write(L"dinput8-chain.ini", text);
    g.write(L"creative.dll", "x");
    const Change c = set_mod_enabled(g.dir, "chain", "creative.dll", false);
    CHECK_FALSE(c.ok);
    CHECK(c.error.find("too large") != std::string::npos);
    CHECK(g.read(L"dinput8-chain.ini") == text);
}

TEST_CASE("app settings: saving one setting leaves the mod's other values in the ini as they are") {
    TempGame g(L"setkeep");
    install_emf(g);
    g.write(L"emf\\mods\\clock\\mod.json", kClock);
    g.write(L"emf\\mods\\clock\\clock.dll", "dll");
    g.write(L"emf\\config\\clock.ini", "[clock]\r\nsize=50\r\n");  // out of range: EMF's core clamps it at its next start
    CHECK(set_mod_setting(g.dir, "clock", "place", "top right").ok);
    const std::string ini = g.read(L"emf\\config\\clock.ini");
    CHECK(ini.find("size=50") != std::string::npos);
    CHECK(ini.find("place=0") != std::string::npos);
}

TEST_CASE("app README: a long README cut in the middle of a character stays UTF-8") {
    TempGame g(L"readme");
    install_emf(g);
    g.write(L"emf\\mods\\clock\\mod.json", kClock);
    g.write(L"emf\\mods\\clock\\clock.dll", "dll");
    std::string readme = "# Uhr\n" + std::string(65536 - 7, 'a') + "\xC3\xA9 and more";  // the 64 KB cut splits the "é"
    g.write(L"emf\\mods\\clock\\README.md", readme);
    const FolderState s = scan_game_folder(g.dir);
    const ModEntry* m = find(s, "emf", "clock");
    REQUIRE(m);
    CHECK(m->readme.compare(0, 6, "# Uhr\n") == 0);
    CHECK(m->readme.size() == 65536 - 1);  // up to the last whole character, read as UTF-8 (not re-read as ANSI)
}

TEST_CASE("app settings: a value is checked like the Mods tab and written to the mod's ini") {
    TempGame g(L"setsetting");
    install_emf(g);
    g.write(L"emf\\mods\\clock\\mod.json", kClock);
    g.write(L"emf\\mods\\clock\\clock.dll", "dll");
    CHECK(set_mod_setting(g.dir, "clock", "size", "15").ok);
    CHECK(g.read(L"emf\\config\\clock.ini").find("size=15") != std::string::npos);
    Change c = set_mod_setting(g.dir, "clock", "size", "99");
    CHECK_FALSE(c.ok);
    CHECK(c.error.find("between 1 and 20") != std::string::npos);
    CHECK(set_mod_setting(g.dir, "clock", "place", "top right").ok);
    CHECK(g.read(L"emf\\config\\clock.ini").find("place=0") != std::string::npos);
    CHECK_FALSE(set_mod_setting(g.dir, "clock", "nokey", "1").ok);
    CHECK_FALSE(set_mod_setting(g.dir, "nomod", "size", "1").ok);
    CHECK(find(scan_game_folder(g.dir), "emf", "clock")->settings[1].value == "15");
}

// ---- edge cases ----

TEST_CASE("app chain: only [Chain] and lines before any section count; another tool's section is left alone") {
    const std::string text = "[Chain]\r\ncreative.dll\r\n[OtherTool]\r\nPreload=creative.dll\r\n; helper.dll\r\n";
    const auto off = switch_chain_line(text, L"creative.dll", false);
    REQUIRE(off);
    CHECK(*off == "[Chain]\r\n; creative.dll\r\n[OtherTool]\r\nPreload=creative.dll\r\n; helper.dll\r\n");
    CHECK(switched_off_chain_entries(text).empty());
    const auto on = switch_chain_line(*off, L"creative.dll", true);
    REQUIRE(on);
    CHECK(*on == text);
    CHECK(switched_off_chain_entries("top.dll\r\n; early.dll\r\n[chain]\r\n; late.dll\r\n[x]\r\n; no.dll\r\n") ==
          std::vector<std::wstring>{L"early.dll", L"late.dll"});

    TempGame g(L"chainsections");
    install_emf(g);
    g.write(L"dinput8-chain.ini", text);
    g.write(L"creative.dll", "x");
    g.write(L"helper.dll", "x");
    const FolderState s = scan_game_folder(g.dir);
    CHECK(find(s, "chain", "creative.dll"));
    CHECK_FALSE(find(s, "chain", "helper.dll"));
    CHECK(set_mod_enabled(g.dir, "chain", "creative.dll", false).ok);
    CHECK(g.read(L"dinput8-chain.ini") == *off);
}

TEST_CASE("app switch: a UTF-16 chain file is edited as UTF-16, with its BOM and every other line") {
    const auto utf16 = [](const std::wstring& w) {
        std::string b = "\xFF\xFE";
        b.append(reinterpret_cast<const char*>(w.data()), w.size() * sizeof(wchar_t));
        return b;
    };
    TempGame g(L"chain16");
    install_emf(g);
    const std::string original = utf16(L"[Chain]\r\ncreative.dll\r\nm\u00f6d.dll\r\n");
    g.write(L"dinput8-chain.ini", original);
    g.write(L"creative.dll", "x");
    g.write(L"m\u00f6d.dll", "x");
    CHECK(set_mod_enabled(g.dir, "chain", "creative.dll", false).ok);
    CHECK(g.read(L"dinput8-chain.ini") == utf16(L"[Chain]\r\n; creative.dll\r\nm\u00f6d.dll\r\n"));
    const FolderState s = scan_game_folder(g.dir);
    REQUIRE(find(s, "chain", "creative.dll"));
    CHECK_FALSE(find(s, "chain", "creative.dll")->enabled);
    REQUIRE(find(s, "chain", "m\xC3\xB6" "d.dll"));
    CHECK(find(s, "chain", "m\xC3\xB6" "d.dll")->enabled);
    CHECK(set_mod_enabled(g.dir, "chain", "creative.dll", true).ok);
    CHECK(g.read(L"dinput8-chain.ini") == original);
}

TEST_CASE("app EMF mods: a script mod whose host is switched off says it will not load, and why") {
    TempGame g(L"willnotload_host");
    install_emf(g);
    g.write(L"emf\\mods\\lua\\mod.json", R"({"id":"lua","version":"1.1.0"})");
    g.write(L"emf\\mods\\lua\\lua.dll", "dll");
    g.write(L"emf\\config\\lua.ini", "[lua]\r\nenabled=0\r\n");
    g.write(L"emf\\mods\\life_skills\\mod.json",
            R"({"id":"life_skills","name":"Life Skills","version":"1.0.0","main":"main.lua","depends":{"lua":">=1.1"}})");
    g.write(L"emf\\mods\\life_skills\\main.lua", "-- lua");
    const FolderState s = scan_game_folder(g.dir);
    const ModEntry* ls = find(s, "emf", "life_skills");
    REQUIRE(ls);
    CHECK(ls->error.rfind("Will not load: ", 0) == 0);
    CHECK(ls->error.find("lua") != std::string::npos);
    CHECK(ls->error.find("switched off") != std::string::npos);
    CHECK(find(s, "emf", "lua")->error.empty());  // switched off is not an error
}

TEST_CASE("app EMF mods: a missing or too old dependency, a newer EMF API or a cycle says it will not load") {
    TempGame g(L"willnotload_deps");
    install_emf(g);
    g.write(L"emf\\mods\\clock\\mod.json", kClock);
    g.write(L"emf\\mods\\clock\\clock.dll", "dll");
    const auto mod = [&](const wchar_t* id, const std::string& json) {
        g.write(std::wstring(L"emf\\mods\\") + id + L"\\mod.json", json);
        g.write(std::wstring(L"emf\\mods\\") + id + L"\\" + id + L".dll", "dll");
    };
    mod(L"needs_missing", R"({"id":"needs_missing","version":"1.0.0","depends":{"nothere":"*"}})");
    mod(L"needs_newer", R"({"id":"needs_newer","version":"1.0.0","depends":{"clock":">=9.0"}})");
    mod(L"new_api", R"({"id":"new_api","version":"1.0.0","emf":999})");
    mod(L"cycle_a", R"({"id":"cycle_a","version":"1.0.0","depends":{"cycle_b":"*"}})");
    mod(L"cycle_b", R"({"id":"cycle_b","version":"1.0.0","depends":{"cycle_a":"*"}})");
    mod(L"off_missing", R"({"id":"off_missing","version":"1.0.0","enabled":false,"depends":{"nothere":"*"}})");
    const FolderState s = scan_game_folder(g.dir);
    CHECK(find(s, "emf", "clock")->error.empty());
    CHECK(find(s, "emf", "needs_missing")->error == "Will not load: needs nothere, which is not installed");
    CHECK(find(s, "emf", "needs_newer")->error == "Will not load: needs clock >=9.0 (9.0.0 or newer), found 1.1.0: too old (update clock)");
    CHECK(find(s, "emf", "new_api")->error.find("Will not load: needs EMF API 999") == 0);
    CHECK(find(s, "emf", "cycle_a")->error.find("Will not load: ") == 0);
    CHECK(find(s, "emf", "cycle_b")->error.find("Will not load: ") == 0);
    CHECK(find(s, "emf", "off_missing")->error.empty());  // switched off: it does not load anyway
}

TEST_CASE("app ShroudForge: not the active loader when its config hands the mods to EML") {
    TempGame g(L"sf_eml");
    g.write(L"emf\\known_tools.txt", kTools);
    g.write(L"dbghelp.dll", "shroudforge EML_LOG_FILE_PATH");
    g.write(L"shroudforge\\config\\modloader-config.json", R"({"modloader": "eml"})");
    g.write(L"mods\\a\\mod.json", R"({"id":"a","version":"1.0.0"})");
    const FolderState s = scan_game_folder(g.dir);
    CHECK_FALSE(s.shroudforge_active);
    REQUIRE(find(s, "eml", "a"));
    CHECK(find(s, "eml", "a")->loaded_by == "EML");
    CHECK(find(s, "eml", "a")->can_toggle);
}

TEST_CASE("app ShroudForge: zip mods are EML's even while ShroudForge is active (it ignores zips)") {
    TempGame g(L"sf_zip");
    g.write(L"emf\\known_tools.txt", kTools);
    g.mkdir(L"shroudforge");
    g.write(L"dbghelp.dll", "shroudforge");
    g.write(L"mods\\pack.zip", "PK");
    const FolderState s = scan_game_folder(g.dir);
    CHECK(s.shroudforge_active);
    const ModEntry* z = find(s, "eml", "pack.zip");
    REQUIRE(z);
    CHECK(z->loaded_by == "EML");
    CHECK(z->can_toggle);
    CHECK(set_mod_enabled(g.dir, "eml", "pack.zip", false).ok);
    CHECK(g.has(L"mods\\.pack.zip"));
}

TEST_CASE("app chain: a file the installer renamed whose tool does not run through another proxy is just not listed") {
    TempGame g(L"notlisted");
    install_emf(g);
    g.write(L"dinput8-chain.ini", "[Chain]\r\n");
    g.write(L"dinput8-previous.dll", "someone's proxy");
    g.write(L"emf\\moved.txt", "moved\tdinput8-previous.dll\r\n");
    const FolderState s = scan_game_folder(g.dir);
    const ModEntry* m = find(s, "chain", "dinput8-previous.dll");
    REQUIRE(m);
    CHECK(m->status != "kept aside");
    CHECK(m->note.find("renamed by EMF's installer; not listed in dinput8-chain.ini") != std::string::npos);
    CHECK_FALSE(m->enabled);
}

TEST_CASE("app list: every entry has its own key, also for mods\\foo next to mods\\.foo and duplicate ids") {
    TempGame g(L"keys");
    install_emf(g);
    g.write(L"mods\\foo\\mod.json", R"({"id":"foo","version":"2.0.0"})");
    g.write(L"mods\\.foo\\mod.json", R"({"id":"foo","version":"1.0.0"})");
    g.write(L"emf\\mods\\one\\mod.json", R"({"id":"same","version":"1.0.0"})");
    g.write(L"emf\\mods\\one\\same.dll", "dll");
    g.write(L"emf\\mods\\two\\mod.json", R"({"id":"same","version":"1.0.0"})");
    g.write(L"emf\\mods\\two\\same.dll", "dll");
    const FolderState s = scan_game_folder(g.dir);
    std::vector<std::string> keys;
    for (const auto& m : s.mods) {
        CHECK_FALSE(m.key.empty());
        CHECK(std::find(keys.begin(), keys.end(), m.key) == keys.end());
        keys.push_back(m.key);
    }
    CHECK(keys.size() == s.mods.size());
}

TEST_CASE("app EML mods: a folder without mod.json, or a DLL folder while EML runs, says EML stops the game") {
    TempGame g(L"emlstops");
    g.write(L"emf\\known_tools.txt", kTools);
    g.write(L"dbghelp.dll", "EML_LOG_FILE_PATH");
    g.write(L"mods\\empty\\notes.txt", "x");
    g.write(L"mods\\Minimap\\minimap.dll", "dll");
    const FolderState s = scan_game_folder(g.dir);
    REQUIRE(find(s, "eml", "empty"));
    CHECK(find(s, "eml", "empty")->error.find("stops the game") != std::string::npos);
    bool dll_folder = false;
    for (const auto& m : s.mods)
        if (m.name == "Minimap") {
            dll_folder = true;
            CHECK(m.error.find("stops the game") != std::string::npos);
        }
    CHECK(dll_folder);
}

TEST_CASE("app EMF mods: an optional dependency that is missing or switched off is no error; a range shows as written") {
    TempGame g(L"optional_deps");
    install_emf(g);
    g.write(L"emf\\mods\\clock\\mod.json", kClock);
    g.write(L"emf\\mods\\clock\\clock.dll", "dll");
    g.write(L"emf\\config\\clock.ini", "[clock]\r\nenabled=0\r\n");
    const auto mod = [&](const wchar_t* id, const std::string& json) {
        g.write(std::wstring(L"emf\\mods\\") + id + L"\\mod.json", json);
        g.write(std::wstring(L"emf\\mods\\") + id + L"\\" + id + L".dll", "dll");
    };
    mod(L"uses_clock", R"({"id":"uses_clock","version":"1.0.0","depends":{"clock":{"version":"^1.0","optional":true},"nothere":{"optional":true}}})");
    mod(L"too_new", R"({"id":"too_new","version":"1.0.0","depends":{"clock":"~1.0"}})");
    const FolderState s = scan_game_folder(g.dir);
    const ModEntry* u = find(s, "emf", "uses_clock");
    REQUIRE(u);
    CHECK(u->error.empty());
    CHECK(u->depends == std::vector<std::string>{"clock ^1.0 (optional)", "nothere (optional)"});
    CHECK(find(s, "emf", "too_new")->error ==
          "Will not load: needs clock ~1.0 (1.0.0 or newer, below 1.1.0), found 1.1.0: too new (look for an update of this mod)");
}

namespace {
const char* kStarter = R"({"id":"starter","version":"1.0.0","main":"main.lua","settings":[
  {"key":"keys_note","type":"text","default":"x","group":"Keys"},
  {"key":"show","type":"bool","default":true,"label":"Show the line","group":"On screen"},
  {"key":"loose","type":"int","default":1},
  {"key":"corner","type":"select","default":"bl","group":"On screen","enabledWhen":{"setting":"show","equals":true},
   "options":[{"value":"tr","label":"Top right"},{"value":"bl","label":"Bottom left"}]},
  {"key":"tint","type":"color","default":"#FFD070","group":"On screen"},
  {"key":"speed","type":"slider","min":0,"max":2,"step":0.5,"default":1,"group":"On screen"},
  {"key":"toggle","type":"keybind","default":"F7","group":"Keys"}]})";
}  // namespace

TEST_CASE("app settings: listed as the Mods tab shows them, with what greys them out and what they control") {
    TempGame g(L"settings_rich");
    install_emf(g);
    g.write(L"emf\\mods\\starter\\mod.json", kStarter);
    g.write(L"emf\\mods\\starter\\main.lua", "-- lua");
    g.write(L"emf\\config\\starter.ini", "[starter]\r\nshow=0\r\n");
    const FolderState s = scan_game_folder(g.dir);
    const ModEntry* m = find(s, "emf", "starter");
    REQUIRE(m);
    std::vector<std::string> keys;
    for (const auto& st : m->settings) keys.push_back(st.def.key);
    CHECK(keys == std::vector<std::string>{"loose", "keys_note", "toggle", "show", "corner", "tint", "speed"});
    const auto at = [&](const char* key) -> const ModSetting& {
        for (const auto& st : m->settings)
            if (st.def.key == key) return st;
        FAIL("no setting " << key);
        return m->settings[0];
    };
    CHECK_FALSE(at("corner").active);  // show is off in the ini
    CHECK(at("corner").when == "only while Show the line is on");
    CHECK(at("show").controls);
    CHECK_FALSE(at("tint").controls);
    CHECK(at("tint").active);
    CHECK(at("tint").value == "#ffd070");
    CHECK(at("speed").def.slider);
    CHECK(at("toggle").value == "F7");
}

TEST_CASE("app settings: select, colour, slider and key bind values are checked like the Mods tab and saved") {
    TempGame g(L"settings_rich_save");
    install_emf(g);
    g.write(L"emf\\mods\\starter\\mod.json", kStarter);
    g.write(L"emf\\mods\\starter\\main.lua", "-- lua");
    CHECK(set_mod_setting(g.dir, "starter", "corner", "Top right").ok);
    CHECK(set_mod_setting(g.dir, "starter", "tint", "#ABC").ok);
    CHECK(set_mod_setting(g.dir, "starter", "speed", "1.3").ok);
    CHECK(set_mod_setting(g.dir, "starter", "toggle", "g").ok);
    const Change bad = set_mod_setting(g.dir, "starter", "corner", "middle");
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("choose one of") == 0);
    const std::string ini = g.read(L"emf\\config\\starter.ini");
    CHECK(ini.find("corner=tr") != std::string::npos);
    CHECK(ini.find("tint=#aabbcc") != std::string::npos);
    CHECK(ini.find("speed=1.5") != std::string::npos);
    CHECK(ini.find("toggle=G") != std::string::npos);
}

namespace {
void agree_log(const char*, const char*, ...) {}
std::wstring agree_exe_dir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/"));
}
}  // namespace

TEST_CASE("app and core agree: every mod the core will not load says so in EMF.exe, with the core's own reason") {
    // one fixture, loaded by EMF's core (real DLLs) and scanned by EMF.exe
    // this run's own folder: earlier runs' folders go (their DLLs are free now), a run going on at the same time keeps its
    const std::wstring root = test_scratch::run_folder(agree_exe_dir() + L"\\test_agree_runs");
    const std::wstring game = root + L"\\game";
    CreateDirectoryW(game.c_str(), nullptr);
    CreateDirectoryW((game + L"\\emf").c_str(), nullptr);
    CreateDirectoryW((game + L"\\emf\\mods").c_str(), nullptr);
    CreateDirectoryW((game + L"\\emf\\config").c_str(), nullptr);
    std::ofstream(game + L"\\enshrouded.exe") << "fake";
    const std::wstring dll = agree_exe_dir() + L"\\test_folder_mod\\emf_test_folder_mod.dll";
    const auto mod = [&](const std::string& id, const std::string& json, bool with_dll = true) {
        const std::wstring folder = game + L"\\emf\\mods\\" + std::wstring(id.begin(), id.end());
        CreateDirectoryW(folder.c_str(), nullptr);
        std::ofstream(folder + L"\\mod.json", std::ios::binary) << json;
        if (with_dll) REQUIRE(CopyFileW(dll.c_str(), (folder + L"\\" + std::wstring(id.begin(), id.end()) + L".dll").c_str(), FALSE));
        else std::ofstream(folder + L"\\main.lua") << "-- lua";
    };
    mod("base", R"({"id":"base","version":"1.4.0"})");
    mod("needs_missing", R"({"id":"needs_missing","version":"1","depends":{"ghost":"*"}})");
    mod("too_old", R"({"id":"too_old","version":"1","depends":{"base":">=2.0"}})");
    mod("too_new", R"({"id":"too_new","version":"1","depends":{"base":"~1.2"}})");
    mod("gap", R"({"id":"gap","version":"1","depends":{"base":"1.0 - 1.2 || ^2.0"}})");
    mod("cycle_a", R"({"id":"cycle_a","version":"1","depends":{"cycle_b":"*"}})");
    mod("cycle_b", R"({"id":"cycle_b","version":"1","depends":{"cycle_a":"*"}})");
    mod("opt_a", R"({"id":"opt_a","version":"1","depends":{"opt_b":{"optional":true}}})");
    mod("opt_b", R"({"id":"opt_b","version":"1","depends":{"opt_a":{"optional":true}}})");
    mod("off_mod", R"({"id":"off_mod","version":"1.0.0"})");
    mod("needs_off", R"({"id":"needs_off","version":"1","depends":{"off_mod":"*"}})");
    mod("needs_needs_off", R"({"id":"needs_needs_off","version":"1","depends":{"needs_off":"*"}})");
    mod("uses_off_opt", R"({"id":"uses_off_opt","version":"1","depends":{"off_mod":{"version":"^9","optional":true}}})");
    mod("uses_on_opt_wrong", R"({"id":"uses_on_opt_wrong","version":"1","depends":[{"id":"base","version":"^2.0","optional":true}]})");
    mod("new_api", R"({"id":"new_api","version":"1","emf":999})");
    mod("scripty", R"({"id":"scripty","version":"1","main":"main.lua"})", false);  // no lua host installed
    std::ofstream(game + L"\\emf\\config\\off_mod.ini") << "[off_mod]\r\nenabled=0\r\n";

    emf::core::FileIniBackend ini(game + L"\\emf\\config");
    emf::core::SettingsStore store(&ini);
    emf::core::ModRegistry registry;
    EmfApi api{};
    api.version = EMF_API_VERSION;
    api.size = sizeof api;
    api.log = agree_log;
    emf::core::LoadOptions opt;
    opt.settings = &store;
    opt.registry = &registry;
    emf::core::load_mods(game + L"\\emf\\mods", &api, [](const std::string&) {}, opt);
    const FolderState s = scan_game_folder(game);

    size_t rejected = 0;
    for (const auto& rec : registry.list()) {
        const ModEntry* e = find(s, "emf", rec.id);
        REQUIRE_MESSAGE(e, rec.id);
        if (rec.status == emf::core::ModStatus::Rejected) {
            ++rejected;
            CHECK_MESSAGE(e->error == "Will not load: " + rec.reason, (rec.id + ": core says " + rec.reason + " / app says " + e->error));
        } else {
            CHECK_MESSAGE(e->error.empty(), (rec.id + " loads, but EMF.exe says " + e->error));
        }
    }
    CHECK(rejected == 11);  // every kind of reason is in the fixture
    const auto status = [&](const char* id) {
        for (const auto& rec : registry.list())
            if (rec.id == id) return rec.status;
        return emf::core::ModStatus::Pending;
    };
    CHECK(status("opt_a") == emf::core::ModStatus::Loaded);
    CHECK(status("uses_off_opt") == emf::core::ModStatus::Loaded);
    CHECK(status("uses_on_opt_wrong") == emf::core::ModStatus::Rejected);
    CHECK(status("needs_needs_off") == emf::core::ModStatus::Rejected);
}
