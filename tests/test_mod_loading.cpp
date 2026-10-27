// The mod loader with folder mods: manifests, dependency order, the enabled switch, settings declared before load,
// broken manifests, script mods. Uses copies of test_folder_mod.dll (it logs "load <folder>").
#include <windows.h>

#include <doctest.h>

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "core/commands.h"
#include "core/console.h"
#include "core/file_poll.h"
#include "core/mod_registry.h"
#include "core/mods.h"
#include "core/settings.h"
#include "test_scratch.h"

using namespace emf::core;

namespace {
std::wstring exe_dir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/"));
}

struct FakeIni : IniBackend {
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

std::vector<std::string> g_loads;
SettingsStore* g_store = nullptr;
bool g_speed_declared_at_load = false;
ModRegistry* g_registry = nullptr;
std::vector<ModRecord> g_registry_at_load;  // what the registry said while z_base loaded
void fake_log(const char*, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::string line(buf);
    if (line.rfind("load ", 0) == 0) {
        g_loads.push_back(line.substr(5));
        if (line == "load z_base" && g_store) g_speed_declared_at_load = g_store->has("z_base", "speed");
        if ((line == "load z_base" || line == "load lua") && g_registry) g_registry_at_load = g_registry->list();
    }
}

// A fresh folder of mods for one test case: <exe>\test_folder_runs\<run>\<name>. <run> is this run's own, so a second
// emf_tests from the same build never loads or deletes these DLLs; earlier runs' folders go once their process ended.
std::wstring fresh_dir(const wchar_t* name) {
    static const std::wstring root = test_scratch::run_folder(exe_dir() + L"\\test_folder_runs");
    const std::wstring dir = root + L"\\" + name;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}
void add_mod(const std::wstring& dir, const wchar_t* folder, const std::string& manifest, const wchar_t* dll_name) {
    const std::wstring f = dir + L"\\" + folder;
    CreateDirectoryW(f.c_str(), nullptr);
    if (!manifest.empty()) std::ofstream(f + L"\\mod.json", std::ios::binary) << manifest;
    if (dll_name)
        CopyFileW((exe_dir() + L"\\test_folder_mod\\emf_test_folder_mod.dll").c_str(), (f + L"\\" + dll_name).c_str(),
                  FALSE);
}
const ModRecord* record(const std::vector<ModRecord>& all, const char* id) {
    for (const auto& r : all)
        if (r.id == id) return &r;
    return nullptr;
}
EmfApi fake_api() {
    EmfApi a{};
    a.version = EMF_API_VERSION;
    a.size = sizeof(EmfApi);
    a.log = fake_log;
    return a;
}
}  // namespace

TEST_CASE("mod loading: folder mods load in dependency order, with their manifest in the registry") {
    const std::wstring dir = fresh_dir(L"order");
    add_mod(dir, L"a_needs_z", R"({"id": "a_needs_z", "name": "Needs Z", "version": "1.0", "authors": ["x", "y"],
        "depends": {"z_base": ">=2.0"}})", L"a_needs_z.dll");
    add_mod(dir, L"z_base", R"({"id": "z_base", "name": "Base", "version": "2.1.0", "description": "the base",
        "settings": [{"key": "speed", "type": "float", "default": 1.5, "min": 0.5, "max": 4}]})", L"z_base.dll");
    FakeIni ini;
    SettingsStore store(&ini);
    ModRegistry registry;
    g_loads.clear();
    g_store = &store;
    g_speed_declared_at_load = false;
    g_registry = &registry;
    g_registry_at_load.clear();
    const EmfApi api = fake_api();
    LoadOptions opt;
    opt.settings = &store;
    opt.registry = &registry;
    std::vector<std::string> reports;
    auto mods = load_mods(dir, &api, [&](const std::string& s) { reports.push_back(s); }, opt);
    CHECK(mods.size() == 2);
    REQUIRE(g_loads.size() == 2);
    CHECK(g_loads[0] == "z_base");  // the dependency first, although it sorts last
    CHECK(g_loads[1] == "a_needs_z");
    CHECK(g_speed_declared_at_load);  // manifest settings exist before emf_mod_load runs
    // mods can ask about other mods (mod_info) during emf_mod_load: the registry is filled before the first load
    REQUIRE(g_registry_at_load.size() == 2);
    for (const auto& r : g_registry_at_load) CHECK(r.status == ModStatus::Pending);
    g_registry = nullptr;
    CHECK(store.get("z_base", "speed") == "1.5");
    const auto list = registry.list();
    const ModRecord* base = record(list, "z_base");
    REQUIRE(base);
    CHECK(base->status == ModStatus::Loaded);
    CHECK(base->name == "Base");
    CHECK(base->version == "2.1.0");
    CHECK(base->description == "the base");
    CHECK(base->has_manifest);
    const ModRecord* user = record(list, "a_needs_z");
    REQUIRE(user);
    CHECK(user->authors == "x, y");
    g_store = nullptr;
}

