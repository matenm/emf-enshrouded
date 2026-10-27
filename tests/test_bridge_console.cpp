// The debug bridge's `console`: a line runs exactly as if typed. The command center tells when a queued line is done
// and what became of it (ran, unknown, refused, needs a world, too old, crashed), so the bridge can answer with its
// output and status; lines that come a step later (put's result) are still collected while the reply settles.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/bridge/console_runner.h"
#include "core/bridge/service.h"
#include "core/commands.h"
#include "core/json.h"

using namespace emf::core;
using namespace emf::core::bridge;

namespace {
EmfWorld* const kWorld = reinterpret_cast<EmfWorld*>(0x4000);
void say_two(EmfWorld*, int, const char* const* argv, void*) {
    command_reply_line(std::string("one ") + argv[0]);
    command_reply_line("two");
}
void say_help(EmfWorld* w, int, const char* const*, void*) {
    command_reply_line(w ? "help in a world" : "help without a world");
}
ReplyFn g_later;  // like put: its result comes one server step later through current_reply()
void put_later(EmfWorld*, int, const char* const*, void*) {
    command_reply_line("queued");
    g_later = current_reply();
}
void crash(EmfWorld*, int, const char* const*, void*) { *static_cast<volatile int*>(nullptr) = 1; }

// A service whose console runs lines through a real command center, the way the core wires it.
struct ConsoleRig {
    CommandCenter center;
    HostState state = HostState::Hosting;
    uint64_t now = 5000;
    std::vector<std::string> sent, sink;
    std::unique_ptr<Service> service;
    ConsoleRig() {
        REQUIRE(center.add("two", "", say_two, nullptr, nullptr));
        REQUIRE(center.add("help", "", say_help, nullptr, nullptr, true));
        REQUIRE(center.add("put", "", put_later, nullptr, nullptr));
        REQUIRE(center.add("boom", "", crash, nullptr, nullptr));
        Sources s;
        s.now_ms = [this] { return now; };
        s.console = [this](const std::string& text, std::function<void(const std::string&)> line,
                           std::function<void(const std::string&)> done) {
            run_console_line(center, state, text, line, done, [this](const std::string& l) { sink.push_back(l); }, now);
        };
        Output o;
        o.send = [this](ClientId, std::string line, bool) { sent.push_back(std::move(line)); };
        service = std::make_unique<Service>(s, o);
        service->on_connect(1);
    }
    // The core's server tick: runs the queued lines with its reply sink.
    void tick() { center.run_pending(kWorld, [this](const std::string& l) { sink.push_back(l); }, now); }
    void settle() {
        now += kSettleDefaultMs;
        service->poll();
    }
    json::Value last() {
        REQUIRE_FALSE(sent.empty());
        return *json::parse(sent.back()).value;
    }
    std::string status() {
        const json::Value v = last();
        const json::Value* s = v.get("status");
        return s ? s->s : "<none>";
    }
    std::vector<std::string> output() {
        std::vector<std::string> out;
        const json::Value v = last();
        INFO("reply: ", sent.back());
        REQUIRE(v.get("output"));
        for (const auto& l : v.get("output")->arr) out.push_back(l.s);
        return out;
    }
};
}  // namespace

TEST_CASE("bridge console: a hosted line waits for the world's tick, then answers with its output; the log gets it too") {
    ConsoleRig rig;
    rig.service->on_line(1, R"({"id":1,"cmd":"console","text":"two"})");
    CHECK(rig.sent.empty());  // queued for the server world
    rig.tick();
    CHECK(rig.sent.empty());  // it ran: late lines may still come
    rig.settle();
    REQUIRE(rig.sent.size() == 1);
    CHECK(rig.status() == "ran");
    CHECK(rig.output() == std::vector<std::string>{"one two", "two"});
    CHECK(rig.sink == std::vector<std::string>{"> two", "one two", "two"});  // the log and console as if typed
}

TEST_CASE("bridge console: a line that comes a step later (put's result) is in the reply") {
    ConsoleRig rig;
    rig.service->on_line(1, R"({"id":1,"cmd":"console","text":"put Wood 1"})");
    rig.tick();
    REQUIRE(g_later);
    rig.now += 16;  // the next server step
    g_later("put 1 x Wood into entity 1: 1 added");
    g_later = nullptr;
    rig.settle();
    CHECK(rig.output() == std::vector<std::string>{"queued", "put 1 x Wood into entity 1: 1 added"});
}

TEST_CASE("bridge console: a line the world does not run in time times out; its output still reaches the log") {
    ConsoleRig rig;
    rig.service->on_line(1, R"({"id":2,"cmd":"console","text":"two","timeout_ms":1000})");
    rig.now += 1001;
    rig.service->poll();
    REQUIRE(rig.sent.size() == 1);
    const json::Value r = rig.last();
    CHECK_FALSE(r.get("ok")->b);
    CHECK(r.get("code")->s == "timeout");
    CHECK(rig.output().empty());
    rig.tick();  // the world comes back within 30 s: the line runs then
    rig.settle();
    CHECK(rig.sent.size() == 1);
    CHECK(rig.sink.size() == 3);
}

