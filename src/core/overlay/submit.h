#pragma once
// Where a line typed into the in-game console (or the console window) goes: the server tick when you host a world,
// otherwise an immediate answer (help and mods run anywhere).
#include <cstdint>
#include <string>

#include "core/commands.h"
#include "core/world_tracker.h"

namespace emf::core {

enum class HostState { Hosting, Client, None };

// A server world that ticked within 10 s: Hosting; else a client world within 10 s: Client (someone else's game);
// else None (main menu, loading screen).
HostState host_state(const WorldTracker& tracker, uint64_t now_ms);

void submit_line(CommandCenter& center, HostState state, const std::string& line, const ReplyFn& reply);

}  // namespace emf::core
