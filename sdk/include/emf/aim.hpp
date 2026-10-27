#pragma once
// emf/aim.hpp — what the player is looking at.
//
// The host's server world has the player's camera in PlayerInput.fromClient.camera (position in fixed point, an
// orientation quaternion whose +z axis is the view direction; docs/engine_parts/re_villager_jobs.md). camera_ray()
// turns it into a ray in metres, and pick() chooses the candidate point under the crosshair:
//
//   if (auto ray = emf::aim::camera_ray(w, emf::local_player())) {
//       std::vector<emf::aim::Vec3> points = ...;            // e.g. entities' positions + body height
//       if (auto i = emf::aim::pick(*ray, player_pos, points, {40.0, 1.5, 0.25})) { ... points[*i] ... }
//   }
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"

namespace emf::aim {

struct Vec3 {
    double x = 0, y = 0, z = 0;
};
struct Quat {
    double x = 0, y = 0, z = 0, w = 1;
};

inline Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 scale(const Vec3& a, double k) { return {a.x * k, a.y * k, a.z * k}; }
inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double length(const Vec3& a) { return std::sqrt(dot(a, a)); }

// v rotated by the unit quaternion q.
inline Vec3 rotate(const Quat& q, const Vec3& v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
            v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}
// The game camera's view direction: its orientation applied to +z.
inline Vec3 forward(const Quat& q) { return rotate(q, {0, 0, 1}); }

struct Ray {
    Vec3 origin;
    Vec3 dir;  // unit length
};

struct PickLimits {
    double range = 8.0;              // metres from `from` (usually the player)
    double max_ray_distance = 1.2;   // metres from the ray
    double tie = 0.25;               // closer to the ray by less than this: the nearer one along the ray wins
};

// Index of the point under the crosshair: in front of the ray's origin, within `range` of `from`, at most
// `max_ray_distance` from the ray, the closest to the ray (near ties: the nearest along it). Nothing if none.
inline std::optional<size_t> pick(const Ray& ray, const Vec3& from, const std::vector<Vec3>& points,
                                  const PickLimits& lim = {}) {
    std::optional<size_t> best;
    double best_perp = 0, best_t = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        const Vec3& p = points[i];
        if (length(sub(p, from)) > lim.range) continue;
        const Vec3 d = sub(p, ray.origin);
        const double t = dot(d, ray.dir);
        if (t <= 0) continue;
        const double perp = length(sub(d, scale(ray.dir, t)));
        if (perp > lim.max_ray_distance) continue;
        const bool better =
            !best || perp < best_perp - lim.tie || (std::fabs(perp - best_perp) <= lim.tie && t < best_t);
        if (better) {
            best = i;
            best_perp = perp;
            best_t = t;
        }
    }
    return best;
}

constexpr double kFixedPoint = 4294967296.0;  // WorldPosition: 32.32 fixed point

// A position component (WorldPosition) in metres.
inline Vec3 metres(const emf::gen::WorldPosition& p) {
    return {p.x / kFixedPoint, p.y / kFixedPoint, p.z / kFixedPoint};
}

// The player's camera ray on the host's server world (PlayerInput.fromClient.camera); nothing if the player has no
// PlayerInput there. Call it inside a world callback.
inline std::optional<Ray> camera_ray(emf::World w, uint32_t player) {
    auto* input = w.get<emf::gen::ecs::PlayerInput>(player);
    if (!input) return std::nullopt;
    const auto& cam = input->fromClient.camera;
    const Quat q{cam.orientation.x, cam.orientation.y, cam.orientation.z, cam.orientation.w};
    Vec3 dir = forward(q);
    const double len = length(dir);
    if (len < 1e-9) return std::nullopt;
    dir = scale(dir, 1.0 / len);
    return Ray{metres(cam.position), dir};
}

}  // namespace emf::aim
