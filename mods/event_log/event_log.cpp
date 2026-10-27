// event_log: an EMF sample mod for on_event (API v4). On the host (server world) it logs gameplay events:
// deaths, hits, pickups, crafting, level ups, experience, building and consumed items. The first 20 of each
// kind with their details, then a count per kind every 10 s. Read-only; turn it off with
// emf/config/event_log.ini: [event_log] enabled=0
#include <windows.h>

#include <cstdint>
#include <string>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"

namespace gen = emf::gen;

namespace {
constexpr const char* kMod = "event_log";
constexpr int kDetailed = 20;

enum Kind { Died, Hit, Pickup, Craft, LevelUp, Xp, Build, Consume, kKinds };
const char* const kNames[kKinds] = {"deaths", "hits", "pickups", "crafts", "level ups", "xp gains", "buildings placed",
                                    "items consumed"};
int g_total[kKinds] = {};
int g_since_report[kKinds] = {};
uint64_t g_last_report_ms = 0;

// True for the first kDetailed events of a kind (server thread only).
bool count(Kind k) {
    ++g_since_report[k];
    return ++g_total[k] <= kDetailed;
}

const char* what_is(emf::World w, uint32_t id) {
    if (id && id == emf::local_player()) return "the player";
    if (w.get<gen::ecs::DynamicActiveNpcState>(id)) return "a villager";
    if (w.get<gen::ecs::DynamicEnemy01BehaviorState>(id)) return "an enemy/creature";
    return "an entity";
}

void report(emf::World w) {
    if (!w.is_server()) return;
    const uint64_t now = GetTickCount64();
    if (g_last_report_ms == 0) g_last_report_ms = now;
    if (now - g_last_report_ms < 10000) return;
    g_last_report_ms = now;
    std::string line;
    for (int k = 0; k < kKinds; ++k) {
        if (!g_since_report[k]) continue;
        line += (line.empty() ? "" : ", ") + std::to_string(g_since_report[k]) + " " + kNames[k];
        g_since_report[k] = 0;
    }
    if (!line.empty()) EMF_LOG(kMod, "last 10 s: %s", line.c_str());
}

bool subscribe_all() {
    bool ok = true;
    ok &= emf::on_event<gen::ecs::EntityDiedEvent>(EMF_WORLD_SERVER, [](emf::World w, const gen::ecs::EntityDiedEvent& e) {
        if (count(Died)) EMF_LOG(kMod, "died: entity %u (%s)", e.deadEntityId.id, what_is(w, e.deadEntityId.id));
    });
    ok &= emf::on_event<gen::ecs::HitEvent>(EMF_WORLD_SERVER, [](emf::World w, const gen::ecs::HitEvent& e) {
        if (count(Hit))
            EMF_LOG(kMod, "hit: %u (%s) by %u, health %+d, stamina %+d", e.targetId.id, what_is(w, e.targetId.id),
                    e.rootSourceId.id, e.healthChange, e.staminaChange);
    });
    ok &= emf::on_event<gen::ecs::PickupEvent>(EMF_WORLD_SERVER, [](emf::World, const gen::ecs::PickupEvent& e) {
        if (count(Pickup))
            EMF_LOG(kMod, "pickup: %u x item 0x%08x by %u", e.itemCount, e.itemId.value, e.pickedUpByEntityId.id);
    });
    ok &= emf::on_event<gen::ecs::CraftRecipeSuccessEvent>(
        EMF_WORLD_SERVER, [](emf::World, const gen::ecs::CraftRecipeSuccessEvent& e) {
            if (count(Craft)) EMF_LOG(kMod, "crafted: recipe 0x%08x at workshop 0x%08x", e.recipeId.value, e.workshopId.value);
        });
    ok &= emf::on_event<gen::ecs::LevelUpEvent>(EMF_WORLD_SERVER, [](emf::World, const gen::ecs::LevelUpEvent& e) {
        if (count(LevelUp)) EMF_LOG(kMod, "level up: player %u", e.playerEntityId.id);
    });
    ok &= emf::on_event<gen::ecs::XPGainedEvent>(EMF_WORLD_SERVER, [](emf::World, const gen::ecs::XPGainedEvent& e) {
        if (count(Xp))
            EMF_LOG(kMod, "xp: player %u +%u (source %u)", e.playerEntityId.id, e.amount, static_cast<unsigned>(e.source));
    });
    ok &= emf::on_event<gen::ecs::BuildingPlaceEvent>(EMF_WORLD_SERVER, [](emf::World, const gen::ecs::BuildingPlaceEvent& e) {
        if (count(Build)) EMF_LOG(kMod, "building placed at (%.1f, %.1f, %.1f)", e.position.x, e.position.y, e.position.z);
    });
    ok &= emf::on_event<gen::ecs::ConsumeItemEvent>(EMF_WORLD_SERVER, [](emf::World, const gen::ecs::ConsumeItemEvent& e) {
        if (count(Consume)) EMF_LOG(kMod, "consumed: player %u", e.playerEntityId.id);
    });
    return ok;
}

std::wstring ini_path() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring ini(exe);
    return ini.substr(0, ini.find_last_of(L"\\/")) + L"\\emf\\config\\event_log.ini";
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;
    if (GetPrivateProfileIntW(L"event_log", L"enabled", 1, ini_path().c_str()) == 0) {
        EMF_LOG(kMod, "disabled in emf\\config\\event_log.ini");
        return true;
    }
    const bool ok = subscribe_all();
    emf::on_tick(report);
    EMF_LOG(kMod, "%s", ok ? "logging gameplay events on the host" : "some event subscriptions were refused (see above)");
    return true;
}
