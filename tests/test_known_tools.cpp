// emf\known_tools.txt names the loaders and DLL mods EMF recognises next to the game, so the installer, the core's
// start-up report and the manager show the same names.
#include <doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/known_tools.h"
#include "loader/loader_id.h"

using emf::core::KnownTool;
using emf::core::parse_known_tools;

namespace {
const char* kTable =
    "# word | name | how it is recognised\n"
    "shroudforge | ShroudForge | text:shroudforge | beside:shroudforge | same-loader:dbghelp.dll | helper:winmm.dll\n"
    "eml | EML (Enshrouded Mod Loader) | text:EML_LOG_FILE_PATH\n"
    "\n"
    "greeneye | Greeneye Loader | beside:greeneye\n"
    "broken line without bars\n"
    " | no word | text:x\n";
}

TEST_CASE("known tools: each line gives a word, a name and how the tool is recognised; bad lines are skipped") {
    const auto tools = parse_known_tools(kTable);
    REQUIRE(tools.size() == 3);
    CHECK(tools[0].word == "shroudforge");
    CHECK(tools[0].name == "ShroudForge");
    CHECK(tools[0].texts == std::vector<std::string>{"shroudforge"});
    CHECK(tools[0].beside == std::vector<std::string>{"shroudforge"});
    CHECK(tools[0].same_loader == std::vector<std::string>{"dbghelp.dll"});
    CHECK(tools[0].helpers == std::vector<std::string>{"winmm.dll"});
    CHECK(tools[2].texts.empty());
}

TEST_CASE("known tools: chain-as names the file a tool must have when EMF moves it aside (it runs only under its names)") {
    const auto tools = parse_known_tools(
        "ultimate-asi-loader | Ultimate ASI Loader | text:IsUltimateASILoader | chain-as:XInput9_1_0.dll\n"
        "eml | EML (Enshrouded Mod Loader) | text:EML_LOG_FILE_PATH\n");
    REQUIRE(tools.size() == 2);
    CHECK(tools[0].chain_as == "xinput9_1_0.dll");
    CHECK(tools[1].chain_as.empty());
    CHECK(tool_for_dll_text(tools, std::string("MZ ... IsUltimateASILoader\0 ...", 31)) == "ultimate-asi-loader");
}

TEST_CASE("known tools: a DLL is the first tool whose text it contains (case ignored); else unknown") {
    const auto tools = parse_known_tools(kTable);
    CHECK(emf::core::tool_for_dll_text(tools, "...ShroudForge...EML_LOG_FILE_PATH...") == "shroudforge");
    CHECK(emf::core::tool_for_dll_text(tools, "xx eml_log_file_path xx") == "eml");
    CHECK(emf::core::tool_for_dll_text(tools, "nothing known").empty());
}

TEST_CASE("known tools: EMF's own loader is recognised by its marker, not by text that merely mentions it") {
    CHECK(emf::core::tool_for_dll_text({}, std::string("....") + EMF_LOADER_MARKER + "....") == "emf");
    CHECK(emf::core::tool_for_dll_text({}, "reads emf\\emf_loader.log of EMF").empty());
}

TEST_CASE("known tools: the started DLLs in chain_status.txt") {
    CHECK(emf::core::started_chain_names("a.dll\tstarted\r\ngone.dll\tmissing\r\nb.dll\tstarted\r\nbad.dll\tfailed 5\r\n") ==
          std::vector<std::string>{"a.dll", "b.dll"});
    CHECK(emf::core::started_chain_names("").empty());
}

TEST_CASE("installed report: proxy files with their tool, chained DLLs, and a loader whose file another took") {
    const auto tools = parse_known_tools(kTable);
    emf::core::GameFolderView v;
    v.proxies = {{"dinput8.dll", "emf"}, {"dbghelp.dll", "eml"}, {"winmm.dll", ""}};
    v.beside = {"shroudforge"};  // ShroudForge's folder is there, but its dbghelp.dll was replaced by EML's
    v.chain_status = "creative.dll\tstarted\r\ngone.dll\tmissing\r\n";
    const auto lines = emf::core::installed_report(tools, v);
    CHECK(lines == std::vector<std::string>{
                       "next to the game: dinput8.dll = EMF, dbghelp.dll = EML (Enshrouded Mod Loader), winmm.dll = "
                       "unknown tool",
                       "started by EMF (dinput8-chain.ini): creative.dll; listed but not started: gone.dll (missing)",
                       "ShroudForge is installed (shroudforge) but none of its DLLs is active: EML (Enshrouded Mod "
                       "Loader) has dbghelp.dll, which both want"});
}

