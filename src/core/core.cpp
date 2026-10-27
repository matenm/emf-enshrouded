// emf_core.dll: started by the loader (dinput8.dll). Resolves signatures, hooks the ECS sim step,
// dispatches mod callbacks after each world step and loads mods from <game>/emf/mods.
#include <windows.h>

#include <share.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "api.h"
#include "bridge/runtime.h"
#include "bridge/switch.h"
#include "common/code_offsets.h"
#include "common/hooks.h"
#include "common/log.h"
#include "common/offsets.h"
#include "common/seh.h"
#include "common/sha256.h"
#include "common/signatures.h"
#include "common/world_view.h"
#include "fault_report.h"
#include "builtin_commands.h"
#include "profile_commands.h"
#include "save_commands.h"
#include "save_slots.h"
#include "saves/backup.h"
#include "saves/save_info.h"
#include "chat_commands.h"
#include "server_chat.h"
#include "console.h"
#include "inventory.h"
#include "teleport.h"
#include "known_tools.h"
#include "overlay/input_hook.h"
#include "overlay/profiles_ui.h"
#include "overlay/overlay.h"
#include "overlay/vk_overlay.h"
#include "overlay/submit.h"
#include "file_poll.h"
#include "mods.h"
#include "menu_start.h"
#include "ready_gate.h"
#include "version.h"
#include "system_programs.h"
#include "world_jobs.h"

// The Vulkan layer entry point lives in emf_core_lib (a static library): keep it in emf_core.dll's exports.
#pragma comment(linker, "/include:emf_vk_negotiate")

