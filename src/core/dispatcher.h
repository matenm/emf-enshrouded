#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "emf/emf_api.h"

namespace emf::core {

using ErrorSink = std::function<void(const std::string&)>;

// Runs mod callbacks right after a world's simulation step.
//  - post(): one-shot work for the next step of a world of that kind (any thread).
//  - subscribe_tick(): called after every step of every world.
// A callback that faults (SEH) is reported; a faulting tick subscriber is disabled.
class Dispatcher {
public:
    void subscribe_tick(EmfWorldCallback cb, void* user);
    void post(uint8_t kind, EmfWorldCallback cb, void* user);
    void run(EmfWorld* world, uint8_t kind, const ErrorSink& on_error);
    size_t active_subscribers() const;

private:
    struct Item {
        EmfWorldCallback cb;
        void* user;
        uint8_t kind;
        bool enabled;
    };
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<Item> queue_;
    std::vector<Item> subscribers_;
};

// The world whose callbacks Dispatcher::run is running on this thread right now, or nullptr.
EmfWorld* dispatching_world();

// Calls cb(world, user) under an SEH guard. Returns false if it faulted.
bool guarded_world_call(EmfWorldCallback cb, EmfWorld* world, void* user);

}  // namespace emf::core
