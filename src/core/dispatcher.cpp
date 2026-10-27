#include "dispatcher.h"

#include "fault_report.h"

namespace emf::core {
namespace {
thread_local EmfWorld* t_dispatching = nullptr;

struct DispatchScope {
    EmfWorld* previous;
    explicit DispatchScope(EmfWorld* w) : previous(t_dispatching) { t_dispatching = w; }
    ~DispatchScope() { t_dispatching = previous; }
};
}  // namespace

EmfWorld* dispatching_world() { return t_dispatching; }

bool guarded_world_call(EmfWorldCallback cb, EmfWorld* world, void* user) {
    __try {
        cb(world, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Dispatcher::subscribe_tick(EmfWorldCallback cb, void* user) {
    if (!cb) return;
    AcquireSRWLockExclusive(&lock_);
    subscribers_.push_back({cb, user, 0, true});
    ReleaseSRWLockExclusive(&lock_);
}

void Dispatcher::post(uint8_t kind, EmfWorldCallback cb, void* user) {
    if (!cb) return;
    AcquireSRWLockExclusive(&lock_);
    queue_.push_back({cb, user, kind, true});
    ReleaseSRWLockExclusive(&lock_);
}

size_t Dispatcher::active_subscribers() const {
    AcquireSRWLockShared(&lock_);
    size_t n = 0;
    for (const auto& s : subscribers_) n += s.enabled;
    ReleaseSRWLockShared(&lock_);
    return n;
}

void Dispatcher::run(EmfWorld* world, uint8_t kind, const ErrorSink& on_error) {
    // Take this kind's queued items and a snapshot of subscribers under the lock, run them outside it.
    std::vector<Item> due;
    std::vector<size_t> subs;
    AcquireSRWLockExclusive(&lock_);
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->kind == kind) {
            due.push_back(*it);
            it = queue_.erase(it);
        } else {
            ++it;
        }
    }
    for (size_t i = 0; i < subscribers_.size(); ++i)
        if (subscribers_[i].enabled) subs.push_back(i);
    std::vector<Item> sub_items;
    for (size_t i : subs) sub_items.push_back(subscribers_[i]);
    ReleaseSRWLockExclusive(&lock_);

    const DispatchScope scope(world);
    for (const auto& item : due) {
        if (!guarded_world_call(item.cb, world, item.user))
            on_error("posted callback in " + module_name_of(reinterpret_cast<const void*>(item.cb)) +
                     " faulted (world kind " + std::to_string(kind) + ")");
    }
    for (size_t k = 0; k < sub_items.size(); ++k) {
        if (guarded_world_call(sub_items[k].cb, world, sub_items[k].user)) continue;
        AcquireSRWLockExclusive(&lock_);
        subscribers_[subs[k]].enabled = false;
        ReleaseSRWLockExclusive(&lock_);
        on_error("tick callback in " + module_name_of(reinterpret_cast<const void*>(sub_items[k].cb)) +
                 " faulted and was disabled (world kind " + std::to_string(kind) + ")");
    }
}

}  // namespace emf::core
