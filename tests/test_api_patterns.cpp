// API v14: find_pattern, hook_pattern, hook_address, hook (v1) through the hook table, game_build.
#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/api.h"
#include "core/api_sizes.h"
#include "emf/emf.hpp"

using namespace emf::core;

namespace {
std::vector<std::string> g_log;
struct Made {
    uintptr_t target;
    void* detour;
};
std::vector<Made> g_made;

struct Fixture {
    CoreState state;
    std::vector<uint8_t> image = std::vector<uint8_t>(0x2000, 0xCC);
    Fixture() {
        state.log_line = [](const std::string& s) { g_log.push_back(s); };
        state.now_ms = [] { return uint64_t{1000}; };
        const auto base = reinterpret_cast<uintptr_t>(image.data());
        state.exe.base = base;
        state.exe.size = image.size();
        state.exe.sections = {{".text", base + 0x1000, 0x800}, {".rdata", base + 0x1800, 0x800}};
        state.exe.sha256 = std::string(64, 'a');
        state.exe.build_number = 1273043;
        state.known_build = true;
        state.hooks = std::make_unique<HookTable>([](uintptr_t t, void* d, void** o) {
            g_made.push_back({t, d});
            if (o) *o = reinterpret_cast<void*>(0x7777);
            return true;
        });
        set_core_state(&state);
        g_log.clear();
        g_made.clear();
    }
    ~Fixture() { set_core_state(nullptr); }
    uintptr_t at(size_t off) const { return reinterpret_cast<uintptr_t>(image.data()) + off; }
    void put(size_t off, std::initializer_list<uint8_t> b) {
        for (uint8_t x : b) image[off++] = x;
    }
};

EmfPattern pat(const char* bytes, const char* section = nullptr, int32_t offset = 0, uint32_t follow = EMF_FOLLOW_NONE) {
    EmfPattern p{};
    p.size = sizeof p;
    p.bytes = bytes;
    p.section = section;
    p.offset = offset;
    p.follow = follow;
    return p;
}
bool logged(const char* needle) {
    for (const auto& l : g_log)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}
void detour_a() {}
void detour_b() {}
}  // namespace

TEST_CASE("api v14: find_pattern searches the exe's .text (or a named section), uniquely; refusals are logged") {
    Fixture f;
    f.put(0x1100, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57});
    f.put(0x1900, {0x10, 0x20, 0x30, 0x40});
    const EmfApi* a = api_table();
    EmfPattern p = pat("48 89 5C 24 08 57");
    CHECK(a->find_pattern(&p) == f.at(0x1100));
    p.offset = 5;
    CHECK(a->find_pattern(&p) == f.at(0x1105));
    EmfPattern data = pat("10 20 30 40");
    CHECK(a->find_pattern(&data) == 0);  // not in .text
    CHECK(logged("not found"));
    data.section = ".rdata";
    CHECK(a->find_pattern(&data) == f.at(0x1900));
    EmfPattern nosuch = pat("10 20", ".nope");
    CHECK(a->find_pattern(&nosuch) == 0);
    CHECK(logged(".nope"));
    EmfPattern short_size = pat("10 20");
    short_size.size = 4;
    CHECK(a->find_pattern(&short_size) == 0);
    CHECK(a->find_pattern(nullptr) == 0);
}

TEST_CASE("api sizes: find_pattern takes the v14 size (the end of rel32_tail, not sizeof); game_build likewise") {
    Fixture f;
    f.put(0x1100, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57});
    const EmfApi* a = api_table();
    EmfPattern p = pat("48 89 5C 24 08 57");
    p.size = kPatternV14Size;  // sizeof adds padding, and grows when a field is appended
    CHECK(a->find_pattern(&p) == f.at(0x1100));
    p.size = kPatternV14Size - 1;
    g_log.clear();
    CHECK(a->find_pattern(&p) == 0);
    CHECK(logged("size field is too small"));

    alignas(8) uint8_t buf[sizeof(EmfGameBuild) + 8];  // an output struct: nothing is written past its size
    std::memset(buf, 0xAB, sizeof buf);
    auto* b = reinterpret_cast<EmfGameBuild*>(buf);
    b->size = kGameBuildV14Size;
    REQUIRE(a->game_build(b));
    CHECK(b->text_size == 0x800);
    for (size_t i = kGameBuildV14Size; i < sizeof buf; ++i) CHECK(buf[i] == 0xAB);
    b->size = kGameBuildV14Size - 1;
    CHECK_FALSE(a->game_build(b));
}

