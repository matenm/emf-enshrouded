#pragma once
// Teleporting entities (docs/engine_parts/re_teleport.md, sections 5 and 8). EMF's own server program "emf_teleport"
// pushes the game's "add component" record for `Teleport` (kind 3, 80 bytes of data) into its job's command buffer
// with the game's QueuePush, as the game's actor_teleport and admin_player do; the game's program `teleport` (#701)
// then moves the entity on the next step (ground search, hot point, fall damage reset, TeleportCount + 1) and every
// player's game snaps to the new position.
//
// The flow of one request:
//   request()        any thread: the numbers, the world kind, a teleport of the entity already under way, room.
//   on_server_tick() the server world's thread after a step: the entity's state through getComponent (it has
//                    CurrentTransform, LastTransform and TeleportCount; it is not dead; no game Teleport is pending; a
//                    player is ready for gameplay; a spawn point has PlayerSpawnPoint), then the 80-byte record is built.
//   run()            emf_teleport, inside the next step: QueuePush (a full buffer: again next step).
//   on_server_tick() after the steps that follow: TeleportCount changed = moved (the callback gets where the entity is
//                    now); 600 ticks (10 s) without a move = given up, and a Teleport still there is removed with a
//                    kind-4 record, so a player is never left "not ready for gameplay".
// Callbacks run in on_server_tick, on the server world's thread, outside every lock.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "common/log_limiter.h"
#include "common/world_view.h"
#include "emf/emf_api.h"  // EmfWorld
#include "emf/generated/components.hpp"
#include "system_programs.h"

