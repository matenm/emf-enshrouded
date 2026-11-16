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

#include "common/perf_book.h"
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

// A callback registered through emf.bus.subscribe(). Every callback is valid only for messages
// published at or after first_sequence. A token keeps unsubscribe() during a callback safe.
struct BusSubscription {
    int ref = LUA_NOREF;
    int kind = EMF_WORLD_SERVER;
    uint64_t first_sequence = 0;
    uint64_t token = 0;
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
    std::multiset<std::string> windows;                // full ids of the mod windows it opened (lua 1.13); closed on unload
    std::set<uint32_t> offers;                         // entities it put an offer on (lua 1.14); removed after unload
    std::vector<KeyWatch> keys;                        // emf.on_key
    std::vector<int> chat_cbs;                         // emf.on_chat (lua 1.6)
    std::vector<int> player_cbs[3];                    // emf.on_player_joined, _left, _died (lua 1.20)
    std::vector<int> player_intro;                     // joined functions registered after players came: told next step
    std::vector<int> ws_cbs;                           // emf.on_world_setting_changed (lua 1.10)
    std::vector<int> hit_cbs;                          // emf.on_hit (lua 1.23)
    std::vector<uint8_t> hit_cb_kinds;                 // the kinds each on_hit function asked for (bit per kind)
    std::atomic<uint32_t> hit_cb_kinds_any{0};         // their union, read without m: a hit of another kind skips
    std::vector<int> buff_cbs;                         // emf.on_buff (lua 1.24)
    std::atomic<uint32_t> buff_cb_count{0};            // buff_cbs.size(), read without m
    std::atomic<uint32_t> hit_cb_count{0};             // hit_cbs.size(), read without m: a hit skips scripts without
    std::set<uint32_t> hit_rules;                      // its emf.hit_rule ids (lua 1.23): removed on unload and pause
    std::set<uint32_t> enemy_orders;                   // its emf.enemy_order ids (lua 1.32): cancelled on unload and pause
    std::set<uint32_t> enemy_moves;                    // its emf.enemy_move ids (lua 1.33): cancelled on unload and pause
    std::set<uint32_t> villager_moves;                 // its emf.villager_move ids (lua 1.39, a hold too): the same
    std::set<uint32_t> villager_holds;                 // ...of them, the ones that arrived and hold (no callback left)
    std::set<std::string> markers;                     // its emf.marker ids ("<id>:<name>", lua 1.25): removed with it
    std::set<std::pair<uint32_t, std::string>> sent_markers;  // emf.send_marker: (player or 0, id), unsent with it
    std::map<std::string, int> map_actions;            // emf.map_action (lua 1.26): "<id>:<name>" -> function; removed with it
    std::vector<int> map_click_cbs;                    // emf.on_map_click (lua 1.26)
    std::atomic<uint32_t> map_click_count{0};          // map_click_cbs.size(), read without m (the render thread)
    std::deque<EmfMapClick> map_clicks;                // clicks for the next client world step (Host::Impl::map_m)
    std::set<std::string> canvases;                    // emf.canvas ids ("<id>:<name>", lua 1.27): removed with it
    std::map<std::string, std::vector<BusSubscription>> bus_subs;  // cross-mod message callbacks
    uint64_t next_bus_subscription = 0;             // unique callback tokens, even after unsubscribe
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
    // Steps in a row of each world kind that left this script out because it was busy in another world's step (only
    // that kind's step, under its world_step_m, uses its slot): tick_body waits after kMaxBusySkips.
    uint32_t busy_skips[3] = {0, 0, 0};
    // Read without m by a step or an event that finds the script busy in another world: its bus callbacks and event
    // functions per world kind (only a script that has some for this world is waited for). Changed only under m.
    std::atomic<uint32_t> bus_kind_subs[3]{};
    std::atomic<uint32_t> event_kind_subs[3]{};
    std::string perf_names[3];  // "<id> (server)" etc., made once (under m) for perf_report
    uint64_t loads = 0;                                // +1 per load: answers meant for an older load are dropped
    // data per world: <game>\emf\config\<id>\<slot>.txt (lives as long as the script; attached once)
    std::unique_ptr<emf::WorldFile> file;
    int world_cb = LUA_NOREF;      // emf.on_world_data's function
    uint64_t world_delivered = 0;  // the load (loads) the world's text was last handed to
    bool world_pending = false;    // on_world_data came after that: the next server tick hands the text to it
};

