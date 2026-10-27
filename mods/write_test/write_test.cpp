// write_test (developer, off by default): checks that API v16 writes reach the players.
//   write_test.faction <entity> <value>          next server step: write the entity's Faction through
//                                                program_write_other (the game marks it changed)
//   write_test.faction <entity> <value> plain    the same through a get_component pointer (not tracked: the control)
// Every client world step it reads the same entity's Faction in the client world and logs when it changes. Seen in a
// game you host: both writes show up in the local client (it follows the server's data either way); the difference
// is for players who joined over the network and for the save (to check on the dedicated server).
#include <windows.h>

#include <atomic>
#include <cstdlib>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"

namespace {
constexpr const char* kMod = "write_test";
namespace ecs = emf::gen::ecs;

std::atomic<uint32_t> g_target{1};        // the entity watched (the local player by default)
std::atomic<int> g_value{-1};             // a pending tracked write (-1: none)
std::atomic<int> g_plain_value{-1};       // a pending plain write
std::atomic<int> g_last_client{-1};       // the client world's last seen value

void on_command(emf::World, const std::vector<std::string>& args) {
    if (args.size() < 3) {
        EMF_REPLY("usage: write_test.faction <entity> <value 0-255> [plain]");
        return;
    }
    g_target = static_cast<uint32_t>(std::strtoul(args[1].c_str(), nullptr, 10));
    const int v = std::atoi(args[2].c_str()) & 0xFF;
    if (args.size() > 3 && args[3] == "plain") g_plain_value = v;
    else g_value = v;
    g_last_client = -1;
    EMF_REPLY("faction %d for entity %u on the next server step (%s)", v, g_target.load(),
              args.size() > 3 ? "plain pointer" : "program_write_other");
}

void program(void* ctx) {
    const int v = g_value.exchange(-1);
    if (v < 0) return;
    auto* f = emf::write_other<ecs::Faction>(ctx, "Faction", g_target);
    if (!f) {
        EMF_LOG(kMod, "program_write_other('Faction', %u) gave nothing (see the lines above)", g_target.load());
        return;
    }
    const int before = static_cast<int>(f->faction);
    f->faction = static_cast<emf::gen::FactionType>(v);
    EMF_LOG(kMod, "server: entity %u faction %d -> %d (tracked)", g_target.load(), before, v);
}

void on_tick(emf::World w) {
    if (w.is_server()) {
        const int v = g_plain_value.exchange(-1);
        if (v >= 0) {
            if (auto* f = w.get<ecs::Faction>(g_target)) {
                const int before = static_cast<int>(f->faction);
                f->faction = static_cast<emf::gen::FactionType>(v);
                EMF_LOG(kMod, "server: entity %u faction %d -> %d (plain pointer)", g_target.load(), before, v);
            }
        }
        return;
    }
    if (!w.is_client()) return;
    const auto* f = w.get<ecs::Faction>(g_target);
    const int now = f ? static_cast<int>(f->faction) : -2;
    if (now != g_last_client.exchange(now)) EMF_LOG(kMod, "client: entity %u faction %d", g_target.load(), now);
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;
    if (!emf::has_v16()) {
        EMF_LOG(kMod, "needs EMF API v16 (program_write_other)");
        return false;
    }
    if (!emf::add_program("write_test", EMF_WORLD_SERVER, {}, {}, {}, {}, {"Faction"}, program)) {
        EMF_LOG(kMod, "the program could not be added (see above)");
        return false;
    }
    emf::add_command("write_test.faction", "<entity> <value> [plain]: write a Faction tracked (or plain) and watch the client", on_command);
    emf::on_tick(on_tick);
    EMF_LOG(kMod, "ready");
    return true;
}
