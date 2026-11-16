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
#include "common/world_offsets.h"
#include "common/world_view.h"
#include "fault_report.h"
#include "builtin_commands.h"
#include "profile_commands.h"
#include "hud_commands.h"
#include "hud_editor/editor.h"
#include "hud_editor/bars.h"
#include "hud_editor/gate.h"
#include "hud_editor/hook.h"
#include "emf/aim.hpp"
#include "emf/keys.hpp"
#include "save_commands.h"
#include "save_slots.h"
#include "saves/backup.h"
#include "saves/save_info.h"
#include "chat_commands.h"
#include "emf_settings.h"
#include "server_chat.h"
#include "console.h"
#include "content_status.h"
#include "buff_service.h"
#include "ground_glue.h"
#include "game_screen.h"
#include "marker_capture.h"
#include "minimap_draw.h"
#include "inventory.h"
#include "doors.h"
#include "enemy_orders.h"
#include "move_orders.h"
#include "villager_moves.h"
#include "path_glue.h"
#include "block_commands.h"
#include "block_glue.h"
#include "teleport.h"
#include "component_service.h"
#include "offers.h"
#include "net/net_service.h"
#include "spawn.h"
#include "world_settings_glue.h"
#include "world_settings_service.h"
#include "world_commands.h"
#include "emf/storage.hpp"
#include "known_tools.h"
#include "overlay/input_hook.h"
#include "overlay/profiles_ui.h"
#include "overlay/overlay.h"
#include "overlay/vk_overlay.h"
#include "overlay/window_model.h"
#include "overlay/submit.h"
#include "file_poll.h"
#include "mods.h"
#include "game_text.h"
#include "item_text.h"
#include "screen_glyphs.h"
#include "menu_start.h"
#include "ready_gate.h"
#include "version.h"
#include "system_programs.h"
#include "world_jobs.h"
#include "mods_check_service.h"
#include "mods_check_content.h"
#include "mods_policy.h"
#include "mods_refuse.h"
#include "game_kick.h"

// The Vulkan layer entry point lives in emf_core_lib (a static library): keep it in emf_core.dll's exports.
#pragma comment(linker, "/include:emf_vk_negotiate")
// the overlay's hook path (Wine / Proton, [overlay] path=hook): the loader asks this export for EMF's vkGetInstanceProcAddr
#pragma comment(linker, "/include:emf_vk_shim_proc")

namespace {
emf::core::ModsCheck* g_mods_check = nullptr;  // the mod check on join (core channel emf/mods)
emf::core::ModsRefuse* g_mods_refuse = nullptr;  // [net] mods_check=refuse (null: warn only)
std::vector<std::string> g_mods_required;          // [net] mods_required, checked against this server's mods at start-up
emf::core::HandleAccountActionFn g_kick_fn = nullptr;  // the game's handleAccountAction (refuse only)

// players.mods: each player in the server world, with their EMF and what differs from this server's mods.
void cmd_players_mods(EmfWorld* world, int, const char* const*, void*) {
    if (!world || emf::WorldView(reinterpret_cast<uint8_t*>(world)).kind() != EMF_WORLD_SERVER) {
        emf::core::command_reply_line("players.mods: only where the world runs (a game you host or the dedicated server)");
        return;
    }
    const auto players = emf::core::read_players(emf::core::api_table(), world);
    if (players.empty()) {
        emf::core::command_reply_line("players.mods: no player in the world");
        return;
    }
    for (const auto& p : players) {
        std::string who = (p.name.empty() ? "player " + std::to_string(p.entity) : p.name) + " (entity " +
                          std::to_string(p.entity) + ")";
        if (p.host) {
            emf::core::command_reply_line(who + ": the host (this game)");
            continue;
        }
        const auto seen = g_mods_check && p.account ? g_mods_check->current(p.account, p.entity) : std::nullopt;
        const std::string refused = g_mods_refuse && p.account ? g_mods_refuse->state_of(p.account) : std::string();
        const std::string tail = refused.empty() ? std::string() : "; " + refused;
        if (seen)
            emf::core::command_reply_line(who + ": EMF " + (seen->emf.empty() ? std::string("?") : seen->emf) + ", " +
                                          seen->diff + tail);
        else
            emf::core::command_reply_line(who + ": no EMF, or its mod list has not come" +
                                          std::string(g_mods_check && g_mods_check->server_has_content()
                                                          ? " (content mods cannot be checked)"
                                                          : "") +
                                          tail);
    }
}
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
// EML content at this start (content_status.h): set once in emf_core_start; its notice is shown when a world loads
emf::core::ContentLines g_content_lines;
std::optional<emf::loader::ContentStatus> g_content_status;  // read once (reading clears it): the mod check uses it too
std::atomic<bool> g_content_notice_said{false};
constexpr auto kModsReadyTimeout = std::chrono::seconds(30);
emf::core::BuiltinContext g_builtin;                     // the core's own commands
emf::core::ProfileCommandContext g_profiles;             // "profile": mod profiles
emf::core::SaveCommandContext g_save_commands;           // "saves.backup", "saves.list"
std::unique_ptr<emf::core::hud::HudEditor> g_hud_editor;  // the HUD editor (game client only)
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

// ---- API v27 mod messages: what the chat transport asks of the server world ----
// A player entity's entry in the server world's BasicPlayerInfos (their connection handle and account), or nothing.
std::optional<emf::core::PlayerInfo> server_player_info(uint32_t entity) {
    const EmfApi* api = emf::core::api_table();
    EmfWorld* w = api->server_world();
    const auto* infos = static_cast<const uint8_t*>(w ? api->singleton(w, "BasicPlayerInfos") : nullptr);
    if (!infos || entity == 0 || entity > emf::core::kPlayerInfoCount) return std::nullopt;
    uint8_t entry[emf::core::kPlayerInfoSize];
    if (!emf::seh_read(infos + static_cast<size_t>(entity - 1) * emf::core::kPlayerInfoSize, entry, sizeof entry))
        return std::nullopt;
    return emf::core::read_player_info(entry, entity);
}

// A player entity's account (its BasicPlayerInfos entry's), or 0.
uint64_t net_account_of(uint32_t entity) {
    const auto info = server_player_info(entity);
    return info ? info->account : 0;
}

// This game's own account as the server stamps it on a line it speaks as this player: the local player's entry in the
// client world's BasicPlayerInfos. Nothing until both exist; the transport answers no marked line before that.
std::optional<uint64_t> net_self_account() {
    const EmfApi* api = emf::core::api_table();
    EmfWorld* w = api->client_world();
    const uint32_t local = api->local_player();
    const auto* infos = static_cast<const uint8_t*>(w ? api->singleton(w, "BasicPlayerInfos") : nullptr);
    if (!infos || local == 0 || local > emf::core::kPlayerInfoCount) return std::nullopt;
    uint8_t entry[emf::core::kPlayerInfoSize];
    if (!emf::seh_read(infos + static_cast<size_t>(local - 1) * emf::core::kPlayerInfoSize, entry, sizeof entry))
        return std::nullopt;
    const auto info = emf::core::read_player_info(entry, local);
    if (!info || info->account == 0) return std::nullopt;
    static std::atomic<uint64_t> told{0};
    if (told.exchange(info->account) != info->account)
        emf::logf("mod messages: this game's own account (the client world's player %u): 0x%016llx", local,
                  static_cast<unsigned long long>(info->account));
    return info->account;
}

// A player's connection (handle, account) from the server's own records, or nothing.
std::optional<std::pair<uint32_t, uint64_t>> net_connection_of(uint32_t entity) {
    const uint64_t account = net_account_of(entity);
    uint8_t* const server = emf::core::chat_server();
    if (!account || !server) return std::nullopt;
    std::vector<uint8_t> records(
        static_cast<size_t>(emf::core::server_layout::kConnectionSize) * emf::core::server_layout::kConnectionCount, 0);
    if (!emf::seh_read(server + emf::core::server_layout::kConnections, records.data(), records.size()))
        return std::nullopt;
    const auto c = emf::core::connection_of(records.data(), account);
    if (!c) return std::nullopt;
    return std::make_pair(c->handle, c->account);
}

// True for the connection of this process's own player (a game it hosts): its messages go in process, not by chat.
bool net_is_local(uint32_t, uint64_t account) {
    const EmfApi* api = emf::core::api_table();
    if (g_state->exe.server || !api->server_world()) return false;  // a joined game or the dedicated server hosts no player
    const uint32_t local = api->local_player();
    return local != 0 && account != 0 && net_account_of(local) == account;
}

emf::core::ChatPolicy g_net_policy;  // [chat] admins / everyone: who is an admin for a message's sender

// The game counts this connected player as an admin (CanKickBan in the record of their connection `handle`, 0 when not
// known: a dedicated server's Admin group, or a host's admins). Any thread (a guarded copy of the server's records);
// false when not connected, or not known (a quick reconnect's two records disagree).
bool game_admin_account(uint32_t handle, uint64_t account) {
    uint8_t* const server = emf::core::chat_server();
    std::vector<uint8_t> records(
        static_cast<size_t>(emf::core::server_layout::kConnectionSize) * emf::core::server_layout::kConnectionCount, 0);
    if (!server || !emf::seh_read(server + emf::core::server_layout::kConnections, records.data(), records.size()))
        return false;
    return emf::core::connection_is_admin(records.data(), handle, account).value_or(false);
}

// Switches the chat transport on (the chat hooks are in place): the server end everywhere (a game that hosts, the
// dedicated server), the client end on a game exe, with the client's outbox flush and chatClientSend when the build has
// them. Mod messages to and from a joined game or the server need that; in process they never did.
void start_net_transport(bool known, const std::wstring& ini) {
    (void)ini;
    auto* net = g_state->net;
    emf::core::net::ServerEnv senv;
    senv.send = [](uint32_t handle, uint64_t account, const std::string& line) {
        return emf::core::queue_net_line(handle, account, line);
    };
    senv.send_lines = [](uint32_t handle, uint64_t account, const std::vector<std::string>& lines) {
        return emf::core::queue_net_lines(handle, account, lines);
    };
    senv.room = [] { return emf::core::chat_outbox_room(); };
    senv.is_local = net_is_local;
    senv.emf_version = EMF_VERSION;
    senv.api = EMF_API_VERSION;
    senv.build = g_state->exe.build_number;
    senv.greet = GetPrivateProfileIntW(L"net", L"greet", 1, ini.c_str()) != 0;
    net->enable_server(std::move(senv));
    g_state->net_connection_of = net_connection_of;
    g_state->net_is_admin = [](uint32_t handle, uint64_t account) { return g_net_policy.allows(account, handle); };
    g_state->net_entity_of = chat_sender_entity;
    if (g_state->exe.server) {
        emf::logf("mod messages: on the server end ([net] greet=%d)", senv.greet ? 1 : 0);
        return;
    }
    const auto& send = g_state->sigs[static_cast<size_t>(emf::Sig::ChatClientSend)];
    const auto& flush = g_state->sigs[static_cast<size_t>(emf::Sig::ClientOutboxFlush)];
    if (!emf::is_usable(send, known) || !emf::is_usable(flush, known)) {
        emf::logf("mod messages: this game cannot send them to a server (the client chat functions were not found on this "
                  "game build); in process and as a server they work");
        return;
    }
    emf::core::net::ClientEnv cenv;
    cenv.self_account = net_self_account;
    cenv.emf_version = EMF_VERSION;
    cenv.api = EMF_API_VERSION;
    cenv.build = g_state->exe.build_number;
    net->enable_client(std::move(cenv));
    emf::core::ClientChatDeps cd;
    cd.line = [](std::string_view text, uint64_t speaker) { return g_state->net->client_line(text, speaker); };
    cd.take = [](size_t max) { return g_state->net->client_take(max); };
    cd.put_back = [](std::vector<std::string> lines) { g_state->net->client_put_back(std::move(lines)); };
    cd.log = [](const std::string& t) { emf::logf("%s", t.c_str()); };
    cd.reset = [] { g_state->net->client_reset(); };  // a new session's GameClient: the old server is forgotten
    const bool ok = emf::core::install_client_chat(flush.addr, send.addr, std::move(cd));
    emf::logf("mod messages: %s", ok ? "on (server and client ends)" : "server end only (the client's chat hook failed)");
}

// chat.players (admin): the players of the server world with their account hashes (what [chat] admins takes), the
// connection handle their BasicPlayerInfos entry holds and whether the server's connection record for it matches (their
// chat reader), and their name. (No raw dump: connection records hold players' network data, and logs go into bug
// reports.)
// ui.windows / ui.click / ui.close: a mod's open window (API v24) as text, a click on one of its widgets and closing it
// as the player would; the debug bridge cannot click ImGui (the game keeps the mouse), so tests go through these.
void cmd_ui_windows(EmfWorld*, int, const char* const*, void*) {
    const EmfApi* api = emf::core::api_table();
    const auto w = emf::core::overlay_window();
    if (!w) {
        api->command_reply("no mod window is open");
        return;
    }
    for (const auto& line : emf::core::describe_window(*w)) api->command_reply("%s", line.c_str());
}

void cmd_ui_click(EmfWorld*, int argc, const char* const* argv, void*) {
    const EmfApi* api = emf::core::api_table();
    const bool right = argc == 3 && std::strcmp(argv[2], "right") == 0;
    if (argc < 2 || argc > 3 || (argc == 3 && !right)) {
        api->command_reply("usage: ui.click <key> [right]");
        return;
    }
    std::string why;
    if (emf::core::overlay_click_window(argv[1], right, &why))
        api->command_reply("%s %s", right ? "right-clicked" : "clicked", argv[1]);
    else
        api->command_reply("ui.click: %s", why.c_str());
}

void cmd_ui_close(EmfWorld*, int, const char* const*, void*) {
    const EmfApi* api = emf::core::api_table();
    api->command_reply(emf::core::overlay_close_window_by_player() ? "closed the mod window"
                                                                    : "no mod window is open");
}

void add_window_commands() {
    struct Cmd {
        const char* name;
        const char* help;
        emf::core::CommandFn fn;
    };
    const Cmd cmds[] = {
        {"ui.windows", ": the open mod window as text (its widgets with their keys)", cmd_ui_windows},
        {"ui.click", "<key> [right]: click a widget of the open mod window, as the mouse would", cmd_ui_click},
        {"ui.close", ": close the open mod window, as Esc would", cmd_ui_close},
    };
    for (const auto& c : cmds)
        if (!g_state->commands.add(c.name, c.help, c.fn, nullptr, nullptr))
            emf::logf("commands: \"%s\" could not be added", c.name);
}

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
        char line[320];
        std::snprintf(line, sizeof line, "player %u: account %s, handle 0x%x, %s, name %s, %s", id,
                      account_text(info->account).c_str(), info->handle, conn.c_str(),
                      name ? printable(*name).c_str() : "(not known)", game_role_text(e[kPlayerInfoPermissions]).c_str());
        api->command_reply("%s", line);
    }
    if (!count) api->command_reply("no players in this world's player list");
}

