#pragma once
// villager_jobs: what the farm / tend / haul jobs see and remember (no game; unit-tested with jobs_logic.h).
// The game side is described in docs/engine_parts/re_villages.md.
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "emf/aim.hpp"

namespace jobs {

using Vec3 = emf::aim::Vec3;

// A plant root near the chest: `stage` = its current stage entity (what is harvested), ripe = fully grown.
struct Crop {
    uint32_t root = 0, stage = 0;
    Vec3 pos;
    bool ripe = false;
};
// An animal feeder and how many items it holds.
struct Feeder {
    uint32_t id = 0;
    Vec3 pos;
    uint32_t items = 0;
};
// A wild pickable near the chest (twigs, plant fibre, berries...): looting it gives its items; the game then removes it.
struct Pickable {
    uint32_t id = 0;
    Vec3 pos;
};
// A tame farm animal; has_produce = its own inventory holds something to collect.
struct Animal {
    uint32_t id = 0;
    Vec3 pos;
    bool has_produce = false;
};

// What the work tick can look up around a chest (the game wiring fills these in; empty = nothing there).
struct WorkView {
    std::function<std::vector<Crop>(const Vec3& center, double radius)> crops;
    std::function<std::vector<Feeder>(const Vec3& center, double radius)> feeders;
    std::function<std::vector<Animal>(const Vec3& center, double radius)> animals;
    std::function<std::vector<Pickable>(const Vec3& center, double radius)> pickables;
    // haul: does chest `from` hold something chest `to` takes? (empty = assume yes)
    std::function<bool(uint32_t from, uint32_t to)> haul_has_work;
    // tend: does the chest hold animal food? (empty = assume yes)
    std::function<bool(uint32_t chest)> has_food;
};

struct WorkConfig {
    double farm_radius = 20.0;       // crops within this of the chest
    double tend_radius = 20.0;       // animals and feeders within this of the chest
    double gather_radius = 20.0;     // pickables within this of the chest
    uint64_t gather_every_ms = 20000;  // a gatherer rests this long after each pickable it brought in
    uint64_t looted_forget_ms = 30000;  // a looted pickable fades out for a while: no gatherer takes it again meanwhile
    double act_radius = 6.0;         // the villager acts within this of its target (a walk order only gets it near)
    uint64_t goto_every_ms = 5000;   // walk orders are re-sent this often (villagers roam off after a few seconds)
    uint32_t haul_per_trip = 20;
    uint32_t feed_low = 5;           // a feeder with fewer items is refilled
    uint32_t feed_amount = 10;       // items per refill
    uint64_t loot_wait_ms = 2500;    // a harvest / collect whose PickupEvent does not come by then failed
    uint64_t transfer_wait_ms = 5000;  // a feed / haul whose result does not come by then failed
    uint64_t walk_give_up_ms = 60000;  // not there by then: the target is unreachable
    uint64_t skip_full_ms = 60000;     // a target whose loot did not fit is left alone this long
    uint64_t skip_far_ms = 120000;     // an unreachable target is left alone this long
    uint64_t idle_ms = 15000;          // nothing to do: look again after this
    uint64_t refuse_food_ms = 600000;  // a food kind a feeder refused is not tried there again this long
};

// The work state of one job.
struct WorkState {
    uint32_t target = 0;   // farm: root; tend: animal or feeder; haul: unused
    uint32_t stage = 0;    // farm: the stage harvested (its pickup is awaited)
    bool feeding = false;  // tend: the target is a feeder (else an animal)
    bool to_target = false;  // haul: on the way to the second chest
    bool awaiting = false;   // a harvest / collect / transfer was sent; its result is awaited
    bool sent_goto = false;
    uint64_t since_ms = 0;      // walking to the target since / awaiting since
    uint64_t last_goto_ms = 0;
    uint64_t idle_until_ms = 0;  // haul / tend: nothing to do until then
    std::map<uint32_t, uint64_t> skip;  // target id -> left alone until (ms)
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> refused_food;  // (feeder, item) -> not tried until (ms)
    uint32_t refused = 0;        // farm / gather: the target a full chest refused last (others are tried first)
    uint32_t done = 0;           // harvests / collections / items hauled
    uint32_t fed = 0;            // tend: items put into feeders
    bool full_noticed = false;
    bool empty_noticed = false;  // tend: the chest has no animal food
    bool unreach_noticed = false;  // a target could not be reached (noted once until something works)
    std::string pending_note;    // a note from a transfer result, given at the next tick
};

inline bool skipped(const WorkState& w, uint32_t id, uint64_t now_ms) {
    auto it = w.skip.find(id);
    return it != w.skip.end() && now_ms < it->second;
}

// The nearest ripe crop to `from` that is not being skipped (nullptr if none).
inline const Crop* nearest_ripe(const std::vector<Crop>& crops, const WorkState& w, const Vec3& from, uint64_t now_ms) {
    const Crop* best = nullptr;
    double best_d = 0;
    for (const auto& c : crops) {
        if (!c.ripe || !c.stage || skipped(w, c.root, now_ms)) continue;
        const double d = emf::aim::length(emf::aim::sub(c.pos, from));
        if (!best || d < best_d) {
            best = &c;
            best_d = d;
        }
    }
    return best;
}

// The nearest pickable to `from` that is not being skipped (nullptr if none).
inline const Pickable* nearest_pickable(const std::vector<Pickable>& items, const WorkState& w, const Vec3& from,
                                        uint64_t now_ms) {
    const Pickable* best = nullptr;
    double best_d = 0;
    for (const auto& p : items) {
        if (!p.id || skipped(w, p.id, now_ms)) continue;
        const double d = emf::aim::length(emf::aim::sub(p.pos, from));
        if (!best || d < best_d) {
            best = &p;
            best_d = d;
        }
    }
    return best;
}

}  // namespace jobs