TEST_CASE("mod loading: the enabled switch, broken manifests, missing DLLs, script mods and failed dependencies") {
    const std::wstring dir = fresh_dir(L"status");
    add_mod(dir, L"off_by_ini", R"({"id": "off_by_ini", "version": "1"})", L"off_by_ini.dll");
    add_mod(dir, L"off_by_default", R"({"id": "off_by_default", "version": "1", "enabled": false})",
            L"off_by_default.dll");
    add_mod(dir, L"on_by_ini", R"({"id": "on_by_ini", "version": "1", "enabled": false})", L"on_by_ini.dll");
    add_mod(dir, L"broken", "{\"id\": \"broken\", \"version\": }", L"broken.dll");
    add_mod(dir, L"no_dll", R"({"id": "no_dll", "version": "1"})", nullptr);
    add_mod(dir, L"needs_off", R"({"id": "needs_off", "version": "1", "depends": {"off_by_ini": "*"}})",
            L"needs_off.dll");
    add_mod(dir, L"scripted", R"({"id": "scripted", "version": "0.3", "main": "main.lua"})", nullptr);
    add_mod(dir, L"data_only", "", nullptr);  // a folder without mod.json is not a mod
    FakeIni ini;
    ini.values["off_by_ini.enabled"] = "0";
    ini.values["on_by_ini.enabled"] = "1";
    SettingsStore store(&ini);
    ModRegistry registry;
    g_loads.clear();
    const EmfApi api = fake_api();
    LoadOptions opt;
    opt.settings = &store;
    opt.registry = &registry;
    std::vector<std::string> reports;
    auto mods = load_mods(dir, &api, [&](const std::string& s) { reports.push_back(s); }, opt);
    CHECK(g_loads == std::vector<std::string>{"on_by_ini"});
    CHECK(mods.size() == 1);
    const auto list = registry.list();
    auto status = [&](const char* id) {
        const ModRecord* r = record(list, id);
        REQUIRE_MESSAGE(r, id);
        return r->status;
    };
    CHECK(status("off_by_ini") == ModStatus::Disabled);
    CHECK(record(list, "off_by_ini")->reason.find("enabled=0") != std::string::npos);
    CHECK(status("off_by_default") == ModStatus::Disabled);
    CHECK(status("on_by_ini") == ModStatus::Loaded);
    CHECK(status("broken") == ModStatus::Rejected);
    CHECK(record(list, "broken")->reason.find("line 1") != std::string::npos);
    CHECK(status("no_dll") == ModStatus::Rejected);
    CHECK(record(list, "no_dll")->reason.find("no_dll.dll") != std::string::npos);
    CHECK(status("needs_off") == ModStatus::Rejected);
    CHECK(record(list, "needs_off")->reason.find("off_by_ini") != std::string::npos);
    CHECK(status("scripted") == ModStatus::Rejected);  // no script host (lua) installed
    CHECK(record(list, "scripted")->reason.find("lua") != std::string::npos);
    CHECK(record(list, "data_only") == nullptr);
    CHECK(GetModuleHandleW((dir + L"\\off_by_ini\\off_by_ini.dll").c_str()) == nullptr);  // never loaded
    bool said = false;
    for (const auto& r : reports) said |= r.find("off_by_ini") != std::string::npos;
    CHECK(said);
}

