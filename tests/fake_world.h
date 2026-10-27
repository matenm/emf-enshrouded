#pragma once
// Test helpers: a zeroed fake ECS world plus 8-slot Keen maps planted inside it.
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/fnv.h"
#include "common/keen_map.h"
#include "common/offsets.h"

namespace emf::test {

struct FakeWorld {
    std::unique_ptr<uint8_t[]> mem{new uint8_t[off::kWorldMinSize]()};
    uint8_t* p() { return mem.get(); }
    template <class T>
    void put(uint32_t offset, T v) { std::memcpy(mem.get() + offset, &v, sizeof(T)); }
};

// Backing arrays for an 8-slot Keen map embedded in a FakeWorld at `map_off`.
struct MapStore {
    uint64_t bits[1] = {};
    uint32_t keys[8] = {};
    uint64_t vals8[8] = {};
    uint16_t vals2[8] = {};
    void attach(FakeWorld& w, uint32_t map_off, bool u16_values) {
        w.put<const void*>(map_off + off::kMapBits, bits);
        w.put<uint64_t>(map_off + off::kMapCapacity, 8);
        w.put<const void*>(map_off + off::kMapKeys, keys);
        w.put<const void*>(map_off + off::kMapValues, u16_values ? static_cast<const void*>(vals2) : vals8);
    }
    void put(FakeWorld& w, uint32_t map_off, size_t slot, uint32_t key) {
        bits[0] |= 1ull << slot;
        keys[slot] = key;
        uint32_t c;
        std::memcpy(&c, w.p() + map_off + off::kMapCount, 4);
        ++c;
        w.put<uint32_t>(map_off + off::kMapCount, c);
    }
};

// A component registry (*(world + kStorage)) whose per-entity ("dynamic") component map at +0x88 holds `hashes`
// (plain keys, linear probing, u16 type indices).
struct FakeRegistry {
    alignas(8) uint8_t reg[0x100] = {};
    uint64_t bits[1] = {};
    uint32_t keys[8] = {};
    uint16_t vals[8] = {};
    void attach(FakeWorld& w, std::initializer_list<uint32_t> dynamic_hashes) {
        uint8_t* map = reg + off::kRegistryDynamicMap;
        const void* b = bits;
        const void* k = keys;
        const void* v = vals;
        const uint64_t cap = 8;
        std::memcpy(map + off::kMapBits, &b, 8);
        std::memcpy(map + off::kMapCapacity, &cap, 8);
        std::memcpy(map + off::kMapKeys, &k, 8);
        std::memcpy(map + off::kMapValues, &v, 8);
        uint16_t i = 0;
        for (uint32_t h : dynamic_hashes) {
            size_t slot = h & 7;
            while (bits[0] >> slot & 1) slot = (slot + 1) & 7;
            bits[0] |= 1ull << slot;
            keys[slot] = h;
            vals[slot] = i++;
        }
        const uint32_t count = i;
        std::memcpy(map + off::kMapCount, &count, 4);
        w.put<const void*>(off::kStorage, reg);
    }
};

// Plants entities (mixed-key slots) into the world's entity map.
inline void plant_entities(FakeWorld& w, MapStore& ms, std::initializer_list<uint32_t> ids) {
    ms.attach(w, off::kEntityMap, false);
    for (uint32_t id : ids) {
        size_t slot = KeenMapView::mix(id) & 7;
        while (ms.bits[0] >> slot & 1) slot = (slot + 1) & 7;
        ms.put(w, off::kEntityMap, slot, id);
    }
}

// Plants one named singleton (entry index 0) pointing at `object`.
struct SingletonStore {
    MapStore map;
    alignas(8) uint8_t entries[off::kSingletonEntrySize] = {};
    void plant(FakeWorld& w, const char* name, void* object) {
        map.attach(w, off::kSingletonMap, true);
        const uint32_t h = fnv1a32(name);
        map.vals2[h & 7] = 0;
        map.put(w, off::kSingletonMap, h & 7, h);
        std::memcpy(entries + off::kSingletonEntryHash, &h, 4);
        std::memcpy(entries + off::kSingletonEntryObj, &object, 8);
        w.put<const void*>(off::kSingletonArray, entries);
        w.put<uint64_t>(off::kSingletonCount, 1);
    }
};

// An event map + event type table planted in a FakeWorld (see docs/engine_parts/re_events.md).
struct EventStore {
    struct TypeEntry {
        const void* desc;
        uint8_t rest[0x18];
    };
    MapStore map;
    TypeEntry types[8] = {};
    alignas(8) uint8_t descs[8][0x90] = {};
    alignas(8) uint8_t game_event_desc[0x90] = {};  // keen::ecs::GameEvent (timeStamp at +0)
    void attach(FakeWorld& w) {
        map.attach(w, off::kEventMap, true);
        w.put<const void*>(off::kEventTypes, types);
        const uint32_t ge = fnv1a32(std::string("keen::ecs::GameEvent"));
        std::memcpy(game_event_desc + off::kTypeDescHash, &ge, 4);
    }
    // desc_hash 0 = the event's own hash; timestamped = derives from GameEvent (else from the old ecs::Event)
    void plant(FakeWorld& w, const char* name, uint16_t index, uint32_t size, uint32_t desc_hash = 0,
               bool timestamped = true) {
        const uint32_t h = fnv1a32(std::string("keen::ecs::") + name);
        size_t slot = h & 7;
        while (map.bits[0] >> slot & 1) slot = (slot + 1) & 7;
        map.vals2[slot] = index;
        map.put(w, off::kEventMap, slot, h);
        types[index].desc = descs[index];
        const uint32_t dh = desc_hash ? desc_hash : h;
        std::memcpy(descs[index] + off::kTypeDescSize, &size, 4);
        std::memcpy(descs[index] + off::kTypeDescHash, &dh, 4);
        const void* base = timestamped ? game_event_desc : nullptr;
        std::memcpy(descs[index] + off::kTypeDescBase, &base, 8);
    }
};

// An engine event queue with `pages` pages of 0x800 bytes; fill() plants events in engine order.
struct FakeQueue {
    alignas(8) uint8_t q[0x48] = {};
    std::unique_ptr<uint8_t[]> base;
    FakeQueue(uint16_t pages, uint32_t count) : base(new uint8_t[size_t(pages) * off::kEventPageSize]()) {
        std::memcpy(q + off::kQueueCount, &count, 4);
        const uint8_t* b = base.get();
        std::memcpy(q + off::kQueuePages, &b, 8);
        const uint16_t first = pages ? 1 : 0;
        std::memcpy(q + off::kQueueFirstPage, &first, 2);
        for (uint16_t i = 0; i + 1 < pages; ++i) link(i, uint16_t(i + 2));
    }
    void link(uint16_t page, uint16_t next_index) {
        std::memcpy(base.get() + size_t(page) * off::kEventPageSize + off::kEventPageNext, &next_index, 2);
    }
    // Writes u32 i at +8 of event i (events `stride` bytes apart).
    void fill(uint32_t count, uint32_t stride) {
        uint32_t page = 0, offset = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (offset + stride > off::kEventPageUsable) {
                ++page;
                offset = 0;
            }
            std::memcpy(base.get() + size_t(page) * off::kEventPageSize + off::kEventPageData + offset + 8, &i, 4);
            offset += stride;
        }
    }
};

