#include "teleport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "common/seh.h"
#include "common/signatures.h"

namespace emf::core {
namespace {
namespace gen = emf::gen::ecs;
constexpr double kFixedOne = 4294967296.0;

bool is_player(uint32_t entity) { return entity >= 1 && entity <= 1023; }

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}
std::string where(const double p[3]) { return "(" + num(p[0]) + ", " + num(p[1]) + ", " + num(p[2]) + ")"; }

// A component of `entity` when the game's size is the generated struct's (else nullptr; *wrong_size set).
template <class T>
const T* comp(GetComponentFn get, uint8_t* world, uint32_t entity, bool* wrong_size = nullptr) {
    const ComponentRef r = get_component(get, world, T::kHash, entity);
    if (!r.ptr) return nullptr;
    if (r.size != sizeof(T)) {
        if (wrong_size) *wrong_size = true;
        return nullptr;
    }
    return static_cast<const T*>(r.ptr);
}

bool read_transform(GetComponentFn get, uint8_t* world, uint32_t entity, emf::gen::WorldTransform* out) {
    const auto* ct = comp<gen::CurrentTransform>(get, world, entity);
    return ct && seh_read(ct, out, sizeof *out);
}

void position_of(const emf::gen::WorldTransform& t, double out[3]) {
    out[0] = from_fixed(t.position.x);
    out[1] = from_fixed(t.position.y);
    out[2] = from_fixed(t.position.z);
}

// ---- guarded engine calls (no C++ objects with destructors in these frames) ----
enum class Push { Ok, Full, Fault };
Push push_guarded(QueuePushFn fn, uint8_t* cb, uint8_t kind, uint32_t size, uint32_t entity, const uint8_t* image) {
    __try {
        uint8_t* rec = fn(cb, cb + kCmdBufQueue, kind, size, entity);
        if (!rec) return Push::Full;
        std::memcpy(rec + kRecHashes, image + kRecHashes, size - kRecHashes);
        return Push::Ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return Push::Fault;
    }
}
Push push(QueuePushFn fn, uint8_t* cb, uint8_t kind, uint32_t entity, const std::vector<uint8_t>& image) {
    return push_guarded(fn, cb, kind, static_cast<uint32_t>(image.size()), entity, image.data());
}

// The running job's command buffer: cmdbuf = *(*ctx + 8), with a capacity and a fill that make sense.
uint8_t* command_buffer(void* ctx) {
    uint8_t* job = nullptr;
    uint8_t* cb = nullptr;
    uint64_t cap = 0;
    uint32_t used = 0;
    if (!seh_read(ctx, &job, 8) || !job || !seh_read(job + kJobCmdBuf, &cb, 8) || !cb ||
        !seh_read(cb + kCmdBufCapacity, &cap, 8) || !seh_read(cb + kCmdBufUsed, &used, 4))
        return nullptr;
    if (cap == 0 || cap > (uint64_t(1) << 30) || used > cap) return nullptr;
    return cb;
}

void teleport_program(void* ctx, void* user) { static_cast<TeleportService*>(user)->run(ctx); }
}  // namespace

bool teleport_layouts_checked(int build) {
    if (build < 0 || build >= static_cast<int>(kKnownBuildCount)) return false;
    const SigDef* d = find_signature("QueuePush");
    const uint32_t rva = d ? d->rva[static_cast<size_t>(build)] : 0;
    return rva != 0 && rva != kRvaUnchecked;
}

int64_t to_fixed(double metres) { return static_cast<int64_t>(std::llround(metres * kFixedOne)); }
double from_fixed(int64_t fixed) { return static_cast<double>(fixed) / kFixedOne; }

emf::gen::Quaternion yaw_quaternion(float yaw_deg) {
    const double half = static_cast<double>(yaw_deg) * 3.14159265358979323846 / 360.0;
    emf::gen::Quaternion q{};
    q.x = 0.0f;
    q.y = static_cast<float>(std::sin(half));
    q.z = 0.0f;
    q.w = static_cast<float>(std::cos(half));
    return q;
}

