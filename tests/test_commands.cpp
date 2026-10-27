#include <doctest.h>

#include <string>
#include <thread>
#include <vector>

#include "core/commands.h"
#include "core/fault_report.h"

using namespace emf::core;

namespace {
std::vector<std::string> g_out;
auto reply = [](const std::string& s) { g_out.push_back(s); };
bool said(const std::string& needle) {
    for (const auto& l : g_out)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}
EmfWorld* const kWorld = reinterpret_cast<EmfWorld*>(0x4000);

struct Seen {
    EmfWorld* world = nullptr;
    std::vector<std::string> args;
    int calls = 0;
} g_seen;
void record(EmfWorld* w, int argc, const char* const* argv, void*) {
    g_seen.world = w;
    g_seen.args.assign(argv, argv + argc);
    ++g_seen.calls;
    command_reply_line("did " + std::string(argv[0]));
}
void crash(EmfWorld*, int, const char* const*, void*) { *static_cast<volatile int*>(nullptr) = 1; }
}  // namespace

TEST_CASE("split_command: spaces, quoted arguments and escaped quotes") {
    CHECK(split_command("give Material_T1_Wood_Logs 5") == std::vector<std::string>{"give", "Material_T1_Wood_Logs", "5"});
    CHECK(split_command("  say   \"hello world\"  x ") == std::vector<std::string>{"say", "hello world", "x"});
    CHECK(split_command("note \"a \\\"b\\\"\"") == std::vector<std::string>{"note", "a \"b\""});
    CHECK(split_command("").empty());
    CHECK(split_command("   ").empty());
    CHECK(split_command("x \"\"") == std::vector<std::string>{"x", ""});
}

TEST_CASE("commands: a queued line runs its command with the world and the arguments; replies are collected") {
    CommandCenter c;
    g_out.clear();
    g_seen = {};
    REQUIRE(c.add("Echo", "echo the arguments", record, nullptr, nullptr));
    c.enqueue("echo 1 \"two words\"");
    CHECK(c.pending() == 1);
    CHECK(c.run_pending(kWorld, reply) == 1);
    CHECK(c.pending() == 0);
    CHECK(g_seen.world == kWorld);
    CHECK(g_seen.args == std::vector<std::string>{"echo", "1", "two words"});
    CHECK(said("> echo 1 \"two words\""));  // the line is echoed
    CHECK(said("did echo"));
    command_reply_line("outside a command");  // goes nowhere, no crash
}

TEST_CASE("commands: unknown commands and empty lines reply without running anything") {
    CommandCenter c;
    g_out.clear();
    g_seen = {};
    REQUIRE(c.add("echo", "", record, nullptr, nullptr));
    c.enqueue("nope 1 2");
    c.enqueue("   ");
    CHECK(c.run_pending(kWorld, reply) == 0);
    CHECK(g_seen.calls == 0);
    CHECK(said("unknown command 'nope'"));
}

TEST_CASE("commands: names are checked and unique (case-insensitive)") {
    CommandCenter c;
    std::string why;
    CHECK(c.add("give", "", record, nullptr, &why));
    CHECK_FALSE(c.add("GIVE", "", record, nullptr, &why));
    CHECK(why.find("already") != std::string::npos);
    CHECK_FALSE(c.add("", "", record, nullptr, &why));
    CHECK_FALSE(c.add("has space", "", record, nullptr, &why));
    CHECK_FALSE(c.add(std::string(33, 'a'), "", record, nullptr, &why));
    CHECK_FALSE(c.add("ok", "", nullptr, nullptr, &why));
    CHECK(c.add("mod.cmd-1", "", record, nullptr, &why));
}

