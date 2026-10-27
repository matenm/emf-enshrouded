#pragma once
// Every inline hook EMF and the mods install, by owner and name. Each hook installs its own jump stub to its detour;
// a hook on a function that is already hooked patches the newest hook's stub (the newer hook runs first, its
// "original" calls the older detour, the oldest calls the game), so two mods, or a mod and EMF, can hook the same
// function, and no mod's code is ever patched. A hook inside another hook's patched bytes is refused. Hooks are never
// removed. Thread-safe.
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace emf::core {

// Creates and enables a hook (MinHook): `*original` must be set before the hook is live.
using CreateHookFn = std::function<bool(uintptr_t target, void* detour, void** original)>;
// The same, and on failure says why (MinHook's status), which the refusal then quotes.
using CreateHookWhyFn = std::function<bool(uintptr_t target, void* detour, void** original, std::string* why)>;

class HookTable {
public:
    explicit HookTable(CreateHookFn create);
    explicit HookTable(CreateHookWhyFn create);
    // How a detour gets its stub (default: a jump stub in executable memory; tests may pass identity).
    void set_stub_maker(std::function<void*(void*)> make) { make_stub_ = std::move(make); }
    // Addresses that may be hooked (default: anything); the core passes "executable memory of a loaded image".
    void set_code_check(std::function<bool(uintptr_t)> check) { code_check_ = std::move(check); }

    enum class Result { Ok, Refused, NotCode };
    Result install(const std::string& owner, const std::string& name, uintptr_t target, void* detour, void** original,
                   std::string* why = nullptr);

    struct Entry {
        std::string owner, name;
        uintptr_t target = 0;  // the function that was asked for
        uintptr_t hooked = 0;  // what was patched: the target, or the previous detour in its chain
        void* detour = nullptr;
        void* stub = nullptr;  // what MinHook installed: a jump to `detour` that the next hook in the chain patches
    };
    std::vector<Entry> list() const;

private:
    CreateHookWhyFn create_;
    std::function<void*(void*)> make_stub_;
    std::function<bool(uintptr_t)> code_check_;
    mutable std::mutex m_;
    std::vector<Entry> entries_;
};

}  // namespace emf::core
