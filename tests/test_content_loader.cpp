// The content loader (docs/engine_parts/content_loader.md): the parts in src/loader/ that decide whether EMF builds the
// EML mods' content into a shadow copy of the game's data, build it, and remember what was built.
#include <doctest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "content_fixtures.h"
#include "loader/content_base.h"
#include "loader/content_owner.h"
#include "loader/eml_mods.h"
#include "loader/kfc_header.h"
#include "test_scratch.h"

using content_fixtures::file_text;
using content_fixtures::kfc_bytes;
using content_fixtures::put_file;

TEST_CASE("kfc header: synthetic header") {
    const auto h = emf::loader::parse_kfc_header(kfc_bytes("1|x|y", 3));
    REQUIRE(h.has_value());
    CHECK(h->version == "1|x|y");
    CHECK(h->containers == 3);
}

TEST_CASE("kfc header: wrong magic") {
    std::string b = kfc_bytes("1|x|y", 3);
    b[3] = '5';  // "KFC5"
    CHECK_FALSE(emf::loader::parse_kfc_header(b).has_value());
}

TEST_CASE("kfc header: version past the end") {
    std::string b = kfc_bytes("1|x|y", 3);
    content_fixtures::put_u32(b, 20, 100);  // a 100-byte version in 64 bytes
    CHECK_FALSE(emf::loader::parse_kfc_header(b).has_value());
    b = kfc_bytes("1|x|y", 3);
    content_fixtures::put_u32(b, 16, 0xFFFFFFF0u);  // an offset that wraps around
    CHECK_FALSE(emf::loader::parse_kfc_header(b).has_value());
}

TEST_CASE("kfc header: truncated") {
    CHECK_FALSE(emf::loader::parse_kfc_header(kfc_bytes("1|x|y", 3).substr(0, 20)).has_value());
    CHECK_FALSE(emf::loader::parse_kfc_header("").has_value());
}
// ---- the base the shadow is built from, and the EML mods in <game>\mods ----

namespace {
emf::loader::BaseFile base_file(const std::string& version, uint32_t containers, uint64_t size) {
    emf::loader::BaseFile f;
    f.header = emf::loader::parse_kfc_header(kfc_bytes(version, containers));
    f.size = size;
    f.exists = true;
    return f;
}
}  // namespace

TEST_CASE("content base: no bak") {
    const auto c = emf::loader::choose_base(base_file("7|r|t", 55, 9000), {});
    CHECK(c.ok);
    CHECK_FALSE(c.use_bak);
    CHECK(c.dat_count == 55);
}

TEST_CASE("content base: stale bak (other version)") {
    // an update since EML's patch: the .bak is of an older build
    const auto c = emf::loader::choose_base(base_file("8|r|t", 56, 9100), base_file("7|r|t", 64, 12000));
    CHECK(c.ok);
    CHECK_FALSE(c.use_bak);
    CHECK(c.dat_count == 56);
}

TEST_CASE("content base: identical bak") {
    const auto c = emf::loader::choose_base(base_file("7|r|t", 55, 9000), base_file("7|r|t", 55, 9000));
    CHECK(c.ok);
    CHECK_FALSE(c.use_bak);
    CHECK(c.dat_count == 55);
}

TEST_CASE("content base: patched kfc") {
    // EML or ShroudForge patched the game's files in place: never patch on top of that
    auto c = emf::loader::choose_base(base_file("7|r|t", 64, 12000), base_file("7|r|t", 55, 9000));
    CHECK(c.ok);
    CHECK(c.use_bak);
    CHECK(c.dat_count == 55);
    c = emf::loader::choose_base(base_file("7|r|t", 55, 9500), base_file("7|r|t", 55, 9000));  // same count, other size
    CHECK(c.ok);
    CHECK(c.use_bak);
    CHECK(c.dat_count == 55);
    CHECK(c.why.find("enshrouded.kfc.bak") != std::string::npos);
}