// A ProgramContext {job*, entity index} whose job points at a world's entity storage and has an event
// buffer, a program id and a clock.
struct FakeProgramCtx {
    alignas(8) uint8_t job[0x200] = {};
    alignas(8) uint8_t buffer_obj[0x18] = {};
    struct {
        void* job;
        uint32_t index, pad;
    } ctx{};
    explicit FakeProgramCtx(uint8_t* world, uint16_t program_id = 9, int64_t tick = 10, uint32_t tick_len = 100) {
        uint8_t* storage = world + off::kStorage;
        void* b = buffer_obj;
        std::memcpy(job + off::kJobStorage, &storage, 8);
        std::memcpy(job + off::kJobEventBuffer, &b, 8);
        std::memcpy(job + off::kJobProgramId, &program_id, 2);
        std::memcpy(job + off::kJobTick, &tick, 8);
        std::memcpy(job + off::kJobTickLength, &tick_len, 4);
        ctx.job = job;
    }
    void* p() { return &ctx; }
};

// A fake g38_items::ItemRegistry: item map at +0x38 (8 slots), ItemInfo blobs with a debugName BlobString.
struct FakeItemRegistry {
    alignas(8) uint8_t reg[0x100] = {};
    uint64_t bits[1] = {};
    uint32_t keys[8] = {};
    const uint8_t* vals[8] = {};
    std::vector<std::unique_ptr<uint8_t[]>> infos;
    FakeItemRegistry() {
        const void* b = bits;
        const void* k = keys;
        const void* v = vals;
        const uint64_t cap = 8;
        std::memcpy(reg + off::kItemMap + off::kMapBits, &b, 8);
        std::memcpy(reg + off::kItemMap + off::kMapCapacity, &cap, 8);
        std::memcpy(reg + off::kItemMap + off::kMapKeys, &k, 8);
        std::memcpy(reg + off::kItemMap + off::kMapValues, &v, 8);
    }
    // Adds an item; name nullptr = no debug name (zero BlobString); bad_name = an unprintable name.
    uint8_t* add(uint32_t id, uint8_t guid_seed, uint16_t max_stack, const char* name, bool bad_name = false) {
        const size_t len = name ? std::strlen(name) : 0;
        auto blob = std::make_unique<uint8_t[]>(off::kItemInfoSize + len + 1);
        uint8_t* p = blob.get();
        std::memset(p, 0, off::kItemInfoSize + len + 1);
        std::memcpy(p + off::kItemInfoId, &id, 4);
        for (int i = 0; i < 16; ++i) p[off::kItemInfoGuid + i] = static_cast<uint8_t>(guid_seed + i);
        std::memcpy(p + off::kItemInfoMaxStack, &max_stack, 2);
        if (name) {
            std::memcpy(p + off::kItemInfoSize, name, len);
            if (bad_name) p[off::kItemInfoSize] = 0x01;
            const uint32_t rel = off::kItemInfoSize - off::kItemInfoDebugName;  // relative to the field itself
            const uint32_t n = static_cast<uint32_t>(len);
            std::memcpy(p + off::kItemInfoDebugName, &rel, 4);
            std::memcpy(p + off::kItemInfoDebugName + 4, &n, 4);
        }
        size_t slot = id & 7;
        while (bits[0] >> slot & 1) slot = (slot + 1) & 7;
        bits[0] |= 1ull << slot;
        keys[slot] = id;
        vals[slot] = p;
        uint32_t count;
        std::memcpy(&count, reg + off::kItemMap + off::kMapCount, 4);
        ++count;
        std::memcpy(reg + off::kItemMap + off::kMapCount, &count, 4);
        infos.push_back(std::move(blob));
        return p;
    }
};

}  // namespace emf::test
