#pragma once
// The debug bridge inside the game ([debug] bridge=1 in emf\emf_core.ini; docs/modding/debug_bridge.md): the pipe
// server, the command service with sources from the core's state, emf\bridge.txt, the game events and the keys for
// mods. Nothing here is created and no thread runs while the bridge is off.
#include <cstdint>
#include <functional>
#include <string>

#include "core/commands.h"

namespace emf::core {
struct CoreState;
namespace bridge {

struct RuntimeDeps {
    std::wstring emf_dir;                          // <game>\emf
    CoreState* state = nullptr;
    ReplyFn command_sink;                          // the core's reply sink: the log, the console window, the overlay
    std::function<void(const std::string&)> log;
    std::string load_world_off;  // why load_world is off ("" = on: the core installed the main-menu hooks)
};

// Starts the bridge: false (and logged) when it could not. Call once, after the overlay started (its key hooks) and
// before mods load (the game events must be subscribed before the first world is set up).
bool start_debug_bridge(RuntimeDeps deps);
// The world step's note that a world exists but is not running yet ("loading" in state). Cheap; any thread.
void note_world_setup(uint64_t now_ms);
// A new hosted session sets up its server world (before its save loads): the previous world's slot is forgotten, so
// state.world.slot shows only the new world's (from its load event), or nothing. Not from note_world_setup: that runs
// on every loading step, after the load event too.
void note_session_start();

}  // namespace bridge
}  // namespace emf::core
