// The loader holds the dedicated server at its entry point until EMF has started: the patch jumps to a stub once, the
// stub puts the original bytes back and calls the real entry point.
#include <doctest.h>

#include <windows.h>

#include <cstring>

#include "loader/entry_hold.h"

using emf::loader::EntryHold;

namespace {
EntryHold g_hold;
int g_stub_calls = 0;
using IntFn = int (*)();

int test_stub() {
    ++g_stub_calls;
    REQUIRE(emf::loader::release_entry(g_hold));
    return reinterpret_cast<IntFn>(g_hold.entry)();  // the original code again
}
}  // namespace

TEST_CASE("entry hold: the first call goes through the stub, which restores the code; later calls run it directly") {
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READ));
    REQUIRE(code);
    const uint8_t fn[16] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3,  // mov eax, 42; ret
                            0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    DWORD old = 0;
    REQUIRE(VirtualProtect(code, 16, PAGE_EXECUTE_READWRITE, &old));
    std::memcpy(code, fn, 16);
    REQUIRE(VirtualProtect(code, 16, PAGE_EXECUTE_READ, &old));

    g_hold = {};
    g_stub_calls = 0;
    REQUIRE(emf::loader::hold_entry(g_hold, code, reinterpret_cast<void*>(&test_stub)));
    CHECK(std::memcmp(code, fn, emf::loader::kEntryPatchSize) != 0);  // patched
    CHECK(reinterpret_cast<IntFn>(code)() == 42);
    CHECK(g_stub_calls == 1);
    CHECK(std::memcmp(code, fn, 16) == 0);  // restored byte for byte
    CHECK(reinterpret_cast<IntFn>(code)() == 42);
    CHECK(g_stub_calls == 1);
    CHECK_FALSE(emf::loader::release_entry(g_hold));  // nothing held any more
    VirtualFree(code, 0, MEM_RELEASE);
}

TEST_CASE("entry hold: only the dedicated server's exe is held") {
    CHECK(emf::loader::is_server_exe(L"C:\\Server\\enshrouded_server.exe"));
    CHECK(emf::loader::is_server_exe(L"D:\\x\\ENSHROUDED_SERVER.EXE"));
    CHECK(emf::loader::is_server_exe(L"enshrouded_server.exe"));
    CHECK_FALSE(emf::loader::is_server_exe(L"C:\\SteamLibrary\\steamapps\\common\\Enshrouded\\enshrouded.exe"));
    CHECK_FALSE(emf::loader::is_server_exe(L"C:\\x\\enshrouded_server.exe.bak"));
    CHECK_FALSE(emf::loader::is_server_exe(L"C:\\enshrouded_server.exe\\enshrouded.exe"));
}

namespace {
using ArgFn = uint64_t (*)(uint64_t);
uint64_t arg_stub(uint64_t arg) {
    REQUIRE(emf::loader::release_entry(g_hold));
    return reinterpret_cast<ArgFn>(g_hold.entry)(arg);
}
}  // namespace

TEST_CASE("entry hold: the entry point's argument (rcx) reaches it through the stub") {
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    REQUIRE(code);
    const uint8_t fn[16] = {0x48, 0x89, 0xC8, 0xC3,  // mov rax, rcx; ret
                            0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    std::memcpy(code, fn, 16);
    g_hold = {};
    REQUIRE(emf::loader::hold_entry(g_hold, code, reinterpret_cast<void*>(&arg_stub)));
    CHECK(reinterpret_cast<ArgFn>(code)(0x1234567890ull) == 0x1234567890ull);
    VirtualFree(code, 0, MEM_RELEASE);
}

TEST_CASE("entry hold: only MSVC's standard start-up (sub rsp,28h; call; add rsp,28h; jmp) is held") {
    const uint8_t msvc[18] = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x11, 0x22, 0x33, 0x44,
                              0x48, 0x83, 0xC4, 0x28, 0xE9, 0x55, 0x66, 0x77, 0x88};  // the server of 1273043
    CHECK(emf::loader::entry_is_msvc_startup(msvc));
    uint8_t other[18];
    std::memcpy(other, msvc, 18);
    other[13] = 0xC3;  // ret instead of the tail jump: something else
    CHECK_FALSE(emf::loader::entry_is_msvc_startup(other));
    other[13] = 0xE9;
    other[3] = 0x38;  // another frame size
    CHECK_FALSE(emf::loader::entry_is_msvc_startup(other));
    CHECK_FALSE(emf::loader::entry_is_msvc_startup(nullptr));
}