TEST_CASE("api v14: hook_pattern and hook_address install through the table with the calling mod as owner") {
    Fixture f;
    f.put(0x1200, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20});
    const EmfApi* a = api_table();
    void* original = nullptr;
    EmfPattern p = pat("40 53 48 83 EC 20");
    REQUIRE(a->hook_pattern("my_hook", &p, reinterpret_cast<void*>(&detour_a), &original));
    REQUIRE(g_made.size() == 1);
    CHECK(g_made[0].target == f.at(0x1200));
    CHECK(original == reinterpret_cast<void*>(0x7777));
    auto list = f.state.hooks->list();
    REQUIRE(list.size() == 1);
    CHECK(list[0].name == "my_hook");
    CHECK(list[0].owner == "emf_tests");  // the module that called the API

    REQUIRE(a->hook_address("second", reinterpret_cast<void*>(f.at(0x1200)), reinterpret_cast<void*>(&detour_b), nullptr));
    REQUIRE(g_made.size() == 2);
    CHECK(g_made[1].target == reinterpret_cast<uintptr_t>(f.state.hooks->list()[0].stub));  // chained onto the first hook's stub

    EmfPattern missing = pat("DE AD BE EF");
    CHECK_FALSE(a->hook_pattern("nope", &missing, reinterpret_cast<void*>(&detour_a), nullptr));
    CHECK(logged("hook_pattern"));
    CHECK_FALSE(a->hook_address("null", nullptr, reinterpret_cast<void*>(&detour_a), nullptr));
}

TEST_CASE("api v14: hook by signature name (v1) goes through the table too") {
    Fixture f;
    f.state.sigs[static_cast<size_t>(emf::Sig::GameLog)] = emf::Resolved{f.at(0x1300), 1, true};
    const EmfApi* a = api_table();
    REQUIRE(a->hook("GameLog", reinterpret_cast<void*>(&detour_a), nullptr));
    const auto list = f.state.hooks->list();
    REQUIRE(list.size() == 1);
    CHECK(list[0].name == "GameLog");
    CHECK(list[0].target == f.at(0x1300));
}

TEST_CASE("api v14: game_build tells the build, its hash, where the exe is; has_v14 checks the size") {
    Fixture f;
    const EmfApi* a = api_table();
    EmfGameBuild b{};
    b.size = sizeof b;
    REQUIRE(a->game_build(&b));
    CHECK(b.number == 1273043);
    CHECK(b.known == 1);
    CHECK(b.server == 0);
    CHECK(std::string(b.sha256) == std::string(64, 'a'));
    CHECK(b.module_base == f.at(0));
    CHECK(b.text_rva == 0x1000);
    CHECK(b.text_size == 0x800);
    EmfGameBuild tiny{};
    tiny.size = 8;
    CHECK_FALSE(a->game_build(&tiny));

    emf::api_ref() = a;
    CHECK(emf::has_v14());
    EmfApi old = *a;
    old.size = static_cast<uint32_t>(offsetof(EmfApi, find_pattern));
    emf::api_ref() = &old;
    CHECK_FALSE(emf::has_v14());
    emf::api_ref() = nullptr;
}

TEST_CASE("emf.hpp v14: find_pattern and hook_pattern helpers") {
    Fixture f;
    f.put(0x1400, {0x55, 0x48, 0x8B, 0xEC});
    REQUIRE(emf::init(api_table()));
    CHECK(emf::find_pattern("55 48 8B EC") == f.at(0x1400));
    void* original = nullptr;
    CHECK(emf::hook_pattern("helper_hook", "55 48 8B EC", reinterpret_cast<void*>(&detour_a), &original));
    CHECK(emf::game_build().has_value());
    emf::api_ref() = nullptr;
}

TEST_CASE("api v14: patterns match the exe as it is on disk, so a hooked function (first bytes now a jump) is found") {
    Fixture f;
    std::vector<uint8_t> disk = f.image;  // the exe file's bytes
    const uint8_t start[] = {0x40, 0x55, 0x53, 0x56, 0x41, 0x55, 0x48, 0x8D};
    std::memcpy(&disk[0x1500], start, sizeof start);
    std::memcpy(&f.image[0x1500], start, sizeof start);
    f.image[0x1500] = 0xE9;  // live: a hook's jump over the first five bytes
    f.image[0x1501] = 0x11;
    f.image[0x1502] = 0x22;
    f.image[0x1503] = 0x33;
    f.image[0x1504] = 0x44;
    f.state.exe.pristine_base = reinterpret_cast<uintptr_t>(disk.data());
    const EmfApi* a = api_table();
    EmfPattern p = pat("40 55 53 56 41 55 48 8D");
    CHECK(a->find_pattern(&p) == f.at(0x1500));  // the live address
    void* original = nullptr;
    CHECK(a->hook_pattern("chained", &p, reinterpret_cast<void*>(&detour_a), &original));
    CHECK(g_made.back().target == f.at(0x1500));
}
