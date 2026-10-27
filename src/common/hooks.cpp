#include "hooks.h"

#include <MinHook.h>

namespace emf {
namespace {
bool g_initialized = false;
}

bool hooks_init() {
    if (g_initialized) return true;
    MH_STATUS s = MH_Initialize();
    g_initialized = (s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED);
    return g_initialized;
}

bool hook_create(uintptr_t target, void* detour, void** original) {
    if (!g_initialized || !target || !detour) return false;
    return MH_CreateHook(reinterpret_cast<void*>(target), detour, original) == MH_OK;
}

bool hook_enable(uintptr_t target) {
    return g_initialized && target && MH_EnableHook(reinterpret_cast<void*>(target)) == MH_OK;
}

bool hooks_enable_all() { return g_initialized && MH_EnableHook(MH_ALL_HOOKS) == MH_OK; }

HookCalls minhook_calls() {
    return {[](void* t, void* d, void** o) { return static_cast<int>(MH_CreateHook(t, d, o)); },
            [](void* t) { return static_cast<int>(MH_EnableHook(t)); },
            [](void* t) { return static_cast<int>(MH_RemoveHook(t)); }};
}

std::string hook_status_text(int status) {
    if (status < MH_UNKNOWN || status > MH_ERROR_FUNCTION_NOT_FOUND) return "MinHook status " + std::to_string(status);
    const char* what = nullptr;
    switch (status) {
        case MH_ERROR_NOT_INITIALIZED: what = "MinHook is not initialized"; break;
        case MH_ERROR_ALREADY_CREATED: what = "MinHook already hooks this address"; break;
        case MH_ERROR_NOT_CREATED: what = "the hook was not created"; break;
        case MH_ERROR_ENABLED: what = "the hook is already enabled"; break;
        case MH_ERROR_NOT_EXECUTABLE: what = "the address is not executable code"; break;
        case MH_ERROR_UNSUPPORTED_FUNCTION:
            what = "its first bytes cannot be moved (too short, or patched by another tool)";
            break;
        case MH_ERROR_MEMORY_ALLOC: what = "no memory for a trampoline near the function"; break;
        case MH_ERROR_MEMORY_PROTECT: what = "the code could not be made writable"; break;
        default: break;
    }
    const std::string name = MH_StatusToString(static_cast<MH_STATUS>(status));
    return what ? std::string(what) + " (" + name + ")" : name;
}

bool hook_install_with(const HookCalls& mh, uintptr_t target, void* detour, void** original, std::string* why) {
    auto fail = [why](const std::string& text) {
        if (why) *why = text;
        return false;
    };
    if (!target || !detour) return fail("no address or no detour");
    void* t = reinterpret_cast<void*>(target);
    const int created = mh.create(t, detour, original);
    if (created != MH_OK) return fail(hook_status_text(created));
    const int enabled = mh.enable(t);
    if (enabled == MH_OK) return true;
    // A hook left created but not enabled would make every later attempt on this function fail (already created).
    const int removed = mh.remove(t);
    if (removed == MH_OK && original) *original = nullptr;  // it pointed at the trampoline just freed
    return fail("enabling it failed: " + hook_status_text(enabled) +
                (removed == MH_OK ? "; the hook was removed again"
                                  : "; removing it failed too: " + hook_status_text(removed)));
}

bool hook_install(uintptr_t target, void* detour, void** original, std::string* why) {
    if (!g_initialized) {
        if (why) *why = hook_status_text(MH_ERROR_NOT_INITIALIZED);
        return false;
    }
    return hook_install_with(minhook_calls(), target, detour, original, why);
}

void hooks_shutdown() {
    if (!g_initialized) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_initialized = false;
}

}  // namespace emf
