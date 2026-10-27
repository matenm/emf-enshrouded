#include <windows.h>

#include <doctest.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "../mods/big_stacks/big_stacks_logic.h"
#include "emf/emf_api.h"
#include "test_scratch.h"

using big_stacks::big_stack;

TEST_CASE("big_stacks: new stack size from the original, a multiplier and a cap") {
    CHECK(big_stack(1, 4.0, 9999) == 1);        // single items stay single
    CHECK(big_stack(0, 4.0, 9999) == 0);
    CHECK(big_stack(250, 4.0, 9999) == 1000);
    CHECK(big_stack(250, 400.0, 9999) == 9999);  // capped
    CHECK(big_stack(20000, 4.0, 65535) == 65535);
    CHECK(big_stack(250, 0.5, 9999) == 250);     // never lowers
    CHECK(big_stack(250, 4.0, 100) == 250);      // a cap below the original does not lower either
}

TEST_CASE("big_stacks: an item is raised once from its original; a second pass or a reload does not multiply again") {
    big_stacks::Originals o;
    CHECK(o.decide(0x11, 250, 4.0, 9999) == 1000);  // first sight: raise
    CHECK(o.decide(0x11, 1000, 4.0, 9999) == 0);    // already raised: nothing to do
    CHECK(o.decide(0x11, 250, 4.0, 9999) == 1000);  // the game reloaded its data: raise again
    CHECK(o.decide(0x22, 1, 4.0, 9999) == 0);       // single items stay single
    CHECK(o.size() == 2);
}

TEST_CASE("big_stacks: a stack size someone else changed (stack command, another mod) is left alone") {
    big_stacks::Originals o;
    CHECK(o.decide(0x11, 250, 4.0, 9999) == 1000);
    CHECK(o.decide(0x11, 5000, 4.0, 9999) == 0);  // "stack Material_T1_Wood_Logs 5000"
    CHECK(o.decide(0x33, 1, 4.0, 9999) == 0);
    CHECK(o.decide(0x33, 20, 4.0, 9999) == 0);    // "stack Torch 20": not lowered back to 1
}

TEST_CASE("big_stacks works on the host's server world only (one thread; a joined game's client is left alone)") {
    CHECK(big_stacks::processes_world(EMF_WORLD_SERVER));
    CHECK_FALSE(big_stacks::processes_world(EMF_WORLD_CLIENT));
    CHECK_FALSE(big_stacks::processes_world(EMF_WORLD_PREDICTION));
}

namespace {
std::vector<std::string> g_bs_log;
void bs_fake_log(const char* mod, const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    g_bs_log.push_back(std::string("[") + mod + "] " + b);
}
std::wstring big_stacks_dll() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\dist\\emf\\mods\\big_stacks\\big_stacks.dll";
}
}  // namespace

TEST_CASE("big_stacks.dll loads against the SDK and stays off when its ini says enabled=0") {
    // The core skips a mod whose ini says enabled=0; the mod checks too (a loose copy, an older core).
    const test_scratch::BuildFolderLock lock;  // the ini is <exe folder>\emf\config\big_stacks.ini for every run of this build
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\emf";
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\config").c_str(), nullptr);
    const std::wstring ini = dir + L"\\config\\big_stacks.ini";
    WritePrivateProfileStringW(L"big_stacks", L"enabled", L"0", ini.c_str());
    static EmfApi api{};
    api.version = EMF_API_VERSION;
    api.size = sizeof(EmfApi);
    api.log = bs_fake_log;
    HMODULE m = LoadLibraryW(big_stacks_dll().c_str());
    REQUIRE(m != nullptr);
    auto load = reinterpret_cast<EmfModLoadFn>(GetProcAddress(m, "emf_mod_load"));
    REQUIRE(load != nullptr);
    g_bs_log.clear();
    CHECK(load(&api));
    REQUIRE_FALSE(g_bs_log.empty());
    CHECK(g_bs_log[0].find("[big_stacks] disabled") == 0);
    DeleteFileW(ini.c_str());
}
