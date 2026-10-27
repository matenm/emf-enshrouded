#include "console_runner.h"

namespace emf::core::bridge {

void run_console_line(CommandCenter& center, HostState state, const std::string& text, const ReplyFn& line,
                      std::function<void(const std::string& outcome)> done, const ReplyFn& sink, uint64_t now_ms) {
    if (state == HostState::Hosting) {
        center.enqueue(text, now_ms, CommandSender{}, line, [done](CommandOutcome o) {
            if (done) done(command_outcome_name(o));
        });
        return;
    }
    const ReplyFn both = [line, sink](const std::string& s) {
        if (sink) sink(s);
        if (line) line(s);
    };
    CommandOutcome outcome = CommandOutcome::Empty;
    center.run_without_world(text, both,
                             state == HostState::Client
                                 ? "not run: commands need a world you host (this is someone else's game)"
                                 : "not run: load a world you host first",
                             &outcome);
    if (done) done(command_outcome_name(outcome));
}

}  // namespace emf::core::bridge
