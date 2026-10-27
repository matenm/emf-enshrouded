// sample_mod: the first EMF mod. Built only against sdk/include.
//  - client world: logs the local player's position every 5 s
//  - server world (host only): logs how many villagers exist and their AI state every 10 s
//  - optional write test (emf/config/sample_mod.ini: [sample_mod] enable_write=1):
//    F8 on the host moves the time of day forward by 3 hours via the game's own setTimeOfDay,
//    F9/F11 spawn templates, F10 destroys the last one, F7 makes the nearest villager walk to the player,
//    F12 gives the player `give_item` x `give_count` (item debug names: emf/items.csv, written with dump_items=1)
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "item_dump.h"
#include "npc_command.h"
#include "spawn_track.h"
#include "time_skip.h"

namespace gen = emf::gen;

namespace {
constexpr const char* kMod = "sample_mod";
constexpr double kFixed = 4294967296.0;  // WorldPosition: 32.32 fixed point
constexpr int64_t kHourNs = 3600ll * 1000000000ll;

uint64_t g_last_client_ms = 0;
uint64_t g_last_server_ms = 0;
bool g_enable_write = false;
bool g_f8_was_down = false;
std::atomic<uint32_t> g_program_calls{0};
std::atomic<uint32_t> g_own_program_calls{0};

// Our own ECS system program (registered into the server world): runs once per server tick.
void own_server_program(void*, void*) { g_own_program_calls.fetch_add(1, std::memory_order_relaxed); }

// Program hook demo: runs on game worker threads before every client_player_state batch.
void count_program_call(void*, void*) { g_program_calls.fetch_add(1, std::memory_order_relaxed); }

void client_report(emf::World w) {
    EMF_LOG(kMod, "client_player_state ran %u times since the last report", g_program_calls.exchange(0));
    const uint32_t id = emf::local_player();
    if (!id) return;
    auto* t = w.get<gen::ecs::CurrentTransform>(id);
    if (!t) {
        EMF_LOG(kMod, "player %u: no transform yet", id);
        return;
    }
    const auto& p = t->transform.position;
    EMF_LOG(kMod, "player %u at (%.2f, %.2f, %.2f) m", id, p.x / kFixed, p.y / kFixed, p.z / kFixed);
}

bool entity_pos(emf::World w, uint32_t id, double out[3]);

void server_report(emf::World w) {
    EMF_LOG(kMod, "sample_mod_server_tick ran %u times since the last report", g_own_program_calls.exchange(0));
    struct Count {
        emf::World w;
        int villagers = 0;
        int logged = 0;
        int raw_only = 0;
        uint64_t raw_size = 0;
    } c{w};
    w.for_each_entity([&c](uint32_t id) {
        // The per-entity half of a split component is stored under its OWN type hash
        // (DynamicActiveNpcState), not under the template component's hash (ActiveNpcState).
        auto* npc = c.w.get<gen::ecs::DynamicActiveNpcState>(id);
        if (!npc) {
            uint64_t size = 0;
            if (c.w.get_raw(gen::ecs::DynamicActiveNpcState::kHash, id, &size)) {
                ++c.raw_only;
                c.raw_size = size;
            }
            return;
        }
        ++c.villagers;
        if (c.logged++ < 5) {
            double p[3] = {};
            const bool has_pos = entity_pos(c.w, id, p);
            EMF_LOG(kMod, "villager %u state=%u previous=%u decision=%u at (%.1f, %.1f, %.1f) m%s", id,
                    (unsigned)npc->state, (unsigned)npc->previousState, (unsigned)npc->decision, p[0], p[1], p[2],
                    has_pos ? "" : " (no transform)");
        }
    });
    EMF_LOG(kMod, "villagers: %d", c.villagers);
    if (c.raw_only) EMF_LOG(kMod, "warning: %d DynamicActiveNpcState components with unexpected size %llu", c.raw_only,
                            (unsigned long long)c.raw_size);
}

void server_write_test(emf::World w) {
    const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    const bool pressed = down && !g_f8_was_down;
    g_f8_was_down = down;
    if (!pressed) return;
    auto set_tod = emf::resolve<sample::SetTimeOfDayFn>("SetTimeOfDay");
    auto* daytime = static_cast<uint8_t*>(w.singleton("g38_daytime::Daytime"));
    auto* weather = static_cast<uint8_t*>(w.singleton("WeatherSystem"));
    if (!set_tod || !daytime) {
        EMF_LOG(kMod, "F8: SetTimeOfDay or Daytime not available");
        return;
    }
    const int64_t now = w.ecs_time_ns();
    if (now == 0) {  // an unknown game build whose tick fields are not confirmed: setTimeOfDay needs the real time
        EMF_LOG(kMod, "F8: the world's time is not confirmed on this game build: the time stays as it is");
        return;
    }
    const auto r = sample::time_skip(daytime, weather, set_tod, now, 3 * kHourNs);
    EMF_LOG(kMod, "F8: time of day %.2f h -> %.2f h (weather layers reset: %d)", r.old_tod / double(kHourNs),
            r.new_tod / double(kHourNs), r.layers_reset);
}

// ---- F9 spawn / F10 destroy test (server world) ----
// slot 0: spawn_guid (F9), slot 1: spawn_guid2 (F11)
uint8_t g_spawn_guid[2][16] = {};
bool g_have_spawn_guid[2] = {false, false};
bool g_f9_was_down = false;
bool g_f11_was_down = false;
bool g_f10_was_down = false;
double g_spawn_target[3] = {};
int g_track_ticks = 0;        // > 0 while waiting for the spawned entity's transform
const char* g_track_key = "F9";  // key of the spawn being tracked
uint32_t g_spawned_id = 0;
uint32_t g_destroyed_id = 0;  // waiting to confirm it is gone

bool edge(int vk, bool& was_down) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !was_down;
    was_down = down;
    return pressed;
}