namespace {
constexpr const char* kVersion = emf::core::version_string();

using SimStepFn = void (*)(void* storage, char ticked);
SimStepFn o_sim_step = nullptr;
using RegisterProgramFn = uint64_t (*)(void* world, const uint8_t* desc, char disabled);
RegisterProgramFn o_register_program = nullptr;
using RegisterAllProgramsFn = void (*)(void* world);
RegisterAllProgramsFn o_register_all_programs = nullptr;
uintptr_t g_register_program_addr = 0;  // the game's registerProgram (our hook runs too)
using Save2WriteWorldFn = void (*)(uint8_t* save2, void* user);
Save2WriteWorldFn o_save2_write_world = nullptr;
using LoadSavegameStepFn = uint8_t* (*)(uint8_t* out, uint8_t* inst);
LoadSavegameStepFn o_load_savegame_step = nullptr;
emf::core::LoadGuard g_load_guard;  // server thread only
std::unique_ptr<emf::core::CoreState> g_state;
std::vector<emf::core::LoadedMod> g_mods;
// Open once emf_core_start is done (mods loaded, or EMF stays idle). The dedicated server sets up its world during
// start-up; that setup waits here so mods' programs and event subscriptions reach the world.
emf::core::ReadyGate g_mods_ready;
constexpr auto kModsReadyTimeout = std::chrono::seconds(30);
emf::core::BuiltinContext g_builtin;                     // the core's own commands
emf::core::ProfileCommandContext g_profiles;             // "profile": mod profiles
emf::core::SaveCommandContext g_save_commands;           // "saves.backup", "saves.list"
std::unique_ptr<emf::core::CommandFile> g_command_file;  // emf/commands.txt
emf::core::ConsoleWindow g_console;                      // optional ([console] window=1 in emf/emf_core.ini)
emf::core::ManifestWatch g_manifest_watch;  // mods' settings follow their mod.json while the game runs
emf::core::FilePoll g_file_poll;            // commands.txt and the mods' mod.json, once a second
std::string g_load_world_off;  // why the bridge's load_world is off ("" = on, or the bridge is off)
// The server world's own jobs (grid update, range queries) finish before EMF's tick work runs (core/world_jobs.h).
emf::core::WorldJobsGate g_world_jobs;

// The game's waitForWorldJobs. The wait also runs the game's own queued tasks on this thread, so a fault in it is most
// likely a fault in one of those tasks: it is logged and then passed on to the game's own crash handler (never
// swallowed, which would leave the game's task group half done and the game running on in a broken state).
LONG world_jobs_fault_filter(unsigned long code) {
    emf::logf("world jobs: a fault (0x%08lx) while the game finished its own world jobs; the game's crash handler takes it",
              code);
    return EXCEPTION_CONTINUE_SEARCH;
}
bool guarded_wait_world_jobs(emf::core::WaitWorldJobsFn fn, void* world) {
    __try {
        fn(world);
        return true;
    } __except (world_jobs_fault_filter(GetExceptionCode())) {
        return false;  // not reached: the filter passes every fault on
    }
}

void command_reply_sink(const std::string& line) {
    // a private reply (saves.list: the player's character and world names) is shown, never logged
    if (!emf::core::reply_is_private()) emf::logf("[cmd] %s", line.c_str());
    g_console.write_line(line);
    if (auto* c = emf::core::overlay_console()) c->add_line(line);
}

std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// [chat] debug=1: research dumps to find a chat sender's entity (the server's player slots, BasicPlayerInfos entries).
void chat_debug_dump(uint8_t* server, uint32_t handle, uint64_t account) {
    auto hex = [](const uint8_t* p, size_t n) {
        std::string s;
        char b[4];
        for (size_t i = 0; i < n; ++i) {
            std::snprintf(b, sizeof b, "%02x", p[i]);
            s += b;
            if (i % 4 == 3) s += ' ';
        }
        return s;
    };
    const EmfApi* api = emf::core::api_table();
    emf::logf("chat debug: handle 0x%x account 0x%016llx server %p local player %u", handle,
              static_cast<unsigned long long>(account), static_cast<void*>(server), api->local_player());
    uint8_t slots[17 * 8];
    if (server && emf::seh_read(server + 0x16a0, slots, sizeof slots))
        emf::logf("chat debug: server slots +0x16a0: %s", hex(slots, sizeof slots).c_str());
    // the sender's connection record (one of the 16 at Server+0x1E0, server_layout)
    uint8_t conn[emf::core::server_layout::kConnectionSize];
    const auto conn_at = emf::core::connection_record_offset(handle);
    if (!conn_at)
        emf::logf("chat debug: handle 0x%x is past the %u connection records", handle,
                  emf::core::server_layout::kConnectionCount);
    else if (server && emf::seh_read(server + *conn_at, conn, sizeof conn))
        for (size_t row = 0; row < sizeof conn; row += 32)
            emf::logf("chat debug: connection +0x%03zx: %s", row, hex(conn + row, std::min<size_t>(32, sizeof conn - row)).c_str());
    EmfWorld* w = api->server_world();
    auto* infos = static_cast<uint8_t*>(w ? api->singleton(w, "BasicPlayerInfos") : nullptr);
    emf::logf("chat debug: BasicPlayerInfos %p", static_cast<void*>(infos));
    for (uint32_t i = 0; infos && i < 8; ++i) {
        uint8_t e[0xA0];
        if (!emf::seh_read(infos + i * 0xA0, e, sizeof e)) break;
        uint32_t id = 0;
        std::memcpy(&id, e, 4);
        if (id != i + 1) continue;
        for (int row = 0; row < 5; ++row)
            emf::logf("chat debug: info[%u] +0x%02x: %s", i, row * 32, hex(e + row * 32, 32).c_str());
    }
}

// A chat sender's entity in the server world: the BasicPlayerInfos entry that holds their account hash and whose
// entity is alive (has a CurrentTransform); 0 if none or several. Runs on the server world's thread.
uint32_t chat_sender_entity(uint64_t account) {
    constexpr uint32_t kCurrentTransform = 0x027ac564u;
    const EmfApi* api = emf::core::api_table();
    EmfWorld* w = api->server_world();
    const auto* infos = static_cast<const uint8_t*>(w ? api->singleton(w, "BasicPlayerInfos") : nullptr);
    if (!infos) return 0;
    std::vector<uint8_t> copy(emf::core::kPlayerInfoCount * emf::core::kPlayerInfoSize, 0);
    size_t n = 0;  // entries read (stops early if the table ends before entity 1023)
    while (n < emf::core::kPlayerInfoCount &&
           emf::seh_read(infos + n * emf::core::kPlayerInfoSize, copy.data() + n * emf::core::kPlayerInfoSize,
                         emf::core::kPlayerInfoSize))
        ++n;
    return emf::core::entity_by_account(copy.data(), n, account, [api, w](uint32_t e) {
        return api->get_component(w, kCurrentTransform, e, nullptr) != nullptr;
    });
}

// chat.players (admin): the players of the server world with their account hashes (what [chat] admins takes), the
// connection handle their BasicPlayerInfos entry holds and whether the server's connection record for it matches (their
// chat reader), and their name. (No raw dump: connection records hold players' network data, and logs go into bug
// reports.)
void cmd_chat_players(EmfWorld* w, int argc, const char* const* argv, void*) {
    const EmfApi* api = emf::core::api_table();
    (void)argc;
    (void)argv;
    const auto* infos = static_cast<const uint8_t*>(api->singleton(w, "BasicPlayerInfos"));
    if (!infos) {
        api->command_reply("this world has no player list (BasicPlayerInfos)");
        return;
    }
    using namespace emf::core;
    std::vector<uint8_t> records(static_cast<size_t>(server_layout::kConnectionSize) * server_layout::kConnectionCount, 0);
    uint8_t* const server = chat_server();
    const bool have_records = server && emf::seh_read(server + server_layout::kConnections, records.data(), records.size());
    const MemoryRead read = [](const void* src, void* dst, size_t n) { return emf::seh_read(src, dst, n); };
    int count = 0;
    for (uint32_t id = 1; id <= kPlayerInfoCount; ++id) {
        uint8_t e[kPlayerInfoSize];
        if (!emf::seh_read(infos + static_cast<size_t>(id - 1) * kPlayerInfoSize, e, sizeof e)) break;
        const auto info = read_player_info(e, id);
        if (!info) continue;
        ++count;
        std::string conn = "no connection records (players' chat is off, or no line came in yet)";
        if (have_records) {
            const auto at = connection_record_offset(info->handle);
            const auto reader = at ? reader_from_connection(records.data() + (*at - server_layout::kConnections),
                                                            info->handle, info->account)
                                   : std::nullopt;
            if (reader) conn = "connection record matches, chat reader " + std::to_string(*reader);
            else if (const auto c = connection_of(records.data(), info->account)) {
                char h[96];
                std::snprintf(h, sizeof h, "the handle does not match its record; the account's record is 0x%x (reader %u)",
                              c->handle, c->reader);
                conn = h;
            }
            else conn = "no connection record (the host's own game, or not connected)";
        }
        const auto name = have_records ? connection_name(records.data(), info->handle, info->account, read) : std::nullopt;
        char line[256];
        std::snprintf(line, sizeof line, "player %u: account %s, handle 0x%x, %s, name %s", id,
                      account_text(info->account).c_str(), info->handle, conn.c_str(),
                      name ? printable(*name).c_str() : "(not known)");
        api->command_reply("%s", line);
    }
    if (!count) api->command_reply("no players in this world's player list");
}

void hk_sim_step(void* storage, char ticked) {
    o_sim_step(storage, ticked);
    try {
        emf::WorldView view = emf::WorldView::from_storage(storage);
        if (view.state() != 2) {  // world not running yet
            emf::core::bridge::note_world_setup(GetTickCount64());  // the debug bridge's "loading"
            return;
        }
        const uint8_t kind = view.kind();
        if (kind > 2) return;
        auto* world = reinterpret_cast<EmfWorld*>(view.raw());
        // The server world's SimStep has returned with its grid update and range queries still running on the game's
        // workers; the game waits for them later in this frame. Wait now, so mods' on_tick and commands (spawn,
        // destroy, give) never change the grid under them. Other worlds: nothing to wait for.
        g_world_jobs.before_tick_work(world, kind);
        g_state->tracker.on_tick(world, kind, GetTickCount64());
        g_state->dispatcher.run(world, kind, [](const std::string& e) { emf::logf("dispatch: %s", e.c_str()); });
        emf::core::pump_setting_changes(world);  // v13 on_setting_changed
    } catch (...) {
        // never let a C++ exception reach game code
    }
}
uint64_t hk_register_program(void* world, const uint8_t* desc, char disabled) {
    const uint64_t result = o_register_program(world, desc, disabled);
    try {
        const char* name = nullptr;
        uint64_t len = 0;
        uintptr_t fn = 0;
        if (g_state->programs && emf::seh_read(desc, &name, 8) && emf::seh_read(desc + 8, &len, 8) &&
            emf::seh_read(desc + 0x10, &fn, 8) && name && len && len < 128) {
            std::string n(len, '\0');
            if (emf::seh_read(name, n.data(), len)) g_state->programs->on_program_registered(n, fn);
        }
    } catch (...) {
    }
    return result;
}

// Save state offsets read from save2WriteWorld's code at startup (0 = not found: the save hook is not installed).
uint32_t g_save2_result_offset = 0;
uint32_t g_save2_slot_offset = 0;
// GameServer instance's "local world slot set" flag, read from loadSavegameStep's code (0 = not found: no load hook).
uint32_t g_inst_slot_flag_offset = 0;
// put_items: the emf_inventory program and its request queue (null when the inventory signatures are not usable).
std::unique_ptr<emf::core::InventoryService> g_inventory;
// teleport: the emf_teleport program and its requests (null when off on this game build).
std::unique_ptr<emf::core::TeleportService> g_teleport;

// Save2Thread: a world save was just written (result byte 0 = success).
void hk_save2_write_world(uint8_t* save2, void* user) {
    o_save2_write_world(save2, user);
    uint16_t slot = 0;
    if (emf::core::saved_world_slot(save2, g_save2_result_offset, g_save2_slot_offset, &slot)) {
        const int faults = g_state->save_events.fire(slot);
        emf::logf("world saved (slot 0x%04x)%s", slot, faults ? ", a save callback faulted" : "");
    }
}

// Server thread, instance state "load savegame": returns result byte 1 while waiting, 0 when loaded.
uint8_t* hk_load_savegame_step(uint8_t* out, uint8_t* inst) {
    uint8_t* r = o_load_savegame_step(out, inst);
    if (inst) {  // v19 request_save: the GameServer instance of this session (its load runs after the world's setup)
        g_state->save_instance = inst;
        g_state->save_instance_session = g_state->server_session.load();
    }
    uint8_t result = 0xFF;
    uint16_t slot = 0;
    if (emf::seh_read(r, &result, 1) && g_load_guard.on_step(inst, result) &&
        emf::core::loaded_world_slot(inst, g_inst_slot_flag_offset, &slot)) {
        const int faults = g_state->load_events.fire(slot);
        emf::logf("world loaded (slot 0x%04x)%s", slot, faults ? ", a load callback faulted" : "");
        if (auto* info = emf::core::save_info_recorder()) info->on_world_loaded(slot);  // save-info.json
    }
    return r;
}

// Called once per world during its setup, after all game programs registered and before the first tick.
void hk_register_all_programs(void* world) {
    o_register_all_programs(world);
    try {
        const ULONGLONG wait_start = GetTickCount64();
        switch (g_mods_ready.wait(kModsReadyTimeout)) {
            case emf::core::ReadyGate::Wait::Opened:
                emf::logf("world setup waited %llu ms for the mods to load", GetTickCount64() - wait_start);
                break;
            case emf::core::ReadyGate::Wait::TimedOut:
                emf::logf("world setup waited %lld s for the mods to load and goes on without them: their programs and "
                          "event subscriptions miss this world", static_cast<long long>(kModsReadyTimeout.count()));
                break;
            case emf::core::ReadyGate::Wait::GaveUp:
                emf::logf("world setup goes on without waiting: EMF's start-up still has not finished (an earlier world "
                          "setup waited %lld s for it); mods' programs and event subscriptions miss this world",
                          static_cast<long long>(kModsReadyTimeout.count()));
                break;
            default:
                break;
        }
        const uint8_t kind = emf::WorldView(static_cast<uint8_t*>(world)).kind();
        if (kind == EMF_WORLD_SERVER) {
            g_load_guard.reset();         // a new session: its save load must fire on_load
            g_state->relay.new_session();  // and an event overflow is logged again
            emf::core::bridge::note_session_start();  // the bridge forgets the last world's save slot
            if (g_inventory) g_inventory->world_reset();  // requests for the old world end with 0
            if (g_teleport) g_teleport->world_reset();    // teleports under way end (FAILED)
            g_state->damage_queue.clear();                // hits for the old session never land
            ++g_state->server_session;                    // mods notice the new session (API v9)
        }
        g_state->relay.forget_world(world);                  // nothing an earlier world left at this address
        if (g_register_program_addr && !g_state->relay.closed()) {
            const size_t relays = emf::core::add_relay_programs(
                g_state->relay, g_state->system_programs, static_cast<uint8_t*>(world), g_state->relay_programs,
                [](const std::string& s) { emf::logf("%s", s.c_str()); });
            if (relays) emf::logf("on_event: %zu event relay program(s) added", relays);
        }
        if (g_register_program_addr && g_state->system_programs.count()) {
            std::vector<std::string> problems;
            const size_t n = g_state->system_programs.register_into(
                world, reinterpret_cast<emf::core::RegisterProgramFn>(g_register_program_addr), &problems);
            for (const auto& p : problems) emf::logf("%s", p.c_str());
            emf::logf("registered %zu mod program(s) into world %p (kind %u)", n, world, kind);
        }
        g_state->system_programs.close_registration();  // later register_program calls would reach no world
        // On the host the client world registers after the server world: anything still pending is unknown.
        if (kind == EMF_WORLD_CLIENT && g_state->programs)
            for (const auto& name : g_state->programs->take_unreported_pending())
                emf::logf("hook_program('%s'): no program with that name was registered (see docs/ecs_programs.md); "
                          "the hook stays inactive", name.c_str());
    } catch (...) {
    }
}
}  // namespace

