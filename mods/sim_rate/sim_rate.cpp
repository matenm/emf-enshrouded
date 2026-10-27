// sim_rate: an EMF example mod (developer). Hooks the game's simulation step by its bytes (API v14 hook_pattern) and
// shows how many steps run per second, over all worlds, in a corner of the screen.
//
// EMF hooks the same function itself (it runs mods' callbacks after each step): the hooks chain, so this detour runs
// first and its "original" calls EMF's detour, which calls the game. The pattern is also in sim_rate.patterns, so
// "emf_sigcheck --patterns sim_rate.patterns <exe>" checks it after a game update without starting the game.
#include <windows.h>

#include <atomic>
#include <cstdio>

#include "emf/emf.hpp"
#include "sim_rate.h"

namespace {
constexpr const char* kMod = "sim_rate";
constexpr const char* kLine = "sim_rate.steps";
constexpr const char* kSimStep = "40 55 53 56 41 55 48 8D AC 24 F8 FF FD FF B8 08 01 02 00 E8 ?? ?? ?? ??";

using SimStepFn = void (*)(void* storage, char ticked);
SimStepFn g_original = nullptr;   // set by hook_pattern before the hook is live
std::atomic<uint64_t> g_steps{0};  // counted on every world's thread
std::atomic<uint32_t> g_corner{EMF_HUD_BOTTOM_LEFT};

void detour(void* storage, char ticked) {
    g_steps.fetch_add(1, std::memory_order_relaxed);
    g_original(storage, ticked);  // EMF's detour, then the game
}

void read_settings() {
    const int64_t c = emf::setting_int(kMod, "corner", EMF_HUD_BOTTOM_LEFT);
    g_corner = c >= 0 && c <= EMF_HUD_BOTTOM_LEFT ? static_cast<uint32_t>(c) : EMF_HUD_BOTTOM_LEFT;
}

void on_tick(emf::World w) {
    static uint64_t last_ms = 0, last_steps = 0;
    if (!w.is_client()) return;
    const uint64_t now = GetTickCount64();
    if (last_ms == 0) {
        last_ms = now;
        last_steps = g_steps;
        return;
    }
    if (now - last_ms < 1000) return;
    const uint64_t steps = g_steps;
    char text[64];
    std::snprintf(text, sizeof text, "%u sim steps/s", simrate::per_second(steps - last_steps, now - last_ms));
    emf::hud(kLine, text, g_corner);
    last_ms = now;
    last_steps = steps;
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;
    if (!emf::has_v14()) {
        EMF_LOG(kMod, "needs EMF API v14 (hook_pattern)");
        return false;
    }
    if (!emf::hook_pattern("sim_rate.sim_step", kSimStep, reinterpret_cast<void*>(&detour),
                           reinterpret_cast<void**>(&g_original))) {
        EMF_LOG(kMod, "the simulation step was not found or could not be hooked (see the lines above): off");
        return false;
    }
    read_settings();
    emf::on_setting_changed(kMod, [](emf::World, const std::string&) { read_settings(); });
    emf::on_tick(on_tick);
    if (auto b = emf::game_build())
        EMF_LOG(kMod, "hooked the simulation step on build %u (%s)", b->number, b->known ? "known" : "signatures only");
    return true;
}
