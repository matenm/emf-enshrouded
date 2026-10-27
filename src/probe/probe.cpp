// Probe step A: read-only hooks.
//  - SimStep (0x140a1e2e0): after each world tick, the Reporter logs world/player/villager info.
//  - RegisterSingleton (0x1409d50d0): logs every singleton name -> object as worlds are built.
//  - NpcSetState (0x140453cf0): logs villager state transitions.
#include "probe.h"

#include <windows.h>

#include <atomic>
#include <memory>
#include <string>

#include "common/hooks.h"
#include "common/log.h"
#include "common/offsets.h"
#include "common/seh.h"
#include "common/world_view.h"
#include "report.h"

namespace emf::probe {
namespace {

struct NameSlice {
    const char* ptr;
    uint64_t len;
};

using SimStepFn = void (*)(void* storage, char ticked);
using RegisterSingletonFn = uint64_t (*)(void* world, const NameSlice* name, void* object, void* options);
using SetStateFn = uint64_t (*)(void* ctx, uint8_t new_state);

SimStepFn o_sim_step = nullptr;
RegisterSingletonFn o_register_singleton = nullptr;
SetStateFn o_set_state = nullptr;
std::unique_ptr<Reporter> g_reporter;

std::atomic<int> g_singleton_lines{0};
constexpr int kMaxSingletonLines = 400;
std::atomic<uint64_t> g_npc_window{0};
std::atomic<int> g_npc_lines_in_window{0};
constexpr int kMaxNpcLinesPerSecond = 50;

void log_line(const std::string& s) { emf::logf("%s", s.c_str()); }

void hk_sim_step(void* storage, char ticked) {
    o_sim_step(storage, ticked);
    try {
        g_reporter->on_tick(WorldView::from_storage(storage), GetTickCount64(), log_line);
    } catch (...) {
        // never let a C++ exception reach game code
    }
}

uint64_t hk_register_singleton(void* world, const NameSlice* name, void* object, void* options) {
    const uint64_t result = o_register_singleton(world, name, object, options);
    if (g_singleton_lines.fetch_add(1) < kMaxSingletonLines) {
        NameSlice n{};
        char text[128] = {};
        if (seh_read(name, &n, sizeof(n)) && n.ptr && n.len)
            seh_read(n.ptr, text, n.len < sizeof(text) - 1 ? n.len : sizeof(text) - 1);
        const uint8_t kind = WorldView(static_cast<uint8_t*>(world)).kind();
        emf::logf("singleton world=%p kind=%u %s -> %p (ok=%llu)", world, kind, text, object,
                  (unsigned long long)(result & 0xFF));
    }
    return result;
}

uint64_t hk_set_state(void* ctx, uint8_t new_state) {
    uint32_t id = 0;
    uint8_t old_state = 0;
    if (npc_ctx_info(ctx, &id, &old_state)) {
        const uint64_t second = GetTickCount64() / 1000;
        if (g_npc_window.exchange(second) != second) g_npc_lines_in_window = 0;
        if (g_npc_lines_in_window.fetch_add(1) < kMaxNpcLinesPerSecond)
            emf::logf("npc %u state %u -> %u", id, old_state, new_state);
    }
    return o_set_state(ctx, new_state);
}

}  // namespace

void start(const std::array<Resolved, kSigCount>& sigs, bool known_build) {
    auto addr = [&](Sig s) { return sigs[static_cast<size_t>(s)].addr; };
    g_reporter = std::make_unique<Reporter>(reinterpret_cast<GetComponentFn>(addr(Sig::GetComponent)));
    if (!hooks_init()) {
        emf::logf("probe: MinHook init failed");
        return;
    }
    int created = 0;
    created += hook_create(addr(Sig::SimStep), reinterpret_cast<void*>(&hk_sim_step),
                           reinterpret_cast<void**>(&o_sim_step));
    created += hook_create(addr(Sig::RegisterSingleton), reinterpret_cast<void*>(&hk_register_singleton),
                           reinterpret_cast<void**>(&o_register_singleton));
    if (is_usable(sigs[static_cast<size_t>(Sig::NpcSetState)], known_build)) {
        created += hook_create(addr(Sig::NpcSetState), reinterpret_cast<void*>(&hk_set_state),
                               reinterpret_cast<void**>(&o_set_state));
    }
    const bool enabled = hooks_enable_all();
    emf::logf("probe: %d hooks created, enable %s", created, enabled ? "OK" : "FAILED");
}

void stop() { emf::hooks_shutdown(); }

}  // namespace emf::probe
