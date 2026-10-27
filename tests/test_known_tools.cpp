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

TEST_CASE("known tools: Greeneye's other file names are proxy names too") {
    const auto& names = emf::core::proxy_dll_names();
    for (const char* n : {"dinput8.dll", "dbghelp.dll", "dbgcore.dll", "winmm.dll", "umpdc.dll", "cfgmgr32.dll"})
        CHECK(std::find(names.begin(), names.end(), n) != names.end());
}