// A step over the limit (a hitch the player may feel): logged at most once a minute per name, so a player's own
// session leaves a trace in emf_core.log (`perf` in the console has the numbers).
void note_slow(const std::string& who, double ms, double limit) {
    if (ms <= limit) return;
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::vector<std::pair<std::string, uint64_t>> last;
    const uint64_t now = GetTickCount64();
    bool say = false;
    AcquireSRWLockExclusive(&lock);
    auto it = std::find_if(last.begin(), last.end(), [&](const auto& l) { return l.first == who; });
    if (it == last.end()) {
        last.emplace_back(who, now);
        say = true;
    } else if (now - it->second >= 60000) {
        it->second = now;
        say = true;
    }
    ReleaseSRWLockExclusive(&lock);
    if (say)
        emf::logf("slow: %s took %.0f ms in one world step (said at most once a minute; perf in the console has "
                  "the numbers)",
                  who.c_str(), ms);
}

void hk_sim_step(void* storage, char ticked) {
    const double game_start = emf::perf_clock_ms();
    o_sim_step(storage, ticked);
    const double game_ms = emf::perf_clock_ms() - game_start;
    try {
        emf::WorldView view = emf::WorldView::from_storage(storage);
        if (view.state() != 2) {  // world not running yet
            emf::core::bridge::note_world_setup(GetTickCount64());  // the debug bridge's "loading"
            return;
        }
        const uint8_t kind = view.kind();
        if (kind > 2) return;
        auto* world = reinterpret_cast<EmfWorld*>(view.raw());
        static const std::string kGameStep[3] = {"the game's server world step", "the game's prediction world step",
                                                 "the game's client world step"};
        g_state->perf.add(kGameStep[kind], game_ms);
        note_slow(kGameStep[kind], game_ms, 100.0);
        // The server world's SimStep has returned with its grid update and range queries still running on the game's
        // workers; the game waits for them later in this frame. Wait now, so mods' on_tick and commands (spawn,
        // destroy, give) never change the grid under them. Other worlds: nothing to wait for.
        const double wait_start = emf::perf_clock_ms();
        if (g_world_jobs.before_tick_work(world, kind)) {
            const double wait_ms = emf::perf_clock_ms() - wait_start;
            static const std::string kWait = "EMF: waiting for the server world's jobs";
            g_state->perf.add(kWait, wait_ms);
            note_slow(kWait, wait_ms, emf::core::Dispatcher::kSlowCallMs);
        }
        g_state->tracker.on_tick(world, kind, GetTickCount64());
        g_state->dispatcher.run(world, kind, [](const std::string& e) { emf::logf("dispatch: %s", e.c_str()); });
        emf::core::pump_setting_changes(world);  // v13 on_setting_changed
        // the characters of the item names for EMF's screens, once per text table (two reads and a compare otherwise;
        // never on the dedicated server, which has no game text)
        if (emf::core::game_text().table_key() != emf::core::screen_glyphs().collected_key()) {
            const auto* registry = static_cast<const uint8_t*>(view.singleton("g38_items::ItemRegistry"));
            const uint64_t before = emf::core::screen_glyphs().wanted_version();
            emf::core::pump_screen_glyphs(registry, emf::core::item_names(), emf::core::game_text(),
                                          emf::core::screen_glyphs());
            const emf::core::GlyphWant w = emf::core::screen_glyphs().wanted();
            if (w.version != before)
                emf::logf("item names: the %s names use %zu characters beyond EMF's base fonts",
                          emf::core::language_code(w.language), w.glyphs.size());
        }
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
std::unique_ptr<emf::core::BuffService> g_buffs;  // v32 buffs: emf_buffs
// teleport: the emf_teleport program and its requests (null when off on this game build).
std::unique_ptr<emf::core::TeleportService> g_teleport;
// v38 doors: the emf_doors program and its orders and locks (null when off on this game build).
std::unique_ptr<emf::core::DoorService> g_doors;
// The door locks kept per world: <game>\emf\config\emf_doors\<slot>.txt.
emf::WorldFile g_door_file{"emf_doors"};
// A 32.32 fixed-point position (CurrentTransform's first 24 bytes) in metres; false when it cannot be read.
bool read_place(EmfWorld* w, uint32_t id, std::array<double, 3>& out) {
    uint64_t size = 0;
    const void* t = emf::core::api_table()->get_component(w, emf::gen::ecs::CurrentTransform::kHash, id, &size);
    int64_t raw[3];
    if (!t || size < sizeof raw || !emf::seh_read(t, raw, sizeof raw)) return false;
    for (int i = 0; i < 3; ++i) out[i] = static_cast<double>(raw[i]) / 4294967296.0;
    return true;
}
// Every door in the server world (ServerToggleState and DirectionalInteraction) with its place, in one pass over the
// entities (doors are rare: ServerToggleState is asked first).
std::vector<emf::core::DoorSpot> list_doors(uint8_t* world) {
    struct Acc {
        EmfWorld* w;
        std::vector<emf::core::DoorSpot> out;
    } acc{reinterpret_cast<EmfWorld*>(world), {}};
    emf::core::api_table()->for_each_entity(acc.w, [](uint32_t id, void* user) {
        auto& acc = *static_cast<Acc*>(user);
        const EmfApi* a = emf::core::api_table();
        if (!a->get_component(acc.w, emf::gen::ecs::ServerToggleState::kHash, id, nullptr) ||
            !a->get_component(acc.w, emf::gen::ecs::DirectionalInteraction::kHash, id, nullptr))
            return;
        emf::core::DoorSpot d;
        d.entity = id;
        if (read_place(acc.w, id, d.pos)) acc.out.push_back(d);
    }, &acc);
    return acc.out;
}
// Where the players in the server world stand.
std::vector<std::array<double, 3>> player_places(uint8_t* world) {
    std::vector<std::array<double, 3>> out;
    EmfWorld* w = reinterpret_cast<EmfWorld*>(world);
    const EmfApi* a = emf::core::api_table();
    for (uint32_t id = 1; id <= 1023; ++id) {  // player entities have the low ids (read_players)
        if (!a->get_component(w, emf::gen::ecs::PlayerState::kHash, id, nullptr)) continue;
        std::array<double, 3> p{};
        if (read_place(w, id, p)) out.push_back(p);
    }
    return out;
}
// v39 enemy orders: written after every server step.
std::unique_ptr<emf::core::EnemyOrders> g_orders;
// v40 move orders: walked after every server step; their guard orders go out from the emf_moves program at the next step.
std::unique_ptr<emf::core::MoveOrders> g_moves;
// v46 villager moves: walked after every server step; their GoToPosition commands go out from the emf_moves program.
std::unique_ptr<emf::core::VillagerMoves> g_villager_moves;
emf::core::NpcSendQueue g_npc_queue;
std::mutex g_guard_lock;
std::vector<std::pair<uint32_t, uint32_t>> g_guard_queue;  // (guarding, guarded)
constexpr const char* kMovesProgramName = "emf_moves";
constexpr size_t kGuardQueueMax = 1024;
void moves_program(void* ctx, void*) {
    std::vector<std::pair<uint32_t, uint32_t>> work;
    {
        std::lock_guard<std::mutex> l(g_guard_lock);
        work.swap(g_guard_queue);
    }
    size_t sent = 0;
    for (; sent < work.size(); ++sent) {
        auto* ev = static_cast<emf::gen::ecs::StartGuardingEvent*>(emf::core::api_table()->emit_event(
            ctx, "StartGuardingEvent", emf::gen::ecs::StartGuardingEvent::kSize));
        if (!ev) break;  // logged once by emit_event; the rest waits for the next step
        ev->guardingEntityId.id = work[sent].first;
        ev->guardedEntityId.id = work[sent].second;
    }
    if (sent < work.size()) {
        std::lock_guard<std::mutex> l(g_guard_lock);
        g_guard_queue.insert(g_guard_queue.begin(), work.begin() + static_cast<std::ptrdiff_t>(sent), work.end());
        if (g_guard_queue.size() > kGuardQueueMax) g_guard_queue.resize(kGuardQueueMax);
    }
    // v46: villager moves' GoToPosition (the game's own command; one per villager, the latest point)
    std::vector<emf::core::NpcSendQueue::Send> sends = g_npc_queue.take();
    size_t done = 0;
    for (; done < sends.size(); ++done) {
        namespace gen = emf::gen::ecs;
        auto* ev = static_cast<gen::NpcCommandEvent*>(
            emf::core::api_table()->emit_event(ctx, "NpcCommandEvent", gen::NpcCommandEvent::kSize));
        if (!ev) break;  // logged once by emit_event; the rest waits for the next step
        emf::core::fill_npc_command(ev, sends[done]);
    }
    if (done < sends.size())
        g_npc_queue.put_back(std::vector<emf::core::NpcSendQueue::Send>(sends.begin() + static_cast<std::ptrdiff_t>(done), sends.end()));
}
// v23 change_component: the emf_components program and its requests (null when off on this game build).
std::unique_ptr<emf::core::ComponentService> g_components;
// v25 E prompts: on top of the component service (null when that is off).
std::unique_ptr<emf::core::OfferService> g_offers;

// offers.list: the offers of the running session (entity, mod, verb, accepts), for tests and mod authors.
// hits: the v31 hit hooks' state and cost
void cmd_hits(EmfWorld*, int, const char* const*, void*) {
    const auto& st = g_state->hit_stats;
    const uint64_t seen = st.seen.load();
    char avg[32] = "-";
    if (seen) std::snprintf(avg, sizeof avg, "%.1f", static_cast<double>(st.ns.load()) / 1000.0 / static_cast<double>(seen));
    const std::string before = (emf::core::hit_hook_on() ? std::string("on") : "off (" + g_state->hit_hook_off_why + ")") +
                               ", fall/shroud/cold/drowning " +
                               (g_state->env_off_why.empty() ? std::string("on") : "off (" + g_state->env_off_why + ")");
    char line[512];
    std::snprintf(line, sizeof line,
                  "hits: before the hit %s, %zu rules, %zu before and %zu after callbacks; %llu seen, %llu changed, "
                  "%llu cancelled, %llu faults, %s us each",
                  before.c_str(), g_state->hit_rules.size(), g_state->hit_callbacks.before_count(),
                  g_state->hit_callbacks.after_count(), static_cast<unsigned long long>(seen),
                  static_cast<unsigned long long>(st.changed.load()), static_cast<unsigned long long>(st.cancelled.load()),
                  static_cast<unsigned long long>(st.faults.load()), avg);
    emf::core::command_reply_line(line);
}

void cmd_offers_list(EmfWorld*, int, const char* const*, void*) {
    const EmfApi* api = emf::core::api_table();
    if (!g_offers) {
        api->command_reply("offers are off (see the log)");
        return;
    }
    const uint32_t session = g_state->server_session.load();
    size_t n = 0;
    for (const auto& e : g_offers->list()) {
        if (e.session != session) continue;
        ++n;
        char verb[96];
        if (e.spec.text.empty()) std::snprintf(verb, sizeof verb, "0x%08X", emf::core::offer_verb(e.spec));
        else std::snprintf(verb, sizeof verb, "\"%s\"", e.spec.text.c_str());
        api->command_reply("%u: %s, verb %s, %u accept(s)", e.entity, e.owner.c_str(), verb, e.accepts);
    }
    api->command_reply("%zu offer(s)", n);
}
// v21 world settings: the change queue, the server step's write and resync, the per-world kept settings.
struct WorldSettingsRuntime {
    emf::core::ws::State state;
    std::unique_ptr<emf::core::ws::Glue> glue;
    std::unique_ptr<emf::core::ws::Service> service;
    emf::WorldFile file{"world_settings"};
};
std::unique_ptr<WorldSettingsRuntime> g_ws;
// The hosted world's ecs::GameSettings, or null (no server world here: a joined client, the main menu).
uint8_t* server_game_settings() {
    EmfWorld* s = g_state ? g_state->tracker.latest(EMF_WORLD_SERVER, GetTickCount64()) : nullptr;
    return s ? static_cast<uint8_t*>(emf::WorldView(reinterpret_cast<uint8_t*>(s)).singleton("ecs::GameSettings")) : nullptr;
}

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
        if (g_state->players) g_state->players->world_loaded();  // v28: everyone joins 1 s later
        if (auto* info = emf::core::save_info_recorder()) info->on_world_loaded(slot);  // save-info.json
        // EML content could not be applied at this start: said once on screen, when the first world is there
        if (!g_content_lines.notice.empty() && !g_content_notice_said.exchange(true) && g_state->notify)
            g_state->notify(g_content_lines.notice);
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
            if (g_buffs) g_buffs->world_reset();          // buff requests for the old world are refused
            emf::core::buffs_new_session();               // every player's buffs are a first sight again
            if (g_teleport) g_teleport->world_reset();
            emf::core::path_world_reset();                // path queries of the old world end (cancelled)
            emf::core::blocks_world_reset();              // breaks waiting for the old world end (cancelled)
            if (g_doors) g_doors->world_reset();          // door orders and locks of the old world go
            if (g_orders) g_orders->world_reset();        // enemy orders of the old world end
            emf::core::builtin_world_reset(g_builtin);   // god's hit rules of the old world go
            if (g_moves) g_moves->world_reset();  // move orders of the old world end (cancelled, no world)
            if (g_villager_moves) g_villager_moves->world_reset();  // villager moves too (holds quietly)
            g_npc_queue.clear();
            {
                std::lock_guard<std::mutex> l(g_guard_lock);
                g_guard_queue.clear();
            }
            if (g_components) g_components->world_reset();  // component changes under way end (FAILED)    // teleports under way end (FAILED)
            g_state->damage_queue.clear();                // hits for the old session never land
            emf::core::env_new_session();                 // nor its env hits and carries
            emf::core::markers_world_setup(EMF_WORLD_SERVER);  // nothing of the old world is followed or replayed
            emf::core::map_world_setup(EMF_WORLD_SERVER);      // ground searches for the old world end (not ok)
            ++g_state->server_session;                    // mods notice the new session (API v9)
        }
        // a new client world (a new session): the server the last one talked to is forgotten (its welcome, its lines);
        // a beacon held while the game did not know its own account stays (the same connection, still loading: a new
        // connection's GameClient drops it)
        if (kind == EMF_WORLD_CLIENT) {
            ++g_state->client_session;
            if (g_state->net) g_state->net->client_reset(/*keep_held_beacon=*/true);
            emf::core::markers_world_setup(EMF_WORLD_CLIENT);  // the last server's markers go
            emf::core::map_world_setup(EMF_WORLD_CLIENT);      // and its map actions
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
// `label`: a backup EML content asks for when it could not be built (mod items vanish when a world loads without their
// mod): min_minutes does not hold that one back.
void start_auto_backup(const std::wstring& dir, const char* label = nullptr) {
    emf::saves::AutoSettings settings = emf::saves::read_auto_settings(dir + L"\\emf\\emf_core.ini");
    if (label) settings.min_minutes = 0;
    const std::string why = label ? label : "";
    std::thread([settings, why] {
        try {
            const ULONGLONG t0 = GetTickCount64();
            const auto roots = emf::saves::with_app_data(emf::saves::default_roots(false), emf::saves::default_app_data());
            emf::saves::BackupOptions o;
            o.made_by = "game";
            o.label = why;
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

// Which tool owns each proxy DLL next to the game, what the loader chain-loaded (emf\chain_status.txt), what ModLoader
// Proxy starts (chainloader.ini), and a loader whose file another tool took (emf\known_tools.txt names them). Reading
// the DLLs takes a moment: it runs on its own thread, so EMF's start (and a held server) does not wait for it.
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
    // ModLoader Proxy (a proxy DLL, or started by EMF) starts the DLLs in its chainloader.ini: EMF, ShroudForge...
    emf::core::FolderLook look;
    look.read_text = [&](const std::string& rel) { return read_head(dir + L"\\" + widen(rel), 1u << 20); };
    look.exists = [&](const std::string& rel) {  // a file: not a folder, not a device
        const DWORD a = GetFileAttributesW((dir + L"\\" + widen(rel)).c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE));
    };
    look.tool = [&](const std::string& rel) {
        return emf::core::tool_for_dll_text(tools, read_head(dir + L"\\" + widen(rel), kDllHead));
    };
    look.dlls_in = [&](const std::string& rel) {
        std::vector<std::string> names;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\" + widen(rel) + L"\\*.dll").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return names;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const int n = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nullptr, 0, nullptr, nullptr);
            std::string name(static_cast<size_t>(n > 1 ? n - 1 : 0), '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name.data(), n, nullptr, nullptr);
            names.push_back(name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return names;
    };
    emf::core::add_loader_started(view, look);
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
    // [debug] local_only=1 asked for, but not in place: say so, a test that relies on it must not go on unguarded
    const bool local_only_wanted =
        GetPrivateProfileIntW(L"debug", L"local_only", 0, (dir + L"\\emf\\emf_core.ini").c_str()) == 1;
    const auto local_only_not_on = [local_only_wanted](const char* why) {
        if (local_only_wanted) emf::logf("local_only: NOT on (%s): session starts are not guarded", why);
    };
    if (debug_bridge) emf::log_ring_enable(5000);
    emf::logf("%s starting (game dir %s)", kVersion, utf8(dir).c_str());
    {
        // EML content at this start, as the loader set it before it started the core (content_status.h)
        g_content_status = emf::core::read_content_status();
        g_content_lines = emf::core::content_lines(g_content_status,
                                                   emf::saves::read_auto_settings(dir + L"\\emf\\emf_core.ini").on);
        if (!g_content_lines.console.empty()) emf::logf("%s", g_content_lines.console.c_str());
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const std::wstring exe_name = std::wstring(self).substr(std::wstring(self).find_last_of(L"\\/") + 1);
        if (_wcsicmp(exe_name.c_str(), L"enshrouded_server.exe") != 0)  // first: before any save
            start_auto_backup(dir, g_content_lines.backup ? emf::core::kContentBackupLabel : nullptr);
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
    // EMF's own signatures too: a DLL mod in the chain that hooked one of these functions before the core started
    // (put_items' inventory transaction, say) no longer switches EMF's use of it off. The addresses are the live ones.
    const uintptr_t live_base = g_state->exe.base;
    const uintptr_t scan_base = g_state->exe.pristine_base ? g_state->exe.pristine_base : live_base;
    const auto scan_sigs = emf::resolve_all(reinterpret_cast<const void*>(scan_base), build);
    g_state->sigs = emf::rebased(scan_sigs, scan_base, live_base);
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
    g_state->perf_since_ms = GetTickCount64();
    g_state->dispatcher.set_perf(&g_state->perf, [](const std::string& module, double ms) {
        note_slow(module, ms, emf::core::Dispatcher::kSlowCallMs);
    });
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
        local_only_not_on("EMF stays idle");
        return;
    }
    // Struct offsets read from instructions in the located functions (common/code_offsets.h), before any hook: the
    // world's own offsets first (build 1283531 moved the world's block by +0x1C00; common/world_offsets.h).
    const auto code_offsets = emf::resolve_code_offsets(reinterpret_cast<const void*>(scan_base), scan_sigs, build);
    for (const auto& note : emf::unconfirmed_offset_notes(code_offsets)) emf::logf("%s", note.c_str());
    const emf::WorldOffsetPlan world_plan = emf::plan_world_offsets(code_offsets, known);
    emf::logf("%s", world_plan.line.c_str());
    if (!world_plan.ok) {
        emf::logf("the world's offsets are not known on this game build: EMF stays idle, no mods loaded");
        local_only_not_on("EMF stays idle");
        return;
    }
    emf::set_world_offsets(world_plan.offsets);
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
        menu.client_ready = [] {
            return g_state->tracker.latest(EMF_WORLD_CLIENT, GetTickCount64()) != nullptr &&
                   emf::core::api_table()->local_player() != 0;
        };
        menu.any_world_running = [] {
            const uint64_t now = GetTickCount64();
            return g_state->tracker.latest(EMF_WORLD_SERVER, now) || g_state->tracker.latest(EMF_WORLD_CLIENT, now);
        };
        menu.now_ms = [] { return GetTickCount64(); };
        menu.log = [](const std::string& s) { emf::logf("%s", s.c_str()); };
        if (emf::core::install_menu_start(g_state->sigs, known, build, std::move(menu), &g_load_world_off)) {
            emf::logf("load_world: on (main-menu hooks installed; the guard acts only during a load_world); join: %s",
                      emf::core::menu_start()->can_join() ? "on" : "off (PostJoinPending not usable on this build)");
            // [debug] local_only=1: tests that join a server at boot (+connect) post no load_world, so the guard is on for
            // every start of this process; [debug] local_only_name: the LOCAL character must have this name too
            const std::wstring ini = dir + L"\\emf\\emf_core.ini";
            if (GetPrivateProfileIntW(L"debug", L"local_only", 0, ini.c_str()) == 1) {
                wchar_t name[256] = {};
                GetPrivateProfileStringW(L"debug", L"local_only_name", L"", name, 256, ini.c_str());
                const std::string expect = utf8(name);
                emf::core::menu_start()->set_local_only(expect);
                const std::string other = expect.empty() ? "" : ", or another character than '" + expect + "'";
                emf::logf("local_only: on (every session start with a Steam character or world%s is refused)",
                          other.c_str());
            }
        } else {
            emf::logf("load_world: off (%s)", g_load_world_off.c_str());
            local_only_not_on("the main-menu hooks are off on this build");
        }
    } else if (!g_state->exe.server) {
        local_only_not_on("it needs [debug] bridge=1");
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
        // the game's item names in the player's language: its text pool, read through the gameApp the pregame hook (just
        // installed with save info) notes; also without the debug bridge
        std::string text_why;
        if (emf::core::install_game_text(g_state->sigs, known, build, &text_why))
            emf::logf("item names: on (the game's own text, in the player's language)");
        else
            emf::logf("item names: off (%s); debug names are used", text_why.c_str());
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
    if (emf::is_usable(reg, known) && world_plan.program_table &&
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
            for (const auto s : {emf::Sig::AttrFind, emf::Sig::AttrAdd, emf::Sig::AllocPendingHit, emf::Sig::ApplyHitToEntity})
                g_state->sigs[static_cast<size_t>(s)].count = 0;
            emf::logf("attributes: off: attribute_get, attribute_add and damage rely on layouts EMF checked on build 1273043 "
                      "(game and server) only; this build is %s", known ? "one whose exe was not at hand" : "unknown");
        }
        g_state->attr_find = reinterpret_cast<emf::core::AttrFindFn>(sig(emf::Sig::AttrFind));
        g_state->attr_add = reinterpret_cast<emf::core::AttrAddFn>(sig(emf::Sig::AttrAdd));
        g_state->alloc_pending_hit = reinterpret_cast<emf::core::AllocPendingHitFn>(sig(emf::Sig::AllocPendingHit));
        // v22 levels: the game's XP curve and level cap (pure functions on the balancing table)
        g_state->xp_required = reinterpret_cast<uint32_t (*)(uint32_t, const void*)>(sig(emf::Sig::XpRequired));
        g_state->level_cap = reinterpret_cast<uint32_t (*)(const void*)>(sig(emf::Sig::MaxPlayerLevel));
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
        // v31 hits before they land: a detour on applyHitToEntity (rules and on_hit_before), only where the HitCandidate
        // layout and attribute reads were checked (the same builds as damage)
        g_state->hit_callbacks.set_log([](const std::string& line) { emf::logf("%s", line.c_str()); });
        const uintptr_t apply_hit = sig(emf::Sig::ApplyHitToEntity);
        if (!apply_hit)
            g_state->hit_hook_off_why = "the ApplyHitToEntity signature is not usable on this game build";
        else if (!g_state->attr_find)
            g_state->hit_hook_off_why = "attribute reads are off on this game build";
        else if (!emf::core::install_hit_hook(apply_hit))
            g_state->hit_hook_off_why = "applyHitToEntity could not be hooked";
        else
            g_state->hit_hook_off_why.clear();
        if (g_state->hit_hook_off_why.empty())
            emf::logf("hits: before the hit on (applyHitToEntity hooked: rules and on_hit_before)");
        else
            emf::logf("hits: before the hit off (%s): rules and on_hit_before are refused", g_state->hit_hook_off_why.c_str());
        // v31 on_hit: the server world's HitEvents, asked for now (before the relay closes) so a mod or a Lua script can
        // ask for hits at any time
        if (emf::core::subscribe_hit_events())
            emf::logf("hits: after the hit on (HitEvent on the server world)");
        else
            emf::logf("hits: after the hit off (%s): on_hit is refused", g_state->hit_events_off_why.c_str());
        // fall, shroud, cold and drowning (feature 4b): pre/post hooks on the four programs; the program slice's
        // layout read from the code of the current-entity and iterator functions (the count follows the list)
        {
            const auto& cur = g_state->sigs[static_cast<size_t>(emf::Sig::CurrentEntityId)];
            const auto& nxt = g_state->sigs[static_cast<size_t>(emf::Sig::IterNext)];
            int32_t list_off = 0, count_off = 0;
            const bool layout = emf::is_usable(cur, known) && emf::is_usable(nxt, known) &&
                                emf::seh_read(reinterpret_cast<const uint8_t*>(cur.addr) + 0x1C, &list_off, 4) &&
                                emf::seh_read(reinterpret_cast<const uint8_t*>(nxt.addr) + 0x15, &count_off, 4) &&
                                list_off > 0 && list_off < 0x10000 && count_off == list_off + 8;
            if (!g_state->attr_find || !g_state->attr_add)
                g_state->env_off_why = "attribute reads and writes are off on this game build";
            else if (!g_state->programs)
                g_state->env_off_why = "program hooks are not available";
            else if (!layout)
                g_state->env_off_why = "the program slice's layout was not found in the game's code";
            else {
                g_state->env_list_off = static_cast<uint32_t>(list_off);
                g_state->env_count_off = static_cast<uint32_t>(count_off);
                static const std::pair<const char*, uint8_t> kEnvPrograms[] = {
                    {"fall_damage_infliction", EMF_HIT_KIND_FALL}, {"fog_resistance_update", EMF_HIT_KIND_SHROUD},
                    {"bodyheat_depletion", EMF_HIT_KIND_COLD}, {"oxygen_update", EMF_HIT_KIND_DROWNING}};
                for (const auto& [name, kind] : kEnvPrograms)
                    g_state->programs->request_hook(
                        name,
                        [](void* ctx, void* user) { emf::core::env_pre(ctx, static_cast<uint8_t>(reinterpret_cast<uintptr_t>(user))); },
                        [](void* ctx, void* user) { emf::core::env_post(ctx, static_cast<uint8_t>(reinterpret_cast<uintptr_t>(user))); },
                        reinterpret_cast<void*>(static_cast<uintptr_t>(kind)));
                g_state->env_off_why.clear();
            }
            if (g_state->env_off_why.empty())
                emf::logf("hits: fall, shroud, cold and drowning on (hooks on their four programs, slice at +0x%x)",
                          static_cast<unsigned>(list_off));
            else
                emf::logf("hits: fall, shroud, cold and drowning off (%s)", g_state->env_off_why.c_str());
            g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
                if (emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() == EMF_WORLD_SERVER) emf::core::deliver_env_hits(w);
            }, nullptr);
        }
        if (!g_state->commands.add("hits", ": the hit hooks' state, rules, callbacks and cost", cmd_hits, nullptr, nullptr))
            emf::logf("commands: \"hits\" could not be added");
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
    // v32 buffs: an internal server program calls the game's applyBuffCore and cleanses slots (private research
    // buffs_2026-11-11); listing and reading need no program.
    {
        using emf::Sig;
        auto sig = [&](Sig s) -> uintptr_t {
            const auto& r = g_state->sigs[static_cast<size_t>(s)];
            return emf::is_usable(r, known) ? r.addr : 0;
        };
        emf::core::BuffService::Deps deps;
        deps.apply = reinterpret_cast<emf::core::ApplyBuffCoreFn>(sig(Sig::ApplyBuffCore));  // optional: removing works
        deps.read_other = reinterpret_cast<emf::core::ReadOtherFn>(sig(Sig::ReadOther));
        deps.write_other = reinterpret_cast<emf::core::WriteOtherFn>(sig(Sig::WriteOther));
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
        const bool giving = deps.apply != nullptr;
        if (!deps.read_other || !deps.write_other || !sig(Sig::IterBegin)) {
            emf::logf("buffs: giving and removing off (the ReadOther, WriteOther or IterBegin signatures are not usable); "
                      "listing and reading work");
        } else if (g_state->system_programs.registration_closed()) {
            emf::logf("buffs: giving and removing off (mod programs are unavailable)");
        } else {
            g_buffs = std::make_unique<emf::core::BuffService>(std::move(deps));
            if (g_state->system_programs.add(g_buffs->spec(), true)) {
                g_state->buffs = g_buffs.get();
                emf::logf("buffs: on (program %s in the server world%s)", emf::core::kBuffsProgramName,
                          giving ? "" : "; giving off: the ApplyBuffCore signature is not usable, removing works");
            } else {
                emf::logf("buffs: giving and removing off (the %s program could not be added)", emf::core::kBuffsProgramName);
                g_buffs.reset();
            }
        }
        if (!emf::core::add_buff_commands(g_state->commands)) emf::logf("commands: the buffs.* commands could not be added");
    }
    // v44 breaking blocks: blocks and break aim with the camera of the player the command means
    {
        emf::core::BlockCommandDeps bd;
        bd.camera = [](EmfWorld* w, float o[3], float d[3]) -> bool {
            namespace ecs = emf::gen::ecs;
            const EmfApi* a = emf::core::api_table();
            const uint32_t player = emf::core::command_player(a->local_player());
            uint64_t size = 0;
            const auto* in = player ? static_cast<const ecs::PlayerInput*>(a->get_component(w, ecs::PlayerInput::kHash,
                                                                                               player, &size))
                                    : nullptr;
            if (!in || size < sizeof(ecs::PlayerInput)) return false;
            const auto& cam = in->fromClient.camera;
            const emf::aim::Vec3 dir = emf::aim::forward(
                emf::aim::Quat{cam.orientation.x, cam.orientation.y, cam.orientation.z, cam.orientation.w});
            const double len = emf::aim::length(dir);
            if (len < 1e-9) return false;
            const emf::aim::Vec3 pos = emf::aim::metres(cam.position);
            o[0] = static_cast<float>(pos.x), o[1] = static_cast<float>(pos.y), o[2] = static_cast<float>(pos.z);
            d[0] = static_cast<float>(dir.x / len), d[1] = static_cast<float>(dir.y / len), d[2] = static_cast<float>(dir.z / len);
            return true;
        };
        bd.find_wall = [](EmfWorld* w, const float f[3], const float t[3], EmfBlockInfo* out) {
            return emf::core::api_table()->find_wall(w, f, t, out);
        };
        bd.block_at = [](EmfWorld* w, float x, float y, float z, EmfBlockInfo* out) {
            return emf::core::api_table()->block_at(w, x, y, z, out);
        };
        bd.break_blocks = [](EmfWorld* w, EmfBreakBlocks* r, EmfBreakCallback cb, void* u) {
            return emf::core::api_table()->break_blocks(w, r, cb, u);
        };
        bd.origin = [](EmfWorld* w, float x, float y, float z) -> std::string {
            emf::core::BlockOrigin o;
            std::string why;
            if (!emf::core::blocks_origin(w, x, y, z, &o, &why)) return why.empty() ? "" : "nothing read there (" + why + ")";
            return emf::core::block_origin_line(o);
        };
        if (!emf::core::add_block_commands(g_state->commands, std::move(bd)))
            emf::logf("commands: blocks and break could not be added");
    }
    // v39 enemy orders: an enemy's pick and an aggro entry for its target, written after every server step; the damage
    // option is a hit rule (enemy_orders.h)
    {
        emf::core::EnemyOrders::Deps d;
        d.get = [](EmfWorld* w, uint32_t hash, uint32_t entity, uint64_t* size) -> void* {
            return emf::core::api_table()->get_component(w, hash, entity, size);
        };
        d.now_ms = [] { return GetTickCount64(); };
        d.add_rule = [](uint32_t attacker, uint32_t target, int32_t add) -> uint32_t {
            EmfHitRule r{};
            r.size = sizeof r;
            r.attacker = attacker;
            r.target = target;
            r.scale = 1.0f;
            r.add = add;
            return emf::core::api_table()->hit_rule_add(&r);
        };
        d.remove_rule = [](uint32_t id) { emf::core::api_table()->hit_rule_remove(id); };
        d.dead = [](EmfWorld* w, uint32_t entity) {
            int32_t cur = 0;
            return emf::core::api_table()->attribute_get(w, entity, EMF_ATTR_HEALTH, &cur) && cur <= 0;
        };
        g_orders = std::make_unique<emf::core::EnemyOrders>(std::move(d));
        g_state->orders = g_orders.get();
    }
    // v40 move orders: the game's guard order on a hidden waypoint EMF spawns at each point (move_orders.h); the
    // emf_moves program sends the guard orders
    {
        emf::core::MoveOrders::Deps d;
        d.get = [](EmfWorld* w, uint32_t hash, uint32_t entity, uint64_t* size) -> void* {
            return emf::core::api_table()->get_component(w, hash, entity, size);
        };
        d.now_ms = [] { return GetTickCount64(); };
        d.spawn = [](EmfWorld* w, double x, double y, double z) {
            return emf::core::api_table()->spawn(w, emf::core::kWaypointGuid, x, y, z);
        };
        d.destroy = [](EmfWorld* w, uint32_t entity) { emf::core::api_table()->destroy(w, entity); };
        d.guard = [](uint32_t guarding, uint32_t guarded) {
            std::lock_guard<std::mutex> l(g_guard_lock);
            if (g_guard_queue.size() < kGuardQueueMax) g_guard_queue.emplace_back(guarding, guarded);
        };
        d.unguard = [](uint32_t guarding) {
            std::lock_guard<std::mutex> l(g_guard_lock);
            g_guard_queue.erase(std::remove_if(g_guard_queue.begin(), g_guard_queue.end(),
                                               [guarding](const auto& g) { return g.first == guarding; }),
                                g_guard_queue.end());
        };
        d.dead = [](EmfWorld* w, uint32_t entity) {
            int32_t cur = 0;
            return emf::core::api_table()->attribute_get(w, entity, EMF_ATTR_HEALTH, &cur) && cur <= 0;
        };
        d.nudge = [](EmfWorld* w, uint32_t entity, double x, double y, double z) {
            EmfTeleport t{};
            t.size = sizeof t;
            t.entity = entity;
            t.x = x;
            t.y = y;
            t.z = z;
            t.flags = EMF_TELEPORT_FIND_GROUND;
            const EmfApi* api = emf::core::api_table();
            return api->teleport && api->teleport(w, &t);
        };
        emf::core::ProgramSpec s;
        s.name = kMovesProgramName;
        s.world_kind = EMF_WORLD_SERVER;
        s.fn = moves_program;
        s.events_out = {"StartGuardingEvent", "NpcCommandEvent"};
        if (g_state->system_programs.registration_closed() || !g_state->system_programs.add(s, true)) {
            emf::logf("move orders: off (the %s program could not be added)", kMovesProgramName);
        } else {
            emf::core::VillagerMoves::Deps vd;
            vd.get = d.get;
            vd.tpl = [](EmfWorld* w, uint32_t hash, uint32_t entity, uint64_t* size) -> const void* {
                const EmfApi* a = emf::core::api_table();
                return a->get_template_component ? a->get_template_component(w, hash, entity, size) : nullptr;
            };
            vd.now_ms = [] { return GetTickCount64(); };
            vd.go_to = [](uint32_t npc, double x, double y, double z) { g_npc_queue.push(npc, x, y, z); };
            vd.drop = [](uint32_t npc) { g_npc_queue.drop(npc); };
            g_moves = std::make_unique<emf::core::MoveOrders>(std::move(d));
            g_state->moves = g_moves.get();
            g_villager_moves = std::make_unique<emf::core::VillagerMoves>(std::move(vd));
            g_state->villager_moves = g_villager_moves.get();
            emf::logf("move orders and villager moves: on (program %s in the server world)", kMovesProgramName);
        }
    }
    // v38 doors: an internal server program writes the door components through the game's writeOther (private research
    // doors_2026-11-12); reading needs no program.
    {
        using emf::Sig;
        auto sig = [&](Sig s) -> uintptr_t {
            const auto& r = g_state->sigs[static_cast<size_t>(s)];
            return emf::is_usable(r, known) ? r.addr : 0;
        };
        emf::core::DoorService::Deps deps;
        deps.read_other = reinterpret_cast<emf::core::ReadOtherFn>(sig(Sig::ReadOther));
        deps.write_other = reinterpret_cast<emf::core::WriteOtherFn>(sig(Sig::WriteOther));
        deps.get_component = g_state->get_component;
        deps.log = [](const std::string& line) { emf::logf("%s", line.c_str()); };
        deps.save = [](const std::string& text) { return g_door_file.save(text); };  // false until the world's file was read
        deps.list_doors = list_doors;
        deps.players = player_places;
        if (!deps.read_other || !deps.write_other || !sig(Sig::IterBegin)) {
            emf::logf("doors: off (the ReadOther, WriteOther or IterBegin signatures are not usable)");
        } else if (g_state->system_programs.registration_closed()) {
            emf::logf("doors: off (mod programs are unavailable)");
        } else {
            g_doors = std::make_unique<emf::core::DoorService>(std::move(deps));
            if (g_state->system_programs.add(g_doors->spec(), true)) {
                g_state->doors = g_doors.get();
                emf::logf("doors: on (program %s in the server world)", emf::core::kDoorsProgramName);
                if (!g_door_file.attach(emf::core::api_table())) emf::logf("doors: locks are not kept per world");
            } else {
                emf::logf("doors: off (the %s program could not be added)", emf::core::kDoorsProgramName);
                g_doors.reset();
            }
        }
    }
    // the chat key gate (client only): the HUD screen byte's offset from the chat window's own read of it
    if (!g_state->exe.server) {
        const auto& r = g_state->sigs[static_cast<size_t>(emf::Sig::HudScreenRead)];
        const uint32_t off = emf::is_usable(r, known) ? emf::core::hud_screen_offset_at(reinterpret_cast<const uint8_t*>(r.addr)) : 0;
        emf::core::game_screen().set_offset(off);
        if (off)
            emf::logf("chat key gate: on (mods' keys read as up while the game's chat or rename box is open)");
        else
            emf::logf("chat key gate: off (the HudScreenRead signature is not usable on this game build): mods' hotkeys "
                      "also fire while typing in the game's chat");
    }
    // v33 markers (client only): the camera block's offset and the game's map list from the guards' matched code
    // (private plans/2026-11-11-markers-step1.ledger.md, Task 1)
    {
        using emf::Sig;
        auto& mk = g_state->markers;
        const auto& cam = g_state->sigs[static_cast<size_t>(Sig::UiCameraBlock)];
        uint32_t view_off = 0;
        if (g_state->exe.server)
            mk.world_why = "the dedicated server draws nothing";
        else if (!emf::is_usable(cam, known))
            mk.world_why = "the UiCameraBlock signature is not usable on this game build";
        else if (!emf::seh_read(reinterpret_cast<const uint8_t*>(cam.addr) + 3, &view_off, 4) || view_off < 0x100 ||
                 view_off > 0x100000)
            mk.world_why = "the camera block's offset could not be read from the game's code";
        if (mk.world_why.empty()) {
            // the camera comes through the UI scope detours: hooked here in pass-through, so markers work also when
            // the HUD editor is switched off (its own install reuses them)
            std::string why;
            if (emf::core::hud::install_scope_hooks(g_state->sigs, known, build, &why)) {
                emf::core::marker_capture_set_layout(view_off);
                // v35 the minimap: the game's own map drawn into the canvases' MAP commands
                auto usable = [&](Sig s) -> uintptr_t {
                    const auto& r = g_state->sigs[static_cast<size_t>(s)];
                    return emf::is_usable(r, known) ? r.addr : 0;
                };
                emf::core::minimap_draw_setup(
                    reinterpret_cast<emf::core::MapCanvasFn>(usable(Sig::MapCanvasDraw)),
                    reinterpret_cast<emf::core::UiPushClipFn>(usable(Sig::UiPushClip)),
                    reinterpret_cast<emf::core::UiPopClipFn>(usable(Sig::UiPopClip)), emf::core::hud::scope_open_original(),
                    emf::core::hud::scope_close_original());
                emf::logf("minimap: the game's map in canvases %s", emf::core::minimap_draw_status().c_str());
            } else {
                mk.world_why = "the UI scope hooks: " + why;
            }
        }
        const auto& clr = g_state->sigs[static_cast<size_t>(Sig::MapMarkerSliceClear)];
        uint32_t count_off = 0;
        if (g_state->exe.server)
            mk.list_why = "the dedicated server has no map";
        else if (!emf::is_usable(clr, known) ||
                 !emf::is_usable(g_state->sigs[static_cast<size_t>(Sig::MapMarkerEntryLayout)], known) ||
                 !emf::is_usable(g_state->sigs[static_cast<size_t>(Sig::MapMarkerEntryLabel)], known))
            mk.list_why = "the map list's signatures are not usable on this game build";
        else if (!emf::seh_read(reinterpret_cast<const uint8_t*>(clr.addr) + 15, &count_off, 4) || count_off < 0x1000 ||
                 count_off > 0x1000000)
            mk.list_why = "the map list's offset could not be read from the game's code";
        else if (!g_state->programs)
            mk.list_why = "program hooks are not available";
        if (mk.list_why.empty()) {
            mk.list_count_off = count_off;
            g_state->reserved_programs.push_back("clear_map_markers_for_ui");
            g_state->programs->request_hook(
                "clear_map_markers_for_ui", [](void* ctx, void*) { emf::core::markers_map_pre(ctx); },
                [](void* ctx, void*) { emf::core::markers_map_post(ctx); }, nullptr);
        }
        if (mk.world_why.empty())
            emf::logf("markers: world on (camera block at +0x%x); the game's map list %s", view_off,
                      mk.list_why.empty() ? "on" : ("off (" + mk.list_why + ")").c_str());
        else
            emf::logf("markers: drawing off (%s); marker_send works", mk.world_why.c_str());
        if (!emf::core::add_marker_commands(g_state->commands)) emf::logf("commands: the markers commands could not be added");
    }
    // v34 the map: ground_at from the game's voxel ray (the world's reset box from a hook on its setter); the map
    // screen's clicks for on_map_click (private plans/2026-11-11-markers-step2.md)
    {
        using emf::Sig;
        const auto& ray = g_state->sigs[static_cast<size_t>(Sig::VoxelRay)];
        const auto& box = g_state->sigs[static_cast<size_t>(Sig::ResetBoundsSet)];
        std::string why;
        if (!emf::is_usable(ray, known) || !emf::is_usable(box, known))
            why = "the voxel ray or reset-box signature is not usable on this game build";
        else if (emf::core::ground_hook_reset_bounds(box.addr, &why))
            emf::core::ground_set_ray(reinterpret_cast<emf::core::VoxelRayFn>(ray.addr));
        if (why.empty()) {
            emf::core::GroundService::Deps d;
            d.limits = emf::core::ground_limits;
            d.ray = emf::core::ground_ray;
            d.now_ms = [] { return GetTickCount64(); };
            // the shroud at a landing (EmfGround.fog): the game's fog sampler, when this build has it
            const auto& fog = g_state->sigs[static_cast<size_t>(Sig::FogSample)];
            if (emf::is_usable(fog, known)) {
                emf::core::ground_set_fog(reinterpret_cast<emf::core::FogSampleFn>(fog.addr));
                d.fog = emf::core::ground_fog;
            } else {
                emf::logf("map: the shroud at a landing is not read on this game build (the fog stays unknown)");
            }
            // the water over a landing (EmfGround.water): the game's water surface query, when this build has it
            const auto& water = g_state->sigs[static_cast<size_t>(Sig::WaterSurfacePlane)];
            if (emf::is_usable(water, known)) {
                emf::core::ground_set_water(reinterpret_cast<emf::core::WaterSurfaceFn>(water.addr));
                d.water = emf::core::ground_water;
            } else {
                emf::logf("map: the water at a landing is not read on this game build (a lake's bed is the landing)");
            }
            g_state->map.ground = std::make_unique<emf::core::GroundService>(std::move(d));
            emf::logf("map: ground_at on");
        } else {
            emf::logf("map: ground_at off (%s)", why.c_str());
        }
        emf::core::map_input_setup();
        g_state->map.actions.set_log([](const std::string& l) { emf::logf("%s", l.c_str()); });
        if (!emf::core::add_map_commands(g_state->commands)) emf::logf("commands: the map commands could not be added");
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
    // v37 path_find: the internal emf_path program asks the game's path grid (private research path_query_impl).
    {
        auto addr = [&](emf::Sig s) -> uintptr_t {
            const auto& r = g_state->sigs[static_cast<size_t>(s)];
            return emf::is_usable(r, known) ? r.addr : 0;
        };
        std::string off;
        if (!addr(emf::Sig::CreatePathQuery) || !addr(emf::Sig::DestroyPathQuery) ||
            !addr(emf::Sig::GetPathQueryResult) || !addr(emf::Sig::PathQueryDefaults))
            off = "its signatures are not usable on this game build";
        else if (g_state->system_programs.registration_closed())
            off = "mod programs are unavailable";
        if (off.empty()) {
            emf::core::PathGlueDeps deps;
            deps.fns.create = reinterpret_cast<decltype(deps.fns.create)>(addr(emf::Sig::CreatePathQuery));
            deps.fns.destroy = reinterpret_cast<decltype(deps.fns.destroy)>(addr(emf::Sig::DestroyPathQuery));
            deps.fns.get = reinterpret_cast<decltype(deps.fns.get)>(addr(emf::Sig::GetPathQueryResult));
            deps.fns.defaults = reinterpret_cast<decltype(deps.fns.defaults)>(addr(emf::Sig::PathQueryDefaults));
            deps.component = [](uint8_t* world, uint32_t hash, uint32_t entity) -> const void* {
                return emf::core::api_table()->get_component(reinterpret_cast<EmfWorld*>(world), hash, entity, nullptr);
            };
            deps.after_step = [](std::function<void(EmfWorld*)> fn) {
                auto* job = new std::function<void(EmfWorld*)>(std::move(fn));
                g_state->dispatcher.post(EMF_WORLD_SERVER, [](EmfWorld* w, void* u) {
                    auto* f = static_cast<std::function<void(EmfWorld*)>*>(u);
                    (*f)(w);
                    delete f;
                }, job);
            };
            deps.log = [](const std::string& line) { emf::logf("%s", line.c_str()); };
            deps.now_ms = [] { return GetTickCount64(); };
            // path.door reads the grid's memory: only where its four guards pin the layout EMF knows
            deps.grid_layout = addr(emf::Sig::PathGridRegister) && addr(emf::Sig::PathGridBoxToCells) &&
                               addr(emf::Sig::PathGridNodePos) && addr(emf::Sig::PathGridCellList);
            const bool grid_layout = deps.grid_layout;
            if (g_state->system_programs.add(emf::core::path_setup(std::move(deps)), true)) {
                emf::logf("path_find: on (program %s in the server world)", emf::core::kPathProgramName);
                if (!grid_layout)
                    emf::logf("path.door: off (the PathGrid guard signatures are not usable on this game build)");
                g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
                    if (emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() == EMF_WORLD_SERVER)
                        emf::core::path_server_tick(GetTickCount64());
                }, nullptr);
            } else {
                emf::core::path_stop();
                off = std::string("the ") + emf::core::kPathProgramName + " program could not be added";
            }
        }
        if (!off.empty()) emf::logf("path_find: off (%s)", off.c_str());
    }
    // v44 breaking blocks: the internal emf_blocks program calls the game's building-mining kernel with EMF's own table
    // (private research wall_damage_2026-11-15.md); block_at and find_wall read voxels after a step.
    {
        auto addr = [&](emf::Sig s) -> uintptr_t {
            const auto& r = g_state->sigs[static_cast<size_t>(s)];
            return emf::is_usable(r, known) ? r.addr : 0;
        };
        emf::core::BlockGlueDeps deps;
        deps.fns.kernel = reinterpret_cast<decltype(deps.fns.kernel)>(addr(emf::Sig::BlockKernel));
        deps.fns.region_read = reinterpret_cast<decltype(deps.fns.region_read)>(addr(emf::Sig::VoxelRegionRead));
        deps.fns.config_find = reinterpret_cast<decltype(deps.fns.config_find)>(addr(emf::Sig::BuildingConfigFind));
        deps.fns.damage_get = reinterpret_cast<decltype(deps.fns.damage_get)>(addr(emf::Sig::VoxelDamageGet));
        // ruin protection: the shipped world's column reader (all four, or none)
        deps.fns.column_acquire = reinterpret_cast<decltype(deps.fns.column_acquire)>(addr(emf::Sig::BlockColumnAcquire));
        deps.fns.brick_seek = reinterpret_cast<decltype(deps.fns.brick_seek)>(addr(emf::Sig::BlockBrickSeek));
        deps.fns.brick_decode = reinterpret_cast<decltype(deps.fns.brick_decode)>(addr(emf::Sig::BlockBrickDecode));
        deps.fns.column_release = reinterpret_cast<decltype(deps.fns.column_release)>(addr(emf::Sig::BlockColumnRelease));
        deps.after_step = [](std::function<void(EmfWorld*)> fn) {
            auto* job = new std::function<void(EmfWorld*)>(std::move(fn));
            g_state->dispatcher.post(EMF_WORLD_SERVER, [](EmfWorld* w, void* u) {
                auto* f = static_cast<std::function<void(EmfWorld*)>*>(u);
                (*f)(w);
                delete f;
            }, job);
        };
        deps.log = [](const std::string& line) { emf::logf("%s", line.c_str()); };
        deps.now_ms = [] { return GetTickCount64(); };
        deps.drop = [](EmfWorld* w, const uint8_t guid[16], uint32_t count, const float pos[3]) {
            return emf::core::api_drop_items(w, guid, count, pos);
        };
        std::string off;
        if (!deps.fns.kernel || !deps.fns.config_find)
            off = "the BlockKernel or BuildingConfigFind signature is not usable on this game build";
        else if (!addr(emf::Sig::IterBegin))
            off = "the IterBegin signature is not usable on this game build";
        else if (g_state->system_programs.registration_closed())
            off = "mod programs are unavailable";
        const emf::core::ProgramSpec spec = emf::core::blocks_setup(std::move(deps));
        if (off.empty()) {
            if (g_state->system_programs.add(spec, true)) {
                emf::logf("breaking blocks: on (program %s in the server world)", emf::core::kBlocksProgramName);
                g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
                    if (emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() == EMF_WORLD_SERVER)
                        emf::core::blocks_server_tick(GetTickCount64());
                }, nullptr);
            } else {
                off = std::string("the ") + emf::core::kBlocksProgramName + " program could not be added";
            }
        }
        if (!off.empty()) {
            emf::core::blocks_stop(off.find("signature") != std::string::npos ? "not available on this game version"
                                                                               : "off (" + off + ")");
            emf::logf("breaking blocks: off (%s)", off.c_str());
        }
    }
    // v23 change_component: the same record path (kind 3 / kind 4), in a program of its own.
    {
        const auto& qp = g_state->sigs[static_cast<size_t>(emf::Sig::QueuePush)];
        std::string off;
        if (!emf::is_usable(qp, known))
            off = "the QueuePush signature is not usable on this game build";
        else if (!emf::core::teleport_layouts_checked(build))
            off = "the game's command-buffer layout is not checked on this build";
        else if (g_state->system_programs.registration_closed())
            off = "mod programs are unavailable";
        if (off.empty()) {
            emf::core::ComponentService::Deps deps;
            deps.queue_push = reinterpret_cast<emf::core::QueuePushFn>(qp.addr);
            deps.get_component = g_state->get_component;
            deps.log = [](const std::string& line) { emf::logf("%s", line.c_str()); };
            deps.now_ms = [] { return GetTickCount64(); };
            deps.session = [] { return g_state->server_session.load(); };
            g_components = std::make_unique<emf::core::ComponentService>(std::move(deps));
            if (g_state->system_programs.add(g_components->spec(), true)) {
                g_state->components = g_components.get();
                emf::logf("change_component: on (program %s in the server world)", emf::core::kComponentsProgramName);
                // v25 E prompts: the offers' components go through the service above; the accept is the server
                // world's InteractionAcceptedEvent (asked for now, before the game registers its programs)
                emf::core::OfferService::Deps od;
                od.change = [](const emf::core::ComponentChangeRequest& r, std::string* why) {
                    return g_components && g_components->request(r, why);
                };
                const auto& spawn_sig = g_state->sigs[static_cast<size_t>(emf::Sig::SpawnWrapper)];
                if (emf::is_usable(spawn_sig, known)) {
                    const auto spawn_fn = reinterpret_cast<emf::core::SpawnWrapperFn>(spawn_sig.addr);
                    od.spawn = [spawn_fn](EmfWorld* w, const uint8_t guid[16], double x, double y, double z,
                                          const std::vector<emf::core::OfferComponent>& comps, std::string* why) {
                        std::vector<emf::core::SpawnComponent> sc;
                        for (const auto& c : comps)
                            sc.push_back({c.hash, 0, c.bytes.data(), static_cast<uint32_t>(c.bytes.size())});
                        return emf::core::spawn_entity_ex(spawn_fn, reinterpret_cast<uint8_t*>(w), guid, x, y, z,
                                                          sc.data(), sc.size(), why);
                    };
                }
                od.has = [](EmfWorld* w, uint32_t hash, uint32_t entity) {
                    return emf::get_component(g_state->get_component, reinterpret_cast<uint8_t*>(w), hash, entity)
                               .ptr != nullptr;
                };
                od.read_offer = [](EmfWorld* w, uint32_t entity) -> std::optional<std::pair<uint32_t, uint32_t>> {
                    using Ior = emf::gen::ecs::InteractionOfferRuntime;
                    const auto ref = emf::get_component(g_state->get_component, reinterpret_cast<uint8_t*>(w), Ior::kHash,
                                                        entity);
                    if (!ref.ptr || ref.size < Ior::kSize) return std::nullopt;
                    uint32_t sequence = 0, verb = 0;
                    if (!emf::seh_read(static_cast<const uint8_t*>(ref.ptr) + offsetof(Ior, defaultSequence), &sequence, 4) ||
                        !emf::seh_read(static_cast<const uint8_t*>(ref.ptr) + offsetof(Ior, defaultVerbId), &verb, 4))
                        return std::nullopt;
                    return std::make_pair(sequence, verb);
                };
                od.session = [] { return g_state->server_session.load(); };
                od.log = [](const std::string& line) { emf::logf("%s", line.c_str()); };
                g_offers = std::make_unique<emf::core::OfferService>(std::move(od));
                std::string offer_why;
                if (g_state->relay.subscribe(
                        EMF_WORLD_SERVER, "InteractionAcceptedEvent", emf::gen::ecs::InteractionAcceptedEvent::kSize,
                        [](EmfWorld* w, const void* ev, uint32_t size, void*) {
                            if (g_offers) g_offers->on_accept(w, ev, size);
                        },
                        nullptr, &offer_why)) {
                    g_state->offers = g_offers.get();
                    emf::logf("offers: on (E prompts; InteractionAcceptedEvent in the server world)");
                    if (!g_state->commands.add("offers.list", ": the E prompts mods put on entities in this world",
                                               cmd_offers_list, nullptr, nullptr))
                        emf::logf("commands: \"offers.list\" could not be added");
                } else {
                    g_offers.reset();
                    emf::logf("offers: off (InteractionAcceptedEvent: %s)", offer_why.c_str());
                }
                std::thread([] {  // back to the menu: what is still under way is answered (a new world: its setup)
                    for (;;) {
                        Sleep(1000);
                        if (g_components && g_components->pending() &&
                            !g_state->tracker.latest_within(EMF_WORLD_SERVER, GetTickCount64(), 10000))
                            g_components->end_session("the world ended (back to the menu)");
                    }
                }).detach();
            } else {
                g_components.reset();
                off = std::string("the ") + emf::core::kComponentsProgramName + " program could not be added";
            }
        }
        if (!off.empty()) {
            g_state->components_off = off;
            emf::logf("change_component: off (%s)", off.c_str());
        }
    }
    auto hook_sig = [&](emf::Sig s, void* detour, void** original, const char* what) {
        const auto& r = g_state->sigs[static_cast<size_t>(s)];
        const bool ok = emf::is_usable(r, known) && emf::core::install_emf_hook(what, r.addr, detour, original);
        emf::logf("%s %s", what, ok ? "hooked" : "NOT hooked (event unavailable)");
    };
    // on_save needs two save-state offsets read from save2WriteWorld's code (they moved in build 1273043).
    const auto& result_off = code_offsets[static_cast<size_t>(emf::CodeOffset::Save2Result)];
    const auto& slot_off = code_offsets[static_cast<size_t>(emf::CodeOffset::Save2WorldSlot)];
    const auto& load_flag = code_offsets[static_cast<size_t>(emf::CodeOffset::InstWorldSlotFlag)];
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
    // v21 world settings: the game's resync needs sendServerInfo and the two GameServer instance offsets in it.
    {
        const auto& ws_flag = code_offsets[static_cast<size_t>(emf::CodeOffset::InstSendSettingsFlag)];
        const auto& ws_ptr = code_offsets[static_cast<size_t>(emf::CodeOffset::InstSettingsPtr)];
        // only where sendServerInfo's RVA was checked: the flag write goes into a live GameServer instance
        const bool ws_sync = sig_ok(emf::Sig::SendServerInfo) &&
                             emf::rva_checked(*emf::find_signature("SendServerInfo"), build) &&
                             emf::is_usable(ws_flag, known) && emf::is_usable(ws_ptr, known);
        g_ws = std::make_unique<WorldSettingsRuntime>();
        emf::core::ws::GlueDeps gd;
        gd.settings_ptr_off = ws_sync ? ws_ptr.value : 0;
        gd.flag_off = ws_sync ? ws_flag.value : 0;
        gd.log = [](const std::string& line) { emf::logf("%s", line.c_str()); };
        gd.save_file = [](const std::string& text) { return g_ws && g_ws->file.save(text); };
        g_ws->glue = std::make_unique<emf::core::ws::Glue>(g_ws->state, std::move(gd));
        g_ws->state.require_plausible_base(!known);  // a build EMF does not know: check the struct looks right first
        g_ws->service = std::make_unique<emf::core::ws::Service>(
            g_ws->state, emf::core::ws::ServiceDeps{&server_game_settings, [] { return g_state->server_session.load(); },
                                                    [] { return g_ws && g_ws->glue->sync_ok(); },
                                                    [] { return g_ws && g_ws->file.ready(); }});
        g_state->world_settings = g_ws->service.get();
        if (!g_ws->file.attach(emf::core::api_table())) emf::logf("world settings: the kept settings per world are off");
        emf::logf("world settings: on%s", ws_sync ? "" : " (the game's resync is not known on this build: players' games keep "
                                                          "the old values until they reconnect)");
        g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
            if (!g_ws || emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() != EMF_WORLD_SERVER) return;
            auto* live = static_cast<uint8_t*>(emf::WorldView(reinterpret_cast<uint8_t*>(w)).singleton("ecs::GameSettings"));
            if (!live) return;
            std::string text;
            const bool got = g_ws->file.poll([&](const std::string& t) { text = t; });
            const uint32_t session = g_state->server_session.load();
            auto* inst = g_state->save_instance_session.load() == session ? static_cast<uint8_t*>(g_state->save_instance.load())
                                                                          : nullptr;
            g_ws->service->notify(g_ws->glue->server_step(live, inst, session, GetTickCount64(), got ? &text : nullptr));
        }, nullptr);
    }
    // put_items watchdog: requests end with 0 when emf_inventory does not run in the server world.
    g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
        if (g_inventory && emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() == EMF_WORLD_SERVER)
            g_inventory->on_server_tick(w);
    }, nullptr);
    // v33 markers: the client world's UiPlayData, local player and followed entities; the server's followed positions.
    g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
        const uint8_t kind = emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind();
        if (kind == EMF_WORLD_CLIENT) {
            emf::core::markers_client_tick(w);
            emf::core::minimap_client_tick(w, GetTickCount64());  // v35 the fog of war
        }
        else if (kind == EMF_WORLD_SERVER) {
            emf::core::markers_server_tick(w);
            emf::core::map_server_tick(w);  // v34 ground_at
            if (g_orders) g_orders->step(w);  // v39 enemy orders
            if (g_moves) g_moves->step(w);    // v40 move orders
            if (g_villager_moves) g_villager_moves->step(w);  // v46 villager moves
            if (g_doors) {  // door locks kept per world: read once after the world's save loaded, saved when they change
                g_door_file.poll([](const std::string& text) { g_doors->load_saved(text); });
                g_doors->server_tick(reinterpret_cast<uint8_t*>(w), GetTickCount64());
            }
        }
    }, nullptr);
    // v32 buffs: the emf_buffs watchdog and the players' buff events after each server step.
    g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
        if (emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() != EMF_WORLD_SERVER) return;
        if (g_buffs) g_buffs->on_server_tick(w);
        emf::core::buffs_server_step(w);
    }, nullptr);
    // teleport: the state checks, the watch and the callbacks after each server step.
    g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
        if (g_teleport && emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() == EMF_WORLD_SERVER)
            g_teleport->on_server_tick(w);
        if (g_components && emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() == EMF_WORLD_SERVER)
            g_components->on_server_tick(w);
        if (g_offers && emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() == EMF_WORLD_SERVER)
            g_offers->on_server_tick(w);
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
    g_state->portrait_dir = dir + L"\\emf\\cache\\portraits";  // v26 image_load("portrait:<key>")
    static std::unique_ptr<emf::core::net::NetService> net;      // v27 mod messages (in process for now)
    net = std::make_unique<emf::core::net::NetService>(emf::core::net_deps(g_state.get()));
    g_state->net = net.get();
    static std::unique_ptr<emf::core::PlayerEvents> players;     // v28 on_player
    players = std::make_unique<emf::core::PlayerEvents>(emf::core::player_events_deps());
    g_state->players = players.get();
    // the mod check on join (core channel emf/mods): this game's list is its applied EML content mods and its mods
    static std::vector<std::pair<std::string, std::string>> content_mods;
    content_mods = emf::core::applied_content_mods(g_content_status, dir);
    static std::unique_ptr<emf::core::ModsCheck> mods_check;
    mods_check = std::make_unique<emf::core::ModsCheck>(emf::core::ModsCheck::Deps{
        [] {
            auto list = emf::core::this_games_mods(g_state->mods.list(), content_mods);
            list.insert(list.begin(), emf::core::ModEntry{'e', "emf", EMF_VERSION});
            return list;
        },
        [] { return emf::core::api_table()->server_world() == nullptr; },  // no server world here: someone else's
        [] { return g_state->net ? g_state->net->peer(0, emf::core::kModsChannel) : 0u; },
        [] { return g_state->client_session.load(); },
        [](const std::string& d) {
            std::string why;
            return g_state->net && g_state->net->to_server("emf", emf::core::kModsChannel, d, &why);
        },
        [](uint32_t player, const std::string& d) {
            std::string why;
            return g_state->net && g_state->net->to_player("emf", player, emf::core::kModsChannel, d, &why);
        },
        [](const std::string& n) {
            if (g_state->notify) g_state->notify(n);
        },
        [](const std::string& s) { emf::logf("%s", s.c_str()); },
        [](uint32_t p) { return g_state->net && g_state->net->peer(p, emf::core::kModsChannel) != 0; }});
    g_mods_check = mods_check.get();
    {
        emf::core::net::ChannelDef c;
        c.name = emf::core::kModsChannel;
        c.version = emf::core::kModsChannelVersion;
        c.flags = emf::core::net::kToServer | emf::core::net::kToPlayers;
        c.max_size = emf::core::kMaxModListBytes;
        c.per_10s = 5;
        c.on_server = [](EmfWorld*, const emf::core::net::NetFrom& f, const std::string& d) {
            if (!f.local && g_mods_check) g_mods_check->on_server_message(f.player, f.account, d);
        };
        c.on_player = [](EmfWorld*, const emf::core::net::NetFrom& f, const std::string& d) {
            if (!f.local && g_mods_check) g_mods_check->on_player_message(d);
        };
        std::string why;
        if (!net->add_channel("emf", c, &why)) emf::logf("mods check: off (%s)", why.c_str());
        // v33 markers: a server's markers for players' games
        emf::core::net::ChannelDef mc;
        mc.name = emf::core::kMarkersChannel;
        mc.version = emf::core::kMarkersChannelVersion;
        mc.flags = emf::core::net::kToPlayers;
        mc.max_size = emf::core::net::kMaxMessage;
        mc.on_player = [](EmfWorld*, const emf::core::net::NetFrom&, const std::string& d) {
            g_state->markers.receiver.on_message(d, g_state->markers.store);
        };
        std::string mwhy;
        if (net->add_channel("emf", mc, &mwhy)) {
            g_state->markers.receiver = emf::core::MarkerReceiver([](const std::string& l) { emf::logf("%s", l.c_str()); });
            g_state->markers.sender = std::make_unique<emf::core::MarkerSender>(
                [](uint32_t player, const std::string& d) -> uint32_t {
                    if (!g_state->net) return 0;
                    if (player == 0) return g_state->net->to_players("emf", emf::core::kMarkersChannel, d);
                    std::string w;
                    return g_state->net->to_player("emf", player, emf::core::kMarkersChannel, d, &w) ? 1u : 0u;
                },
                [](const std::string& l) { emf::logf("%s", l.c_str()); });
        } else {
            emf::logf("markers: marker_send off (%s)", mwhy.c_str());
        }
        // v34 map actions: the server's actions to players' games, a player's Shift + hold spot back
        std::string awhy;
        if (!emf::core::map_net_setup(net.get(), &awhy)) emf::logf("map: map actions off (%s)", awhy.c_str());
        // a game's new hello (or its leaving) ends the list it sent before: only the new one counts
        net->set_peer_watch([](uint64_t account, bool joined) {
            if (g_mods_check) g_mods_check->forget(account);
            // markers (v33): a game whose hello came gets what a mod sent it (or every player) before its link was up
            // map actions (v34): it hears which action Shift + hold does here
            if (joined && (g_state->markers.sender || emf::core::map_greets())) {
                auto* acct = new uint64_t(account);
                g_state->dispatcher.post(EMF_WORLD_SERVER, [](EmfWorld*, void* u) {
                    const uint64_t a = *static_cast<uint64_t*>(u);
                    delete static_cast<uint64_t*>(u);
                    const uint32_t entity = g_state->net_entity_of ? g_state->net_entity_of(a) : 0;
                    if (entity && g_state->markers.sender) g_state->markers.sender->replay(entity);
                    if (entity && emf::core::map_greets()) emf::core::map_on_hello(entity);
                }, acct);
            }
        });
        // the core's channel alone greets players only when this server has mods they must match
        net->set_core_greets([] {  // asked on every receive-loop run: the answer is kept for 2 s
            static std::atomic<uint64_t> at{0};
            static std::atomic<bool> yes{false};
            const uint64_t now = GetTickCount64();
            if (now - at.load() >= 2000) {
                yes = g_mods_refuse || (g_mods_check && g_mods_check->has_shared_mods()) ||  // refuse asks every game
                      (g_state->markers.sender && g_state->markers.sender->any_sent()) ||  // a mod sent markers
                      emf::core::map_greets();  // a mod has a map action
                at = now;
            }
            return yes.load();
        });
        // the beacon's words for a player without EMF: never "you can play without it" where they cannot (the mods
        // check's refuse kicks them; content mods give the server other data than theirs)
        net->set_beacon_case([] {
            return emf::core::net::beacon_case_for(g_mods_refuse != nullptr,
                                                   g_mods_check && g_mods_check->server_has_content());
        });
    }
    // [net] mods_check=refuse (the owner's option; warn is the default): a player whose game lacks a required mod (or has
    // no EMF) is told why in the chat, then kicked 5 s later with the game's own admin kick (handleAccountAction with a
    // Kick event; never an admin, never the host). Only with both kick signatures on this build, with the greeting on.
    {
        const std::wstring ini = dir + L"\\emf\\emf_core.ini";
        wchar_t check[64] = {}, required[4096] = {}, info[512] = {};  // 64 ids of up to 48 fit
        GetPrivateProfileStringW(L"net", L"mods_check", L"", check, 64, ini.c_str());
        GetPrivateProfileStringW(L"net", L"mods_required", L"", required, 4096, ini.c_str());
        GetPrivateProfileStringW(L"net", L"mods_info", L"", info, 512, ini.c_str());
        std::vector<std::string> problems;
        auto policy = emf::core::parse_mods_policy(utf8(check), utf8(required), utf8(info), &problems);
        for (const auto& p : problems) emf::logf("mods check: %s", p.c_str());
        g_mods_required = policy.required;  // named after the mods load when this server does not have one
        if (policy.refuse) {
            const auto& handler = g_state->sigs[static_cast<size_t>(emf::Sig::HandleAccountAction)];
            const auto& call = g_state->sigs[static_cast<size_t>(emf::Sig::SessionRequestKick)];
            const char* off = nullptr;
            if (!emf::is_usable(handler, g_state->known_build) || !emf::is_usable(call, g_state->known_build))
                off = "the game's kick was not found on this game build";
            else if (GetPrivateProfileIntW(L"net", L"enabled", 1, ini.c_str()) == 0)
                off = "[net] enabled=0 (no mod lists come)";
            else if (GetPrivateProfileIntW(L"net", L"greet", 1, ini.c_str()) == 0)
                off = "[net] greet=0 (no game is asked for its mods)";
            if (off) {
                emf::logf("mods check: [net] mods_check=refuse is off: %s; players are only warned", off);
                policy.refuse = false;
            }
        }
        if (policy.refuse) {
            g_kick_fn = reinterpret_cast<emf::core::HandleAccountActionFn>(
                g_state->sigs[static_cast<size_t>(emf::Sig::HandleAccountAction)].addr);
            std::string req;
            for (const auto& id : policy.required) req += (req.empty() ? "" : ", ") + id;
            emf::logf("mods check: refuse (players without EMF or without these mods are told why, then kicked 5 s later; "
                      "required: the content and side-both mods%s%s; never admins or the host)",
                      req.empty() ? "" : ", ", req.c_str());
            static std::unique_ptr<emf::core::ModsRefuse> refuse;
            refuse = std::make_unique<emf::core::ModsRefuse>(std::move(policy), emf::core::ModsRefuse::Deps{
                [](uint64_t account, uint32_t entity) -> std::optional<std::vector<emf::core::ModEntry>> {
                    const auto s = g_mods_check ? g_mods_check->current(account, entity) : std::nullopt;
                    if (!s) return std::nullopt;
                    return s->mods;
                },
                [] { return g_mods_check ? g_mods_check->mine() : std::vector<emf::core::ModEntry>{}; },
                [](uint32_t entity, uint64_t account) {
                    // the game's CanKickBan (its kick skips them anyway), or an account in [chat] admins
                    for (uint64_t a : g_net_policy.admins)
                        if (a == account) return true;
                    const auto info = server_player_info(entity);  // their connection: its own record's bit
                    return game_admin_account(info && info->account == account ? info->handle : 0, account);
                },
                [](uint32_t entity, uint64_t account, const std::string& text) {
                    const auto c = net_connection_of(entity);
                    bool ok = true;
                    for (const auto& line : emf::core::chat_lines(text, 2))
                        ok = emf::core::queue_chat_tell(c ? c->first : 0, account, line, true) && ok;
                    return ok;
                },
                [](uint64_t account) { return emf::core::kick_account(g_kick_fn, emf::core::chat_server(), account); },
                [](const std::string& s) { emf::logf("%s", s.c_str()); }});
            g_mods_refuse = refuse.get();
            // the server world's thread (the game runs its own account actions there too), once a second
            g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
                static ULONGLONG last = 0;
                if (!g_mods_refuse || emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() != EMF_WORLD_SERVER) return;
                const ULONGLONG now = GetTickCount64();
                if (now - last < 1000) return;
                last = now;
                g_mods_refuse->tick(emf::core::read_players(emf::core::api_table(), w), now);
            }, nullptr);
        }
    }
    // a joined game: once a second, its list goes to an EMF server that has emf/mods (once per session)
    g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
        static ULONGLONG last = 0;
        if (!g_mods_check || emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() != EMF_WORLD_CLIENT) return;
        const ULONGLONG now = GetTickCount64();
        if (now - last < 1000) return;
        last = now;
        g_mods_check->client_tick();
    }, nullptr);
    // after the chat lines: who joined, left or died (the event relay delivered this step's deaths before)
    g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
        if (g_state->players && emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() == EMF_WORLD_SERVER)
            g_state->players->server_step(w, g_state->server_session.load());
    }, nullptr);
    g_state->core_settings_backend = std::make_unique<emf::core::CoreIniBackend>(dir + L"\\emf\\emf_core.ini");
    g_state->settings_route = std::make_unique<emf::core::RoutingIniBackend>(g_state->core_settings_backend.get(),
                                                                              g_state->settings_backend.get());
    g_state->settings = std::make_unique<emf::core::SettingsStore>(g_state->settings_route.get());
    // EMF's own settings (the console's Settings tab; `set emf hud.size 120`), kept in emf_core.ini
    for (const auto& line : emf::core::declare_emf_settings(*g_state->settings, dir + L"\\emf\\fonts"))
        emf::logf("%s", line.c_str());
    g_builtin.registry = &g_state->mods;
    g_builtin.settings = g_state->settings.get();
    if (!emf::core::add_builtin_commands(g_builtin)) emf::logf("commands: a built-in command could not be added");
    if (!g_state->commands.add("players.mods",
                               ": each player in the world: their EMF and the mods that differ from this server's (the mod "
                               "check on join)",
                               cmd_players_mods, nullptr, nullptr))
        emf::logf("commands: \"players.mods\" could not be added");
    if (g_ws && g_builtin.center && !emf::core::add_world_commands(*g_builtin.center, *g_ws->service))
        emf::logf("commands: a world settings command could not be added");
    // Mod profiles: the same files and rules as EMF.exe's Mods tab; settings go through the live store
    g_profiles.game = dir;
    g_profiles.settings = g_state->settings.get();
    g_profiles.emf_version = EMF_VERSION;
    g_profiles.run = emf::core::profile_worker_runner();  // never on the render thread or a world's thread
    if (!emf::core::add_profile_commands(g_state->commands, g_profiles)) emf::logf("commands: \"profile\" could not be added");
    // The HUD editor: the game client only. On the dedicated server nothing is created, hooked or registered. Its hooks are
    // installed by the editor itself, once, when its gate first says on (known build, signatures usable, setting on, no
    // other HUD mod): until then the game's UI code is untouched.
    if (!g_state->exe.server) {
        using namespace emf::core::hud;
        HudEditorDeps hd;
        hd.settings = g_state->settings.get();
        hd.dir = dir + L"\\emf\\hud";
        hd.build_number = g_state->exe.build_number;
        hd.gate = GateInput{false, hud_signatures_ready(g_state->sigs, known, build), build_label(g_state->exe.build_number, sha)};
        hd.install_hooks = [known, build](std::string* why) {
            return install_hud_hooks(g_state->sigs, known, build, &g_hud_editor->snapshots(), why);
        };
        hd.other_hud_mod = loaded_other_hud_mod;
        hd.fault_code = hud_fault_code;
        hd.live = hud_live;
        hd.scale_state = hud_scale_state;
        hd.bars_ready = hud_bars_hooked;
        hd.game_colours = bar_game_colours;
        hd.colour_stats = colour_stats;
        hd.panel_status = panel_status;
        hd.build_scale = hud_scale_ready(g_state->sigs, known, build);
        hd.build_bars = hud_bars_ready(g_state->sigs, known, build);
        hd.build_panel = hud_panel_ready(g_state->sigs, known, build);
        hd.key_down = [](uint32_t vk) { return emf::keys::down(vk); };
        hd.toggle_page = [] { emf::core::overlay_toggle_settings_page(emf::core::kPageHud); };
        hd.run = emf::core::profile_worker_runner();
        hd.now_ms = [] { return GetTickCount64(); };
        hd.log = [](const std::string& s) { emf::logf("%s", s.c_str()); };
        g_hud_editor = std::make_unique<HudEditor>(std::move(hd));
        g_hud_editor->start();
        if (!emf::core::add_hud_commands(g_state->commands, *g_hud_editor)) emf::logf("commands: \"hud.*\" could not be added");
        g_state->dispatcher.subscribe_tick([](EmfWorld* w, void*) {
            if (g_hud_editor && emf::WorldView(reinterpret_cast<uint8_t*>(w)).kind() == EMF_WORLD_CLIENT) g_hud_editor->tick();
        }, nullptr);
    }
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
    // [console] window, as the Settings tab reads it (on/true/1)
    if (g_state->settings->get(emf::core::kEmfSettingsId, "console.window") == "1")
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
        // the ini's [overlay] enabled, the rest as the Settings tab shows it (one parser: no change at the first frame)
        const auto cfg = emf::core::overlay_config_with_store(
            emf::core::read_overlay_config(dir + L"\\emf\\emf_core.ini"), *g_state->settings);
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
            deps.window_event = [](const emf::core::WindowRequest& r, uint32_t kind, const std::string& key,
                                   uint32_t reason) { emf::core::post_window_event(r, kind, key, reason); };
            deps.hud = &g_state->hud;
            deps.mods = &g_state->mods;
            deps.settings = g_state->settings.get();
            deps.hud_editor = g_hud_editor.get();
            deps.markers = &g_state->markers.store;
            deps.canvases = &g_state->minimap.canvases;  // v35
            deps.fog = &g_state->minimap.fog;
            if (emf::core::start_overlay(cfg, std::move(deps))) {
                g_state->open_wheel = [](const emf::core::WheelRequest& r) {
                    const bool ok = emf::core::overlay_open_wheel(r);
                    if (!ok) emf::logf("wheel_open refused: the console or another wheel is open, or the overlay cannot draw");
                    return ok;
                };
                g_state->open_window = [](emf::core::WindowRequest r, std::optional<emf::core::WindowRequest>* replaced,
                                          std::string* why) {
                    return emf::core::overlay_open_window(std::move(r), replaced, why);
                };
                g_state->update_window = [](const emf::core::WindowRequest& r) {
                    return emf::core::overlay_update_window(r);
                };
                g_state->close_window = [](const std::string& id, std::optional<emf::core::WindowRequest>* closed) {
                    return emf::core::overlay_close_window(id, closed);
                };
                g_state->load_image = [](const std::wstring& path, const std::string& key, std::string* why) {
                    return emf::core::overlay_load_image(path, key, why);
                };
                add_window_commands();
                g_state->notify = [](const std::string& s) {
                    emf::logf("[note] %s", s.c_str());
                    emf::core::overlay_notify(s);
                };
                // how EML content went at this start, in the console's scrollback
                if (!g_content_lines.console.empty())
                    if (auto* console = emf::core::overlay_console()) console->add_line(g_content_lines.console);
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
        // API v27 mod messages ride on the same two hooks, so they are installed for them too ([net] enabled=0: off)
        const bool net_on = g_state->net && GetPrivateProfileIntW(L"net", L"enabled", 1, ini.c_str()) != 0;
        if (players || net_on) {
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
            // [chat] game_admins=1: the game's own admins are EMF admins too (commands, mods' admin checks)
            sc.policy.game_admins = GetPrivateProfileIntW(L"chat", L"game_admins", 0, ini.c_str()) != 0;
            sc.policy.is_game_admin = game_admin_account;
            if (sc.policy.game_admins)
                emf::logf("players' chat commands: [chat] game_admins=1: the game's own admins (a dedicated server's Admin "
                          "group, a host's admins) may run every EMF command");
            g_net_policy = sc.policy;
            if (players) {  // [chat] players=1 (always on the dedicated server): players' /commands and on_chat
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
                if (GetPrivateProfileIntW(L"chat", L"debug", 0, ini.c_str()) != 0) sc.debug = chat_debug_dump;
                sc.on_line = [](emf::core::ChatLine line) { emf::core::note_chat_line(std::move(line)); };  // v19 on_chat
            }
            sc.log = [](const std::string& t) { emf::logf("%s", t.c_str()); };
            if (net_on) {
                sc.net_line = [](uint32_t handle, uint64_t account, std::string_view text) {
                    return g_state->net->server_line(handle, account, text);
                };
                sc.net_tick = [](const std::vector<emf::core::ChatConnection>& connected) {
                    std::vector<emf::core::net::Connection> v;
                    for (const auto& c : connected) v.push_back({c.handle, c.account});
                    g_state->net->server_tick(v);
                };
            }
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
            if (!ok) {
                emf::logf("players' chat commands: off (the server's chat functions were not found on this game build)");
            } else if (players) {
                if (everyone)
                    emf::logf("players' chat commands: on, for every player ([chat] everyone=1)");
                else
                    emf::logf("players' chat commands: on, for %zu admin account(s) ([chat] admins in emf\\emf_core.ini; "
                              "a refused player's account is logged)",
                              admin_count);
            }
            if (ok && net_on) start_net_transport(known, ini);
            else if (net_on) emf::logf("mod messages: in process only (the chat functions were not found on this game build)");
        }
    }
    if (!g_state->commands.add("chat.players",
                               ": the players of this world with their account hashes (for [chat] admins), "
                               "connection, name and game role",
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
    // a [net] mods_required id this server does not have (the same list the join check compares): most likely a typo
    {
        std::vector<std::string> own;
        for (const auto& m : emf::core::this_games_mods(g_state->mods.list(), content_mods)) own.push_back(m.id);
        for (const auto& line : emf::core::unknown_required(g_mods_required, own)) emf::logf("mods check: %s", line.c_str());
    }
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
