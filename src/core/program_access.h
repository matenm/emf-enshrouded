#pragma once
// Reading other entities' components inside a mod program through the game's own other-entity accessors.
#include <cstdint>
#include <string>
#include <string_view>

#include "common/world_view.h"
#include "system_programs.h"

namespace emf::core {

// readOther(void** out, void* worldCtx, Accessor acc, u32 entity) (Sig::ReadOther); out[0] = component* or NULL.
using ReadOtherFn = void* (*)(void** out, void* world_ctx, uint64_t accessor, uint32_t entity);
// writeOther(void** out, Storage* s, ChangeSet* c, Accessor acc, u32 entity, u8 variant) (Sig::WriteOther): the same
// for writing; it marks the component changed (sent to the players, saved) unless the storage is a prediction copy.
using WriteOtherFn = void* (*)(void** out, void* storage, void* change_set, uint64_t accessor, uint32_t entity,
                               uint8_t variant);

struct OtherRead {
    void* ptr = nullptr;
    uint64_t size = 0;  // the game's size of the component, once the accessor is checked
};

// Reads `entity`'s component through the call's accessor `a` (worldCtx = *(job + 0x30)). Until the accessor is checked
// in a world, each read is compared with getComponent: both giving the same pointer marks it Good (its size is kept),
// two different pointers mark it Bad (*problem says so; every later read in that world gives NULL), and a result on one
// side only is inconclusive (NULL, checked again on the next read).
// A fault in the game's function gives NULL and a problem line.
OtherRead read_other(const ProgramCall& call, const OtherAccess& a, std::string_view component, uint32_t entity,
                     ReadOtherFn read, GetComponentFn get_component, std::string* problem);
// The same through a write accessor (a.write): storage = *(job+0x30), change set = *(job+0x1C0), variant =
// *(u8*)(job+0xBB). The first use in a world checks the accessor through readOther (no side effects) against
// getComponent; writeOther (which marks the component changed) is called only through a checked accessor.
OtherRead write_other(const ProgramCall& call, const OtherAccess& a, std::string_view component, uint32_t entity,
                      ReadOtherFn read, WriteOtherFn write, GetComponentFn get_component, std::string* problem);

}  // namespace emf::core