TEST_CASE("content base: unreadable header") {
    emf::loader::BaseFile broken;
    broken.exists = true;
    broken.size = 10;
    auto c = emf::loader::choose_base(broken, {});
    CHECK_FALSE(c.ok);
    CHECK(c.why.find("enshrouded.kfc") != std::string::npos);
    c = emf::loader::choose_base(base_file("7|r|t", 55, 9000), broken);
    CHECK_FALSE(c.ok);
    CHECK(c.why.find("enshrouded.kfc.bak") != std::string::npos);
    c = emf::loader::choose_base({}, {});  // no .kfc at all
    CHECK_FALSE(c.ok);
    CHECK(c.why.find("enshrouded.kfc") != std::string::npos);
    c = emf::loader::choose_base(broken, {}, "enshrouded_server");
    CHECK(c.why.find("enshrouded_server.kfc") != std::string::npos);
}

TEST_CASE("content base: read from the files") {
    const std::wstring dir = test_scratch::temp_path(L"emf_content_base");
    test_scratch::remove_at_exit(dir);
    std::filesystem::create_directories(dir);
    put_file(dir + L"\\enshrouded.kfc", kfc_bytes("7|r|t", 3) + std::string(100, 'x'));
    const auto f = emf::loader::read_base_file(dir + L"\\enshrouded.kfc");
    CHECK(f.exists);
    CHECK(f.size == 164);
    REQUIRE(f.header.has_value());
    CHECK(f.header->containers == 3);
    const auto none = emf::loader::read_base_file(dir + L"\\enshrouded.kfc.bak");
    CHECK_FALSE(none.exists);
    CHECK_FALSE(none.header.has_value());
    std::filesystem::remove_all(dir);
}

namespace {
const char* kGoodMod = R"({"id": "good", "name": "Good", "version": "1.0.0", "capabilities": ["patch", "export"]})";

std::string reason_for(const emf::loader::ModScan& s, const std::wstring& folder) {
    for (const auto& [f, why] : s.skipped)
        if (f == folder) return why;
    return "(not skipped)";
}
}  // namespace

TEST_CASE("eml mods: usable, hidden, runtime, broken json, no id") {
    const std::wstring mods = test_scratch::temp_path(L"emf_eml_mods");
    test_scratch::remove_at_exit(mods);
    put_file(mods + L"\\good\\mod.json", kGoodMod);
    put_file(mods + L"\\good\\patch.lua", "-- patch\n");
    put_file(mods + L"\\.hidden\\mod.json", kGoodMod);
    put_file(mods + L"\\runtime\\mod.json",
             R"({"id": "rt", "name": "Rt", "version": "1.0.0", "capabilities": ["patch", "runtime"]})");
    put_file(mods + L"\\broken\\mod.json", "{ not json");
    put_file(mods + L"\\noid\\mod.json", R"({"name": "No id", "version": "1.0.0"})");
    put_file(mods + L"\\readme.txt", "not a mod\n");  // a file: passed over without a word

    const auto s = emf::loader::scan_eml_mods(mods);
    REQUIRE(s.usable.size() == 1);
    CHECK(s.usable[0].folder == L"good");
    CHECK(s.usable[0].id == "good");
    CHECK(s.usable[0].version == "1.0.0");
    CHECK(s.usable[0].capabilities == std::vector<std::string>{"patch", "export"});
    CHECK(s.skipped.size() == 4);
    CHECK(reason_for(s, L".hidden").find("starts with a dot") != std::string::npos);
    CHECK(reason_for(s, L"runtime") == "needs EML's runtime loader, which EMF does not run");
    CHECK(reason_for(s, L"broken").find("mod.json") != std::string::npos);
    CHECK(reason_for(s, L"noid").find("id") != std::string::npos);
    std::filesystem::remove_all(mods);
}

