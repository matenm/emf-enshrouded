// Loads the real event_log.dll with a fake API table and checks what it subscribes to.
#include <windows.h>

#include <doctest.h>

#include <cstdarg>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "emf/emf_api.h"

namespace {
std::vector<std::string> g_el_log;
std::set<std::string> g_el_events;
std::set<uint8_t> g_el_kinds;
int g_el_ticks = 0;

void el_log(const char* mod, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_el_log.push_back(std::string("[") + mod + "] " + buf);
}
bool el_on_event(uint8_t kind, const char* name, uint32_t size, EmfWorldEventCallback cb, void*) {
    g_el_kinds.insert(kind);
    g_el_events.insert(name);
    return cb != nullptr && size > 0;
}
void el_on_tick(EmfWorldCallback, void*) { ++g_el_ticks; }

std::wstring event_log_dll() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\dist\\emf\\mods\\event_log\\event_log.dll";
}
}  // namespace

TEST_CASE("event_log.dll subscribes to the gameplay events on the server world and reports every tick") {
    static EmfApi api{};
    api.version = EMF_API_VERSION;
    api.size = sizeof(EmfApi);
    api.log = el_log;
    api.on_event = el_on_event;
    api.on_tick = el_on_tick;
    HMODULE m = LoadLibraryW(event_log_dll().c_str());
    REQUIRE(m != nullptr);
    auto load = reinterpret_cast<EmfModLoadFn>(GetProcAddress(m, "emf_mod_load"));
    REQUIRE(load != nullptr);
    CHECK(load(&api));
    const std::set<std::string> expected{"EntityDiedEvent", "HitEvent", "PickupEvent", "CraftRecipeSuccessEvent",
                                         "LevelUpEvent", "XPGainedEvent", "BuildingPlaceEvent", "ConsumeItemEvent"};
    CHECK(g_el_events == expected);
    CHECK(g_el_kinds == std::set<uint8_t>{EMF_WORLD_SERVER});
    CHECK(g_el_ticks == 1);
    REQUIRE_FALSE(g_el_log.empty());
    CHECK(g_el_log.back().find("logging gameplay events") != std::string::npos);
}
