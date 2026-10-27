// What a Nexus download is, from the paths in its archive, and the notes shown before install.
//
// Fixtures (tests/fixtures/nexus/contents_<mod>_<file>.json) are real modFileContents replies:
// Auto Loot Plus (170: 929 full, 973 lite, 1197 not listed by Nexus yet), Creative Mode (146: 1055, 905),
// ShroudForge (147: 857, 811), EML mods (67: 903 Recipe Tweaker, 412 No Intro), a ReShade preset (5: 8), save files
// (20: 59), Cheat Engine tables (10: 146, 653), Quick Deposit 1.1 (195: 1027, a powrprof.dll mod), Continue 1.1
// (197: 1032, an xinput1_4.dll mod) and the Embervale minimap (143: 979, a Shroudtopia mod; the entries of that file
// from a listing of all the mod's files). Nexus had not listed the newer Greeneye files, so that layout is written
// out below from Quick Deposit's description.
#include <doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "nexus/api.h"
#include "nexus/classify.h"

using namespace emf::nexus;
using Kinds = std::vector<ArchiveKind>;
using Strings = std::vector<std::string>;

namespace {
std::vector<NexusArchiveEntry> listing(const char* name) {
    std::ifstream f(std::string(EMF_SOURCE_DIR) + "/tests/fixtures/nexus/" + name, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), name);
    std::stringstream s;
    s << f.rdbuf();
    auto r = parse_contents(s.str());
    REQUIRE_MESSAGE(r.ok(), r.error);
    return r.value.entries;
}

bool note_has(const Classification& c, const std::string& text) {
    for (const auto& n : c.notes)
        if (n.find(text) != std::string::npos) return true;
    return false;
}

std::string all_notes(const Classification& c) {
    std::string s;
    for (const auto& n : c.notes) s += n + "\n";
    return s;
}

GameState game(std::vector<std::pair<std::string, std::string>> proxies) { return {std::move(proxies), true}; }
}  // namespace

TEST_CASE("classify: kind ids and labels") {
    CHECK(std::string(kind_id(ArchiveKind::EmfMod)) == "emf-mod");
    CHECK(std::string(kind_id(ArchiveKind::DllModDinput8)) == "dll-dinput8");
    CHECK(std::string(kind_label(ArchiveKind::EmlMod)) == "EML mod");
    CHECK(std::string(kind_label(ArchiveKind::CheatTable)) == "Cheat Engine table");
    for (int k = 0; k <= static_cast<int>(ArchiveKind::CheatTable); ++k) {
        CHECK(std::string(kind_id(static_cast<ArchiveKind>(k))).size() > 2);
        CHECK(std::string(kind_label(static_cast<ArchiveKind>(k))).size() > 2);
    }
}

TEST_CASE("classify real: Auto Loot Plus is a dinput8.dll mod that also ships an EML mod (needs EML)") {
    const auto c = classify_archive(listing("contents_170_929.json"));
    CHECK(c.kind == ArchiveKind::DllModDinput8);
    CHECK(c.also == Kinds{ArchiveKind::EmlMod});
    CHECK(c.strip_prefix.empty());
    CHECK(c.proxy_dlls == Strings{"dinput8.dll"});
    REQUIRE(c.mods.size() == 1);
    CHECK(c.mods[0].kind == ArchiveKind::EmlMod);
    CHECK(c.mods[0].id == "auto-loot-plus");
    CHECK(c.mods[0].path == "mods/auto-loot-plus/");
    CHECK_MESSAGE(note_has(c, "dinput8-chain.ini"), all_notes(c));
    CHECK_MESSAGE(note_has(c, "EML or ShroudForge"), all_notes(c));
    CHECK_FALSE(note_has(c, "is installed"));  // the game folder was not looked at

    // with EML's dbghelp.dll and EMF's dinput8.dll next to the game
    const auto g = classify_archive(listing("contents_170_929.json"),
                                    game({{"dinput8.dll", "emf"}, {"dbghelp.dll", "eml"}}));
    CHECK_MESSAGE(note_has(g, "EML is installed"), all_notes(g));
    CHECK_FALSE(note_has(g, "EMF is not installed"));
    const auto none = classify_archive(listing("contents_170_929.json"), game({{"dinput8.dll", "emf"}}));
    CHECK_MESSAGE(note_has(none, "Neither EML nor ShroudForge is installed"), all_notes(none));
}

