#pragma once
// Villager moves (API v45): a villager walked to points by re-sending the game's own NpcCommandEvent GoToPosition (the
// command the game uses itself), queued per villager and sent from the emf_moves program. The game takes one command
// per villager per step and drops it when the villager arrives, is interrupted (a player close by, night, a chat,
// water) or its path fails, so EMF sends the point again whenever the villager is in a state that takes it and the order
// is gone, at most once per kVillagerResendMs. Nothing is written to any game component; nothing is saved.
//
// Refused when added: no villager (DynamicActiveNpcState, its template ActiveNpcState with an npcId), a player, a villager
// the player told to stay (roaming range 0), a point outside its walk area (the player's roaming range when set, else the
// template's roamingSetup.maxRadiusFromSpawn, measured across from its home, + kVillagerAreaSlack) or beyond the game's
// leash (max(targetRadius, bedRadius) from home in 3D: the game teleports it home there). Live 2026-11-16: a 30 m area,
// a 50 m leash (the game adds 1); points at 20 m and 28 m were walked to, one at 40 m failed (path result 7).
//
// Each step: paused (nothing sent, the stuck clock waits) while no player is within kVillagerPauseM, in Start, FindBed,
// Sleep or InsideWater, deciding to sleep, or while a player talks to it (interaction bits turning, greeting, a
// sequence). It ends: arrived (within `arrive` across and that + 2 up or down, or Roam ended at the walker's
// TargetReached within arrive + 3: the walker stops up to 5 m short of a point it cannot stand on), stuck (no metre closer
// for stuck_seconds while not paused, a jump of more than kVillagerJumpM in one step: the game teleported it, or the path
// not found for the same point after kVillagerNotReachableMax sends), timeout, gone (no AI, no position, another npcId),
// cancelled (cancel, a new move on it, the world closing). Cancelled, stuck and timeout tell it to stop where it stands.
//
// Hold (hold_seconds, or hold_forever): after the arrival callback it keeps the villager at the last point, sending the
// point again every kVillagerHoldResendMs while it is not walking there. It waits while paused or while a player is
// close (WaitingCloseToPlayer: never pulled away from a player). It ends quietly (no second callback) when its time is
// over, on cancel, a new move, the world closing, the player setting stay, its home moving more than 1 m, a teleport, or
// no way back for stuck_seconds. Game-free: the world is reached through Deps; step() on the server world's thread; a
// callback runs after the move is gone (or holding), so it may cancel it or start another.
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "emf/emf_api.h"

namespace emf::core {

constexpr size_t kVillagerMovesMax = 256;
constexpr uint64_t kVillagerResendMs = 1000;      // a dropped order is sent again at most this often (walking)
constexpr uint64_t kVillagerHoldResendMs = 2000;  // a hold's re-send (live: every 2 s kept it in place)
constexpr double kVillagerJumpM = 4.0;            // farther than this in one step: the game teleported it
constexpr double kVillagerAreaSlack = 1.0;        // m: a point this far past the walk area still counts inside
constexpr double kVillagerPauseM = 150.0;         // no player this close: the game does not run the villager
constexpr uint32_t kVillagerNotReachableMax = 2;
constexpr double kVillagerHomeMovedM = 1.0;       // a hold ends when its home moves this far
constexpr double kVillagerTargetedReach = 3.0;    // Roam ended at TargetReached within arrive + this: arrived

// The GoToPosition commands waiting for the emf_moves program: at most one per villager (the game reads only the first
// per villager and step), the latest point wins; at most kNpcQueueMax villagers. Any thread.
constexpr size_t kNpcQueueMax = 1024;
class NpcSendQueue {
public:
    struct Send {
        uint32_t npc;
        double x, y, z;
    };
    void push(uint32_t npc, double x, double y, double z);  // replaces a send waiting for that villager
    void drop(uint32_t npc);
    std::vector<Send> take();  // everything waiting, oldest first; the queue is empty afterwards
    // sends the program could not emit this step: ahead of the newer ones, unless a newer one names the same villager
    void put_back(std::vector<Send> rest);
    void clear();
    size_t size() const;

private:
    mutable std::mutex m_;
    std::vector<Send> q_;
};

class VillagerMoves {
public:
    using Point = std::array<double, 3>;
    using Callback = std::function<void(EmfWorld* world, uint32_t id, uint32_t status)>;  // status: EMF_MOVE_*
    struct Deps {
        // a component of an entity in that world now (writable), its size in *size; null when it has none
        std::function<void*(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size)> get;
        // the entity's template data of a component; null when none
        std::function<const void*(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size)> tpl;
        std::function<uint64_t()> now_ms;
        std::function<void(uint32_t npc_id, double x, double y, double z)> go_to;  // queues GoToPosition (next step)
        std::function<void(uint32_t npc_id)> drop;  // drops that villager's queued sends
    };
    struct Order {
        uint32_t villager = 0;
        std::vector<Point> points;
        double arrive = 2.0;         // kMoveArriveMin..Max (move_orders.h)
        double seconds = 0;          // 0 = no limit, at most kMoveSecondsMax
        double stuck_seconds = 0;    // 0 = 15 s, else kMoveStuckMin..Max
        double hold_seconds = 0;     // 0 = no hold, at most kMoveSecondsMax
        bool hold_forever = false;   // EMF_MOVE_HOLD: until cancelled
    };
    explicit VillagerMoves(Deps d) : d_(std::move(d)) {}