bool entity_pos(emf::World w, uint32_t id, double out[3]) {
    auto* t = w.get<gen::ecs::CurrentTransform>(id);
    if (!t) return false;
    out[0] = t->transform.position.x / kFixed;
    out[1] = t->transform.position.y / kFixed;
    out[2] = t->transform.position.z / kFixed;
    return true;
}

void spawn_slot(emf::World w, int slot) {
    const char* key = sample::spawn_key(slot);
    const uint32_t player = emf::local_player();
    double p[3];
    if (!player || !entity_pos(w, player, p)) {
        EMF_LOG(kMod, "%s: player %u has no transform on the SERVER world (entity ids not shared?)", key, player);
    } else if (!g_have_spawn_guid[slot]) {
        EMF_LOG(kMod, "%s: player %u found on the server world at (%.2f, %.2f, %.2f): entity ids are shared; "
                      "no spawn guid configured", key, player, p[0], p[1], p[2]);
    } else {
        g_spawn_target[0] = p[0] + 2.0;
        g_spawn_target[1] = p[1];
        g_spawn_target[2] = p[2];
        const uint32_t id =
            emf::api()->spawn(w.raw(), g_spawn_guid[slot], g_spawn_target[0], g_spawn_target[1], g_spawn_target[2]);
        EMF_LOG(kMod, "%s: player %u on server world at (%.2f, %.2f, %.2f); spawned entity %u%s", key, player, p[0],
                p[1], p[2], id, id ? "" : " (FAILED, see the core log)");
        if (id) {
            g_spawned_id = id;
            g_track_ticks = 120;
            g_track_key = key;
        }
    }
}

