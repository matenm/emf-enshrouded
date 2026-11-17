// EMF.exe's EML content card (app/content_panel.h): who applies EML content at the next start and why, what the last
// build did, the copy's disk use, and EML's dbghelp.dll set aside. The panel reuses the loader's own rules
// (src/loader/content_*.h, eml_mods.h), so these tests build temp game folders and real shadow copies with the loader's
// functions and check that the panel reports what the loader would do at the next start.
#include <doctest.h>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "app/content_panel.h"
#include "app/game_folder.h"
#include "app_content_files.h"
#include "app_test_files.h"
#include "content_fixtures.h"
#include "core/json.h"
#include "core/known_tools.h"
#include "loader/chain_list.h"
#include "loader/content_base.h"
#include "loader/content_build.h"
#include "loader/content_fingerprint.h"
#include "loader/content_shadow.h"
#include "loader/content_start.h"
#include "loader/content_state.h"
#include "loader/eml_mods.h"
#include "loader/file_hooks.h"

using namespace emf::app;
using app_test::TempGame;
using content_fixtures::kfc_bytes;
namespace loader = emf::loader;
namespace json = emf::core::json;
using namespace app_content_test;

namespace {

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A member of a JSON object that must be there.
const json::Value& at(const json::Value& v, const char* key) {
    const json::Value* x = v.get(key);
    REQUIRE_MESSAGE(x, key);
    return *x;
}

const ContentPanel::LeftOut* left_out(const ContentPanel& p, const std::string& folder) {
    for (const auto& l : p.left_out)
        if (l.folder == folder) return &l;
    return nullptr;
}

loader::FactsIo facts_io(const TempGame& g);

// The loader's own decision for the folder, gathered the way its DllMain does (dllmain.cpp facts_io): the panel's
// `why` must be the same line.
std::string loader_why(const TempGame& g) {
    return loader::decide_content(loader::collect_facts(facts_io(g), loader::PlayOnce::Normal, false)).why;
}

// The path the loader's start takes for the folder (dllmain.cpp plan_this_start, with content_start.h plan_content).
loader::ContentPath loader_path(const TempGame& g, const std::wstring& spelled = L"") {
    const std::wstring dir = spelled.empty() ? g.dir : spelled;
    loader::ContentFacts facts;
    loader::PlanSteps s;
    s.facts = [&] {
        facts = loader::collect_facts(facts_io(g), loader::PlayOnce::Normal, false);
        return facts;
    };
    s.fingerprint = [&] {
        return loader::content_fingerprint(loader::collect_fingerprint_input(
            dir, "enshrouded", facts.base, facts.mods.usable, loader::cio::join(dir, L"emf\\emf_content.dll"), kVersion));
    };
    s.read_state = [&] { return loader::read_state(loader::cio::join(dir, L"emf\\content\\client\\emf_content_state.json")); };
    s.record_ok = [&] {
        const auto normal = loader::normal_game_dir(dir);
        if (!normal || !loader::shadow_problem(loader::file_hooks::shadow_facts(*normal)).empty()) return false;
        return loader::shadow_intact(dir, loader::cio::join(dir, L"emf\\content\\client"), "enshrouded", facts.base.dat_count)
            .empty();
    };
    s.log = [](const std::string&) {};
    return loader::plan_content(s).path;
}

// The panel's status is the path the loader's next start takes.
void check_loader(const TempGame& g, const ContentPanel& p, const std::wstring& spelled = L"") {
    const loader::ContentPath path = loader_path(g, spelled);
    const loader::ContentPath want = p.status == ContentStatus::Ready        ? loader::ContentPath::UseCurrent
                                     : p.status == ContentStatus::BuildsNext ? loader::ContentPath::BuildHeld
                                     : p.status == ContentStatus::Failed     ? loader::ContentPath::KnownFailed
                                                                             : loader::ContentPath::Off;
    CHECK_MESSAGE(path == want, "the panel says " << content_status_name(p.status) << ", the loader takes path "
                                                  << static_cast<int>(path));
}

loader::FactsIo facts_io(const TempGame& g) {
    loader::FactsIo io;
    const std::wstring dir = g.dir;
    io.mode = [dir] {
        return loader::content_mode([&](const wchar_t* section, const wchar_t* key) {
            wchar_t buf[128] = {};
            GetPrivateProfileStringW(section, key, L"", buf, 128, (dir + L"\\emf\\emf_core.ini").c_str());
            return std::wstring(buf);
        });
    };
    io.look.exists = [dir](const std::wstring& rel) {
        const DWORD a = GetFileAttributesW((dir + L"\\" + rel).c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    };
    io.look.tool_name = [dir](const std::wstring& rel) {
        const auto tools = emf::core::parse_known_tools(kTools);
        const std::string word = emf::core::tool_for_dll_text(tools, content_fixtures::file_text(dir + L"\\" + rel));
        for (const auto& t : tools)
            if (t.word == word) return t.name;
        return std::string();
    };
    io.look.chain = loader::parse_chain_list(content_fixtures::file_text(dir + L"\\dinput8-chain.ini"));
    io.dll_present = [dir] { return GetFileAttributesW((dir + L"\\emf\\emf_content.dll").c_str()) != INVALID_FILE_ATTRIBUTES; };
    io.scan_mods = [dir] { return loader::scan_eml_mods(dir + L"\\mods"); };
    io.base = [dir] {
        return loader::choose_base(loader::read_base_file(dir + L"\\enshrouded.kfc"), loader::read_base_file(dir + L"\\enshrouded.kfc.bak"));
    };
    return io;
}

std::wstring upper(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towupper(c));
    return s;
}

}  // namespace

