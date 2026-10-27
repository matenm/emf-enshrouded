#include "runtime.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "common/log.h"
#include "common/log_limiter.h"
#include "common/log_ring.h"
#include "common/offsets.h"
#include "common/seh.h"
#include "common/world_view.h"
#include "console_runner.h"
#include "core/api.h"
#include "core/daytime.h"
#include "core/menu_start.h"
#include "core/overlay/injected_keys.h"
#include "core/overlay/input_hook.h"
#include "core/overlay/overlay.h"
#include "core/overlay/submit.h"
#include "core/overlay/vk_capture.h"
#include "core/overlay/vk_overlay.h"
#include "core/version.h"
#include "emf/generated/components.hpp"
#include "pipe_server.h"
#include "protocol.h"
#include "runtime_parts.h"
#include "screens.h"
#include "service.h"
#include "state.h"

namespace emf::core::bridge {
namespace {

namespace gen = emf::gen;
constexpr double kFixed = 4294967296.0;  // CurrentTransform's fixed-point metres (as in builtin_commands.cpp)

std::string utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

struct StepStats {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> interval_us{0};  // smoothed time between steps
    LARGE_INTEGER last{};                  // that world's thread only
};

struct Runtime {
    RuntimeDeps deps;
    PipeServer pipe;
    std::unique_ptr<Service> service;
    InjectedKeys keys;
    std::atomic<InjectedKeyHooks> key_hooks{InjectedKeyHooks::None};  // set after the pipe thread started
    std::wstring pipe_name;
    bool verbose = false;                   // [debug] bridge_verbose=1: log every client's connect and leave
    std::atomic<uint64_t> connections{0};
    std::mutex refused_lock;
    emf::LogLimiter refused_log{60000};     // a refused client is logged at most once a minute per reason
    StepStats steps[3];
    // what the client world's tick reads (every 250 ms): the local player and its health (NetworkHealth, the
    // replicated copy the game's own HUD shows)
    std::atomic<uint32_t> local_player{0};
    std::atomic<bool> health_known{false};
    std::atomic<uint32_t> health{0}, health_max{0};
    uint64_t player_read_ms = 0;            // client world thread
    SlotBook slots;                         // the save slot of the hosted world (forgotten when a new one sets up)
    StateBatch states;                      // state requests waiting for the world's values
    PressBook presses;                      // mod_key presses waiting for their release
    std::atomic<uint64_t> quit_at{0};       // quit: when to ask the game to close (after the reply went out)
};

Runtime* g_rt = nullptr;  // lives for the process once started (never freed)
std::atomic<uint64_t> g_setup_ms{0};
wchar_t g_info_path[MAX_PATH] = {};  // emf\bridge.txt while the bridge runs

// Removes emf\bridge.txt when the process ends normally (the CRT runs this as emf_core.dll detaches). A game that is
// killed leaves it behind: clients check that its pid still runs.
struct InfoFileCleanup {
    ~InfoFileCleanup() {
        if (g_info_path[0]) DeleteFileW(g_info_path);
    }
} g_info_cleanup;

// ---- what the bridge reads from the game ----

void on_tick(EmfWorld* w, void*) {
    Runtime* rt = g_rt;
    const uint8_t kind = WorldView(reinterpret_cast<uint8_t*>(w)).kind();
    if (!rt || kind > 2) return;
    StepStats& s = rt->steps[kind];
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (s.count.fetch_add(1, std::memory_order_relaxed) && freq.QuadPart) {
        const double us = static_cast<double>(now.QuadPart - s.last.QuadPart) * 1e6 / static_cast<double>(freq.QuadPart);
        const double old = static_cast<double>(s.interval_us.load(std::memory_order_relaxed));
        s.interval_us.store(static_cast<uint64_t>(old ? old * 0.9 + us * 0.1 : us), std::memory_order_relaxed);
    }
    s.last = now;
    if (kind == EMF_WORLD_CLIENT) {  // the client world knows the local player (LocalPlayerData) and its health
        const uint64_t ms = GetTickCount64();
        if (ms - rt->player_read_ms >= 250) {
            rt->player_read_ms = ms;
            const EmfApi* api = api_table();
            const uint32_t player = api->local_player();
            rt->local_player = player;
            uint64_t size = 0;
            const void* h = player ? api->get_component(w, gen::ecs::NetworkHealth::kHash, player, &size) : nullptr;
            gen::ecs::NetworkHealth nh{};
            const bool known = h && size == sizeof nh && seh_read(h, &nh, sizeof nh);
            if (known) {
                rt->health = nh.health;
                rt->health_max = nh.healthMax;
            }
            rt->health_known = known;
        }
    }
}

bool world_running() {
    const uint64_t now = GetTickCount64();
    const WorldTracker& t = g_rt->deps.state->tracker;
    return t.latest(EMF_WORLD_SERVER, now) || t.latest(EMF_WORLD_CLIENT, now);
}

// What the bridge knows without touching a world.
StateSnapshot base_snapshot() {
    CoreState* st = g_rt->deps.state;
    StateSnapshot s;
    if (GetTickCount64() - g_setup_ms.load(std::memory_order_relaxed) < 2000) s.world_status = "loading";
    // the selected character, as the pregame state hook last read it on the game's own thread (no read here)
    if (st->exe.server) {
        s.character.reset();
    } else {
        CharacterSelection c = current_character();
        s.character = StateSnapshot::Character{c.storage, c.slot, c.name};
    }
    const OverlayStatus ov = overlay_status();
    s.overlay_running = ov.running;
    s.console_open = ov.console_open;
    s.wheel_open = ov.wheel_open;
    s.hud = st->hud.lines();
    s.mods = st->mods.list();
    if (st->hooks)
        for (const auto& e : st->hooks->list()) {
            ++s.hooks_total;
            ++(e.owner == "emf" ? s.hooks_emf : s.hooks_mods);
        }
    auto steps = [](const StepStats& x) -> std::optional<StateSnapshot::Steps> {
        const uint64_t n = x.count.load(std::memory_order_relaxed);
        if (!n) return std::nullopt;
        StateSnapshot::Steps out;
        out.count = n;
        const uint64_t us = x.interval_us.load(std::memory_order_relaxed);
        if (us) out.interval_ms = static_cast<double>(us) / 1000.0;
        return out;
    };
    s.server_steps = steps(g_rt->steps[EMF_WORLD_SERVER]);
    s.client_steps = steps(g_rt->steps[EMF_WORLD_CLIENT]);
    s.world_slot = g_rt->slots.current(std::nullopt);  // hidden again below while the world is someone else's
    if (!st->exe.server) {
        const VulkanFrameStats f = vulkan_frame_stats();
        if (f.device) {
            s.presents = f.presents;
            if (f.recent && f.frame_ms > 0) s.frame_ms = f.frame_ms;
        }
    }
    return s;
}

// The world job: only the world's own values, on that world's thread (posted once per step for all waiting requests).
void read_world(EmfWorld* w, void*) {
    Runtime* rt = g_rt;
    if (!rt) return;
    CoreState* st = rt->deps.state;
    const EmfApi* api = api_table();
    const WorldView view(reinterpret_cast<uint8_t*>(w));
    WorldValues v;
    v.kind = st->exe.server ? "dedicated" : view.kind() == EMF_WORLD_SERVER ? "server" : "client";
    if (st->tick_offsets_ok) {
        v.tick = view.tick();
        v.ecs_time_s = static_cast<double>(api->ecs_time_ns(w)) / 1e9;
    }
    if (const auto* d = static_cast<const uint8_t*>(view.singleton("g38_daytime::Daytime"))) {
        int64_t tod = -1;
        if (seh_read(d + kDaytimeTimeOfDay, &tod, sizeof tod) && tod >= 0 && tod <= kDayNs)
            v.time_of_day_h = static_cast<double>(tod) / 3600e9;
    }
    if (const uint32_t player = rt->local_player.load()) {
        uint64_t size = 0;
        const void* t = api->get_component(w, off::kHashCurrentTransform, player, &size);
        int64_t p[3];
        if (t && size >= sizeof p && seh_read(t, p, sizeof p))
            v.position = std::array<double, 3>{p[0] / kFixed, p[1] / kFixed, p[2] / kFixed};
        // a world this game hosts: the player has no NetworkHealth there, but the server's Health component holds the
        // current health in data slot 0 (confirmed in game: writing 0 there kills through the game's death program)
        if (view.kind() == EMF_WORLD_SERVER) {
            size = 0;
            const void* h = api->get_component(w, gen::ecs::Health::kHash, player, &size);
            gen::ecs::Health hp{};
            if (h && size == sizeof hp && seh_read(h, &hp, sizeof hp)) v.health_slot0 = hp.dataStorage[0];
            // the attributes as the game reads them (API v18), with the max the slot does not give
            int32_t cur = 0, max = 0;
            if (api->attribute_get(w, player, EMF_ATTR_HEALTH, &cur) && api->attribute_get(w, player, EMF_ATTR_MAX_HEALTH, &max))
                v.health_attributes = std::make_pair(cur, max);
        }
    }
    if (view.kind() == EMF_WORLD_SERVER && !st->exe.server) v.world_name = session_world_name();
    rt->states.fill(v);
    rt->pipe.wake();  // the pipe thread builds the replies
}

// The state's JSON, on the pipe thread: what EMF tracks, and the world job's values.
std::string state_with_world(const WorldValues* v) {
    StateSnapshot s = base_snapshot();
    if (v) {
        s.world_status = "running";
        s.world_kind = v->kind;
        s.world_slot = g_rt->slots.current(s.world_kind);
        s.world_tick = v->tick;
        s.ecs_time_s = v->ecs_time_s;
        s.time_of_day_h = v->time_of_day_h;
        s.position = v->position;
        s.world_name = v->world_name;
    }
    // the player only while a world runs (the tracked id stays from the last world otherwise)
    const uint32_t player = v ? g_rt->local_player.load() : 0;
    if (player) {
        s.player_id = player;
        std::optional<std::pair<uint32_t, uint32_t>> network;
        if (g_rt->health_known) network = std::make_pair(g_rt->health.load(), g_rt->health_max.load());
        s.health = pick_health(network, v->health_attributes, v->health_slot0);
    } else {
        s.position.reset();
    }
    return state_json(s);
}

void state_source(std::function<void(std::string)> done) {
    CoreState* st = g_rt->deps.state;
    const uint64_t now = GetTickCount64();
    const bool server = st->tracker.latest(EMF_WORLD_SERVER, now) != nullptr;
    const bool client = st->tracker.latest(EMF_WORLD_CLIENT, now) != nullptr;
    if (!server && !client) {
        done(state_with_world(nullptr));
        return;
    }
    // one job per step for every waiting request (Dispatcher::run guards it); a world that stops first never runs it,
    // and the bridge answers "did not answer" after 2 s
    if (g_rt->states.add(std::move(done), now))
        st->dispatcher.post(server ? EMF_WORLD_SERVER : EMF_WORLD_CLIENT, read_world, nullptr);
}

Refusal shot_start(const ShotOptions& opt, std::function<void(const ShotResult&)> done) {
    if (g_rt->deps.state->exe.server) return Refusal{code::kUnavailable, "the dedicated server has no screen"};
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::wstring dir = g_rt->deps.emf_dir;
    std::wstring path;
    for (unsigned n = 1; n < 100; ++n) {
        path = dir + L"\\screens\\" + screenshot_name(t, n);
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    CaptureOptions copt;
    copt.overlay = opt.overlay;
    copt.max_width = opt.max_width;
    std::string code;
    const std::string why = capture_request(dir, path, copt, [done](const CaptureResult& r) {
        ShotResult s;
        s.ok = r.ok;
        s.code = r.code;
        s.error = r.error;
        s.path = utf8(r.path);
        s.width = r.width;
        s.height = r.height;
        s.hdr = r.hdr;
        done(s);
    }, &code);
    if (why.empty()) return {};
    return Refusal{code.empty() ? code::kUnavailable : code, why};
}

Refusal mod_key(uint32_t vk, KeyAction action, uint32_t hold_ms, std::function<void(const KeyResult&)> done) {
    Runtime& rt = *g_rt;
    if (rt.deps.state->exe.server)
        return Refusal{code::kUnavailable, "the dedicated server has no game window: mods' keys do not work there"};
    if (rt.key_hooks == InjectedKeyHooks::None)
        return Refusal{code::kUnavailable, "mod_key is off: EMF could not hook the key-state functions"};
    return mod_key_action(rt.keys, rt.presses, vk, action, hold_ms, std::move(done), GetTickCount64());
}

Refusal quit_source() {
    Runtime& rt = *g_rt;
    if (rt.deps.state->exe.server)
        return Refusal{code::kUnavailable, "the dedicated server is stopped in its own window, not through the bridge"};
    if (!game_main_window()) return Refusal{code::kUnavailable, "the game's window was not found"};
    rt.quit_at = GetTickCount64() + 300;  // the reply goes out first
    return {};
}

// load_world: the main-menu hooks start a LOCAL save offline (src/core/menu_start.h); this only hands the request over.
Refusal load_world_source(const LoadWorldParams& p, std::function<void(const LoadWorldReply&)> done) {
    Runtime& rt = *g_rt;
    if (rt.deps.state->exe.server) return Refusal{code::kUnavailable, "the dedicated server has no main menu"};
    if (world_running())  // refused at once: the menu's own check would only come in the next UI frame
        return Refusal{code::kUnavailable, "not at the main menu: a world is running (quit to the menu first)"};
    MenuStart* menu = menu_start();
    if (!menu)
        return Refusal{code::kUnavailable,
                       "load_world is off: " + (rt.deps.load_world_off.empty() ? std::string("the main-menu hooks are not "
                                                                                             "installed")
                                                                               : rt.deps.load_world_off)};
    LoadWorldRequest r;
    r.world = p.slot;
    r.character = p.character;
    r.expect_name = p.expect_name;
    r.timeout_ms = p.timeout_ms;
    r.test_refuse = p.test_refuse;
    LoadWorldResult refused;
    const bool ok = menu->request(r, [done](const LoadWorldResult& res) {
        LoadWorldReply reply;
        reply.ok = res.ok;
        reply.code = res.code;
        reply.error = res.error;
        reply.slot = res.world;
        reply.character = res.character;
        reply.world = res.world_name;
        reply.character_name = res.character_name;
        reply.ms = res.ms;
        done(reply);
    }, &refused);
    if (!ok) return Refusal{refused.code.empty() ? code::kRefused : refused.code, refused.error};
    rt.pipe.wake();
    return {};
}

// The pipe thread's poll: presses whose time is up are answered, finished state requests get their JSON, lines queued
// for a world that is gone are answered, a quit is carried out; then the service's timeouts and pushes.
void runtime_poll() {
    Runtime& rt = *g_rt;
    const uint64_t now = GetTickCount64();
    release_due(rt.keys, rt.presses, now);
    for (auto& [done, values] : rt.states.take_ready()) done(state_with_world(&values));
    CoreState* st = rt.deps.state;
    if (st->commands.pending()) answer_lines_without_world(st->commands, host_state(st->tracker, now), rt.deps.command_sink);
    if (const uint64_t at = rt.quit_at.load(); at && now >= at) {
        rt.quit_at = 0;
        if (HWND w = game_main_window()) {
            rt.deps.log("debug bridge: quit: asking the game to save and close");
            PostMessageW(w, WM_CLOSE, 0, 0);
        }
    }
    if (MenuStart* menu = menu_start()) menu->poll();  // a load_world whose time is up, even without UI frames
    rt.service->poll();
}

// ---- game events (relay callbacks run on the server world's thread after its step) ----

void on_pickup(EmfWorld* w, const void* e, uint32_t size, void*) {
    if (!g_rt || size < sizeof(gen::ecs::PickupEvent) || !g_rt->service->wants("pickup")) return;
    gen::ecs::PickupEvent p;
    std::memcpy(&p, e, sizeof p);
    std::string name;
    EmfItemInfo info{};
    info.size = sizeof info;
    if (api_table()->item_info(w, p.itemId.value, &info)) name.assign(info.name, strnlen(info.name, sizeof info.name));
    g_rt->service->publish("pickup", pickup_fields(p.itemId.value, name, p.itemCount, p.pickedUpByEntityId.id,
                                                   p.pickedUpFromEntityId.id, {p.position.x, p.position.y, p.position.z}));
}

void on_died(EmfWorld*, const void* e, uint32_t size, void*) {
    if (!g_rt || size < sizeof(gen::ecs::EntityDiedEvent) || !g_rt->service->wants("kill")) return;
    gen::ecs::EntityDiedEvent d;
    std::memcpy(&d, e, sizeof d);
    g_rt->service->publish("kill", kill_fields(d.deadEntityId.id));
}

void on_hit(EmfWorld*, const void* e, uint32_t size, void*) {
    if (!g_rt || size < sizeof(gen::ecs::HitEvent) || !g_rt->service->wants("damage")) return;
    gen::ecs::HitEvent h;
    std::memcpy(&h, e, sizeof h);
    g_rt->service->publish("damage", damage_fields(h.targetId.id, h.rootSourceId.id, h.healthChange, h.staminaChange));
}

void subscribe_events(Runtime& rt) {
    CoreState* st = rt.deps.state;
    auto sub = [&](const char* event, uint32_t size, RelayCallback cb) {
        std::string why;
        if (!st->relay.subscribe(EMF_WORLD_SERVER, event, size, cb, nullptr, &why))
            rt.deps.log(std::string("debug bridge: the game event ") + event + " is unavailable (" + why + ")");
    };
    sub("PickupEvent", sizeof(gen::ecs::PickupEvent), on_pickup);
    sub("EntityDiedEvent", sizeof(gen::ecs::EntityDiedEvent), on_died);
    sub("HitEvent", sizeof(gen::ecs::HitEvent), on_hit);
    st->save_events.subscribe([](uint16_t slot, void*) {
        if (!g_rt) return;
        g_rt->slots.saved(slot);
        g_rt->service->publish("save", slot_fields(slot));
    }, nullptr);
    st->load_events.subscribe([](uint16_t slot, void*) {
        if (!g_rt) return;
        g_rt->slots.loaded(slot);
        g_rt->service->publish("load", slot_fields(slot));
    }, nullptr);
}

bool write_info_file(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr) && n == text.size();
    CloseHandle(h);
    return ok;
}

}  // namespace

void note_world_setup(uint64_t now_ms) { g_setup_ms.store(now_ms, std::memory_order_relaxed); }

void note_session_start() {
    if (g_rt) g_rt->slots.new_session();
}

bool start_debug_bridge(RuntimeDeps deps) {
    if (!deps.log) deps.log = [](const std::string&) {};
    if (g_rt || !deps.state) return false;
    auto* rt = new Runtime();  // lives for the process
    rt->deps = std::move(deps);
    CoreState* st = rt->deps.state;
    const uint32_t pid = GetCurrentProcessId();
    rt->pipe_name = bridge_pipe_name(pid);
    emf::log_ring_enable(5000);  // normally on since the log opened; a later call keeps that ring

    Sources src;
    src.hello = HelloInfo{version(), EMF_API_VERSION, st->exe.build_number, st->exe.server, pid};
    src.now_ms = [] { return GetTickCount64(); };
    src.console = [rt](const std::string& text, std::function<void(const std::string&)> line,
                       std::function<void(const std::string&)> done) {
        CoreState* s = rt->deps.state;
        const uint64_t now = GetTickCount64();
        run_console_line(s->commands, host_state(s->tracker, now), text, line, std::move(done), rt->deps.command_sink, now);
    };
    src.state = state_source;
    src.mods = [rt] { return mods_json(rt->deps.state->mods.list()); };
    src.log = emf::log_ring();
    src.world_running = world_running;
    src.player_exists = [rt] { return world_running() && rt->local_player.load() != 0; };
    src.shot_start = shot_start;
    src.shot_cancel = [] { return capture_cancel(); };
    src.mod_key = mod_key;
    src.quit = quit_source;
    src.load_world = load_world_source;
    src.event_names = game_event_names();
    Output out;
    out.send = [rt](ClientId c, std::string line, bool droppable) { rt->pipe.send(c, std::move(line), droppable); };
    out.disconnect = [rt](ClientId c) { rt->pipe.disconnect(c); };
    rt->service = std::make_unique<Service>(std::move(src), std::move(out));

    PipeHandler h;
    rt->verbose = GetPrivateProfileIntW(L"debug", L"bridge_verbose", 0, (rt->deps.emf_dir + L"\\emf_core.ini").c_str()) == 1;
    h.connected = [rt](ClientId c) {
        rt->service->on_connect(c);
        const uint64_t n = ++rt->connections;
        if (rt->verbose)
            rt->deps.log("debug bridge: client " + std::to_string(c) + " connected");
        else if (n == 1)
            rt->deps.log("debug bridge: the first client connected (later ones are not logged; [debug] "
                         "bridge_verbose=1 logs each)");
    };
    h.refused = [rt](const std::string& why) {
        std::string line;
        {
            const std::lock_guard<std::mutex> l(rt->refused_lock);
            line = rt->refused_log.pass("debug bridge: refused a client: " + why, GetTickCount64());
        }
        if (!line.empty()) rt->deps.log(line);
    };
    h.stopped = [rt](const std::string& why) {
        rt->deps.log("debug bridge: stopped (" + why + ")");
        overlay_set_status("debug bridge stopped");
        if (g_info_path[0]) DeleteFileW(g_info_path);
    };
    h.line = [rt](ClientId c, std::string_view line) { rt->service->on_line(c, line); };
    h.too_long = [rt](ClientId c) { rt->service->on_too_long(c); };
    h.disconnected = [rt](ClientId c) {
        rt->service->on_disconnect(c);
        if (rt->verbose) rt->deps.log("debug bridge: client " + std::to_string(c) + " left");
    };
    h.poll = runtime_poll;
    const std::wstring user = current_user_sid();
    if (user.empty()) {
        rt->deps.log("debug bridge: off (this user's SID could not be read)");
        return false;
    }
    g_rt = rt;  // the pipe thread's poll and the callbacks below read it
    std::string why;
    if (!rt->pipe.start(rt->pipe_name, pipe_sddl(user, current_integrity_sid()), h, kMaxLineIn, kMaxClients, &why)) {
        rt->deps.log("debug bridge: off (" + why + ")");
        g_rt = nullptr;
        return false;
    }
    if (emf::LogRing* ring = emf::log_ring()) ring->set_notify(rt->pipe.wake_event());  // log followers get lines soon
    if (!st->exe.server) rt->key_hooks = install_injected_keys(&rt->keys);
    subscribe_events(*rt);
    st->dispatcher.subscribe_tick(on_tick, nullptr);
    const std::wstring info = rt->deps.emf_dir + L"\\bridge.txt";
    const bool info_ok = info.size() < MAX_PATH && write_info_file(info, bridge_info_text(rt->pipe_name, pid));
    if (info_ok) wcscpy_s(g_info_path, info.c_str());
    overlay_set_status("debug bridge on");
    std::string keys;
    if (!st->exe.server && rt->key_hooks == InjectedKeyHooks::NeedFocus)
        keys = "; mod_key works only while the game window is in front";
    else if (!st->exe.server && rt->key_hooks == InjectedKeyHooks::None)
        keys = "; mod_key is off (the key-state hooks failed)";
    rt->deps.log("debug bridge: on, pipe " + utf8(rt->pipe_name) + (info_ok ? " (emf\\bridge.txt)" : " (emf\\bridge.txt could not be written)") +
                 "; this Windows user only, up to " + std::to_string(kMaxClients) + " clients" + keys);
    return true;
}

}  // namespace emf::core::bridge