TEST_CASE("eml mods: no mod.json, no name, a zip, the same id twice, no folder") {
    const std::wstring mods = test_scratch::temp_path(L"emf_eml_mods_more");
    test_scratch::remove_at_exit(mods);
    CHECK(emf::loader::scan_eml_mods(mods).usable.empty());  // no mods folder at all
    put_file(mods + L"\\a_first\\mod.json", kGoodMod);
    put_file(mods + L"\\b_again\\mod.json", kGoodMod);
    put_file(mods + L"\\empty\\notes.txt", "x");
    put_file(mods + L"\\noname\\mod.json", R"({"id": "nn", "version": "1.0.0"})");
    put_file(mods + L"\\caps\\mod.json", R"({"id": "c", "name": "C", "version": "1.0.0", "capabilities": "patch"})");
    put_file(mods + L"\\packed.zip", "PK");
    const auto s = emf::loader::scan_eml_mods(mods);
    REQUIRE(s.usable.size() == 1);
    CHECK(s.usable[0].folder == L"a_first");
    CHECK(reason_for(s, L"b_again").find("a_first") != std::string::npos);
    CHECK(reason_for(s, L"empty").find("no mod.json") != std::string::npos);
    CHECK(reason_for(s, L"noname").find("name") != std::string::npos);
    CHECK(reason_for(s, L"caps").find("capabilities") != std::string::npos);
    CHECK(reason_for(s, L"packed.zip").find(".zip") != std::string::npos);
    std::filesystem::remove_all(mods);
}

TEST_CASE("eml mods: a mod that only exports (no \"patch\") changes nothing in the game: left out") {
    const std::wstring mods = test_scratch::temp_path(L"emf_eml_mods_export");
    test_scratch::remove_at_exit(mods);
    put_file(mods + L"\\explorer\\mod.json",
             R"({"id": "npc_explorer", "name": "NPC Explorer", "version": "0.1.0", "capabilities": ["export"]})");
    put_file(mods + L"\\none\\mod.json", R"({"id": "none", "name": "None", "version": "1.0.0"})");
    put_file(mods + L"\\both\\mod.json",
             R"({"id": "both", "name": "Both", "version": "1.0.0", "capabilities": ["export", "patch"]})");
    const auto s = emf::loader::scan_eml_mods(mods);
    REQUIRE(s.usable.size() == 1);
    CHECK(s.usable[0].id == "both");
    CHECK(reason_for(s, L"explorer") == "only exports game data (no \"patch\"): nothing for EMF to build");
    CHECK(reason_for(s, L"none") == "only exports game data (no \"patch\"): nothing for EMF to build");
    std::filesystem::remove_all(mods);
}

TEST_CASE("eml mods: non-ASCII folder name") {
    const std::wstring mods = test_scratch::temp_path(L"emf_eml_mods_utf");
    test_scratch::remove_at_exit(mods);
    const std::wstring folder = L"Bär mod ✓";
    put_file(mods + L"\\" + folder + L"\\mod.json",
             "{\"id\": \"baer\", \"name\": \"B\xC3\xA4r\", \"version\": \"2.1.0\", \"capabilities\": [\"patch\"]}");
    const auto s = emf::loader::scan_eml_mods(mods);
    REQUIRE(s.usable.size() == 1);
    CHECK(s.usable[0].folder == folder);
    CHECK(s.usable[0].id == "baer");
    std::filesystem::remove_all(mods);
}

// ---- who applies the EML content at this start ----

namespace {
using emf::loader::ContentFacts;
using emf::loader::ContentMode;
using emf::loader::PlayOnce;

// Everything there: Auto, a normal start, the DLL, one usable mod, a good base, the game client.
ContentFacts all_there() {
    ContentFacts f;
    f.mode = ContentMode::Auto;
    f.play = PlayOnce::Normal;
    f.dll_present = true;
    emf::loader::EmlMod m;
    m.folder = L"beasts";
    m.id = "beasts";
    m.version = "1.0.0";
    f.mods.usable.push_back(m);
    f.base.ok = true;
    f.base.dat_count = 55;
    f.base.why = "the base is enshrouded.kfc (55 containers)";
    return f;
}
}  // namespace

TEST_CASE("content owner: everything there: EMF builds it, with mode auto or on") {
    auto f = all_there();
    auto d = emf::loader::decide_content(f);
    CHECK(d.emf_owns);
    CHECK(d.why.find("1 mod") != std::string::npos);
    f.mode = ContentMode::On;
    CHECK(emf::loader::decide_content(f).emf_owns);
}

