#pragma once
// Parts of the bridge's glue to the game that can be tested without it (runtime.cpp uses them):
// - StateBatch: state requests share one job per world step that reads only the world's values; the JSON is built on
//   the pipe thread from those values and what EMF tracks anyway.
// - PressBook and the mod_key actions: which presses wait for their release, and their answers.
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/commands.h"
#include "core/overlay/injected_keys.h"
#include "core/overlay/submit.h"
#include "service.h"

namespace emf::core::bridge {

// What the state's world job reads on the world's thread.
struct WorldValues {
    std::string kind;  // server | client | dedicated
    std::optional<uint32_t> tick;
    std::optional<double> ecs_time_s;
    std::optional<double> time_of_day_h;
    std::optional<std::array<double, 3>> position;  // the local player's
    std::optional<int32_t> health_slot0;            // the local player's server Health dataStorage[0] (a world you host)
    std::optional<std::pair<int32_t, int32_t>> health_attributes;  // its current and max health attributes (v18)
    std::optional<std::string> world_name;          // the hosted world's display name
};

class StateBatch {
public:
    using Done = std::function<void(std::string)>;
    static constexpr uint64_t kRepostMs = 2000;  // a job that never ran (the world stopped) is replaced after this

    // A state request. True when the caller must post the world job (none is on its way).
    bool add(Done done, uint64_t now_ms);
    // The world job's values (the world's thread): every waiting request gets them.
    void fill(const WorldValues& values);
    // The requests whose values came (the pipe thread builds their JSON).
    std::vector<std::pair<Done, WorldValues>> take_ready();
    size_t waiting() const;

private:
    mutable std::mutex m_;
    std::vector<Done> waiting_;
    std::vector<std::pair<Done, WorldValues>> ready_;
    bool posted_ = false;
    uint64_t posted_ms_ = 0;
};

// The save slot of the world you host (state.world.slot): set by its load and its saves, forgotten when a new hosted
// session sets up its server world (before that session's load, so a failed or unseen load never shows the previous
// world's slot), and not reported while the running world is someone else's (a client of another game).
class SlotBook {
public:
    void new_session();
    void loaded(uint16_t slot);
    void saved(uint16_t slot);
    // world_kind: the running world's kind (server, client, dedicated), or none (no world runs).
    std::optional<uint32_t> current(const std::optional<std::string>& world_kind) const;

private:
    std::atomic<bool> known_{false};
    std::atomic<uint32_t> slot_{0};
};

// mod_key presses waiting for their release.
class PressBook {
public:
    using Done = std::function<void(const KeyResult&)>;
    void add(uint32_t vk, Done done);
    Done take(uint32_t vk);  // empty when no press of vk waits
    size_t size() const;

private:
    mutable std::mutex m_;
    std::vector<std::pair<uint32_t, Done>> presses_;
};

// The mod_key source's work: press (answered by release_due after the hold), down (answered now), up (answers a press
// of the same key too).
Refusal mod_key_action(InjectedKeys& keys, PressBook& presses, uint32_t vk, KeyAction action, uint32_t hold_ms,
                       PressBook::Done done, uint64_t now_ms);
// Releases the presses whose time is up and answers them with how often mods read the key.
void release_due(InjectedKeys& keys, PressBook& presses, uint64_t now_ms);

// Lines queued while you hosted a world that has gone since would wait for the next world (and hold a bridge client's
// console limit): answered now with "needs a world" when no world you host runs. Returns the lines answered.
size_t answer_lines_without_world(CommandCenter& center, HostState state, const ReplyFn& sink);

// The game's main window: a visible top-level window of this process without an owner, the game's
// "localWindowClass" first. Null if none.
HWND game_main_window();

}  // namespace emf::core::bridge
