#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace emf::core {

// One entry of a world's ECS program table (world+0x746350, stride 0x1F20, count u16 at world+0xE15350).
struct ProgramInfo {
    uint16_t index = 0;
    std::string name;
    const uint8_t* desc = nullptr;  // ProgramDesc (0x188 B): +0x10 update fn
    uintptr_t update_fn = 0;
};

constexpr uint32_t kProgramTable = 0x746350;
constexpr uint32_t kProgramStride = 0x1F20;
constexpr uint32_t kProgramCount = 0xE15350;
constexpr uint32_t kProgramMax = 0x380;

// Snapshot of all programs registered in `world` (fault-guarded; empty on a bad world).
std::vector<ProgramInfo> list_programs(const uint8_t* world);
// The program called `name`, or an empty ProgramInfo (desc == nullptr).
ProgramInfo find_program(const uint8_t* world, std::string_view name);

// Program update hooks: pre(ctx,user) / original / post(ctx,user). Fixed pool of trampolines.
using ProgramCallback = void (*)(void* program_context, void* user);
constexpr size_t kMaxProgramHooks = 64;

struct ProgramHookSlot {
    uintptr_t target = 0;
    void (*original)(void*) = nullptr;
    ProgramCallback pre = nullptr;
    ProgramCallback post = nullptr;
    void* user = nullptr;
    bool faulted = false;
};

// Claims a free trampoline for `target`. Returns the detour to install and stores the slot index,
// or nullptr when the pool is full or the target is already hooked.
void* claim_program_hook(uintptr_t target, ProgramCallback pre, ProgramCallback post, void* user, size_t* slot_out);
// Where MinHook must write the original-function pointer for a claimed slot.
void** program_hook_original(size_t slot);
const ProgramHookSlot& program_hook_slot(size_t slot);
void reset_program_hooks();  // tests only

}  // namespace emf::core
