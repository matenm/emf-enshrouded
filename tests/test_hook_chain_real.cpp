// The hook table with real MinHook hooks on real functions in this test program: chains run in order, a detour used
// on two functions never carries a later hook onto the other one, and a hook inside another hook's patch is refused.
#include <windows.h>

#include <doctest.h>

#include <string>

#include "common/hooks.h"
#include "core/hook_table.h"

using emf::core::HookTable;

namespace {
std::string g_trace;

#pragma optimize("", off)
__declspec(noinline) int target_one(int x) {
    g_trace += "one;";
    return x * 2 + 1;
}
__declspec(noinline) int target_two(int x) {
    g_trace += "two;";
    return x * 3 + 2;
}
#pragma optimize("", on)

using Fn = int (*)(int);
Fn o_a1 = nullptr, o_a2 = nullptr, o_b = nullptr, o_c = nullptr;

// One detour used on two functions (what /OPT:ICF does to identical "turn it off" detours).
__declspec(noinline) int shared_detour(int x) {
    g_trace += "shared;";
    return x;  // the original is not called
}
__declspec(noinline) int detour_b(int x) {
    g_trace += "B;";
    return o_b(x);
}
__declspec(noinline) int detour_c(int x) {
    g_trace += "C;";
    return o_c(x) + 100;
}

// One table for this file's tests: real MinHook hooks stay for the whole process, like in the game.
HookTable& real_table() {
    static HookTable* t = [] {
        REQUIRE(emf::hooks_init());
        return new HookTable(  // what the core passes
            [](uintptr_t a, void* d, void** o, std::string* why) { return emf::hook_install(a, d, o, why); });
    }();
    return *t;
}
}  // namespace

TEST_CASE("hook chain (real MinHook): a detour shared by two functions never carries a later hook to the other") {
    HookTable& table = real_table();
    std::string why;
    const auto t1 = reinterpret_cast<uintptr_t>(&target_one);
    const auto t2 = reinterpret_cast<uintptr_t>(&target_two);
    REQUIRE(table.install("mod_a", "off1", t1, reinterpret_cast<void*>(&shared_detour), reinterpret_cast<void**>(&o_a1), &why) ==
            HookTable::Result::Ok);
    REQUIRE(table.install("mod_a", "off2", t2, reinterpret_cast<void*>(&shared_detour), reinterpret_cast<void**>(&o_a2), &why) ==
            HookTable::Result::Ok);
    REQUIRE(table.install("mod_b", "on1", t1, reinterpret_cast<void*>(&detour_b), reinterpret_cast<void**>(&o_b), &why) ==
            HookTable::Result::Ok);
    g_trace.clear();
    CHECK(target_one(5) == 5);
    CHECK(g_trace == "B;shared;");  // B runs first, then mod_a's detour
    g_trace.clear();
    CHECK(target_two(5) == 5);
    CHECK(g_trace == "shared;");  // B is not on target_two
}

TEST_CASE("hook chain (real MinHook): a hook a few bytes into another hook's patch is refused, not a crash") {
    HookTable& table = real_table();
    std::string why;
    const auto t1 = reinterpret_cast<uintptr_t>(&target_one);
    for (int off = 1; off < 16; ++off)
        CHECK(table.install("mod_c", "inside", t1 + off, reinterpret_cast<void*>(&detour_c), reinterpret_cast<void**>(&o_c), &why) ==
              HookTable::Result::Refused);
    CHECK(why.find("inside") != std::string::npos);
    g_trace.clear();
    target_one(1);  // still works
    CHECK(g_trace == "B;shared;");
}
