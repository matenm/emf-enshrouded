#include "file_poll.h"

#include "commands.h"
#include "console.h"
#include "mod_registry.h"
#include "mods.h"
#include "settings.h"

namespace emf::core {

bool FilePoll::tick(uint64_t now) {
    if (last != 0 && now - last < 1000) return false;
    last = now ? now : 1;
    if (commands_file && commands) {
        std::string problem;
        commands_file->poll(*commands, &problem);
        if (problem != last_problem) {
            if (!problem.empty() && reply) reply(problem);
            last_problem = problem;
        }
    }
    if (manifests && mods && settings && !mods_dir.empty())
        for (const auto& line : manifests->poll(mods_dir, mods->list(), *settings))
            if (log) log(line);
    return true;
}

}  // namespace emf::core