TEST_CASE("mod loading: loose DLLs keep working next to folder mods; the registry lists them by file name") {
    const std::wstring dir = fresh_dir(L"loose");
    CopyFileW((exe_dir() + L"\\test_folder_mod\\emf_test_folder_mod.dll").c_str(), (dir + L"\\Loose_Mod.dll").c_str(),
              FALSE);
    add_mod(dir, L"folder", R"({"id": "folder", "version": "1"})", L"folder.dll");
    ModRegistry registry;
    g_loads.clear();
    const EmfApi api = fake_api();
    LoadOptions opt;
    opt.registry = &registry;  // no settings store: every mod is on
    auto mods = load_mods(dir, &api, [](const std::string&) {}, opt);
    CHECK(mods.size() == 2);
    const auto list = registry.list();
    const ModRecord* loose = record(list, "loose_mod");
    REQUIRE(loose);
    CHECK(loose->status == ModStatus::Loaded);
    CHECK_FALSE(loose->has_manifest);
    CHECK(loose->source == "Loose_Mod.dll");
    CHECK(record(list, "folder")->source == "folder\\");
}

TEST_CASE("mod loading: a copied mod folder (the same id) is rejected and never shadows the real one") {
    const std::wstring dir = fresh_dir(L"dupes");
    add_mod(dir, L"clock", R"({"id": "clock", "name": "Clock", "version": "1.1"})", L"clock.dll");
    add_mod(dir, L"clock - Copy", R"({"id": "clock", "name": "Clock", "version": "1.1"})", L"clock.dll");
    ModRegistry registry;
    g_loads.clear();
    const EmfApi api = fake_api();
    LoadOptions opt;
    opt.registry = &registry;
    load_mods(dir, &api, [](const std::string&) {}, opt);
    CHECK(g_loads == std::vector<std::string>{"clock"});
    const auto list = registry.list();
    REQUIRE(list.size() == 2);
    CHECK(list[0].source == "clock\\");  // same name: the real mod first, the rejected copy after
    CHECK(list[1].source == "clock - Copy\\");
    CHECK(list[1].status == ModStatus::Rejected);
    const auto found = registry.find("clock");
    REQUIRE(found);
    CHECK(found->status == ModStatus::Loaded);  // find() gives the mod that owns the id, not the rejected copy
}

TEST_CASE("mod loading: a folder mod can ship helper DLLs next to its own (the loader searches the mod's folder)") {
    const std::wstring dir = fresh_dir(L"helper");
    add_mod(dir, L"needs_helper", R"({"id": "needs_helper", "version": "1"})", nullptr);
    const std::wstring from = exe_dir() + L"\\test_folder_mod\\";
    const std::wstring to = dir + L"\\needs_helper\\";
    REQUIRE(CopyFileW((from + L"emf_test_needs_helper.dll").c_str(), (to + L"needs_helper.dll").c_str(), FALSE));
    REQUIRE(CopyFileW((from + L"emf_test_helper.dll").c_str(), (to + L"emf_test_helper.dll").c_str(), FALSE));
    ModRegistry registry;
    g_loads.clear();
    const EmfApi api = fake_api();
    LoadOptions opt;
    opt.registry = &registry;
    std::vector<std::string> reports;
    auto mods = load_mods(dir, &api, [&](const std::string& s) { reports.push_back(s); }, opt);
    std::string all;
    for (const auto& r : reports) all += r + "\n";
    CHECK_MESSAGE(mods.size() == 1, all);
    CHECK(g_loads == std::vector<std::string>{"needs_helper 42"});
}