TEST_CASE("app content: the loader's constants the panel copies are the loader's") {
    CHECK(kContentBuildRunning == loader::kBuildRunning);
    CHECK(kContentMaxBuildAttempts == loader::kMaxBuildAttempts);
}

TEST_CASE("app content: without emf_content.dll the card says it is not installed") {
    ContentGame g(L"cp_nodll", false);
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::NotInstalled);
    CHECK_FALSE(p.dll.present);
    CHECK(p.why == loader_why(g));
    CHECK(has(p.detail, "emf_content.dll"));
}

TEST_CASE("app content: [content] mode=off is off, with the loader's reason") {
    ContentGame g(L"cp_off");
    g.write(L"emf\\emf_core.ini", "[backup]\r\non_start=1\r\n[content]\r\nmode=off ; never\r\n");
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::Off);
    CHECK(p.mode == "off");
    CHECK(p.why == loader_why(g));
    CHECK(has(p.detail, "mode=off"));
    CHECK(p.dll.present);
    CHECK(p.dll.size == 20);
    CHECK(p.dll.time > 1600000000);  // Unix seconds
}

TEST_CASE("app content: EML's dbghelp.dll applies it, and EMF offers to take over") {
    ContentGame g(L"cp_emldbg");
    g.write(L"dbghelp.dll", "MZ EML proxy EML_LOG_FILE_PATH");
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::OtherTool);
    CHECK(p.other_tool.rfind("EML", 0) == 0);
    CHECK(has(p.headline, "EML"));
    CHECK(p.why == loader_why(g));
    CHECK(p.take_over == TakeOver::Offer);
}

TEST_CASE("app content: ShroudForge's dbghelp.dll applies it, and EMF never offers to take over") {
    ContentGame g(L"cp_sfdbg");
    g.write(L"dbghelp.dll", "MZ shroudforge runtime EML_LOG_FILE_PATH");
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::OtherTool);
    CHECK(p.other_tool == "ShroudForge");
    CHECK(p.take_over == TakeOver::None);
}

TEST_CASE("app content: EML's dinput8.dll in dinput8-chain.ini applies it, with no take-over") {
    ContentGame g(L"cp_emlchain");
    g.write(L"eml.dll", "MZ EML proxy EML_LOG_FILE_PATH");
    g.write(L"dinput8-chain.ini", "[Chain]\r\neml.dll\r\n");
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::OtherTool);
    CHECK(has(p.other_tool, "dinput8-chain.ini"));
    CHECK(p.why == loader_why(g));
    CHECK(p.take_over == TakeOver::None);
}