gen::Teleport build_teleport(double x, double y, double z, const emf::gen::WorldTransform& current, bool face_yaw,
                             float yaw_deg, uint32_t spawn_point, bool ground) {
    gen::Teleport t;
    std::memset(&t, 0, sizeof t);
    t.targetTransform.position.x = to_fixed(x);
    t.targetTransform.position.y = to_fixed(y);
    t.targetTransform.position.z = to_fixed(z);
    t.targetTransform.orientation = face_yaw ? yaw_quaternion(yaw_deg) : current.orientation;
    t.targetTransform.scale = current.scale;
    t.targetEntityId.id = spawn_point;
    t.searchBestSpawnPosition = ground;
    return t;
}

std::vector<uint8_t> add_teleport_record(const gen::Teleport& t) {
    std::vector<uint8_t> r(kAddTeleportSize, 0);
    const uint32_t hash = gen::Teleport::kHash;
    const uint16_t size = static_cast<uint16_t>(sizeof t);
    std::memcpy(r.data() + kRecHashes, &hash, 4);
    std::memcpy(r.data() + kRecSizes, &size, 2);
    std::memcpy(r.data() + kRecData, &t, sizeof t);
    return r;
}

std::vector<uint8_t> remove_teleport_record() {
    std::vector<uint8_t> r(kRemoveSize, 0);
    const uint32_t hash = gen::Teleport::kHash;
    std::memcpy(r.data() + kRecHashes, &hash, 4);
    return r;
}

std::vector<uint8_t> checkpoint_record(uint32_t player) {
    std::vector<uint8_t> r(kCheckpointSize, 0);
    std::memcpy(r.data() + kRecHashes, &player, 4);
    return r;
}

std::string check_request_numbers(const TeleportRequest& r) {
    if (!r.entity) return "entity 0";
    if (r.flags & ~kTpKnownFlags) return "unknown flags (a newer mod?)";
    if ((r.flags & kTpFaceYaw) && !std::isfinite(r.yaw_deg)) return "the yaw is not a finite number";
    if (r.spawn_point) {
        if (r.spawn_point == r.entity) return "an entity cannot be its own spawn point";
        return "";  // x/y/z are not used
    }
    if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.z)) return "a coordinate is not a finite number";
    if (r.x < kWorldMinXZ || r.x > kWorldMaxXZ || r.z < kWorldMinXZ || r.z > kWorldMaxXZ)
        return "x and z must be 0-10240 m (the world)";
    if (r.y < kWorldMinY || r.y > kWorldMaxY) return "y must be -1000-5000 m";
    return "";
}