TEST_CASE("classify real: Auto Loot Plus Lite is only a dinput8.dll mod") {
    const auto c = classify_archive(listing("contents_170_973.json"));
    CHECK(c.kind == ArchiveKind::DllModDinput8);
    CHECK(c.also.empty());
    CHECK(c.mods.empty());
    // EMF missing from the game folder: say so, since this would replace another dinput8.dll
    const auto g = classify_archive(listing("contents_170_973.json"), game({{"dinput8.dll", ""}}));
    CHECK_MESSAGE(note_has(g, "EMF is not installed"), all_notes(g));
}

TEST_CASE("classify real: Creative Mode (dinput8.dll and its mods/creative folder with a DLL) is a dinput8 mod") {
    for (const char* f : {"contents_146_1055.json", "contents_146_905.json"}) {
        const auto c = classify_archive(listing(f));
        CHECK_MESSAGE(c.kind == ArchiveKind::DllModDinput8, f);
        CHECK(c.also.empty());  // mods/creative/creative.dll is its own part, not a Shroudtopia mod
        CHECK(c.mods.empty());
        CHECK(c.proxy_dlls == Strings{"dinput8.dll"});
    }
}

TEST_CASE("classify real: ShroudForge is another loader with its own mods") {
    for (const char* f : {"contents_147_857.json", "contents_147_811.json"}) {
        const auto c = classify_archive(listing(f));
        CHECK_MESSAGE(c.kind == ArchiveKind::ProxyDll, f);
        CHECK(c.loader == "shroudforge");
        CHECK(c.also == Kinds{ArchiveKind::ShroudForgePackage});
        CHECK(c.proxy_dlls == Strings{"dbghelp.dll", "dinput8.dll", "winmm.dll"});
        CHECK(c.mods.size() >= 2);
        for (const auto& m : c.mods) {
            CHECK(m.kind == ArchiveKind::ShroudForgePackage);
            CHECK(m.path == "mods/" + m.id + "/");
        }
        CHECK_MESSAGE(note_has(c, "ShroudForge"), all_notes(c));
        CHECK_MESSAGE(note_has(c, "second copy"), all_notes(c));  // its dinput8.dll is kept aside, not started
        CHECK_MESSAGE(note_has(c, "programs (.exe)"), all_notes(c));
    }
    const auto c = classify_archive(listing("contents_147_857.json"));
    REQUIRE(c.mods.size() == 2);
    CHECK(c.mods[0].id == "sf-survival");
    CHECK(c.mods[1].id == "sf-unlimited-flight");

    // EML owns dbghelp.dll and Shroudweave owns winmm.dll: both would be replaced
    const auto g = classify_archive(listing("contents_147_857.json"),
                                    game({{"dinput8.dll", "emf"}, {"dbghelp.dll", "eml"}, {"winmm.dll", "shroudtopia"}}));
    CHECK_MESSAGE(note_has(g, "dbghelp.dll is EML's now"), all_notes(g));
    CHECK_MESSAGE(note_has(g, "winmm.dll is Shroudtopia's now"), all_notes(g));
    // ShroudForge already there: a newer or older copy of the same loader
    const auto same = classify_archive(listing("contents_147_857.json"), game({{"dbghelp.dll", "shroudforge"}}));
    CHECK_MESSAGE(note_has(same, "ShroudForge is already installed"), all_notes(same));
}

