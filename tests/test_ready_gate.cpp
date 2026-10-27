// The gate that makes the dedicated server's first world setup wait until EMF has loaded the mods.
#include <doctest.h>

#include <chrono>
#include <thread>

#include "core/ready_gate.h"

using emf::core::ReadyGate;
using namespace std::chrono_literals;

TEST_CASE("ready gate: a wait returns once another thread opens it") {
    ReadyGate g;
    std::thread opener([&] {
        std::this_thread::sleep_for(50ms);
        g.open();
    });
    const auto t0 = std::chrono::steady_clock::now();
    const auto w = g.wait(5000ms);
    CHECK((w == ReadyGate::Wait::Opened || w == ReadyGate::Wait::AlreadyOpen));  // the opener may be first
    CHECK(std::chrono::steady_clock::now() - t0 < 4000ms);
    opener.join();
    CHECK(g.is_open());
}

TEST_CASE("ready gate: a wait on an open gate does not block") {
    ReadyGate g;
    g.open();
    CHECK(g.wait(0ms) == ReadyGate::Wait::AlreadyOpen);
}

TEST_CASE("ready gate: a wait gives up after its timeout") {
    ReadyGate g;
    CHECK(g.wait(30ms) == ReadyGate::Wait::TimedOut);
    CHECK_FALSE(g.is_open());
}

TEST_CASE("ready gate: after one wait timed out, later waits return at once until it opens") {
    ReadyGate g;  // emf_core_start never returned: every later world setup would wait the whole time again
    CHECK(g.wait(30ms) == ReadyGate::Wait::TimedOut);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(g.wait(2000ms) == ReadyGate::Wait::GaveUp);
    CHECK(std::chrono::steady_clock::now() - t0 < 1000ms);
    CHECK_FALSE(g.is_open());
    g.open();  // it may still finish late
    CHECK(g.wait(0ms) == ReadyGate::Wait::AlreadyOpen);
}

TEST_CASE("ready gate: the thread that will open it never waits for itself") {
    ReadyGate g;
    g.set_opener(std::this_thread::get_id());
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(g.wait(5000ms) == ReadyGate::Wait::OpenerThread);
    CHECK(std::chrono::steady_clock::now() - t0 < 1000ms);
}
