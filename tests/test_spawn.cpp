#include <doctest.h>

#include <cstring>

#include "core/spawn.h"
#include "fake_world.h"

using namespace emf::core;
using emf::test::FakeWorld;

namespace {
void* g_world = nullptr;
uint8_t g_guid[16];
SpawnComponent g_comp{};
WorldTransformData g_xf{};
uint64_t g_count = 0;
uint32_t g_spawn_result = 6808;  // what the fake createEntity returns (0 = the game refused)
uint32_t fake_spawn(void* world, const void* guid, const SpawnSlice* comps) {
    g_world = world;
    std::memcpy(g_guid, guid, 16);
    g_count = comps->count;
    g_comp = comps->ptr[0];
    std::memcpy(&g_xf, comps->ptr[0].data, sizeof(g_xf));
    return g_spawn_result;
}
uint32_t crash_spawn(void*, const void*, const SpawnSlice*) {
    *static_cast<volatile int*>(nullptr) = 1;
    return 1;
}

void* g_storage = nullptr;
uint32_t g_destroyed = 0;
char g_flag = 0;
uint64_t fake_destroy(void* storage, uint32_t id, char flag, uint64_t) {
    g_storage = storage;
    g_destroyed = id;
    g_flag = flag;
    return 1;
}
const uint8_t kGuid[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
}  // namespace

TEST_CASE("meters_to_fixed is 32.32 fixed point with rounding") {
    CHECK(meters_to_fixed(1.0) == (int64_t(1) << 32));
    CHECK(meters_to_fixed(-2.5) == -(int64_t(5) << 31));
    CHECK(meters_to_fixed(3704.2) == static_cast<int64_t>(3704.2 * 4294967296.0 + 0.5));
}

TEST_CASE("spawn_entity passes the template GUID and a CurrentTransform spawn component") {
    FakeWorld w;
    w.put<uint8_t>(emf::off::kKind, 0);  // server
    g_spawn_result = 6808;
    REQUIRE(spawn_entity(fake_spawn, w.p(), kGuid, 10.0, 20.5, -3.0) == 6808u);  // the new entity id
    CHECK(g_world == w.p());
    CHECK(std::memcmp(g_guid, kGuid, 16) == 0);
    CHECK(g_count == 1);
    CHECK(g_comp.hash == 0x027ac564u);
    CHECK(g_comp.size == 56);
    CHECK(g_xf.x == meters_to_fixed(10.0));
    CHECK(g_xf.y == meters_to_fixed(20.5));
    CHECK(g_xf.z == meters_to_fixed(-3.0));
    CHECK(g_xf.qw == 1.0f);
    CHECK(g_xf.qx == 0.0f);
    CHECK(g_xf.sx == 1.0f);
    CHECK(g_xf.sz == 1.0f);
}

TEST_CASE("spawn_entity returns 0 for client worlds, missing arguments, a fault or a refused spawn") {
    g_spawn_result = 6808;
    FakeWorld client;
    client.put<uint8_t>(emf::off::kKind, 2);
    CHECK(spawn_entity(fake_spawn, client.p(), kGuid, 0, 0, 0) == 0u);
    FakeWorld server;
    CHECK(spawn_entity(nullptr, server.p(), kGuid, 0, 0, 0) == 0u);
    CHECK(spawn_entity(fake_spawn, server.p(), nullptr, 0, 0, 0) == 0u);
    CHECK(spawn_entity(crash_spawn, server.p(), kGuid, 0, 0, 0) == 0u);
    g_spawn_result = 0;  // e.g. unknown template GUID
    CHECK(spawn_entity(fake_spawn, server.p(), kGuid, 0, 0, 0) == 0u);
    g_spawn_result = 6808;
}

TEST_CASE("destroy_entity calls destroyEntity with the world's storage") {
    FakeWorld w;
    REQUIRE(destroy_entity(fake_destroy, w.p(), 4242));
    CHECK(g_storage == w.p() + emf::off::kStorage);
    CHECK(g_destroyed == 4242);
    CHECK(g_flag == 1);
    FakeWorld client;
    client.put<uint8_t>(emf::off::kKind, 2);
    CHECK_FALSE(destroy_entity(fake_destroy, client.p(), 1));
    CHECK_FALSE(destroy_entity(fake_destroy, w.p(), 0));
}

// ---- spawn with extra components, give a pickup ----
#include <string>
#include <vector>

#include "core/items.h"

namespace {
struct Captured {
    uint32_t hash;
    uint64_t size;
    std::vector<uint8_t> bytes;
};
std::vector<Captured> g_comps;
uint8_t g_ex_guid[16];
uint32_t capture_spawn(void*, const void* guid, const SpawnSlice* comps) {
    std::memcpy(g_ex_guid, guid, 16);
    g_comps.clear();
    for (uint64_t i = 0; i < comps->count; ++i) {
        const auto& c = comps->ptr[i];
        const auto* b = static_cast<const uint8_t*>(c.data);
        g_comps.push_back({c.hash, c.size, std::vector<uint8_t>(b, b + c.size)});
    }
    return 7001;
}
FakeWorld& server_world() {
    static FakeWorld w;
    w.put<uint8_t>(emf::off::kKind, 0);
    return w;
}
}  // namespace

TEST_CASE("spawn_entity_ex passes the transform first, then the extra components") {
    const uint32_t level = 5;
    const uint8_t blob[12] = {1, 2, 3};
    const SpawnComponent extra[2] = {{0x11111111u, 0, &level, 4}, {0x22222222u, 0, blob, 12}};
    std::string why;
    REQUIRE(spawn_entity_ex(capture_spawn, server_world().p(), kGuid, 1, 2, 3, extra, 2, &why) == 7001u);
    REQUIRE(g_comps.size() == 3);
    CHECK(g_comps[0].hash == 0x027ac564u);
    CHECK(g_comps[1].hash == 0x11111111u);
    CHECK(g_comps[1].size == 4);
    CHECK(g_comps[2].hash == 0x22222222u);
    CHECK(g_comps[2].bytes[1] == 2);
}

TEST_CASE("spawn_entity_ex refuses bad extra components and says why") {
    const uint32_t v = 1;
    std::string why;
    const SpawnComponent zero_size[1] = {{0x1u, 0, &v, 0}};
    CHECK(spawn_entity_ex(capture_spawn, server_world().p(), kGuid, 0, 0, 0, zero_size, 1, &why) == 0u);
    CHECK_FALSE(why.empty());
    const SpawnComponent null_data[1] = {{0x1u, 0, nullptr, 4}};
    CHECK(spawn_entity_ex(capture_spawn, server_world().p(), kGuid, 0, 0, 0, null_data, 1, &why) == 0u);
    const SpawnComponent second_transform[1] = {{0x027ac564u, 0, &v, 4}};
    CHECK(spawn_entity_ex(capture_spawn, server_world().p(), kGuid, 0, 0, 0, second_transform, 1, &why) == 0u);
    CHECK(why.find("CurrentTransform") != std::string::npos);
    std::vector<SpawnComponent> many(16, SpawnComponent{0x1u, 0, &v, 4});
    CHECK(spawn_entity_ex(capture_spawn, server_world().p(), kGuid, 0, 0, 0, many.data(), many.size(), &why) == 0u);
    CHECK(why.find("15") != std::string::npos);
    CHECK(spawn_entity_ex(capture_spawn, server_world().p(), kGuid, 0, 0, 0, nullptr, 0, &why) == 7001u);  // none is fine
}

TEST_CASE("give_pickup spawns the generic pickup with a PickupItem of the item and amount") {
    ItemRecord item;
    item.id = 0xabcdef01u;
    for (int i = 0; i < 16; ++i) item.guid[i] = static_cast<uint8_t>(0x30 + i);
    std::string why;
    REQUIRE(give_pickup(capture_spawn, server_world().p(), item, 300, 5, 6, 7, &why) == 7001u);
    const uint8_t generic[16] = {0x77, 0x27, 0x6d, 0xa5, 0xf1, 0xda, 0x64, 0x41,
                                 0x90, 0xd4, 0xb7, 0xb1, 0x27, 0x49, 0xc7, 0x23};
    CHECK(std::memcmp(g_ex_guid, generic, 16) == 0);
    REQUIRE(g_comps.size() == 2);
    CHECK(g_comps[1].hash == 0x08775a90u);
    REQUIRE(g_comps[1].size == 28);
    const auto& b = g_comps[1].bytes;
    uint32_t id = 0, level = 1;
    uint16_t amount = 0;
    std::memcpy(&id, b.data() + 0, 4);
    std::memcpy(&level, b.data() + 20, 4);
    std::memcpy(&amount, b.data() + 26, 2);
    CHECK(id == 0xabcdef01u);
    CHECK(b[4] == 0x30);
    CHECK(b[19] == 0x3f);
    CHECK(level == 0);
    CHECK(b[24] == 0);  // rarity
    CHECK(amount == 300);
}

TEST_CASE("give_pickup refuses count 0, more than 65535 and an item without id") {
    ItemRecord item;
    item.id = 7;
    std::string why;
    CHECK(give_pickup(capture_spawn, server_world().p(), item, 0, 0, 0, 0, &why) == 0u);
    CHECK(give_pickup(capture_spawn, server_world().p(), item, 70000, 0, 0, 0, &why) == 0u);
    CHECK(why.find("65535") != std::string::npos);
    ItemRecord none;
    CHECK(give_pickup(capture_spawn, server_world().p(), none, 1, 0, 0, 0, &why) == 0u);
}
