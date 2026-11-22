// path_find (path_glue.h).
#include "core/path_glue.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "common/offsets.h"
#include "common/seh.h"
#include "core/path_grid.h"

namespace emf::core {

namespace {
constexpr size_t kJobCmdBuffer = 0x08;    // ProgramContext -> job; cmdbuf = *(job + 8)
constexpr size_t kResultBytes = 0x28;     // getPathQueryResult's out: points +0, count +8, door entities +0x10,
                                          // their count +0x18 (one u32 per point; private path_doors_2026-11-14.md
                                          // section 6), result +0x24
constexpr size_t kGamePointBytes = 0x10;  // float x, y, z, u8 move at +0xC
constexpr size_t kMaxGamePoints = 4096;   // read at most (the service keeps 512)

std::unique_ptr<PathService> g_service;
PathGlueDeps g_deps;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_last_run_ms{0};   // when emf_path last ran
std::atomic<uint64_t> g_waiting_since{0};  // when the watchdog first saw open queries with no run since
std::atomic<bool> g_watchdog_said{false};
std::atomic<uint64_t> g_steps_without_run{0};
std::atomic<bool> g_cb_fault_said{false};
// Game records still to destroy in this world: a destroy the game refused (a full command buffer), or queries the
// watchdog ended. The program tries them again each step; a new world forgets them (they went with the old one).
std::mutex g_destroy_lock;
std::vector<std::pair<uint64_t, uint32_t>> g_to_destroy;
void destroy_later(uint64_t id, uint32_t owner) {
    std::lock_guard<std::mutex> l(g_destroy_lock);
    if (g_to_destroy.size() < 4096) g_to_destroy.push_back({id, owner});
}

// path.door: the console's requests, answered by the program's next run (or by the watchdog when it does not run).
struct DoorAsk {
    uint32_t entity = 0;
    std::function<void(const std::string&)> reply;
};
std::mutex g_door_lock;
std::vector<DoorAsk> g_door_asks;
size_t door_asks_waiting() {
    std::lock_guard<std::mutex> l(g_door_lock);
    return g_door_asks.size();
}
// Hands each answer's lines to its reply after this step (the answer may hold two lines: one reply call each).
void deliver_doors(std::vector<std::pair<std::function<void(const std::string&)>, std::string>> answers) {
    if (answers.empty() || !g_deps.after_step) return;
    auto shared = std::make_shared<decltype(answers)>(std::move(answers));
    g_deps.after_step([shared](EmfWorld*) {
        for (const auto& [reply, text] : *shared) {
            if (!reply) continue;
            size_t start = 0;
            while (start <= text.size()) {
                const size_t nl = text.find('\n', start);
                reply(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        }
    });
}
void answer_doors(const uint8_t* grid) {
    std::vector<DoorAsk> asks;
    {
        std::lock_guard<std::mutex> l(g_door_lock);
        asks.swap(g_door_asks);
    }
    std::vector<std::pair<std::function<void(const std::string&)>, std::string>> answers;
    for (DoorAsk& a : asks)
        answers.push_back({std::move(a.reply), path_door_text(a.entity, read_path_door(grid, a.entity))});
    deliver_doors(std::move(answers));
}
void answer_doors_unread(const std::string& why) {
    std::vector<DoorAsk> asks;
    {
        std::lock_guard<std::mutex> l(g_door_lock);
        asks.swap(g_door_asks);
    }
    std::vector<std::pair<std::function<void(const std::string&)>, std::string>> answers;
    for (DoorAsk& a : asks)
        answers.push_back({std::move(a.reply), "path.door " + std::to_string(a.entity) + ": not answered (" + why + ")"});
    deliver_doors(std::move(answers));
}

// A mod's callback under SEH: one bad callback does not cost the other answers of the step.
bool guarded_cb(EmfPathCallback cb, EmfWorld* w, uint32_t id, const EmfPathResult* r, const EmfPathPoint* p, void* u) {
    __try {
        cb(w, id, r, p, u);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- the game calls under SEH (a fault turns the feature off) ----
bool call_defaults(void* (*fn)(void*), uint8_t* q) {
    __try {
        fn(q);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_create(uint32_t* (*fn)(void*, uint32_t*, const void*), void* cb, const uint8_t* q, uint32_t* id) {
    __try {
        fn(cb, id, q);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_destroy(bool (*fn)(void*, uint32_t, uint32_t), void* cb, uint32_t entity, uint32_t id, bool* ok) {
    __try {
        *ok = fn(cb, entity, id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_get(bool (*fn)(void*, void*, uint32_t, bool), void* ctx, uint8_t* out, uint32_t id, bool* ok) {
    __try {
        *ok = fn(ctx, out, id, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void fault(const char* what) {
    if (g_on.exchange(false) && g_deps.log)
        g_deps.log(std::string("path_find: off (the game's ") + what + " faulted; queries are refused until a restart)");
}

uint8_t* command_buffer(void* ctx) {
    uint8_t* job = nullptr;
    uint8_t* cb = nullptr;
    if (!seh_read(ctx, &job, 8) || !job || !seh_read(job + kJobCmdBuffer, &cb, 8)) return nullptr;
    return cb;
}

void deliver(std::vector<PathDone> done) {
    if (done.empty() || !g_deps.after_step) return;
    auto shared = std::make_shared<std::vector<PathDone>>(std::move(done));
    g_deps.after_step([shared](EmfWorld* w) {
        for (const PathDone& d : *shared) {
            const EmfPathResult r = d.view();
            if (d.cb && !guarded_cb(d.cb, w, d.id, &r, d.points.empty() ? nullptr : d.points.data(), d.user) &&
                !g_cb_fault_said.exchange(true) && g_deps.log)
                g_deps.log("path_find: a mod's path callback faulted (query " + std::to_string(d.id) +
                           "); the other answers still came");
        }
    });
}

void path_program(void* ctx, void*) {
    if (!g_on.load() || !g_service) return;
    uint8_t* world = program_context_world(ctx);
    uint8_t* cb = command_buffer(ctx);
    if (!world || !cb) return;  // not counted as a run: the watchdog answers what waits
    g_last_run_ms = g_deps.now_ms ? g_deps.now_ms() : GetTickCount64();
    g_steps_without_run = 0;
    if (door_asks_waiting()) {  // path.door: the grid of this storage (0 outside the server world's)
        const uint8_t* grid = nullptr;
        uint8_t* job = nullptr;
        if (!seh_read(ctx, &job, 8) || !job || !seh_read(job + off::kJobPathGrid, &grid, 8)) grid = nullptr;
        answer_doors(grid);
    }
    const PathFns& f = g_deps.fns;
    std::vector<std::pair<uint64_t, uint32_t>> retry;
    {
        std::lock_guard<std::mutex> l(g_destroy_lock);
        retry.swap(g_to_destroy);
    }
    for (const auto& [id, owner] : retry) {
        bool ok = false;
        if (!g_on.load()) break;
        if (!call_destroy(f.destroy, cb, owner, static_cast<uint32_t>(id), &ok)) fault("destroy path query");
        else if (!ok) destroy_later(id, owner);  // still full: next step
    }
    PathEngine e;
    e.create = [&](const EmfPathQuery& q, uint32_t owner, const PathCreature* as) -> uint64_t {
        if (!g_on.load()) return 0;  // a game call faulted earlier in this step: no more calls
        alignas(16) uint8_t bytes[kPathQueryBytes] = {};
        if (!call_defaults(f.defaults, bytes)) {
            fault("query defaults");
            return 0;
        }
        fill_path_query(bytes, q, owner, as);
        uint32_t id = 0;
        if (!call_create(f.create, cb, bytes, &id)) {
            fault("create path query");
            return 0;
        }
        return id;
    };
    e.poll = [&](uint64_t id) {
        PathAnswer a;
        if (!g_on.load()) return a;
        alignas(8) uint8_t out[kResultBytes] = {};
        bool ok = false;
        if (!call_get(f.get, ctx, out, static_cast<uint32_t>(id), &ok)) {
            fault("path query result");
            return a;
        }
        if (!ok) return a;
        a.done = true;
        a.game_result = out[0x24];
        const uint8_t* pts = nullptr;
        uint64_t count = 0;
        std::memcpy(&pts, out, 8);
        std::memcpy(&count, out + 8, 8);
        if (!pts || !count) return a;
        const size_t n = static_cast<size_t>(count > kMaxGamePoints ? kMaxGamePoints : count);
        std::vector<uint8_t> raw(n * kGamePointBytes);
        if (!seh_read(pts, raw.data(), raw.size())) return a;  // no points: the answer stands, empty
        a.points.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* p = raw.data() + i * kGamePointBytes;
            std::memcpy(&a.points[i].x, p, 12);
            a.points[i].move = p[0xC];
        }
        const uint8_t* door_ids = nullptr;
        uint64_t door_count = 0;
        std::memcpy(&door_ids, out + 0x10, 8);
        std::memcpy(&door_count, out + 0x18, 8);
        if (door_ids && door_count == count) {  // one per point, or not trusted
            a.doors.resize(n);
            if (!seh_read(door_ids, a.doors.data(), n * sizeof(uint32_t))) a.doors.clear();
            for (size_t i = 0; i < a.doors.size(); ++i)  // only a door point names a door (an old array is not trusted)
                if (a.points[i].move != 5) a.doors[i] = 0;
        }
        return a;
    };
    e.destroy = [&](uint64_t id, uint32_t owner) {
        if (!g_on.load()) return;
        bool ok = false;
        if (!call_destroy(f.destroy, cb, owner, static_cast<uint32_t>(id), &ok)) fault("destroy path query");
        else if (!ok) destroy_later(id, owner);  // the command buffer was full: tried again next step
    };
    e.exists = [&](uint32_t entity) {
        return g_deps.component && g_deps.component(world, off::kHashCurrentTransform, entity) != nullptr;
    };
    deliver(g_service->step(e, g_deps.now_ms ? g_deps.now_ms() : GetTickCount64()));
    if (!g_on.load()) deliver(g_service->world_reset());  // a fault: what is still open is answered now
}
}  // namespace

ProgramSpec path_setup(PathGlueDeps deps) {
    g_deps = std::move(deps);
    g_service = std::make_unique<PathService>();
    g_on = g_deps.fns.create && g_deps.fns.destroy && g_deps.fns.get && g_deps.fns.defaults;
    g_last_run_ms = 0;
    g_waiting_since = 0;
    g_watchdog_said = false;
    g_steps_without_run = 0;
    g_cb_fault_said = false;
    {
        std::lock_guard<std::mutex> l(g_destroy_lock);
        g_to_destroy.clear();
    }
    {
        std::lock_guard<std::mutex> l(g_door_lock);
        g_door_asks.clear();
    }
    ProgramSpec s;
    s.name = kPathProgramName;
    s.world_kind = EMF_WORLD_SERVER;
    s.fn = path_program;
    s.user = nullptr;
    return s;
}

PathService* path_service() { return g_on.load() ? g_service.get() : nullptr; }

void path_world_reset() {
    g_watchdog_said = false;
    g_waiting_since = 0;
    g_steps_without_run = 0;
    {
        std::lock_guard<std::mutex> l(g_destroy_lock);
        g_to_destroy.clear();
    }
    if (g_service) deliver(g_service->world_reset());
    answer_doors_unread("the world ended");  // not read from the next world's grid (entity ids come again)
}

void path_server_tick(uint64_t now_ms) {
    if (!g_service || (!g_service->open() && !door_asks_waiting())) {
        g_waiting_since = 0;
        g_steps_without_run = 0;
        return;
    }
    if (!g_waiting_since.load()) g_waiting_since = now_ms;  // the first step that sees open queries
    const uint64_t steps = ++g_steps_without_run;
    const uint64_t last = g_last_run_ms.load();
    const uint64_t since = (std::max)(last, g_waiting_since.load());
    if (steps < kPathWatchdogSteps || now_ms < since || now_ms - since < kPathWatchdogMs) return;
    g_steps_without_run = 0;
    if (!g_watchdog_said.exchange(true) && g_deps.log)
        g_deps.log(std::string("path_find: the ") + kPathProgramName +
                   " program is not running in this world; open queries end as cancelled");
    g_waiting_since = 0;
    std::vector<std::pair<uint64_t, uint32_t>> created;
    std::vector<PathDone> done = g_service->world_reset(&created);
    for (const auto& [id, owner] : created) destroy_later(id, owner);  // the game still has them: destroyed when it runs
    deliver(std::move(done));
    answer_doors_unread("the emf_path program is not running in this world");
}

void path_stop() { g_on = false; }

std::string path_door_ask(uint32_t entity, std::function<void(const std::string&)> reply) {
    if (!g_deps.grid_layout) return "path.door: not available on this game build";
    if (!g_on.load() || !g_service) return "path.door: not available (path_find is off; see the log)";
    std::lock_guard<std::mutex> l(g_door_lock);
    if (g_door_asks.size() >= kMaxDoorAsks)
        return "path.door: " + std::to_string(kMaxDoorAsks) + " are waiting for the path grid (try again in a moment)";
    g_door_asks.push_back({entity, std::move(reply)});
    return {};
}

}  // namespace emf::core