TEST_CASE("mod loading: script mods follow their switch, get their settings, and are listed before any DLL loads") {
    const std::wstring dir = fresh_dir(L"scripts");
    add_mod(dir, L"lua", R"({"id": "lua", "version": "1"})", L"lua.dll");  // the script host: loads before its scripts
    add_mod(dir, L"z_script", R"({"id": "z_script", "version": "1", "main": "main.lua",
        "settings": [{"key": "speed", "type": "int", "default": 3}]})", nullptr);
    add_mod(dir, L"z_script_off", R"({"id": "z_script_off", "version": "1", "main": "main.lua"})", nullptr);
    FakeIni ini;
    ini.values["z_script_off.enabled"] = "0";
    SettingsStore store(&ini);
    ModRegistry registry;
    g_loads.clear();
    g_registry = &registry;
    g_registry_at_load.clear();
    const EmfApi api = fake_api();
    LoadOptions opt;
    opt.settings = &store;
    opt.registry = &registry;
    load_mods(dir, &api, [](const std::string&) {}, opt);
    g_registry = nullptr;
    const ModRecord* early = record(g_registry_at_load, "z_script");
    REQUIRE(early);
    CHECK(early->status == ModStatus::Script);  // already a script while the host's emf_mod_load ran
    const auto list = registry.list();
    CHECK(record(list, "z_script")->status == ModStatus::Script);
    CHECK(record(list, "z_script_off")->status == ModStatus::Disabled);
    CHECK(store.get("z_script", "speed") == "3");  // declared for the script host
    CHECK_FALSE(store.has("z_script_off", "speed"));
}

TEST_CASE("mod loading: a script mod whose DLL dependency is switched off is not left as a script") {
    const std::wstring dir = fresh_dir(L"scriptdeps");
    add_mod(dir, L"lua", R"({"id": "lua", "version": "1"})", L"lua.dll");
    add_mod(dir, L"host_mod", R"({"id": "host_mod", "version": "1"})", L"host_mod.dll");
    add_mod(dir, L"needed", R"({"id": "needed", "version": "1"})", L"needed.dll");
    add_mod(dir, L"uses_needed", R"({"id": "uses_needed", "version": "1", "main": "main.lua",
        "depends": {"host_mod": ">=1", "needed": ">=1"}})", nullptr);
    add_mod(dir, L"uses_host", R"({"id": "uses_host", "version": "1", "main": "main.lua",
        "depends": {"host_mod": ">=1"}})", nullptr);
    FakeIni ini;
    ini.values["needed.enabled"] = "0";
    SettingsStore store(&ini);
    ModRegistry registry;
    g_loads.clear();
    const EmfApi api = fake_api();
    LoadOptions opt;
    opt.settings = &store;
    opt.registry = &registry;
    std::vector<std::string> lines;
    static std::vector<std::string>* sink = nullptr;
    sink = &lines;
    load_mods(dir, &api, [](const std::string& l) { sink->push_back(l); }, opt);
    const auto list = registry.list();
    REQUIRE(record(list, "uses_needed"));
    CHECK(record(list, "uses_needed")->status == ModStatus::Rejected);
    CHECK(record(list, "uses_needed")->reason.find("needed") != std::string::npos);
    CHECK(record(list, "uses_host")->status == ModStatus::Script);
}