TEST_CASE("entry hold: when the server's entry point is not held, the loader's line says why (another tool's jump, or unknown code)") {
    using emf::loader::entry_jump_target;
    using emf::loader::entry_not_held_message;
    const auto no_read = [](uint64_t, uint64_t*) { return false; };
    // jmp rel32 (ShroudForge's winmm.dll parks the main thread at the entry point this way or with an absolute jump)
    uint8_t rel[16] = {0xE9, 0x10, 0x00, 0x00, 0x00};
    CHECK(entry_jump_target(rel, no_read) == reinterpret_cast<uint64_t>(rel) + 5 + 0x10);
    // mov rax, imm64; jmp rax (EMF's own hold looks like this) and mov r11, imm64; jmp r11
    uint8_t rax[16] = {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xE0};
    CHECK(entry_jump_target(rax, no_read) == 0x1122334455667788ull);
    uint8_t r11[16] = {0x49, 0xBB, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x41, 0xFF, 0xE3};
    CHECK(entry_jump_target(r11, no_read) == 0x0102030405060708ull);
    // jmp [rip+disp32]: the address is read from memory (safely: an unreadable one gives nothing)
    uint8_t ind[16] = {0xFF, 0x25, 0x00, 0x01, 0x00, 0x00};
    const uint64_t slot = reinterpret_cast<uint64_t>(ind) + 6 + 0x100;
    CHECK(entry_jump_target(ind, [slot](uint64_t at, uint64_t* out) { return at == slot && (*out = 0xABCDull, true); }) == 0xABCDull);
    CHECK(entry_jump_target(ind, no_read) == 0);
    const uint8_t code[16] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57};  // ordinary code: no jump
    CHECK(entry_jump_target(code, no_read) == 0);
    CHECK(entry_jump_target(nullptr, no_read) == 0);

    const std::string patched = entry_not_held_message(rel, 0x7ff812340000ull, "winmm.dll");
    CHECK(patched.find("the server's entry point is not the usual start-up code") == 0);
    CHECK(patched.find("another tool patched it: it jumps into winmm.dll") != std::string::npos);
    CHECK(patched.find("ShroudForge") != std::string::npos);
    CHECK(patched.find("mods may load after its world setup") != std::string::npos);
    CHECK(entry_not_held_message(rel, 0x7ff812340000ull, "").find("it jumps to 0x7ff812340000") != std::string::npos);
    const std::string unknown = entry_not_held_message(code, 0, "");
    CHECK(unknown.find("its first bytes are 48 89 5c 24 08 55 56 57") != std::string::npos);
    CHECK(unknown.find("a game build EMF does not know") != std::string::npos);
}

TEST_CASE("entry hold: EMF's thread starts first; the entry point is held only when it runs") {
    using emf::loader::HoldStart;
    int holds = 0;
    auto hold = [&] { return ++holds, true; };
    // CreateThread failed: no hold (a held server would wait the whole 20 s for a thread that never runs)
    CHECK(emf::loader::start_core(true, [] { return false; }, hold) == HoldStart::ThreadFailed);
    CHECK(holds == 0);
    CHECK(emf::loader::start_core(true, [] { return true; }, hold) == HoldStart::Held);
    CHECK(holds == 1);
    CHECK(emf::loader::start_core(false, [] { return true; }, hold) == HoldStart::NotWanted);  // the game exe
    CHECK(holds == 1);
    CHECK(emf::loader::start_core(true, [] { return true; }, [] { return false; }) == HoldStart::HoldFailed);
}

TEST_CASE("entry hold: the loader's line says how EMF's start-up ended") {
    using emf::loader::CoreOutcome;
    using emf::loader::hold_message;
    CHECK(hold_message(true, CoreOutcome::Started, 700) == "held the server's start 700 ms until emf_core started");
    const std::string missing = hold_message(true, CoreOutcome::Missing, 12);
    CHECK(missing.find("held the server's start 12 ms") == 0);
    CHECK(missing.find("until emf_core started") == std::string::npos);
    CHECK(missing.find("emf_core.dll was not found or failed to load") != std::string::npos);
    const std::string no_export = hold_message(true, CoreOutcome::NoExport, 12);
    CHECK(no_export.find("no emf_core_start") != std::string::npos);
    CHECK(no_export.find("until emf_core started") == std::string::npos);
    CHECK(hold_message(false, CoreOutcome::Running, 20000).find("emf_core is still starting") != std::string::npos);
}