void server_spawn_test(emf::World w) {
    if (edge(VK_F9, g_f9_was_down)) spawn_slot(w, 0);
    if (edge(VK_F11, g_f11_was_down)) spawn_slot(w, 1);
    if (g_track_ticks > 0 && g_spawned_id) {
        --g_track_ticks;
        double q[3];
        if (entity_pos(w, g_spawned_id, q)) {
            const double dx = q[0] - g_spawn_target[0], dy = q[1] - g_spawn_target[1], dz = q[2] - g_spawn_target[2];
            EMF_LOG(kMod, "%s: entity %u is at (%.2f, %.2f, %.2f), %.2f m from the spawn point", g_track_key,
                    g_spawned_id, q[0], q[1], q[2], std::sqrt(dx * dx + dy * dy + dz * dz));
            g_track_ticks = 0;
        } else if (g_track_ticks == 0) {
            EMF_LOG(kMod, "%s: entity %u has no transform after 2 s", g_track_key, g_spawned_id);
        }
    }
    if (edge(VK_F10, g_f10_was_down) && g_spawned_id) {
        const bool ok = emf::api()->destroy(w.raw(), g_spawned_id);
        EMF_LOG(kMod, "F10: destroy entity %u: %s", g_spawned_id, ok ? "requested" : "FAILED");
        if (ok) g_destroyed_id = g_spawned_id;
        g_spawned_id = 0;
    } else if (g_destroyed_id) {
        double p[3];
        if (!entity_pos(w, g_destroyed_id, p)) {
            EMF_LOG(kMod, "F10: entity %u is gone", g_destroyed_id);
            g_destroyed_id = 0;
        }
    }
}

// ---- F7 villager "come here" (server world; applied inside the villager's AI worker) ----
using NpcWorkerFn = void (*)(void* ctx);
using CurrentEntityIdFn = uint32_t* (*)(void* program_context, uint32_t* out);
NpcWorkerFn o_npc_worker = nullptr;
sample::NpcSetStateFn g_npc_set_state = nullptr;
sample::MarkWrittenFn g_mark_written = nullptr;
CurrentEntityIdFn g_current_entity = nullptr;
sample::PendingCommand g_come_here;
bool g_f7_was_down = false;

// Runs on a game worker thread for one villager. No C++ objects here: the body is SEH-guarded.
void come_here_step(void* ctx, uint32_t id) {
    double target[3];
    if (!g_come_here.take_if(id, GetTickCount64(), target)) return;
    __try {
        const bool ok = sample::apply_go_to_position(static_cast<uint8_t*>(ctx), target, g_npc_set_state, g_mark_written);
        EMF_LOG(kMod, "F7: villager %u %s", id, ok ? "is walking to the player" : "ignored the command (state Start)");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        EMF_LOG(kMod, "F7: villager %u: applying the command faulted", id);
    }
}

uint32_t current_entity(void* ctx) {
    uint32_t id = 0;
    __try {
        g_current_entity(*static_cast<void**>(ctx), &id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        id = 0;
    }
    return id;
}

void npc_worker_detour(void* ctx) {
    const uint32_t id = g_come_here.armed() ? current_entity(ctx) : 0;  // read before the worker runs
    o_npc_worker(ctx);
    if (id) come_here_step(ctx, id);
}

void install_come_here() {
    g_npc_set_state = emf::resolve<sample::NpcSetStateFn>("NpcSetState");
    g_mark_written = emf::resolve<sample::MarkWrittenFn>("MarkWritten");
    g_current_entity = emf::resolve<CurrentEntityIdFn>("CurrentEntityId");
    const bool ok = g_npc_set_state && g_mark_written && g_current_entity && emf::api()->hook &&
                    emf::api()->hook("NpcWorker", reinterpret_cast<void*>(&npc_worker_detour),
                                     reinterpret_cast<void**>(&o_npc_worker));
    if (!ok) o_npc_worker = nullptr;
    EMF_LOG(kMod, "F7 villager command: %s", ok ? "ready (NpcWorker hooked)" : "unavailable (signature or hook missing)");
}

void server_command_test(emf::World w) {
    g_come_here.expire(GetTickCount64());  // the target villager may never run its worker again
    if (!edge(VK_F7, g_f7_was_down)) return;
    if (!o_npc_worker) {
        EMF_LOG(kMod, "F7: villager command unavailable");
        return;
    }
    double player[3];
    const uint32_t pid = emf::local_player();
    if (!pid || !entity_pos(w, pid, player)) {
        EMF_LOG(kMod, "F7: player position unknown");
        return;
    }
    struct Best {
        emf::World w;
        const double* player;
        uint32_t id = 0;
        double dist2 = 1e300;
    } best{w, player};
    w.for_each_entity([&best](uint32_t id) {
        double p[3];
        if (!best.w.get<gen::ecs::DynamicActiveNpcState>(id) || !entity_pos(best.w, id, p)) return;
        const double dx = p[0] - best.player[0], dy = p[1] - best.player[1], dz = p[2] - best.player[2];
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best.dist2) {
            best.dist2 = d2;
            best.id = id;
        }
    });
    if (!best.id) {
        EMF_LOG(kMod, "F7: no villager in this world");
        return;
    }
    g_come_here.post(best.id, player, GetTickCount64());
    EMF_LOG(kMod, "F7: villager %u (%.1f m away) told to come to the player at (%.2f, %.2f, %.2f)", best.id,
            std::sqrt(best.dist2), player[0], player[1], player[2]);
}

