// lua: the EMF mod that runs Lua mods (emf/mods/<id>/mod.json with "main": "main.lua"). Built only on the public API:
// everything it does goes through src/lua_host (one Lua state per script mod, protected calls, live reload).
// Console: lua.status (every script mod and its state), lua.reload (check the files now).
#include <windows.h>

#include <string>

#include "emf/emf.hpp"
#include "lua_host/host.h"

namespace {
constexpr const char* kMod = "lua";
emf::lua::Host* g_host = nullptr;  // lives for the process (mods are never unloaded)

std::wstring mods_dir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d(exe);
    return d.substr(0, d.find_last_of(L"\\/")) + L"\\emf\\mods";
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;
    if (!emf::has_v17()) {  // emf::init refuses an older core already (lua.dll is built against v17)
        EMF_LOG(kMod, "needs EMF API v17 or newer");
        return false;
    }
    g_host = new emf::lua::Host(api, mods_dir());
    g_host->load_all();  // script mods run now; their events and programs are registered before the first world
    emf::on_tick([](emf::World w) { g_host->tick(w.raw()); });
    emf::add_command("lua.status", ": every Lua mod and whether it runs", [](emf::World, const std::vector<std::string>&) {
        for (const auto& id : g_host->ids()) EMF_REPLY("%s: %s", id.c_str(), g_host->status(id).c_str());
        EMF_REPLY("%zu Lua mod(s)", g_host->script_count());
    });
    emf::add_command("lua.reload", ": reload the Lua mods whose files changed (this also happens every second)",
                     [](emf::World, const std::vector<std::string>&) {
                         g_host->check_reload_now();
                         EMF_REPLY("checked");
                     });
    EMF_LOG(kMod, "ready: %zu Lua mod(s)", g_host->script_count());
    return true;
}
