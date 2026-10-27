// big_stacks: an EMF mod. Raises the maximum stack size of every stackable item (x4, at most 9999 by default) on the
// host (server world; the client world shares the item data), in memory only. Needs emf/config/big_stacks.ini:
//   [big_stacks]
//   enabled=1
//   multiplier=4
//   max=9999
// The change lasts until the game restarts. Stacks you build above the normal size are saved in your inventory as they
// are; without the mod the game may treat them differently (not tested).
#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "big_stacks_logic.h"
#include "emf/emf.hpp"

namespace {
constexpr const char* kMod = "big_stacks";
constexpr uint64_t kCheckMs = 5000;

double g_multiplier = 4.0;
uint16_t g_cap = 9999;
big_stacks::Originals g_originals;  // server thread only
uint64_t g_last_check = 0;

struct Item {
    uint32_t id;
    uint16_t stack;
};
std::vector<Item> g_pending;  // items still to look at in the current pass (spread over ticks)
size_t g_raised = 0, g_failed = 0;
bool g_failure_logged = false;

std::wstring ini_path() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring ini(exe);
    return ini.substr(0, ini.find_last_of(L"\\/")) + L"\\emf\\config\\big_stacks.ini";
}

double now_ms_precise() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / freq.QuadPart;
}

// Every few seconds on the host's server world: raise any stackable item that still has its original size. This also
// covers items that finish loading late and data the game reloads. Work is spread over ticks (at most ~2 ms each) so a
// pass never stalls the world.
void on_tick(emf::World w) {
    if (!big_stacks::processes_world(w.kind())) return;
    const uint64_t now = GetTickCount64();
    if (g_pending.empty()) {
        if (now - g_last_check < kCheckMs) return;
        g_last_check = now;
        emf::api()->for_each_item(w.raw(), [](const EmfItemInfo* i, void* u) {
            static_cast<std::vector<Item>*>(u)->push_back(Item{i->item_id, static_cast<uint16_t>(i->max_stack)});
        }, &g_pending);
        g_raised = g_failed = 0;
        if (g_pending.empty()) return;
    }
    const double t0 = now_ms_precise();
    while (!g_pending.empty() && now_ms_precise() - t0 < 2.0) {
        const Item it = g_pending.back();
        g_pending.pop_back();
        const uint16_t target = g_originals.decide(it.id, it.stack, g_multiplier, g_cap);
        if (!target) continue;
        if (emf::set_item_max_stack(it.id, target))
            ++g_raised;
        else
            ++g_failed;
    }
    if (!g_pending.empty()) return;  // continue next tick
    if (g_raised || (g_failed && !g_failure_logged)) {
        g_failure_logged |= g_failed != 0;
        EMF_LOG(kMod, "raised the stack size of %zu item(s) (x%.1f, at most %u)%s", g_raised, g_multiplier,
                static_cast<unsigned>(g_cap), g_failed ? ", some could not be changed" : "");
    }
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;
    const std::wstring ini = ini_path();
    if (GetPrivateProfileIntW(L"big_stacks", L"enabled", 1, ini.c_str()) == 0) {
        EMF_LOG(kMod, "disabled (set [big_stacks] enabled=1 in emf\\config\\big_stacks.ini)");
        return true;
    }
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"big_stacks", L"multiplier", L"4", text, 32, ini.c_str());
    const double m = std::wcstod(text, nullptr);
    g_multiplier = (m >= 1.0 && m <= 1000.0) ? m : 4.0;
    const UINT cap = GetPrivateProfileIntW(L"big_stacks", L"max", 9999, ini.c_str());
    g_cap = static_cast<uint16_t>(cap >= 1 && cap <= 65535 ? cap : 9999);
    if (!api->set_item_max_stack) {
        EMF_LOG(kMod, "this EMF core cannot change items: disabled");
        return true;
    }
    emf::on_tick(on_tick);
    EMF_LOG(kMod, "ready: stack sizes x%.1f, at most %u, applied when a world has loaded its items", g_multiplier,
            static_cast<unsigned>(g_cap));
    return true;
}
