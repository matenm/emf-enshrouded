#include <doctest.h>

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/fault_report.h"
#include "core/module_name.h"
#include "core/programs.h"
#include "fake_world.h"

using namespace emf::core;
using emf::test::FakeWorld;

namespace {
struct FakeProgram {
    std::string name;
    alignas(8) uint8_t desc[0x188] = {};
};

// Plants programs into the fake world's program table.
void plant_programs(FakeWorld& w, std::vector<FakeProgram>& progs) {
    for (size_t i = 0; i < progs.size(); ++i) {
        const uint32_t e = emf::off::kProgramTable + static_cast<uint32_t>(i) * emf::world_offsets().program_stride;
        w.put<const void*>(e + 0x00, progs[i].desc);
        w.put<uint16_t>(e + 0x08, static_cast<uint16_t>(i + 1));
        w.put<const char*>(e + 0x10, progs[i].name.c_str());
        w.put<uint64_t>(e + 0x18, progs[i].name.size());
        const uintptr_t fn = 0x140000000 + 0x1000 * (i + 1);
        std::memcpy(progs[i].desc + 0x10, &fn, 8);
    }
    w.put<uint16_t>(emf::world_offsets().program_count, static_cast<uint16_t>(progs.size()));
}

std::vector<std::string> g_events;
void pre_cb(void* ctx, void* user) { g_events.push_back(std::string("pre:") + static_cast<const char*>(user) + ":" + std::to_string(reinterpret_cast<uintptr_t>(ctx))); }
void post_cb(void*, void* user) { g_events.push_back(std::string("post:") + static_cast<const char*>(user)); }
void crash_cb(void*, void*) { *static_cast<volatile int*>(nullptr) = 1; }
void original_fn(void*) { g_events.push_back("original"); }
}  // namespace

TEST_CASE("list_programs and find_program read the world's program table") {
    FakeWorld w;
    std::vector<FakeProgram> progs(3);
    progs[0].name = "player_state";
    progs[1].name = "npc_active_behavior";
    progs[2].name = "death";
    plant_programs(w, progs);
    auto all = list_programs(w.p());
    REQUIRE(all.size() == 3);
    CHECK(all[1].name == "npc_active_behavior");
    ProgramInfo p = find_program(w.p(), "death");
    CHECK(p.desc == progs[2].desc);
    CHECK(p.update_fn == 0x140003000u);
    CHECK(find_program(w.p(), "nope").desc == nullptr);
}

TEST_CASE("list_programs walks the table with the start-up's count and stride (build 1283531: entries of 0x1F28 bytes)") {
    emf::WorldOffsets moved_offsets = emf::shifted_world_offsets(0xE179C0);
    moved_offsets.program_count = 0xE16F50;
    moved_offsets.program_stride = 0x1F28;
    emf::test::ScopedWorldOffsets moved(moved_offsets);
    FakeWorld w(0x1C00);
    std::vector<FakeProgram> progs(3);
    progs[0].name = "player_state";
    progs[1].name = "npc_active_behavior";
    progs[2].name = "death";
    plant_programs(w, progs);
    auto all = list_programs(w.p());
    REQUIRE(all.size() == 3);
    CHECK(all[2].name == "death");
    CHECK(find_program(w.p(), "death").desc == progs[2].desc);
}

TEST_CASE("list_programs is empty when the program table's layout is not known on this build") {
    FakeWorld w;
    std::vector<FakeProgram> progs(1);
    progs[0].name = "death";
    plant_programs(w, progs);
    emf::WorldOffsets unknown = emf::kBaseWorldOffsets;
    unknown.program_count = 0;
    unknown.program_stride = 0;
    emf::test::ScopedWorldOffsets none(unknown);
    CHECK(list_programs(w.p()).empty());
    CHECK(find_program(w.p(), "death").desc == nullptr);
}

TEST_CASE("list_programs is empty for an empty or invalid world") {
    FakeWorld w;
    CHECK(list_programs(w.p()).empty());
    CHECK(list_programs(reinterpret_cast<const uint8_t*>(0x10)).empty());
}