StateCheck check_teleport_state(GetComponentFn get, uint8_t* world, const TeleportRequest& r, const HealthFn& health) {
    StateCheck s;
    const std::string e = std::to_string(r.entity);
    auto no = [&s](StateCheck::Verdict v, const char* cause, std::string why) {
        s.verdict = v;
        s.cause = cause;
        s.why = std::move(why);
        return s;
    };
    bool wrong = false;
    const auto* ct = comp<gen::CurrentTransform>(get, world, r.entity, &wrong);
    const auto* lt = comp<gen::LastTransform>(get, world, r.entity, &wrong);
    const auto* tc = comp<gen::TeleportCount>(get, world, r.entity, &wrong);
    if (wrong)
        return no(StateCheck::Refuse, "component sizes",
                  "the game's CurrentTransform, LastTransform or TeleportCount has another size than EMF knows (a game "
                  "update?)");
    if (!ct) return no(StateCheck::Refuse, "no CurrentTransform", "entity " + e + " has no position (no CurrentTransform)");
    if (!lt || !tc)
        return no(StateCheck::Refuse, "no LastTransform or TeleportCount",
                  "entity " + e + " cannot be teleported: it has no " + (lt ? "TeleportCount" : "LastTransform") +
                      " (the game's teleport program would never move it)");
    int32_t current = 1;
    const bool known = health && health(world, r.entity, &current);
    const auto* hp = known ? nullptr : comp<gen::NetworkHealth>(get, world, r.entity);
    if ((known && current <= 0) || (hp && hp->health == 0))
        return no(StateCheck::Refuse, "dead", "entity " + e + " is dead");
    if (!known && !hp && is_player(r.entity))  // unknown is not alive: a dead player moved can fall through the world
        return no(StateCheck::Refuse, "health unknown",
                  "player " + e + "'s health could not be read (refused rather than risk moving a dead player)");
    if (comp<gen::Teleport>(get, world, r.entity))
        return no(StateCheck::Wait, "game teleport pending", "a game teleport of entity " + e + " is still under way");
    if (is_player(r.entity)) {
        const ComponentRef actor = get_component(get, world, gen::Actor::kHash, r.entity);
        uint64_t state = 0;
        if (actor.ptr && actor.size >= kActorState + 8 &&
            seh_read(static_cast<const uint8_t*>(actor.ptr) + kActorState, &state, 8) &&
            !((state >> kActorReadyBit) & 1))
            return no(StateCheck::Wait, "not ready for gameplay",
                      "player " + e + " is not ready for gameplay (respawning, loading or being teleported)");
    }
    if (!seh_read(ct, &s.current, sizeof s.current) || !seh_read(&tc->counter, &s.counter, 1))
        return no(StateCheck::Refuse, "unreadable", "entity " + e + "'s components could not be read");
    if (r.spawn_point) {
        const std::string sp = std::to_string(r.spawn_point);
        if (!comp<gen::PlayerSpawnPoint>(get, world, r.spawn_point))
            return no(StateCheck::Refuse, "no spawn point",
                      "entity " + sp + " is no spawn point (no PlayerSpawnPoint: a flame altar, a spire or a start "
                                       "location has one)");
        emf::gen::WorldTransform t{};
        if (!read_transform(get, world, r.spawn_point, &t))
            return no(StateCheck::Refuse, "spawn point without position", "spawn point " + sp + " has no position");
        position_of(t, s.target);
    } else {
        s.target[0] = r.x;
        s.target[1] = r.y;
        s.target[2] = r.z;
    }
    s.verdict = StateCheck::Ok;
    return s;
}

// ---- the service ----
TeleportService::TeleportService(Deps deps) : deps_(std::move(deps)) {}

ProgramSpec TeleportService::spec() {
    ProgramSpec s;
    s.name = kTeleportProgramName;
    s.world_kind = EMF_WORLD_SERVER;
    s.fn = teleport_program;
    s.user = this;
    return s;
}

bool TeleportService::enabled() const {
    AcquireSRWLockShared(&lock_);
    const bool on = enabled_;
    ReleaseSRWLockShared(&lock_);
    return on;
}

size_t TeleportService::under_way() const {
    AcquireSRWLockShared(&lock_);
    const size_t n = jobs_.size();
    ReleaseSRWLockShared(&lock_);
    return n;
}

bool TeleportService::busy(uint32_t entity) const {
    AcquireSRWLockShared(&lock_);
    bool b = false;
    for (const auto& j : jobs_)
        if (j.req.entity == entity && j.phase != Phase::Settle) b = true;
    ReleaseSRWLockShared(&lock_);
    return b;
}

void TeleportService::say(const std::string& line) {
    const uint64_t now = deps_.now_ms ? deps_.now_ms() : 0;
    AcquireSRWLockExclusive(&say_lock_);
    const std::string out = said_.pass(line, now);
    ReleaseSRWLockExclusive(&say_lock_);
    if (!out.empty() && deps_.log) deps_.log(out);
}

void TeleportService::say_cause(const std::string& cause, const std::string& line) {
    AcquireSRWLockExclusive(&say_lock_);
    const bool first = causes_.insert(cause).second;
    ReleaseSRWLockExclusive(&say_lock_);
    if (first && deps_.log) deps_.log(line);
}

