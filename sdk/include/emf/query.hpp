#pragma once
// emf/query.hpp — where entities are, and which are near a point (header-only, on top of emf.hpp).
//
//   if (auto p = emf::position(w, emf::local_player()))
//       for (const auto& n : emf::entities_near(w, *p, 10.0, [&](uint32_t id) { return w.get<emf::gen::ecs::Health>(id); }))
//           EMF_LOG("mymod", "%u is %.1f m away", n.id, n.distance);
//
// Call these inside a world callback (on_tick, run_on_world, on_event, a command), on the world it passed you.
// entities_near walks every entity of the world: fine a few times a second, not thousands of times per tick.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "emf/aim.hpp"
#include "emf/emf.hpp"
#include "emf/generated/components.hpp"

namespace emf {

// An entity's position in metres (its CurrentTransform), or nothing if it has none.
inline std::optional<aim::Vec3> position(World w, uint32_t entity) {
    auto* t = w.get<gen::ecs::CurrentTransform>(entity);
    if (!t) return std::nullopt;
    return aim::metres(t->transform.position);
}

inline double distance(const aim::Vec3& a, const aim::Vec3& b) { return aim::length(aim::sub(a, b)); }

struct NearEntity {
    uint32_t id = 0;
    aim::Vec3 pos;
    double distance = 0;
};

// The entities within `radius` metres of `center` for which pred(id) is true, nearest first.
template <class Pred>
std::vector<NearEntity> entities_near(World w, const aim::Vec3& center, double radius, Pred&& pred) {
    std::vector<NearEntity> out;
    w.for_each_entity([&](uint32_t id) {
        const auto p = position(w, id);
        if (!p) return;
        const double d = distance(*p, center);
        if (d <= radius && pred(id)) out.push_back({id, *p, d});
    });
    std::sort(out.begin(), out.end(), [](const NearEntity& a, const NearEntity& b) { return a.distance < b.distance; });
    return out;
}
inline std::vector<NearEntity> entities_near(World w, const aim::Vec3& center, double radius) {
    return entities_near(w, center, radius, [](uint32_t) { return true; });
}

// Players are the entities 1..1023 (BasicPlayerInfos has one 0xA0-byte entry per player entity, at index id - 1).
constexpr uint32_t kMaxPlayerEntity = 1023;

// The players in a server world, lowest id first: the host and every guest (on a dedicated server all of them; there
// emf::local_player() is 0). Ids 1..1023 with PlayerState and a position.
inline std::vector<uint32_t> players(World w) {
    std::vector<uint32_t> out;
    for (uint32_t id = 1; id <= kMaxPlayerEntity; ++id)
        if (w.get<gen::ecs::PlayerState>(id) && w.get<gen::ecs::CurrentTransform>(id)) out.push_back(id);
    return out;
}

namespace detail {
inline bool copy_guarded(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
        return false;
    }
}
}  // namespace detail

// A player's account hash (from BasicPlayerInfos: the entry's entity id at +0, the account at +8), the value
// emf::command_sender reports for them; it stays the same across sessions (entity ids do not). 0 if unknown.
inline uint64_t player_account(World w, uint32_t player) {
    if (player < 1 || player > kMaxPlayerEntity) return 0;
    const auto* infos = static_cast<const uint8_t*>(w.singleton("BasicPlayerInfos"));
    uint8_t head[16];
    if (!infos || !detail::copy_guarded(head, infos + static_cast<size_t>(player - 1) * 0xA0, sizeof head)) return 0;
    uint32_t held = 0;
    uint64_t account = 0;
    std::memcpy(&held, head, 4);
    std::memcpy(&account, head + 8, 8);
    return held == player ? account : 0;
}

}  // namespace emf
