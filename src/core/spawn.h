#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace emf::core {

// Engine structures for spawning (docs/ENGINE_MAP.md 1.4, engine_parts/re_time_spawn_weather.md).
struct SpawnComponent {   // 0x18
    uint32_t hash;        // component qualifiedHash
    uint32_t pad;
    const void* data;
    uint64_t size;        // must equal the component size
};
struct SpawnSlice {
    const SpawnComponent* ptr;
    uint64_t count;
};
struct WorldTransformData {  // keen::WorldTransform, 56 B
    int64_t x, y, z;         // 32.32 fixed point
    float qx, qy, qz, qw;
    float sx, sy, sz;
    uint32_t pad;
};
static_assert(sizeof(SpawnComponent) == 0x18 && sizeof(WorldTransformData) == 56, "engine layout");

// spawn wrapper 0x1409788d0: (world, Guid* template, Slice<SpawnComponent>*) -> createEntity(id 0);
// tail-returns createEntity's result in eax: the new EntityId, 0 on failure.
using SpawnWrapperFn = uint32_t (*)(void* world, const void* template_guid, const SpawnSlice* components);
// destroyEntity 0x140995ad0: (storage = world+0xE15DC0, EntityId, bool, ?)
using DestroyEntityFn = uint64_t (*)(void* storage, uint32_t entity, char flag, uint64_t extra);

int64_t meters_to_fixed(double meters);

// Spawns template `guid` at (x,y,z) meters on a SERVER world. Returns the new entity id, or 0 if the
// world is not a server world, an argument is missing, the game refused or the call faulted.
// Call only on the world's own thread after its sim step (dispatcher).
uint32_t spawn_entity(SpawnWrapperFn fn, uint8_t* world, const uint8_t guid[16], double x, double y, double z);
bool destroy_entity(DestroyEntityFn fn, uint8_t* world, uint32_t entity);

constexpr size_t kMaxExtraSpawnComponents = 15;
// Like spawn_entity, plus up to 15 extra spawn components after the CurrentTransform (each with data and a size;
// a second CurrentTransform is refused). Returns 0 and sets *why when refused or when the game did not spawn.
uint32_t spawn_entity_ex(SpawnWrapperFn fn, uint8_t* world, const uint8_t guid[16], double x, double y, double z,
                         const SpawnComponent* extra, size_t count, std::string* why);

// keen::ecs::PickupItem (28 B, hash 0x08775a90): the item a loose pickup gives.
struct PickupItemData {
    uint32_t item_id;
    uint8_t item_guid[16];
    uint32_t level;
    uint8_t rarity;
    uint8_t pad;
    uint16_t amount;
};
static_assert(sizeof(PickupItemData) == 28, "engine layout");
constexpr uint32_t kHashPickupItem = 0x08775a90u;
// Base_LootPickup_Generic_Gravity (a56d2777-daf1-4164-90d4-b7b12749c723), the game's generic loose pickup.
constexpr uint8_t kGenericPickupGuid[16] = {0x77, 0x27, 0x6d, 0xa5, 0xf1, 0xda, 0x64, 0x41,
                                            0x90, 0xd4, 0xb7, 0xb1, 0x27, 0x49, 0xc7, 0x23};

struct ItemRecord;
// Spawns a loose pickup of `count` (1..65535) x item at (x,y,z) meters; the game's loot code gives it to whoever
// picks it up. Returns the pickup entity id, or 0 (*why) when refused.
uint32_t give_pickup(SpawnWrapperFn fn, uint8_t* world, const ItemRecord& item, uint32_t count, double x, double y,
                     double z, std::string* why);

}  // namespace emf::core