TEST_CASE("classify real: EML mods packed as <id>/mod.json + <id>/src/mod.lua") {
    const auto c = classify_archive(listing("contents_67_903.json"));
    CHECK(c.kind == ArchiveKind::EmlMod);
    CHECK(c.also.empty());
    REQUIRE(c.mods.size() == 1);
    CHECK(c.mods[0].kind == ArchiveKind::EmlMod);
    CHECK(c.mods[0].id == "recipe_tweaker");
    CHECK(c.mods[0].path == "recipe_tweaker/");
    CHECK_MESSAGE(note_has(c, "mods\\recipe_tweaker\\"), all_notes(c));
    CHECK(c.proxy_dlls.empty());

    const auto n = classify_archive(listing("contents_67_412.json"));
    CHECK(n.kind == ArchiveKind::EmlMod);
    REQUIRE(n.mods.size() == 1);
    CHECK(n.mods[0].id == "no_intro");

    const auto g = classify_archive(listing("contents_67_412.json"), game({{"dbghelp.dll", "shroudforge"}}));
    CHECK_MESSAGE(note_has(g, "ShroudForge is installed"), all_notes(g));
}

TEST_CASE("classify real: a ReShade preset, save files and Cheat Engine tables") {
    const auto r = classify_archive(listing("contents_5_8.json"));
    CHECK(r.kind == ArchiveKind::ReshadePreset);
    CHECK(r.strip_prefix == "Giorma's Realistic Reshade 2.0/");
    CHECK_MESSAGE(note_has(r, "ReShade"), all_notes(r));

    const auto s = classify_archive(listing("contents_20_59.json"));
    CHECK(s.kind == ArchiveKind::SaveFiles);
    CHECK_MESSAGE(note_has(s, "not a mod"), all_notes(s));
    CHECK_MESSAGE(note_has(s, "Back up your saves"), all_notes(s));

    const auto t = classify_archive(listing("contents_10_146.json"));
    CHECK(t.kind == ArchiveKind::CheatTable);
    CHECK(t.strip_prefix == "EnshroudedTable/");
    CHECK_MESSAGE(note_has(t, "Cheat Engine"), all_notes(t));

    // a table with shaders, images, other games' tables and "mods on lua/<id>/" folders (not an installable layout)
    const auto m = classify_archive(listing("contents_10_653.json"));
    CHECK(m.kind == ArchiveKind::CheatTable);
    CHECK(m.also.empty());
    CHECK(m.mods.empty());
}

TEST_CASE("classify real: DLL mods under other proxy names (powrprof.dll, xinput1_4.dll)") {
    const auto q = classify_archive(listing("contents_195_1027.json"));
    CHECK(q.kind == ArchiveKind::ProxyDll);
    CHECK(q.proxy_dlls == Strings{"powrprof.dll"});
    CHECK(q.loader.empty());
    CHECK_MESSAGE(note_has(q, "powrprof.dll"), all_notes(q));
    const auto g = classify_archive(listing("contents_195_1027.json"), game({{"powrprof.dll", "aldrich-modmenu"}}));
    CHECK_MESSAGE(note_has(g, "powrprof.dll is Aldrich Mod Menu's now"), all_notes(g));
    const auto u = classify_archive(listing("contents_195_1027.json"), game({{"POWRPROF.dll", ""}}));
    CHECK_MESSAGE(note_has(u, "a tool EMF does not know"), all_notes(u));

    const auto x = classify_archive(listing("contents_197_1032.json"));
    CHECK(x.kind == ArchiveKind::ProxyDll);
    CHECK(x.proxy_dlls == Strings{"xinput1_4.dll"});
}