TEST_CASE("commands: a crashing command is disabled and reported once; others keep working") {
    CommandCenter c;
    g_out.clear();
    g_seen = {};
    static std::vector<std::string> faults;
    faults.clear();
    set_fault_reporter([](const std::string& s) { faults.push_back(s); });
    REQUIRE(c.add("boom", "", crash, nullptr, nullptr));
    REQUIRE(c.add("echo", "", record, nullptr, nullptr));
    c.enqueue("boom");
    c.enqueue("boom");
    c.enqueue("echo");
    c.run_pending(kWorld, reply);
    set_fault_reporter(nullptr);
    CHECK(faults.size() == 1);
    CHECK(said("'boom' crashed and was disabled"));
    CHECK(said("'boom' is disabled"));
    CHECK(g_seen.calls == 1);
}

TEST_CASE("commands: lines queued from another thread run on the next run_pending; list is sorted") {
    CommandCenter c;
    g_out.clear();
    g_seen = {};
    REQUIRE(c.add("zeta", "last", record, nullptr, nullptr));
    REQUIRE(c.add("alpha", "first", record, nullptr, nullptr));
    std::thread t([&c] {
        for (int i = 0; i < 5; ++i) c.enqueue("alpha");
    });
    t.join();
    CHECK(c.run_pending(kWorld, reply) == 5);
    const auto l = c.list();
    REQUIRE(l.size() == 2);
    CHECK(l[0].first == "alpha");
    CHECK(l[0].second == "first");
    CHECK(l[1].first == "zeta");
}

// ---- edge cases ----
TEST_CASE("commands: a line older than the maximum age is not run; the reply says why") {
    CommandCenter c;
    g_out.clear();
    g_seen = {};
    REQUIRE(c.add("echo", "", record, nullptr, nullptr));
    c.enqueue("echo old", 1000);
    c.enqueue("echo new", 40000);
    CHECK(c.run_pending(kWorld, reply, 41000, 30000) == 1);
    CHECK(g_seen.calls == 1);
    CHECK(g_seen.args == std::vector<std::string>{"echo", "new"});
    CHECK(said("> echo old"));
    CHECK(said("not run: typed 40 s ago"));
}

TEST_CASE("commands: reject_pending drains the queue with a reason and runs nothing") {
    CommandCenter c;
    g_out.clear();
    g_seen = {};
    REQUIRE(c.add("echo", "", record, nullptr, nullptr));
    c.enqueue("echo a");
    c.enqueue("   ");
    c.enqueue("echo b");
    CHECK(c.reject_pending("needs the host", reply) == 2);
    CHECK(c.pending() == 0);
    CHECK(g_seen.calls == 0);
    CHECK(said("> echo a"));
    CHECK(said("> echo b"));
    CHECK(said("needs the host"));
}

TEST_CASE("command_route: the server world runs commands; a client world rejects them only when no server world exists") {
    CHECK(command_route(EMF_WORLD_SERVER, true) == CommandRoute::Run);
    CHECK(command_route(EMF_WORLD_CLIENT, true) == CommandRoute::None);
    CHECK(command_route(EMF_WORLD_CLIENT, false) == CommandRoute::Reject);
    CHECK(command_route(EMF_WORLD_PREDICTION, false) == CommandRoute::None);
}

// ---- commands without a world ----
TEST_CASE("run_without_world runs an 'anywhere' command with no world and answers the others with the reason") {
    CommandCenter c;
    g_out.clear();
    g_seen = {};
    REQUIRE(c.add("help", "", record, nullptr, nullptr, true));  // anywhere
    REQUIRE(c.add("give", "", record, nullptr, nullptr));
    CHECK(c.run_without_world("help", reply, "not run: load a world you host first") == 1);
    CHECK(g_seen.calls == 1);
    CHECK(g_seen.world == nullptr);
    CHECK(said("> help"));
    CHECK(said("did help"));
    g_out.clear();
    CHECK(c.run_without_world("give Torch 1", reply, "not run: load a world you host first") == 0);
    CHECK(g_seen.calls == 1);
    CHECK(said("> give Torch 1"));
    CHECK(said("not run: load a world you host first"));
    g_out.clear();
    CHECK(c.run_without_world("nosuch", reply, "not run: load a world you host first") == 0);
    CHECK(said("unknown command 'nosuch'"));
    CHECK(c.run_without_world("   ", reply, "x") == 0);
    CHECK(c.pending() == 0);
}

