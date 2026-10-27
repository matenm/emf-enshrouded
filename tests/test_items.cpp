#include <windows.h>

#include <doctest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/fnv.h"
#include "common/offsets.h"
#include "core/items.h"
#include "fake_world.h"

using namespace emf::core;
namespace off = emf::off;

using FakeRegistry = emf::test::FakeItemRegistry;

TEST_CASE("items: an item id is FNV-1a-32 over the 16 raw GUID bytes, and 0 for a null GUID") {
    uint8_t g[16];
    for (int i = 0; i < 16; ++i) g[i] = static_cast<uint8_t>(i * 7 + 1);
    CHECK(item_id_from_guid(g) == emf::fnv1a32(g, 16));
    const uint8_t zero[16] = {};
    CHECK(item_id_from_guid(zero) == 0);
}

TEST_CASE("items: list, by id and by name read the registry's item map") {
    FakeRegistry r;
    r.add(0x11, 10, 20, "Wood_Log");
    r.add(0x22, 40, 1, "Torch");
    r.add(0x33, 70, 99, nullptr);
    const auto all = list_items(r.reg);
    CHECK(all.size() == 3);
    const auto torch = item_by_id(r.reg, 0x22);
    REQUIRE(torch.has_value());
    CHECK(torch->name == "Torch");
    CHECK(torch->max_stack == 1);
    CHECK(torch->guid[0] == 40);
    CHECK(torch->guid[15] == 55);
    CHECK_FALSE(item_by_id(r.reg, 0x44).has_value());
    CHECK(item_by_id(r.reg, 0x33)->name.empty());
    REQUIRE(item_by_name(r.reg, "Wood_Log").has_value());
    CHECK(item_by_name(r.reg, "Wood_Log")->id == 0x11);
    REQUIRE(item_by_name(r.reg, "wood_log").has_value());  // case-insensitive fallback
    CHECK(item_by_name(r.reg, "wood_log")->id == 0x11);
    CHECK_FALSE(item_by_name(r.reg, "Stone").has_value());
    CHECK_FALSE(item_by_name(r.reg, "").has_value());
}

TEST_CASE("items: an exact name wins over a case-insensitive one") {
    FakeRegistry r;
    r.add(0x01, 0, 1, "torch");
    r.add(0x02, 0, 1, "Torch");
    CHECK(item_by_name(r.reg, "Torch")->id == 0x02);
    CHECK(item_by_name(r.reg, "torch")->id == 0x01);
}

TEST_CASE("items: broken registries and names give empty results, never a fault") {
    CHECK(list_items(nullptr).empty());
    FakeRegistry empty;
    CHECK(list_items(empty.reg).empty());
    CHECK_FALSE(item_by_id(empty.reg, 1).has_value());
    FakeRegistry r;
    r.add(0x05, 0, 1, "Bad", true);
    REQUIRE(item_by_id(r.reg, 0x05).has_value());
    CHECK(item_by_id(r.reg, 0x05)->name.empty());  // unprintable debug name dropped
    r.vals[0x05 & 7] = reinterpret_cast<const uint8_t*>(0x10);  // a bad ItemInfo pointer
    CHECK_FALSE(item_by_id(r.reg, 0x05).has_value());
    CHECK(list_items(r.reg).empty());
}

TEST_CASE("items: known answer from the game: Material_T1_Wood_Logs") {
    // GUID text from the game's item list; item id from the same run's PickupEvent (Windows GUID byte order).
    const uint8_t guid[16] = {0xae, 0x93, 0x40, 0x3f, 0xed, 0x75, 0x5d, 0x4e,
                              0xbd, 0x0b, 0x78, 0x4b, 0xa8, 0x3b, 0x88, 0xf5};
    CHECK(item_id_from_guid(guid) == 0x2b0cf1d5u);
}

// ---- item definition writes ----
namespace {
// Moves an item's ItemInfo into a read-only page (as if the game's data were not writable).
uint8_t* make_read_only(FakeRegistry& r, uint32_t id) {
    for (int slot = 0; slot < 8; ++slot) {
        if (r.keys[slot] != id || !(r.bits[0] >> slot & 1)) continue;
        auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        std::memcpy(page, r.vals[slot], off::kItemInfoSize);
        std::memset(page + off::kItemInfoDebugName, 0, 8);  // the name is not copied
        DWORD old = 0;
        VirtualProtect(page, 0x1000, PAGE_READONLY, &old);
        r.vals[slot] = page;
        return page;
    }
    return nullptr;
}
}  // namespace