TEST_CASE("classify real: a Shroudtopia mod (mods/<id>/ with a DLL)") {
    const auto c = classify_archive(listing("contents_143_979.json"));
    CHECK(c.kind == ArchiveKind::ShroudtopiaMod);
    REQUIRE(c.mods.size() == 1);
    CHECK(c.mods[0].kind == ArchiveKind::ShroudtopiaMod);
    CHECK(c.mods[0].id == "minimap_mod");
    CHECK(c.mods[0].path == "mods/minimap_mod/");
    CHECK_MESSAGE(note_has(c, "winmm.dll"), all_notes(c));
    const auto g = classify_archive(listing("contents_143_979.json"), game({{"winmm.dll", "shroudtopia"}}));
    CHECK_MESSAGE(note_has(g, "Shroudtopia is installed"), all_notes(g));
    const auto n = classify_archive(listing("contents_143_979.json"), game({{"winmm.dll", "shroudforge"}}));
    CHECK_MESSAGE(note_has(n, "Shroudtopia is not installed"), all_notes(n));
}

TEST_CASE("classify real: a file Nexus has not listed yet") {
    const auto c = classify_archive(listing("contents_170_1197.json"));
    CHECK(c.kind == ArchiveKind::Unknown);
    CHECK_MESSAGE(note_has(c, "Nexus has no list"), all_notes(c));
}

TEST_CASE("classify: a Greeneye mod with its loader (layout from Quick Deposit's description)") {
    const Strings paths = {"dbgcore.dll", "greeneye/quick-deposit/quick-deposit.dll", "greeneye/quick-deposit/mod.json",
                           "README.txt"};
    const auto c = classify_archive(paths);
    CHECK(c.kind == ArchiveKind::GreeneyeMod);
    CHECK(c.loader == "greeneye");
    CHECK(c.proxy_dlls == Strings{"dbgcore.dll"});
    REQUIRE(c.mods.size() == 1);
    CHECK(c.mods[0].id == "quick-deposit");
    CHECK(c.mods[0].path == "greeneye/quick-deposit/");
    CHECK_MESSAGE(note_has(c, "Greeneye Loader"), all_notes(c));

    const auto same = classify_archive(paths, game({{"dbgcore.dll", "greeneye"}}));
    CHECK_MESSAGE(note_has(same, "Greeneye Loader is already installed"), all_notes(same));
    const auto other = classify_archive(paths, game({{"dbgcore.dll", ""}}));
    CHECK_MESSAGE(note_has(other, "dbgcore.dll is already there"), all_notes(other));

    // a Greeneye mod without the loader, which is not installed
    const auto bare = classify_archive(Strings{"greeneye/find/find.dll", "greeneye/find/mod.json"}, game({}));
    CHECK(bare.kind == ArchiveKind::GreeneyeMod);
    CHECK(bare.loader.empty());
    CHECK_MESSAGE(note_has(bare, "Greeneye Loader (dbgcore.dll) is not installed"), all_notes(bare));
}

