// Villager moves (villager_moves.h).
#include "core/villager_moves.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/move_orders.h"
#include "emf/generated/components.hpp"

namespace emf::core {

namespace {
namespace gen = emf::gen::ecs;
using Deps = VillagerMoves::Deps;
using Point = VillagerMoves::Point;

template <class T>
T* comp(const Deps& d, EmfWorld* w, uint32_t entity) {
    uint64_t size = 0;
    void* p = d.get ? d.get(w, T::kHash, entity, &size) : nullptr;
    return p && size == sizeof(T) ? static_cast<T*>(p) : nullptr;
}
template <class T>
const T* tmpl(const Deps& d, EmfWorld* w, uint32_t entity) {
    uint64_t size = 0;
    const void* p = d.tpl ? d.tpl(w, T::kHash, entity, &size) : nullptr;
    return p && size >= sizeof(T) ? static_cast<const T*>(p) : nullptr;
}
bool has(const Deps& d, EmfWorld* w, uint32_t hash, uint32_t entity) {
    return d.get && d.get(w, hash, entity, nullptr) != nullptr;
}
// CurrentTransform's first 24 bytes: 32.32 fixed-point metres
bool place_of(const Deps& d, EmfWorld* w, uint32_t entity, Point& out) {
    uint64_t size = 0;
    const void* t = d.get ? d.get(w, gen::CurrentTransform::kHash, entity, &size) : nullptr;
    int64_t raw[3];
    if (!t || size < sizeof raw) return false;
    std::memcpy(raw, t, sizeof raw);
    for (int i = 0; i < 3; ++i) out[i] = static_cast<double>(raw[i]) / 4294967296.0;
    return true;
}
double across(const Point& a, const Point& b) { return std::hypot(a[0] - b[0], a[2] - b[2]); }
double dist3(const Point& a, const Point& b) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
Point point_of(const emf::gen::float3& f) { return {f.x, f.y, f.z}; }

// active_npc::State
constexpr uint8_t kStart = 1, kFindBed = 2, kIdle = 3, kDecideTask = 4, kFindTarget = 5, kGoToTarget = 6, kRoam = 7,
                  kSleep = 8, kWaitPlayer = 13, kInsideWater = 16, kFinishTask = 17;
constexpr uint8_t kWantsToSleep = 5;               // active_npc::Decision
constexpr uint8_t kTargetReached = 8, kNotReachable = 9;  // PathWalkerState
constexpr uint8_t kTalkBits = 0x0b;                 // NpcInteractionState: turning, greeting, playing a sequence
constexpr double kTargetSame = 0.1;                 // m: NpcCommon.currentTargetPosition is our point

// The game does not run it now (nothing sent, the stuck clock waits).
bool paused(uint8_t s, uint8_t decision, const gen::NpcCommon* c) {
    if (s == kStart || s == kFindBed || s == kSleep || s == kInsideWater || decision == kWantsToSleep) return true;
    return c && (!(c->closestPlayerDistance < kVillagerPauseM) || (c->interactionStates & kTalkBits) != 0);
}
// States that take a GoToPosition cleanly (the game switches them to Roam); others end by themselves (sitting,
// interacting, socializing, avoiding water or a build cursor). A hold never takes it from WaitingCloseToPlayer.
bool takes_order(uint8_t s, bool holding) {
    switch (s) {
    case kIdle: case kDecideTask: case kFindTarget: case kGoToTarget: case kRoam: case kFinishTask:
        return true;
    case kWaitPlayer:
        return !holding;
    default:
        return false;
    }
}
}  // namespace

void NpcSendQueue::push(uint32_t npc, double x, double y, double z) {
    std::lock_guard<std::mutex> l(m_);
    for (Send& s : q_)
        if (s.npc == npc) {
            s = {npc, x, y, z};
            return;
        }
    if (q_.size() < kNpcQueueMax) q_.push_back({npc, x, y, z});
}

void NpcSendQueue::drop(uint32_t npc) {
    std::lock_guard<std::mutex> l(m_);
    q_.erase(std::remove_if(q_.begin(), q_.end(), [npc](const Send& s) { return s.npc == npc; }), q_.end());
}

std::vector<NpcSendQueue::Send> NpcSendQueue::take() {
    std::lock_guard<std::mutex> l(m_);
    std::vector<Send> out;
    out.swap(q_);
    return out;
}

void NpcSendQueue::put_back(std::vector<Send> rest) {
    std::lock_guard<std::mutex> l(m_);
    std::vector<Send> merged;
    for (const Send& s : rest) {
        const bool newer = std::any_of(q_.begin(), q_.end(), [&](const Send& n) { return n.npc == s.npc; });
        if (!newer) merged.push_back(s);
    }
    merged.insert(merged.end(), q_.begin(), q_.end());
    if (merged.size() > kNpcQueueMax) merged.resize(kNpcQueueMax);
    q_.swap(merged);
}

void NpcSendQueue::clear() {
    std::lock_guard<std::mutex> l(m_);
    q_.clear();
}

size_t NpcSendQueue::size() const {
    std::lock_guard<std::mutex> l(m_);
    return q_.size();
}

std::string VillagerMoves::check(EmfWorld* w, const Order& o, uint32_t* npc, Point* home) const {
    if (!o.villager) return "no villager";
    if (has(d_, w, gen::PlayerState::kHash, o.villager)) return "that is a player";
    if (!comp<gen::DynamicActiveNpcState>(d_, w, o.villager)) return "it is no villager (it has no DynamicActiveNpcState)";
    const auto* t = tmpl<gen::ActiveNpcState>(d_, w, o.villager);
    if (!t || t->npcId.value == 0) return "it has no villager template (ActiveNpcState with an npcId)";
    const auto* saved = comp<gen::ActiveNpcStateSaved>(d_, w, o.villager);
    if (!saved) return "it has no villager home (ActiveNpcStateSaved)";
    if (saved->overrideRoamingRange && !(saved->roamingRange > 0)) return "the player told it to stay (roaming range 0)";
    const double area = saved->overrideRoamingRange ? saved->roamingRange : t->roamingSetup.maxRadiusFromSpawn;
    if (!(area > 0) || !std::isfinite(area)) return "it has no walk area (its roaming radius is 0)";
    const double leash = std::max(t->searchTargetSetup.targetRadius, t->searchTargetSetup.bedRadius);
    if (o.points.empty() || o.points.size() > kMovePointsMax) return "1 to 16 points";
    for (const Point& p : o.points)
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return "a point is not a number";
    if (!(o.arrive >= kMoveArriveMin && o.arrive <= kMoveArriveMax)) return "arrive must be 0.5 to 50 metres";
    if (!(o.seconds >= 0 && o.seconds <= kMoveSecondsMax)) return "seconds must be 0 (no limit) to 3600";
    if (!(o.stuck_seconds == 0 || (o.stuck_seconds >= kMoveStuckMin && o.stuck_seconds <= kMoveStuckMax)))
        return "stuck must be 2 to 120 seconds (0: 15)";
    if (!(o.hold_seconds >= 0 && o.hold_seconds <= kMoveSecondsMax)) return "hold must be 0 to 3600 seconds";
    const Point h = point_of(saved->spawnPosition);
    char buf[160];
    for (const Point& p : o.points) {
        if (leash > 0 && dist3(p, h) > leash) {
            std::snprintf(buf, sizeof buf,
                          "a point is %.0f m from the villager's home, beyond its leash (%.0f m): the game would teleport "
                          "it home",
                          dist3(p, h), leash);
            return buf;
        }
        if (across(p, h) > area + kVillagerAreaSlack) {
            std::snprintf(buf, sizeof buf,
                          "a point is %.0f m from the villager's home, outside its walk area (%.0f m around its home)",
                          across(p, h), area);
            return buf;
        }
    }
    *npc = t->npcId.value;
    *home = h;
    return "";
}

uint32_t VillagerMoves::add(EmfWorld* world, const Order& o, Callback cb, std::string* why) {
    auto no = [why](const std::string& text) {
        if (why) *why = text;
        return 0u;
    };
    uint32_t npc = 0;
    Point home{};
    const std::string bad = check(world, o, &npc, &home);
    if (!bad.empty()) return no(bad);
    Point at{};
    if (!place_of(d_, world, o.villager, at)) return no("it has no position");
    const uint64_t now = d_.now_ms ? d_.now_ms() : 0;
    std::vector<Done> done;
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> l(m_);
        const bool replaces = moving_locked(world, o.villager) != 0;
        if (!replaces && moves_.size() >= kVillagerMovesMax) return no("too many villager moves (256)");
        for (size_t i = 0; i < moves_.size(); ++i)
            if (moves_[i].villager == o.villager && moves_[i].world == world) {
                Move old = std::move(moves_[i]);
                moves_.erase(moves_.begin() + static_cast<std::ptrdiff_t>(i));
                if (d_.drop) d_.drop(old.npc);  // the new point is sent below
                if (!old.holding) done.push_back({old.cb, world, old.id, EMF_MOVE_CANCELLED});
                break;
            }
        Move m;
        m.villager = o.villager;
        m.npc = npc;
        m.world = world;
        m.points = o.points;
        m.arrive = o.arrive;
        if (o.seconds > 0) m.until = now + static_cast<uint64_t>(std::llround(o.seconds * 1000.0));
        if (o.stuck_seconds > 0) m.stuck_ms = static_cast<uint64_t>(std::llround(o.stuck_seconds * 1000.0));
        m.hold_ms = static_cast<uint64_t>(std::llround(o.hold_seconds * 1000.0));
        m.hold_forever = o.hold_forever;
        m.home = home;
        m.last = at;
        m.since = now;
        m.best = dist3(at, o.points[0]);
        m.cb = std::move(cb);
        m.id = id = next_id_++;
        send(m, now);
        moves_.push_back(std::move(m));
    }
    for (Done& x : done)
        if (x.cb) x.cb(x.world, x.id, x.status);
    return id;
}

