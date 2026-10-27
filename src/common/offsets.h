#pragma once
#include <cstdint>

// Engine offsets for builds 1269579 and 1273043 (unchanged by that update). Source: docs/ENGINE_MAP.md section 1.
namespace emf::off {

// World object
constexpr uint32_t kState = 0x0;                // u8: 1 loading templates, 2 running
constexpr uint32_t kSingletonArray = 0x7462D0;  // ptr to entries, stride kSingletonEntrySize
constexpr uint32_t kSingletonCount = 0x7462D8;  // u64
constexpr uint32_t kSingletonMap = 0x7462F8;    // Keen map: fnv1a32(name) -> u16 index
constexpr uint32_t kSingletonEntrySize = 0xE8;
constexpr uint32_t kSingletonEntryHash = 0xA0;  // u32 fnv1a32(name)
constexpr uint32_t kSingletonEntryObj = 0xA8;   // void* object
constexpr uint32_t kStorage = 0xE15DC0;         // entity storage / sim (argument of the sim step)
constexpr uint32_t kKind = 0xE15DC8;            // u8: 0 server, 1 prediction, 2 client
constexpr uint32_t kEntityMap = 0xE15F08;       // Keen map: EntityId (mixed) -> entity record*
constexpr uint32_t kTickPeriodNs = 0xE16280;    // i64 ns per tick (16,666,667 = 60 Hz); ecsNow = period * tick
constexpr uint32_t kTick = 0xE1629C;            // u32 current tick
constexpr uint32_t kTickLengthNs = 0x1CFB6C0;   // i64 tick length in ns
constexpr uint32_t kWorldMinSize = 0x1CFB6C8;   // highest offset we touch + 8

// ECS events (docs/engine_parts/re_events.md)
constexpr uint32_t kEventTypes = 0xE15AB8;      // ptr to event type entries, stride kEventTypeStride
constexpr uint32_t kEventMap = 0xE15AE0;        // Keen map: fnv1a32("keen::ecs::" + name) -> u16 event type index
constexpr uint32_t kEventTypeStride = 0x20;     // entry +0x00 = reflection descriptor*
constexpr uint32_t kTypeDescBase = 0x38;        // reflection descriptor: base type descriptor*
constexpr uint32_t kTypeDescSize = 0x40;        // reflection descriptor: u32 size
constexpr uint32_t kTypeDescHash = 0x50;        // reflection descriptor: u32 qualifiedHash
// Component registry (storage[0] = world+0x928; see docs/engine_parts/re_templates.md)
constexpr uint32_t kRegistryTypes = 0x08;        // registry: type entries* (kComponentTypeStride each), see 0x1409e6570
constexpr uint32_t kRegistryTypeCount = 0x10;    // registry: u64 number of type entries
constexpr uint32_t kRegistryTemplateMap = 0x30;  // registry: Keen map qualifiedHash -> u16 type index (template)
constexpr uint32_t kRegistryDynamicMap = 0x88;   // registry: Keen map qualifiedHash -> u16 type index (per-entity)
constexpr uint32_t kComponentTypeStride = 0x100;
constexpr uint32_t kComponentTypeDesc = 0x30;    // type entry: the component's reflection descriptor*
constexpr uint32_t kQueueCount = 0x18;          // event queue: u32 number of events
constexpr uint32_t kQueuePages = 0x30;          // event queue: page base pointer
constexpr uint32_t kQueueFirstPage = 0x40;      // event queue: u16 first page (1-based, 0 = none)
constexpr uint32_t kEventPageSize = 0x800;
constexpr uint32_t kEventPageNext = 0x02;       // page: u16 next page (1-based, 0 = none)
constexpr uint32_t kEventPageData = 0x08;       // page: events packed from here
constexpr uint32_t kEventPageUsable = 0x7F8;
constexpr uint32_t kMaxEventPages = 4096;       // sanity cap when walking a page chain

// Items (docs/engine_parts/inventory_crafting.md 1): singleton "g38_items::ItemRegistry"
constexpr uint32_t kItemMap = 0x38;             // Keen map: ItemId -> ItemInfo* (8-byte values)
constexpr uint32_t kItemInfoId = 0x000;         // ItemInfo: u32 itemId
constexpr uint32_t kItemInfoGuid = 0x004;       // ItemInfo: 16-byte objectId (GUID)
constexpr uint32_t kItemInfoMaxStack = 0x014;   // ItemInfo: u16 maxStackSize
constexpr uint32_t kItemInfoDebugName = 0x820;  // ItemInfo: BlobString {u32 offset from this field, u32 length}
constexpr uint32_t kItemInfoSize = 0x828;
constexpr uint32_t kMaxItemNameLength = 255;

// Program job (the struct ProgramContext.job points to)
constexpr uint32_t kJobStorage = 0x000;         // entity storage (world + kStorage for server/client worlds)
constexpr uint32_t kJobTick = 0x050;            // i64 tick
constexpr uint32_t kJobTickLength = 0x06C;      // u32 ns per tick; programTime = tick * length
constexpr uint32_t kJobProgramId = 0x0B8;       // u16 program id
constexpr uint32_t kJobEventBuffer = 0x1B8;     // event write buffer (first argument of emitEvent)
constexpr uint32_t kJobChangeSet = 0x1C0;  // change set for writeOther (0x1409b4070 reads it; server 0x1406548f0)
constexpr uint32_t kJobVariant = 0x0BB;    // u8 passed to writeOther (0x1409b9a20; server 0x14065a3c0)
constexpr uint32_t kJobWorldCtx = 0x030;        // world context for readOther (re_inventory_api.md, 0x1409b2fe0)

// Keen hash map
constexpr uint32_t kMapBits = 0x08;
constexpr uint32_t kMapCapacity = 0x10;
constexpr uint32_t kMapKeys = 0x28;
constexpr uint32_t kMapValues = 0x40;
constexpr uint32_t kMapCount = 0x54;
constexpr uint64_t kMapMaxCapacity = 1ull << 22;  // sanity cap when reading game maps

// Singletons / components
constexpr uint32_t kLocalPlayerId = 0x04;  // LocalPlayerData+4 = local player EntityId

constexpr uint32_t kHashCurrentTransform = 0x027ac564;
constexpr uint32_t kHashActiveNpcState = 0x1a8b5909;
constexpr uint32_t kHashClientPlayerInput = 0x9621e4c3;

}  // namespace emf::off