// A copied message between otherwise isolated Lua states.
struct BusMessage {
    std::string source;
    std::string topic;
    std::string payload;
    int kind = EMF_WORLD_SERVER;
    uint64_t sequence = 0;  // global publication order, protected by the host mutex
    uint64_t source_epoch = 0;  // invalidated when the publishing mod reloads or stops
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

// The user pointer of a script's map action in the core: one per "<id>:<name>", kept until the host goes (the core may
// still hold it while the action is removed); the function is looked up in the script when the action is done.
struct MapActionSlot {
    Host::Impl* host = nullptr;
    std::string script, full;
};

struct Host::Impl {
    const EmfApi* api = nullptr;
    std::wstring mods_dir;
    std::vector<std::unique_ptr<Script>> scripts;
    std::atomic<uint64_t> last_check{0};  // GetTickCount64 of the last reload check (tick runs on several world threads)
    emf::PerfBook perf;                   // each script's time per world step, and the reload check's (perf_report)
    std::atomic<uint64_t> perf_since{0};  // GetTickCount64 of the last perf_report
    double (*perf_clock)() = nullptr;     // that timing's clock (nullptr: QueryPerformanceCounter)
    std::mutex slow_m;                    // slow_said
    std::map<std::string, uint64_t> slow_said;  // script id -> when its last "slow" line was logged
    size_t memory_limit = kMemoryLimit;   // per script, for states made from now on
    std::mutex m;  // the maps below
    std::mutex world_step_m[3];  // serialize steps of the same world kind; preserves bus delivery order
    std::map<std::string, std::vector<WheelSection>> wheel_groups;  // group name -> sections in publishing order
    std::map<std::string, std::string> command_owner;  // full command name (lower case) -> script id
    std::map<std::string, void*> command_slots;        // full command name (lower case) -> its CommandSlot (the user)
    std::set<std::string> wanted;                      // "<kind>:<EventName>" the scripts asked for while loading
    std::set<std::string> subscribed;                  // "<kind>:<EventName>" subscribed with on_event
    std::set<std::string> emits[3];                    // per world kind: events the scripts send
    bool load_done = false;                            // after load_all: no new subscriptions
    std::atomic<bool> rechecked{false};                // the first tick compared the scripts with the core's list
    std::deque<std::pair<uint8_t, QueuedEvent>> outbox;  // (world kind, event) waiting for the host's program
    std::deque<BusMessage> bus_outbox;                 // queued for the next world step
    std::map<std::string, size_t> bus_queued_by_source; // 256 pending publications per publisher
    std::map<std::string, uint64_t> bus_source_epoch;   // invalidates publications already drained on stop/reload
    std::set<std::string> bus_refusal_logged;          // one warning per rejection streak and publisher
    uint64_t bus_next_sequence = 0;                    // guarded by m
    std::function<bool(uint32_t)> key_down;              // a key is held and the game is in front (tests replace it)
    std::atomic<uint64_t> server_seen{0};                // GetTickCount64 of the last server world step
    std::atomic<int> chat_hooked{0};                     // the core's on_chat for emf.on_chat: 0 not asked, 1 on, -1 refused
    std::atomic<int> player_hooked{0};                   // the core's on_player (lua 1.20): 0 not asked, 1 on, -1 refused
    std::atomic<int> buff_hooked{0};                     // the core's on_buff (lua 1.24): 0 not asked, 1 on, -1 refused
    std::atomic<int> hit_hooked{0};                      // the core's on_hit (lua 1.23): 0 not asked, 1 on, -1 refused
    std::atomic<int> click_hooked{0};                    // the core's on_map_click (lua 1.26): 0 not asked, 1 on, -1 refused
    std::mutex map_m;                                    // map_action_slots and every script's map_clicks
    std::map<std::string, std::unique_ptr<MapActionSlot>> map_action_slots;
    struct PresentPlayer {
        uint32_t player = 0;
        std::string name;
        bool host = false;
    };
    std::mutex present_m;                                // present, present_session
    std::map<uint64_t, PresentPlayer> present;           // the players the core said are in (by account)
    uint32_t present_session = 0;                        // the server session they are of
    std::atomic<int> ws_hooked{0};                       // the core's on_world_setting_changed: 0 not asked, 1 on, -1 refused
    std::atomic<int> moves{0};                           // emf.move requests in flight (each holds one place in the item queue)
    std::function<uint64_t()> now_ms;                    // the clock of the chat limit and time limit (tests replace it)
    uint64_t (*file_clock)() = nullptr;                  // the world-data files' clock (nullptr: GetTickCount64)
    // lua 1.14 offers: to the core every script is one owner, so the host keeps which script offered which entity
    std::map<uint32_t, void*> offer_slots;               // entity -> its OfferSlot (bindings.cpp)
    std::map<std::string, void*> net_slots;              // "<script>/<name>" -> its NetSlot (bindings.cpp; kept)
    std::vector<std::pair<uint32_t, void*>> offer_drops;  // (entity, slot) of an unloaded script: off on the next step