TEST_CASE("mod loading: switched-off mods' settings are declared too (the Mods tab shows them), with no change callbacks") {
    const std::wstring dir = fresh_dir(L"offsettings");
    add_mod(dir, L"lua", R"({"id": "lua", "version": "1"})", L"lua.dll");
    add_mod(dir, L"dll_off", R"({"id": "dll_off", "version": "1",
        "settings": [{"key": "radius", "type": "int", "default": 5, "min": 1, "max": 9}]})", L"dll_off.dll");
    add_mod(dir, L"script_off", R"({"id": "script_off", "version": "1", "main": "main.lua",
        "settings": [{"key": "speed", "type": "int", "default": 3}]})", nullptr);
    add_mod(dir, L"needs_off", R"({"id": "needs_off", "version": "1", "depends": {"dll_off": "*"},
        "settings": [{"key": "size", "type": "int", "default": 2}]})", L"needs_off.dll");
    FakeIni ini;
    ini.values["dll_off.enabled"] = "0";
    ini.values["script_off.enabled"] = "0";
    ini.values["dll_off.radius"] = "99";
    SettingsStore store(&ini);
    ModRegistry registry;
    g_loads.clear();
    const EmfApi api = fake_api();
    LoadOptions opt;
    opt.settings = &store;
    opt.registry = &registry;
    load_mods(dir, &api, [](const std::string&) {}, opt);
    CHECK(g_loads == std::vector<std::string>{"lua"});  // the switched-off DLL never loads
    CHECK(store.get("dll_off", "radius") == "9");        // declared (and clamped) although it is off
    CHECK(ini.values["dll_off.radius"] == "9");
    CHECK(store.get("script_off", "speed") == "3");
    CHECK(store.get("needs_off", "size") == "2");  // not loaded (its dependency is off), its settings still editable
    CHECK(store.take_changes().empty());           // declaring is not a change: no callbacks
}

namespace {
struct LoadRun {
    FakeIni ini;
    SettingsStore store{&ini};
    ModRegistry registry;
    std::vector<ModRecord> list;
    void run(const std::wstring& dir) {
        g_loads.clear();
        const EmfApi api = fake_api();
        LoadOptions opt;
        opt.settings = &store;
        opt.registry = &registry;
        load_mods(dir, &api, [](const std::string&) {}, opt);
        list = registry.list();
    }
    const ModRecord& rec(const char* id) {
        const ModRecord* r = record(list, id);
        REQUIRE_MESSAGE(r, id);
        return *r;
    }
};
void add_script_users(const std::wstring& dir) {
    add_mod(dir, L"a_user", R"({"id": "a_user", "version": "1", "depends": {"b_script": "*"}})", L"a_user.dll");
    add_mod(dir, L"b_script", R"({"id": "b_script", "version": "1", "main": "main.lua"})", nullptr);
    add_mod(dir, L"other_script", R"({"id": "other_script", "version": "1", "main": "main.lua",
        "depends": {"b_script": "*"}})", nullptr);
}
}  // namespace

TEST_CASE("mod loading: script mods run once their host (lua) loaded; mods that need a script load after the host") {
    const std::wstring dir = fresh_dir(L"hoston");
    add_mod(dir, L"lua", R"({"id": "lua", "version": "1"})", L"lua.dll");
    add_script_users(dir);
    LoadRun r;
    r.run(dir);
    CHECK(r.rec("b_script").status == ModStatus::Script);
    CHECK(r.rec("other_script").status == ModStatus::Script);
    CHECK(r.rec("a_user").status == ModStatus::Loaded);
    // by name a_user and b_script come before lua, but a_user needs a script: the host loads first
    CHECK(g_loads == std::vector<std::string>{"lua", "a_user"});
}

TEST_CASE("mod loading: with the script host switched off, script mods and every mod that needs them do not run") {
    const std::wstring dir = fresh_dir(L"hostoff");
    add_mod(dir, L"lua", R"({"id": "lua", "version": "1"})", L"lua.dll");
    add_script_users(dir);
    LoadRun r;
    r.ini.values["lua.enabled"] = "0";
    r.run(dir);
    CHECK(r.rec("b_script").status == ModStatus::Rejected);
    CHECK(r.rec("b_script").reason.find("lua") != std::string::npos);
    CHECK(r.rec("b_script").reason.find("switched off") != std::string::npos);
    CHECK(r.rec("a_user").status == ModStatus::Rejected);
    CHECK(r.rec("a_user").reason.find("b_script") != std::string::npos);
    CHECK(r.rec("other_script").status == ModStatus::Rejected);
    CHECK(g_loads.empty());
}