TEST_CASE("classify: EMF mods, by folder and by layout") {
    auto c = classify_archive(Strings{"emf/mods/my_mod/mod.json", "emf/mods/my_mod/my_mod.dll", "README.md"});
    CHECK(c.kind == ArchiveKind::EmfMod);
    CHECK(c.strip_prefix.empty());  // "emf" is never a wrapper
    REQUIRE(c.mods.size() == 1);
    CHECK(c.mods[0].kind == ArchiveKind::EmfMod);
    CHECK(c.mods[0].id == "my_mod");
    CHECK(c.mods[0].path == "emf/mods/my_mod/");
    CHECK_MESSAGE(note_has(c, "emf\\mods\\my_mod\\"), all_notes(c));

    c = classify_archive(Strings{"My Mod 1.2/emf/mods/my_mod/mod.json", "My Mod 1.2/emf/mods/my_mod/main.lua"});
    CHECK(c.kind == ArchiveKind::EmfMod);
    CHECK(c.strip_prefix == "My Mod 1.2/");
    REQUIRE(c.mods.size() == 1);
    CHECK(c.mods[0].path == "My Mod 1.2/emf/mods/my_mod/");

    // a loose DLL in emf/mods/ (EMF loads those too; no folder to name)
    c = classify_archive(Strings{"emf/mods/tool.dll", "readme.txt"});
    CHECK(c.kind == ArchiveKind::EmfMod);
    CHECK(c.mods.empty());
    CHECK_MESSAGE(note_has(c, "it goes to emf\\mods\\ and"), all_notes(c));

    // by layout: <id>/mod.json with main.lua (EMF's Lua mods)
    c = classify_archive(Strings{"readme.txt", "kill_counter/mod.json", "kill_counter/main.lua"});
    CHECK(c.kind == ArchiveKind::EmfMod);
    REQUIRE(c.mods.size() == 1);
    CHECK(c.mods[0].id == "kill_counter");
    CHECK(c.mods[0].path == "kill_counter/");
    CHECK_MESSAGE(note_has(c, "looks like an EMF Lua mod"), all_notes(c));
    // mod.json next to a DLL: an EMF, Greeneye or Shroudtopia mod without its parent folder; EMF checks later
    for (const Strings& paths : {Strings{"my_mod/mod.json", "my_mod/my_mod.dll"},
                                 Strings{"readme.txt", "thing/mod.json", "thing/other.dll"}}) {
        c = classify_archive(paths);
        CHECK(c.kind == ArchiveKind::Unknown);
        CHECK(c.mods.empty());
        CHECK_MESSAGE(note_has(c, "could be an EMF, Greeneye or Shroudtopia mod"), all_notes(c));
        CHECK_MESSAGE(note_has(c, "after the download"), all_notes(c));
    }
    // EML's src/mod.lua wins
    c = classify_archive(Strings{"both/mod.json", "both/main.lua", "both/src/mod.lua"});
    CHECK(c.kind == ArchiveKind::EmlMod);
}

TEST_CASE("classify real: EMF's own release zip (scripts/package.ps1) is EMF itself") {
    std::ifstream f(std::string(EMF_SOURCE_DIR) + "/tests/fixtures/nexus/listing_emf_release_0.2.0.txt");
    REQUIRE(f.good());
    Strings paths;
    for (std::string line; std::getline(f, line);)
        if (!line.empty()) paths.push_back(line);
    REQUIRE(paths.size() > 100);
    const auto c = classify_archive(paths, game({{"dinput8.dll", "emf"}}));
    CHECK(c.kind == ArchiveKind::EmfFramework);
    CHECK(c.loader == "emf");
    CHECK(c.strip_prefix == "EMF-0.2.0/");
    CHECK(c.also.empty());
    CHECK(c.mods.empty());
    CHECK_MESSAGE(note_has(c, "EMF itself"), all_notes(c));
}

TEST_CASE("classify: EMF itself") {
    const auto c = classify_archive(Strings{"EMF-0.2.0/dinput8.dll", "EMF-0.2.0/emf/emf_core.dll",
                                            "EMF-0.2.0/emf/mods/clock/mod.json", "EMF-0.2.0/install.cmd"});
    CHECK(c.kind == ArchiveKind::EmfFramework);
    CHECK(c.loader == "emf");
    CHECK_MESSAGE(note_has(c, "EMF itself"), all_notes(c));
}

