// Enemy orders (enemy_orders.h).
#include "core/enemy_orders.h"

#include <algorithm>
#include <cmath>

#include "emf/generated/components.hpp"

namespace emf::core {

namespace {
namespace gen = emf::gen::ecs;

// The component when it has the generated struct's size.
template <class T>
T* comp(const EnemyOrders::Deps& d, EmfWorld* w, uint32_t entity) {
    uint64_t size = 0;
    void* p = d.get ? d.get(w, T::kHash, entity, &size) : nullptr;
    return p && size == sizeof(T) ? static_cast<T*>(p) : nullptr;
}
bool has(const EnemyOrders::Deps& d, EmfWorld* w, uint32_t hash, uint32_t entity) {
    return d.get && d.get(w, hash, entity, nullptr) != nullptr;
}
constexpr uint8_t kPlayerAlly = 11;
}  // namespace

uint32_t EnemyOrders::add(EmfWorld* world, uint32_t enemy, uint32_t target, double seconds, int32_t damage,
                          uint32_t flags, std::string* why) {
    auto no = [why](const char* text) {
        if (why) *why = text;
        return 0u;
    };
    if (flags & ~static_cast<uint32_t>(EMF_ORDER_ALLIES)) return no("unknown flags (a newer mod?)");
    if (!std::isfinite(seconds) || seconds < 0) return no("seconds must be 0 or more (0: until it ends otherwise)");
    if (damage < 0 || damage > kOrderDamageMax) return no("damage must be 0..100000");
    if (!enemy || !target || enemy == target) return no("an enemy and another entity to go for");
    if (has(d_, world, gen::PlayerState::kHash, enemy)) return no("that is a player");
    if (!comp<gen::DynamicAggro>(d_, world, enemy) || !comp<gen::SelectedTargets>(d_, world, enemy))
        return no("it has no enemy AI to send (DynamicAggro, SelectedTargets)");
    if (has(d_, world, gen::Boss::kHash, enemy) || has(d_, world, gen::BossHealthBar::kHash, enemy))
        return no("it is a boss: its fight and its leash stay the game's");
    if (const auto* f = comp<gen::Faction>(d_, world, enemy);
        f && static_cast<uint8_t>(f->faction) == kPlayerAlly && !(flags & EMF_ORDER_ALLIES))
        return no("it is a player's ally (a tamed beast): pass EMF_ORDER_ALLIES to send it");
    if (!has(d_, world, gen::CurrentTransform::kHash, target)) return no("no such target (or it has no position)");
    if (d_.dead && d_.dead(world, target)) return no("the target is dead");
    std::lock_guard<std::mutex> l(m_);
    // a new order on the same enemy ends its old one, but only once nothing can refuse the new one (issue #42): the
    // old order's slot is its own, so a replacement needs no free one, and every refusal comes before anything changes
    const auto old = std::find_if(orders_.begin(), orders_.end(),
                                  [&](const Order& x) { return x.enemy == enemy && x.world == world; });
    if (old == orders_.end() && orders_.size() >= kEnemyOrdersMax) return no("too many orders (256)");
    Order o;
    o.enemy = enemy;
    o.target = target;
    o.world = world;
    o.flags = flags;
    if (seconds > 0) o.until = (d_.now_ms ? d_.now_ms() : 0) + static_cast<uint64_t>(std::llround(seconds * 1000.0));
    if (damage > 0) {
        o.rule = d_.add_rule ? d_.add_rule(enemy, target, damage) : 0;
        if (!o.rule)  // the 64 hit rules every mod shares (hits.h), or the hit hook not usable on this build
            return no("no hit rule for its damage: all 64 shared ones are in use, or hit rules are off on this build");
    }
    if (old != orders_.end()) {
        finish(*old, true);
        orders_.erase(old);
    }
    o.id = next_id_++;
    write(world, o);
    orders_.push_back(o);
    return o.id;
}

bool EnemyOrders::write(EmfWorld* w, const Order& o) {
    auto* ag = comp<gen::DynamicAggro>(d_, w, o.enemy);
    auto* st = comp<gen::SelectedTargets>(d_, w, o.enemy);
    if (!ag || !st) return false;
    auto& pick = st->targets[0];
    if (pick.targetEntityId.id != o.target) pick.targetEntityId.id = o.target;
    if (pick.targetRootEntityId.id != o.target) pick.targetRootEntityId.id = o.target;
    if (ag->allowTargetSwitches) ag->allowTargetSwitches = false;
    // its aggro entry: the target's own, else a free slot, else the one with the lowest aggro
    gen::AggroTarget* slot = nullptr;
    for (auto& t : ag->targets)
        if (t.entityId.id == o.target) slot = &t;
    if (!slot)
        for (auto& t : ag->targets)
            if (!slot && (t.entityId.id == 0 || t.state == gen::AggroTargetState::Unknown)) slot = &t;
    if (!slot)
        for (auto& t : ag->targets)
            if (!slot || t.aggro < slot->aggro) slot = &t;
    if (slot->entityId.id != o.target) slot->entityId.id = o.target;
    if (slot->state != gen::AggroTargetState::Alarmed) slot->state = gen::AggroTargetState::Alarmed;
    if (slot->aggro < kOrderAggro) slot->aggro = kOrderAggro;
    // an enemy at rest is put in its Combat state: the game does not start a fight from what is written (seen live: an
    // idle or roaming Vukah stayed put; in Combat it walked to the door and broke it). One in an attack, hit, fall,
    // spawn or death is left as it is.
    if (auto* ai = comp<gen::DynamicEnemy01BehaviorState>(d_, w, o.enemy)) {
        using S = emf::gen::enemy01::State;
        switch (ai->state) {
        case S::Idle: case S::Investigate: case S::ReturnHome: case S::FleeHome: case S::WaitAtHome: case S::ReactToNoise:
        case S::Patrol: case S::TurnAfterPatrol: case S::WaitAfterPatrol: case S::Roaming: case S::Flee:
        case S::WaitAfterFlee: case S::Guard: case S::Sleep: case S::MoveToMarker:
            ai->state = S::Combat;
            break;
        default:
            break;
        }
    }
    return true;
}

void EnemyOrders::finish(const Order& o, bool clear) {
    if (o.rule && d_.remove_rule) d_.remove_rule(o.rule);
    if (!clear || !o.world) return;
    if (auto* ag = comp<gen::DynamicAggro>(d_, o.world, o.enemy)) {
        ag->allowTargetSwitches = true;
        for (auto& t : ag->targets)
            if (t.entityId.id == o.target) {
                t.state = gen::AggroTargetState::Unknown;
                t.aggro = 0;
                t.entityId.id = 0;
            }
    }
    if (auto* st = comp<gen::SelectedTargets>(d_, o.world, o.enemy)) {
        auto& pick = st->targets[0];
        if (pick.targetEntityId.id == o.target) {
            pick.targetEntityId.id = 0;
            pick.targetRootEntityId.id = 0;
        }
        // in Combat with nothing to fight it would stand there (Beastmaster saw it): back to Idle
        auto* ai = comp<gen::DynamicEnemy01BehaviorState>(d_, o.world, o.enemy);
        if (ai && ai->state == emf::gen::enemy01::State::Combat && pick.targetEntityId.id == 0)
            ai->state = emf::gen::enemy01::State::Idle;
    }
}

bool EnemyOrders::ended(EmfWorld* w, const Order& o, uint64_t now) const {
    if (o.cancelled || (o.until && now >= o.until)) return true;
    if (!has(d_, w, gen::CurrentTransform::kHash, o.target) || (d_.dead && d_.dead(w, o.target))) return true;
    // the enemy died (its id may soon be another creature's), became a player, or a player's ally (tamed)
    if ((d_.dead && d_.dead(w, o.enemy)) || has(d_, w, gen::PlayerState::kHash, o.enemy)) return true;
    const auto* f = comp<gen::Faction>(d_, w, o.enemy);
    return f && static_cast<uint8_t>(f->faction) == kPlayerAlly && !(o.flags & EMF_ORDER_ALLIES);
}

bool EnemyOrders::cancel(uint32_t id, bool now) {
    std::lock_guard<std::mutex> l(m_);
    for (size_t i = 0; i < orders_.size(); ++i)
        if (orders_[i].id == id) {
            if (!now) {
                orders_[i].cancelled = true;
                return true;
            }
            finish(orders_[i], true);
            orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    return false;
}

uint32_t EnemyOrders::order_of(EmfWorld* world, uint32_t enemy) const {
    std::lock_guard<std::mutex> l(m_);
    for (const Order& o : orders_)
        if (o.enemy == enemy && o.world == world && !o.cancelled) return o.id;
    return 0;
}

bool EnemyOrders::cancel_enemy(EmfWorld* world, uint32_t enemy, bool now) {
    const uint32_t id = order_of(world, enemy);
    return id && cancel(id, now);
}

void EnemyOrders::step(EmfWorld* world) {
    const uint64_t now = d_.now_ms ? d_.now_ms() : 0;
    std::lock_guard<std::mutex> l(m_);
    std::vector<Order> keep;
    keep.reserve(orders_.size());
    for (const Order& o : orders_) {
        if (o.world != world) {
            keep.push_back(o);
            continue;
        }
        if (ended(world, o, now) || !write(world, o)) {
            finish(o, true);
            continue;
        }
        keep.push_back(o);
    }
    orders_ = std::move(keep);
}

void EnemyOrders::world_reset() {
    std::lock_guard<std::mutex> l(m_);
    for (const Order& o : orders_) finish(o, false);
    orders_.clear();
}

uint32_t EnemyOrders::target_of(EmfWorld* world, uint32_t enemy) const {
    const auto* st = comp<gen::SelectedTargets>(d_, world, enemy);
    return st ? st->targets[0].targetEntityId.id : 0;
}

}  // namespace emf::core
