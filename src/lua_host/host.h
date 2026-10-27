#pragma once
// The Lua host: runs script mods (emf/mods/<id>/mod.json with "main": "main.lua") in one lua_State each, on the public
// EMF API only. Every call into Lua is protected and limited to 250 ms; a failing script is paused until its files
// change; changed files reload the script while the game runs. Lua runs only inside load_all and EMF callbacks (one
// mutex per script).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "emf/emf_api.h"

namespace emf::lua {

// What one script's Lua state may allocate; past it the call fails with "not enough memory" and the script pauses.
constexpr size_t kMemoryLimit = 256u << 20;

class Host {
public:
    Host(const EmfApi* api, std::wstring mods_dir);
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // From the host mod's emf_mod_load: runs every script mod the loader lists (mod_info_at, EMF_MOD_SCRIPT), then
    // subscribes the engine events and registers the event program the scripts asked for (load time only).
    void load_all();
    // After every world step (the host mod's on_tick): on_tick callbacks, and at most once a second a reload check.
    void tick(EmfWorld* world);
    // Reloads every script mod whose files changed since it was loaded.
    void check_reload_now();
    size_t script_count() const;
    std::vector<std::string> ids() const;  // every script mod's id, in load order
    std::string status(std::string_view id) const;  // "running", "paused: <error>", "not loaded"
    // The memory limit per script for states made from now on (default kMemoryLimit; tests use a small one).
    void set_memory_limit(size_t bytes);
    // How emf.on_key and emf.key_down read a key (default: held, with the game window in front; tests use a fake).
    void set_key_reader(std::function<bool(uint32_t vk)> down);
    // The clock (ms) of the scripts' chat line limit (emf.tell / emf.broadcast: 30 lines per 10 s) and of the 250 ms
    // time limit per call (default GetTickCount64; tests use a fake).
    void set_clock(std::function<uint64_t()> now_ms);

    struct Impl;

private:
    void tick_body(EmfWorld* world);
    std::unique_ptr<Impl> impl_;
};

}  // namespace emf::lua
