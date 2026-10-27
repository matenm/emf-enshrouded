#pragma once
// Internals shared by host.cpp (lifecycle, safety, the basic emf.* functions) and bindings.cpp (worlds, components,
// items, events, the wheel).
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/manifest.h"
#include "emf/emf_api.h"
#include "emf/storage.hpp"
#include "host.h"
// Lua is compiled as C++ (CMakeLists.txt): its functions have C++ linkage and errors are exceptions.
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

namespace emf::lua {

// What a script's Lua state has allocated (changed only under the script's lock, like everything in the state).
struct Memory {
    size_t used = 0;
    size_t limit = kMemoryLimit;
    bool armed = false;  // the limit holds only inside protected calls: a refusal elsewhere would end the game
};

// One emf.on_key: a key (fixed, or one of the mod's keybind settings, read at every poll) and its function.
struct KeyWatch {
    std::string setting;      // a keybind setting of the mod ("" = the fixed key below)
    uint32_t vk = 0;          // the fixed key
    int ref = LUA_NOREF;      // the function
    int kind = -1;            // EMF_WORLD_SERVER / EMF_WORLD_CLIENT, or -1: the server world when this game hosts one
    bool was_down = false;    // at the last poll
    uint64_t pressed_at = 0;  // GetTickCount64 of a press not handed to the function yet (0: none)
};

struct Script {
    Host::Impl* host = nullptr;
    Memory mem;
    std::string id, main;
    std::wstring folder;  // <mods>\<id>
    std::recursive_mutex m;
    lua_State* L = nullptr;
    bool paused = false;
    std::string error;
    std::map<std::wstring, uint64_t> mtimes;  // the mod's files when it was loaded
    std::map<std::string, core::SettingType> setting_types;
    // registry references of Lua functions
    std::vector<int> ticks, setting_cbs;
    std::map<std::string, int> commands;               // full command name (lower case) -> function
    std::map<std::string, std::vector<int>> events;    // "<kind>:<EventName>" -> functions
    std::set<std::string> hud;                         // full HUD ids shown
    std::vector<KeyWatch> keys;                        // emf.on_key
    std::vector<int> chat_cbs;                         // emf.on_chat (lua 1.6)
    // While a take's callback runs: the taken items not handed on yet (emf.put / emf.give of that item lower it). If the
    // callback raises, what is left goes back into the source.
    struct TakeHand {
        bool active = false;
        uint32_t item = 0;
        uint32_t left = 0;
    } hand;
    uint64_t chat_window = 0;                         // emf.tell / emf.broadcast: start of the 10 s window
    int chat_lines = 0;                                // lines sent in it
    uint64_t call_start = 0;                           // the host clock at the start of the current call (moved on
                                                       // by the time its world-data writes took, up to 1 s)
    uint64_t call_credit = 0;                          // how far call_start was moved on in this call
    std::string timeout;                               // the time limit's message with file:line, while it unwinds
    std::string not_run;                               // the core did not keep it as a script mod: never runs again
    uint64_t generation = 1;                         // worlds and proxies made in an older generation are stale
    uint64_t loads = 0;                                // +1 per load: answers meant for an older load are dropped
    // data per world: <game>\emf\config\<id>\<slot>.txt (lives as long as the script; attached once)
    std::unique_ptr<emf::WorldFile> file;
    int world_cb = LUA_NOREF;      // emf.on_world_data's function
    uint64_t world_delivered = 0;  // the load (loads) the world's text was last handed to
    bool world_pending = false;    // on_world_data came after that: the next server tick hands the text to it
};

struct QueuedEvent {
    std::string name;
    struct Field {
        std::string name;
        int kind;  // 0 bool, 1 integer, 2 number, 3 string
        bool b;
        int64_t i;
        double n;
        std::string s;
    };
    std::vector<Field> values;
};

// One mod's part of a wheel group (lua 1.9 emf.wheel_section): its labels as published last and its pick function
// (a registry reference in that mod's state).
struct WheelSection {
    std::string script;  // the mod's id
    uint64_t load = 0;   // the mod's load that published it (an older load's section is never shown or called)
    std::string title;
    std::vector<std::string> labels;
    int pick = LUA_NOREF;
};

struct Host::Impl {
    const EmfApi* api = nullptr;
    std::wstring mods_dir;
    std::vector<std::unique_ptr<Script>> scripts;
    std::atomic<uint64_t> last_check{0};  // GetTickCount64 of the last reload check (tick runs on several world threads)
    size_t memory_limit = kMemoryLimit;   // per script, for states made from now on
    std::mutex m;  // the maps below
    std::map<std::string, std::vector<WheelSection>> wheel_groups;  // group name -> sections in publishing order
    std::map<std::string, std::string> command_owner;  // full command name (lower case) -> script id
    std::map<std::string, void*> command_slots;        // full command name (lower case) -> its CommandSlot (the user)
    std::set<std::string> wanted;                      // "<kind>:<EventName>" the scripts asked for while loading
    std::set<std::string> subscribed;                  // "<kind>:<EventName>" subscribed with on_event
    std::set<std::string> emits[3];                    // per world kind: events the scripts send
    bool load_done = false;                            // after load_all: no new subscriptions
    std::atomic<bool> rechecked{false};                // the first tick compared the scripts with the core's list
    std::deque<std::pair<uint8_t, QueuedEvent>> outbox;  // (world kind, event) waiting for the host's program
    std::function<bool(uint32_t)> key_down;              // a key is held and the game is in front (tests replace it)
    std::atomic<uint64_t> server_seen{0};                // GetTickCount64 of the last server world step
    std::atomic<int> chat_hooked{0};                     // the core's on_chat for emf.on_chat: 0 not asked, 1 on, -1 refused
    std::atomic<int> moves{0};                           // emf.move requests in flight (each holds one place in the item queue)
    std::function<uint64_t()> now_ms;                    // the clock of the chat limit and time limit (tests replace it)

    Script* find(const std::string& id);
    bool load(Script& s);
    void unload(Script& s);
    void pause(Script& s, const std::string& why);
    void drop_sections(const std::string& id);  // the mod's wheel sections leave every group (unload, pause)
    // Stops scripts whose mod the core no longer lists as a script mod (a dependency failed after the host loaded).
    void recheck();
};

Script* script_of(lua_State* L);
// Calls the function under the arguments on the stack (protected, 250 ms limit). False: the script is paused.
bool call_lua(Script& s, int nargs);
// The world userdata passed to callbacks (valid until the callback returns).
void push_world(lua_State* L, Script& s, EmfWorld* w);
// Adds the world/component/item/event/wheel functions to the emf table on top of the stack.
void open_bindings(lua_State* L, Script& s);
// After the first load of every script: subscribes the wanted events, registers the emit programs.
void finish_load(Host::Impl& h);
// Text of a value for logging (tostring).
std::string to_text(lua_State* L, int index);
std::string utf8(const std::wstring& w);

}  // namespace emf::lua
