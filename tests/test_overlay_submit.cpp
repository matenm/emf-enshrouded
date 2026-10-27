#include <doctest.h>

#include <string>
#include <vector>

#include "core/commands.h"
#include "core/overlay/submit.h"
#include "core/world_tracker.h"

using namespace emf::core;

namespace {
std::vector<std::string> g_sub;
void sub_reply(const std::string& s) { g_sub.push_back(s); }
bool sub_said(const std::string& n) {
    for (const auto& l : g_sub)
        if (l.find(n) != std::string::npos) return true;
    return false;
}
void sub_cmd(EmfWorld*, int, const char* const*, void*) { command_reply_line("ran"); }
EmfWorld* const kServer = reinterpret_cast<EmfWorld*>(0x1000);
EmfWorld* const kClient = reinterpret_cast<EmfWorld*>(0x2000);
}  // namespace

TEST_CASE("host_state: a server world seen within 10 s is hosting; else a client world means someone else's game") {
    WorldTracker t;
    CHECK(host_state(t, 50000) == HostState::None);
    t.on_tick(kServer, EMF_WORLD_SERVER, 50000);
    t.on_tick(kClient, EMF_WORLD_CLIENT, 50000);
    CHECK(host_state(t, 55000) == HostState::Hosting);
    CHECK(host_state(t, 61000) == HostState::None);  // both stale
    t.on_tick(kClient, EMF_WORLD_CLIENT, 60500);
    CHECK(host_state(t, 61000) == HostState::Client);
}

TEST_CASE("submit_line: hosting queues for the server tick; otherwise it answers at once") {
    CommandCenter c;
    REQUIRE(c.add("time", "", sub_cmd, nullptr, nullptr));
    REQUIRE(c.add("help", "", sub_cmd, nullptr, nullptr, true));
    g_sub.clear();
    submit_line(c, HostState::Hosting, "time 12", sub_reply);
    CHECK(c.pending() == 1);
    CHECK(g_sub.empty());
    c.reset();
    REQUIRE(c.add("time", "", sub_cmd, nullptr, nullptr));
    REQUIRE(c.add("help", "", sub_cmd, nullptr, nullptr, true));
    submit_line(c, HostState::None, "time 12", sub_reply);
    CHECK(c.pending() == 0);
    CHECK(sub_said("not run: load a world you host first"));
    g_sub.clear();
    submit_line(c, HostState::Client, "time 12", sub_reply);
    CHECK(sub_said("not run: commands need a world you host (this is someone else's game)"));
    g_sub.clear();
    submit_line(c, HostState::None, "help", sub_reply);
    CHECK(sub_said("ran"));
}