TEST_CASE("a claimed trampoline calls pre, the original, then post") {
    reset_program_hooks();
    g_events.clear();
    static char tag[] = "T";
    size_t slot = 99;
    void* detour = claim_program_hook(0x1000, pre_cb, post_cb, tag, &slot);
    REQUIRE(detour != nullptr);
    REQUIRE(slot < kMaxProgramHooks);
    *program_hook_original(slot) = reinterpret_cast<void*>(&original_fn);  // what MinHook would write
    reinterpret_cast<void (*)(void*)>(detour)(reinterpret_cast<void*>(7));
    CHECK(g_events == std::vector<std::string>{"pre:T:7", "original", "post:T"});
}

TEST_CASE("a faulting pre callback is disabled but the original still runs") {
    reset_program_hooks();
    g_events.clear();
    size_t slot = 0;
    void* detour = claim_program_hook(0x2000, crash_cb, nullptr, nullptr, &slot);
    REQUIRE(detour);
    *program_hook_original(slot) = reinterpret_cast<void*>(&original_fn);
    auto fn = reinterpret_cast<void (*)(void*)>(detour);
    fn(nullptr);
    fn(nullptr);
    CHECK(program_hook_slot(slot).faulted);
    CHECK(g_events == std::vector<std::string>{"original", "original"});
}

TEST_CASE("claim_program_hook refuses duplicates and a full pool") {
    reset_program_hooks();
    size_t slot = 0;
    REQUIRE(claim_program_hook(0x3000, pre_cb, nullptr, nullptr, &slot));
    CHECK(claim_program_hook(0x3000, pre_cb, nullptr, nullptr, &slot) == nullptr);
    for (uintptr_t i = 1; i < kMaxProgramHooks; ++i) CHECK(claim_program_hook(0x10000 + i, pre_cb, nullptr, nullptr, &slot));
    CHECK(claim_program_hook(0x99999, pre_cb, nullptr, nullptr, &slot) == nullptr);
}

TEST_CASE("a faulting program hook is reported once, naming the module of the callback") {
    reset_program_hooks();
    static std::vector<std::string> faults;
    faults.clear();
    set_fault_reporter([](const std::string& s) { faults.push_back(s); });
    size_t slot = 0;
    void* detour = claim_program_hook(0x4000, crash_cb, nullptr, nullptr, &slot);
    REQUIRE(detour);
    *program_hook_original(slot) = reinterpret_cast<void*>(&original_fn);
    auto fn = reinterpret_cast<void (*)(void*)>(detour);
    fn(nullptr);
    fn(nullptr);
    set_fault_reporter(nullptr);
    REQUIRE(faults.size() == 1);
    CHECK(faults[0].find("program hook") != std::string::npos);
    CHECK(faults[0].find("emf_tests.exe") != std::string::npos);
}

// The name came from the ANSI path: a mod file named with letters outside the code page was named wrongly (in hook
// owners and the log). Markers name their owner with module_name_of, which was UTF-8 already.
TEST_CASE("module_stem_of names a mod by its file name without the extension, in UTF-8") {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    const std::wstring tmp = dir + L"\\stem_test";
    CreateDirectoryW(tmp.c_str(), nullptr);
    const std::wstring copy = tmp + L"\\Fr\u00f6hlich \u03a9.dll";  // Froehlich Omega: not in one ANSI code page
    REQUIRE(CopyFileW((dir + L"\\dist\\emf\\mods\\herald\\herald.dll").c_str(), copy.c_str(), FALSE));
    HMODULE m = LoadLibraryW(copy.c_str());
    REQUIRE(m);
    const void* fn = reinterpret_cast<const void*>(GetProcAddress(m, "emf_mod_load"));
    REQUIRE(fn);
    CHECK(module_stem_of(fn) == "Fr\xc3\xb6hlich \xce\xa9");
    FreeLibrary(m);
    DeleteFileW(copy.c_str());
    CHECK(module_stem_of(reinterpret_cast<const void*>(&original_fn)) == "emf_tests");
    CHECK(module_stem_of(nullptr) == "unknown");
}

TEST_CASE("module_name_of names the module containing an address") {
    CHECK(module_name_of(reinterpret_cast<const void*>(&original_fn)) == "emf_tests.exe");
    CHECK(module_name_of(nullptr) == "unknown module");
}