// ---- command "sample.villagers": lists the villagers of the server world with state and position ----
void villagers_command(emf::World w, const std::vector<std::string>&) {
    int n = 0;
    w.for_each_entity([&w, &n](uint32_t id) {
        auto* npc = w.get<gen::ecs::DynamicActiveNpcState>(id);
        if (!npc) return;
        ++n;
        double p[3] = {};
        entity_pos(w, id, p);
        EMF_REPLY("villager %u state=%u decision=%u at (%.1f, %.1f, %.1f) m", id, (unsigned)npc->state,
                  (unsigned)npc->decision, p[0], p[1], p[2]);
    });
    EMF_REPLY("%d villager(s)", n);
}

// ---- items: dump_items=1 writes emf/items.csv once; F12 gives an item ----
bool g_dump_items = false;
bool g_items_dumped = false;
bool g_f12_was_down = false;
std::wstring ini_path();
std::wstring game_dir();

void server_items(emf::World w) {
    if (g_dump_items && !g_items_dumped) {
        std::vector<EmfItemInfo> items;
        emf::for_each_item(w, [&items](const EmfItemInfo& i) { items.push_back(i); });
        if (!items.empty()) {  // the registry fills while the world loads
            g_items_dumped = true;
            const std::wstring path = game_dir() + L"\\emf\\items.csv";
            const std::string csv = sample::items_csv(items);
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"wb") != 0) f = nullptr;
            if (f) {
                fwrite(csv.data(), 1, csv.size(), f);
                fclose(f);
            }
            EMF_LOG(kMod, "wrote %zu items to emf\\items.csv%s", items.size(), f ? "" : " (FAILED to open the file)");
        }
    }
    if (!edge(VK_F12, g_f12_was_down)) return;
    wchar_t wide[96] = {};
    GetPrivateProfileStringW(L"sample_mod", L"give_item", L"", wide, 96, ini_path().c_str());
    std::string name;
    for (int i = 0; i < 95 && wide[i]; ++i) name += static_cast<char>(wide[i]);
    const UINT count = GetPrivateProfileIntW(L"sample_mod", L"give_count", 1, ini_path().c_str());
    const uint32_t player = emf::local_player();
    if (name.empty() || !player) {
        EMF_LOG(kMod, "F12: set give_item=<debug name> in emf\\config\\sample_mod.ini (see emf\\items.csv)");
        return;
    }
    const uint32_t pickup = emf::give_item(w, player, name.c_str(), count);
    EMF_LOG(kMod, "F12: give %u x '%s' to player %u: %s (pickup entity %u)", count, name.c_str(), player,
            pickup ? "dropped at the player" : "FAILED (unknown name or refused, see the core log)", pickup);
}