void VillagerMoves::send(Move& m, uint64_t now) {
    const Point& p = m.points[m.next];
    if (d_.go_to) d_.go_to(m.npc, p[0], p[1], p[2]);
    m.last_send = now;
    ++m.sends;
}

void VillagerMoves::stop(const Move& m) {
    if (d_.drop) d_.drop(m.npc);
    Point at{};
    if (d_.go_to && place(m, at)) d_.go_to(m.npc, at[0], at[1], at[2]);
}

bool VillagerMoves::place(const Move& m, Point& out) const { return place_of(d_, m.world, m.villager, out); }

int VillagerMoves::tick(Move& m, uint64_t now) {
    if (m.cancelled) return EMF_MOVE_CANCELLED;
    auto* a = comp<gen::DynamicActiveNpcState>(d_, m.world, m.villager);
    const auto* t = tmpl<gen::ActiveNpcState>(d_, m.world, m.villager);
    Point at{};
    if (!a || !t || t->npcId.value != m.npc || !place(m, at)) return EMF_MOVE_GONE;
    const double jump = dist3(at, m.last);
    m.last = at;
    if (jump > kVillagerJumpM) return EMF_MOVE_STUCK;  // the game teleported it (stuck, or beyond its leash)
    const uint8_t s = static_cast<uint8_t>(a->state), prev = static_cast<uint8_t>(a->previousState);
    const uint8_t decision = static_cast<uint8_t>(a->decision);
    const auto* c = comp<gen::NpcCommon>(d_, m.world, m.villager);
    const auto* w = comp<gen::DynamicPathWalker>(d_, m.world, m.villager);
    const uint8_t ws = w ? static_cast<uint8_t>(w->state) : 0;
    const Point& p = m.points[m.next];
    const double flat = across(at, p), up = std::abs(at[1] - p[1]), dist = dist3(at, p);
    bool reached = flat <= m.arrive && up <= m.arrive + kMoveUpDown;
    if (!reached && ws == kTargetReached && s == kIdle && prev == kRoam && flat <= m.arrive + kVillagerTargetedReach &&
        up <= m.arrive + kMoveUpDown)
        reached = true;  // the walker stopped at the path's last node, up to 5 m short of a point it cannot stand on
    if (reached) {
        if (m.next + 1 >= m.points.size()) return EMF_MOVE_ARRIVED;
        ++m.next;
        m.sends = m.struck_send = m.strikes = 0;
        m.since = now;
        m.best = dist3(at, m.points[m.next]);
        send(m, now);
        return -1;
    }
    if (m.until && now >= m.until) return EMF_MOVE_TIMEOUT;
    if (paused(s, decision, c)) {  // the game does not run it now: the stuck clock waits
        m.since = now;
        m.best = dist;
        return -1;
    }
    // the path not found for this point, counted once per send (live: path result 7 for a point outside the area)
    if (w && m.sends > m.struck_send) {
        const uint8_t r = static_cast<uint8_t>(w->lastPathfindingResult);
        if (ws == kNotReachable || (r >= 2 && r <= 7)) {
            m.struck_send = m.sends;
            if (++m.strikes >= kVillagerNotReachableMax) return EMF_MOVE_STUCK;
        }
    }
    if (dist <= m.best - kMoveProgress) {
        m.best = dist;
        m.since = now;
    } else if (now - m.since >= m.stuck_ms) {
        return EMF_MOVE_STUCK;
    }
    const bool alive = s == kRoam && c && dist3(point_of(c->currentTargetPosition), p) <= kTargetSame;
    if (!alive && takes_order(s, false) && now - m.last_send >= kVillagerResendMs) send(m, now);
    return -1;
}

