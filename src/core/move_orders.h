#pragma once
// Move orders (API v40): a creature with the enemy AI walked to points through the game's own guard order
// (StartGuardingEvent) on a hidden waypoint EMF spawns at the point (private spec move-order 2026-11-12; live spike: a
// guard order on an entity 25 m away walked a Vukah straight there). At each point the waypoint goes and a new one is
// spawned at the next. The guard order is sent again every kMoveResendMs while the creature is not fighting and not
// guarding its waypoint (the game ends a guard on Stuck). It may fight on the way (the game's way); EMF_MOVE_PEACEFUL
// clears its fights against players each step. A waypoint that vanishes is placed again (at most kMoveReplaceMax times,
// once per kMoveResendMs). The order ends: arrived (the last point within `arrive` across, or within the creature's guard
// distance + 1 m once it guards the waypoint: it stops that far short; and that + 2 m up or down), stuck (no metre of
// progress for kMoveStuckMs while not fighting), timeout, gone (the creature
// died, went, became a player or a player's ally without EMF_MOVE_ALLIES), cancelled (cancel, a new move on it, the
// world closing). Then the waypoint goes and the guard orders still waiting for the creature are dropped (Deps::unguard);
// a creature that guarded someone (still there) before the move guards them again, any other
// stops guarding, and on arrival its home becomes the last point (it stays there). Game-free: the world is reached
// through Deps; step() on the server world's thread; the callback runs after the order is gone (it may start another).
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "emf/emf_api.h"

namespace emf::core {

constexpr size_t kMovesMax = 256;
constexpr size_t kMovePointsMax = 16;
constexpr uint64_t kMoveResendMs = 3000;
constexpr uint64_t kMoveStuckMs = 15000;  // the default stuck time
constexpr double kMoveStuckMin = 2.0, kMoveStuckMax = 120.0;  // seconds
constexpr double kMoveProgress = 1.0;    // metres closer to the point that count as progress
constexpr double kMoveArriveMin = 0.5, kMoveArriveMax = 50.0;
constexpr double kMoveSecondsMax = 3600.0;
constexpr double kMoveUpDown = 2.0;      // a point is reached within arrive + this up or down
// A guarding creature stops its guard distance short of the waypoint (currentGuardTargetDistance, 3 m on a Vukah, seen
// live): a point also counts as reached within that distance + this.
constexpr double kMoveGuardSlack = 1.0;
constexpr double kMoveGuardMax = 8.0;    // a larger guard distance is not believed
constexpr uint32_t kMoveReplaceMax = 3;  // a waypoint that vanishes is placed again this often, then the move is stuck
// Two creatures on one spot, both with a move, never walk (a modder's raids; live 2026-11-15: Idle <-> Guard, 0 m; moved
// 1 m apart, both walked at once). After kMoveUnstackMs without a metre of progress, the creature of the later move
// (the higher id) is moved kMoveUnstackM aside, at most kMoveUnstackMax times per move, kMoveResendMs apart.
constexpr double kMoveStackedM = 0.3;    // across: "on one spot" (and within 1 m up or down)
constexpr double kMoveUnstackM = 1.0;
constexpr uint64_t kMoveUnstackMs = 2000;
constexpr uint32_t kMoveUnstackMax = 2;
// The invisible waypoint: the game's TestTransform template 6d71f70d-602c-4d55-9de2-73e5b84133b5 (CurrentTransform,
// StaticTransform, DoNotSave), in the game's GUID layout (first three groups little-endian, docs/ENGINE_MAP.md).
constexpr uint8_t kWaypointGuid[16] = {0x0d, 0xf7, 0x71, 0x6d, 0x2c, 0x60, 0x55, 0x4d,
                                       0x9d, 0xe2, 0x73, 0xe5, 0xb8, 0x41, 0x33, 0xb5};

class MoveOrders {
public:
    using Point = std::array<double, 3>;
    // world: null when the world closed (world_reset); status: EMF_MOVE_*
    using Callback = std::function<void(EmfWorld* world, uint32_t id, uint32_t status)>;
    struct Deps {
        // a component of an entity in that world now (writable), its size in *size; null when it has none
        std::function<void*(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size)> get;
        std::function<uint64_t()> now_ms;
        std::function<uint32_t(EmfWorld*, double x, double y, double z)> spawn;  // a waypoint there; 0 when it failed
        std::function<void(EmfWorld*, uint32_t entity)> destroy;
        std::function<void(uint32_t guarding, uint32_t guarded)> guard;  // the game's guard order, sent at the next step
        std::function<void(uint32_t guarding)> unguard;  // drops the guard orders still waiting for that creature
        std::function<bool(EmfWorld*, uint32_t entity)> dead;             // its health is known and 0 or less
        // moves a creature there with the game's teleport (one standing on another moving creature's spot); false: refused
        std::function<bool(EmfWorld*, uint32_t entity, double x, double y, double z)> nudge;
        // v49 EMF_MOVE_CLEAR: a building block on the line from `from` to `to` (metres); null: no wall checks here
        std::function<bool(EmfWorld*, const Point& from, const Point& to)> wall_between;
    };
    explicit MoveOrders(Deps d) : d_(std::move(d)) {}