namespace emf::core {

// QueuePush(cmdbuf, buf = cmdbuf + 0x10, kind, size (the whole record), entity): a zeroed record (16-byte aligned,
// header filled) or NULL when the buffer is full (re_teleport.md 1.2).
using QueuePushFn = uint8_t* (*)(void* cmdbuf, void* buf, uint8_t kind, uint32_t size, uint32_t entity);

// ---- the engine's layouts (re_teleport.md 1.2, 1.3; confirmed live on build 1273043, game and dedicated server) ----
constexpr size_t kJobCmdBuf = 0x08;                           // ProgramContext -> job; cmdbuf = *(job + 8)
constexpr size_t kCmdBufQueue = 0x10;                         // the record buffer inside the command buffer
constexpr size_t kCmdBufCapacity = 0x18, kCmdBufUsed = 0x20;  // u64 capacity (1 MiB live), u32 bytes used
constexpr uint8_t kRecAddComponents = 3, kRecRemoveComponents = 4, kRecCheckpoint = 0x10;
constexpr uint32_t kRecHashes = 0x0C, kRecSizes = 0x4C, kRecData = 0x6C;  // kind 3: u32 hash[16], u16 size[16], data
constexpr uint32_t kAddTeleportSize = kRecData + sizeof(emf::gen::ecs::Teleport);  // 0xBC
constexpr uint32_t kRemoveSize = kRecSizes;                                         // kind 4: u32 hash[16]
constexpr uint32_t kCheckpointSize = 0x10;                                          // kind 0x10: u32 player
constexpr size_t kActorState = 0xBF8;  // Actor.currentState (u64); bit 50 = ready for gameplay (actor_is_ready_for_gameplay)
constexpr int kActorReadyBit = 50;
static_assert(sizeof(emf::gen::ecs::Teleport) == 80 && kAddTeleportSize == 0xBC, "Teleport record (re_teleport.md 1.2)");
static_assert(offsetof(emf::gen::ecs::Actor, currentState) == kActorState, "Actor.currentState");

// ---- limits ----
constexpr size_t kMaxTeleports = 64;           // requests under way at once (checked, waiting to push, or watched)
constexpr uint32_t kGiveUpTicks = 600;         // 10 s at 60 Hz after the push: given up, a Teleport left is removed
constexpr uint32_t kReadyWatchTicks = 600;     // NOTIFY_READY: the second callback at the latest 10 s after the move
constexpr uint32_t kStateWaitTicks = 300;      // 5 s waiting for a game teleport to end or a player to be ready
constexpr int kPushRetries = 120;              // ticks a push waits for room in a full command buffer
constexpr uint64_t kProgramWatchdogMs = 2000;  // emf_teleport has not run in this world for 2 s: requests fail
constexpr double kOffTargetM = 1.0;            // the final position is "off target" beyond this (x/y/z requests)
constexpr double kSpawnPointOffM = 25.0;       // ... and this far from a spawn point (it lands on a pad around it)
constexpr double kRawTargetM = 0.05;           // FIND_GROUND gave the target as it was (no walkable cell near it)
// The world's x/z extent and a generous y range (re_teleport.md 5.2/7: the EntityGrid is 160 x 64 m; live positions
// were x 3619-4748, z 1160-1434, y 677-905).
constexpr double kWorldMinXZ = 0.0, kWorldMaxXZ = 10240.0, kWorldMinY = -1000.0, kWorldMaxY = 5000.0;
constexpr const char* kTeleportProgramName = "emf_teleport";

// Request flags, results and result flags (the API's EMF_TELEPORT_* have the same values; api_sizes.h checks).
constexpr uint32_t kTpFindGround = 0x01;   // searchBestSpawnPosition: the nearest walkable cell within about 10 m
constexpr uint32_t kTpFaceYaw = 0x02;      // turn to yaw_deg (else keep the current orientation)
constexpr uint32_t kTpCheckpoint = 0x04;   // players: also a replication checkpoint, as fast travel does
constexpr uint32_t kTpNotifyReady = 0x08;  // players: a second callback once the player can play again
constexpr uint32_t kTpKnownFlags = kTpFindGround | kTpFaceYaw | kTpCheckpoint | kTpNotifyReady;
constexpr uint32_t kTpFailed = 0, kTpMoved = 1, kTpReady = 2;  // outcome status
constexpr uint32_t kTpOffTarget = 0x01;  // moved, but farther than kOffTargetM from the target (kSpawnPointOffM: a pad)
constexpr uint32_t kTpRawTarget = 0x02;  // FIND_GROUND found no walkable cell: the game used the target as it was

// The command-buffer and record layouts were checked on this known build (index into known_builds()): QueuePush has a
// checked RVA there (1273043, game and dedicated server). Elsewhere teleport stays off.
bool teleport_layouts_checked(int build);

// ---- pure helpers (tested) ----
// 32.32 fixed point as the game rounds it: round(m * 2^32) (the game: trunc(m * 2^32 + 0.5)).
int64_t to_fixed(double metres);
double from_fixed(int64_t fixed);
// The rotation by `yaw_deg` about the up axis (y): (0, sin(yaw/2), 0, cos(yaw/2)).
emf::gen::Quaternion yaw_quaternion(float yaw_deg);
// The Teleport component for a move to (x, y, z): the entity's current orientation (or the yaw) and scale (#701 writes
// scale too), targetEntityId = spawn_point, searchBestSpawnPosition = ground, applied 0, delayEndTime 0.
emf::gen::ecs::Teleport build_teleport(double x, double y, double z, const emf::gen::WorldTransform& current,
                                       bool face_yaw, float yaw_deg, uint32_t spawn_point, bool ground);
// A whole record as QueuePush hands it out (the header's first 0xC bytes stay QueuePush's): kind 3 adding `t` (hash at
// +0xC, the other 15 hash slots 0, size 80 at +0x4C, the data at +0x6C), kind 4 removing Teleport (hash at +0xC), kind
// 0x10 (the player at +0xC).
std::vector<uint8_t> add_teleport_record(const emf::gen::ecs::Teleport& t);
std::vector<uint8_t> remove_teleport_record();
std::vector<uint8_t> checkpoint_record(uint32_t player);

// How a request ended (the callback's view).
struct TeleportOutcome {
    uint32_t entity = 0;
    uint32_t status = kTpFailed;
    uint32_t flags = 0;    // kTpOffTarget | kTpRawTarget
    double pos[3] = {};    // where the entity is now (MOVED, READY; FAILED: its position if known)
    double distance = 0;   // metres between pos and the target (MOVED, READY)
    std::string reason;    // FAILED, or a READY that timed out: why
    bool session_ended = false;  // the session ended first (a new world, the menu): do not touch the world, only clean up
};
using TeleportDone = std::function<void(EmfWorld* world, const TeleportOutcome& outcome)>;

struct TeleportRequest {
    EmfWorld* world = nullptr;
    uint32_t entity = 0;
    double x = 0, y = 0, z = 0;  // ignored with spawn_point (the spawn point's own position is the target)
    float yaw_deg = 0;
    uint32_t spawn_point = 0;
    uint32_t flags = 0;  // kTp*
    TeleportDone done;   // may be empty; called once (with kTpNotifyReady a MOVED is followed by a READY)
};
// Why a request's numbers are refused ("" = fine): entity 0, unknown flags, a non-finite number, a position outside the
// world (x/z 0-10240 m, y -1000-5000 m), a yaw that is not finite.
std::string check_request_numbers(const TeleportRequest& r);

// What the state check of a request found.
struct StateCheck {
    enum Verdict { Ok, Wait, Refuse } verdict = Ok;
    std::string cause;  // short and fixed per kind of problem (logged once per cause)
    std::string why;    // for the callback and the log
    emf::gen::WorldTransform current{};  // Ok: the entity's CurrentTransform
    uint8_t counter = 0;                 // Ok: TeleportCount.counter now
    double target[3] = {};               // Ok: x/y/z, or the spawn point's position
};
// The current health of an entity as the game reads it (its health attribute): false when it has none (or the
// attribute functions are off on this build). A player in the server world has Health but no NetworkHealth (live).
using HealthFn = std::function<bool(uint8_t* world, uint32_t entity, int32_t* current)>;
// The entity's state in `world` through getComponent (the server world's thread, between steps). Dead = the health
// attribute at 0 (health), else NetworkHealth.health 0.
StateCheck check_teleport_state(GetComponentFn get, uint8_t* world, const TeleportRequest& r,
                                const HealthFn& health = nullptr);

class TeleportService {
public:
    struct Deps {
        QueuePushFn queue_push = nullptr;  // required
        GetComponentFn get_component = nullptr;
        HealthFn health;  // optional: the dead check reads NetworkHealth only without it
        std::function<void(const std::string&)> log;
        std::function<uint64_t()> now_ms;  // wall clock (the program watchdog, log limits)
        std::function<uint32_t()> session;  // the hosted session number (server_session); a request belongs to one
    };
    explicit TeleportService(Deps deps);
    TeleportService(const TeleportService&) = delete;
    TeleportService& operator=(const TeleportService&) = delete;

