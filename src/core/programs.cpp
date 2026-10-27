#include "programs.h"

#include <windows.h>

#include <array>
#include <utility>

#include "common/seh.h"
#include "fault_report.h"

namespace emf::core {
namespace {

template <class T>
bool rd(const void* p, T& out) {
    return seh_read(p, &out, sizeof(T));
}

std::array<ProgramHookSlot, kMaxProgramHooks> g_slots{};
SRWLOCK g_slots_lock = SRWLOCK_INIT;

bool guarded(ProgramCallback cb, void* ctx, void* user) {
    __try {
        cb(ctx, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <size_t I>
void trampoline(void* ctx) {
    ProgramHookSlot& s = g_slots[I];
    if (s.pre && !s.faulted && !guarded(s.pre, ctx, s.user)) {
        s.faulted = true;
        report_fault("program hook (pre)", reinterpret_cast<const void*>(s.pre));
    }
    s.original(ctx);
    if (s.post && !s.faulted && !guarded(s.post, ctx, s.user)) {
        s.faulted = true;
        report_fault("program hook (post)", reinterpret_cast<const void*>(s.post));
    }
}

template <size_t... I>
constexpr std::array<void (*)(void*), sizeof...(I)> make_trampolines(std::index_sequence<I...>) {
    return {&trampoline<I>...};
}
constexpr auto kTrampolines = make_trampolines(std::make_index_sequence<kMaxProgramHooks>{});

}  // namespace

std::vector<ProgramInfo> list_programs(const uint8_t* world) {
    std::vector<ProgramInfo> out;
    uint16_t count = 0;
    if (!world || !rd(world + kProgramCount, count) || count > kProgramMax) return out;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* e = world + kProgramTable + uint64_t(i) * kProgramStride;
        ProgramInfo p;
        p.index = i;
        const char* name = nullptr;
        uint64_t len = 0;
        if (!rd(e, p.desc) || !p.desc || !rd(e + 0x10, name) || !rd(e + 0x18, len) || !name || len == 0 || len > 128)
            continue;
        p.name.resize(len);
        if (!seh_read(name, p.name.data(), len)) continue;
        rd(p.desc + 0x10, p.update_fn);
        out.push_back(std::move(p));
    }
    return out;
}

ProgramInfo find_program(const uint8_t* world, std::string_view name) {
    for (auto& p : list_programs(world))
        if (p.name == name) return p;
    return {};
}

void* claim_program_hook(uintptr_t target, ProgramCallback pre, ProgramCallback post, void* user, size_t* slot_out) {
    if (!target) return nullptr;
    AcquireSRWLockExclusive(&g_slots_lock);
    size_t free_slot = kMaxProgramHooks;
    for (size_t i = 0; i < kMaxProgramHooks; ++i) {
        if (g_slots[i].target == target) {
            ReleaseSRWLockExclusive(&g_slots_lock);
            return nullptr;  // already hooked
        }
        if (!g_slots[i].target && free_slot == kMaxProgramHooks) free_slot = i;
    }
    if (free_slot == kMaxProgramHooks) {
        ReleaseSRWLockExclusive(&g_slots_lock);
        return nullptr;
    }
    g_slots[free_slot] = {target, nullptr, pre, post, user, false};
    ReleaseSRWLockExclusive(&g_slots_lock);
    if (slot_out) *slot_out = free_slot;
    return reinterpret_cast<void*>(kTrampolines[free_slot]);
}

void** program_hook_original(size_t slot) { return reinterpret_cast<void**>(&g_slots[slot].original); }
const ProgramHookSlot& program_hook_slot(size_t slot) { return g_slots[slot]; }

void reset_program_hooks() {
    AcquireSRWLockExclusive(&g_slots_lock);
    g_slots = {};
    ReleaseSRWLockExclusive(&g_slots_lock);
}

}  // namespace emf::core
