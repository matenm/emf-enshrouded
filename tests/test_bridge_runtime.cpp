// The bridge's glue to the game, the parts that run without it: state requests sharing one world job, and mod_key's
// press, down and up bookkeeping.
#include <windows.h>

#include <doctest.h>

#include <string>
#include <vector>

#include "core/bridge/runtime_parts.h"

using namespace emf::core;
using namespace emf::core::bridge;

TEST_CASE("bridge runtime: the world slot is forgotten when a new hosted session starts, and hidden in a client") {
    SlotBook b;
    CHECK_FALSE(b.current(std::string("server")).has_value());
    b.loaded(0x0001);
    CHECK(b.current(std::string("server")) == 1u);
    CHECK(b.current(std::nullopt) == 1u);                       // the main menu: the world last loaded or saved
    CHECK_FALSE(b.current(std::string("client")).has_value());  // someone else's game: our old slot means nothing
    b.new_session();                                            // the next hosted world is set up, before its load
    CHECK_FALSE(b.current(std::string("server")).has_value());
    b.saved(0x0101);
    CHECK(b.current(std::string("dedicated")) == 0x0101u);
}

TEST_CASE("bridge runtime: state requests share one world job; its values reach every waiting request") {
    StateBatch b;
    std::vector<std::string> got;
    CHECK(b.add([&](std::string s) { got.push_back("a" + s); }, 1000));  // post a job
    CHECK_FALSE(b.add([&](std::string s) { got.push_back("b" + s); }, 1010));  // one is on its way
    CHECK(b.waiting() == 2);
    CHECK(b.take_ready().empty());
    WorldValues v;
    v.kind = "server";
    v.tick = 77;
    b.fill(v);  // the world's thread
    auto ready = b.take_ready();
    REQUIRE(ready.size() == 2);
    CHECK(ready[0].second.kind == "server");
    CHECK(ready[1].second.tick == 77u);
    ready[0].first("1");
    ready[1].first("2");
    CHECK(got == std::vector<std::string>{"a1", "b2"});
    CHECK(b.waiting() == 0);
    CHECK(b.add([](std::string) {}, 2000));  // the next request posts a new job
}

TEST_CASE("bridge runtime: a world job that never ran (the world stopped) is posted again after 2 s") {
    StateBatch b;
    CHECK(b.add([](std::string) {}, 1000));
    CHECK_FALSE(b.add([](std::string) {}, 2999));
    CHECK(b.add([](std::string) {}, 3000));
}

TEST_CASE("bridge runtime: lines queued for a hosted world that has gone are answered at once (needs a world)") {
    CommandCenter c;
    std::vector<CommandOutcome> outcomes;
    std::vector<std::string> sink;
    c.enqueue("pos", 1000, {}, {}, [&](CommandOutcome o) { outcomes.push_back(o); });
    CHECK(answer_lines_without_world(c, HostState::Hosting, [&](const std::string& s) { sink.push_back(s); }) == 0);
    CHECK(outcomes.empty());  // the world still runs: the line waits for its tick
    CHECK(answer_lines_without_world(c, HostState::None, [&](const std::string& s) { sink.push_back(s); }) == 1);
    REQUIRE(outcomes.size() == 1);
    CHECK(outcomes[0] == CommandOutcome::NeedsWorld);
    CHECK(sink.back() == "not run: no world you host is running any more");
    CHECK(c.pending() == 0);
}

TEST_CASE("bridge runtime: mod_key press answers after the release with the mod reads; up answers a waiting press") {
    InjectedKeys keys;
    PressBook presses;
    std::vector<KeyResult> answers;
    auto done = [&](const KeyResult& r) { answers.push_back(r); };
    CHECK_FALSE(mod_key_action(keys, presses, 'L', KeyAction::Press, 150, done, 1000).refused());
    CHECK(answers.empty());
    CHECK(presses.size() == 1);
    keys.read('L', 1010);
    keys.read('L', 1020);
    release_due(keys, presses, 1149);
    CHECK(answers.empty());
    release_due(keys, presses, 1150);
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].ok);
    CHECK(answers[0].mod_reads == 2);
    CHECK(presses.size() == 0);

    CHECK_FALSE(mod_key_action(keys, presses, 'K', KeyAction::Press, 5000, done, 2000).refused());
    const Refusal again = mod_key_action(keys, presses, 'K', KeyAction::Press, 100, done, 2001);
    CHECK(again.refused());
    CHECK(again.code == "busy");
    keys.read('K', 2002);
    CHECK_FALSE(mod_key_action(keys, presses, 'K', KeyAction::Up, 0, done, 2003).refused());
    REQUIRE(answers.size() == 3);  // the waiting press and the up
    CHECK(answers[1].mod_reads == 1);
    CHECK(answers[2].mod_reads == 1);
    CHECK(presses.size() == 0);
    CHECK_FALSE(keys.any());

    CHECK_FALSE(mod_key_action(keys, presses, VK_F6, KeyAction::Down, 0, done, 3000).refused());
    CHECK(answers.size() == 4);  // down answers at once
    CHECK(keys.held(VK_F6, 3001));
    release_due(keys, presses, 3000 + InjectedKeys::kMaxDownMs);  // a down nobody released: let go after a minute
    CHECK_FALSE(keys.held(VK_F6, 3000 + InjectedKeys::kMaxDownMs));
    CHECK(answers.size() == 4);
}