TEST_CASE("app content: no EML mod EMF can build is off, and the mods left out carry the loader's reasons") {
    ContentGame g(L"cp_nomods", true, false);
    g.write(L"mods\\.sleeping\\mod.json", mod_json("sleeping", "Sleeping"));
    g.write(L"mods\\npc_explorer\\mod.json", mod_json("npc_explorer", "NPC Explorer", R"(["export"])"));
    g.write(L"mods\\runtime_mod\\mod.json", mod_json("runtime_mod", "Runtime", R"(["patch", "runtime"])"));
    g.write(L"mods\\packed.zip", "PK zip");
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::Off);
    CHECK(p.why == loader_why(g));
    CHECK(has(p.detail, "no EML mod"));
    CHECK(p.mods.empty());
    REQUIRE(p.left_out.size() == 4);
    REQUIRE(left_out(p, ".sleeping"));
    CHECK(has(left_out(p, ".sleeping")->why, "starts with a dot"));
    REQUIRE(left_out(p, "npc_explorer"));
    CHECK(has(left_out(p, "npc_explorer")->why, "only exports"));
    REQUIRE(left_out(p, "runtime_mod"));
    CHECK(has(left_out(p, "runtime_mod")->why, "runtime loader"));
    REQUIRE(left_out(p, "packed.zip"));
    CHECK(has(left_out(p, "packed.zip")->why, ".zip"));
}

TEST_CASE("app content: a mod whose id another folder took is left out, the first one is built") {
    ContentGame g(L"cp_dupe");
    g.write(L"mods\\beasts_copy\\mod.json", mod_json("beasts", "Beasts again"));
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::BuildsNext);
    REQUIRE(p.mods.size() == 1);
    CHECK(p.mods[0].folder == "beasts");
    CHECK(p.mods[0].name == "Beasts of the Shroud");
    CHECK(p.mods[0].version == "1.0.0");
    REQUIRE(left_out(p, "beasts_copy"));
    CHECK(has(left_out(p, "beasts_copy")->why, "taken by the folder beasts"));
}

TEST_CASE("app content: never built yet builds at the next start, and says so") {
    ContentGame g(L"cp_never");
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::BuildsNext);
    CHECK(p.why == loader_why(g));
    CHECK(p.headline == "Builds at the next start");
    CHECK(has(p.detail, "not built"));
    CHECK_FALSE(p.last_build);
    REQUIRE(p.mods.size() == 1);
    CHECK_FALSE(p.mods[0].built);
}

TEST_CASE("app content: a current copy with a good build is ready") {
    ContentGame g(L"cp_ready");
    g.built();
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::Ready);
    CHECK(p.headline == "Ready");
    REQUIRE(p.last_build);
    CHECK(p.last_build->code == 0);
    CHECK(p.last_build->ms == 28000);
    CHECK(p.last_build->mods == std::vector<std::string>{"beasts 1.0.0"});
    REQUIRE(p.mods.size() == 1);
    CHECK(p.mods[0].built);
    CHECK(p.copy_there);
    CHECK(p.copy_bytes > 0);
    CHECK_FALSE(p.patched_in_place);
}

TEST_CASE("app content: the Ready line leaves the build time to the last build's line (local time there)") {
    ContentGame g(L"cp_ready_time");
    g.built();
    const ContentPanel p = content_panel(g.dir, false);
    REQUIRE(p.status == ContentStatus::Ready);
    REQUIRE(p.last_build);
    CHECK(has(p.detail, "1 mod"));
    CHECK_FALSE(has(p.detail, p.last_build->built_at));  // the state's UTC text, e.g. "2026-11-02T15:35:04Z"
}

TEST_CASE("app content: Build again (the record gone, the copy kept) does not say EMF never built it") {
    ContentGame g(L"cp_forgot");
    g.built();
    std::filesystem::remove(g.shadow() + L"\\emf_content_state.json");  // what the card's Build again does
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::BuildsNext);
    CHECK(p.copy_there);
    CHECK_FALSE(has(p.detail, "not built"));
    CHECK(has(p.detail, "no record of its build"));
}

TEST_CASE("app content: a mod file changed since the build: it builds at the next start, naming the mod") {
    ContentGame g(L"cp_touched");
    g.built();
    g.write(L"mods\\beasts\\main.lua", "-- the mod, changed");
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::BuildsNext);
    CHECK(has(p.detail, "Beasts of the Shroud changed"));
    CHECK_FALSE(has(p.detail, "game changed"));
    CHECK_FALSE(has(p.detail, "emf_content.dll changed"));
    REQUIRE(p.mods.size() == 1);
    CHECK(p.mods[0].built);  // in the last build, in its earlier form
}