namespace {
std::string id_list(const std::vector<uint32_t>& ids) {
    std::string s;
    for (uint32_t e : ids) s += (s.empty() ? "" : ", ") + std::to_string(e);
    return s;
}
}  // namespace

bool TeleportService::request(const TeleportRequest& r, bool on_world_thread, std::string* why, std::string* cause) {
    auto refuse = [&](const std::string& c, const std::string& w) {
        if (why) *why = w;
        if (cause) *cause = c;
        say_cause("refused: " + c, "teleport refused: " + w + " (each kind of refusal is logged once per session)");
        return false;
    };
    if (!r.world) return refuse("no world", "no server world");
    if (WorldView(reinterpret_cast<uint8_t*>(r.world)).kind() != EMF_WORLD_SERVER)
        return refuse("not the server world", "only the server world (the host, or a dedicated server) can teleport");
    const std::string bad = check_request_numbers(r);
    if (!bad.empty()) return refuse("bad request", bad);
    Job j;
    j.req = r;
    j.session = session();
    if (on_world_thread) {
        const StateCheck s = check_teleport_state(deps_.get_component, reinterpret_cast<uint8_t*>(r.world), r, deps_.health);
        if (s.verdict == StateCheck::Refuse) return refuse(s.cause, s.why);
        if (s.verdict == StateCheck::Ok) prepare(j, s);  // pushed in the next step (checked again if it has to wait)
    }
    AcquireSRWLockExclusive(&lock_);
    const bool on = enabled_;
    bool dup = false;
    for (const auto& o : jobs_)
        if (o.req.entity == r.entity && o.phase != Phase::Settle) dup = true;
    const bool room = jobs_.size() < kMaxTeleports;
    if (on && !dup && room) {
        j.id = next_id_++;
        jobs_.push_back(std::move(j));
    }
    ReleaseSRWLockExclusive(&lock_);
    if (!on) return refuse("off", "teleport is off (an earlier call faulted, see the log)");
    if (dup) return refuse("already under way", "a teleport of entity " + std::to_string(r.entity) + " is already under way");
    if (!room) return refuse("full", "64 teleports are already under way");
    return true;
}

void TeleportService::prepare(Job& j, const StateCheck& s) {
    std::copy(s.target, s.target + 3, j.target);
    j.counter_before = s.counter;
    j.record = build_teleport(s.target[0], s.target[1], s.target[2], s.current, (j.req.flags & kTpFaceYaw) != 0,
                              j.req.yaw_deg, j.req.spawn_point, (j.req.flags & kTpFindGround) != 0);
    j.phase = Phase::Push;
}

TeleportService::Result TeleportService::failure(const Job& j, const std::string& cause, const std::string& why) {
    say_cause("failed: " + cause, "teleport of entity " + std::to_string(j.req.entity) + " failed: " + why +
                                      " (each kind of failure is logged once per session)");
    Result r;
    r.done = j.req.done;
    r.world = j.req.world;
    r.out.entity = j.req.entity;
    r.out.status = kTpFailed;
    r.out.reason = why;
    return r;
}

TeleportService::Result TeleportService::arrival(const Job& j, const double pos[3], uint32_t status) {
    Result r;
    r.done = j.req.done;
    r.world = j.req.world;
    r.out.entity = j.req.entity;
    r.out.status = status;
    std::copy(pos, pos + 3, r.out.pos);
    const double d[3] = {pos[0] - j.target[0], pos[1] - j.target[1], pos[2] - j.target[2]};
    r.out.distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const double limit = j.req.spawn_point ? kSpawnPointOffM : kOffTargetM;
    if (r.out.distance > limit) r.out.flags |= kTpOffTarget;
    if ((j.req.flags & kTpFindGround) && !j.req.spawn_point && r.out.distance < kRawTargetM) r.out.flags |= kTpRawTarget;
    return r;
}