    // The emf_teleport program (server world, no entity query, nothing declared: it only pushes records).
    ProgramSpec spec();
    // Queues a request. on_world_thread: the caller runs in a callback on r.world after its step, so the entity's state
    // is checked at once (a refusal returns false); otherwise it is checked after the server world's next step and a
    // refusal then reaches the callback. False and *why (and *cause, fixed per kind of problem) when refused.
    bool request(const TeleportRequest& r, bool on_world_thread, std::string* why, std::string* cause = nullptr);
    // The program callback (inside the program call).
    void run(void* program_context);
    // After every step of a server world, on its thread (the state checks, the watch, the callbacks).
    void on_server_tick(EmfWorld* world);
    // A new server world is being set up (its setup, on the server thread): every request under way is answered now
    // with the world it was meant for and session_ended ("a new world was set up"); never on the new world's ticks.
    void world_reset();
    // The server world is gone without a new one (back to the menu): every request under way is answered now, on this
    // thread, FAILED (READY after a move) with session_ended and `why`. Returns how many were answered.
    size_t end_session(const std::string& why);
    bool enabled() const;
    size_t under_way() const;
    // The entity has a teleport under way (checked, waiting, or not moved yet).
    bool busy(uint32_t entity) const;

private:
    enum class Phase { Check, Push, Flight, Settle };
    struct Job {
        uint32_t id = 0;
        TeleportRequest req;
        uint32_t session = 0;  // the hosted session the request belongs to
        Phase phase = Phase::Check;
        uint32_t first_tick = 0;  // world tick of the first state check that said "wait"
        bool waiting = false;
        emf::gen::ecs::Teleport record{};
        uint8_t counter_before = 0;
        double target[3] = {};
        int push_waits = 0;
        uint32_t pushed_tick = 0;
        uint32_t moved_tick = 0;
        bool removal_queued = false;
        bool finished = false;
    };
    // A kind-4 record to push for a Teleport EMF gave and the game never applied. Pushed only after a post-step check
    // found that same Teleport (the record's target) still on that entity, in that session.
    struct Removal {
        EmfWorld* world = nullptr;
        uint32_t session = 0;
        uint32_t entity = 0;
        emf::gen::ecs::Teleport record{};
        bool checked = false;  // the last post-step found EMF's Teleport there
    };
    struct Result {
        TeleportDone done;
        TeleportOutcome out;
        EmfWorld* world = nullptr;  // the world it is delivered with (nullptr: the tick's)
    };
    // A FAILED result for the job (the line is logged once per cause and session).
    Result failure(const Job& j, const std::string& cause, const std::string& why);
    // A MOVED or READY result: where the entity is now, its distance from the target and the result flags.
    static Result arrival(const Job& j, const double pos[3], uint32_t status);
    // The answer for a job the session ended under (FAILED, READY after a move), with session_ended.
    static Result ended(const Job& j, const std::string& why);
    // Check -> Push: the record from the entity's state.
    static void prepare(Job& j, const StateCheck& s);
    // One post-step look at a pushed job (Flight, Settle). True when the job is done.
    bool watch(uint8_t* world, uint32_t tick, Job& j, std::vector<Result>& out, std::vector<Removal>& removals);
    void deliver(EmfWorld* world, std::vector<Result>& results);
    void say(const std::string& line);                                  // the same line at most every 10 s
    void say_cause(const std::string& cause, const std::string& line);  // once per cause and session
    // Turns new requests off (a fault or a layout surprise): the jobs not pushed yet end FAILED with `why`; the pushed
    // ones stay watched. push_broken: QueuePush itself cannot be used any more (no removals either): the entities that
    // may keep EMF's Teleport are named in the log (lock_ held; *stranded gets them).
    void disable_locked(const std::string& why, bool push_broken, std::vector<uint32_t>* stranded);
    // The entities EMF gave a Teleport the game has not applied yet (pushed, not moved, or waiting for removal).
    std::vector<uint32_t> stranded_locked() const;
    uint32_t session() const { return deps_.session ? deps_.session() : 0; }

    Deps deps_;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<Job> jobs_;
    std::vector<Removal> removals_;  // kind-4 records to push
    std::vector<Result> early_;  // results made outside on_server_tick (the program)
    uint32_t next_id_ = 1;
    bool enabled_ = true;
    bool push_broken_ = false;  // QueuePush faulted (or the command buffer is unusable): nothing is pushed any more
    // program watchdog (guarded by lock_)
    void* last_run_world_ = nullptr;
    uint64_t last_run_ms_ = 0;
    EmfWorld* seen_world_ = nullptr;
    uint64_t seen_since_ms_ = 0;
    // log limits
    SRWLOCK say_lock_ = SRWLOCK_INIT;
    LogLimiter said_;
    std::set<std::string> causes_;
};

}  // namespace emf::core
