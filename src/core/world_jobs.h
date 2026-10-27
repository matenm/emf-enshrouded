#pragma once
// EMF's tick work waits for the server world's own jobs first (docs/ENGINE_MAP.md 1.1, "Jobs still running after
// SimStep").
//
// On the server world the game's SimStep returns while two of its jobs still run on the game's worker threads: the
// EntityGrid update (it moves and inserts entities between the spatial grid's cells, freeing and allocating buckets)
// and the range queries (they read that grid). The game waits for them later in the same frame: GameServer update calls
// waitForWorldJobs(world) once its other post-tick work is done. EMF runs its tick work (mods' on_event and on_tick,
// commands: spawn, destroy, give, the bridge's world jobs) right after SimStep returns, inside that window, so a spawn
// or destroy there changes the grid under the game's own jobs (a crash seen in game: a range query walked a bucket
// the grid update had just freed). On the server world EMF therefore calls the game's own wait first. The client and
// prediction worlds finish those jobs inside SimStep.
//
// The wait is safe to call early and twice: it helps run the world's task group until nothing is queued or running
// and, with nothing pending, only empties the group's finished-task list and returns. The game calls it from the same
// thread a moment later and then finds nothing to wait for.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace emf::core {

// waitForWorldJobs(world) (signature WaitWorldJobs): mov rcx, [world+0xE15D60] (the world's task group); jmp to the
// task group's wait.
using WaitWorldJobsFn = void (*)(void* world);

// Whether EMF waits before its tick work for a world of this kind: only the server world (kind 0), and only when the
// game's wait was found.
bool should_wait_for_world_jobs(uint8_t kind, bool wait_available);

class WorldJobsGate {
public:
    // Runs the wait; returns false if the call faulted. The default calls it directly (the core passes an SEH guard).
    using Caller = std::function<bool(WaitWorldJobsFn, void* world)>;

    void set_wait(WaitWorldJobsFn fn) { fn_.store(fn); }
    void set_caller(Caller c) { caller_ = std::move(c); }
    void set_log(std::function<void(const std::string&)> log) { log_ = std::move(log); }
    bool available() const { return fn_.load() != nullptr; }

    // After the game's SimStep returned and before EMF's tick work, on that world's own thread. Waits for the world's
    // jobs when should_wait_for_world_jobs says so; returns true if it waited. The first wait is logged once. A caller
    // that reports a fault turns the gate off (logged once); the core's caller passes real faults on to the game's crash
    // handler instead, because the wait runs the game's own tasks.
    bool before_tick_work(void* world, uint8_t kind);
    uint64_t waits() const { return waits_.load(); }

    // The start-up log line: whether mods' tick work waits, and what that means when it does not.
    static std::string startup_line(bool available);

private:
    std::atomic<WaitWorldJobsFn> fn_{nullptr};
    Caller caller_;
    std::function<void(const std::string&)> log_;
    std::atomic<uint64_t> waits_{0};
};

}  // namespace emf::core
