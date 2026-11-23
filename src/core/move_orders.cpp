// Move orders (move_orders.h).
#include "core/move_orders.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "emf/generated/components.hpp"

namespace emf::core {

namespace {
namespace gen = emf::gen::ecs;
using State = emf::gen::enemy01::State;

template <class T>
T* comp(const MoveOrders::Deps& d, EmfWorld* w, uint32_t entity) {
    uint64_t size = 0;
    void* p = d.get ? d.get(w, T::kHash, entity, &size) : nullptr;
    return p && size == sizeof(T) ? static_cast<T*>(p) : nullptr;
}
bool has(const MoveOrders::Deps& d, EmfWorld* w, uint32_t hash, uint32_t entity) {
    return d.get && d.get(w, hash, entity, nullptr) != nullptr;
}
// CurrentTransform's first 24 bytes: 32.32 fixed-point metres
bool place_of(const MoveOrders::Deps& d, EmfWorld* w, uint32_t entity, MoveOrders::Point& out) {
    uint64_t size = 0;
    const void* t = d.get ? d.get(w, gen::CurrentTransform::kHash, entity, &size) : nullptr;
    int64_t raw[3];
    if (!t || size < sizeof raw) return false;
    std::memcpy(raw, t, sizeof raw);
    for (int i = 0; i < 3; ++i) out[i] = static_cast<double>(raw[i]) / 4294967296.0;
    return true;
}
constexpr uint8_t kPlayerAlly = 11;

// A calm state: not fighting, hit, falling, spawning or dead.
bool calm_state(State s) {
    switch (s) {
    case State::Idle: case State::Roaming: case State::ReturnHome: case State::WaitAtHome: case State::Patrol:
    case State::TurnAfterPatrol: case State::WaitAfterPatrol: case State::Investigate: case State::ReactToNoise:
    case State::Sleep: case State::MoveToMarker:
        return true;
    default:
        return false;
    }
}
}  // namespace

uint32_t MoveOrders::add(EmfWorld* world, uint32_t enemy, const std::vector<Point>& points, double arrive,
                         double seconds, uint32_t flags, Callback cb, std::string* why, double stuck_seconds) {
    auto no = [why](const char* text) {
        if (why) *why = text;
        return 0u;
    };
    if (flags & ~static_cast<uint32_t>(EMF_MOVE_ALLIES | EMF_MOVE_PEACEFUL | EMF_MOVE_PEACEFUL_ALL | EMF_MOVE_CLEAR))
        return no("unknown flags (a newer mod?)");
    if (flags & EMF_MOVE_CLEAR) {
        if (!d_.wall_between) return no("EMF_MOVE_CLEAR: EMF cannot look for walls on this game build");
        if (const std::string w = d_.walls_why ? d_.walls_why() : std::string(); !w.empty()) {
            if (why) *why = "EMF_MOVE_CLEAR: EMF cannot look for walls now: " + w;
            return 0u;
        }
    }
    if (!(stuck_seconds == 0 || (stuck_seconds >= kMoveStuckMin && stuck_seconds <= kMoveStuckMax)))
        return no("stuck must be 2 to 120 seconds (0: 15)");
    if (points.empty() || points.size() > kMovePointsMax) return no("1 to 16 points");
    for (const Point& p : points)
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return no("a point is not a number");
    if (!(arrive >= kMoveArriveMin && arrive <= kMoveArriveMax)) return no("arrive must be 0.5 to 50 metres");
    if (!(seconds >= 0 && seconds <= kMoveSecondsMax)) return no("seconds must be 0 (no limit) to 3600");
    if (!enemy) return no("no creature");
    if (has(d_, world, gen::PlayerState::kHash, enemy)) return no("that is a player");
    auto* ai = comp<gen::DynamicEnemy01BehaviorState>(d_, world, enemy);
    if (!ai) return no("it has no enemy AI to move (DynamicEnemy01BehaviorState)");
    if (has(d_, world, gen::Boss::kHash, enemy) || has(d_, world, gen::BossHealthBar::kHash, enemy))
        return no("it is a boss: its fight and its leash stay the game's");
    if (const auto* f = comp<gen::Faction>(d_, world, enemy);
        f && static_cast<uint8_t>(f->faction) == kPlayerAlly && !(flags & EMF_MOVE_ALLIES))
        return no("it is a player's ally (a tamed beast): pass EMF_MOVE_ALLIES to move it");
    if (d_.dead && d_.dead(world, enemy)) return no("it is dead");
    const uint64_t now = d_.now_ms ? d_.now_ms() : 0;
    std::vector<Done> done;
    uint32_t id = 0;
    const char* refused = nullptr;
    {
        std::lock_guard<std::mutex> l(m_);
        Move m;
        m.enemy = enemy;
        m.world = world;
        m.flags = flags;
        m.points = points;
        m.arrive = arrive;
        m.cb = std::move(cb);
        if (stuck_seconds > 0) m.stuck_ms = static_cast<uint64_t>(std::llround(stuck_seconds * 1000.0));
        if (seconds > 0) m.until = now + static_cast<uint64_t>(std::llround(seconds * 1000.0));
        // a move on a creature that is moving already ends the old one; whom it guarded before stays the old one's
        uint32_t prior = ai->guardEntityId.id;  // whom it guards now (a stale id is dropped below)
        const bool replaces = moving_locked(world, enemy) != 0;
        if (!replaces && moves_.size() >= kMovesMax) return no("too many moves (256)");
        for (size_t i = 0; i < moves_.size(); ++i)
            if (moves_[i].enemy == enemy && moves_[i].world == world) {
                prior = moves_[i].prior_guard;
                moves_[i].prior_guard = 0;  // not given back: the new move takes it over
                Move old = std::move(moves_[i]);
                moves_.erase(moves_.begin() + static_cast<std::ptrdiff_t>(i));
                finish(old, EMF_MOVE_CANCELLED);
                done.push_back({old.cb, world, old.id, EMF_MOVE_CANCELLED});
                break;
            }
        if (prior && !alive(world, prior)) prior = 0;
        m.prior_guard = prior;
        if (place_waypoint(m, now, true)) {
            if (flags & (EMF_MOVE_PEACEFUL | EMF_MOVE_PEACEFUL_ALL))
                calm_players(world, enemy, (flags & EMF_MOVE_PEACEFUL_ALL) != 0);
            m.id = id = next_id_++;
            moves_.push_back(std::move(m));
        } else {
            refused = "its waypoint could not be spawned (spawn is off on this game build?)";
            if (!done.empty() && prior) {  // the move it replaced is gone: its guard back
                if (d_.guard) d_.guard(enemy, prior);
                ai->guardEntityId.id = prior;
            }
        }
    }
    for (Done& x : done)
        if (x.cb) x.cb(x.world, x.id, x.status);
    return refused ? no(refused) : id;
}

bool MoveOrders::place_waypoint(Move& m, uint64_t now, bool fresh) {
    const Point& p = m.points[m.next];
    const uint32_t w = d_.spawn ? d_.spawn(m.world, p[0], p[1], p[2]) : 0;
    m.last_place = now;
    if (!w) return false;
    m.waypoint = w;
    if (!place_of(d_, m.world, w, m.waypoint_at)) m.waypoint_at = p;
    send_guard(m.world, m.enemy, w);
    m.last_guard = now;
    if (fresh) {
        m.since = now;
        m.best = distance(m.world, m.enemy, p);
    }
    return true;
}

bool MoveOrders::waypoint_alive(const Move& m) const {
    Point at{};
    if (!m.waypoint || !place_of(d_, m.world, m.waypoint, at)) return false;
    for (int i = 0; i < 3; ++i)
        if (std::abs(at[i] - m.waypoint_at[i]) > 0.05) return false;  // the id names another entity now
    return true;
}

bool MoveOrders::alive(EmfWorld* w, uint32_t entity) const {
    return entity && has(d_, w, gen::CurrentTransform::kHash, entity) && !(d_.dead && d_.dead(w, entity));
}

void MoveOrders::send_guard(EmfWorld* w, uint32_t enemy, uint32_t waypoint) {
    if (d_.guard) d_.guard(enemy, waypoint);
    // a calm creature takes the guard order only from Idle (seen live: a roaming one walked on to its roam spot first)
    if (auto* ai = comp<gen::DynamicEnemy01BehaviorState>(d_, w, enemy); ai && ai->state != State::Idle && calm_state(ai->state))
        ai->state = State::Idle;
}

double MoveOrders::distance(EmfWorld* w, uint32_t enemy, const Point& p) const {
    Point at{};
    if (!place_of(d_, w, enemy, at)) return -1;
    const double dx = at[0] - p[0], dy = at[1] - p[1], dz = at[2] - p[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool MoveOrders::fighting(EmfWorld* w, uint32_t enemy) const {
    const auto* ai = comp<gen::DynamicEnemy01BehaviorState>(d_, w, enemy);
    const auto* st = comp<gen::SelectedTargets>(d_, w, enemy);
    if (!ai || !st || st->targets[0].targetEntityId.id == 0) return false;
    switch (ai->state) {  // a fight passes through these (an attack, a hit, circling, a block, a stun, a fall)
    case State::Combat: case State::ControlledByAttack: case State::Hit: case State::Circling:
    case State::BlockProjectile: case State::Stunned: case State::Falling: case State::Landing:
        return true;
    default:
        return false;
    }
}

void MoveOrders::calm_players(EmfWorld* w, uint32_t enemy, bool all) const {
    if (auto* ag = comp<gen::DynamicAggro>(d_, w, enemy))
        for (auto& t : ag->targets)
            if (t.entityId.id && (all || has(d_, w, gen::PlayerState::kHash, t.entityId.id))) {
                t.state = gen::AggroTargetState::Unknown;
                t.aggro = 0;
                t.entityId.id = 0;
            }
    if (auto* st = comp<gen::SelectedTargets>(d_, w, enemy)) {
        auto& pick = st->targets[0];
        if (pick.targetEntityId.id && (all || has(d_, w, gen::PlayerState::kHash, pick.targetEntityId.id))) {
            pick.targetEntityId.id = 0;
            pick.targetRootEntityId.id = 0;
        }
        // in Combat with nothing to fight it stands there (seen live): Idle, where it takes the guard order
        auto* ai = comp<gen::DynamicEnemy01BehaviorState>(d_, w, enemy);
        if (ai && ai->state == State::Combat && pick.targetEntityId.id == 0) ai->state = State::Idle;
    }
}

bool MoveOrders::gone(EmfWorld* w, const Move& m) const {
    if (!comp<gen::DynamicEnemy01BehaviorState>(d_, w, m.enemy)) return true;
    if ((d_.dead && d_.dead(w, m.enemy)) || has(d_, w, gen::PlayerState::kHash, m.enemy)) return true;
    const auto* f = comp<gen::Faction>(d_, w, m.enemy);
    return f && static_cast<uint8_t>(f->faction) == kPlayerAlly && !(m.flags & EMF_MOVE_ALLIES);
}

int MoveOrders::tick(Move& m, uint64_t now) {
    if (m.cancelled) return EMF_MOVE_CANCELLED;
    if (gone(m.world, m)) return EMF_MOVE_GONE;
    {
        const Point& p = m.points[m.next];
        Point at{};
        if (!place_of(d_, m.world, m.enemy, at)) return EMF_MOVE_GONE;
        const double dx = at[0] - p[0], dz = at[2] - p[2];
        double radius = m.arrive;
        const auto* ai = comp<gen::DynamicEnemy01BehaviorState>(d_, m.world, m.enemy);
        if (ai && m.waypoint && ai->guardEntityId.id == m.waypoint) {  // its guard distance to this waypoint, not an old one
            const double g = ai->currentGuardTargetDistance;  // it stops this far short of its waypoint
            if (std::isfinite(g) && g > 0 && g <= kMoveGuardMax) radius = std::max(radius, g + kMoveGuardSlack);
        }
        const bool near = std::sqrt(dx * dx + dz * dz) <= radius && std::abs(at[1] - p[1]) <= radius + kMoveUpDown;
        // v49 EMF_MOVE_CLEAR: on the point's side of any wall (a line 1 m up, clear of building blocks), asked only when
        // near; a check that could not look is not clear (it walks on, or ends stuck)
        bool clear = true;
        if (near && (m.flags & EMF_MOVE_CLEAR) && d_.wall_between) {
            const std::optional<bool> wall =
                d_.wall_between(m.world, Point{at[0], at[1] + 1.0, at[2]}, Point{p[0], p[1] + 1.0, p[2]});
            clear = wall.has_value() && !*wall;
        }
        if (near && clear) {
            if (m.next + 1 >= m.points.size()) return EMF_MOVE_ARRIVED;
            if (waypoint_alive(m) && d_.destroy) d_.destroy(m.world, m.waypoint);  // on to the next point
            m.old_waypoint = m.waypoint;
            m.waypoint = 0;
            ++m.next;
            m.replaced = 0;
            if (!place_waypoint(m, now, true)) return EMF_MOVE_STUCK;
            return -1;
        }
    }
    if (m.until && now >= m.until) return EMF_MOVE_TIMEOUT;
    if (m.flags & (EMF_MOVE_PEACEFUL | EMF_MOVE_PEACEFUL_ALL))
        calm_players(m.world, m.enemy, (m.flags & EMF_MOVE_PEACEFUL_ALL) != 0);
    // its waypoint went (destroyed, or its id is another entity's now): a new one, at most kMoveReplaceMax times
    if (!waypoint_alive(m)) {
        if (m.waypoint) m.old_waypoint = m.waypoint;
        m.waypoint = 0;
        if (now - m.last_place >= kMoveResendMs) {
            if (++m.replaced > kMoveReplaceMax || !place_waypoint(m, now, false)) return EMF_MOVE_STUCK;
        }
    }
    const double dist = distance(m.world, m.enemy, m.points[m.next]);
    if (fighting(m.world, m.enemy)) {  // a fight on the way (the game's way): the stuck clock and the guard order wait
        m.since = now;
        m.best = dist;
        m.last_guard = now;
        return -1;
    }
    if (dist >= 0 && dist <= m.best - kMoveProgress) {
        m.best = dist;
        m.since = now;
    } else if (now - m.since >= m.stuck_ms) {
        return EMF_MOVE_STUCK;
    } else if (now - m.since >= kMoveUnstackMs) {
        unstack(m, now);
    }
    auto* ai = comp<gen::DynamicEnemy01BehaviorState>(d_, m.world, m.enemy);
    // it took the waypoint as its guard target but stays calm (a fresh spawn sent farther than ~10 m, seen live: Roaming
    // or Idle, never walking; put in Guard it walked): Guard it is. Not while it goes for something it picked (it may
    // fight on the way), and at most every kMoveResendMs (another state writer is not fought every step).
    const auto* sel = comp<gen::SelectedTargets>(d_, m.world, m.enemy);
    const bool picked = sel && sel->targets[0].targetEntityId.id != 0;
    if (ai && m.waypoint && ai->guardEntityId.id == m.waypoint && calm_state(ai->state) && !picked &&
        (m.last_nudge == 0 || now - m.last_nudge >= kMoveResendMs)) {
        ai->state = State::Guard;
        m.last_nudge = now;
    }
    const bool guarding = ai && ai->state == State::Guard && ai->guardEntityId.id == m.waypoint;
    if (m.waypoint && !guarding && now - m.last_guard >= kMoveResendMs) {
        send_guard(m.world, m.enemy, m.waypoint);
        m.last_guard = now;
    }
    return -1;
}

void MoveOrders::finish(Move& m, uint32_t status) {
    if (waypoint_alive(m) && d_.destroy) d_.destroy(m.world, m.waypoint);
    if (d_.unguard) d_.unguard(m.enemy);  // its guard orders still waiting name a waypoint that is gone now
    auto* ai = comp<gen::DynamicEnemy01BehaviorState>(d_, m.world, m.enemy);
    if (!ai || status == EMF_MOVE_GONE) {
        m.waypoint = 0;
        return;
    }
    const uint32_t g = ai->guardEntityId.id;
    const bool guards_waypoint = g && (g == m.waypoint || g == m.old_waypoint);
    if (alive(m.world, m.prior_guard)) {
        if (d_.guard) d_.guard(m.enemy, m.prior_guard);  // back to whom it guarded (a tamed beast: its owner)
        ai->guardEntityId.id = m.prior_guard;             // now: a move started from the callback reads it
    } else {
        if (guards_waypoint) {  // the waypoint's id may soon be another entity's: it stops guarding
            ai->guardEntityId.id = 0;
            if (ai->state == State::Guard) ai->state = State::Idle;
        }
        if (status == EMF_MOVE_ARRIVED) {  // it lives there now (its way home leads back here)
            const Point& p = m.points.back();
            ai->homeZonePosition.x = static_cast<float>(p[0]);
            ai->homeZonePosition.y = static_cast<float>(p[1]);
            ai->homeZonePosition.z = static_cast<float>(p[2]);
        }
    }
    m.waypoint = 0;
}

bool MoveOrders::cancel(uint32_t id, bool now) {
    Done x{};
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
        finish(m, EMF_MOVE_CANCELLED);
        x = {m.cb, m.world, m.id, EMF_MOVE_CANCELLED};
    }
    if (x.cb) x.cb(x.world, x.id, x.status);
    return true;
}

bool MoveOrders::cancel_enemy(EmfWorld* world, uint32_t enemy, bool now) {
    const uint32_t id = moving(world, enemy);
    return id && cancel(id, now);
}

void MoveOrders::unstack(Move& m, uint64_t now) {
    if (!d_.nudge || m.unstacked >= kMoveUnstackMax || (m.last_unstack && now - m.last_unstack < kMoveResendMs)) return;
    Point at{};
    if (!place_of(d_, m.world, m.enemy, at)) return;
    for (const Stand& s : stand_) {
        if (s.id >= m.id || s.enemy == m.enemy) continue;  // only the later of two moves goes aside
        const double dx = s.at[0] - at[0], dz = s.at[2] - at[2];
        if (std::sqrt(dx * dx + dz * dz) > kMoveStackedM || std::abs(s.at[1] - at[1]) > 1.0) continue;
        const double a = static_cast<double>(m.id) * 2.39996323;  // the golden angle: each move its own side
        d_.nudge(m.world, m.enemy, at[0] + std::cos(a) * kMoveUnstackM, at[1], at[2] + std::sin(a) * kMoveUnstackM);
        ++m.unstacked;
        m.last_unstack = now;
        return;
    }
}

void MoveOrders::step(EmfWorld* world) {
    const uint64_t now = d_.now_ms ? d_.now_ms() : 0;
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> l(m_);
        stand_.clear();
        for (const Move& m : moves_) {
            Point at{};
            if (m.world == world && !m.cancelled && place_of(d_, world, m.enemy, at)) stand_.push_back({m.id, m.enemy, at});
        }
        std::vector<Move> keep;
        keep.reserve(moves_.size());
        for (Move& m : moves_) {
            if (m.world != world) {
                keep.push_back(std::move(m));
                continue;
            }
            const int status = tick(m, now);
            if (status < 0) {
                keep.push_back(std::move(m));
                continue;
            }
            finish(m, static_cast<uint32_t>(status));
            done.push_back({m.cb, world, m.id, static_cast<uint32_t>(status)});
        }
        moves_ = std::move(keep);
    }
    for (Done& x : done)
        if (x.cb) x.cb(x.world, x.id, x.status);
}

void MoveOrders::world_reset() {
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> l(m_);
        for (const Move& m : moves_) done.push_back({m.cb, nullptr, m.id, EMF_MOVE_CANCELLED});
        moves_.clear();
    }
    for (Done& x : done)
        if (x.cb) x.cb(x.world, x.id, x.status);
}

uint32_t MoveOrders::moving(EmfWorld* world, uint32_t enemy) const {
    std::lock_guard<std::mutex> l(m_);
    return moving_locked(world, enemy);
}

uint32_t MoveOrders::moving_locked(EmfWorld* world, uint32_t enemy) const {
    for (const Move& m : moves_)
        if (m.enemy == enemy && m.world == world && !m.cancelled) return m.id;
    return 0;
}

}  // namespace emf::core
