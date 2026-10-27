// The wait for the server world's own jobs before EMF's tick work (core/world_jobs.h, docs/ENGINE_MAP.md 1.1).
#include <doctest.h>

#include <string>
#include <vector>

#include "core/world_jobs.h"

using emf::core::should_wait_for_world_jobs;
using emf::core::WorldJobsGate;

namespace {
std::vector<void*> g_waited;  // the worlds the fake wait was called with
void fake_wait(void* world) { g_waited.push_back(world); }
void* const kWorld = reinterpret_cast<void*>(0x3006b0bed80ull);
}  // namespace

TEST_CASE("world jobs: only the server world waits, and only when the game's wait was found") {
    CHECK(should_wait_for_world_jobs(0, true));         // server: its grid update and range queries may still run
    CHECK_FALSE(should_wait_for_world_jobs(0, false));  // signature missing: today's behaviour
    CHECK_FALSE(should_wait_for_world_jobs(1, true));   // prediction: those jobs run inline inside SimStep
    CHECK_FALSE(should_wait_for_world_jobs(2, true));   // client: SimStep waits for its range queries itself
    CHECK_FALSE(should_wait_for_world_jobs(3, true));   // not a world kind
}

TEST_CASE("world jobs gate: waits with the world on the server world, never on the client or prediction world") {
    g_waited.clear();
    WorldJobsGate gate;
    gate.set_wait(&fake_wait);
    CHECK(gate.available());
    CHECK(gate.before_tick_work(kWorld, 0));
    REQUIRE(g_waited.size() == 1);
    CHECK(g_waited[0] == kWorld);
    CHECK_FALSE(gate.before_tick_work(kWorld, 2));
    CHECK_FALSE(gate.before_tick_work(kWorld, 1));
    CHECK(g_waited.size() == 1);
    CHECK(gate.before_tick_work(kWorld, 0));  // every server tick, not just the first
    CHECK(g_waited.size() == 2);
    CHECK(gate.waits() == 2);
}

TEST_CASE("world jobs gate: without the signature nothing is called (EMF behaves as before)") {
    g_waited.clear();
    WorldJobsGate gate;
    CHECK_FALSE(gate.available());
    CHECK_FALSE(gate.before_tick_work(kWorld, 0));
    gate.set_wait(nullptr);
    CHECK_FALSE(gate.before_tick_work(kWorld, 0));
    CHECK(g_waited.empty());
    CHECK(gate.waits() == 0);
}

TEST_CASE("world jobs gate: the first wait is logged once, so a run's log shows the wait active") {
    g_waited.clear();
    std::vector<std::string> log;
    WorldJobsGate gate;
    gate.set_wait(&fake_wait);
    gate.set_log([&](const std::string& s) { log.push_back(s); });
    gate.before_tick_work(kWorld, 2);
    CHECK(log.empty());  // a client tick is not a wait
    gate.before_tick_work(kWorld, 0);
    gate.before_tick_work(kWorld, 0);
    gate.before_tick_work(kWorld, 0);
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "world jobs: waited for the server world's jobs before mods' tick work (first wait; from now on "
                    "every server tick)");
}

TEST_CASE("world jobs gate: a wait that faults turns the gate off, logged once") {
    g_waited.clear();
    std::vector<std::string> log;
    int calls = 0;
    WorldJobsGate gate;
    gate.set_wait(&fake_wait);
    gate.set_log([&](const std::string& s) { log.push_back(s); });
    gate.set_caller([&](emf::core::WaitWorldJobsFn, void*) {
        ++calls;
        return false;  // the call faulted
    });
    CHECK_FALSE(gate.before_tick_work(kWorld, 0));
    CHECK_FALSE(gate.available());
    CHECK_FALSE(gate.before_tick_work(kWorld, 0));
    CHECK(calls == 1);
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "world jobs: the game's waitForWorldJobs faulted; mods' tick work no longer waits for the server "
                    "world's jobs");
    CHECK(gate.waits() == 0);
}

TEST_CASE("world jobs gate: the custom caller gets the game's function and the world") {
    WorldJobsGate gate;
    gate.set_wait(&fake_wait);
    emf::core::WaitWorldJobsFn seen_fn = nullptr;
    void* seen_world = nullptr;
    gate.set_caller([&](emf::core::WaitWorldJobsFn fn, void* w) {
        seen_fn = fn;
        seen_world = w;
        return true;
    });
    CHECK(gate.before_tick_work(kWorld, 0));
    CHECK(seen_fn == &fake_wait);
    CHECK(seen_world == kWorld);
}

TEST_CASE("world jobs: the start-up line says whether mods' tick work waits") {
    CHECK(WorldJobsGate::startup_line(true) ==
          "world jobs: on (mods' tick work on the server world waits for the game's grid update and range queries)");
    CHECK(WorldJobsGate::startup_line(false) ==
          "world jobs: off (the WaitWorldJobs signature is not usable on this game build): mods' tick work on the "
          "server world may run while the game's grid update and range queries still run, so a spawn or destroy can "
          "race them");
}
