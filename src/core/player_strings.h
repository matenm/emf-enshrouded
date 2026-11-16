#pragma once
// A player's character name (private research character_name_2026-11-10): each client sends its character's name once
// it is in the world; the server keeps it in the CustomPlayerStringsSystem singleton, an id -> entry map, and the player
// entity's CustomPlayerString component holds the id, the FNV-1a64 of the text. The name is display text the player's game
// chose, not an identity.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace emf::core {

constexpr uint32_t kCustomPlayerStringHash = 0x0e699564;  // keen::ecs::CustomPlayerString: u64 stringId
constexpr const char* kCustomStringsSingleton = "CustomPlayerStringsSystem";
// Build 1287369, game and dedicated server: the map in the system (a Keen map: u64 keys, entry pointers) and an entry's
// id, text pointer and length.
constexpr size_t kCustomStringsMap = 0x190, kEntryId = 0x10, kEntryText = 0x18, kEntryLen = 0x20;
constexpr size_t kMaxCustomString = 0x100;

// The text of the entry `id` in the system's map: only when the entry's id is `id`, its length is 1 to 256 bytes, the text
// hashes to `id` (FNV-1a64) and display_name_ok takes it (valid UTF-8, no control characters or bidi controls); else
// nothing (a freed or reused entry, a layout change after a game update, a name that would garble the chat). Every read is guarded. On the server world's thread; never cache the pointers.
std::optional<std::string> custom_player_string(const uint8_t* system, uint64_t id);

}  // namespace emf::core
