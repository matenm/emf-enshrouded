#include "hook_table.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace emf::core {

namespace {
std::string hex(uintptr_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// A 16-byte stub "mov rax, detour; jmp rax" in executable memory that lives for the process. Hooks install their
// stub (not the mod's function), so chaining patches the previous hook's stub and never a mod's code: a detour used
// on two functions (or merged with another by the linker) cannot carry a later hook to the other function.
void* real_stub(void* detour) {
    static std::mutex m;
    static uint8_t* page = nullptr;
    static size_t used = 4096;
    std::lock_guard<std::mutex> l(m);
    if (used + 16 > 4096) {
        // RWX: stubs on the page may run while a new one is written next to them
        page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!page) return nullptr;
        used = 0;
    }
    uint8_t* s = page + used;
    used += 16;
    s[0] = 0x48;  // mov rax, imm64
    s[1] = 0xB8;
    std::memcpy(s + 2, &detour, 8);
    s[10] = 0xFF;  // jmp rax (rax holds no argument in the x64 calling convention)
    s[11] = 0xE0;
    std::memset(s + 12, 0xCC, 4);
    FlushInstructionCache(GetCurrentProcess(), s, 16);
    return s;
}

constexpr uintptr_t kPatchSpan = 16;  // functions start 16 bytes apart; a hook patches the first 5-15
}  // namespace

HookTable::HookTable(CreateHookFn create) : make_stub_(real_stub) {
    if (create)
        create_ = [c = std::move(create)](uintptr_t t, void* d, void** o, std::string*) { return c(t, d, o); };
}
HookTable::HookTable(CreateHookWhyFn create) : create_(std::move(create)), make_stub_(real_stub) {}

HookTable::Result HookTable::install(const std::string& owner, const std::string& name, uintptr_t target, void* detour,
                                     void** original, std::string* why) {
    auto refuse = [why](Result r, const std::string& text) {
        if (why) *why = text;
        return r;
    };
    if (!target || !detour) return refuse(Result::Refused, "no address or no detour");
    if (code_check_ && !code_check_(target)) return refuse(Result::NotCode, hex(target) + " is not game code");
    std::lock_guard<std::mutex> l(m_);
    uintptr_t patch = target;
    for (const auto& e : entries_) {
        const uintptr_t d = e.target > target ? e.target - target : target - e.target;
        if (e.target != target && d < kPatchSpan)
            return refuse(Result::Refused, hex(target) + " is inside the bytes the hook " + e.owner + ":" + e.name +
                                               " patched at " + hex(e.target) + " (hook " + hex(e.target) + " itself)");
        if (e.target != target) continue;
        if (e.detour == detour) return refuse(Result::Refused, "this detour already hooks " + hex(target));
        patch = reinterpret_cast<uintptr_t>(e.stub);  // the newest hook's stub in the chain (entries are in order)
    }
    void* stub = make_stub_ ? make_stub_(detour) : detour;
    if (!stub) return refuse(Result::Refused, "no memory for the hook's stub");
    std::string made_why;
    if (!create_ || !create_(patch, stub, original, &made_why))
        return refuse(Result::Refused, "MinHook refused to hook " + hex(patch) +
                                           (patch == target ? "" : " (the previous hook in the chain)") + ": " +
                                           (made_why.empty() ? "too short, or patched by another tool" : made_why));
    entries_.push_back({owner, name, target, patch, detour, stub});
    return Result::Ok;
}

std::vector<HookTable::Entry> HookTable::list() const {
    std::lock_guard<std::mutex> l(m_);
    return entries_;
}

}  // namespace emf::core