TeleportService::Result TeleportService::ended(const Job& j, const std::string& why) {
    Result r;
    r.done = j.req.done;
    r.world = j.req.world;
    r.out.entity = j.req.entity;
    r.out.status = j.phase == Phase::Settle ? kTpReady : kTpFailed;
    r.out.reason = why;
    r.out.session_ended = true;
    return r;
}

std::vector<uint32_t> TeleportService::stranded_locked() const {
    std::vector<uint32_t> ids;
    auto add = [&ids](uint32_t e) {
        if (std::find(ids.begin(), ids.end(), e) == ids.end()) ids.push_back(e);
    };
    for (const auto& j : jobs_)
        if (j.phase == Phase::Flight) add(j.req.entity);
    for (const auto& r : removals_) add(r.entity);
    return ids;
}

void TeleportService::disable_locked(const std::string& why, bool push_broken, std::vector<uint32_t>* stranded) {
    enabled_ = false;
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (it->phase == Phase::Check || it->phase == Phase::Push) {  // never pushed: they end now
            Result r;
            r.done = it->req.done;
            r.world = it->req.world;
            r.out.entity = it->req.entity;
            r.out.status = kTpFailed;
            r.out.reason = why;
            early_.push_back(std::move(r));
            it = jobs_.erase(it);
        } else {
            ++it;  // pushed: still watched (moved, given up, ready)
        }
    }
    if (push_broken) {
        push_broken_ = true;
        if (stranded) *stranded = stranded_locked();
        removals_.clear();  // nothing can push them any more
    }
}

void TeleportService::run(void* program_context) {
    const ProgramCall* call = current_program_call();
    if (!call || call->ctx != program_context) return;
    auto* world = reinterpret_cast<EmfWorld*>(call->world);
    const uint64_t now = deps_.now_ms ? deps_.now_ms() : 0;
    const uint32_t cur = session();
    std::vector<Job> work;
    std::vector<Removal> removals;
    AcquireSRWLockExclusive(&lock_);
    last_run_world_ = call->world;
    last_run_ms_ = now;
    if (!push_broken_) {
        if (enabled_)
            for (const auto& j : jobs_)
                if (j.phase == Phase::Push && j.req.world == world && j.session == cur) work.push_back(j);
        for (auto it = removals_.begin(); it != removals_.end();) {
            if (it->world == world && it->session == cur && it->checked) {
                removals.push_back(*it);
                it = removals_.erase(it);
            } else {
                ++it;
            }
        }
    }
    ReleaseSRWLockExclusive(&lock_);
    if (work.empty() && removals.empty()) return;

    std::vector<uint32_t> stranded;
    uint8_t* cb = command_buffer(program_context);
    if (!cb || !deps_.queue_push) {
        const std::string why = "the game's command buffer is not where EMF expects it (a game update?): teleport is off";
        AcquireSRWLockExclusive(&lock_);
        for (auto& r : removals) removals_.push_back(r);  // named below with the rest
        disable_locked(why, true, &stranded);
        ReleaseSRWLockExclusive(&lock_);
        say(why);
    } else {
        const uint32_t tick = WorldView(call->world).tick();
        std::vector<Removal> again;  // no room now: the next step (checked again after this one)
        bool faulted = false;
        for (auto& r : removals) {
            const Push p = faulted ? Push::Fault : push(deps_.queue_push, cb, kRecRemoveComponents, r.entity, remove_teleport_record());
            if (p == Push::Ok) {
                say("teleport: entity " + std::to_string(r.entity) + "'s Teleport removed (the 10 s watchdog)");
            } else {
                if (p == Push::Fault) faulted = true;
                r.checked = false;
                again.push_back(r);
            }
        }
        std::vector<Result> failed;
        for (auto& j : work) {
            if (faulted) break;
            const Push p = push(deps_.queue_push, cb, kRecAddComponents, j.req.entity, add_teleport_record(j.record));
            if (p == Push::Fault) {
                faulted = true;
                break;
            }
            if (p == Push::Full) {
                if (++j.push_waits > kPushRetries) {
                    failed.push_back(failure(j, "buffer full", "the game's command buffer stayed full for 2 s"));
                    j.finished = true;
                }
                continue;
            }
            j.phase = Phase::Flight;  // pushed: watched from now on, whatever comes next
            j.pushed_tick = tick;
            if ((j.req.flags & kTpCheckpoint) && is_player(j.req.entity)) {
                const Push c = push(deps_.queue_push, cb, kRecCheckpoint, j.req.entity, checkpoint_record(j.req.entity));
                if (c == Push::Fault) {
                    faulted = true;
                    break;
                }
                if (c == Push::Full)
                    say("teleport: the replication checkpoint for player " + std::to_string(j.req.entity) + " could not be set");
            }
        }
        AcquireSRWLockExclusive(&lock_);
        for (const auto& j : work)
            for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
                if (it->id == j.id) {
                    if (j.finished) jobs_.erase(it);
                    else *it = j;
                    break;
                }
        for (auto& r : again) removals_.push_back(r);
        for (auto& r : failed) early_.push_back(std::move(r));
        if (faulted) disable_locked("the game's QueuePush faulted: teleport is off until the game restarts", true, &stranded);
        ReleaseSRWLockExclusive(&lock_);
        if (faulted) say("teleport: the game's QueuePush faulted: teleport is off until the game restarts");
    }
    if (!stranded.empty() && deps_.log)
        deps_.log("teleport: QueuePush is unusable: EMF cannot take back the Teleport it gave to these entities, which may stay "
                  "frozen \"not ready\" until the game removes it or the player rejoins: " + id_list(stranded));
}

