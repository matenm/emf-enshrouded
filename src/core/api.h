#pragma once
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "attributes.h"
#include "overlay/hud_model.h"
#include "common/log_limiter.h"
#include "common/pe_image.h"
#include "common/signatures.h"
#include "common/world_view.h"
#include "commands.h"
#include "dispatcher.h"
#include "event_relay.h"
#include "hook_table.h"
#include "mod_registry.h"
#include "program_registry.h"
#include "save_events.h"
#include "server_chat.h"
#include "settings.h"
#include "system_programs.h"
#include "wheel_request.h"
#include "emf/emf_api.h"
#include "world_tracker.h"

namespace emf::core {

class InventoryService;
class TeleportService;

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
    GetComponentFn get_component = nullptr;
    GetTemplateComponentFn get_template_component = nullptr;  // optional signature
    Dispatcher dispatcher;
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
    std::atomic<uint32_t> server_session{0};            // +1 per server world set up (API v9 server_session)
    std::function<bool(const WheelRequest&)> open_wheel;   // the overlay's wheel (null: no overlay)
    std::function<void(const std::string&)> notify;        // the overlay's top-of-screen note (null: log only)
    ModRegistry mods;                                    // every mod found in emf/mods and what became of it
    std::unique_ptr<IniBackend> settings_backend;         // emf/config/<mod>.ini
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
    std::atomic<bool> damage_hooked{false};  // impact_hit_conclusion's pre-hook (damage_pre_conclusion) is requested
    DamageQueue damage_queue;                // damage requests waiting for that pre-hook
    HitOrderCheck hit_order;                 // logs once whether it runs after clear_hit_candidates in the same step
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
    std::function<bool(uint32_t handle_hint, uint64_t account, const std::string& line)> chat_tell_line;
    std::function<bool(const std::string& line)> chat_everyone_line;
    std::function<uint32_t(uint64_t account)> entity_by_account;  // on_chat's entity, on the server world's thread
    // player_name: a copy of the server's connection records (empty: read from chat_server(); false: none to read)
    std::function<bool(std::vector<uint8_t>* records)> read_connections;
    using RequestSaveFn = void (*)(void* game_server_instance);
    RequestSaveFn request_save = nullptr;           // the game's requestServerSave (null: not found on this build)
    std::atomic<void*> save_instance{nullptr};      // the GameServer instance loadSavegameStep last ran for
    std::atomic<uint32_t> save_instance_session{0}; // the server session it ran in
};

void set_core_state(CoreState* state);
CoreState* core_state();
// The API table handed to mods. Valid for the process lifetime.
const EmfApi* api_table();
// Hands a wheel's result to the mod: its callback runs after the next step of a world of request.world_kind.
void post_wheel_choice(const WheelRequest& request, int choice);
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

}  // namespace emf::core