TEST_CASE("content owner: mode off wins over everything") {
    auto f = all_there();
    f.mode = ContentMode::Off;
    f.play = PlayOnce::NoMods;
    f.other_tool = "EML (Enshrouded Mod Loader)";
    const auto d = emf::loader::decide_content(f);
    CHECK_FALSE(d.emf_owns);
    CHECK(d.why.find("mode=off") != std::string::npos);
}

TEST_CASE("content owner: Play without any mods turns it off") {
    auto f = all_there();
    f.play = PlayOnce::NoMods;
    f.other_tool = "EML (Enshrouded Mod Loader)";
    const auto d = emf::loader::decide_content(f);
    CHECK_FALSE(d.emf_owns);
    CHECK(d.why.find("Play without any mods") != std::string::npos);
}

TEST_CASE("content owner: Play without EMF mods keeps content") {
    auto f = all_there();
    f.play = PlayOnce::NoEmf;
    CHECK(emf::loader::decide_content(f).emf_owns);
}

TEST_CASE("content owner: EML's dbghelp wins over everything but Off") {
    auto f = all_there();
    f.other_tool = "EML (Enshrouded Mod Loader)";
    f.dll_present = false;
    f.mods.usable.clear();
    f.base.ok = false;
    f.server = true;
    auto d = emf::loader::decide_content(f);
    CHECK_FALSE(d.emf_owns);
    CHECK(d.why.find("applied by EML (Enshrouded Mod Loader)") == 0);
    f.mode = ContentMode::On;  // On never overrides another tool
    CHECK(emf::loader::decide_content(f).why.find("applied by EML") == 0);
    f.mode = ContentMode::Off;
    CHECK(emf::loader::decide_content(f).why.find("mode=off") != std::string::npos);
}

TEST_CASE("content owner: the dedicated server takes EML content from EMF as the game does") {
    auto f = all_there();
    f.server = true;
    auto d = emf::loader::decide_content(f);
    CHECK(d.emf_owns);
    CHECK(d.why.find("EMF builds it from 1 mod") == 0);
    f.mods.usable.clear();
    d = emf::loader::decide_content(f);
    CHECK_FALSE(d.emf_owns);
    CHECK(d.why.find("no EML mod") != std::string::npos);
}

TEST_CASE("content owner: no emf_content.dll, then no usable mod, then the base") {
    auto f = all_there();
    f.dll_present = false;
    f.mods.usable.clear();
    f.base.ok = false;
    auto d = emf::loader::decide_content(f);
    CHECK_FALSE(d.emf_owns);
    CHECK(d.why.find("emf_content.dll is not installed") != std::string::npos);
    f.dll_present = true;
    d = emf::loader::decide_content(f);
    CHECK_FALSE(d.emf_owns);
    CHECK(d.why.find("no EML mod") != std::string::npos);
    f.mods = all_there().mods;
    f.base.why = "enshrouded.kfc's header could not be read";
    d = emf::loader::decide_content(f);
    CHECK_FALSE(d.emf_owns);
    CHECK(d.why.find("enshrouded.kfc's header could not be read") != std::string::npos);
}

TEST_CASE("content owner: the mode from emf_core.ini") {
    const auto mode = [](const wchar_t* text) {
        return emf::loader::content_mode([&](const wchar_t* section, const wchar_t* key) {
            CHECK(std::wstring(section) == L"content");
            CHECK(std::wstring(key) == L"mode");
            return std::wstring(text);
        });
    };
    CHECK(mode(L"") == ContentMode::Auto);
    CHECK(mode(L"auto") == ContentMode::Auto);
    CHECK(mode(L"on") == ContentMode::On);
    CHECK(mode(L" OFF ") == ContentMode::Off);
    CHECK(mode(L"off ; no EML content from EMF") == ContentMode::Off);
    CHECK(mode(L"maybe") == ContentMode::Auto);
}