TEST_CASE("app content: a mod added since the build is named as new") {
    ContentGame g(L"cp_added");
    g.built();
    g.write(L"mods\\wolves\\mod.json", mod_json("wolves", "Wolf Pack"));
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::BuildsNext);
    CHECK(has(p.detail, "Wolf Pack"));
    REQUIRE(p.mods.size() == 2);
}

TEST_CASE("app content: a current state with the copy gone builds again") {
    ContentGame g(L"cp_gone");
    g.built();
    std::filesystem::remove(g.shadow() + L"\\enshrouded.kfc");
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::BuildsNext);
    CHECK(has(p.detail, "copy"));
}

TEST_CASE("app content: the same files failed before: failed, with the message, until something changes") {
    ContentGame g(L"cp_failed");
    g.built(1, "a mod could not be read");
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::Failed);
    CHECK(p.headline == "Failed");
    CHECK(has(p.detail, "a mod could not be read"));
    CHECK(has(p.detail, "EMF tries again when a mod, the game or emf_content.dll changes"));
    REQUIRE(p.last_build);
    CHECK(p.last_build->code == 1);
    REQUIRE(p.last_build->errors.size() == 1);
}

TEST_CASE("app content: a build the game stopped in is tried once more, then it counts as failed") {
    ContentGame g(L"cp_running");
    g.built();
    auto s = *loader::read_state(g.shadow() + L"\\emf_content_state.json");
    s.code = kContentBuildRunning;
    s.message = "a build was running";
    s.attempts = 1;
    REQUIRE(loader::write_state(g.shadow() + L"\\emf_content_state.json", s));
    ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::BuildsNext);
    CHECK(has(p.detail, "did not finish"));
    s.attempts = 2;
    REQUIRE(loader::write_state(g.shadow() + L"\\emf_content_state.json", s));
    p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.status == ContentStatus::Failed);
    CHECK(has(p.detail, "stopped the game 2 times"));
}

TEST_CASE("app content: a corrupt, empty or huge state builds at the next start and never crashes") {
    ContentGame g(L"cp_corrupt");
    g.built();
    const std::wstring state = g.shadow() + L"\\emf_content_state.json";
    for (const std::string& text : {std::string("{not json"), std::string(), std::string(5u << 20, 'x'),
                                    std::string(R"({"fingerprint": 7, "code": "zero"})")}) {
        content_fixtures::put_file(state, text);
        const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
        CHECK(p.status == ContentStatus::BuildsNext);
        CHECK(has(p.detail, "could not be read"));
        CHECK_FALSE(p.last_build);
    }
    // unknown fields are passed over
    content_fixtures::put_file(state, R"({"fingerprint": ")" + g.fingerprint() +
                                          R"(", "code": 0, "mods": ["beasts 1.0.0"], "unloaded": true, "new_field": {"x": [1]}})");
    CHECK(content_panel(g.dir, false).status == ContentStatus::Ready);
}

TEST_CASE("app content: a .kfc.bak of the same game build with another size means the game's files were patched in place") {
    ContentGame g(L"cp_patched");
    g.write(L"enshrouded.kfc", kfc_bytes("7|r|t", 2) + std::string(100, 'p'));  // EML grew the index in place
    g.write(L"enshrouded.kfc.bak", kfc_bytes("7|r|t", 2));
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.patched_in_place);
    CHECK(p.status == ContentStatus::BuildsNext);
}

TEST_CASE("app content: the copy's disk use counts only its own files, never the links to the game's files") {
    ContentGame g(L"cp_bytes");
    g.mkdir(L"emf\\content\\client\\mods\\beasts");
    REQUIRE(CreateHardLinkW((g.shadow() + L"\\enshrouded_000.dat").c_str(), g.at(L"enshrouded_000.dat").c_str(), nullptr));
    content_fixtures::put_file(g.shadow() + L"\\enshrouded.kfc", std::string(1000, 'k'));
    content_fixtures::put_file(g.shadow() + L"\\mods\\beasts\\main.lua", std::string(234, 'l'));
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.copy_there);
    CHECK(p.copy_bytes == 1234);
}