TEST_CASE("mod loading: without a script host, or when it failed to load, script mods and their users do not run") {
    {
        const std::wstring dir = fresh_dir(L"hostmissing");
        add_script_users(dir);
        LoadRun r;
        r.run(dir);
        CHECK(r.rec("b_script").status == ModStatus::Rejected);
        CHECK(r.rec("b_script").reason.find("lua") != std::string::npos);
        CHECK(r.rec("a_user").status == ModStatus::Rejected);
        CHECK(r.rec("other_script").status == ModStatus::Rejected);
        CHECK(g_loads.empty());
    }
    {
        const std::wstring dir = fresh_dir(L"hostfailed");
        add_mod(dir, L"lua", R"({"id": "lua", "version": "1"})", nullptr);
        REQUIRE(CopyFileW((exe_dir() + L"\\test_mods\\emf_test_mod_reject.dll").c_str(), (dir + L"\\lua\\lua.dll").c_str(),
                          FALSE));  // its emf_mod_load returns false
        add_script_users(dir);
        LoadRun r;
        r.run(dir);
        CHECK(r.rec("lua").status == ModStatus::Failed);
        CHECK(r.rec("b_script").status == ModStatus::Rejected);
        CHECK(r.rec("a_user").status == ModStatus::Rejected);
        CHECK(r.rec("other_script").status == ModStatus::Rejected);
        CHECK(g_loads.empty());
    }
}

TEST_CASE("mod loading: a switched-off script mod does not satisfy a dependency") {
    const std::wstring dir = fresh_dir(L"scriptoff");
    add_mod(dir, L"lua", R"({"id": "lua", "version": "1"})", L"lua.dll");
    add_script_users(dir);
    LoadRun r;
    r.ini.values["b_script.enabled"] = "0";
    r.run(dir);
    CHECK(r.rec("b_script").status == ModStatus::Disabled);
    CHECK(r.rec("a_user").status == ModStatus::Rejected);
    CHECK(r.rec("a_user").reason.find("switched off") != std::string::npos);
    CHECK(r.rec("other_script").status == ModStatus::Rejected);
    CHECK(g_loads == std::vector<std::string>{"lua"});
}

TEST_CASE("mod loading: an optional dependency that is switched off or missing never stops a mod, whatever its version") {
    const std::wstring dir = fresh_dir(L"optional");
    add_mod(dir, L"a_uses_extra", R"({"id": "a_uses_extra", "version": "1",
        "depends": {"z_extra": {"version": "^1.0", "optional": true}, "ghost": {"optional": true}}})", L"a_uses_extra.dll");
    add_mod(dir, L"b_wants_new", R"({"id": "b_wants_new", "version": "1",
        "depends": [{"id": "z_extra", "version": ">=2.0.0", "optional": true}]})", L"b_wants_new.dll");
    add_mod(dir, L"z_extra", R"({"id": "z_extra", "version": "1.4.0"})", L"z_extra.dll");
    FakeIni ini;
    ini.values["z_extra.enabled"] = "0";
    SettingsStore store(&ini);
    ModRegistry registry;
    g_loads.clear();
    const EmfApi api = fake_api();
    LoadOptions opt;
    opt.settings = &store;
    opt.registry = &registry;
    std::vector<std::string> reports;
    load_mods(dir, &api, [&](const std::string& s) { reports.push_back(s); }, opt);
    // z_extra is switched off: both load, also b_wants_new, which could not work with z_extra 1.4.0 if it ran
    CHECK(g_loads == std::vector<std::string>{"a_uses_extra", "b_wants_new"});
    const auto list = registry.list();
    CHECK(record(list, "a_uses_extra")->status == ModStatus::Loaded);
    CHECK(record(list, "z_extra")->status == ModStatus::Disabled);
    CHECK(record(list, "b_wants_new")->status == ModStatus::Loaded);
}

