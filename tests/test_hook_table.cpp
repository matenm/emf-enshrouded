#include <doctest.h>

#include <string>
#include <vector>

#include "core/hook_table.h"

using namespace emf::core;

namespace {
struct Created {
    uintptr_t patched;
    void* detour;
};
struct FakeHooks {
    std::vector<Created> calls;
    bool refuse = false;
    uintptr_t next_trampoline = 0x9000;
    CreateHookFn fn() {
        return [this](uintptr_t target, void* detour, void** original) {
            if (refuse) return false;
            calls.push_back({target, detour});
            if (original) *original = reinterpret_cast<void*>(next_trampoline++);
            return true;
        };
    }
};
void* fake(uintptr_t v) { return reinterpret_cast<void*>(v); }
}  // namespace

TEST_CASE("hook table: the first hook patches the target; later hooks on it chain onto the newest detour") {
    FakeHooks f;
    HookTable t(f.fn());
    t.set_stub_maker([](void* d) { return d; });  // the chaining logic alone (test_hook_chain_real covers stubs)
    void* o1 = nullptr;
    void* o2 = nullptr;
    void* o3 = nullptr;
    std::string why;
    CHECK(t.install("emf", "SimStep", 0x1000, fake(0xA000), &o1, &why) == HookTable::Result::Ok);
    CHECK(t.install("mod_a", "my_sim", 0x1000, fake(0xB000), &o2, &why) == HookTable::Result::Ok);
    CHECK(t.install("mod_b", "their_sim", 0x1000, fake(0xC000), &o3, &why) == HookTable::Result::Ok);
    REQUIRE(f.calls.size() == 3);
    CHECK(f.calls[0].patched == 0x1000);   // the game function
    CHECK(f.calls[1].patched == 0xA000);   // EMF's hook (its stub; here the detour itself)
    CHECK(f.calls[2].patched == 0xB000);   // mod_a's hook: mod_b runs first, then mod_a, EMF, the game
    CHECK(o1 == fake(0x9000));
    CHECK(o3 == fake(0x9002));
    const auto list = t.list();
    REQUIRE(list.size() == 3);
    CHECK(list[1].owner == "mod_a");
    CHECK(list[1].name == "my_sim");
    CHECK(list[1].target == 0x1000);
    CHECK(list[1].hooked == 0xA000);
    CHECK(list[2].hooked == 0xB000);

    CHECK(t.install("mod_c", "other", 0x2000, fake(0xD000), nullptr, &why) == HookTable::Result::Ok);
    CHECK(f.calls.back().patched == 0x2000);  // another target: not chained
}

TEST_CASE("hook table: refusals change nothing (MinHook refused, not code, null detour, the same detour twice)") {
    FakeHooks f;
    HookTable t(f.fn());
    t.set_stub_maker([](void* d) { return d; });  // the chaining logic alone (test_hook_chain_real covers stubs)
    t.set_code_check([](uintptr_t a) { return a != 0x5555; });
    std::string why;
    CHECK(t.install("m", "x", 0x5555, fake(0xA000), nullptr, &why) == HookTable::Result::NotCode);
    CHECK(why.find("not game code") != std::string::npos);
    CHECK(t.install("m", "x", 0x1000, nullptr, nullptr, &why) == HookTable::Result::Refused);
    CHECK(t.install("m", "x", 0, fake(0xA000), nullptr, &why) == HookTable::Result::Refused);
    f.refuse = true;
    CHECK(t.install("m", "x", 0x1000, fake(0xA000), nullptr, &why) == HookTable::Result::Refused);
    CHECK(why.find("MinHook") != std::string::npos);
    CHECK(t.list().empty());
    f.refuse = false;
    CHECK(t.install("m", "x", 0x1000, fake(0xA000), nullptr, &why) == HookTable::Result::Ok);
    CHECK(t.install("m", "x2", 0x1000, fake(0xA000), nullptr, &why) == HookTable::Result::Refused);  // same detour
    CHECK(why.find("already") != std::string::npos);
    CHECK(t.list().size() == 1);
}

TEST_CASE("hook table: a refusal carries the reason the hook maker gave (MinHook's status), not one generic text") {
    HookTable t([](uintptr_t target, void*, void**, std::string* why) {
        if (why) *why = target == 0x1000 ? "MH_ERROR_UNSUPPORTED_FUNCTION (too short)" : "MH_ERROR_MEMORY_ALLOC";
        return false;
    });
    t.set_stub_maker([](void* d) { return d; });
    std::string why;
    CHECK(t.install("m", "x", 0x1000, fake(0xA000), nullptr, &why) == HookTable::Result::Refused);
    CHECK(why.find("MinHook refused to hook 0x1000") != std::string::npos);
    CHECK(why.find("MH_ERROR_UNSUPPORTED_FUNCTION") != std::string::npos);
    CHECK(t.install("m", "y", 0x2000, fake(0xB000), nullptr, &why) == HookTable::Result::Refused);
    CHECK(why.find("MH_ERROR_MEMORY_ALLOC") != std::string::npos);
    CHECK(why.find("too short") == std::string::npos);
    CHECK(t.list().empty());
}