TEST_CASE("app content: EML's dbghelp.dll set aside by EMF is given") {
    ContentGame g(L"cp_given");
    g.write(L"eml-dbghelp.dll", "MZ EML proxy EML_LOG_FILE_PATH");
    g.write(L"emf\\moved.txt", "moved\tcreative.dll\r\nset-aside\teml-dbghelp.dll\tdbghelp.dll\r\n");
    const ContentPanel p = content_panel(g.dir, false);
    check_loader(g, p);
    CHECK(p.take_over == TakeOver::Given);
    CHECK(p.set_aside == "eml-dbghelp.dll");
    CHECK(p.status == ContentStatus::BuildsNext);
}

TEST_CASE("app content: a dedicated server's folder: the server's own files and emf\\content\\server") {
    ContentGame g(L"cp_server", true, true, true);
    ContentPanel p = content_panel(g.dir, true);
    CHECK(p.status == ContentStatus::BuildsNext);
    CHECK(p.why.find("the base is enshrouded_server.kfc") != std::string::npos);
    REQUIRE(p.mods.size() == 1);
    CHECK(p.mods[0].folder == "beasts");
    CHECK_FALSE(p.copy_there);
    g.built();
    p = content_panel(g.dir, true);
    CHECK(p.status == ContentStatus::Ready);
    CHECK(p.copy_there);
    CHECK(p.copy_bytes > 0);
    CHECK(p.detail.find("emf\\content\\server") != std::string::npos);
    CHECK(p.detail.find("emf\\content\\client") == std::string::npos);
    // a change to a server container since the build: the next start builds again
    g.write(L"enshrouded_server_002.dat", "a new container");
    CHECK(content_panel(g.dir, true).status == ContentStatus::BuildsNext);
}

TEST_CASE("app content: the panel as JSON for the page") {
    ContentGame g(L"cp_json");
    g.write(L"mods\\.off\\mod.json", mod_json("off", "<img src=x onerror=alert(1)>"));
    g.built();
    const auto r = json::parse(content_panel_json(content_panel(g.dir, false)));
    REQUIRE(r.value);
    const auto& v = *r.value;
    CHECK(at(v, "status").s == "ready");
    CHECK(at(v, "headline").s == "Ready");
    CHECK(at(v, "mode").s == "auto");
    CHECK(at(at(v, "dll"), "present").b);
    REQUIRE(at(v, "mods").arr.size() == 1);
    CHECK(at(at(v, "mods").arr[0], "name").s == "Beasts of the Shroud");
    CHECK(at(at(v, "mods").arr[0], "built").b);
    REQUIRE(at(v, "left_out").arr.size() == 1);
    CHECK(at(at(v, "left_out").arr[0], "folder").s == ".off");
    CHECK(at(at(v, "last_build"), "ms").n == 28000);
    CHECK(at(v, "copy_bytes").n > 0);
    CHECK_FALSE(at(v, "copy_text").s.empty());
    CHECK(at(v, "take_over").s == "none");
    CHECK_FALSE(at(v, "patched_in_place").b);
}