TEST_CASE("content owner: another tool: a dbghelp.dll in the game folder, or EML's dinput8 in the chain") {
    emf::loader::ToolLook look;
    std::vector<std::wstring> read;  // the DLLs whose text was read
    std::vector<std::wstring> present;
    look.exists = [&](const std::wstring& name) {
        for (const auto& p : present)
            if (p == name) return true;
        return false;
    };
    look.tool_name = [&](const std::wstring& name) -> std::string {
        read.push_back(name);
        if (name == L"dbghelp.dll") return "ShroudForge";
        if (name == L"modloader.dll") return "EML (Enshrouded Mod Loader)";
        return "";
    };
    CHECK_FALSE(emf::loader::other_content_tool(look, true, true).has_value());

    // Kiln (its own EML layer and ShroudForge) loads the game's mods, EMF too: it applies the content, not EMF (a
    // player's report: EMF's build replaced ShroudForge's data under Kiln)
    present = {L"kiln\\kiln.json"};
    CHECK(emf::loader::other_content_tool(look, true, false) == std::optional<std::string>("Kiln (its EML layer and ShroudForge)"));

    present = {L"dbghelp.dll"};
    CHECK(emf::loader::other_content_tool(look, true, false) == std::optional<std::string>("ShroudForge"));
    look.tool_name = [&](const std::wstring&) { return std::string(); };  // a dbghelp.dll EMF does not know
    CHECK(emf::loader::other_content_tool(look, true, false) == std::optional<std::string>("dbghelp.dll (EML or ShroudForge)"));

    // the installer renames another tool's dinput8.dll to <word>.dll and lists it in dinput8-chain.ini
    present.clear();
    look.chain = {L"creative.dll", L"eml.dll"};
    read.clear();
    CHECK(emf::loader::other_content_tool(look, true, false) == std::optional<std::string>("EML (from dinput8-chain.ini: eml.dll)"));
    CHECK(read.empty());  // known by its name: no DLL read
    look.chain = {L"ShroudForge.dll"};
    CHECK(emf::loader::other_content_tool(look, true, false) == std::optional<std::string>("ShroudForge (from dinput8-chain.ini: ShroudForge.dll)"));
    // under another name it takes reading the DLLs, which only happens when asked
    look.chain = {L"creative.dll", L"modloader.dll"};
    look.tool_name = [&](const std::wstring& name) -> std::string {
        read.push_back(name);
        return name == L"modloader.dll" ? "EML (Enshrouded Mod Loader)" : "";
    };
    CHECK_FALSE(emf::loader::other_content_tool(look, true, false).has_value());
    CHECK(read.empty());
    CHECK(emf::loader::other_content_tool(look, true, true) ==
          std::optional<std::string>("EML (Enshrouded Mod Loader) (from dinput8-chain.ini: modloader.dll)"));
}

TEST_CASE("content owner: a dbghelp.dll is named without reading it unless asked, and chained DLLs known clean are not read") {
    emf::loader::ToolLook look;
    std::vector<std::wstring> read;
    look.exists = [](const std::wstring& name) { return name == L"dbghelp.dll"; };
    look.tool_name = [&](const std::wstring& name) -> std::string {
        read.push_back(name);
        return "EML (Enshrouded Mod Loader)";
    };
    CHECK(emf::loader::other_content_tool(look, false, false) == std::optional<std::string>("dbghelp.dll (EML or ShroudForge)"));
    CHECK(read.empty());
    CHECK(emf::loader::other_content_tool(look, true, false) == std::optional<std::string>("EML (Enshrouded Mod Loader)"));
    CHECK(read == std::vector<std::wstring>{L"dbghelp.dll"});

    look.exists = [](const std::wstring&) { return false; };
    look.chain = {L"creative.dll", L"stats.dll"};
    look.tool_name = [&](const std::wstring& name) -> std::string { return read.push_back(name), std::string(); };
    look.known_clean = [](const std::wstring& name) { return name == L"creative.dll"; };  // unchanged since the last build
    read.clear();
    CHECK_FALSE(emf::loader::chained_tool_by_text(look).has_value());
    CHECK(read == std::vector<std::wstring>{L"stats.dll"});
}

TEST_CASE("content owner: another program that loads EMF's loader gets no EML content") {
    auto f = all_there();
    f.game = false;
    const auto d = emf::loader::decide_content(f);
    CHECK_FALSE(d.emf_owns);
    CHECK(d.why.find("not the game") != std::string::npos);
}