bool TeleportService::watch(uint8_t* world, uint32_t tick, Job& j, std::vector<Result>& out, std::vector<Removal>& removals) {
    const uint32_t e = j.req.entity;
    const uint32_t age = tick - j.pushed_tick;
    const GetComponentFn get = deps_.get_component;
    emf::gen::WorldTransform t{};
    const auto* tc = comp<gen::TeleportCount>(get, world, e);
    if (!read_transform(get, world, e, &t) || !tc) {
        out.push_back(j.phase == Phase::Settle ? arrival(j, j.target, kTpReady)
                                               : failure(j, "gone", "entity " + std::to_string(e) + " no longer exists"));
        if (j.phase == Phase::Settle) out.back().out.reason = "entity " + std::to_string(e) + " no longer exists";
        return true;
    }
    double pos[3];
    position_of(t, pos);
    const bool has_teleport = comp<gen::Teleport>(get, world, e) != nullptr;
    if (j.phase == Phase::Flight) {
        // Only before the move: a Teleport after it (a death, a respawn, fast travel) is the game's own.
        if (has_teleport && age >= kGiveUpTicks && !j.removal_queued) {
            j.removal_queued = true;
            Removal r;
            r.world = j.req.world;
            r.session = j.session;
            r.entity = e;
            r.record = j.record;
            removals.push_back(r);
        }
        uint8_t counter = j.counter_before;
        seh_read(&tc->counter, &counter, 1);
        if (counter != j.counter_before) {
            out.push_back(arrival(j, pos, kTpMoved));
            if (j.req.flags & kTpNotifyReady) {
                if (is_player(e)) {
                    j.phase = Phase::Settle;
                    j.moved_tick = tick;
                    return false;
                }
                out.push_back(arrival(j, pos, kTpReady));  // NPCs have no "ready" state: at once
            }
            return true;
        }
        if (age >= kGiveUpTicks) {
            Result r = failure(j, "not moved", "entity " + std::to_string(e) +
                                                   " was not moved within 10 s (the game's teleport program did not run "
                                                   "for it, or its ground search never finished)");
            std::copy(pos, pos + 3, r.out.pos);
            out.push_back(std::move(r));
            return true;
        }
        return false;
    }
    // Settle: the second callback once the player is ready for gameplay again (Actor bit 50), at the latest after 10 s
    const ComponentRef actor = get_component(get, world, gen::Actor::kHash, e);
    uint64_t state = 0;
    const bool ready = actor.ptr && actor.size >= kActorState + 8 &&
                       seh_read(static_cast<const uint8_t*>(actor.ptr) + kActorState, &state, 8) &&
                       ((state >> kActorReadyBit) & 1) && !has_teleport;
    if (ready || tick - j.moved_tick >= kReadyWatchTicks) {
        out.push_back(arrival(j, pos, kTpReady));
        if (!ready) out.back().out.reason = "player " + std::to_string(e) + " was still not ready for gameplay 10 s after the move";
        return true;
    }
    return false;
}