TEST_CASE("bridge console: without a world you host, anywhere commands run and others say why not") {
    ConsoleRig rig;
    rig.state = HostState::None;
    rig.service->on_line(1, R"({"id":3,"cmd":"console","text":"two"})");
    CHECK(rig.status() == "needs_world");
    CHECK(rig.output() == std::vector<std::string>{"not run: load a world you host first"});
    rig.service->on_line(1, R"({"id":4,"cmd":"console","text":"help"})");
    rig.settle();
    CHECK(rig.status() == "ran");
    CHECK(rig.output() == std::vector<std::string>{"help without a world"});
    rig.state = HostState::Client;
    rig.service->on_line(1, R"({"id":5,"cmd":"console","text":"two"})");
    CHECK(rig.status() == "needs_world");
    CHECK(rig.output() == std::vector<std::string>{"not run: commands need a world you host (this is someone else's game)"});
    rig.service->on_line(1, R"({"id":6,"cmd":"console","text":"nosuch"})");
    CHECK(rig.status() == "unknown");
    CHECK(rig.output() == std::vector<std::string>{"unknown command 'nosuch' (type help)"});
    CHECK(rig.sink.size() == 8);  // echo + answer for each, in the log as well
}

TEST_CASE("bridge console: a command that crashes says so, and a disabled one is refused") {
    ConsoleRig rig;
    rig.service->on_line(1, R"({"id":1,"cmd":"console","text":"boom"})");
    rig.tick();
    CHECK(rig.status() == "crashed");
    rig.service->on_line(1, R"({"id":2,"cmd":"console","text":"boom"})");
    rig.tick();
    CHECK(rig.status() == "refused");
}

TEST_CASE("commands: a queued line's done callback runs once with its outcome, after its command and its replies") {
    CommandCenter c;
    REQUIRE(c.add("two", "", say_two, nullptr, nullptr));
    std::vector<std::string> seen;
    int done = 0;
    CommandOutcome outcome = CommandOutcome::Empty;
    c.enqueue("two", 1000, {}, [&](const std::string& s) { seen.push_back(s + (done ? " (late)" : "")); },
              [&](CommandOutcome o) {
                  ++done;
                  outcome = o;
              });
    CHECK(done == 0);
    c.run_pending(kWorld, ReplyFn(), 1000);
    CHECK(done == 1);
    CHECK(outcome == CommandOutcome::Ran);
    CHECK(seen == std::vector<std::string>{"> two", "one two", "two"});
    c.run_pending(kWorld, ReplyFn(), 1000);
    CHECK(done == 1);  // once
}

TEST_CASE("commands: the outcome of an unknown command, a line typed too long ago, a refused line and an empty one") {
    CommandCenter c;
    std::vector<CommandOutcome> outcomes;
    std::vector<std::string> seen;
    auto reply = [&](const std::string& s) { seen.push_back(s); };
    auto done = [&](CommandOutcome o) { outcomes.push_back(o); };
    c.enqueue("nosuch", 1000, {}, reply, done);
    c.run_pending(kWorld, ReplyFn(), 1000);
    CHECK(seen.back().find("unknown command") != std::string::npos);

    c.enqueue("nosuch", 1000, {}, reply, done);
    c.run_pending(kWorld, ReplyFn(), 1000 + 60000, 30000);
    CHECK(seen.back().find("not run: typed 60 s ago") != std::string::npos);

    c.enqueue("nosuch", 1000, {}, reply, done);
    c.reject_pending("not run: someone else's game", ReplyFn());
    CHECK(seen.back() == "not run: someone else's game");

    c.enqueue("   ", 1000, {}, reply, done);  // an empty line still finishes
    c.run_pending(kWorld, ReplyFn(), 1000);
    CHECK(outcomes == std::vector<CommandOutcome>{CommandOutcome::Unknown, CommandOutcome::TooOld,
                                                  CommandOutcome::NeedsWorld, CommandOutcome::Empty});
    CHECK(std::string(command_outcome_name(CommandOutcome::NeedsWorld)) == "needs_world");
    CHECK(std::string(command_outcome_name(CommandOutcome::TooOld)) == "too_old");

    CommandOutcome o = CommandOutcome::Empty;
    REQUIRE(c.add("help", "", say_help, nullptr, nullptr, true));
    REQUIRE(c.add("two", "", say_two, nullptr, nullptr));
    c.run_without_world("help", reply, "no world", &o);
    CHECK(o == CommandOutcome::Ran);
    c.run_without_world("two", reply, "no world", &o);
    CHECK(o == CommandOutcome::NeedsWorld);
    c.run_without_world("zzz", reply, "no world", &o);
    CHECK(o == CommandOutcome::Unknown);
}
