#include <doctest.h>
#include <cstring>
#include "common/keen_map.h"

namespace {
// Builds an 8-slot Keen map in plain memory.
struct FakeMap {
    alignas(8) uint8_t header[0x58] = {};
    uint64_t bits[1] = {};
    uint32_t keys[8] = {};
    uint64_t values[8] = {};

    FakeMap() {
        const uint64_t* b = bits; const uint32_t* k = keys; const uint64_t* v = values; uint64_t cap = 8;
        std::memcpy(header + 0x08, &b, 8);
        std::memcpy(header + 0x10, &cap, 8);
        std::memcpy(header + 0x28, &k, 8);
        std::memcpy(header + 0x40, &v, 8);
    }
    void put(size_t slot, uint32_t key, uint64_t value) {
        bits[0] |= 1ull << slot; keys[slot] = key; values[slot] = value;
        uint32_t c; std::memcpy(&c, header + 0x54, 4); ++c; std::memcpy(header + 0x54, &c, 4);
    }
};
}  // namespace

TEST_CASE("KeenMapView::mix matches the engine's entity-id mixing") {
    CHECK((emf::KeenMapView::mix(2) & 7) == 0);
    CHECK((emf::KeenMapView::mix(7777) & 7) == 0);
    CHECK((emf::KeenMapView::mix(3) & 7) == 5);
}

TEST_CASE("find with mixing follows linear probing on collisions") {
    FakeMap m;
    m.put(0, 2, 200);      // mix(2) -> slot 0
    m.put(1, 7777, 7770);  // mix(7777) -> slot 0, probes to slot 1
    emf::KeenMapView v(m.header);
    CHECK(v.capacity() == 8);
    CHECK(v.count() == 2);
    CHECK(v.find(2, true) == 0);
    CHECK(v.find(7777, true) == 1);
    CHECK(*v.value<uint64_t>(1) == 7770);
    CHECK_FALSE(v.find(3, true).has_value());  // slot 5 is empty
}

TEST_CASE("find without mixing uses key & (capacity-1)") {
    FakeMap m;
    m.put(5, 13, 1300);  // 13 & 7 == 5
    emf::KeenMapView v(m.header);
    CHECK(v.find(13, false) == 5);
    CHECK(v.occupied(5));
    CHECK_FALSE(v.occupied(4));
    CHECK(v.key(5) == 13);
}

TEST_CASE("find on an empty map returns nothing") {
    FakeMap m;
    emf::KeenMapView v(m.header);
    CHECK_FALSE(v.find(2, true).has_value());
}
