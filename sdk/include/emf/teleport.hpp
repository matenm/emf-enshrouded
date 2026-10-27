#pragma once
// emf/teleport.hpp — moving players and NPCs (API v20; header-only, on top of emf.hpp and query.hpp).
//
//   emf::TeleportOptions o;
//   o.ground = true;  // the game's ground search: the nearest walkable spot within about 10 m
//   emf::teleport(w, player, {x, y + 3, z}, o, [](emf::World w, const emf::TeleportResult& r) {
//       if (!r.moved()) EMF_LOG("mymod", "not moved: %s", r.reason.c_str());
//   });
//   // to the player's own respawn point, landing on one of its pads as a respawn does:
//   if (uint32_t sp = emf::player_spawn_point(w, player)) emf::teleport_to_spawn_point(w, player, sp);
//
// Server world only (the host or a dedicated server). The result comes after a later server step, on its thread.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "emf/query.hpp"

namespace emf {

inline bool has_v20() { return api() && api()->size >= offsetof(EmfApi, teleport) + sizeof(api()->teleport); }

struct TeleportOptions {
    bool ground = false;        // EMF_TELEPORT_FIND_GROUND
    std::optional<float> yaw;   // EMF_TELEPORT_FACE_YAW with this yaw in degrees; nothing = keep the orientation
    uint32_t spawn_point = 0;   // land on this PlayerSpawnPoint entity's pads (the target position is then not used)
    bool checkpoint = false;    // EMF_TELEPORT_CHECKPOINT (players)
    bool notify_ready = false;  // EMF_TELEPORT_NOTIFY_READY: f runs again (status READY) once the player can play
};

struct TeleportResult {
    uint32_t entity = 0;
    uint32_t status = EMF_TELEPORT_FAILED;  // EMF_TELEPORT_FAILED / _MOVED / _READY
    bool off_target = false;  // more than 1 m from the target (spawn point: 25 m)
    bool raw_target = false;  // ground search found nothing: it stands exactly at the target
    bool last = true;         // no further call for this request
    bool session_ended = false;  // the session ended first: do not touch the world, give nothing back, only clean up
    aim::Vec3 pos;            // where the entity is now
    double distance = 0;      // from the target
    std::string reason;       // why it failed
    bool moved() const { return status != EMF_TELEPORT_FAILED; }
};

namespace detail {
inline TeleportResult teleport_result(const EmfTeleportResult& r) {
    TeleportResult t;
    t.entity = r.entity;
    t.status = r.status;
    t.off_target = (r.flags & EMF_TELEPORT_OFF_TARGET) != 0;
    t.raw_target = (r.flags & EMF_TELEPORT_RAW_TARGET) != 0;
    t.last = (r.flags & EMF_TELEPORT_LAST) != 0;
    t.session_ended = (r.flags & EMF_TELEPORT_SESSION_ENDED) != 0;
    t.pos = {r.x, r.y, r.z};
    t.distance = r.distance;
    t.reason = r.reason ? r.reason : "";
    return t;
}
inline EmfTeleport teleport_request(uint32_t entity, const aim::Vec3& to, const TeleportOptions& o) {
    EmfTeleport t{};
    t.size = sizeof t;
    t.entity = entity;
    t.x = to.x;
    t.y = to.y;
    t.z = to.z;
    t.spawn_point = o.spawn_point;
    if (o.ground) t.flags |= EMF_TELEPORT_FIND_GROUND;
    if (o.yaw) {
        t.flags |= EMF_TELEPORT_FACE_YAW;
        t.yaw_deg = *o.yaw;
    }
    if (o.checkpoint) t.flags |= EMF_TELEPORT_CHECKPOINT;
    if (o.notify_ready) t.flags |= EMF_TELEPORT_NOTIFY_READY;
    return t;
}
}  // namespace detail

// Teleports `entity` to `to` (metres); f(World, const TeleportResult&) runs after the step that moved it (or when it
// failed), and again with status READY when o.notify_ready. With r.session_ended (a reload, back to the menu) it may run
// outside a world callback: then f must not touch the World. False when refused at once (logged; f not called) or on a
// core older than v20. See teleport in emf_api.h.
template <class F>
bool teleport(World w, uint32_t entity, const aim::Vec3& to, const TeleportOptions& o, F f) {
    if (!has_v20() || !api()->teleport) return false;
    auto* holder = new F(std::move(f));
    EmfTeleport t = detail::teleport_request(entity, to, o);
    t.cb = [](EmfWorld* world, const EmfTeleportResult* r, void* u) {
        F* fn = static_cast<F*>(u);
        const TeleportResult res = detail::teleport_result(*r);
        try {
            (*fn)(World(world), res);
        } catch (...) {
            EMF_LOG("emf::teleport", "a teleport callback threw a C++ exception");
        }
        if (res.last) delete fn;
    };
    t.user = holder;
    if (api()->teleport(w.raw(), &t)) return true;
    delete holder;
    return false;
}
inline bool teleport(World w, uint32_t entity, const aim::Vec3& to, const TeleportOptions& o = {}) {
    if (!has_v20() || !api()->teleport) return false;
    EmfTeleport t = detail::teleport_request(entity, to, o);
    return api()->teleport(w.raw(), &t);
}
// To a spawn point (a flame altar, a spire, a start location): the game lands the entity on one of its pads.
template <class F>
bool teleport_to_spawn_point(World w, uint32_t entity, uint32_t spawn_point, F f) {
    TeleportOptions o;
    o.spawn_point = spawn_point;
    return teleport(w, entity, {}, o, std::move(f));
}
inline bool teleport_to_spawn_point(World w, uint32_t entity, uint32_t spawn_point) {
    TeleportOptions o;
    o.spawn_point = spawn_point;
    return teleport(w, entity, {}, o);
}

// ---- spawn points (any core) ----
// A spawn point the game would respawn a player at: it has PlayerSpawnPoint (not "no respawn") and a position.
inline bool is_spawn_point(World w, uint32_t entity) {
    const auto* sp = w.get<gen::ecs::PlayerSpawnPoint>(entity);
    return sp && sp->target != gen::ecs::PlayerSpawnType::NoRespawn && w.get<gen::ecs::CurrentTransform>(entity);
}

// The player's own respawn point in the server world: the game keeps it in SelectedTargets slot 9 (the flame altar,
// spire or start location whose area the player last stood in; the game's own "emergency teleport" reads it there:
// docs/engine_parts/re_teleport.md). 0 when there is none or it is gone (the game then uses one it picked when the player
// joined, which mods cannot read): fall back to spawn_points_near.
inline uint32_t player_spawn_point(World w, uint32_t player) {
    if (player < 1 || player > kMaxPlayerEntity) return 0;
    const auto* st = w.get<gen::ecs::SelectedTargets>(player);
    if (!st) return 0;
    const uint32_t id = st->targets[9].targetEntityId.id;
    return id && is_spawn_point(w, id) ? id : 0;
}

// The spawn points within `radius` metres of `center`, nearest first. Walks every entity: not every tick.
inline std::vector<NearEntity> spawn_points_near(World w, const aim::Vec3& center, double radius) {
    return entities_near(w, center, radius, [&](uint32_t id) { return is_spawn_point(w, id); });
}

}  // namespace emf