namespace {
CommandSender g_sender_seen;
bool g_had_sender = false;
void who(EmfWorld*, int, const char* const*, void*) {
    const CommandSender* s = current_sender();
    g_had_sender = s != nullptr;
    if (s) g_sender_seen = *s;
    command_reply_line("hello sender");
}
}  // namespace

TEST_CASE("commands: a line from chat runs with its sender; its replies also go to the sender's own reply") {
    CommandCenter c;
    REQUIRE(c.add("who", "", who, nullptr, nullptr));
    g_out.clear();
    std::vector<std::string> to_player;
    CommandSender from;
    from.from_chat = true;
    from.account = 0x1122334455667788ull;
    from.handle = 0x83;
    from.entity = 1;
    c.enqueue("who", 1000, from, [&](const std::string& s) { to_player.push_back(s); });
    c.enqueue("who", 1000);  // from the console: no sender
    CHECK(c.run_pending(kWorld, reply, 1000) == 2);
    CHECK(g_had_sender);
    CHECK_FALSE(g_sender_seen.from_chat);  // the last line ran last
    REQUIRE(to_player.size() == 2);       // only the chat line's echo and reply
    CHECK(to_player[0] == "> who");
    CHECK(to_player[1] == "hello sender");
    CHECK(said("hello sender"));  // the log/console still sees every reply
    c.enqueue("who", 1000, from, nullptr);
    c.run_pending(kWorld, reply, 1000);
    CHECK(g_sender_seen.from_chat);
    CHECK(g_sender_seen.account == 0x1122334455667788ull);
    CHECK(g_sender_seen.handle == 0x83u);
    CHECK(g_sender_seen.entity == 1u);
    CHECK(current_sender() == nullptr);  // outside a command
}

namespace {
uint32_t g_you = 0;
void you(EmfWorld*, int, const char* const*, void*) { g_you = command_player(42); }
}  // namespace

TEST_CASE("command_player: 'you' is the chat sender's entity (0 if unknown), never the local player; else the local player") {
    CommandCenter c;
    REQUIRE(c.add("you", "", you, nullptr, nullptr));
    c.enqueue("you", 1000);
    c.run_pending(kWorld, nullptr, 1000);
    CHECK(g_you == 42u);  // console: the local player
    CommandSender p;
    p.from_chat = true;
    p.entity = 3;
    c.enqueue("you", 1000, p);
    c.run_pending(kWorld, nullptr, 1000);
    CHECK(g_you == 3u);
    p.entity = 0;  // a player whose entity EMF could not tell
    c.enqueue("you", 1000, p);
    c.run_pending(kWorld, nullptr, 1000);
    CHECK(g_you == 0u);
}

TEST_CASE("commands: a chat sender's entity is resolved when the line runs (on the world's thread), not when queued") {
    CommandCenter c;
    REQUIRE(c.add("who", "", who, nullptr, nullptr));
    int calls = 0;
    c.set_sender_resolver([&](CommandSender& s) {
        ++calls;
        if (s.account == 0x55) s.entity = 9;
    });
    CommandSender p;
    p.from_chat = true;
    p.account = 0x55;
    c.enqueue("who", 1000, p);
    CHECK(calls == 0);  // queued on the network thread: nothing looked up yet
    c.enqueue("who", 1000);  // the console: no lookup
    c.run_pending(kWorld, nullptr, 1000);
    CHECK(calls == 1);
    CHECK_FALSE(g_sender_seen.from_chat);  // the console line ran last
    c.enqueue("who", 1000, p);
    c.run_pending(kWorld, nullptr, 1000);
    CHECK(g_sender_seen.entity == 9u);
}
