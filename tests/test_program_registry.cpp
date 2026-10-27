#include <doctest.h>

#include <string>
#include <vector>

#include "core/program_registry.h"
#include "core/programs.h"

using namespace emf::core;

namespace {
std::vector<uintptr_t> g_installed;
bool fake_installer(uintptr_t target, void* detour, void** original) {
    g_installed.push_back(target);
    *original = detour;  // pretend: MinHook writes the trampoline-to-original here
    return true;
}
void pre_cb(void*, void*) {}
}  // namespace

TEST_CASE("a hook requested before the program registers is installed when it registers") {
    reset_program_hooks();
    g_installed.clear();
    ProgramRegistry r(fake_installer);
    CHECK(r.request_hook("player_state", pre_cb, nullptr, nullptr) == ProgramRegistry::Result::Pending);
    CHECK(g_installed.empty());
    r.on_program_registered("death", 0x1111);
    CHECK(g_installed.empty());
    r.on_program_registered("player_state", 0x2222);
    REQUIRE(g_installed.size() == 1);
    CHECK(g_installed[0] == 0x2222);
    r.on_program_registered("player_state", 0x2222);  // client world registers the same program again
    CHECK(g_installed.size() == 1);
}

TEST_CASE("a hook requested after the program registered is installed immediately") {
    reset_program_hooks();
    g_installed.clear();
    ProgramRegistry r(fake_installer);
    r.on_program_registered("death", 0x3333);
    CHECK(r.request_hook("death", pre_cb, nullptr, nullptr) == ProgramRegistry::Result::Installed);
    CHECK(g_installed == std::vector<uintptr_t>{0x3333});
    CHECK(r.known_programs() == 1);
}

TEST_CASE("request_hook rejects empty names and missing callbacks") {
    ProgramRegistry r(fake_installer);
    CHECK(r.request_hook("", pre_cb, nullptr, nullptr) == ProgramRegistry::Result::Rejected);
    CHECK(r.request_hook("death", nullptr, nullptr, nullptr) == ProgramRegistry::Result::Rejected);
}

TEST_CASE("two programs sharing one update function are reported, not double-hooked") {
    reset_program_hooks();
    g_installed.clear();
    ProgramRegistry r(fake_installer);
    r.on_program_registered("server_update_daytime", 0x4444);
    r.on_program_registered("client_update_daytime", 0x4444);  // identical code folded by the linker
    CHECK(r.request_hook("server_update_daytime", pre_cb, nullptr, nullptr) == ProgramRegistry::Result::Installed);
    CHECK(r.request_hook("client_update_daytime", pre_cb, nullptr, nullptr) == ProgramRegistry::Result::Failed);
    CHECK(r.shares_update_fn("client_update_daytime"));
    CHECK(g_installed.size() == 1);
}

TEST_CASE("take_unreported_pending names hook requests that no registered program matched, once") {
    reset_program_hooks();
    g_installed.clear();
    ProgramRegistry r(fake_installer);
    CHECK(r.request_hook("client_player_stat", pre_cb, nullptr, nullptr) == ProgramRegistry::Result::Pending);  // typo
    CHECK(r.request_hook("death", pre_cb, nullptr, nullptr) == ProgramRegistry::Result::Pending);
    r.on_program_registered("death", 0x5555);
    CHECK(r.take_unreported_pending() == std::vector<std::string>{"client_player_stat"});
    CHECK(r.take_unreported_pending().empty());  // reported only once
}

TEST_CASE("program registry: every install attempt is reported with the program's name, a failure too") {
    reset_program_hooks();
    std::vector<std::pair<std::string, bool>> seen;
    ProgramRegistry r([](uintptr_t, void*, void**) { return false; });  // MinHook says no
    r.set_report([&](const std::string& n, bool ok) { seen.emplace_back(n, ok); });
    CHECK(r.request_hook("impact_hit_conclusion", pre_cb, nullptr, nullptr) == ProgramRegistry::Result::Pending);
    CHECK(seen.empty());
    r.on_program_registered("impact_hit_conclusion", 0x4444);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == std::make_pair(std::string("impact_hit_conclusion"), false));
    reset_program_hooks();
    ProgramRegistry ok(fake_installer);
    ok.set_report([&](const std::string& n, bool installed) { seen.emplace_back(n, installed); });
    ok.on_program_registered("clear_hit_candidates", 0x5555);
    CHECK(ok.request_hook("clear_hit_candidates", pre_cb, nullptr, nullptr) == ProgramRegistry::Result::Installed);
    REQUIRE(seen.size() == 2);
    CHECK(seen[1] == std::make_pair(std::string("clear_hit_candidates"), true));
}