int VillagerMoves::tick_hold(Move& m, uint64_t now) {
    if (m.cancelled) return 1;
    if (!m.hold_forever && now >= m.hold_until) return 1;
    auto* a = comp<gen::DynamicActiveNpcState>(d_, m.world, m.villager);
    const auto* t = tmpl<gen::ActiveNpcState>(d_, m.world, m.villager);
    const auto* saved = comp<gen::ActiveNpcStateSaved>(d_, m.world, m.villager);
    Point at{};
    if (!a || !t || !saved || t->npcId.value != m.npc || !place(m, at)) return 1;
    const double jump = dist3(at, m.last);
    m.last = at;
    if (jump > kVillagerJumpM) return 1;                                                  // teleported
    if (saved->overrideRoamingRange && !(saved->roamingRange > 0)) return 1;             // the player said stay
    if (dist3(point_of(saved->spawnPosition), m.home) > kVillagerHomeMovedM) return 1;   // a new home
    const uint8_t s = static_cast<uint8_t>(a->state), decision = static_cast<uint8_t>(a->decision);
    const auto* c = comp<gen::NpcCommon>(d_, m.world, m.villager);
    const Point& p = m.points.back();
    const bool there = across(at, p) <= m.arrive && std::abs(at[1] - p[1]) <= m.arrive + kMoveUpDown;
    if (there || paused(s, decision, c) || s == kWaitPlayer) {
        m.since = now;  // at its point, or waiting (a player close by, night, a chat): the clock waits
        if (!there) return -1;
    } else if (now - m.since >= m.stuck_ms) {
        return 1;  // no way back
    }
    const bool alive = s == kRoam && c && dist3(point_of(c->currentTargetPosition), p) <= kTargetSame;
    if (!alive && takes_order(s, true) && now - m.last_send >= kVillagerHoldResendMs) send(m, now);
    return -1;
}

