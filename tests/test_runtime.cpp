// Tests for the in-process runtime helpers: logger, hooks, sha256.
#include <doctest.h>
#include <windows.h>

#include <MinHook.h>

#include <fstream>
#include <sstream>
#include <string>

#include "common/hooks.h"
#include "common/log.h"
#include "common/sha256.h"
#include "test_scratch.h"

namespace {
std::string read_all(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

__declspec(noinline) int target_add(int a, int b) { return a + b; }
using AddFn = int (*)(int, int);
AddFn g_original_add = nullptr;
int detour_add(int a, int b) { return g_original_add(a, b) * 10; }
}  // namespace

TEST_CASE("logf before log_open does not crash") { emf::logf("nobody hears this %d", 1); }

TEST_CASE("logf writes formatted lines that are on disk immediately") {
    const std::wstring path = test_scratch::temp_path(L"emf_test_log", L".txt");
    DeleteFileW(path.c_str());
    emf::log_open(path);
    emf::logf("hello %s %d", "world", 42);
    emf::logf("second line");
    std::string text = read_all(path);  // read while the log is still open: flushed per line
    CHECK(text.find("hello world 42") != std::string::npos);
    CHECK(text.find("second line") != std::string::npos);
    CHECK(text.find("\n") != std::string::npos);
    emf::log_close();
    DeleteFileW(path.c_str());
}

TEST_CASE("hook_create + hooks_enable_all redirect a function and keep the original callable") {
    REQUIRE(emf::hooks_init());
    volatile AddFn f = target_add;
    CHECK(f(2, 3) == 5);
    REQUIRE(emf::hook_create(reinterpret_cast<uintptr_t>(&target_add), reinterpret_cast<void*>(&detour_add),
                             reinterpret_cast<void**>(&g_original_add)));
    REQUIRE(emf::hooks_enable_all());
    CHECK(f(2, 3) == 50);
    CHECK(g_original_add(2, 3) == 5);
    emf::hooks_shutdown();
    CHECK(f(2, 3) == 5);
}

TEST_CASE("hook_create refuses a null target") {
    REQUIRE(emf::hooks_init());
    void* orig = nullptr;
    CHECK_FALSE(emf::hook_create(0, reinterpret_cast<void*>(&detour_add), &orig));
    emf::hooks_shutdown();
}

TEST_CASE("sha256 matches known test vectors") {
    CHECK(emf::sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(emf::sha256_hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("sha256_file hashes a file and returns empty for a missing file") {
    const std::wstring path = test_scratch::temp_path(L"emf_test_sha", L".txt");
    {
        std::ofstream f(path, std::ios::binary);
        f << "abc";
    }
    CHECK(emf::sha256_file(path) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(emf::sha256_file(L"Z:\\definitely\\missing.bin").empty());
    DeleteFileW(path.c_str());
}

namespace {
__declspec(noinline) int target_mul(int a, int b) { return a * b; }
AddFn g_original_mul = nullptr;
int detour_mul(int a, int b) { return g_original_mul(a, b) + 1; }
}  // namespace

TEST_CASE("hook_enable switches on a single hook created after the others") {
    REQUIRE(emf::hooks_init());
    volatile AddFn f = target_mul;
    REQUIRE(emf::hook_create(reinterpret_cast<uintptr_t>(&target_mul), reinterpret_cast<void*>(&detour_mul),
                             reinterpret_cast<void**>(&g_original_mul)));
    CHECK(f(3, 4) == 12);  // created but not enabled yet
    REQUIRE(emf::hook_enable(reinterpret_cast<uintptr_t>(&target_mul)));
    CHECK(f(3, 4) == 13);
    emf::hooks_shutdown();
}

namespace {
#pragma optimize("", off)
__declspec(noinline) int target_sub(int a, int b) { return a * 31 - b; }
#pragma optimize("", on)
AddFn g_original_sub = nullptr;
int detour_sub(int a, int b) { return g_original_sub(a, b) - 1000; }
int g_not_code = 0;
}  // namespace

TEST_CASE("hook_install: a hook whose enable fails is removed again, so the function can be hooked later") {
    REQUIRE(emf::hooks_init());
    volatile AddFn f = target_sub;
    const auto t = reinterpret_cast<uintptr_t>(&target_sub);
    emf::HookCalls broken = emf::minhook_calls();  // MinHook itself, but enabling fails
    broken.enable = [](void*) { return static_cast<int>(MH_ERROR_MEMORY_PROTECT); };
    std::string why;
    CHECK_FALSE(emf::hook_install_with(broken, t, reinterpret_cast<void*>(&detour_sub),
                                       reinterpret_cast<void**>(&g_original_sub), &why));
    CHECK(why.find("MH_ERROR_MEMORY_PROTECT") != std::string::npos);
    CHECK(g_original_sub == nullptr);  // not left pointing at the freed trampoline
    CHECK(f(2, 1) == 61);              // not hooked
    // had the failed hook stayed created, MinHook would refuse this one (MH_ERROR_ALREADY_CREATED)
    REQUIRE(emf::hook_install(t, reinterpret_cast<void*>(&detour_sub), reinterpret_cast<void**>(&g_original_sub), &why));
    CHECK(f(2, 1) == -939);
    CHECK_FALSE(emf::hook_install(t, reinterpret_cast<void*>(&detour_sub), reinterpret_cast<void**>(&g_original_sub), &why));
    CHECK(why.find("MH_ERROR_ALREADY_CREATED") != std::string::npos);
    emf::hooks_shutdown();
}

TEST_CASE("hook_install: a refusal says which MinHook status it was") {
    REQUIRE(emf::hooks_init());
    void* orig = nullptr;
    std::string why;
    CHECK_FALSE(emf::hook_install(reinterpret_cast<uintptr_t>(&g_not_code), reinterpret_cast<void*>(&detour_add), &orig, &why));
    CHECK(why.find("MH_ERROR_NOT_EXECUTABLE") != std::string::npos);
    CHECK(why.find("not executable code") != std::string::npos);
    CHECK(emf::hook_status_text(MH_ERROR_UNSUPPORTED_FUNCTION).find("MH_ERROR_UNSUPPORTED_FUNCTION") != std::string::npos);
    CHECK(emf::hook_status_text(12345).find("12345") != std::string::npos);  // unknown to this MinHook
    emf::hooks_shutdown();
    CHECK_FALSE(emf::hook_install(reinterpret_cast<uintptr_t>(&target_sub), reinterpret_cast<void*>(&detour_sub), &orig, &why));
    CHECK(why.find("not initialized") != std::string::npos);
}
