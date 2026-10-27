#pragma once
#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "programs.h"

namespace emf::core {

// Remembers every program's update function as worlds register them (hook on registerProgram)
// and installs program hooks requested by mods, immediately or once the program registers.
class ProgramRegistry {
public:
    using Installer = std::function<bool(uintptr_t target, void* detour, void** original)>;
    enum class Result { Installed, Pending, Rejected, Failed };

    explicit ProgramRegistry(Installer installer) : installer_(std::move(installer)) {}
    // Told after each install attempt (at once, or when a pending request's program registers): program name, success.
    // Set it before requesting hooks.
    void set_report(std::function<void(const std::string& name, bool installed)> report) { report_ = std::move(report); }

    void on_program_registered(const std::string& name, uintptr_t update_fn);
    Result request_hook(const std::string& name, ProgramCallback pre, ProgramCallback post, void* user);
    size_t known_programs() const;
    bool shares_update_fn(const std::string& name) const;
    // Names of pending hook requests not reported yet (call after a world registered all its programs).
    std::vector<std::string> take_unreported_pending();

private:
    struct Request {
        std::string name;
        ProgramCallback pre;
        ProgramCallback post;
        void* user;
    };
    Result install(uintptr_t fn, const Request& req);

    Installer installer_;
    std::function<void(const std::string&, bool)> report_;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::map<std::string, uintptr_t> fns_;
    std::vector<Request> pending_;
    std::set<std::string> reported_;
};

}  // namespace emf::core