bool VillagerMoves::cancel(uint32_t id, bool now) {
    Done x{};
    bool call = false;
    {
        std::lock_guard<std::mutex> l(m_);
        auto it = std::find_if(moves_.begin(), moves_.end(), [&](const Move& m) { return m.id == id && !m.cancelled; });
        if (it == moves_.end()) return false;
        if (!now) {
            it->cancelled = true;
            return true;
        }
        Move m = std::move(*it);
        moves_.erase(it);
        stop(m);
        if (!m.holding) {
            x = {m.cb, m.world, m.id, EMF_MOVE_CANCELLED};
            call = true;
        }
    }
    if (call && x.cb) x.cb(x.world, x.id, x.status);
    return true;
}

bool VillagerMoves::cancel_villager(EmfWorld* world, uint32_t villager, bool now) {
    const uint32_t id = moving(world, villager);
    return id && cancel(id, now);
}

void VillagerMoves::step(EmfWorld* world) {
    const uint64_t now = d_.now_ms ? d_.now_ms() : 0;
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> l(m_);
        std::vector<Move> keep;
        keep.reserve(moves_.size());
        for (Move& m : moves_) {
            if (m.world != world) {
                keep.push_back(std::move(m));
                continue;
            }
            if (m.holding) {
                if (tick_hold(m, now) < 0) {
                    keep.push_back(std::move(m));
                } else if (m.cancelled) {
                    stop(m);
                } else if (d_.drop) {
                    d_.drop(m.npc);  // its time is over (or it went): it goes back to its routine by itself
                }
                continue;
            }
            const int status = tick(m, now);
            if (status < 0) {
                keep.push_back(std::move(m));
                continue;
            }
            if (status == EMF_MOVE_ARRIVED && (m.hold_ms > 0 || m.hold_forever)) {
                m.holding = true;
                m.hold_until = now + m.hold_ms;
                m.since = now;
                done.push_back({m.cb, world, m.id, EMF_MOVE_ARRIVED});
                keep.push_back(std::move(m));
                continue;
            }
            if (status == EMF_MOVE_CANCELLED || status == EMF_MOVE_STUCK || status == EMF_MOVE_TIMEOUT) {
                stop(m);
            } else if (d_.drop) {
                d_.drop(m.npc);
            }
            done.push_back({m.cb, world, m.id, static_cast<uint32_t>(status)});
        }
        moves_ = std::move(keep);
    }
    for (Done& x : done)
        if (x.cb) x.cb(x.world, x.id, x.status);
}

void VillagerMoves::world_reset() {
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> l(m_);
        for (const Move& m : moves_)
            if (!m.holding) done.push_back({m.cb, nullptr, m.id, EMF_MOVE_CANCELLED});
        moves_.clear();
    }
    for (Done& x : done)
        if (x.cb) x.cb(x.world, x.id, x.status);
}

uint32_t VillagerMoves::moving(EmfWorld* world, uint32_t villager) const {
    std::lock_guard<std::mutex> l(m_);
    return moving_locked(world, villager);
}

uint32_t VillagerMoves::moving_locked(EmfWorld* world, uint32_t villager) const {
    for (const Move& m : moves_)
        if (m.villager == villager && m.world == world && !m.cancelled) return m.id;
    return 0;
}

}  // namespace emf::core