void on_tick(emf::World w) {
    const uint64_t now = GetTickCount64();
    if (w.is_client() && (g_last_client_ms == 0 || now - g_last_client_ms >= 5000)) {
        g_last_client_ms = now;
        client_report(w);
    }
    if (w.is_server()) {
        if (g_enable_write) {
            server_write_test(w);
            server_spawn_test(w);
            server_command_test(w);
            server_items(w);
        }
        if (g_last_server_ms == 0 || now - g_last_server_ms >= 10000) {
            g_last_server_ms = now;
            server_report(w);
        }
    }
}

std::wstring game_dir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

std::wstring ini_path() { return game_dir() + L"\\emf\\config\\sample_mod.ini"; }

bool read_enable_write() { return GetPrivateProfileIntW(L"sample_mod", L"enable_write", 0, ini_path().c_str()) != 0; }

// [sample_mod] spawn_guid=..., spawn_guid2=... (EML text form), guid_layout=0 (ms) or 1 (raw): a choice in mod.json.
// The game keeps GUIDs in Windows layout ("ms"): verified against the pickup template GUID in the exe.
void read_spawn_guids() {
    wchar_t layout[8] = {};
    GetPrivateProfileStringW(L"sample_mod", L"guid_layout", L"0", layout, 8, ini_path().c_str());
    const bool ms = sample::guid_layout_ms(layout);
    const wchar_t* keys[2] = {L"spawn_guid", L"spawn_guid2"};
    for (int slot = 0; slot < 2; ++slot) {
        wchar_t wide[64] = {};
        GetPrivateProfileStringW(L"sample_mod", keys[slot], L"", wide, 64, ini_path().c_str());
        char text[64] = {};
        for (int i = 0; i < 63 && wide[i]; ++i) text[i] = static_cast<char>(wide[i]);
        g_have_spawn_guid[slot] = sample::parse_guid(text, g_spawn_guid[slot], ms);
        if (text[0]) EMF_LOG(kMod, "%s %s (%s layout): %s", slot == 0 ? "spawn_guid" : "spawn_guid2", text,
                             ms ? "ms" : "raw", g_have_spawn_guid[slot] ? "ok" : "INVALID");
    }
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;
    g_enable_write = read_enable_write();
    g_dump_items = GetPrivateProfileIntW(L"sample_mod", L"dump_items", 0, ini_path().c_str()) != 0;
    EMF_LOG(kMod, "loaded (write test %s)",
            g_enable_write ? "ON: F8 = +3 h, F9/F11 spawn, F10 destroy, F7 villager come here, F12 give item" : "off");
    if (g_enable_write) {
        read_spawn_guids();
        install_come_here();
    }
    const bool hooked = api->hook_program && api->hook_program("client_player_state", count_program_call, nullptr, nullptr);
    EMF_LOG(kMod, "program hook on client_player_state: %s", hooked ? "requested" : "unavailable");
    const bool own = api->register_program &&
                     api->register_program("sample_mod_server_tick", EMF_WORLD_SERVER, own_server_program, nullptr);
    EMF_LOG(kMod, "own server program sample_mod_server_tick: %s", own ? "queued" : "unavailable");
    if (api->on_save && api->on_load) {
        api->on_save([](uint16_t slot, void*) {
            EMF_LOG(kMod, "world saved: slot index %u, %s", (slot & 0xFF) - 1u, (slot >> 8) ? "Steam" : "Local");
        }, nullptr);
        api->on_load([](uint16_t slot, void*) {
            EMF_LOG(kMod, "world loaded: slot index %u, %s", (slot & 0xFF) - 1u, (slot >> 8) ? "Steam" : "Local");
        }, nullptr);
    }
    if (api->size >= sizeof(EmfApi) && api->register_command) {
        const bool cmd = emf::add_command("sample.villagers", ": list the villagers with their AI state", villagers_command);
        EMF_LOG(kMod, "command sample.villagers: %s", cmd ? "added" : "refused");
    }
    emf::on_tick(on_tick);
    return true;
}
