#pragma once
// Night Raids' game side, kept apart from raids.cpp so tests can run it over a fake API: who a raider is (entity ids are
// reused: a raider's id can belong to another entity a moment after it is gone), the level it gets, the destroy loop,
// and the per-world switch's mirror setting.
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"

namespace raids {

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

}  // namespace raids
