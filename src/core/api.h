#pragma once
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "attributes.h"
#include "buffs.h"
#include "canvas.h"
#include "fog_grid.h"
#include "ground_service.h"
#include "map_actions.h"
#include "marker_map.h"
#include "marker_net.h"
#include "markers.h"
#include "overlay/hud_model.h"
#include "common/log_limiter.h"
#include "common/pe_image.h"
#include "common/signatures.h"
#include "common/world_view.h"
#include "commands.h"
#include "dispatcher.h"
#include "event_relay.h"
#include "player_events.h"
#include "env_damage.h"
#include "hit_hook.h"
#include "hook_table.h"
#include "mod_registry.h"
#include "program_registry.h"
#include "save_events.h"
#include "server_chat.h"
#include "settings.h"
#include "system_programs.h"
#include "wheel_request.h"
#include "window_request.h"
#include "emf/emf_api.h"
#include "world_tracker.h"

namespace emf::core {

class InventoryService;
class BuffService;
struct BuffRequest;
class TeleportService;
class ComponentService;
class OfferService;
class PlayerEvents;
namespace net {
class NetService;
struct NetDeps;
}  // namespace net
namespace ws {
class Service;
}

// Everything the C API functions need. One instance per process (set with set_core_state).
struct CoreState {
    std::array<Resolved, kSigCount> sigs{};
    bool known_build = false;
    // The world's tick fields are confirmed (always on a known build); otherwise world_tick / ecs_time_ns give 0.
    std::atomic<bool> tick_offsets_ok{true};
    std::atomic<bool> tick_fields_warned{false};  // "world_tick / ecs_time_ns give 0" said once
    HudModel hud;  // v12 hud_set lines (the overlay draws them)
    // put_items refusals: the same line at most every 10 s.
    std::mutex put_refusals_lock;
    LogLimiter put_refusals;
    class DoorService* doors = nullptr;  // v38 (null when off on this game build)
    class EnemyOrders* orders = nullptr;  // v39 enemy orders
    class MoveOrders* moves = nullptr;    // v40 move orders (null when off)
    struct {  // v36 remove_building: the objects removed in this server step (the server world's thread only)
        uint32_t session = 0;
        uint64_t step = 0;
        std::vector<uint32_t> ids;
    } removed_this_step;
    GetComponentFn get_component = nullptr;
    GetTemplateComponentFn get_template_component = nullptr;  // optional signature
    Dispatcher dispatcher;
    PerfBook perf;                           // time per world step: the game's, EMF's wait, each mod's (`perf`)
    std::atomic<uint64_t> perf_since_ms{0};  // when `perf` last took the book (GetTickCount64)
    WorldTracker tracker;
    std::function<void(const std::string&)> log_line;  // receives finished lines
    std::function<uint64_t()> now_ms;                  // wall clock in ms (injectable for tests)
    std::unique_ptr<ProgramRegistry> programs;         // set by the core once hooks are available
    SystemPrograms system_programs;                     // mod programs registered into each new world
    SaveEvents save_events{"on_save callback"};         // fired after a world save was written
    SaveEvents load_events{"on_load callback"};         // fired after a world save was loaded
    EventRelay relay;                                   // on_event subscriptions and per-world event copies
    RelayProgram relay_programs[2];                     // the relay's internal mod programs (server, client)
    CommandCenter commands;                             // developer commands (console window, commands.txt)
    InventoryService* inventory = nullptr;              // put_items (null when its signatures are not usable)
    TeleportService* teleport = nullptr;                // teleport (null when off on this game build)
    std::string teleport_off;                           // why teleport is off ("" while it is on or not set up yet)
    ComponentService* components = nullptr;             // v23 change_component (null when off on this game build)
    std::string components_off;                         // why change_component is off
    OfferService* offers = nullptr;                     // v25 E prompts (null when change_component is off)
    net::NetService* net = nullptr;                     // v27 mod messages
    PlayerEvents* players = nullptr;                    // v28 on_player
    std::atomic<uint32_t> server_session{0};            // +1 per server world set up (API v9 server_session)
    std::atomic<uint32_t> client_session{0};            // +1 per client world set up (mod messages: a new session)
    std::function<bool(const WheelRequest&)> open_wheel;   // the overlay's wheel (null: no overlay)
    // The overlay's mod window (API v24; null: no overlay): open (replaced: the window it replaced; why: a refusal),
    // new contents for the open one, close by id (closed: the window closed).
    std::function<bool(WindowRequest, std::optional<WindowRequest>*, std::string*)> open_window;
    std::function<bool(const WindowRequest&)> update_window;
    std::function<bool(const std::string&, std::optional<WindowRequest>*)> close_window;
    // The overlay's pictures (API v26 image_load; null: no overlay): the picture at `path` (a file or a folder of frames)
    // under `key` (one handle per key), 0 and why when refused.
    std::function<uint32_t(const std::wstring& path, const std::string& key, std::string* why)> load_image;
    std::wstring portrait_dir;  // <game>\emf\cache\portraits ("portrait:<key>" pictures)
    std::function<void(const std::string&)> notify;        // the overlay's top-of-screen note (null: log only)
    ModRegistry mods;                                    // every mod found in emf/mods and what became of it
    std::unique_ptr<IniBackend> settings_backend;         // emf/config/<mod>.ini
    std::unique_ptr<IniBackend> core_settings_backend;    // emf/emf_core.ini, EMF's own settings (mod id "emf")
    std::unique_ptr<IniBackend> settings_route;           // the store's backend: "emf" to the core ini, mods to theirs
    std::unique_ptr<SettingsStore> settings;             // mods' settings (v13; null until the core sets it up)
    struct SettingWatcher {
        std::string mod;  // "" = every mod
        EmfSettingCallback cb = nullptr;
        void* user = nullptr;
        bool enabled = true;
    };
    // The game exe as find_pattern sees it (the core fills it from the module; tests use a fake image).
    struct ExeImage {
        uintptr_t base = 0;
        size_t size = 0;
        std::vector<emf::Section> sections;
        std::string sha256;
        uint32_t build_number = 0;  // 0 = not a known build
        bool server = false;        // enshrouded_server.exe
        // A read-only mapping of the exe file (same layout): patterns are matched against it, because hooks rewrite
        // the first bytes of the functions they hook in the live image. 0 = match the live image.
        uintptr_t pristine_base = 0;
        // The bytes find_pattern scans for a section (the file mapping, or the live image).
        const uint8_t* scan_bytes(const emf::Section& s) const {
            return reinterpret_cast<const uint8_t*>(pristine_base ? pristine_base + (s.start - base) : s.start);
        }
    } exe;
    std::unique_ptr<HookTable> hooks;  // every inline hook (EMF's and mods'), chaining hooks on the same function
    // v18 attributes and damage (null when their signatures are not usable on this game build)
    AttrFindFn attr_find = nullptr;
    AttrAddFn attr_add = nullptr;
    AllocPendingHitFn alloc_pending_hit = nullptr;
    // v22 levels: the game's xpRequired(level, BalancingRegistry*) and maxPlayerLevel(BalancingRegistry*) (null: not
    // found on this build)
    uint32_t (*xp_required)(uint32_t level, const void* balancing) = nullptr;
    uint32_t (*level_cap)(const void* balancing) = nullptr;
    std::atomic<bool> damage_hooked{false};  // impact_hit_conclusion's pre-hook (damage_pre_conclusion) is requested
    DamageQueue damage_queue;                // damage requests waiting for that pre-hook
    HitOrderCheck hit_order;                 // logs once whether it runs after clear_hit_candidates in the same step
    // ---- v31 hits ----
    HitRules hit_rules;               // the mods' rules, applied by the detour before the before-callbacks
    HitCallbacks hit_callbacks;       // on_hit_before (the detour) and on_hit (the server world's HitEvents)
    HitStats hit_stats;               // the detour's counters and time (the hits command)
    std::string hit_hook_off_why = "it is not set up yet";  // why the detour is not installed ("" while it is)
    std::atomic<bool> hit_events{false};  // on_hit: the server world's HitEvents come through the relay
    std::string hit_events_off_why;
    // fall, shroud, cold and drowning (feature 4b): the program slice's layout (read from code), the after-queue, the
    // scale carry (the env programs run one at a time) and why they are off ("" while on)
    uint32_t env_list_off = 0, env_count_off = 0;
    EnvQueue env_queue;
    HitCarry hit_carry;
    std::string env_off_why = "it is not set up yet";
    // ---- v32 buffs ----
    BuffService* buffs = nullptr;  // emf_buffs (null when its program could not be added)
    struct BuffWatcher {
        EmfBuffCallback cb = nullptr;
        void* user = nullptr;
        bool on = true;
    };
    std::mutex buff_watchers_lock;
    std::vector<BuffWatcher> buff_watchers;  // on_buff (at most 16; only appended to)
    BuffDiff buff_diff;                      // the players' containers at the last comparison (server world thread)
    std::vector<uint32_t> buff_player_list;  // the players compared each step (read again every kBuffDiffMs)
    std::mutex buff_log_lock;
    LogLimiter buff_log;                     // buff_apply / buff_remove refusals: the same line once per 10 s
    uint64_t buff_diff_ms = 0;
    std::function<std::vector<uint32_t>(EmfWorld*)> buff_players;  // the server world's players (empty: the roster)
    // ---- v33 markers (api_markers.cpp) ----
    struct Markers {
        MarkerStore store;                     // every marker this game draws: its mods' own and a server's
        MapMarkers map;                        // entries in the game's own map list (client world's thread)
        std::unique_ptr<MarkerSender> sender;  // a server's markers for players' games (null: no mod messages)
        MarkerReceiver receiver{nullptr};      // a server's markers for this game
        uint32_t list_count_off = 0;           // the game's map list's count in UiPlayData (0: no entries there)
        EmfWorld* client = nullptr;            // the client world of the last tick (another one: received markers go)
        EmfWorld* server = nullptr;            // the server world of the last tick (another one: the sender starts afresh)
        std::string world_why, list_why;       // why world markers / the game's map list entries are off ("" = on)
        uint64_t refresh_ms = 0;               // the sender's last position refresh
        std::mutex log_lock;
        LogLimiter log;                        // refusals: the same line once per 10 s
    } markers;
    // ---- v34 the map's mouse, map actions, the ground (api_map.cpp) ----
    struct Map {
        MapActions actions{nullptr};             // the server end's actions (announced over emf/mapact)
        MapActionsClient client;                 // what this game's server end announced
        std::unique_ptr<GroundService> ground;   // ground_at (null: not available on this game build)
        std::recursive_mutex ground_lock;        // ground_at from any thread; callbacks may ask again
        std::atomic<uint32_t> host_told{0};      // the host's own game got the list (its local player; 0: not yet)
        std::string host_msg;                    // what it got (the server world's thread)
        uint64_t host_ms = 0;
        std::mutex click_lock;
        std::vector<std::pair<EmfMapClickCallback, void*>> clicks;  // on_map_click (16 at most)
    } map;
    // v35 a minimap's parts
    struct Minimap {
        FogStore fog;  // the client's fog of war, copied twice a second
        CanvasStore canvases;
        std::atomic<uint64_t> fog_asked_ms{0}, markers_asked_ms{0};  // the last fog_at / map_markers (minimap_wanted)
    } minimap;
    std::vector<std::string> reserved_programs;  // programs EMF's core hooks itself: hook_program refuses them
    std::mutex setting_watchers_lock;
    std::vector<SettingWatcher> setting_watchers;  // v13 on_setting_changed (only appended to)
    // ---- v19 chat lines and saves ----
    ChatLineBudget chat_budget;  // per mod (by DLL name): chat_line_limit() lines per 10 s (lua: 120)
    ChatInbox chat_inbox;        // players' lines waiting for the on_chat callbacks (the server chat hook fills it)
    struct ChatWatcher {
        EmfChatCallback cb = nullptr;
        void* user = nullptr;
        bool enabled = true;
    };
    std::mutex chat_watchers_lock;
    std::vector<ChatWatcher> chat_watchers;  // v19 on_chat (only appended to)
    std::atomic<bool> has_chat_watchers{false};
    // The server chat glue (empty: server_chat_on / queue_chat_tell / queue_chat_everyone; tests replace them).
    std::function<bool()> chat_on;
    std::function<bool(uint32_t handle_hint, uint64_t account, const std::string& line, bool system)> chat_tell_line;
    std::function<bool(const std::string& line, bool system)> chat_everyone_line;
    std::function<uint32_t(uint64_t account)> entity_by_account;  // on_chat's entity, on the server world's thread
    // API v27 mod messages (the chat transport; set when it starts): a player entity's connection, an account's entity
    // (the server world's thread) and whether an account is an admin
    std::function<std::optional<std::pair<uint32_t, uint64_t>>(uint32_t entity)> net_connection_of;
    std::function<uint32_t(uint64_t account)> net_entity_of;
    std::function<bool(uint64_t account)> net_is_admin;
    // player_name: a copy of the server's connection records (empty: read from chat_server(); false: none to read)
    std::function<bool(std::vector<uint8_t>* records)> read_connections;
    using RequestSaveFn = void (*)(void* game_server_instance);
    RequestSaveFn request_save = nullptr;           // the game's requestServerSave (null: not found on this build)
    std::atomic<void*> save_instance{nullptr};      // the GameServer instance loadSavegameStep last ran for
    std::atomic<uint32_t> save_instance_session{0}; // the server session it ran in
    ws::Service* world_settings = nullptr;          // v21 world settings (null: not set up)
};

void set_core_state(CoreState* state);
CoreState* core_state();
// The API table handed to mods. Valid for the process lifetime.
const EmfApi* api_table();
// v44 breaking blocks' drops: count x the item with this GUID as loose pickups at pos (0.5 m up), on the server world's
// thread inside a world callback; the number dropped (the rest is logged).
uint32_t api_drop_items(EmfWorld* w, const uint8_t item_guid[16], uint32_t count, const float pos[3]);
// Hands a wheel's result to the mod: its callback runs after the next step of a world of request.world_kind.
void post_wheel_choice(const WheelRequest& request, int choice);
// Hands a window event (EMF_UI_CLICK / _RIGHT_CLICK / _CLOSED) to the mod: its callback runs after the next step of a
// world of request.world_kind; a CLOSED with no such world running is delivered at once (NULL world, EMF_UI_NO_WORLD)
// and a click then is dropped. Any thread.
void post_window_event(const WindowRequest& request, uint32_t kind, const std::string& key, uint32_t reason);
// The net service's ties to the game (v27 mod messages): the host check, the dispatcher, the clock, the log.
net::NetDeps net_deps(CoreState* state);
// v28 on_player: the core's player list reader, the clock, the log and the guarded call for PlayerEvents.
PlayerEvents::Deps player_events_deps();
// Tells the on_setting_changed callbacks about the settings changed since the last call (the sim step calls it
// after each world's step, on that world's thread).
void pump_setting_changes(EmfWorld* world);
// Hands the players' chat lines waiting in the inbox to the on_chat callbacks (the sim step calls it after each server
// world step, on its thread).
void pump_chat_lines(EmfWorld* world);
// The players' chat glue's callback for one line (receive loop thread): queued for pump_chat_lines when a mod listens.
void note_chat_line(ChatLine line);
// Installs one of EMF's own hooks through the hook table (owner "emf"), so mods can chain onto it; without a table
// (before the core set one up) MinHook directly.
bool install_emf_hook(const char* name, uintptr_t target, void* detour, void** original);
// v18 damage: the pre-hooks of the game programs impact_hit_conclusion (writes the waiting hits of the server world into
// its pending-hit list) and clear_hit_candidates (only notes the step, for the order check). Game worker threads.
void damage_pre_conclusion(void* program_context);
void damage_pre_clear(void* program_context);
// The program registry's report of one of EMF's own program hooks: a failure is logged; impact_hit_conclusion's
// installed pre-hook switches damage on.
void note_program_hook(const std::string& program, bool installed);
// v31 hits: the game reads the detour needs on the server world `w` (entity classes, CombatFlags, health, a weapon's item).
void fill_pre_hit_env(EmfWorld* w, PreHitEnv* env);
// v31 on_hit: asks the event relay for the server world's HitEvents (the core does at start, before the relay closes).
bool subscribe_hit_events();
// Fall, shroud, cold and drowning: the pre and post hooks of fall_damage_infliction, fog_resistance_update,
// bodyheat_depletion and oxygen_update (kind EMF_HIT_KIND_*), and the after-queue's delivery after the server step.
void env_pre(void* program_context, uint8_t kind);
void env_post(void* program_context, uint8_t kind);
void deliver_env_hits(EmfWorld* w);
void env_new_session();  // a new server world: the old one's queued env hits and carries are dropped

// ---- v35 a minimap's parts (api_minimap.cpp) ----
bool api_player_view(EmfPlayerView* out);
bool api_fog_at(double x, double z, float* explored);
uint32_t api_map_markers(EmfGameMarker* out, uint32_t cap);
bool api_canvas_set(const char* id, const EmfCanvas* c);
bool api_canvas_draw(const char* id, const EmfCanvasCmd* cmds, uint32_t count);
// The client world's thread, every tick: the fog of war copied (twice a second).
void minimap_client_tick(EmfWorld* client, uint64_t now_ms);

// ---- v34 the map (api_map.cpp) ----
bool api_map_cursor(double* x, double* z);
bool api_on_map_click(EmfMapClickCallback cb, void* user);
bool api_map_action(const char* id, const char* label, uint32_t hold_ms, EmfMapActionCallback cb, void* user);
bool api_ground_at(EmfWorld* world, double x, double z, EmfGroundCallback cb, void* user);
// The server world's thread, after its step: ground searches.
void map_server_tick(EmfWorld* server);
// A world of that kind was set up: client: the last server's announced actions go; server: ground searches end.
void map_world_setup(uint8_t kind);
// The console: map.where, ground <x> <z>.
bool add_map_commands(CommandCenter& commands);
// Start-up, with mod messages: the channel emf/mapact (the server's actions to players' games, a player's finished
// gesture back) and the gesture's done -> the server. False (why) when the channel could not be added.
bool map_net_setup(net::NetService* net, std::string* why);
// Start-up: on_map_click's listeners get the gesture's clicks.
void map_input_setup();
// The server: a player's game said hello: it gets the actions (none: an empty list, the gesture off).
void map_on_hello(uint32_t player);
bool map_greets();  // the server has map actions: players' games must hear about them
// This game's server end does its map action at (x, z), as a finished gesture does (the console's map.act too). False
// (why) when no action is offered here, the spot is outside the world, or the message could not be sent.
bool map_send_done(double x, double z, std::string* why);

// ---- v33 markers (api_markers.cpp) ----
bool api_marker_set(const EmfMarker* m);
bool api_marker_remove(const char* id);
uint32_t api_marker_clear();
uint32_t api_marker_types(EmfMapMarkerType* out, uint32_t cap);
bool api_world_to_screen(double x, double y, double z, float* px, float* py);
uint32_t api_marker_send(uint32_t player, const EmfMarker* m, uint32_t count);
uint32_t api_marker_unsend(uint32_t player, const char* id);
// The client world's thread, after its step: the UiPlayData pointer, the local player's position, followed entities;
// another client world than last time clears the received markers.
void markers_client_tick(EmfWorld* client);
// The server world's thread, after its step: followed entities' positions for players' games (every 2 s).
void markers_server_tick(EmfWorld* server);
// A world of that kind was set up (the game reuses addresses: the tick's pointer compare alone misses a new world):
// client: a server's markers, the map list's state and the UiPlayData pointer go; server: the sender starts afresh.
void markers_world_setup(uint8_t kind);
// The pre- and post-hook of the game's clear_map_markers_for_ui (client world): EMF's entries in the game's map list.
void markers_map_pre(void* program_ctx);
void markers_map_post(void* program_ctx);
// The console: `markers` (what is there, what is on) and `markers.test [text] [game icon]` (a marker 10 m ahead of the
// player, owner "emf"; `markers.test clear` removes it).
std::vector<std::string> markers_status();
bool markers_test(const std::string& text, uint32_t game_icon, std::string* why);
bool add_marker_commands(CommandCenter& commands);

// ---- v32 buffs (api_buffs.cpp) ----
uint32_t api_buff_types(EmfWorld* w, EmfBuffType* out, uint32_t cap);
bool api_buff_type(EmfWorld* w, uint32_t id, EmfBuffType* out);
bool api_buff_text(uint32_t id, uint32_t which, char* buf, uint32_t cap);
uint32_t api_buffs_of(EmfWorld* w, uint32_t entity, EmfBuff* out, uint32_t cap);
bool api_buff_apply(EmfWorld* w, uint32_t target, uint32_t type, int64_t duration_ns, uint32_t source, uint32_t item,
                    EmfBuffCallback done, void* user);
bool api_buff_remove(EmfWorld* w, uint32_t target, uint32_t type, EmfBuffCallback done, void* user);
bool api_on_buff(EmfBuffCallback cb, void* user);
// A give or remove for EMF's own callers (console, Lua): the server world, a known type, then BuffService.
bool buff_request(BuffRequest r, std::string* why);
// A buff type's name in the player's language ("" on the dedicated server or for a type without one).
std::string buff_name(EmfWorld* w, uint32_t id);
// After each server step: the players' buff changes for on_buff (their containers compared every step while a callback
// listens, so a cleanse is seen for the one step it lasts; the player list read at most every kBuffDiffMs).
constexpr uint64_t kBuffDiffMs = 250;
void buffs_server_step(EmfWorld* w);
void buffs_new_session();  // a new server world: every player is a first sight again
// The console's buffs.list / buffs.of / buffs.give / buffs.clear.
bool add_buff_commands(CommandCenter& commands);
std::string buff_type_line(const EmfBuffType& t, const std::string& name);
std::string buff_line(const EmfBuff& b, const std::string& name);

}  // namespace emf::core
