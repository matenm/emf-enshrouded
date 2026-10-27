#pragma once
// What the game's main menu knows about the save slots (docs/saves.md), for save-info.json: the character and
// world slot records in Save2System (menu_layout in menu_start.h), with the character's level and the world's
// last-played time and play time (docs/engine_parts/re_saves.md, the menu's slot records). Build 1273043's client only.
//
// SaveInfoRecorder: a hook on the pregame state handler tells it when the main menu is up; then, on a worker, it reads
// the slot tables once the game has loaded its save lists and merges them into save-info.json. When Start Play begins it
// notes the selected character; when a world has loaded (the core's loadSavegameStep hook) it notes that world and that
// character as loaded. Game memory is only ever read, fault-guarded.
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "common/signatures.h"
#include "saves/save_info.h"

namespace emf::core {

namespace slot_layout {
constexpr uint32_t kCharacterLevel = 0x2F0;  // u32, 1-based: the record's player_character part (+0x2B0) +0x40
constexpr uint32_t kWorldLastPlayed = 0x48;  // i64 Unix seconds: the record's world_metadata (+0x20) +0x28
constexpr uint32_t kWorldPlayTimeNs = 0xB8;  // i64 nanoseconds: world_metadata +0x98
constexpr int kMaxLevel = 1000;
}  // namespace slot_layout

using SlotMemoryRead = std::function<bool(uintptr_t address, void* out, size_t n)>;

// Every slot record of both storages (16 characters and 10 worlds each), or nothing when save2 cannot be read or the
// save lists are not loaded yet (a list job busy, the world list's state not 3).
std::optional<std::vector<saves::SlotSeen>> read_menu_slots(uintptr_t save2, const SlotMemoryRead& read);
// The character slot the menu has selected (a slot id: low byte index + 1, high byte the storage), if one is.
std::optional<uint16_t> selected_character_slot(uintptr_t save2, const SlotMemoryRead& read);

class SaveInfoRecorder {
public:
    struct Deps {
        std::wstring path;  // save-info.json
        SlotMemoryRead read;    // game memory, fault-guarded
        std::function<int64_t()> now;                     // Unix seconds
        std::function<void(const std::string&)> log;
        std::function<void(std::function<void()>)> run;  // where the polling and writing run (empty: at once; tests)
        std::function<void(unsigned ms)> sleep;           // between looks (empty: Sleep)
        unsigned tries = 90;                              // looks at most, a second apart, per menu visit
        std::function<std::string()> steam_user;          // Steam's logged-in account, its userdata folder ("" unknown)
    };
    explicit SaveInfoRecorder(Deps deps);

    // The pregame state handler's call (state 2 = the main menu; phase 0 enter, 1 a command, 2 exit), before the game's
    // handler runs. Cheap: it only keeps pointers and hands the work to deps.run.
    void on_pregame(void* pregame, uint8_t state, uint8_t phase);
    // A world finished loading (the core's on_load): its slot id (low byte index + 1, high byte the storage).
    void on_world_loaded(uint16_t slot_id);
    // One look at the menu's tables now; false when they were not ready (nothing written).
    bool look_once();
    size_t writes() const { return writes_.load(); }

private:
    void save(const saves::SaveInfo& info);
    Deps deps_;
    std::atomic<uintptr_t> save2_{0};
    std::atomic<bool> menu_{false};
    std::atomic<unsigned> visit_{0};
    std::atomic<uint16_t> pending_character_{0};
    std::atomic<size_t> writes_{0};
    std::mutex file_lock_;
};

// The process's recorder (null until install_save_info ran and succeeded).
SaveInfoRecorder* save_info_recorder();
// Hooks the pregame state handler for the recorder, on build 1273043's game exe only (the record offsets are that
// build's). False and why otherwise. The deps' read is set here.
bool install_save_info(const std::array<Resolved, kSigCount>& sigs, bool known_build, int build_index,
                       SaveInfoRecorder::Deps deps, std::string* why);

}  // namespace emf::core
