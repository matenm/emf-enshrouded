#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "common/fnv.h"
#include "common/keen_map.h"
#include "common/offsets.h"
#include "common/world_view.h"
#include "fake_world.h"

using namespace emf;

using emf::test::FakeWorld;
using emf::test::MapStore;

namespace {
void* g_component_value = reinterpret_cast<void*>(0x1234);

void* fake_get_component(void* out, void*, uint32_t hash, uint32_t entity) {
    auto o = static_cast<uint64_t*>(out);
    o[0] = (hash == 7 && entity == 42) ? reinterpret_cast<uint64_t>(g_component_value) : 0;
    o[1] = o[0] ? 136 : 0;
    return out;
}

void* crashing_get_component(void* out, void*, uint32_t, uint32_t) {
    *static_cast<volatile int*>(nullptr) = 1;
    return out;
}
}  // namespace

TEST_CASE("WorldView on a zeroed world returns nothing everywhere") {
    FakeWorld w;
    WorldView v(w.p());
    CHECK(v.state() == 0);
    CHECK(v.kind() == 0);
    CHECK(v.tick() == 0);
    CHECK(v.entity_count() == 0);
    CHECK(v.singleton_count() == 0);
    CHECK(v.singleton("LocalPlayerData") == nullptr);
    int calls = 0;
    v.for_each_entity([&](uint32_t) { ++calls; });
    CHECK(calls == 0);
}

TEST_CASE("WorldView on an invalid pointer does not crash") {
    WorldView v(reinterpret_cast<uint8_t*>(0x10));
    CHECK(v.kind() == 0);
    CHECK(v.tick() == 0);
    CHECK(v.singleton("LocalPlayerData") == nullptr);
    CHECK(v.entity_ids().empty());
}

TEST_CASE("WorldView reads kind, tick, tick length and state at the engine offsets") {
    FakeWorld w;
    w.put<uint8_t>(off::kState, 2);
    w.put<uint8_t>(off::kKind, 2);
    w.put<uint32_t>(off::kTick, 4551);
    w.put<int64_t>(off::kTickLengthNs, 33333333);
    WorldView v(w.p());
    CHECK(v.state() == 2);
    CHECK(v.kind() == static_cast<uint8_t>(WorldKind::Client));
    CHECK(v.tick() == 4551);
    CHECK(v.tick_length_ns() == 33333333);
    CHECK(WorldView::from_storage(w.p() + off::kStorage).raw() == w.p());
}

TEST_CASE("singleton() finds an object by FNV-1a name through the singleton map") {
    FakeWorld w;
    MapStore ms;
    ms.attach(w, off::kSingletonMap, true);
    const uint32_t h = fnv1a32("LocalPlayerData");
    const size_t slot = h & 7;
    ms.vals2[slot] = 1;  // entry index 1
    ms.put(w, off::kSingletonMap, slot, h);
    alignas(8) uint8_t entries[2 * off::kSingletonEntrySize] = {};
    void* planted = reinterpret_cast<void*>(0xABCDEF);
    std::memcpy(entries + off::kSingletonEntrySize + off::kSingletonEntryHash, &h, 4);
    std::memcpy(entries + off::kSingletonEntrySize + off::kSingletonEntryObj, &planted, 8);
    w.put<const void*>(off::kSingletonArray, entries);
    w.put<uint64_t>(off::kSingletonCount, 2);
    WorldView v(w.p());
    CHECK(v.singleton_count() == 2);
    CHECK(v.singleton("LocalPlayerData") == planted);
    CHECK(v.singleton("ServerPlayers") == nullptr);
}

TEST_CASE("singleton() rejects an entry whose stored hash does not match") {
    FakeWorld w;
    MapStore ms;
    ms.attach(w, off::kSingletonMap, true);
    const uint32_t h = fnv1a32("LocalPlayerData");
    ms.vals2[h & 7] = 0;
    ms.put(w, off::kSingletonMap, h & 7, h);
    alignas(8) uint8_t entries[off::kSingletonEntrySize] = {};  // stored hash stays 0
    void* planted = reinterpret_cast<void*>(0xABCDEF);
    std::memcpy(entries + off::kSingletonEntryObj, &planted, 8);
    w.put<const void*>(off::kSingletonArray, entries);
    w.put<uint64_t>(off::kSingletonCount, 1);
    CHECK(WorldView(w.p()).singleton("LocalPlayerData") == nullptr);
}

TEST_CASE("entity_ids() returns exactly the ids in the entity map") {
    FakeWorld w;
    MapStore ms;
    ms.attach(w, off::kEntityMap, false);
    for (uint32_t id : {2u, 3u, 100u}) {
        size_t slot = KeenMapView::mix(id) & 7;
        while (ms.bits[0] >> slot & 1) slot = (slot + 1) & 7;
        ms.put(w, off::kEntityMap, slot, id);
    }
    WorldView v(w.p());
    CHECK(v.entity_count() == 3);
    auto ids = v.entity_ids();
    std::sort(ids.begin(), ids.end());
    CHECK(ids == std::vector<uint32_t>{2, 3, 100});
}

TEST_CASE("get_component passes through the game result and survives a crash") {
    FakeWorld w;
    auto r = get_component(fake_get_component, w.p(), 7, 42);
    CHECK(r.ptr == g_component_value);
    CHECK(r.size == 136);
    CHECK(get_component(fake_get_component, w.p(), 7, 43).ptr == nullptr);
    auto bad = get_component(crashing_get_component, w.p(), 7, 42);
    CHECK(bad.ptr == nullptr);
    CHECK(bad.size == 0);
    CHECK(get_component(nullptr, w.p(), 7, 42).ptr == nullptr);
}

// ---- template components ----
namespace {
uint8_t g_tpl_data[16] = {1, 2, 3};
void* fake_get_template(uint8_t*, uint32_t hash, uint32_t entity) {
    return hash == 7 && entity == 42 ? g_tpl_data : nullptr;
}
void* crashing_get_template(uint8_t*, uint32_t, uint32_t) {
    *static_cast<volatile int*>(nullptr) = 1;
    return nullptr;
}
}  // namespace

TEST_CASE("get_template_component passes through the game result and survives a crash or a missing function") {
    uint8_t world[64] = {};
    CHECK(get_template_component(fake_get_template, world, 7, 42) == g_tpl_data);
    CHECK(get_template_component(fake_get_template, world, 7, 43) == nullptr);
    CHECK(get_template_component(crashing_get_template, world, 7, 42) == nullptr);
    CHECK(get_template_component(nullptr, world, 7, 42) == nullptr);
    CHECK(get_template_component(fake_get_template, nullptr, 7, 42) == nullptr);
}

TEST_CASE("has_dynamic_component looks a hash up in the registry's per-entity component map") {
    FakeWorld w;
    emf::test::FakeRegistry reg;
    reg.attach(w, {0x027ac564u /* CurrentTransform */, 0x9e65507au /* DynamicActiveNpcState */});
    WorldView v(w.p());
    CHECK(v.has_dynamic_component(0x027ac564u) == std::optional<bool>(true));
    CHECK(v.has_dynamic_component(0x9e65507au) == std::optional<bool>(true));
    CHECK(v.has_dynamic_component(0x1a8b5909u) == std::optional<bool>(false));  // ActiveNpcState: template only
    FakeWorld bare;  // no registry: cannot tell
    CHECK_FALSE(WorldView(bare.p()).has_dynamic_component(0x027ac564u).has_value());
}