    // A move; its id (from 1, never reused), or 0 and *why. Its first point is sent at once. A move on a villager that is
    // moving or holding already ends the old one (cancelled; a hold quietly).
    uint32_t add(EmfWorld* world, const Order& o, Callback cb, std::string* why);
    // Ends a move: walking, its callback gets EMF_MOVE_CANCELLED; holding, quietly. now: at once, else at the next step.
    bool cancel(uint32_t id, bool now = true);
    bool cancel_villager(EmfWorld* world, uint32_t villager, bool now = true);
    void step(EmfWorld* world);  // after a server step of `world`
    void world_reset();          // a new server world: walking moves end cancelled (world null), holds quietly
    uint32_t moving(EmfWorld* world, uint32_t villager) const;  // walking or holding: its id; 0 none
    size_t count() const {
        std::lock_guard<std::mutex> l(m_);
        return moves_.size();
    }

private:
    struct Move {
        uint32_t id = 0, villager = 0, npc = 0;
        EmfWorld* world = nullptr;
        std::vector<Point> points;
        size_t next = 0;
        double arrive = 2.0;
        uint64_t until = 0;          // walking: 0 = no time limit
        uint64_t stuck_ms = 15000;
        uint64_t hold_ms = 0;        // 0 = no hold
        bool hold_forever = false;
        bool holding = false;
        uint64_t hold_until = 0;     // holding (not forever): when it ends
        Point home{};                // where its home was when the move began
        Point last{};                // where it stood at the last step (a jump: a teleport)
        uint64_t last_send = 0;
        uint64_t since = 0;          // when it last made progress (or was paused, or stood at its hold point)
        double best = 0;             // its closest distance to the point since then
        uint32_t sends = 0;          // sends for this point
        uint32_t struck_send = 0;    // the send a "path not found" was last counted for
        uint32_t strikes = 0;        // "path not found" for this point
        bool cancelled = false;      // ends at the next step
        Callback cb;
    };
    struct Done {
        Callback cb;
        EmfWorld* world;
        uint32_t id, status;
    };
    // Walking: its status when it ends now (or arrives), else -1. Holding: 1 when the hold ends now, else -1.
    int tick(Move& m, uint64_t now);
    int tick_hold(Move& m, uint64_t now);
    void send(Move& m, uint64_t now);           // its current point
    void stop(const Move& m);                   // drop its queued sends, then "go to where you stand"
    bool place(const Move& m, Point& out) const;
    uint32_t moving_locked(EmfWorld* world, uint32_t villager) const;
    std::string check(EmfWorld* world, const Order& o, uint32_t* npc, Point* home) const;  // "" when it may go

    Deps d_;
    mutable std::mutex m_;  // moves_: a cancel may come from another thread
    std::vector<Move> moves_;
    uint32_t next_id_ = 1;
};

}  // namespace emf::core