TEST_CASE("app content: the fingerprint of a 150 MB pack folder with 2000 files costs under 200 ms") {
    ContentGame g(L"cp_speed");
    for (int i = 0; i < 2000; ++i) {
        wchar_t name[64];
        swprintf_s(name, L"mods\\beasts\\textures\\t%04d.png", i);
        g.write(name, "png");
    }
    {  // one 150 MB file, made by setting its end (no data written)
        const HANDLE f = CreateFileW(g.at(L"mods\\beasts\\big.lua").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        REQUIRE(f != INVALID_HANDLE_VALUE);
        LARGE_INTEGER at{};
        at.QuadPart = 150ll << 20;
        SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
        SetEndOfFile(f);
        CloseHandle(f);
    }
    (void)content_panel(g.dir, false);  // the folder's entries in the file system's cache, as on a second look
    const auto t0 = std::chrono::steady_clock::now();
    const ContentPanel p = content_panel(g.dir, false);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(p.status == ContentStatus::BuildsNext);
    CHECK_MESSAGE(ms < 200, "the panel took " << ms << " ms");
}

// ---- the review's fixes ----

TEST_CASE("app content: a build recorded under another spelling of the game folder's path is still current") {
    ContentGame g(L"cp_spelling");
    for (const std::wstring& spelled : {upper(g.dir), g.dir + L"\\"}) {
        g.built(0, "", 3600, spelled);  // the loader got the folder spelled this way
        const ContentPanel p = content_panel(g.dir, false);
        CHECK(p.status == ContentStatus::Ready);
        check_loader(g, p);
        CHECK(loader_path(g, spelled) == loader::ContentPath::UseCurrent);
    }
    CHECK(content_panel(upper(g.dir), false).status == ContentStatus::Ready);  // EMF.exe shown the folder another way
}

TEST_CASE("app content: the take-over offer says which EML mods stop working until EML's dbghelp.dll is back") {
    ContentGame g(L"cp_loses");
    g.write(L"dbghelp.dll", "MZ EML proxy EML_LOG_FILE_PATH");
    g.write(L"mods\\live\\mod.json", mod_json("live", "Live Map", R"(["patch", "runtime"])"));
    g.write(L"mods\\npc_explorer\\mod.json", mod_json("npc_explorer", "NPC Explorer", R"(["export"])"));
    g.write(L"mods\\packed.zip", "PK zip");
    g.write(L"mods\\.sleeping\\mod.json", mod_json("sleeping", "Sleeping"));
    const ContentPanel p = content_panel(g.dir, false);
    REQUIRE(p.take_over == TakeOver::Offer);
    CHECK(p.status == ContentStatus::OtherTool);
    REQUIRE(p.mods.size() == 1);  // what EMF would build
    CHECK(p.mods[0].id == "beasts");
    std::vector<std::string> loses;
    for (const auto& l : p.take_over_loses) loses.push_back(l.folder);
    CHECK(loses == std::vector<std::string>{"live", "npc_explorer", "packed.zip"});  // not .sleeping: EML skips it too
    for (const auto& l : p.take_over_loses)
        if (l.folder == "live") CHECK(has(l.why, "runtime loader"));
    const auto r = json::parse(content_panel_json(p));
    REQUIRE(r.value);
    CHECK(at(*r.value, "take_over_loses").arr.size() == 3);
}

TEST_CASE("app content: no take-over is offered when it could not work: [content] mode=off, or EML's dinput8.dll in the chain") {
    ContentGame g(L"cp_nooffer");
    g.write(L"dbghelp.dll", "MZ EML proxy EML_LOG_FILE_PATH");
    g.write(L"emf\\emf_core.ini", "[content]\r\nmode=off\r\n");
    CHECK(content_panel(g.dir, false).take_over == TakeOver::None);
    g.write(L"emf\\emf_core.ini", "[content]\r\nmode=auto\r\n");
    CHECK(content_panel(g.dir, false).take_over == TakeOver::Offer);
    CHECK(content_take_over_problem(g.dir).empty());
    g.write(L"eml.dll", "MZ EML proxy EML_LOG_FILE_PATH");
    g.write(L"dinput8-chain.ini", "[Chain]\r\neml.dll\r\n");
    CHECK(content_panel(g.dir, false).take_over == TakeOver::None);
    CHECK(has(content_take_over_problem(g.dir), "dinput8-chain.ini"));
}

TEST_CASE("app content: a hand-edited set-aside record that names another file is not EMF's") {
    ContentGame g(L"cp_badrecord");
    g.write(L"creative.dll", "creative");
    g.write(L"x.dll", "x");
    g.write(L"emf\\moved.txt",
            "set-aside\tcreative.dll\tdbghelp.dll\r\nset-aside\t..\\x.dll\tdbghelp.dll\r\nset-aside\tdinput8.dll\tdbghelp.dll\r\n");
    CHECK(content_set_aside_names(g.dir).empty());
    CHECK(content_panel(g.dir, false).take_over == TakeOver::None);
    g.write(L"EML-DBGHELP-2.DLL", "MZ EML proxy EML_LOG_FILE_PATH");
    g.write(L"emf\\moved.txt", "set-aside\tEML-DBGHELP-2.DLL\tdbghelp.dll\r\n");  // the name's case does not matter
    CHECK(content_panel(g.dir, false).take_over == TakeOver::Given);
}

TEST_CASE("app content: an EMF update since the build is named as the cause") {
    ContentGame g(L"cp_emfupdate");
    g.built();
    g.write(L"emf\\emf_core.dll", std::string("MZ emf_core 1.2.4\0 more", 23));
    const ContentPanel p = content_panel(g.dir, false);
    CHECK(p.status == ContentStatus::BuildsNext);
    CHECK(has(p.detail, "EMF was updated"));
}

TEST_CASE("app content: .git and DLL mod folders are no EML mods left out (as the Mods tab sees them)") {
    ContentGame g(L"cp_notmods");
    g.write(L"mods\\.git\\config", "x");
    g.write(L"mods\\Minimap\\Minimap.dll", "MZ");
    g.write(L"mods\\.sleeping\\mod.json", mod_json("sleeping", "Sleeping"));
    const ContentPanel p = content_panel(g.dir, false);
    CHECK_FALSE(left_out(p, ".git"));
    CHECK_FALSE(left_out(p, "Minimap"));
    CHECK(left_out(p, ".sleeping"));
}

TEST_CASE("app content: a long list of build errors is cut, with the count of the rest") {
    ContentGame g(L"cp_errors");
    g.built(1, "many mods failed");
    auto s = *loader::read_state(g.shadow() + L"\\emf_content_state.json");
    s.errors.clear();
    for (int i = 0; i < 100; ++i) s.errors.push_back("mods\\m" + std::to_string(i) + "\\main.lua: bad");
    REQUIRE(loader::write_state(g.shadow() + L"\\emf_content_state.json", s));
    const ContentPanel p = content_panel(g.dir, false);
    REQUIRE(p.last_build);
    CHECK(p.last_build->errors.size() == 20);
    CHECK(p.last_build->errors_more == 80);
    const auto r = json::parse(content_panel_json(p));
    REQUIRE(r.value);
    CHECK(at(at(*r.value, "last_build"), "errors_more").n == 80);
}

TEST_CASE("app content: without emf\\known_tools.txt the tool is named as the loader names it, and no take-over is offered") {
    ContentGame g(L"cp_notools");
    std::filesystem::remove(g.at(L"emf\\known_tools.txt"));
    g.write(L"dbghelp.dll", "MZ EML proxy EML_LOG_FILE_PATH");
    set_fallback_known_tools(kTools);  // EMF.exe's own copy: the loader never reads it
    const ContentPanel p = content_panel(g.dir, false);
    set_fallback_known_tools("");
    CHECK(p.other_tool == "dbghelp.dll (EML or ShroudForge)");
    CHECK(p.take_over == TakeOver::None);
}

// A good build whose mods warned (a creature pack that could not add a creature): the loader keeps the warnings in its
// state; the card lists them under the last build, cut like the errors.
TEST_CASE("app content: a good build's mod warnings reach the card, cut with the count of the rest") {
    ContentGame g(L"cp_warnings");
    g.built(0, "EML content: 3 mods (built in 28 s); a mod reported a problem");
    const auto read = loader::read_state(g.shadow() + L"\\emf_content_state.json");
    REQUIRE(read);
    auto s = *read;
    s.warnings = {"emf_beastmaster_beasts: beastmaster_stable: not added: the source model was not found"};
    REQUIRE(loader::write_state(g.shadow() + L"\\emf_content_state.json", s));
    ContentPanel p = content_panel(g.dir, false);
    REQUIRE(p.last_build);
    REQUIRE(p.last_build->warnings.size() == 1);
    CHECK(p.last_build->warnings[0].find("beastmaster_stable: not added") != std::string::npos);
    CHECK(p.last_build->warnings_more == 0);
    auto r = json::parse(content_panel_json(p));
    REQUIRE(r.value);
    const json::Value* lb = r.value->get("last_build");
    REQUIRE(lb);
    const json::Value* wv = lb->get("warnings");
    REQUIRE(wv);
    CHECK(wv->arr.size() == 1);
    s.warnings.clear();
    for (int i = 0; i < 30; ++i) s.warnings.push_back("m" + std::to_string(i) + ": warned");
    REQUIRE(loader::write_state(g.shadow() + L"\\emf_content_state.json", s));
    p = content_panel(g.dir, false);
    REQUIRE(p.last_build);
    CHECK(p.last_build->warnings.size() == 20);
    CHECK(p.last_build->warnings_more == 10);
    r = json::parse(content_panel_json(p));
    REQUIRE(r.value);
    CHECK(at(at(*r.value, "last_build"), "warnings_more").n == 10);
}
