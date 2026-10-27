#include "world_jobs.h"

namespace emf::core {

bool should_wait_for_world_jobs(uint8_t kind, bool wait_available) { return wait_available && kind == 0; }

bool WorldJobsGate::before_tick_work(void* world, uint8_t kind) {
    const WaitWorldJobsFn fn = fn_.load();
    if (!should_wait_for_world_jobs(kind, fn != nullptr)) return false;
    const bool ok = caller_ ? caller_(fn, world) : (fn(world), true);
    if (!ok) {
        fn_.store(nullptr);  // the server world's thread is the only caller: no second fault to report
        if (log_)
            log_("world jobs: the game's waitForWorldJobs faulted; mods' tick work no longer waits for the server "
                 "world's jobs");
        return false;
    }
    if (waits_.fetch_add(1) == 0 && log_)
        log_("world jobs: waited for the server world's jobs before mods' tick work (first wait; from now on every "
             "server tick)");
    return true;
}

std::string WorldJobsGate::startup_line(bool available) {
    if (available)
        return "world jobs: on (mods' tick work on the server world waits for the game's grid update and range "
               "queries)";
    return "world jobs: off (the WaitWorldJobs signature is not usable on this game build): mods' tick work on the "
           "server world may run while the game's grid update and range queries still run, so a spawn or destroy can "
           "race them";
}

}  // namespace emf::core
