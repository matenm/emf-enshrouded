#include "submit.h"

namespace emf::core {

HostState host_state(const WorldTracker& tracker, uint64_t now_ms) {
    if (tracker.latest_within(EMF_WORLD_SERVER, now_ms, 10000)) return HostState::Hosting;
    if (tracker.latest_within(EMF_WORLD_CLIENT, now_ms, 10000)) return HostState::Client;
    return HostState::None;
}

void submit_line(CommandCenter& center, HostState state, const std::string& line, const ReplyFn& reply) {
    switch (state) {
        case HostState::Hosting: center.enqueue(line); break;
        case HostState::Client:
            center.run_without_world(line, reply, "not run: commands need a world you host (this is someone else's game)");
            break;
        case HostState::None: center.run_without_world(line, reply, "not run: load a world you host first"); break;
    }
}

}  // namespace emf::core