TEST_CASE("items: set_max_stack changes the stack size the registry reports") {
    FakeRegistry r;
    r.add(0x11, 10, 250, "Wood_Log");
    r.add(0x22, 40, 1, "Torch");
    CHECK(item_data_ptr(r.reg, 0x11) != nullptr);
    CHECK(item_data_ptr(r.reg, 0x44) == nullptr);
    CHECK(set_max_stack(r.reg, 0x11, 1000));
    CHECK(item_by_id(r.reg, 0x11)->max_stack == 1000);
    CHECK(item_by_id(r.reg, 0x22)->max_stack == 1);
}

TEST_CASE("items: set_max_stack refuses 0, unknown items and a null registry") {
    FakeRegistry r;
    r.add(0x11, 10, 250, "Wood_Log");
    CHECK_FALSE(set_max_stack(r.reg, 0x11, 0));
    CHECK(item_by_id(r.reg, 0x11)->max_stack == 250);
    CHECK_FALSE(set_max_stack(r.reg, 0x44, 10));
    CHECK_FALSE(set_max_stack(nullptr, 0x11, 10));
}

TEST_CASE("items: data the game made read-only (like its resource memory) is written, and stays read-only after") {
    FakeRegistry r;
    r.add(0x22, 40, 20, "Stone");
    uint8_t* page = make_read_only(r, 0x22);
    REQUIRE(page != nullptr);
    CHECK(set_max_stack(r.reg, 0x22, 99));
    CHECK(item_by_id(r.reg, 0x22)->max_stack == 99);
    MEMORY_BASIC_INFORMATION mbi{};
    REQUIRE(VirtualQuery(page, &mbi, sizeof(mbi)));
    CHECK((mbi.Protect & 0xff) == PAGE_READONLY);
    VirtualFree(page, 0, MEM_RELEASE);
}

TEST_CASE("write_game_memory refuses code, no-access and unmapped memory") {
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    REQUIRE(page != nullptr);
    const uint32_t v = 0x12345678;
    CHECK(write_game_memory(page, &v, 4));
    DWORD old = 0;
    VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old);
    CHECK_FALSE(write_game_memory(page, &v, 4));  // code: never
    VirtualProtect(page + 0x1000, 0x1000, PAGE_NOACCESS, &old);
    CHECK_FALSE(write_game_memory(page + 0x1000, &v, 4));
    CHECK_FALSE(write_game_memory(page + 0xffe, &v, 4));  // straddles into code / no-access pages
    VirtualFree(page, 0, MEM_RELEASE);
    CHECK_FALSE(write_game_memory(page, &v, 4));  // released
    CHECK_FALSE(write_game_memory(nullptr, &v, 4));
}

// ---- write_game_memory speed and scope ----
namespace {
const int kImageConstant = 0x5eed;  // lives in the test exe's read-only .rdata
}

TEST_CASE("write_game_memory: thousands of writes into a large read-only region stay fast") {
    const size_t kRegion = size_t(256) << 20;  // the game's resource region is ~400 MB; one VirtualQuery scans it all
    auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRegion, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    REQUIRE(base != nullptr);
    DWORD old = 0;
    REQUIRE(VirtualProtect(base, kRegion, PAGE_READONLY, &old));
    const ULONGLONG t0 = GetTickCount64();
    bool all = true;
    for (int i = 0; i < 1000; ++i) {
        const uint16_t v = static_cast<uint16_t>(i);
        all &= write_game_memory(base + size_t(i) * 0x10000 + 0x14, &v, 2);
    }
    const ULONGLONG ms = GetTickCount64() - t0;
    CHECK(all);
    CHECK(ms < 150);
    uint16_t back = 0;
    std::memcpy(&back, base + size_t(999) * 0x10000 + 0x14, 2);
    CHECK(back == 999);
    MEMORY_BASIC_INFORMATION mbi{};
    REQUIRE(VirtualQuery(base + 0x10000, &mbi, sizeof(mbi)));
    CHECK((mbi.Protect & 0xff) == PAGE_READONLY);
    VirtualFree(base, 0, MEM_RELEASE);
}

TEST_CASE("write_game_memory unlocks only private memory: image constants and file views stay untouched") {
    const int v = 1;
    CHECK_FALSE(write_game_memory(const_cast<int*>(&kImageConstant), &v, sizeof(v)));
    CHECK(kImageConstant == 0x5eed);
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 0x1000, nullptr);
    REQUIRE(map != nullptr);
    void* view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0x1000);
    REQUIRE(view != nullptr);
    CHECK_FALSE(write_game_memory(view, &v, sizeof(v)));
    UnmapViewOfFile(view);
    CloseHandle(map);
}
