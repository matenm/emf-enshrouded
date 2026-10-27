#pragma once
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

#include "common/world_view.h"

namespace emf::probe {

// WorldPosition components are 32.32 fixed point.
double fixed_to_meters(int64_t v);

// Reads the villager context passed to SetState: entity id and current state byte.
// ctx+0x000 = ProgramContext* (pc[0] = batch Q, pc+8 = u32 1-based entity index),
// entity id = *(u32*)(*(u64*)(Q+0x828) + (index-1)*16), ctx+0x1B0 -> DynamicActiveNpcState*.
bool npc_ctx_info(const void* ctx, uint32_t* entity_id, uint8_t* current_state);

using LineSink = std::function<void(const std::string&)>;

// Decides when to report and what. Thread-safe (server and client worlds tick on different threads).
class Reporter {
public:
    explicit Reporter(GetComponentFn get_component, uint64_t interval_ms = 2000)
        : get_component_(get_component), interval_ms_(interval_ms) {}
    // Call after every sim step of `world`, with a wall-clock time in milliseconds.
    void on_tick(const WorldView& world, uint64_t now_ms, const LineSink& sink);

private:
    void report_client(const WorldView& w, const LineSink& sink);
    void report_server(const WorldView& w, const LineSink& sink);

    GetComponentFn get_component_;
    uint64_t interval_ms_;
    SRWLOCK lock_ = SRWLOCK_INIT;
    uint64_t last_report_ms_[3] = {0, 0, 0};
    bool reported_once_[3] = {false, false, false};
    uint64_t first_client_ms_ = 0;
    bool server_seen_ = false;
    bool warned_no_server_ = false;
};

}  // namespace emf::probe
