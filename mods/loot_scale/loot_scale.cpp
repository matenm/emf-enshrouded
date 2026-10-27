// loot_scale: more (or fewer) materials per loot stack, on top of the world's own "Loot Amount" setting.
//
// The game reads that setting through one small function (BalancingRegistry -> GameSettings
// .resourceDropStackAmountFactor) whenever it fills a container, applies an enemy's or chest's default inventory, or
// drops a world pickup (docs/engine_parts/re_content.md, "Loot"). This mod hooks that function by its bytes (API v14
// hook_pattern) and multiplies what it returns. Nothing is written into the world: switch the mod off and the world's
// setting is what it was. On a dedicated server or a game you host it applies to every player (loot is the host's).
// The pattern is in loot_scale.patterns too: "emf_sigcheck --patterns loot_scale.patterns <exe>" checks it after an
// update without starting the game.
#include <windows.h>

#include <atomic>

#include "emf/emf.hpp"
#include "loot_scale.h"

namespace {
constexpr const char* kMod = "loot_scale";
// mov rax, [rcx+0x90] (BalancingRegistry -> GameSettings); movss xmm0, [rax+0x38] (resourceDropStackAmountFactor); ret
constexpr const char* kLootFactor = "48 8B 81 90 00 00 00 F3 0F 10 40 38 C3";

using GetFactorFn = float (*)(void* balancing);
GetFactorFn g_original = nullptr;
std::atomic<double> g_factor{2.0};

std::atomic<bool> g_told{false};

float detour(void* balancing) {
    const float game = g_original(balancing);
    const float ours = lootscale::scaled(game, g_factor.load());
    if (!g_told.exchange(true))  // once: shows the hook works (the first loot after loading)
        EMF_LOG(kMod, "first loot: the world's loot amount %.2f, with this mod %.2f", game, ours);
    return ours;
}

void read_settings() {
    const double f = lootscale::clamp_factor(emf::setting_float(kMod, "factor", 2.0));
    if (f != g_factor.load()) EMF_LOG(kMod, "loot amount x%.2f", f);
    g_factor = f;
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;
    if (!emf::has_v14()) {
        EMF_LOG(kMod, "needs EMF API v14 (hook_pattern)");
        return false;
    }
    read_settings();
    if (!emf::hook_pattern("loot_scale.factor", kLootFactor, reinterpret_cast<void*>(&detour),
                           reinterpret_cast<void**>(&g_original))) {
        EMF_LOG(kMod, "the game's loot amount function was not found or could not be hooked (see above): off");
        return false;
    }
    emf::on_setting_changed(kMod, [](emf::World, const std::string&) { read_settings(); });
    EMF_LOG(kMod, "on: loot amount x%.2f (on top of the world's Loot Amount setting)", g_factor.load());
    return true;
}
