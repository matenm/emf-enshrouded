#include "program_registry.h"

namespace emf::core {

ProgramRegistry::Result ProgramRegistry::install(uintptr_t fn, const Request& req) {
    size_t slot = 0;
    void* detour = claim_program_hook(fn, req.pre, req.post, req.user, &slot);
    // pool full or this function is already hooked; else MinHook's answer
    const bool ok = detour && installer_(fn, detour, program_hook_original(slot));
    if (report_) report_(req.name, ok);
    return ok ? Result::Installed : Result::Failed;
}

void ProgramRegistry::on_program_registered(const std::string& name, uintptr_t update_fn) {
    if (name.empty() || !update_fn) return;
    std::vector<Request> ready;
    AcquireSRWLockExclusive(&lock_);
    const bool first = fns_.emplace(name, update_fn).second;
    if (first) {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->name == name) {
                ready.push_back(*it);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    ReleaseSRWLockExclusive(&lock_);
    for (const auto& r : ready) install(update_fn, r);
}

ProgramRegistry::Result ProgramRegistry::request_hook(const std::string& name, ProgramCallback pre,
                                                      ProgramCallback post, void* user) {
    if (name.empty() || (!pre && !post)) return Result::Rejected;
    AcquireSRWLockExclusive(&lock_);
    auto it = fns_.find(name);
    if (it == fns_.end()) {
        pending_.push_back({name, pre, post, user});
        ReleaseSRWLockExclusive(&lock_);
        return Result::Pending;
    }
    const uintptr_t fn = it->second;
    ReleaseSRWLockExclusive(&lock_);
    return install(fn, {name, pre, post, user});
}

std::vector<std::string> ProgramRegistry::take_unreported_pending() {
    std::vector<std::string> names;
    AcquireSRWLockExclusive(&lock_);
    for (const auto& r : pending_)
        if (reported_.insert(r.name).second) names.push_back(r.name);
    ReleaseSRWLockExclusive(&lock_);
    return names;
}

size_t ProgramRegistry::known_programs() const {
    AcquireSRWLockShared(&lock_);
    size_t n = fns_.size();
    ReleaseSRWLockShared(&lock_);
    return n;
}

bool ProgramRegistry::shares_update_fn(const std::string& name) const {
    AcquireSRWLockShared(&lock_);
    bool shared = false;
    auto it = fns_.find(name);
    if (it != fns_.end())
        for (const auto& [other, fn] : fns_)
            if (other != name && fn == it->second) shared = true;
    ReleaseSRWLockShared(&lock_);
    return shared;
}

}  // namespace emf::core