TEST_CASE("classify: wrapper folders are stripped (two levels at most), never the game's own folders") {
    auto c = classify_archive(Strings{"Cool Mod v2/dinput8.dll", "Cool Mod v2/readme.txt"});
    CHECK(c.kind == ArchiveKind::DllModDinput8);
    CHECK(c.strip_prefix == "Cool Mod v2/");
    c = classify_archive(Strings{"A/B/dinput8.dll", "A/B/x.txt"});
    CHECK(c.strip_prefix == "A/B/");
    CHECK(c.kind == ArchiveKind::DllModDinput8);
    c = classify_archive(Strings{"A/B/C/dinput8.dll"});
    CHECK(c.strip_prefix == "A/B/");
    CHECK(c.kind == ArchiveKind::Unknown);  // three folders deep is not a layout we know
    c = classify_archive(Strings{"readme.txt", "Mod/dinput8.dll"});
    CHECK(c.kind == ArchiveKind::Unknown);  // not wrapped: the DLL is not at the top
    c = classify_archive(Strings{"mods/x/mod.json", "mods/x/src/mod.lua"});
    CHECK(c.strip_prefix.empty());
    CHECK(c.kind == ArchiveKind::EmlMod);
    REQUIRE(c.mods.size() == 1);
    CHECK(c.mods[0].path == "mods/x/");
    c = classify_archive(Strings{"Pack/mods/a/mod.json", "Pack/mods/a/src/mod.lua", "Pack/mods/b/mod.json",
                                 "Pack/mods/b/src/mod.lua"});
    CHECK(c.strip_prefix == "Pack/");
    CHECK(c.kind == ArchiveKind::EmlMod);
    REQUIRE(c.mods.size() == 2);
    CHECK(c.mods[0].path == "Pack/mods/a/");
    CHECK(c.mods[1].id == "b");
    // a mod.json at the top of the archive, without a folder: the installer names the folder
    c = classify_archive(Strings{"mod.json", "src/mod.lua"});
    CHECK(c.kind == ArchiveKind::EmlMod);
    REQUIRE(c.mods.size() == 1);
    CHECK(c.mods[0].id.empty());
    CHECK(c.mods[0].path.empty());
}

TEST_CASE("classify: backslashes, letter case, folder entries and macOS junk") {
    auto c = classify_archive(Strings{"MODS\\Big\\MOD.JSON", "mods\\Big\\SRC\\Mod.Lua"});
    CHECK(c.kind == ArchiveKind::EmlMod);
    REQUIRE(c.mods.size() == 1);
    CHECK(c.mods[0].id == "Big");
    CHECK(c.mods[0].path == "MODS/Big/");
    c = classify_archive(Strings{"__MACOSX/._dinput8.dll", "Wrap/", "Wrap/DInput8.DLL", ".DS_Store", "Wrap/.DS_Store"});
    CHECK(c.kind == ArchiveKind::DllModDinput8);
    CHECK(c.strip_prefix == "Wrap/");
    CHECK(c.proxy_dlls == Strings{"dinput8.dll"});
}

TEST_CASE("classify: unsafe paths are reported and left out") {
    auto c = classify_archive(Strings{"../evil.dll", "C:/Windows/system32/x.dll", "/abs.txt", "ok/../../up.txt",
                                      "./winmm.dll", "CON", "notes.txt:hidden", "dinput8.dll"});
    CHECK(c.kind == ArchiveKind::DllModDinput8);
    CHECK(c.proxy_dlls == Strings{"dinput8.dll"});
    CHECK_MESSAGE(note_has(c, "unsafe"), all_notes(c));
    CHECK_MESSAGE(note_has(c, "../evil.dll"), all_notes(c));
    // Windows drops trailing dots and spaces: "dinput8.dll. " is dinput8.dll
    c = classify_archive(Strings{"dinput8.dll. ", "readme.txt"});
    CHECK(c.kind == ArchiveKind::DllModDinput8);
    CHECK(c.proxy_dlls == Strings{"dinput8.dll"});
}

TEST_CASE("classify: unsafe_archive_path") {
    for (const char* bad : {"../x", "/x", "\\x", "C:/x", "c:x", "a/./b", "./a", "a/../b", "a/..", "file.txt:stream",
                            "CON", "con.txt", "a/NUL/b", "COM1.log", "lpt9", "aux", "Prn.ini", "a/.../b", "a/ /b",
                            "a|b", "a?b", "a*b", "a<b", "a>b", "a\"b", "a\x01" "b", ""})
        CHECK_MESSAGE(unsafe_archive_path(bad), bad);
    CHECK(unsafe_archive_path(std::string("a\0b", 3)));
    for (const char* ok : {"dinput8.dll", "mods/x/mod.json", "mods\\x\\src\\mod.lua", "Giorma's Reshade.ini",
                           "COM10.txt", "CONSOLE.txt", "a/b c/d", "dinput8.dll.", "nul_file.txt", "lpt.txt"})
        CHECK_FALSE_MESSAGE(unsafe_archive_path(ok), ok);
}