namespace {
// At most `limit` bytes of a file (the start is enough to recognise a DLL; a mod DLL is rarely bigger).
std::string read_head(const std::wstring& path, size_t limit) {
    std::string text;
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return text;
    char buf[65536];
    size_t n = 0;
    while (text.size() < limit && (n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    return text;
}

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Save backups (docs/saves.md): a backup of the player's saves at game start ([backup] in emf\emf_core.ini: on_start, keep,
// min_minutes), the starter backup when there is none yet. It runs on its own thread, so EMF's start does not wait; the
// game is still starting then (logos, loading), long before a world can write a save, and a file that changes while it
// is copied is copied again or named. Never in the dedicated server (its world lives in its own savegame folder).
void start_auto_backup(const std::wstring& dir) {
    const emf::saves::AutoSettings settings = emf::saves::read_auto_settings(dir + L"\\emf\\emf_core.ini");
    std::thread([settings] {
        try {
            const ULONGLONG t0 = GetTickCount64();
            const auto roots = emf::saves::with_app_data(emf::saves::default_roots(false), emf::saves::default_app_data());
            emf::saves::BackupOptions o;
            o.made_by = "game";
            o.emf_version = EMF_VERSION;
            o.info = emf::saves::read_save_info(roots.info).slots;
            const auto a = emf::saves::auto_backup(roots, settings, o);
            emf::logf("save backup at game start: %s (%llu ms)", a.text.c_str(),
                      static_cast<unsigned long long>(GetTickCount64() - t0));
            if (a.made.ok)
                for (const auto& p : a.made.backup.problems) emf::logf("save backup: not copied: %s", p.c_str());
        } catch (...) {
            emf::logf("save backup at game start: stopped by an error");
        }
    }).detach();
}

// Which tool owns each proxy DLL next to the game, what the loader chain-loaded (emf\chain_status.txt), and a loader
// whose file another tool took (emf\known_tools.txt names them). Reading the DLLs takes a moment: it runs on its own
// thread, so EMF's start (and a held server) does not wait for it.
void log_installed(const std::wstring& dir) {
    constexpr size_t kDllHead = 32u << 20;
    const auto tools = emf::core::parse_known_tools(read_head(dir + L"\\emf\\known_tools.txt", 1u << 20));
    emf::core::GameFolderView view;
    for (const auto& name : emf::core::proxy_dll_names()) {
        const std::wstring path = dir + L"\\" + widen(name);
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            view.proxies.emplace_back(name, emf::core::tool_for_dll_text(tools, read_head(path, kDllHead)));
    }
    // written by the loader in its DllMain, before it started EMF's core
    view.chain_status = read_head(dir + L"\\emf\\chain_status.txt", 1u << 20);
    for (const auto& name : emf::core::started_chain_names(view.chain_status))
        view.chained_tools.push_back(emf::core::tool_for_dll_text(tools, read_head(dir + L"\\" + widen(name), kDllHead)));
    for (const auto& t : tools)
        for (const auto& b : t.beside)
            if (GetFileAttributesW((dir + L"\\" + widen(b)).c_str()) != INVALID_FILE_ATTRIBUTES) view.beside.push_back(b);
    for (const auto& line : emf::core::installed_report(tools, view)) emf::logf("%s", line.c_str());
}
}  // namespace

extern "C" __declspec(dllexport) void emf_core_start(const wchar_t* game_dir) {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;  // once per process, even if two loaders call it
    // every way out of here (mods loaded, or EMF idle after a problem) lets a waiting world setup go on
    g_mods_ready.set_opener(std::this_thread::get_id());
    struct OpenOnExit {
        ~OpenOnExit() { g_mods_ready.open(); }
    } open_on_exit;

    const std::wstring dir = game_dir ? game_dir : L".";
    emf::log_open(dir + L"\\emf\\emf_core.log");
    // [debug] bridge=1: the debug bridge (started below); its log ring keeps lines from here on
    const bool debug_bridge = emf::core::bridge::bridge_switch_on(dir + L"\\emf\\emf_core.ini");
    if (debug_bridge) emf::log_ring_enable(5000);
    emf::logf("%s starting (game dir %s)", kVersion, utf8(dir).c_str());
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const std::wstring exe_name = std::wstring(self).substr(std::wstring(self).find_last_of(L"\\/") + 1);
        if (_wcsicmp(exe_name.c_str(), L"enshrouded_server.exe") != 0) start_auto_backup(dir);  // first: before any save
    }
    std::thread([dir] {
        try {
            log_installed(dir);
        } catch (...) {
        }
    }).detach();
    // EMF's Vulkan layer may already have seen the game's device (the loader can load this DLL first): its lines go
    // to the log from here on.
    emf::core::set_vulkan_log([](const char* s) { emf::logf("%s", s); });

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::string sha = emf::sha256_file(exe);
    const int build = emf::find_known_build(sha);
    const bool known = build >= 0;
    if (known)
        emf::logf("game sha256 %s (known build %u)", sha.c_str(), emf::known_builds()[static_cast<size_t>(build)].number);
    else
        emf::logf("game sha256 %s (UNKNOWN build: signatures only)", sha.c_str());

    g_state = std::make_unique<emf::core::CoreState>();
    g_state->sigs = emf::resolve_all(GetModuleHandleW(nullptr), build);
    g_state->known_build = known;
    {  // the exe as find_pattern / game_build see it
        const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        auto& e = g_state->exe;
        e.base = reinterpret_cast<uintptr_t>(base);
        e.size = nt->OptionalHeader.SizeOfImage;
        e.sections = emf::sections(base);
        e.sha256 = sha;
        e.build_number = known ? emf::known_builds()[static_cast<size_t>(build)].number : 0;
        std::wstring name(exe);
        name = name.substr(name.find_last_of(L"\\/") + 1);
        e.server = _wcsicmp(name.c_str(), L"enshrouded_server.exe") == 0;
        if (e.server) {
            emf::log_to_console(true);  // the server's window shows EMF's lines next to its own
            emf::logf("running inside the dedicated server");
        }
        // Patterns are matched against the exe as it is on disk: hooks rewrite the first bytes of what they hook.
        if (const uintptr_t copy = emf::map_image_file(exe))
            e.pristine_base = copy;  // kept mapped for the process
        else
            emf::logf("could not map the exe file (error %lu): patterns are matched against the live code",
                      GetLastError());
    }
    // Every inline hook goes through one table, so hooks on the same function chain (API v14).
    emf::hooks_init();
    g_state->hooks = std::make_unique<emf::core::HookTable>([](uintptr_t target, void* detour, void** original,
                                                               std::string* why) {
        return emf::hook_install(target, detour, original, why);  // removed again if enabling fails
    });
    g_state->hooks->set_code_check([](uintptr_t a) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) return false;
        const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE && (mbi.Protect & exec) != 0;
    });
    g_state->log_line = [](const std::string& s) { emf::logf("%s", s.c_str()); };
    emf::core::set_fault_reporter([](const std::string& s) { emf::logf("%s", s.c_str()); });
    g_state->now_ms = [] { return GetTickCount64(); };
    auto defs = emf::all_signatures();
    int usable = 0;
    int absent = 0;
    for (size_t i = 0; i < emf::kSigCount; ++i) {
        // client-only code on the server exe counts as absent (an unknown server build names it, not as an error)
        const auto report = emf::report_signature(defs[i], g_state->sigs[i], build, g_state->exe.server);
        usable += report.usable;
        absent += report.absent;
        if (!report.note.empty()) emf::logf("%s", report.note.c_str());
    }
    if (absent) emf::logf("signatures: %d of %zu usable (%d not in this exe)", usable, emf::kSigCount, absent);
    else emf::logf("signatures: %d of %zu usable", usable, emf::kSigCount);
    if (!emf::required_signatures_ok(g_state->sigs, known)) {
        emf::logf("a required signature is missing: EMF stays idle, no mods loaded");
        return;
    }
    // Before EMF's tick work on the server world: the game's own wait for that world's jobs (core/world_jobs.h).
    {
        const auto& wait = g_state->sigs[static_cast<size_t>(emf::Sig::WaitWorldJobs)];
        if (emf::is_usable(wait, known)) g_world_jobs.set_wait(reinterpret_cast<emf::core::WaitWorldJobsFn>(wait.addr));
        g_world_jobs.set_caller(&guarded_wait_world_jobs);
        g_world_jobs.set_log([](const std::string& s) { emf::logf("%s", s.c_str()); });
        emf::logf("%s", emf::core::WorldJobsGate::startup_line(g_world_jobs.available()).c_str());
    }
    g_state->get_component = reinterpret_cast<emf::GetComponentFn>(
        g_state->sigs[static_cast<size_t>(emf::Sig::GetComponent)].addr);
    if (emf::is_usable(g_state->sigs[static_cast<size_t>(emf::Sig::GetTemplateComponent)], known))
        g_state->get_template_component = reinterpret_cast<emf::GetTemplateComponentFn>(
            g_state->sigs[static_cast<size_t>(emf::Sig::GetTemplateComponent)].addr);
    else
        emf::logf("GetTemplateComponent not found: get_template_component returns NULL");
    g_state->programs = std::make_unique<emf::core::ProgramRegistry>([](uintptr_t target, void* detour, void** original) {
        const bool ok = emf::core::install_emf_hook("program update (hook_program)", target, detour, original);
        emf::logf("program hook at %p: %s", reinterpret_cast<void*>(target), ok ? "installed" : "FAILED");
        return ok;
    });
    emf::core::set_core_state(g_state.get());
    // The bridge's load_world: the main-menu hooks, only with the bridge on and only in the game exe (the server has no
    // menu). Early, so the hooks are in place before the game's pregame enters the main menu.
    if (debug_bridge && !g_state->exe.server) {
        emf::core::MenuStart::Deps menu;
        menu.world_ready = [] {
            return g_state->tracker.latest(EMF_WORLD_SERVER, GetTickCount64()) != nullptr &&
                   emf::core::api_table()->local_player() != 0;
        };
        menu.any_world_running = [] {
            const uint64_t now = GetTickCount64();
            return g_state->tracker.latest(EMF_WORLD_SERVER, now) || g_state->tracker.latest(EMF_WORLD_CLIENT, now);
        };
        menu.now_ms = [] { return GetTickCount64(); };
        menu.log = [](const std::string& s) { emf::logf("%s", s.c_str()); };
        if (emf::core::install_menu_start(g_state->sigs, known, build, std::move(menu), &g_load_world_off))
            emf::logf("load_world: on (main-menu hooks installed; the guard acts only during a load_world)");
        else
            emf::logf("load_world: off (%s)", g_load_world_off.c_str());
    }
    // save-info.json: the slot names and levels the main menu shows, for the save backups' list. The game
    // exe only; the hook runs before the game's pregame enters the main menu.
    if (!g_state->exe.server) {
        emf::core::SaveInfoRecorder::Deps info;
        info.path = emf::saves::with_app_data({}, emf::saves::default_app_data()).info;
        info.log = [](const std::string& s) { emf::logf("%s", s.c_str()); };
        info.run = [](std::function<void()> job) { std::thread(std::move(job)).detach(); };  // never on the game's thread
        // Steam names go with the account Steam has logged in (HKCU\Software\Valve\Steam\ActiveProcess ActiveUser: the
        // userdata folder's number), so they never cross Steam accounts
        info.steam_user = [] {
            DWORD id = 0, size = sizeof id;
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser", RRF_RT_REG_DWORD,
                             nullptr, &id, &size) != ERROR_SUCCESS || id == 0)
                return std::string();
            return std::to_string(id);
        };
        std::string why;
        if (info.path.empty())
            emf::logf("save info: off (Windows does not say where %%LOCALAPPDATA%% is)");
        else if (emf::core::install_save_info(g_state->sigs, known, build, std::move(info), &why))
            emf::logf("save info: on (the main menu's slot names and levels go to save-info.json)");
        else
            emf::logf("save info: off (%s)", why.c_str());
    }
    const auto& iter_begin = g_state->sigs[static_cast<size_t>(emf::Sig::IterBegin)];
    if (emf::is_usable(iter_begin, known)) {
        emf::core::set_program_iter_begin(reinterpret_cast<void (*)(void*, void*, uint32_t)>(iter_begin.addr));
    } else {
        // Without iter_begin no program can read events: say so instead of delivering nothing.
        g_state->relay.close("the IterBegin signature is not usable on this game build");
        emf::logf("on_event and read_events unavailable: the IterBegin signature is not usable on this game build");
    }

    if (!emf::core::install_emf_hook("SimStep", g_state->sigs[static_cast<size_t>(emf::Sig::SimStep)].addr,
                                     reinterpret_cast<void*>(&hk_sim_step), reinterpret_cast<void**>(&o_sim_step))) {
        emf::logf("could not hook the sim step: EMF stays idle");
        return;
    }
    const auto& reg = g_state->sigs[static_cast<size_t>(emf::Sig::RegisterProgram)];
    if (emf::is_usable(reg, known) &&
        emf::core::install_emf_hook("RegisterProgram", reg.addr, reinterpret_cast<void*>(&hk_register_program),
                                    reinterpret_cast<void**>(&o_register_program))) {
        emf::logf("registerProgram hooked (program hooks available)");
        g_register_program_addr = reg.addr;
    } else {
        emf::logf("registerProgram not hooked: hook_program and register_program are unavailable");
        g_state->programs.reset();  // hook_program now returns false instead of queueing forever
    }
    const auto& all = g_state->sigs[static_cast<size_t>(emf::Sig::RegisterAllPrograms)];
    if (g_register_program_addr && emf::is_usable(all, known) &&
        emf::core::install_emf_hook("RegisterAllPrograms", all.addr, reinterpret_cast<void*>(&hk_register_all_programs),
                                    reinterpret_cast<void**>(&o_register_all_programs))) {
        emf::logf("registerAllPrograms hooked (mod programs available)");
    } else {
        emf::logf("registerAllPrograms not hooked: register_program and on_event unavailable");
        g_state->system_programs.close_registration();  // register_program now returns false
        g_state->relay.close("registerAllPrograms is not hooked on this game build");  // on_event returns false
    }
    // v18 attributes and damage (docs/engine_parts/re_attributes_menu.md): attrFind / attrAdd for attribute_get and
    // attribute_add; damage queues a pending hit that impact_hit_conclusion's pre-hook hands to the game.
    {
        auto sig = [&](emf::Sig s) -> uintptr_t {
            const auto& r = g_state->sigs[static_cast<size_t>(s)];
            return emf::is_usable(r, known) ? r.addr : 0;
        };
        // Only on a build whose layouts (AttrInfo's slot, attrAdd, the HitCandidate) were checked: after a game update a
        // shifted offset must never turn damage into something else. Elsewhere the three signatures count as missing
        // (so emf::kill takes its old path).
        if (!emf::core::attribute_layouts_checked(build)) {
            for (const auto s : {emf::Sig::AttrFind, emf::Sig::AttrAdd, emf::Sig::AllocPendingHit})
                g_state->sigs[static_cast<size_t>(s)].count = 0;
            emf::logf("attributes: off: attribute_get, attribute_add and damage rely on layouts EMF checked on build 1273043 "
                      "(game and server) only; this build is %s", known ? "one whose exe was not at hand" : "unknown");
        }
        g_state->attr_find = reinterpret_cast<emf::core::AttrFindFn>(sig(emf::Sig::AttrFind));
        g_state->attr_add = reinterpret_cast<emf::core::AttrAddFn>(sig(emf::Sig::AttrAdd));
        g_state->alloc_pending_hit = reinterpret_cast<emf::core::AllocPendingHitFn>(sig(emf::Sig::AllocPendingHit));
        if (g_state->alloc_pending_hit && g_state->programs) {
            // damage switches on once impact_hit_conclusion's pre-hook is really installed (note_program_hook)
            g_state->reserved_programs = {"impact_hit_conclusion", "clear_hit_candidates"};
            g_state->programs->set_report([](const std::string& name, bool ok) { emf::core::note_program_hook(name, ok); });
            g_state->programs->request_hook(
                "impact_hit_conclusion", [](void* ctx, void*) { emf::core::damage_pre_conclusion(ctx); }, nullptr, nullptr);
            // the order check only: clear_hit_candidates should run earlier in the same step
            g_state->programs->request_hook(
                "clear_hit_candidates", [](void* ctx, void*) { emf::core::damage_pre_clear(ctx); }, nullptr, nullptr);
        }
        emf::logf("attributes: attribute_get %s, attribute_add %s, damage %s", g_state->attr_find ? "on" : "off",
                  g_state->attr_find && g_state->attr_add ? "on" : "off",
                  g_state->alloc_pending_hit && g_state->programs
                      ? "on once impact_hit_conclusion's pre-hook is installed (at world setup)"
                      : "off (AllocPendingHit or the program hooks are not available)");
    }
    // v19 request_save: the game's requestServerSave, called with the instance loadSavegameStep last ran for
    {
        const auto& r = g_state->sigs[static_cast<size_t>(emf::Sig::RequestServerSave)];
        if (emf::is_usable(r, known)) g_state->request_save = reinterpret_cast<emf::core::CoreState::RequestSaveFn>(r.addr);
        emf::logf("request_save: %s", g_state->request_save ? "on (the game's own save request, once a save loaded)"
                                                            : "off (the RequestServerSave signature is not usable)");
    }
    // put_items: an internal server program runs the game's inventory transaction (docs/engine_parts/re_inventory_api.md).
    {
        using emf::Sig;
        auto sig = [&](Sig s) -> uintptr_t {
            const auto& r = g_state->sigs[static_cast<size_t>(s)];
            return emf::is_usable(r, known) ? r.addr : 0;
        };
        emf::core::InventoryService::Deps deps;
        deps.fns.ctor = reinterpret_cast<emf::core::TxCtorFn>(sig(Sig::InventoryTxCtor));
        deps.fns.init = reinterpret_cast<emf::core::TxInitFn>(sig(Sig::InventoryTxInit));
        deps.fns.add_items = reinterpret_cast<emf::core::AddItemsFn>(sig(Sig::InventoryAddItems));
        deps.fns.remove_items = reinterpret_cast<emf::core::RemoveItemsFn>(sig(Sig::InventoryRemoveItems));  // optional
        deps.fns.commit = reinterpret_cast<emf::core::TxFn>(sig(Sig::InventoryCommit));
        deps.fns.rollback = reinterpret_cast<emf::core::TxFn>(sig(Sig::InventoryRollback));
        deps.fns.free_arrays = reinterpret_cast<emf::core::TxFn>(sig(Sig::InventoryFreeArrays));
        deps.read_other = reinterpret_cast<emf::core::ReadOtherFn>(sig(Sig::ReadOther));
        deps.get_component = g_state->get_component;
        deps.log = [](const std::string& line) { emf::logf("%s", line.c_str()); };
        deps.now_ms = [] { return GetTickCount64(); };
        deps.after_step = [](std::function<void(EmfWorld*)> fn) {
            auto* job = new std::function<void(EmfWorld*)>(std::move(fn));
            g_state->dispatcher.post(EMF_WORLD_SERVER, [](EmfWorld* w, void* u) {
                auto* f = static_cast<std::function<void(EmfWorld*)>*>(u);
                (*f)(w);
                delete f;
            }, job);
        };
        const bool iter_ok = sig(Sig::IterBegin) != 0;
        if (!deps.fns.complete() || !deps.read_other || !iter_ok) {
            emf::logf("put_items: off (the inventory transaction, ReadOther or IterBegin signatures are not usable)");
        } else if (g_state->system_programs.registration_closed()) {
            emf::logf("put_items: off (mod programs are unavailable)");
        } else {
            g_inventory = std::make_unique<emf::core::InventoryService>(std::move(deps));
            if (g_state->system_programs.add(g_inventory->spec(), true)) {
                g_state->inventory = g_inventory.get();
                emf::logf("put_items: on (program %s in the server world)", emf::core::kInventoryProgramName);
            } else {
                emf::logf("put_items: off (the %s program could not be added)", emf::core::kInventoryProgramName);
                g_inventory.reset();
            }
        }
    }
    // teleport: an internal server program pushes the game's add-Teleport record (docs/engine_parts/re_teleport.md).
    {
        const auto& qp = g_state->sigs[static_cast<size_t>(emf::Sig::QueuePush)];
        std::string off;
        if (!emf::is_usable(qp, known))
            off = "the QueuePush signature is not usable on this game build";
        else if (!emf::core::teleport_layouts_checked(build))
            off = std::string("its command-buffer layout was checked on build 1273043 (game and server) only; this build is ") +
                  (known ? "one whose exe was not at hand" : "unknown");
        else if (g_state->system_programs.registration_closed())
            off = "mod programs are unavailable";
        if (off.empty()) {
            emf::core::TeleportService::Deps deps;
            deps.queue_push = reinterpret_cast<emf::core::QueuePushFn>(qp.addr);
            deps.get_component = g_state->get_component;
            // dead = the current-health attribute at 0 (a player in the server world has no NetworkHealth: live)
            deps.health = [](uint8_t* world, uint32_t entity, int32_t* current) {
                const EmfApi* api = emf::core::api_table();
                return api->attribute_get(reinterpret_cast<EmfWorld*>(world), entity, EMF_ATTR_HEALTH, current);
            };
            deps.log = [](const std::string& line) { emf::logf("%s", line.c_str()); };
            deps.now_ms = [] { return GetTickCount64(); };
            deps.session = [] { return g_state->server_session.load(); };
            g_teleport = std::make_unique<emf::core::TeleportService>(std::move(deps));
            if (g_state->system_programs.add(g_teleport->spec(), true)) {
                g_state->teleport = g_teleport.get();
                emf::logf("teleport: on (program %s in the server world)", emf::core::kTeleportProgramName);
                // Back to the menu (no server world for 10 s, and none set up since): every teleport still under way is
                // answered, so mods and the Lua host free what they hold. (A new world answers them in its setup.)
                std::thread([] {
                    for (;;) {
                        Sleep(1000);
                        if (g_teleport && g_teleport->under_way() &&
                            !g_state->tracker.latest_within(EMF_WORLD_SERVER, GetTickCount64(), 10000)) {
                            const size_t n = g_teleport->end_session("the world ended (back to the menu)");
                            if (n) emf::logf("teleport: %zu request(s) answered: the world ended", n);
                        }
                    }
                }).detach();
            } else {
                g_teleport.reset();
                off = std::string("the ") + emf::core::kTeleportProgramName + " program could not be added";
            }
        }
        if (!off.empty()) {
            g_state->teleport_off = off;
            emf::logf("teleport: off (%s)", off.c_str());
        }
    }
    auto hook_sig = [&](emf::Sig s, void* detour, void** original, const char* what) {
        const auto& r = g_state->sigs[static_cast<size_t>(s)];
        const bool ok = emf::is_usable(r, known) && emf::core::install_emf_hook(what, r.addr, detour, original);
        emf::logf("%s %s", what, ok ? "hooked" : "NOT hooked (event unavailable)");
    };
    // on_save needs two save-state offsets read from save2WriteWorld's code (they moved in build 1273043).
    const auto code_offsets = emf::resolve_code_offsets(GetModuleHandleW(nullptr), g_state->sigs, build);
    const auto& result_off = code_offsets[static_cast<size_t>(emf::CodeOffset::Save2Result)];
    const auto& slot_off = code_offsets[static_cast<size_t>(emf::CodeOffset::Save2WorldSlot)];
    const auto& load_flag = code_offsets[static_cast<size_t>(emf::CodeOffset::InstWorldSlotFlag)];
    for (const auto& note : emf::unconfirmed_offset_notes(code_offsets)) emf::logf("%s", note.c_str());
    g_state->tick_offsets_ok = emf::world_tick_offsets_ok(code_offsets, known);
    if (!g_state->tick_offsets_ok)
        emf::logf("the world's tick fields are not confirmed in SimStep's code: world_tick, ecs_time_ns and the time "
                  "command are off");
    auto sig_ok = [&](emf::Sig s) { return emf::is_usable(g_state->sigs[static_cast<size_t>(s)], known); };
    const auto save_plan = emf::plan_offset_hook("save2WriteWorld (on_save)", sig_ok(emf::Sig::Save2WriteWorld),
                                                 {{"Save2Result", emf::is_usable(result_off, known)},
                                                  {"Save2WorldSlot", emf::is_usable(slot_off, known)}});
    if (save_plan.hook) {
        g_save2_result_offset = result_off.value;
        g_save2_slot_offset = slot_off.value;
        emf::logf("save state offsets: result +0x%x, world slot +0x%x", result_off.value, slot_off.value);
        hook_sig(emf::Sig::Save2WriteWorld, reinterpret_cast<void*>(&hk_save2_write_world),
                 reinterpret_cast<void**>(&o_save2_write_world), "save2WriteWorld (on_save)");
    } else {
        emf::logf("%s (result: %zu match(es) 0x%x, world slot: %zu match(es) 0x%x)", save_plan.reason.c_str(),
                  result_off.count, result_off.value, slot_off.count, slot_off.value);
    }
    const auto load_plan = emf::plan_offset_hook("loadSavegameStep (on_load)", sig_ok(emf::Sig::LoadSavegameStep),
                                                 {{"InstWorldSlotFlag", emf::is_usable(load_flag, known)}});
    if (load_plan.hook) {
        g_inst_slot_flag_offset = load_flag.value;
        emf::logf("load slot offset: world slot +0x%x", load_flag.value - 2);
        hook_sig(emf::Sig::LoadSavegameStep, reinterpret_cast<void*>(&hk_load_savegame_step),
                 reinterpret_cast<void**>(&o_load_savegame_step), "loadSavegameStep (on_load)");
    } else {
        emf::logf("%s (%zu match(es) 0x%x)", load_plan.reason.c_str(), load_flag.count, load_flag.value);
    }
    // put_items watchdog: requests end with 0 when emf_inventory does not run in the server world.
    g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
        if (g_inventory && emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() == EMF_WORLD_SERVER)
            g_inventory->on_server_tick(w);
    }, nullptr);
    // teleport: the state checks, the watch and the callbacks after each server step.
    g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
        if (g_teleport && emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() == EMF_WORLD_SERVER)
            g_teleport->on_server_tick(w);
    }, nullptr);
    // Event relay first among the tick subscribers: mods' on_event callbacks run before their on_tick ones.
    g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
        g_state->relay.deliver(w, emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind(),
                               [](const std::string& s) { emf::logf("%s", s.c_str()); });
    }, nullptr);
    // v19 on_chat: players' chat lines reach mods on the server world's thread, inside its callbacks (like on_event)
    g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
        if (emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() == EMF_WORLD_SERVER) emf::core::pump_chat_lines(w);
    }, nullptr);
    // Developer commands: built-ins, emf/commands.txt (polled each second) and the optional console window. They run
    // on the server world's tick, inside the dispatch scope (so they may spawn and give items).
    g_builtin.api = emf::core::api_table();
    g_builtin.center = &g_state->commands;
    g_builtin.player = [] { return emf::core::command_player(emf::core::api_table()->local_player()); };
    g_state->settings_backend = std::make_unique<emf::core::FileIniBackend>(dir + L"\\emf\\config");
    g_state->settings = std::make_unique<emf::core::SettingsStore>(g_state->settings_backend.get());
    g_builtin.registry = &g_state->mods;
    g_builtin.settings = g_state->settings.get();
    if (!emf::core::add_builtin_commands(g_builtin)) emf::logf("commands: a built-in command could not be added");
    // Mod profiles: the same files and rules as EMF.exe's Mods tab; settings go through the live store
    g_profiles.game = dir;
    g_profiles.settings = g_state->settings.get();
    g_profiles.emf_version = EMF_VERSION;
    g_profiles.run = emf::core::profile_worker_runner();  // never on the render thread or a world's thread
    if (!emf::core::add_profile_commands(g_state->commands, g_profiles)) emf::logf("commands: \"profile\" could not be added");
    // Save backups from the console: for players without EMF.exe; the copying runs on the profile worker.
    // Not in the dedicated server: its world is in its own savegame folder.
    if (!g_state->exe.server) {
        g_save_commands.roots = emf::saves::with_app_data(emf::saves::default_roots(false), emf::saves::default_app_data());
        g_save_commands.emf_version = EMF_VERSION;
        g_save_commands.run = emf::core::profile_worker_runner();
        // saves.backup only truly at the main menu: no world hosted and no one else's world joined
        g_save_commands.in_a_world = [] {
            return emf::core::host_state(g_state->tracker, GetTickCount64()) != emf::core::HostState::None;
        };
        if (!emf::core::add_save_commands(g_state->commands, g_save_commands)) emf::logf("commands: \"saves.*\" could not be added");
    }
    // and the picker at the top of the in-game Mods tab (its reads and switches run on the same worker)
    static emf::core::ProfileBar profile_bar(g_profiles, emf::core::profile_worker_runner());
    emf::core::set_mods_tab_profile_bar(&profile_bar);
    g_command_file = std::make_unique<emf::core::CommandFile>(dir + L"\\emf\\commands.txt", dir + L"\\emf\\commands.done.txt");
    g_file_poll.commands_file = g_command_file.get();  // set before the tick below can use it
    g_file_poll.commands = &g_state->commands;
    g_file_poll.manifests = &g_manifest_watch;
    g_file_poll.mods_dir = dir + L"\\emf\\mods";
    g_file_poll.mods = &g_state->mods;
    g_file_poll.settings = g_state->settings.get();
    g_file_poll.reply = command_reply_sink;
    g_file_poll.log = [](const std::string& line) { emf::logf("%s", line.c_str()); };
    if (GetPrivateProfileIntW(L"console", L"window", 0, (dir + L"\\emf\\emf_core.ini").c_str()) != 0)
        emf::logf("console window: %s", g_console.open([](const std::string& line) {
            emf::core::submit_line(g_state->commands, emf::core::host_state(g_state->tracker, GetTickCount64()), line,
                                   command_reply_sink);
        }, L"EMF console") ? "open" : "could not be opened");
    g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
        // The server world runs commands; in someone else's game (no server world) the client world answers them.
        // A host's server world ticks 60 times a second; only 10 s without one means this is someone else's game.
        const bool server = g_state->tracker.latest_within(EMF_WORLD_SERVER, GetTickCount64(), 10000) != nullptr;
        const auto route = emf::core::command_route(emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind(), server);
        if (route == emf::core::CommandRoute::None) return;
        g_file_poll.tick(GetTickCount64());  // commands.txt and the mods' mod.json, at most once a second
        if (!g_state->commands.pending()) return;
        if (route == emf::core::CommandRoute::Run)
            g_state->commands.run_pending(w, command_reply_sink);
        else
            g_state->commands.reject_pending("not run: commands need a world you host (this is someone else's game)",
                                             command_reply_sink);
    }, nullptr);
    emf::logf("commands: write them into emf\\commands.txt, or set [console] window=1 in emf\\emf_core.ini");
    // The in-game drop-down console ([overlay] in emf\emf_core.ini: enabled=0 turns it off, key=<scan code>).
    // The dedicated server has no screen and no chat box of its own: neither starts there.
    if (g_state->exe.server) {
        emf::logf("dedicated server: no overlay; players' chat commands are set up below; this log also goes to the "
                  "server window");
    } else {
        const auto cfg = emf::core::read_overlay_config(dir + L"\\emf\\emf_core.ini");
        if (cfg.enabled) {
            emf::core::OverlayDeps deps;
            deps.center = &g_state->commands;
            deps.tracker = &g_state->tracker;
            deps.emf_dir = dir + L"\\emf";
            deps.reply = command_reply_sink;
            deps.item_names = [] {
                std::vector<std::string> names;
                const EmfApi* api = emf::core::api_table();
                if (EmfWorld* w = api->server_world())
                    api->for_each_item(w, [](const EmfItemInfo* i, void* u) {
                        static_cast<std::vector<std::string>*>(u)->emplace_back(i->name);
                    }, &names);
                return names;
            };
            deps.log = [](const char* s) { emf::logf("%s", s); };
            deps.wheel_done = [](const emf::core::WheelRequest& r, int choice) { emf::core::post_wheel_choice(r, choice); };
            deps.hud = &g_state->hud;
            deps.mods = &g_state->mods;
            deps.settings = g_state->settings.get();
            if (emf::core::start_overlay(cfg, std::move(deps))) {
                g_state->open_wheel = [](const emf::core::WheelRequest& r) {
                    const bool ok = emf::core::overlay_open_wheel(r);
                    if (!ok) emf::logf("wheel_open refused: the console or another wheel is open, or the overlay cannot draw");
                    return ok;
                };
                g_state->notify = [](const std::string& s) {
                    emf::logf("[note] %s", s.c_str());
                    emf::core::overlay_notify(s);
                };
            }
        } else {
            emf::logf("overlay: off ([overlay] enabled=0)");
        }
        // "/give ..." typed into the game's own chat runs as a command; the replies show briefly at the top of the
        // screen (and in the console and the log). Other chat is untouched.
        if (cfg.chat_commands) {
            emf::core::ChatDeps chat;
            chat.is_command = [](std::string_view word) {
                for (const auto& [name, help] : g_state->commands.list())
                    if (name == word) return true;
                return false;
            };
            // in someone else's game the line goes on to the server: an EMF server answers players' commands itself
            chat.ours = [] {
                return emf::core::host_state(g_state->tracker, GetTickCount64()) != emf::core::HostState::Client;
            };
            chat.run = [](const std::string& line) {
                emf::core::overlay_show_replies();
                emf::core::submit_line(g_state->commands, emf::core::host_state(g_state->tracker, GetTickCount64()),
                                       line, command_reply_sink);
            };
            const auto& qc = g_state->sigs[static_cast<size_t>(emf::Sig::QueueChatMessage)];
            const bool ok = emf::is_usable(qc, known) && emf::core::install_chat_commands(qc.addr, std::move(chat));
            emf::logf("chat commands: %s", ok ? "on (type /help into the game's chat)"
                                              : "off (the chat function was not found on this game build)");
        } else {
            emf::logf("chat commands: off ([overlay] chat=0)");
        }
    }
    // Players' /commands, caught where the server receives chat: always on the dedicated server; in a game this client
    // hosts only with [chat] players=1 (the host's own lines already run through the chat box hook above).
    {
        const std::wstring ini = dir + L"\\emf\\emf_core.ini";
        const bool players = g_state->exe.server || GetPrivateProfileIntW(L"chat", L"players", 0, ini.c_str()) != 0;
        if (players) {
            wchar_t admins[2048] = {};
            GetPrivateProfileStringW(L"chat", L"admins", L"", admins, 2048, ini.c_str());
            const bool everyone = GetPrivateProfileIntW(L"chat", L"everyone", 0, ini.c_str()) != 0;
            // [chat] deny: commands for players the server owner takes away from players who are no admins
            wchar_t deny[2048] = {};
            GetPrivateProfileStringW(L"chat", L"deny", L"", deny, 2048, ini.c_str());
            const auto denied = emf::core::parse_command_names(utf8(deny));
            if (!denied.empty()) {
                std::string list;
                for (const auto& d : denied) list += (list.empty() ? "" : ", ") + d;
                emf::logf("players' chat commands: [chat] deny takes %s away from players who are no admins", list.c_str());
            }
            g_state->commands.set_denied_for_players(denied);
            emf::core::ServerChatDeps sc;
            std::vector<std::string> skipped_admins;
            sc.policy = emf::core::parse_chat_policy(utf8(admins), everyone, &skipped_admins);
            for (const auto& entry : skipped_admins) emf::logf("%s", emf::core::skipped_admin_warning(entry).c_str());
            sc.is_command = [](std::string_view word) {
                for (const auto& [name, help] : g_state->commands.list())
                    if (name == word) return true;
                return false;
            };
            // commands a mod marked for every player (API v17 EMF_COMMAND_PLAYERS); the admins may run all of them
            sc.for_players = [](std::string_view word) { return g_state->commands.for_players(word); };
            sc.run = [](const std::string& line, const emf::core::CommandSender& sender, emf::core::ReplyFn reply) {
                g_state->commands.enqueue(line, GetTickCount64(), sender, std::move(reply));
            };
            sc.log = [](const std::string& t) { emf::logf("%s", t.c_str()); };
            if (GetPrivateProfileIntW(L"chat", L"debug", 0, ini.c_str()) != 0) sc.debug = chat_debug_dump;
            sc.on_line = [](emf::core::ChatLine line) { emf::core::note_chat_line(std::move(line)); };  // v19 on_chat
            g_state->entity_by_account = chat_sender_entity;
            // the sender's entity: the player info entry that holds their account hash (players are the first ids)
            // the sender's entity, looked up when the command runs (the server world's thread): the player info
            // entry that holds their account hash and whose entity is alive
            g_state->commands.set_sender_resolver([](emf::core::CommandSender& s) {
                s.entity = chat_sender_entity(s.account);
            });
            const size_t admin_count = sc.policy.admins.size();
            const auto& rcv = g_state->sigs[static_cast<size_t>(emf::Sig::ChatReceive)];
            const auto& add = g_state->sigs[static_cast<size_t>(emf::Sig::ChatLogAdd)];
            const bool ok = emf::is_usable(rcv, known) && emf::is_usable(add, known) &&
                            emf::core::install_server_chat(rcv.addr, add.addr, std::move(sc));
            if (!ok)
                emf::logf("players' chat commands: off (the server's chat functions were not found on this game build)");
            else if (everyone)
                emf::logf("players' chat commands: on, for every player ([chat] everyone=1)");
            else
                emf::logf("players' chat commands: on, for %zu admin account(s) ([chat] admins in emf\\emf_core.ini; a "
                          "refused player's account is logged)",
                          admin_count);
        }
    }
    if (!g_state->commands.add("chat.players",
                               ": the players of this world with their account hashes (for [chat] admins), "
                               "connection and name",
                               cmd_chat_players, nullptr, nullptr))
        emf::logf("commands: \"chat.players\" could not be added");
    if (debug_bridge) {  // after the overlay (its key hooks), before the mods (game events need the first world)
        emf::core::bridge::RuntimeDeps bridge;
        bridge.load_world_off = g_load_world_off;
        bridge.emf_dir = dir + L"\\emf";
        bridge.state = g_state.get();
        bridge.command_sink = command_reply_sink;
        bridge.log = [](const std::string& s) { emf::logf("%s", s.c_str()); };
        emf::core::bridge::start_debug_bridge(std::move(bridge));
    }
    emf::logf("sim step hooked; loading mods from emf\\mods");
    emf::core::LoadOptions load_opt;
    load_opt.settings = g_state->settings.get();
    load_opt.registry = &g_state->mods;
    std::vector<HMODULE> mapped;  // every mod DLL that stays loaded (also those that returned false or faulted)
    load_opt.kept_loaded = &mapped;
    g_mods = emf::core::load_mods(dir + L"\\emf\\mods", emf::core::api_table(),
                                  [](const std::string& s) { emf::logf("%s", s.c_str()); }, load_opt);
    emf::logf("%zu mod(s) loaded", g_mods.size());
    g_manifest_watch.poll(g_file_poll.mods_dir, g_state->mods.list(), *g_state->settings);  // notes the files as loaded
    // While the in-game console is open, mods' GetAsyncKeyState/GetKeyState calls see no keys (no hotkeys while typing).
    std::vector<std::pair<uintptr_t, uintptr_t>> ranges;
    // Every mod DLL that stays loaded counts: one whose emf_mod_load returned false or faulted may still have callbacks.
    for (HMODULE module : mapped) {
        const auto base = reinterpret_cast<uintptr_t>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        ranges.emplace_back(base, base + nt->OptionalHeader.SizeOfImage);
    }
    emf::core::set_mod_ranges(std::move(ranges));
}
