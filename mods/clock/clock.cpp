// clock: an EMF mod. Shows the game's time of day in a corner of the screen (API v12 hud_set). It only reads the game
// (no world changes), so it also works when you play in someone else's world.
// Settings (mod.json; the Mods tab of the console, or emf/config/clock.ini):
//   place=1     (0 top right, below the quest tracker; 1 bottom right; 2 bottom left)   applies at once
//   h24=1       (0: 2:05 PM)                                                              applies at once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "clock_text.h"
#include "emf/emf.hpp"

namespace {
constexpr const char* kMod = "clock";
constexpr const char* kLine = "clock.time";
constexpr size_t kDaytimeTimeOfDay = 0x48;  // g38_daytime::Daytime: i64 ns since midnight (docs/ENGINE_MAP.md 2.6)

// Written by the settings callback (any world's thread), read by the client tick.
std::atomic<uint32_t> g_place{EMF_HUD_BOTTOM_RIGHT};
std::atomic<bool> g_h24{true};
std::atomic<bool> g_redraw{false};
// Client tick only.
uint64_t g_last_ms = 0;
std::string g_shown;

bool guarded_read(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::wstring ini_path() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d(exe);
    return d.substr(0, d.find_last_of(L"\\/")) + L"\\emf\\config\\clock.ini";
}

void read_settings() {
    if (emf::has_v13()) {  // declared by mod.json
        g_place = clockmod::place_from(emf::setting_int(kMod, "place", EMF_HUD_BOTTOM_RIGHT));
        g_h24 = emf::setting_bool(kMod, "h24", true);
    } else {  // an older core: the ini directly
        const std::wstring ini = ini_path();
        g_place = clockmod::place_from(GetPrivateProfileIntW(L"clock", L"place", 1, ini.c_str()));
        g_h24 = GetPrivateProfileIntW(L"clock", L"h24", 1, ini.c_str()) != 0;
    }
    g_redraw = true;
}

void on_tick(emf::World w) {
    if (!w.is_client()) return;  // the client world exists for everyone, host or not
    const uint64_t now = GetTickCount64();
    const bool redraw = g_redraw.exchange(false);
    if (!redraw && now - g_last_ms < 1000) return;
    g_last_ms = now;
    const auto* daytime = static_cast<const uint8_t*>(w.singleton("g38_daytime::Daytime"));
    int64_t tod = -1;
    if (!daytime || !guarded_read(&tod, daytime + kDaytimeTimeOfDay, sizeof(tod))) tod = -1;
    const std::string text = clockmod::text(tod, g_h24);  // "" (the line goes away) when there is no time
    if (text == g_shown && !redraw) return;
    g_shown = text;
    emf::hud(kLine, text, g_place);
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;
    if (GetPrivateProfileIntW(L"clock", L"enabled", 1, ini_path().c_str()) == 0) {
        EMF_LOG(kMod, "disabled ([clock] enabled=0)");
        return true;
    }
    if (!emf::has_v12()) {
        EMF_LOG(kMod, "needs EMF API v12 (hud_set): disabled");
        return true;
    }
    read_settings();
    emf::on_setting_changed(kMod, [](emf::World, const std::string&) { read_settings(); });  // the Mods tab, "set"
    emf::on_tick(on_tick);
    const uint32_t place = g_place;
    EMF_LOG(kMod, "ready: the time of day in the %s corner (%s)",
            place == 0 ? "top right" : place == 1 ? "bottom right" : "bottom left", g_h24 ? "24 h" : "12 h");
    return true;
}
