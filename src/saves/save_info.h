#pragma once
// save-info.json (%LOCALAPPDATA%\EMF\save-info.json): what the game's menu showed for each save slot (the character's
// name and level, the world's name), keyed by storage and slot. EMF's core writes it when the main menu is up (from
// the game's slot records, src/core/save_slots.h) and after a world loads; a backup copies the entries for its slots
// into its manifest (backup.h), so the Saves tab can say "Ember (level 8)" instead of "character slot 1".
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "saves/backup.h"

namespace emf::saves {

struct SaveInfo {
    int format = 1;
    int64_t updated = 0;          // when it was last written (Unix seconds)
    std::vector<SlotInfo> slots;  // sorted: steam before local, characters before worlds, by slot
};

std::string save_info_json(const SaveInfo& info);
std::optional<SaveInfo> parse_save_info(std::string_view text, std::string* error = nullptr);
// The file's contents; empty when it is missing or unreadable.
SaveInfo read_save_info(const std::wstring& path);
// Writes the file through a temporary file next to it (a failed write keeps the old one). False when that failed.
bool write_save_info(const std::wstring& path, const SaveInfo& info);

// One slot record as the game's menu tables hold it.
struct SlotSeen {
    std::string storage;  // "steam" or "local"
    std::string steam_id;  // steam: the Steam account the game runs with ("" unknown)
    std::string type;     // "character" or "world"
    int slot = -1;        // 0-based
    uint8_t state = 0;    // the record's state byte: 0 not scanned, 1 empty, 2 exists, 3 being created, 4 changed,
                          // 5 unknown, 6 delete pending
    std::string name;
    std::optional<int> level;
    int64_t played = 0;        // worlds: when last played (Unix seconds; 0 unknown)
    int64_t play_seconds = 0;  // worlds: total play time (0 unknown)
};

// What one look at the menu changes: a slot that exists (state 2 or 4, with a name) is added or updated; an empty or
// deleted slot (state 1 or 6) is removed; any other state (not scanned yet, being created, unreadable) keeps what was
// known. A slot whose name changed is a different character or world: its level and load time start over. A level the
// look did not get keeps the one known for the same name; so do a world's last-played time and play time (taken when
// the look has them). *changed (optional) says whether a name, level, time or slot changed.
SaveInfo merge_seen(SaveInfo info, const std::vector<SlotSeen>& seen, int64_t now, bool* changed = nullptr);
// A world or character the game just loaded: its last_loaded becomes `now` (when its slot is known). `steam_id`: the
// Steam account for a Steam slot ("" when unknown).
SaveInfo note_loaded(SaveInfo info, const std::string& storage, const std::string& type, int slot, int64_t now,
                     bool* changed = nullptr, const std::string& steam_id = {});

}  // namespace emf::saves
