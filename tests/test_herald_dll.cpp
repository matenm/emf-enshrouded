// Server Herald's DLL (mods/herald) loaded over the Lua tests' fake API (tests/lua_test_fake.h): what it does once the
// Mods tab switched it off. Its rules alone are test_herald.cpp.
#include <windows.h>

#include <doctest.h>

#include <string>

#include "lua_test_fake.h"
#include "test_scratch.h"

namespace {
std::wstring herald_exe_dir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/"));
}

// The Mods tab's switch: emf\config\herald.ini [herald] enabled, then the core's "enabled" change call.
void switch_herald(Fake& f, const std::wstring& ini, bool on) {
    WritePrivateProfileStringW(L"herald", L"enabled", on ? L"1" : L"0", ini.c_str());
    for (const auto& [mod, watcher] : f.setting_watchers)
        if (mod == "herald") watcher.first(kServer, "herald", "enabled", watcher.second);
}

std::string all_replies(Fake& f) {
    std::string out;
    for (const auto& r : f.replies) out += r + "\n";
    return out;
}
}  // namespace

TEST_CASE("herald.dll: switched off in the Mods tab, every herald command says so and sends nothing") {
    const test_scratch::BuildFolderLock lock;  // herald.ini is <exe folder>\emf\config\herald.ini for every run of this build
    const std::wstring dir = herald_exe_dir();
    CreateDirectoryW((dir + L"\\emf").c_str(), nullptr);
    CreateDirectoryW((dir + L"\\emf\\config").c_str(), nullptr);
    const std::wstring ini = dir + L"\\emf\\config\\herald.ini";
    struct Remove {
        std::wstring path;
        ~Remove() { DeleteFileW(path.c_str()); }
    } remove{ini};
    Rig r(L"herald_dll");
    r.fake.settings["herald.chat_log"] = "0";  // nothing is written into the build folder's emf\logs
    r.fake.settings["herald.playtime"] = "0";
    static EmfApi api;  // the DLL keeps the pointer for the life of the process
    api = r.api;
    api.on_tick = [](EmfWorldCallback, void*) {};  // the herald's second: not run here
    api.local_player = []() -> uint32_t { return 1; };
    HMODULE m = LoadLibraryW((dir + L"\\dist\\emf\\mods\\herald\\herald.dll").c_str());
    REQUIRE(m != nullptr);
    auto load = reinterpret_cast<EmfModLoadFn>(GetProcAddress(m, "emf_mod_load"));
    REQUIRE(load != nullptr);
    REQUIRE(load(&api));
    switch_herald(r.fake, ini, true);
    r.run_command("herald.say Hello all");
    REQUIRE(r.fake.broadcasts.size() == 1u);

    switch_herald(r.fake, ini, false);
    for (const char* command : {"herald.say Still here?", "herald.tell #1 hi", "herald.restart 5 nightly", "herald.cancel",
                                "herald.save", "herald.seen Ember", "herald.reload", "players", "playtime", "motd"}) {
        const std::string line = command;
        CAPTURE(line);
        r.fake.replies.clear();
        r.run_command(command);
        CHECK(all_replies(r.fake).find("Server Herald is switched off") != std::string::npos);
    }
    CHECK(r.fake.broadcasts.size() == 1u);  // nothing more went out
    CHECK(r.fake.tells.empty());
    CHECK(r.fake.saves == 0);

    switch_herald(r.fake, ini, true);  // on again: the commands work again
    r.fake.replies.clear();
    r.run_command("herald.say Back");
    CHECK(r.fake.broadcasts.size() == 2u);
    switch_herald(r.fake, ini, false);  // left off: the DLL stays loaded in this process
}
