#include "spawn.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "common/offsets.h"
#include "common/world_view.h"
#include "items.h"

namespace emf::core {
namespace {

uint32_t guarded_spawn(SpawnWrapperFn fn, void* world, const void* guid, const SpawnSlice* slice) {
    __try {
        return fn(world, guid, slice);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool guarded_destroy(DestroyEntityFn fn, void* storage, uint32_t id) {
    __try {
        fn(storage, id, 1, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool is_server(uint8_t* world) { return WorldView(world).kind() == 0; }

WorldTransformData transform_at(double x, double y, double z) {
    WorldTransformData xf{};
    xf.x = meters_to_fixed(x);
    xf.y = meters_to_fixed(y);
    xf.z = meters_to_fixed(z);
    xf.qw = 1.0f;
    xf.sx = xf.sy = xf.sz = 1.0f;
    return xf;
}

void set_why(std::string* why, const char* text) {
    if (why) *why = text;
}

}  // namespace

int64_t meters_to_fixed(double meters) { return std::llround(meters * 4294967296.0); }

uint32_t spawn_entity(SpawnWrapperFn fn, uint8_t* world, const uint8_t guid[16], double x, double y, double z) {
    if (!fn || !world || !guid || !is_server(world)) return 0;
    WorldTransformData xf{};
    xf.x = meters_to_fixed(x);
    xf.y = meters_to_fixed(y);
    xf.z = meters_to_fixed(z);
    xf.qw = 1.0f;
    xf.sx = xf.sy = xf.sz = 1.0f;
    const SpawnComponent comp{off::kHashCurrentTransform, 0, &xf, sizeof(xf)};
    const SpawnSlice slice{&comp, 1};
    return guarded_spawn(fn, world, guid, &slice);
}

uint32_t spawn_entity_ex(SpawnWrapperFn fn, uint8_t* world, const uint8_t guid[16], double x, double y, double z,
                         const SpawnComponent* extra, size_t count, std::string* why) {
    if (!fn || !world || !guid) {
        set_why(why, "missing spawn function, world or template GUID");
        return 0;
    }
    if (!is_server(world)) {
        set_why(why, "not a server world");
        return 0;
    }
    if (count > kMaxExtraSpawnComponents) {
        set_why(why, "at most 15 extra components");
        return 0;
    }
    if (count && !extra) {
        set_why(why, "no component array");
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!extra[i].data || !extra[i].size) {
            set_why(why, "an extra component has no data or size 0");
            return 0;
        }
        if (extra[i].hash == off::kHashCurrentTransform) {
            set_why(why, "the CurrentTransform comes from x, y, z; do not pass it again");
            return 0;
        }
    }
    const WorldTransformData xf = transform_at(x, y, z);
    std::vector<SpawnComponent> comps;
    comps.push_back({off::kHashCurrentTransform, 0, &xf, sizeof(xf)});
    for (size_t i = 0; i < count; ++i) comps.push_back(extra[i]);
    const SpawnSlice slice{comps.data(), comps.size()};
    const uint32_t id = guarded_spawn(fn, world, guid, &slice);
    if (!id) set_why(why, "the game created no entity (unknown template, bad component data, or a fault)");
    return id;
}

uint32_t give_pickup(SpawnWrapperFn fn, uint8_t* world, const ItemRecord& item, uint32_t count, double x, double y,
                     double z, std::string* why) {
    if (!item.id) {
        set_why(why, "unknown item");
        return 0;
    }
    if (count == 0 || count > 65535) {
        set_why(why, "count must be 1..65535");
        return 0;
    }
    PickupItemData pickup{};
    pickup.item_id = item.id;
    std::memcpy(pickup.item_guid, item.guid, 16);
    pickup.amount = static_cast<uint16_t>(count);
    const SpawnComponent comp{kHashPickupItem, 0, &pickup, sizeof(pickup)};
    return spawn_entity_ex(fn, world, kGenericPickupGuid, x, y, z, &comp, 1, why);
}

bool destroy_entity(DestroyEntityFn fn, uint8_t* world, uint32_t entity) {
    if (!fn || !world || !entity || !is_server(world)) return false;
    return guarded_destroy(fn, world + off::kStorage, entity);
}

}  // namespace emf::core