    Script* find(const std::string& id);
    bool load(Script& s);
    void unload(Script& s);
    void pause(Script& s, const std::string& why);
    void drop_sections(const std::string& id);  // the mod's wheel sections leave every group (unload, pause)
    void drop_bus(const std::string& id);       // discard this mod's queued outgoing publications (unload, pause)
    // Stops scripts whose mod the core no longer lists as a script mod (a dependency failed after the host loaded).
    void recheck();
};

Script* script_of(lua_State* L);
// Calls the function under the arguments on the stack (protected, 250 ms limit). False: the script is paused.
bool call_lua(Script& s, int nargs);
// lua 1.20: the players already in the world, as joined, to the functions in s.player_intro (server world, the
// script's lock held).
void deliver_player_intro(Host::Impl& h, Script& s, EmfWorld* w);
// The world userdata passed to callbacks (valid until the callback returns).
void push_world(lua_State* L, Script& s, EmfWorld* w);
// Adds the world/component/item/event/wheel functions to the emf table on top of the stack.
void open_bindings(lua_State* L, Script& s);
// After the first load of every script: subscribes the wanted events, registers the emit programs.
void finish_load(Host::Impl& h);
// Text of a value for logging (tostring).
std::string to_text(lua_State* L, int index);
std::string utf8(const std::wstring& w);

// lua 1.14 offers (bindings.cpp): unload queues a script's offers; every server step removes the queued ones.
void queue_script_offers(Host::Impl& h, Script& s);
void run_offer_drops(Host::Impl& h, EmfWorld* w);

// lua 1.23 hits (hit_bindings.cpp): emf.on_hit, emf.hit_rule, emf.hit_rule_remove; a script's rules leave the core
// when it unloads or pauses.
int l_on_hit(lua_State* L);
int l_hit_rule(lua_State* L);
int l_hit_rule_remove(lua_State* L);
void drop_hit_rules(Host::Impl& h, Script& s);

// lua 1.24 buffs (buff_bindings.cpp): emf.buff_types, emf.buff_type, emf.buffs, emf.apply_buff, emf.remove_buff,
// emf.on_buff.
int l_buff_types(lua_State* L);
int l_buff_type(lua_State* L);
int l_buffs(lua_State* L);
int l_apply_buff(lua_State* L);
int l_remove_buff(lua_State* L);
int l_on_buff(lua_State* L);
// lua 1.25 markers (marker_bindings.cpp): emf.marker, emf.remove_marker, emf.clear_markers, emf.marker_types,
// emf.world_to_screen, emf.send_marker, emf.unsend_marker; a script's markers leave with it (unload, pause, reload).
int l_marker(lua_State* L);
int l_remove_marker(lua_State* L);
int l_clear_markers(lua_State* L);
int l_marker_types(lua_State* L);
int l_world_to_screen(lua_State* L);
int l_send_marker(lua_State* L);
int l_unsend_marker(lua_State* L);
void drop_markers(Host::Impl& h, Script& s);
// "<script id>:<id>" for a script's own markers and map actions; raises on a bad id (A-Z a-z 0-9 _ . - :, 48 in all).
std::string scoped_id(lua_State* L, Script* s, const char* id, const char* fn);
// lua 1.26 the map (map_bindings.cpp): emf.map_cursor, emf.on_map_click, emf.map_action, emf.remove_map_action,
// emf.ground_at; a script's actions leave with it (unload, pause, reload), its clicks come on client world steps.
int l_map_cursor(lua_State* L);
int l_on_map_click(lua_State* L);
int l_map_action(lua_State* L);
int l_remove_map_action(lua_State* L);
int l_ground_at(lua_State* L);
void deliver_map_clicks(Host::Impl& h, Script& s, EmfWorld* world);  // s.m held
void drop_map(Host::Impl& h, Script& s);
// lua 1.27 a minimap's parts (canvas_bindings.cpp): emf.player_view, emf.fog_at, emf.map_markers, emf.canvas,
// emf.canvas_draw; a script's canvases leave with it.
int l_player_view(lua_State* L);
// lua 1.29 paths (path_bindings.cpp): emf.path_find, emf.path_cancel
int l_path_find(lua_State* L);
int l_path_cancel(lua_State* L);
// lua 1.37 breaking blocks (block_bindings.cpp): emf.break_blocks, emf.block_at, emf.find_wall
int l_break_blocks(lua_State* L);
int l_block_at(lua_State* L);
int l_find_wall(lua_State* L);
// lua 1.30 doors (door_bindings.cpp)
int l_door(lua_State* L);
int l_door_open(lua_State* L);
int l_door_close(lua_State* L);
int l_door_lock(lua_State* L);
// lua 1.32 enemy orders (door_bindings.cpp)
int l_enemy_order(lua_State* L);
int l_enemy_order_cancel(lua_State* L);
int l_enemy_target(lua_State* L);
void drop_enemy_orders(Host::Impl& h, Script& s);
int l_enemy_move(lua_State* L);  // move_bindings.cpp (lua 1.33)
int l_enemy_move_cancel(lua_State* L);
int l_enemy_moving(lua_State* L);
void drop_enemy_moves(Host::Impl& h, Script& s);
int l_villager_move(lua_State* L);  // move_bindings.cpp (lua 1.39)
int l_villager_move_cancel(lua_State* L);
int l_villager_moving(lua_State* L);
int l_fog_at(lua_State* L);
int l_map_markers(lua_State* L);
int l_canvas(lua_State* L);
int l_canvas_draw(lua_State* L);
void drop_canvases(Host::Impl& h, Script& s);
// The world argument at `index` (a world value from a callback); an error otherwise.
EmfWorld* lua_world_arg(lua_State* L, int index);

}  // namespace emf::lua