TEST_CASE("mod loading: an optional dependency that runs in a version outside the range stops the mod, saying why") {
    const std::wstring dir = fresh_dir(L"optional_on");
    add_mod(dir, L"b_wants_new", R"({"id": "b_wants_new", "version": "1",
        "depends": [{"id": "z_extra", "version": ">=2.0.0", "optional": true}]})", L"b_wants_new.dll");
    add_mod(dir, L"z_extra", R"({"id": "z_extra", "version": "1.4.0"})", L"z_extra.dll");
    FakeIni ini;
    SettingsStore store(&ini);
    ModRegistry registry;
    g_loads.clear();
    const EmfApi api = fake_api();
    LoadOptions opt;
    opt.settings = &store;
    opt.registry = &registry;
    load_mods(dir, &api, [](const std::string&) {}, opt);
    CHECK(g_loads == std::vector<std::string>{"z_extra"});
    const auto list = registry.list();
    CHECK(record(list, "b_wants_new")->status == ModStatus::Rejected);
    CHECK(record(list, "b_wants_new")->reason ==
          "works with z_extra >=2.0.0 (2.0.0 or newer) when it runs, found 1.4.0: too old (update z_extra, or switch it off)");
}

namespace {
// Rewrites a file and moves its write time on, so a change is seen even within the file system's time resolution.
void rewrite(const std::wstring& path, const std::string& text, int seconds_later) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER t{{ft.dwLowDateTime, ft.dwHighDateTime}};
    t.QuadPart += static_cast<ULONGLONG>(seconds_later) * 10000000ULL;
    ft.dwLowDateTime = t.LowPart;
    ft.dwHighDateTime = t.HighPart;
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}
}  // namespace

TEST_CASE("mod loading: a setting added to mod.json while the game runs is declared at the next look (Mods tab at once)") {
    const std::wstring dir = fresh_dir(L"watch");
    add_mod(dir, L"scripted", R"({"id": "scripted", "version": "1", "main": "main.lua",
        "settings": [{"key": "show", "type": "bool", "default": true}]})", nullptr);
    add_mod(dir, L"lua", R"({"id": "lua", "version": "1.3.0"})", L"lua.dll");
    add_mod(dir, L"broken_from_start", "{\"id\": ", nullptr);
    FakeIni ini;
    SettingsStore store(&ini);
    ModRegistry registry;
    g_loads.clear();
    const EmfApi api = fake_api();
    LoadOptions opt;
    opt.settings = &store;
    opt.registry = &registry;
    load_mods(dir, &api, [](const std::string&) {}, opt);
    REQUIRE(store.has("scripted", "show"));
    ManifestWatch watch;
    CHECK(watch.poll(dir, registry.list(), store).empty());  // the first look only notes the files
    CHECK(watch.poll(dir, registry.list(), store).empty());  // nothing changed

    rewrite(dir + L"\\scripted\\mod.json", R"({"id": "scripted", "version": "1", "main": "main.lua",
        "settings": [{"key": "show", "type": "bool", "default": true, "label": "Show it"},
                     {"key": "corner", "type": "select", "options": ["tr", "bl"], "default": "bl"}]})", 5);
    auto lines = watch.poll(dir, registry.list(), store);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "mod scripted\\: mod.json changed: 2 setting(s) declared again (the Mods tab shows them now)");
    CHECK(store.get("scripted", "corner") == "bl");
    CHECK(store.defs("scripted")[0].label == "Show it");
    CHECK(watch.poll(dir, registry.list(), store).empty());  // seen

    rewrite(dir + L"\\scripted\\mod.json", "{\"id\": \"scripted\", ", 10);  // saved half-way: it does not read
    lines = watch.poll(dir, registry.list(), store);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("does not read") != std::string::npos);
    CHECK(store.has("scripted", "corner"));  // kept as they were

    rewrite(dir + L"\\broken_from_start\\mod.json", R"({"id": "broken_from_start", "version": "1",
        "settings": [{"key": "x", "type": "int"}]})", 15);
    CHECK(watch.poll(dir, registry.list(), store).empty());  // rejected before declaring anything: left for the next start
    CHECK_FALSE(store.has("broken_from_start", "x"));
}

