#pragma once
// Night Raids' game side, kept apart from raids.cpp so tests can run it over a fake API: who a raider is (entity ids are
// reused: a raider's id can belong to another entity a moment after it is gone), the level it gets, the destroy loop,
// and the per-world switch's mirror setting.
#include <cstdint>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "raid_logic.h"

namespace raids {

// Health left (the replicated health, else the first Health slot: the current health in game tests).
inline bool has_health(emf::World w, uint32_t id) {
    if (auto* n = w.get<emf::gen::ecs::NetworkHealth>(id)) return n->health > 0;
    auto* h = w.get<emf::gen::ecs::Health>(id);
    return h && h->dataStorage[0] > 0;
}

// A boss or a mini-boss: it carries Boss or a boss health bar (as Beastmaster and Necromancer tell them).
inline bool is_boss(emf::World w, uint32_t id) {
    return w.get<emf::gen::ecs::Boss>(id) != nullptr || w.get<emf::gen::ecs::BossHealthBar>(id) != nullptr;
}

// A fighter on the players' side that is not a player: the companion, a summon, a tamed beast. Its faction is the
// players' side (Player, AutomatedPlayer, PlayerAlly) and it runs the enemy AI that fights (they all guard their owner
// through it). Baby farm animals are PlayerAlly too, but have no such AI; villagers (their template has ActiveNpcState,
// as the companion's Attack order tells them) never count.
inline bool is_ally(emf::World w, uint32_t id) {
    namespace ecs = emf::gen::ecs;
    using F = emf::gen::FactionType;
    if (w.get<ecs::PlayerState>(id)) return false;
    const EmfApi* a = emf::api();
    if (a && a->get_template_component && a->get_template_component(w.raw(), ecs::ActiveNpcState::kHash, id, nullptr))
        return false;
    auto* f = w.get<ecs::Faction>(id);
    if (!f || (f->faction != F::Player && f->faction != F::AutomatedPlayer && f->faction != F::PlayerAlly)) return false;
    return w.get<ecs::DynamicEnemy01BehaviorState>(id) != nullptr;
}

// The allies within `radius` metres of `center` (horizontally), in the world's order. Walks every entity: a few times a
// minute, not every step.
inline std::vector<Foe> allies_near(emf::World w, const Vec3& center, double radius) {
    std::vector<Foe> out;
    w.for_each_entity([&](uint32_t id) {
        auto* t = w.get<emf::gen::ecs::CurrentTransform>(id);
        if (!t) return;
        constexpr double kFixed = 4294967296.0;
        const Vec3 p{t->transform.position.x / kFixed, t->transform.position.y / kFixed, t->transform.position.z / kFixed};
        if (horizontal_distance(p, center) > radius || !is_ally(w, id)) return;
        out.push_back({id, p, false, has_health(w, id)});
    });
    return out;
}

struct RaiderId {
    uintptr_t tpl = 0;   // its template's Enemy01BehaviorState data (every entity of a template shares it); 0 = unknown
    uint32_t level = 0;  // the level written on it; 0 = not written yet
};

inline RaiderId identify(emf::World w, uint32_t id) {
    RaiderId r;
    const EmfApi* a = emf::api();
    if (a && a->get_template_component)
        r.tpl = reinterpret_cast<uintptr_t>(
            a->get_template_component(w.raw(), emf::gen::ecs::Enemy01BehaviorState::kHash, id, nullptr));
    return r;
}

// Still the raider we spawned: it exists, its template is the one it had, and its level the one we wrote.
inline bool is_raider(emf::World w, uint32_t id, const RaiderId& r) {
    if (!w.get<emf::gen::ecs::CurrentTransform>(id)) return false;
    if (r.tpl && identify(w, id).tpl != r.tpl) return false;
    if (r.level) {
        auto* l = w.get<emf::gen::ecs::Level>(id);
        if (!l || l->dataStorage[0] != r.level) return false;
    }
    return true;
}

// The raider's level, then a rescale at once (DynamicEnemyScaling.nextUpdateTime = 0), as the companion does (health
// 119 -> 417 at level 10). False while it has no Level yet.
inline bool write_level(emf::World w, uint32_t id, uint32_t level) {
    auto* l = w.get<emf::gen::ecs::Level>(id);
    if (!l) return false;
    l->dataStorage[0] = level;
    if (auto* d = w.get<emf::gen::ecs::DynamicEnemyScaling>(id)) d->nextUpdateTime.value = 0;
    return true;
}

// Destroys the raiders among `ids` that still exist and are still ours (never an entity that took a gone raider's id).
inline int destroy_raiders(emf::World w, const std::vector<uint32_t>& ids, const std::map<uint32_t, RaiderId>& known) {
    int n = 0;
    for (uint32_t id : ids) {
        auto it = known.find(id);
        if (it == known.end() || !is_raider(w, id, it->second)) continue;
        if (emf::api()->destroy(w.raw(), id)) ++n;
    }
    return n;
}

// The per-world switch's mirror setting: the new value of the world's switch when the setting (-1 = not declared) was
// changed while the world's file is loaded, else nothing.
inline std::optional<bool> switch_step(bool file_ready, int64_t setting, bool world_on) {
    if (!file_ready || setting < 0) return std::nullopt;
    const bool want = setting != 0;
    if (want == world_on) return std::nullopt;
    return want;
}

// ---- walls ("Raiders break walls", EMF API v44) ----
inline bool walls_available() { return emf::has_v44(); }

// The wall search's line: from the raider's chest (1.2 m up), level, toward the altar, `reach` metres long.
inline std::pair<Vec3, Vec3> chest_line(const Vec3& pos, const Vec3& altar, double reach) {
    const Vec3 from{pos.x, pos.y + WallWork::kChest, pos.z};
    const double dx = altar.x - pos.x, dz = altar.z - pos.z, len = std::hypot(dx, dz);
    if (len < 1e-6) return {from, from};
    return {from, Vec3{from.x + dx / len * reach, from.y, from.z + dz / len * reach}};
}

// The first door point (move 5) of a path within `range` metres across of the raider: its door and where it is.
inline std::optional<std::pair<uint32_t, Vec3>> nearest_gate(const EmfPathPoint* pts, uint32_t count, const uint32_t* doors,
                                                             const Vec3& raider, double range) {
    if (!pts || !doors) return std::nullopt;
    for (uint32_t i = 0; i < count; ++i) {
        if (pts[i].move != 5 || !doors[i]) continue;
        const Vec3 p{pts[i].x, pts[i].y, pts[i].z};
        if (horizontal_distance(p, raider) <= range) return std::make_pair(doors[i], p);
    }
    return std::nullopt;
}

// A door's most health (its template's Destructible), 0 = unknown.
inline int32_t door_max_health(emf::World w, uint32_t door) {
    const EmfApi* a = emf::api();
    if (!a || !a->get_template_component) return 0;
    const auto* d = static_cast<const uint16_t*>(
        a->get_template_component(w.raw(), emf::gen::ecs::Destructible::kHash, door, nullptr));
    return d ? *d : 0;
}
// A door's health now: its Health once it was hit (slot 0), else its most; 0 = unknown.
inline int32_t door_health(emf::World w, uint32_t door) {
    if (auto* h = w.get<emf::gen::ecs::Health>(door)) return h->dataStorage[0];
    return door_max_health(w, door);
}
// The gate is gone: its entity is, or the id is no door any more (a new entity took a broken gate's id).
inline bool gate_gone(emf::World w, uint32_t door) {
    if (!w.get<emf::gen::ecs::CurrentTransform>(door)) return true;
    const EmfApi* a = emf::api();
    return emf::has_v38() && a->door_state && a->door_state(w.raw(), door) == EMF_DOOR_NONE;
}
// The last hit: the gate is taken away (remove_building) once its health is at or below one hit's damage.
inline bool gate_finish_due(int32_t health, int32_t per_hit) { return health > 0 && health <= per_hit; }
// Takes the gate away like a pick-up; its item drops at its spot when `drops` (the "Broken blocks drop items" setting).
inline bool remove_gate(emf::World w, uint32_t door, bool drops, std::string* why) {
    const EmfApi* a = emf::api();
    if (!emf::has_v36() || !a->remove_building) {
        if (why) *why = "this EMF core is too old (remove_building needs API v36)";
        return false;
    }
    EmfRemoveBuilding r{};
    r.size = sizeof r;
    r.entity = door;
    r.refund = drops ? EMF_REFUND_DROP : EMF_REFUND_NONE;
    r.flags = EMF_REMOVE_FORCE;
    const bool ok = a->remove_building(w.raw(), &r);
    if (!ok && why) *why = r.why;
    return ok;
}
// A player within `range` metres across: a raider there is fighting, not held up by a wall.
inline bool player_near(const Vec3& p, const std::vector<Player>& players, double range) {
    for (const auto& pl : players)
        if (horizontal_distance(pl.pos, p) <= range) return true;
    return false;
}
// One raider hit on a wall: the game's T4 pickaxe (EMF_BREAK_DEFAULT_DAMAGE) times scale, whole, at most 100000.
inline void wall_hit_damage(double scale, int32_t out[5]) {
    const int32_t def[5] = EMF_BREAK_DEFAULT_DAMAGE;
    for (int i = 0; i < 5; ++i) out[i] = static_cast<int32_t>((std::min)(100000.0, std::round(def[i] * scale)));
}

}  // namespace raids
