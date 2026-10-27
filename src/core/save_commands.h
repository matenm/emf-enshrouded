#pragma once
// The console's save backup commands (docs/saves.md), for players with or without EMF.exe:
//   saves.backup [label]   a manual backup of every save file, only at the main menu: while a world runs the game
//                          writes its saves (every 30 seconds), so a copy then could catch a file half written
//   saves.list             the backups, newest first, with what each holds, and where they are
// Restoring needs the game closed: EMF.exe's Saves tab does it, or by hand from the backup folder (docs/saves.md).
// The copying runs on a worker (SaveCommandContext::run), never on the render thread or a world's thread; the answer
// goes back to where the command came from.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "saves/backup.h"

namespace emf::core {

class CommandCenter;

struct SaveCommandContext {
    saves::SaveRoots roots;
    std::string emf_version;
    std::function<int64_t()> now;                     // Unix seconds (the clock when empty)
    std::function<void(std::function<void()>)> run;  // where the work runs (empty: on the caller's thread; tests)
    // A world runs now, hosted or someone else's (the game writes saves then): saves.backup refuses. Empty: only the
    // world a command comes with counts.
    std::function<bool()> in_a_world;
};

// Registers saves.backup and saves.list (they run without a world too); `ctx` must stay alive as long as they can run.
bool add_save_commands(CommandCenter& center, SaveCommandContext& ctx);

// What the commands answer, line by line, on the caller's thread. `world_running`: a world runs now (the command came
// with one), so saves.backup refuses.
std::vector<std::string> run_saves_backup(SaveCommandContext& ctx, const std::string& label, bool world_running);
std::vector<std::string> run_saves_list(SaveCommandContext& ctx);

}  // namespace emf::core
