#pragma once
// The game's item definitions, read from the g38_items::ItemRegistry singleton (fault-guarded).
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace emf::core {

struct ItemRecord {
    uint32_t id = 0;
    uint8_t guid[16] = {};
    uint16_t max_stack = 0;
    std::string name;  // the game's debug name, or empty
};

// FNV-1a-32 over the 16 raw GUID bytes (the game's ItemId); 0 for a null GUID.
uint32_t item_id_from_guid(const uint8_t guid[16]);
// Every item in the registry's map (empty while it is still loading or on a bad pointer).
std::vector<ItemRecord> list_items(const uint8_t* registry);
std::optional<ItemRecord> item_by_id(const uint8_t* registry, uint32_t id);
// Exact debug name first, then the first case-insensitive match in map order.
std::optional<ItemRecord> item_by_name(const uint8_t* registry, std::string_view name);

// The game's ItemInfo blob for an id (off::kItemInfoSize bytes), or null.
uint8_t* item_data_ptr(const uint8_t* registry, uint32_t id);
// Sets ItemInfo.maxStackSize. False (nothing written) for 0, an unknown id, or memory that is not writable.
bool set_max_stack(const uint8_t* registry, uint32_t id, uint16_t max);
// Writes n bytes into game data. Pages the game keeps read-only (its resource memory: item and template data) are made
// writable for the copy and set back. False (nothing written) for code, no-access, guard or unmapped pages.
bool write_game_memory(void* dst, const void* src, size_t n);

}  // namespace emf::core