TEST_CASE("installed report: a tool whose only file left is a helper of its loader (ShroudForge's winmm.dll) is not active") {
    const auto tools = parse_known_tools(kTable);
    emf::core::GameFolderView v;
    v.proxies = {{"dinput8.dll", "emf"}, {"dbghelp.dll", "eml"}, {"winmm.dll", "shroudforge"}};
    v.beside = {"shroudforge"};
    const auto lines = emf::core::installed_report(tools, v);
    REQUIRE(lines.size() == 2);
    CHECK(lines[1] == "ShroudForge is installed (shroudforge) but none of its DLLs is active: EML (Enshrouded Mod Loader) "
                      "has dbghelp.dll, which both want; its winmm.dll alone does not start it");
    // with its own dbghelp.dll it is active, and nothing is said
    v.proxies = {{"dinput8.dll", "emf"}, {"dbghelp.dll", "shroudforge"}, {"winmm.dll", "shroudforge"}};
    CHECK(emf::core::installed_report(tools, v).size() == 1);
    // its loader's name held by nobody known: the general words
    v.proxies = {{"dinput8.dll", "emf"}, {"winmm.dll", "shroudforge"}};
    CHECK(emf::core::installed_report(tools, v).back() ==
          "ShroudForge is installed (shroudforge) but none of its DLLs is active: another tool has its file name; its "
          "winmm.dll alone does not start it");
}

TEST_CASE("known tools: the shipped table marks ShroudForge's winmm.dll as a helper") {
    std::ifstream f(std::string(EMF_SOURCE_DIR) + "/src/core/known_tools.txt", std::ios::binary);
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const auto tools = parse_known_tools(ss.str());
    REQUIRE_FALSE(tools.empty());
    CHECK(tools[0].word == "shroudforge");
    CHECK(tools[0].helpers == std::vector<std::string>{"winmm.dll"});
}

TEST_CASE("installed report: a tool active only through a chained DLL counts as active") {
    const auto tools = parse_known_tools(kTable);
    emf::core::GameFolderView v;
    v.proxies = {{"dinput8.dll", "emf"}};
    v.chained_tools = {"shroudforge"};
    v.beside = {"shroudforge"};
    v.chain_status = "shroudforge.dll\tstarted\r\n";
    CHECK(emf::core::installed_report(tools, v).size() == 2);  // no "none of its DLLs is active" line
}

TEST_CASE("installed report: nothing else installed gives just EMF's line") {
    emf::core::GameFolderView v;
    v.proxies = {{"dinput8.dll", "emf"}};
    CHECK(emf::core::installed_report({}, v) == std::vector<std::string>{"next to the game: dinput8.dll = EMF"});
}