    // A move (arrive: metres, kMoveArriveMin..Max; seconds: 0 = no limit, at most kMoveSecondsMax; flags: EMF_MOVE_*);
    // its id (from 1, never reused), or 0 and *why. Its first waypoint is spawned and the guard order queued at once.
    // A move on a creature that is moving already ends the old one (cancelled).
    // stuck_seconds: no metre closer for this long while not fighting ends it stuck (0 = kMoveStuckMs; else kMoveStuckMin..Max).
    uint32_t add(EmfWorld* world, uint32_t enemy, const std::vector<Point>& points, double arrive, double seconds,
                 uint32_t flags, Callback cb, std::string* why, double stuck_seconds = 0);
    // Ends a move (cancelled). now: at once (inside a callback on its world: its waypoint goes now), else at the next
    // step. False: no such move.
    bool cancel(uint32_t id, bool now = true);
    bool cancel_enemy(EmfWorld* world, uint32_t enemy, bool now = true);  // its move, if any (an enemy order on it starts)
    void step(EmfWorld* world);  // after a server step of `world`
    void world_reset();          // a new server world: every move ends cancelled (world null), nothing is written
    uint32_t moving(EmfWorld* world, uint32_t enemy) const;  // its move's id, 0 none
    size_t count() const {
        std::lock_guard<std::mutex> l(m_);
        return moves_.size();
    }

private:
    struct Move {
        uint32_t id = 0, enemy = 0, flags = 0, waypoint = 0, prior_guard = 0;
        uint32_t old_waypoint = 0;  // the one before (the creature may still guard it)
        Point waypoint_at{};        // where the waypoint was spawned: an id at another place is not ours any more
        uint32_t replaced = 0;      // times its waypoint vanished and was placed again
        uint64_t last_place = 0;    // when a waypoint was last spawned
        uint64_t last_nudge = 0;    // when it was last put in Guard (a calm creature that took the waypoint)
        uint32_t unstacked = 0;     // times it was moved aside (it stood on another moving creature's spot)
        uint64_t last_unstack = 0;
        EmfWorld* world = nullptr;
        std::vector<Point> points;
        size_t next = 0;  // the point it walks to
        double arrive = 2.0;
        uint64_t until = 0;       // 0: no time limit
        uint64_t last_guard = 0;  // when the guard order was last sent
        uint64_t since = 0;       // when it last made progress (or stopped fighting)
        uint64_t stuck_ms = kMoveStuckMs;
        double best = 0;          // its closest distance to the point since then
        bool cancelled = false;   // ends at the next step
        Callback cb;
    };
    struct Done {
        Callback cb;
        EmfWorld* world;
        uint32_t id, status;
    };
    // Its status when it ends now, else -1 (and the move is walked on: next point, guard order, peaceful).
    int tick(Move& m, uint64_t now);
    void finish(Move& m, uint32_t status);  // its waypoint goes; its guard and home as the header says
    // spawns the waypoint at its point and sends the guard order; fresh (a new point): the stuck clock starts again
    bool place_waypoint(Move& m, uint64_t now, bool fresh);
    bool waypoint_alive(const Move& m) const;  // its waypoint is still there, where it was spawned
    bool alive(EmfWorld* w, uint32_t entity) const;  // an entity with a position, not dead
    void send_guard(EmfWorld* w, uint32_t enemy, uint32_t waypoint);  // and a calm creature is put in Idle
    bool fighting(EmfWorld* w, uint32_t enemy) const;
    void calm_players(EmfWorld* w, uint32_t enemy, bool all) const;  // EMF_MOVE_PEACEFUL (all: _PEACEFUL_ALL)
    bool gone(EmfWorld* w, const Move& m) const;
    uint32_t moving_locked(EmfWorld* world, uint32_t enemy) const;  // under m_
    double distance(EmfWorld* w, uint32_t enemy, const Point& p) const;  // metres; < 0: no position

    void unstack(Move& m, uint64_t now);  // kMoveUnstack*: moved aside when it stands on another moving creature's spot

    struct Stand {
        uint32_t id, enemy;
        Point at;
    };
    Deps d_;
    mutable std::mutex m_;  // moves_: a cancel may come from another thread
    std::vector<Move> moves_;
    std::vector<Stand> stand_;  // this step: where each move's creature stands (under m_)
    uint32_t next_id_ = 1;
};

}  // namespace emf::core