TEST_CASE("classify: ShroudForge mods, Shroudtopia files, ReShade with its DLL, other loaders' files") {
    auto c = classify_archive(Strings{"mods/lantern/mod.json", "mods/lantern/src/mod.lua",
                                      "mods/lantern/extended.mod.json"},
                              game({{"dbghelp.dll", "eml"}}));
    CHECK(c.kind == ArchiveKind::ShroudForgePackage);
    REQUIRE(c.mods.size() == 1);
    CHECK(c.mods[0].kind == ArchiveKind::ShroudForgePackage);
    CHECK_MESSAGE(note_has(c, "needs ShroudForge"), all_notes(c));
    CHECK_MESSAGE(note_has(c, "ShroudForge is not installed"), all_notes(c));

    c = classify_archive(Strings{"winmm.dll", "shroudtopia.json", "mods/README.txt"});
    CHECK(c.kind == ArchiveKind::ProxyDll);
    CHECK(c.loader == "shroudtopia");
    c = classify_archive(Strings{"shroudtopia.json"});
    CHECK(c.kind == ArchiveKind::ShroudtopiaMod);
    c = classify_archive(Strings{"mods/fp.dll"});
    CHECK(c.kind == ArchiveKind::ShroudtopiaMod);

    c = classify_archive(Strings{"dxgi.dll", "ReShade.ini", "reshade-shaders/Shaders/x.fx"});
    CHECK(c.kind == ArchiveKind::ReshadePreset);
    CHECK_MESSAGE(note_has(c, "dxgi.dll"), all_notes(c));
    c = classify_archive(Strings{"reshade-shaders/Textures/a.png"});
    CHECK(c.kind == ArchiveKind::ReshadePreset);
    CHECK(c.strip_prefix.empty());

    c = classify_archive(Strings{"dinput8.dll", "chainloader.ini", "mods/readme.txt"});
    CHECK(c.kind == ArchiveKind::ProxyDll);
    CHECK(c.loader == "modloader-proxy");

    c = classify_archive(Strings{"dbghelp.dll", "readme.txt"}, game({{"dbghelp.dll", "shroudforge"}}));
    CHECK(c.kind == ArchiveKind::ProxyDll);
    CHECK_MESSAGE(note_has(c, "dbghelp.dll is ShroudForge's now"), all_notes(c));
}

TEST_CASE("classify: loader releases are told by their own files (known_tools.txt's markers)") {
    // EML's release: the same loader as dbghelp.dll and dinput8.dll, with emm.exe (Auto Loot Plus's description)
    auto c = classify_archive(Strings{"EML/dbghelp.dll", "EML/dinput8.dll", "EML/emm.exe", "EML/kfc-parser.exe"});
    CHECK(c.kind == ArchiveKind::ProxyDll);
    CHECK(c.loader == "eml");
    CHECK_MESSAGE(note_has(c, "This is EML"), all_notes(c));
    CHECK_MESSAGE(note_has(c, "EML starts twice"), all_notes(c));
    CHECK_FALSE(note_has(c, "dinput8-chain.ini"));

    c = classify_archive(Strings{"POWRPROF.dll", "aldrich-modmenu/settings.ini"},
                         game({{"powrprof.dll", "aldrich-modmenu"}}));
    CHECK(c.kind == ArchiveKind::ProxyDll);
    CHECK(c.loader == "aldrich-modmenu");
    CHECK_MESSAGE(note_has(c, "Aldrich Mod Menu is already installed"), all_notes(c));

    c = classify_archive(Strings{"dbgcore.dll", "greeneye/loader.ini"});
    CHECK(c.kind == ArchiveKind::ProxyDll);
    CHECK(c.loader == "greeneye");
}

