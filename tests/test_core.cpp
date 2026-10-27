#include <doctest.h>
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/dispatcher.h"
#include "core/mods.h"

using namespace emf::core;

namespace {
std::vector<std::string> g_calls;
EmfWorld* const kWorldA = reinterpret_cast<EmfWorld*>(0x1000);

void cb_a(EmfWorld*, void*) { g_calls.push_back("a"); }
void cb_b(EmfWorld*, void*) { g_calls.push_back("b"); }
void cb_tick(EmfWorld* w, void* user) {
    g_calls.push_back(std::string("tick:") + (w == kWorldA ? "A" : "?") + ":" + static_cast<const char*>(user));
}
void cb_crash(EmfWorld*, void*) { *static_cast<volatile int*>(nullptr) = 1; }

std::wstring exe_dir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/"));
}
}  // namespace

TEST_CASE("posted work runs once, in order, only for the matching world kind") {
    g_calls.clear();
    Dispatcher d;
    d.post(EMF_WORLD_SERVER, cb_a, nullptr);
    d.post(EMF_WORLD_SERVER, cb_b, nullptr);
    d.post(EMF_WORLD_CLIENT, cb_a, nullptr);
    d.run(kWorldA, EMF_WORLD_SERVER, [](const std::string&) {});
    CHECK(g_calls == std::vector<std::string>{"a", "b"});
    d.run(kWorldA, EMF_WORLD_SERVER, [](const std::string&) {});
    CHECK(g_calls.size() == 2);  // not repeated
    d.run(kWorldA, EMF_WORLD_CLIENT, [](const std::string&) {});
    CHECK(g_calls == std::vector<std::string>{"a", "b", "a"});
}

TEST_CASE("tick subscribers run after every step with the world pointer") {
    g_calls.clear();
    Dispatcher d;
    static char tag[] = "x";
    d.subscribe_tick(cb_tick, tag);
    d.run(kWorldA, EMF_WORLD_SERVER, [](const std::string&) {});
    d.run(kWorldA, EMF_WORLD_CLIENT, [](const std::string&) {});
    CHECK(g_calls == std::vector<std::string>{"tick:A:x", "tick:A:x"});
}

TEST_CASE("a faulting tick subscriber is disabled and reported; others keep running") {
    g_calls.clear();
    Dispatcher d;
    static char tag[] = "y";
    d.subscribe_tick(cb_crash, nullptr);
    d.subscribe_tick(cb_tick, tag);
    std::vector<std::string> errors;
    d.run(kWorldA, EMF_WORLD_SERVER, [&](const std::string& e) { errors.push_back(e); });
    d.run(kWorldA, EMF_WORLD_SERVER, [&](const std::string& e) { errors.push_back(e); });
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("emf_tests.exe") != std::string::npos);  // names the module the callback lives in
    CHECK(d.active_subscribers() == 1);
    CHECK(g_calls == std::vector<std::string>{"tick:A:y", "tick:A:y"});
}

TEST_CASE("a faulting posted item does not stop the items after it") {
    g_calls.clear();
    Dispatcher d;
    d.post(EMF_WORLD_SERVER, cb_crash, nullptr);
    d.post(EMF_WORLD_SERVER, cb_b, nullptr);
    int errors = 0;
    d.run(kWorldA, EMF_WORLD_SERVER, [&](const std::string&) { ++errors; });
    CHECK(errors == 1);
    CHECK(g_calls == std::vector<std::string>{"b"});
}

TEST_CASE("dispatching_world is the world being dispatched on this thread, and null outside") {
    Dispatcher d;
    static EmfWorld* seen = nullptr;
    CHECK(dispatching_world() == nullptr);
    d.post(EMF_WORLD_SERVER, [](EmfWorld*, void*) { seen = dispatching_world(); }, nullptr);
    d.run(kWorldA, EMF_WORLD_SERVER, [](const std::string&) {});
    CHECK(seen == kWorldA);
    CHECK(dispatching_world() == nullptr);
}

TEST_CASE("load_mods loads a good mod and reports a faulting one") {
    // The build puts emf_test_mod.dll and emf_test_mod_crash.dll in <exe dir>/test_mods.
    EmfApi api{};
    api.version = EMF_API_VERSION;
    api.size = sizeof(EmfApi);
    std::vector<std::string> reports;
    auto mods = load_mods(exe_dir() + L"\\test_mods", &api, [&](const std::string& s) { reports.push_back(s); });
    REQUIRE(mods.size() == 1);
    auto seen = reinterpret_cast<uint32_t (*)()>(GetProcAddress(mods[0].module, "emf_test_mod_seen_version"));
    REQUIRE(seen != nullptr);
    CHECK(seen() == EMF_API_VERSION);
    bool crash_reported = false;
    for (auto& r : reports) crash_reported |= r.find("emf_test_mod_crash") != std::string::npos;
    CHECK(crash_reported);
}

TEST_CASE("load_mods on a missing directory loads nothing") {
    EmfApi api{};
    CHECK(load_mods(L"Z:\\no\\such\\dir", &api, [](const std::string&) {}).empty());
}

TEST_CASE("load_mods keeps a mod that returned false mapped, so callbacks it registered stay valid") {
    static std::vector<std::pair<EmfWorldCallback, void*>> ticks;
    ticks.clear();
    EmfApi api{};
    api.version = EMF_API_VERSION;
    api.size = sizeof(EmfApi);
    api.on_tick = [](EmfWorldCallback cb, void* user) { ticks.push_back({cb, user}); };
    std::vector<std::string> reports;
    load_mods(exe_dir() + L"\\test_mods", &api, [&](const std::string& s) { reports.push_back(s); });
    HMODULE m = GetModuleHandleW((exe_dir() + L"\\test_mods\\emf_test_mod_reject.dll").c_str());
    REQUIRE(m != nullptr);
    REQUIRE(ticks.size() == 1);
    ticks[0].first(nullptr, ticks[0].second);  // would crash if the DLL had been unloaded
    auto count = reinterpret_cast<int (*)()>(GetProcAddress(m, "emf_test_mod_reject_ticks"));
    REQUIRE(count != nullptr);
    CHECK(count() == 1);
    bool reported = false;
    for (auto& r : reports) reported |= r.find("emf_test_mod_reject") != std::string::npos && r.find("returned false") != std::string::npos;
    CHECK(reported);
}

TEST_CASE("load_mods lists every DLL it keeps mapped, also those that returned false or faulted (for the key hiding)") {
    EmfApi api{};
    api.version = EMF_API_VERSION;
    api.size = sizeof(EmfApi);
    std::vector<HMODULE> kept;
    LoadOptions opt;
    opt.kept_loaded = &kept;
    auto mods = load_mods(exe_dir() + L"\\test_mods", &api, [](const std::string&) {}, opt);
    CHECK(mods.size() == 1);  // only the good one runs
    auto has = [&](const wchar_t* file) {
        HMODULE m = GetModuleHandleW((exe_dir() + L"\\test_mods\\" + file).c_str());
        return m && std::find(kept.begin(), kept.end(), m) != kept.end();
    };
    CHECK(kept.size() == 3);
    CHECK(has(L"emf_test_mod.dll"));
    CHECK(has(L"emf_test_mod_crash.dll"));   // faulted: still mapped
    CHECK(has(L"emf_test_mod_reject.dll"));  // returned false: still mapped
}