TEST_CASE("mod loading: a second folder with a taken id cannot redefine that mod's settings by editing its mod.json") {
    const std::wstring dir = fresh_dir(L"watch_dup");
    add_mod(dir, L"a_real", R"({"id": "dup", "version": "1", "settings": [{"key": "size", "type": "int", "default": 2}]})",
            L"dup.dll");
    add_mod(dir, L"b_copy", R"({"id": "dup", "version": "1", "settings": [{"key": "size", "type": "int", "default": 2}]})",
            L"dup.dll");
    FakeIni ini;
    SettingsStore store(&ini);
    ModRegistry registry;
    g_loads.clear();
    const EmfApi api = fake_api();
    LoadOptions opt;
    opt.settings = &store;
    opt.registry = &registry;
    load_mods(dir, &api, [](const std::string&) {}, opt);
    ManifestWatch watch;
    watch.poll(dir, registry.list(), store);
    rewrite(dir + L"\\b_copy\\mod.json", R"({"id": "dup", "version": "1",
        "settings": [{"key": "size", "type": "int", "default": 2, "label": "From the copy"}]})", 5);
    CHECK(watch.poll(dir, registry.list(), store).empty());
    const auto defs = store.defs("dup");
    REQUIRE(defs.size() == 1);
    CHECK(defs[0].label == "size");  // the real mod's definition stays
}

TEST_CASE("mod loading: the core's once-a-second file look reaches the mods' mod.json, never writing a saved value back") {
    const std::wstring dir = fresh_dir(L"file_poll");
    add_mod(dir, L"scripted", R"({"id": "scripted", "version": "1", "main": "main.lua",
        "settings": [{"key": "radius", "type": "int", "min": 0, "max": 100, "default": 10}]})", nullptr);
    add_mod(dir, L"lua", R"({"id": "lua", "version": "1.3.0"})", L"lua.dll");
    FakeIni ini;
    ini.values["scripted.radius"] = "50";  // the player's value
    SettingsStore store(&ini);
    ModRegistry registry;
    g_loads.clear();
    const EmfApi api = fake_api();
    LoadOptions opt;
    opt.settings = &store;
    opt.registry = &registry;
    load_mods(dir, &api, [](const std::string&) {}, opt);
    CommandCenter center;
    CommandFile commands(dir + L"\\commands.txt", dir + L"\\commands.done.txt");
    ManifestWatch watch;
    std::vector<std::string> log, replies;
    FilePoll poll;
    poll.commands_file = &commands;
    poll.commands = &center;
    poll.manifests = &watch;
    poll.mods_dir = dir;
    poll.mods = &registry;
    poll.settings = &store;
    poll.log = [&](const std::string& l) { log.push_back(l); };
    poll.reply = [&](const std::string& l) { replies.push_back(l); };
    CHECK(poll.tick(10000));   // the first look notes the files
    CHECK_FALSE(poll.tick(10500));  // not a second yet
    rewrite(dir + L"\\scripted\\mod.json", R"({"id": "scripted", "version": "1", "main": "main.lua",
        "settings": [{"key": "radius", "type": "int", "min": 0, "max": 5, "default": 1}]})", 5);
    CHECK(poll.tick(11000));
    REQUIRE(log.size() == 1);
    CHECK(log[0].find("mod scripted\\: mod.json changed") == 0);
    CHECK(store.get("scripted", "radius") == "5");    // fitted in memory
    CHECK(ini.values["scripted.radius"] == "50");    // the saved value is the player's still
    CHECK(store.take_changes().size() == 1);         // the mod hears its value moved
    std::ofstream(dir + L"\\commands.txt") << "help\n";  // and commands.txt is read on the same look
    CHECK(poll.tick(12000));
    CHECK(center.pending());
}
