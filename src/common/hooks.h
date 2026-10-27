#pragma once
#include <cstdint>
#include <string>

namespace emf {

// Thin MinHook wrapper. All hooks are created first, then enabled together.
bool hooks_init();
bool hook_create(uintptr_t target, void* detour, void** original);
bool hooks_enable_all();
bool hook_enable(uintptr_t target);
void hooks_shutdown();

// Creates and enables one hook. A hook whose enable fails is removed again, so nothing half-made stays behind and the
// function can be hooked later. On failure `why` (may be null) says which MinHook status it was.
bool hook_install(uintptr_t target, void* detour, void** original, std::string* why = nullptr);

// The MinHook calls hook_install makes; each returns an MH_STATUS (0 = MH_OK). Tests swap one for a fake.
struct HookCalls {
    int (*create)(void* target, void* detour, void** original);
    int (*enable)(void* target);
    int (*remove)(void* target);
};
HookCalls minhook_calls();
bool hook_install_with(const HookCalls& mh, uintptr_t target, void* detour, void** original, std::string* why);

// A MinHook status as text: what it means and its name ("... (MH_ERROR_NOT_EXECUTABLE)").
std::string hook_status_text(int status);

}  // namespace emf