namespace {
const char* kTableWithProxy =
    "shroudforge | ShroudForge | text:shroudforge | beside:shroudforge | same-loader:dbghelp.dll | helper:winmm.dll\n"
    "eml | EML (Enshrouded Mod Loader) | text:EML_LOG_FILE_PATH\n"
    "modloader-proxy | ModLoader Proxy | text:chainloader.ini | beside:chainloader.ini\n";

// A text as a DLL built with wide strings holds it (UTF-16LE: L"chainloader.ini").
std::string utf16(const std::string& s) {
    std::string out;
    for (char ch : s) out += std::string(1, ch) + '\0';
    return out;
}

std::string shipped_table() {
    std::ifstream f(std::string(EMF_SOURCE_DIR) + "/src/core/known_tools.txt", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

TEST_CASE("known tools: a text the DLL holds as UTF-16 counts too (ModLoader Proxy's L\"chainloader.ini\"), case ignored") {
    const auto tools = parse_known_tools(kTableWithProxy);
    CHECK(emf::core::tool_for_dll_text(tools, "MZ\x90" + utf16("chainloader.ini") + "...") == "modloader-proxy");
    CHECK(emf::core::tool_for_dll_text(tools, "MZ\x90" + utf16("ChainLoader.INI")) == "modloader-proxy");
    CHECK(emf::core::tool_for_dll_text(tools, "MZ" + utf16("ShroudForge") + "EML_LOG_FILE_PATH") == "shroudforge");
    CHECK(emf::core::tool_for_dll_text(tools, utf16("chainloader")).empty());  // not the whole text
}

TEST_CASE("known tools: the shipped table names ModLoader Proxy by the name its export table keeps") {
    const auto tools = parse_known_tools(shipped_table());
    // modloader-proxy 1.0.0 and 1.1.0 (x64): the export directory says modloader-proxy.dll, whatever the file is called
    // now; chainloader.ini and modloader-proxy.log are UTF-16 only
    CHECK(emf::core::tool_for_dll_text(tools, std::string("\xa9\x01modloader-proxy.dll\0CallNtPowerInformation", 42)) ==
          "modloader-proxy");
    CHECK(emf::core::tool_for_dll_text(tools, utf16("modloader-proxy.log") + utf16("chainloader.ini")) == "modloader-proxy");
}

TEST_CASE("chainloader.ini: [Load] in order, ForwardTo, the mods folder setting; comments and other keys skipped") {
    const auto l = emf::core::parse_chainloader_ini(
        "\xEF\xBB\xBF; modloader-proxy configuration\r\n"
        "[Settings]\r\n"
        "Log = true\r\n"
        "Deferred = true\r\n"
        "LoadFromFolder = Yes\r\n"
        "ModsFolder = Proxy Mods\r\n"
        "ForwardTo = mods/XHL/dinput8.dll ; the game's calls\r\n"
        "\r\n"
        "[Load]\r\n"
        "; mods\\ExampleMod.dll\r\n"
        "  EMF.dll\r\n"
        "# also a comment\r\n"
        "sub/shroudforge.dll ; ShroudForge\r\n"
        "[Other]\r\n"
        "not-loaded.dll\r\n");
    CHECK(l.load == std::vector<std::string>{"EMF.dll", "sub\\shroudforge.dll"});
    CHECK(l.forward_to == "mods\\XHL\\dinput8.dll");
    CHECK(l.load_from_folder);
    CHECK(l.mods_folder == "Proxy Mods");
    const auto d = emf::core::parse_chainloader_ini("[load]\nA.dll\n");  // section names in any case; defaults
    CHECK(d.load == std::vector<std::string>{"A.dll"});
    CHECK_FALSE(d.load_from_folder);
    CHECK(d.mods_folder == "mods");
    CHECK(d.forward_to.empty());
}

TEST_CASE("chainloader.ini: the DLLs ModLoader Proxy starts, in its order, never itself or one twice") {
    std::vector<std::string> asked;
    const auto in = [&](const std::string& folder) {
        asked.push_back(folder);
        return std::vector<std::string>{"c.dll", "b.dll", "A.dll"};  // ModLoader Proxy sorts them, case ignored
    };
    const std::string ini =
        "[Settings]\nForwardTo = fwd.dll\nLoadFromFolder = true\nModsFolder = mods/\n"
        "[Load]\nEMF.dll\nfwd.dll\nDINPUT8.dll\nmods\\b.dll\nsf.dll\nemf.dll\n";
    CHECK(emf::core::chainloader_dlls(ini, "dinput8.dll", in) ==
          std::vector<std::string>{"fwd.dll", "EMF.dll", "mods\\b.dll", "sf.dll", "mods\\A.dll", "mods\\c.dll"});
    CHECK(asked == std::vector<std::string>{"mods"});
    // the folder must be a subfolder; without LoadFromFolder it is not looked at
    asked.clear();
    CHECK(emf::core::chainloader_dlls("[Settings]\nLoadFromFolder=1\nModsFolder=.\n[Load]\nx.dll\n", "", in) ==
          std::vector<std::string>{"x.dll"});
    CHECK(emf::core::chainloader_dlls("[Load]\nx.dll\n", "", in) == std::vector<std::string>{"x.dll"});
    CHECK(asked.empty());
}

TEST_CASE("ModLoader Proxy runs when one of its DLLs is a proxy next to the game, or when EMF chain-loads it") {
    emf::core::GameFolderView v;
    std::string self;
    v.proxies = {{"dinput8.dll", "modloader-proxy"}, {"winmm.dll", "shroudforge"}};
    CHECK(emf::core::modloader_proxy_runs(v, &self));
    CHECK(self == "dinput8.dll");
    v.proxies = {{"dinput8.dll", "emf"}};
    CHECK_FALSE(emf::core::modloader_proxy_runs(v, &self));
    v.chain_status = "creative.dll\tstarted\r\nmodloader-proxy.dll\tstarted\r\n";
    v.chained_tools = {"", "modloader-proxy"};
    CHECK(emf::core::modloader_proxy_runs(v, &self));
    CHECK(self == "modloader-proxy.dll");
}

TEST_CASE("ModLoader Proxy's DLLs: read from chainloader.ini only when it runs; their tools; nothing outside the game") {
    std::vector<std::string> read;
    emf::core::FolderLook look;
    look.read_text = [&](const std::string& p) {
        read.push_back(p);
        return std::string(
            "[Settings]\nLoadFromFolder = true\n[Load]\nEMF.dll\ngone.dll\nD:\\elsewhere\\x.dll\n..\\up.dll\n");
    };
    look.exists = [](const std::string& p) { return p != "gone.dll"; };
    look.tool = [](const std::string& p) { return p == "EMF.dll" ? std::string("emf") : std::string("shroudforge"); };
    look.dlls_in = [](const std::string&) { return std::vector<std::string>{"sf.dll"}; };
    emf::core::GameFolderView v;
    v.proxies = {{"dinput8.dll", "emf"}};
    emf::core::add_loader_started(v, look);
    CHECK(v.loader_started.empty());
    CHECK(read.empty());  // ModLoader Proxy does not run: its file is not even read
    v.proxies = {{"dinput8.dll", "modloader-proxy"}};
    emf::core::add_loader_started(v, look);
    CHECK(read == std::vector<std::string>{"chainloader.ini"});
    REQUIRE(v.loader_started.size() == 5);
    CHECK((v.loader_started[0].name == "EMF.dll" && v.loader_started[0].word == "emf" && v.loader_started[0].found));
    CHECK((v.loader_started[1].name == "gone.dll" && !v.loader_started[1].found));
    CHECK((v.loader_started[2].name == "D:\\elsewhere\\x.dll" && v.loader_started[2].outside));
    CHECK((v.loader_started[3].name == "..\\up.dll" && v.loader_started[3].outside));
    CHECK((v.loader_started[4].name == "mods\\sf.dll" && v.loader_started[4].word == "shroudforge"));
    CHECK_FALSE(v.loader_started[0].outside);
}

TEST_CASE("ModLoader Proxy's DLLs: a device name (CON, COM1, NUL) is never opened and counts as not found") {
    std::vector<std::string> looked;
    emf::core::FolderLook look;
    look.read_text = [](const std::string&) {
        return std::string("[Load]\nCON\ncom1.dll\nsub\\NUL.dll\nLPT9.txt.dll\nreal.dll\n");
    };
    look.exists = [&](const std::string& p) {
        looked.push_back(p);
        return true;
    };
    look.tool = [&](const std::string& p) {
        looked.push_back(p);
        return std::string("emf");
    };
    look.dlls_in = [](const std::string&) { return std::vector<std::string>{}; };
    emf::core::GameFolderView v;
    v.proxies = {{"dinput8.dll", "modloader-proxy"}};
    emf::core::add_loader_started(v, look);
    CHECK(looked == (std::vector<std::string>{"real.dll", "real.dll"}));
    REQUIRE(v.loader_started.size() == 5);
    for (size_t i = 0; i < 4; ++i) CHECK_FALSE(v.loader_started[i].found);
    CHECK(v.loader_started[4].found);
}

TEST_CASE("ModLoader Proxy's DLLs: started by EMF from a subfolder, it reads the chainloader.ini next to it") {
    std::vector<std::string> read;
    emf::core::FolderLook look;
    look.read_text = [&](const std::string& p) {
        read.push_back(p);
        return std::string("[Load]\nsf.dll\nMODLOADER-PROXY.dll\n");  // itself, by its file name: skipped
    };
    look.exists = [](const std::string&) { return true; };
    look.tool = [](const std::string&) { return std::string("shroudforge"); };
    look.dlls_in = [](const std::string&) { return std::vector<std::string>{}; };
    emf::core::GameFolderView v;
    v.proxies = {{"dinput8.dll", "emf"}};
    v.chain_status = "mods\\mlp\\modloader-proxy.dll\tstarted\r\n";
    v.chained_tools = {"modloader-proxy"};
    emf::core::add_loader_started(v, look);
    CHECK(read == std::vector<std::string>{"mods\\mlp\\chainloader.ini"});
    REQUIRE(v.loader_started.size() == 1);
    CHECK(v.loader_started[0].name == "mods\\mlp\\sf.dll");
    CHECK(v.loader_started[0].word == "shroudforge");
}

TEST_CASE("ModLoader Proxy's DLLs: a ModsFolder outside the game is listed, not dropped; a path starting with \\ is inside") {
    std::vector<std::string> folders;
    emf::core::FolderLook look;
    look.read_text = [](const std::string&) {
        return std::string("[Settings]\nLoadFromFolder = true\nModsFolder = D:\\mods\n[Load]\n\\top.dll\n");
    };
    look.exists = [](const std::string&) { return true; };
    look.tool = [](const std::string& p) { return p == "top.dll" ? std::string("emf") : std::string(); };
    look.dlls_in = [&](const std::string& f) {
        folders.push_back(f);
        return std::vector<std::string>{"x.dll"};
    };
    emf::core::GameFolderView v;
    v.proxies = {{"dinput8.dll", "modloader-proxy"}};
    emf::core::add_loader_started(v, look);
    CHECK(folders.empty());  // never looked into
    REQUIRE(v.loader_started.size() == 2);
    CHECK((v.loader_started[0].name == "top.dll" && v.loader_started[0].word == "emf" && !v.loader_started[0].outside));
    CHECK((v.loader_started[1].name == "D:\\mods\\*.dll" && v.loader_started[1].outside));
    const auto lines = emf::core::installed_report({}, v);
    CHECK(lines.back() == "started by modloader-proxy (chainloader.ini): top.dll = EMF, D:\\mods\\*.dll (outside the game "
                          "folder, not read)");
}

TEST_CASE("ModLoader Proxy runs when EMF's dinput8-chain.ini lists it (EMF.exe, before a game start wrote the status)") {
    emf::core::GameFolderView v;
    std::string self;
    v.proxies = {{"dinput8.dll", "emf"}};
    v.chain_listed = {{"creative.dll", ""}, {"modloader-proxy.dll", "modloader-proxy"}};
    CHECK(emf::core::modloader_proxy_runs(v, &self));
    CHECK(self == "modloader-proxy.dll");
}

TEST_CASE("installed report: ModLoader Proxy as dinput8.dll starts EMF and ShroudForge, so neither is reported inactive") {
    // a player's setup: ModLoader Proxy chains EMF next to ShroudForge (on Linux)
    const auto tools = parse_known_tools(kTableWithProxy);
    emf::core::GameFolderView v;
    v.proxies = {{"dinput8.dll", "modloader-proxy"}, {"dbghelp.dll", ""}, {"winmm.dll", "shroudforge"}, {"dxgi.dll", ""}};
    v.beside = {"shroudforge", "chainloader.ini"};
    v.loader_started = {{"EMF.dll", "emf", true}, {"shroudforge.dll", "shroudforge", true}, {"gone.dll", "", false}};
    const auto lines = emf::core::installed_report(tools, v);
    CHECK(lines == std::vector<std::string>{
                       "next to the game: dinput8.dll = ModLoader Proxy, dbghelp.dll = unknown tool, winmm.dll = "
                       "ShroudForge, dxgi.dll = unknown tool",
                       "started by ModLoader Proxy (chainloader.ini): EMF.dll = EMF, shroudforge.dll = ShroudForge; "
                       "listed but not found: gone.dll"});
}

TEST_CASE("known tools: the loader names EMF.exe installs are proxy names in the game folder view too") {
    const auto& names = emf::core::proxy_dll_names();
    for (const char* n : {"d3d12.dll", "opengl32.dll", "dwmapi.dll", "uxtheme.dll", "wtsapi32.dll", "iphlpapi.dll", "hid.dll"})
        CHECK_MESSAGE(std::find(names.begin(), names.end(), n) != names.end(), n);
}

TEST_CASE("known tools: Greeneye's other file names are proxy names too") {
    const auto& names = emf::core::proxy_dll_names();
    for (const char* n : {"dinput8.dll", "dbghelp.dll", "dbgcore.dll", "winmm.dll", "umpdc.dll", "cfgmgr32.dll"})
        CHECK(std::find(names.begin(), names.end(), n) != names.end());
}
