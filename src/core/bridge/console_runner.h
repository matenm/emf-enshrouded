#pragma once
// The bridge's `console`: a line goes where a line typed into the in-game console goes (overlay/submit.h): queued for
// the server world's tick when you host a world, else answered at once (help, mods and the other "anywhere" commands
// still run).
#include <cstdint>
#include <functional>
#include <string>

#include "core/commands.h"
#include "core/overlay/submit.h"

namespace emf::core::bridge {

// `line` gets each line of output for the bridge client; `sink` is the core's reply sink (the log, the console window
// and the overlay), which a queued line reaches through run_pending; `done` is called once with what became of the
// line (command_outcome_name), after its lines.
void run_console_line(CommandCenter& center, HostState state, const std::string& text, const ReplyFn& line,
                      std::function<void(const std::string& outcome)> done, const ReplyFn& sink, uint64_t now_ms);

}  // namespace emf::core::bridge
