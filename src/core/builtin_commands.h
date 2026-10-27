#pragma once
// The core's own commands: help, mods, settings, set, items, give, put, spawn, time, pos, goto, entities, inspect, tpl, tplset, stack, destroy.
// Built on the public API.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "commands.h"
#include "emf/emf_api.h"

namespace emf::core {

class ModRegistry;
class SettingsStore;

struct BuiltinContext {
    const EmfApi* api = nullptr;
    CommandCenter* center = nullptr;
    std::function<uint32_t()> player;                  // the local player's entity id
    ModRegistry* registry = nullptr;                   // the mods found and their status ("mods")
    SettingsStore* settings = nullptr;                 // mods' settings ("settings", "set")
    // (put's and take's result, one server step later, goes to the reply the command had: current_reply())
};

// Registers the built-in commands; `ctx` must stay alive as long as they can run. False if one could not be added.
bool add_builtin_commands(BuiltinContext& ctx);

}  // namespace emf::core