TEST_CASE("classify: save file names and a plain unknown archive") {
    for (const char* name : {"characters", "characters-1", "slot.ksd"}) {
        const auto c = classify_archive(Strings{std::string("Saves/") + name, "Saves/readme.txt"});
        CHECK_MESSAGE(c.kind == ArchiveKind::SaveFiles, name);
    }
    // a world save: the world file with its _info file, or a backup of it
    CHECK(classify_archive(Strings{"3BD85C7D", "3bd85c7d_info"}).kind == ArchiveKind::SaveFiles);
    CHECK(classify_archive(Strings{"w/3bd85c7d-2", "w/3bd85c7d_info"}).kind == ArchiveKind::SaveFiles);
    // an 8-hex-digit name alone is not a save
    for (const char* name : {"3bd85c7d", "3bd85c7d_info", "deadbeef", "3bd85c7", "3bd85c7dx", "charactersx"}) {
        const auto c = classify_archive(Strings{name, "readme.txt"});
        CHECK_MESSAGE(c.kind == ArchiveKind::Unknown, name);
    }
    const auto c = classify_archive(Strings{"readme.txt", "tool.dll"});
    CHECK(c.kind == ArchiveKind::Unknown);
    CHECK_MESSAGE(note_has(c, "cannot tell"), all_notes(c));
    CHECK(classify_archive(Strings{}).kind == ArchiveKind::Unknown);
}

TEST_CASE("classify: tools EMF starts through dinput8-chain.ini count as installed") {
    const Strings eml = {"no_intro/mod.json", "no_intro/src/mod.lua"};
    GameState g = game({{"dinput8.dll", "emf"}});
    g.chained = {"eml"};  // EML's dinput8.dll variant, renamed to eml.dll by EMF's installer
    auto c = classify_archive(eml, g);
    CHECK_MESSAGE(note_has(c, "EML is installed"), all_notes(c));
    CHECK_FALSE(note_has(c, "Neither"));
    // ShroudForge loads EML mods only after they are switched on in its window
    c = classify_archive(eml, game({{"dbghelp.dll", "shroudforge"}}));
    CHECK_MESSAGE(note_has(c, "once you switch them on in its F9 window (they start off)"), all_notes(c));
}

TEST_CASE("classify: notes when EMF is not installed, and other proxy DLLs next to dinput8.dll") {
    // EMF is not there: no promise that it starts the DLL
    auto c = classify_archive(Strings{"dinput8.dll"}, game({{"dbghelp.dll", "eml"}}));
    CHECK_MESSAGE(note_has(c, "With EMF installed, EMF starts it"), all_notes(c));
    CHECK_FALSE(note_has(c, "EMF will start it"));
    c = classify_archive(Strings{"dinput8.dll", "chainloader.ini"}, game({}));
    CHECK_MESSAGE(note_has(c, "With EMF installed, EMF starts it"), all_notes(c));
    CHECK_FALSE(note_has(c, "EMF will start it"));
    // without a marker, a dbghelp.dll next to dinput8.dll is named too
    c = classify_archive(Strings{"dinput8.dll", "dbghelp.dll"});
    CHECK(c.kind == ArchiveKind::DllModDinput8);
    CHECK(c.proxy_dlls == Strings{"dbghelp.dll", "dinput8.dll"});
    CHECK_MESSAGE(note_has(c, "dbghelp.dll"), all_notes(c));
    // aldrich-modmenu/ is the game folder's, never a wrapper
    c = classify_archive(Strings{"aldrich-modmenu/a.ini", "aldrich-modmenu/b.txt"});
    CHECK(c.strip_prefix.empty());
}
