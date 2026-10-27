#include <doctest.h>

#include <string>
#include <vector>

#include "core/fault_report.h"
#include "core/save_events.h"

using namespace emf::core;

namespace {
std::vector<uint16_t> g_slots;
void record(uint16_t slot, void* user) { g_slots.push_back(static_cast<uint16_t>(slot + *static_cast<int*>(user))); }
void crash(uint16_t, void*) { *static_cast<volatile int*>(nullptr) = 1; }
}  // namespace

TEST_CASE("SaveEvents calls every subscriber with the slot id") {
    g_slots.clear();
    SaveEvents e;
    static int zero = 0, hundred = 100;
    e.subscribe(record, &zero);
    e.subscribe(record, &hundred);
    CHECK(e.fire(0x0001) == 0);
    CHECK(g_slots == std::vector<uint16_t>{1, 101});
}

TEST_CASE("SaveEvents disables a faulting subscriber and keeps the others") {
    g_slots.clear();
    SaveEvents e;
    static int zero = 0;
    e.subscribe(crash, nullptr);
    e.subscribe(record, &zero);
    CHECK(e.fire(7) == 1);
    CHECK(e.fire(8) == 0);
    CHECK(e.active() == 1);
    CHECK(g_slots == std::vector<uint16_t>{7, 8});
}

TEST_CASE("SaveEvents ignores null callbacks") {
    SaveEvents e;
    e.subscribe(nullptr, nullptr);
    CHECK(e.active() == 0);
    CHECK(e.fire(1) == 0);
}

TEST_CASE("a faulting save subscriber is reported, naming the event and its module") {
    static std::vector<std::string> faults;
    faults.clear();
    set_fault_reporter([](const std::string& s) { faults.push_back(s); });
    SaveEvents e("on_save callback");
    e.subscribe(crash, nullptr);
    CHECK(e.fire(1) == 1);
    set_fault_reporter(nullptr);
    REQUIRE(faults.size() == 1);
    CHECK(faults[0].find("on_save callback") != std::string::npos);
    CHECK(faults[0].find("emf_tests.exe") != std::string::npos);
}

TEST_CASE("LoadGuard fires once per completed load, also when the instance address is reused") {
    LoadGuard g;
    int a = 0, b = 0;
    CHECK_FALSE(g.on_step(&a, 1));  // still loading
    CHECK(g.on_step(&a, 0));        // loaded
    CHECK_FALSE(g.on_step(&a, 0));  // polled again: no second event
    CHECK_FALSE(g.on_step(&a, 1));  // next session reuses the instance address and waits again
    CHECK(g.on_step(&a, 0));
    g.reset();                      // a new server world was set up
    CHECK(g.on_step(&a, 0));        // load finished on the first poll, same address
    CHECK(g.on_step(&b, 0));        // a different instance
    CHECK_FALSE(g.on_step(nullptr, 0));
}

TEST_CASE("saved_world_slot reads the slot after a successful save, at the offsets the core read from the game's code") {
    std::vector<uint8_t> save2(0xB7400, 0);
    const uint32_t result_off = 0xB729C, slot_off = 0xB7297;  // build 1273043
    save2[slot_off] = 0x01;  // slot id 0x0001: Local, index 0
    save2[slot_off + 1] = 0x00;
    uint16_t slot = 0xFFFF;
    CHECK(saved_world_slot(save2.data(), result_off, slot_off, &slot));
    CHECK(slot == 0x0001);
    save2[result_off] = 3;  // the save failed: no event
    CHECK_FALSE(saved_world_slot(save2.data(), result_off, slot_off, &slot));
    CHECK_FALSE(saved_world_slot(nullptr, result_off, slot_off, &slot));
    CHECK_FALSE(saved_world_slot(save2.data(), 0, slot_off, &slot));  // offsets not found: never read
    CHECK_FALSE(saved_world_slot(save2.data(), result_off, 0, &slot));
}

TEST_CASE("loaded_world_slot reads the u16 slot just before the instance's slot flag (offset from the game's code)") {
    std::vector<uint8_t> inst(0x1800, 0);
    inst[0x1754] = 0x01;  // slot id 0x0001
    inst[0x1756] = 1;     // local world slot set
    uint16_t slot = 0xFFFF;
    CHECK(loaded_world_slot(inst.data(), 0x1756, &slot));
    CHECK(slot == 0x0001);
    CHECK_FALSE(loaded_world_slot(inst.data(), 0, &slot));  // flag offset not found: never read
    CHECK_FALSE(loaded_world_slot(inst.data(), 1, &slot));  // no room for a u16 before it
    CHECK_FALSE(loaded_world_slot(nullptr, 0x1756, &slot));
}