void TeleportService::on_server_tick(EmfWorld* world) {
    if (!world) return;
    auto* raw = reinterpret_cast<uint8_t*>(world);
    const uint64_t now = deps_.now_ms ? deps_.now_ms() : 0;
    const uint32_t tick = WorldView(raw).tick();
    const uint32_t cur = session();
    std::vector<Result> out;
    std::vector<Job> work;
    AcquireSRWLockExclusive(&lock_);
    out.swap(early_);
    if (world != seen_world_) {
        seen_world_ = world;
        seen_since_ms_ = now;
    }
    const bool running = last_run_world_ == static_cast<void*>(world) && now - last_run_ms_ < kProgramWatchdogMs;
    const bool program_missing = !running && now - seen_since_ms_ >= kProgramWatchdogMs;
    work = jobs_;
    ReleaseSRWLockExclusive(&lock_);

    std::vector<Removal> removals;
    for (auto& j : work) {
        if (j.req.world != world || j.session != cur) {  // the session it belongs to is over (no reset was seen)
            out.push_back(ended(j, "the world changed"));
            j.finished = true;
            continue;
        }
        switch (j.phase) {
            case Phase::Check:
            case Phase::Push: {
                if (j.phase == Phase::Push && program_missing) {
                    out.push_back(failure(j, "program not running",
                                          std::string("the ") + kTeleportProgramName +
                                              " program is not running in this world (left out at world setup or "
                                              "faulted, see the log)"));
                    j.finished = true;
                    break;
                }
                // also every step while it waits to be pushed (a full buffer): a death or a game Teleport stops it
                const StateCheck s = check_teleport_state(deps_.get_component, raw, j.req, deps_.health);
                if (s.verdict == StateCheck::Ok) {
                    prepare(j, s);
                } else if (s.verdict == StateCheck::Refuse) {
                    out.push_back(failure(j, s.cause, s.why));
                    j.finished = true;
                } else {
                    j.phase = Phase::Check;
                    if (!j.waiting) {
                        j.waiting = true;
                        j.first_tick = tick;
                    } else if (tick - j.first_tick >= kStateWaitTicks) {
                        out.push_back(failure(j, s.cause, s.why + " (waited 5 s)"));
                        j.finished = true;
                    }
                }
                break;
            }
            case Phase::Flight:
            case Phase::Settle:
                j.finished = watch(raw, tick, j, out, removals);
                break;
        }
    }
    std::vector<std::string> dropped;
    std::vector<uint32_t> cannot_remove;
    AcquireSRWLockExclusive(&lock_);
    for (const auto& j : work)
        for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
            if (it->id == j.id) {
                if (j.finished) jobs_.erase(it);
                else *it = j;
                break;
            }
    for (auto& r : removals) {
        if (push_broken_) cannot_remove.push_back(r.entity);
        else removals_.push_back(r);
    }
    // Every removal is checked again before its push: the same session, and that same Teleport (EMF's record) still on
    // the entity; an id reused after a disconnect, or a Teleport the game added since, is never removed.
    for (auto it = removals_.begin(); it != removals_.end();) {
        if (it->world != world) {
            ++it;
            continue;
        }
        if (it->session != cur) {
            it = removals_.erase(it);
            continue;
        }
        const auto* tp = comp<gen::Teleport>(deps_.get_component, raw, it->entity);
        const bool parts = comp<gen::CurrentTransform>(deps_.get_component, raw, it->entity) &&
                           comp<gen::LastTransform>(deps_.get_component, raw, it->entity) &&
                           comp<gen::TeleportCount>(deps_.get_component, raw, it->entity);
        gen::Teleport now_tp{};
        const bool same = tp && parts && seh_read(tp, &now_tp, sizeof now_tp) &&
                          now_tp.targetTransform.position.x == it->record.targetTransform.position.x &&
                          now_tp.targetTransform.position.y == it->record.targetTransform.position.y &&
                          now_tp.targetTransform.position.z == it->record.targetTransform.position.z &&
                          now_tp.targetEntityId.id == it->record.targetEntityId.id && !now_tp.applied;
        if (!same) {
            if (tp && parts) dropped.push_back("teleport: entity " + std::to_string(it->entity) +
                                               " holds a Teleport that is not EMF's any more: not removed");
            it = removals_.erase(it);
            continue;
        }
        it->checked = true;
        ++it;
    }
    std::vector<uint32_t> waiting_removal;
    if (program_missing) {
        for (const auto& j : jobs_)
            if (j.phase == Phase::Flight && j.req.world == world) waiting_removal.push_back(j.req.entity);
        for (const auto& r : removals_)
            if (r.world == world && std::find(waiting_removal.begin(), waiting_removal.end(), r.entity) == waiting_removal.end())
                waiting_removal.push_back(r.entity);
    }
    ReleaseSRWLockExclusive(&lock_);
    for (const auto& line : dropped) say(line);
    for (const auto& r : removals)
        say("teleport: entity " + std::to_string(r.entity) + " still has a Teleport 10 s after EMF's push: it is removed");
    if (!cannot_remove.empty() && deps_.log)
        deps_.log("teleport: QueuePush is unusable: EMF cannot take back the Teleport it gave to entity " + id_list(cannot_remove) +
                  ", which may stay frozen \"not ready\" until the game removes it or the player rejoins");
    if (!waiting_removal.empty())
        say_cause("program missing, stranded",
                  std::string("teleport: the ") + kTeleportProgramName + " program is not running in this world: EMF cannot "
                  "take back the Teleport it gave to entit" + (waiting_removal.size() == 1 ? "y " : "ies ") +
                  id_list(waiting_removal) + " if the game never applies it (they may stay frozen \"not ready\")");
    deliver(world, out);
}

void TeleportService::world_reset() {
    std::vector<Result> out;
    AcquireSRWLockExclusive(&lock_);
    out.swap(early_);
    for (auto& r : out) r.out.session_ended = true;  // made in the old session, delivered outside its ticks
    for (const auto& j : jobs_) out.push_back(ended(j, "a new world was set up"));
    jobs_.clear();
    removals_.clear();  // Teleport is never saved: nothing of the old session is left to take back
    ReleaseSRWLockExclusive(&lock_);
    AcquireSRWLockExclusive(&say_lock_);
    causes_.clear();  // a refusal in the new session is news
    ReleaseSRWLockExclusive(&say_lock_);
    deliver(nullptr, out);  // now, with the world each was meant for: never the new world's pointer
}

size_t TeleportService::end_session(const std::string& why) {
    std::vector<Result> out;
    AcquireSRWLockExclusive(&lock_);
    out.swap(early_);
    for (auto& r : out) r.out.session_ended = true;
    for (const auto& j : jobs_) out.push_back(ended(j, why));
    jobs_.clear();
    removals_.clear();
    ReleaseSRWLockExclusive(&lock_);
    deliver(nullptr, out);
    return out.size();
}

void TeleportService::deliver(EmfWorld* world, std::vector<Result>& results) {
    for (auto& r : results) {
        if (!r.done) continue;
        try {
            r.done(r.world ? r.world : world, r.out);
        } catch (...) {
            say("teleport: a callback threw a C++ exception");
        }
    }
}

}  // namespace emf::core
