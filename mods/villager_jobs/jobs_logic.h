#pragma once
// villager_jobs: the decisions, without the game (unit-tested). Which villager or chest the player looks at, which
// recipes a chest can feed, when a working villager walks to its chest and crafts, how the assignment goes, and how
// jobs are saved.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "emf/aim.hpp"
#include "emf/keys.hpp"
#include "work.h"

namespace jobs {

// Geometry and the camera pick are the SDK's (emf/aim.hpp).
using Vec3 = emf::aim::Vec3;
using Quat = emf::aim::Quat;
using emf::aim::dot;
using emf::aim::forward;
using emf::aim::length;
using emf::aim::rotate;
using emf::aim::sub;

enum class Kind : uint8_t { Villager, Chest };
struct Candidate {
    uint32_t id = 0;
    Vec3 aim;  // the point the crosshair should be near (feet + a body height)
    Kind kind = Kind::Chest;
};
constexpr double kPickRange = 8.0;        // metres from the player
constexpr double kPickRayDistance = 1.2;  // metres from the camera ray
constexpr double kPickTie = 0.25;         // closer to the ray by less than this: the nearer one along the ray wins

// The candidate under the crosshair: closest to the camera ray (at most kPickRayDistance), in front of the camera,
// within kPickRange of the player.
inline std::optional<Candidate> pick_target(const Vec3& cam, const Quat& q, const Vec3& player,
                                            const std::vector<Candidate>& cands) {
    std::vector<Vec3> points;
    for (const auto& c : cands) points.push_back(c.aim);
    const auto i = emf::aim::pick({cam, forward(q)}, player, points, {kPickRange, kPickRayDistance, kPickTie});
    if (!i) return std::nullopt;
    return cands[*i];
}

// job.recall: where a stuck villager goes (EMF's teleport with the ground search): 1.5 m beside its job's chest, else
// beside its base's flame altar, else beside the player who asked. Nothing when none is known.
constexpr double kRecallBeside = 1.5;
inline std::optional<Vec3> recall_target(const std::optional<Vec3>& job_chest, const std::optional<Vec3>& altar,
                                         const std::optional<Vec3>& player) {
    for (const auto* p : {&job_chest, &altar, &player})
        if (*p) return Vec3{(*p)->x + kRecallBeside, (*p)->y, (*p)->z};
    return std::nullopt;
}

struct Stack {
    uint32_t item = 0;
    uint32_t count = 0;
};
struct Recipe {
    uint32_t id = 0;
    std::vector<Stack> inputs;   // item 0 inputs are category inputs ("any ...")
    std::vector<Stack> outputs;
    bool category_input = false;
    int64_t duration_ns = 0;
    uint64_t required_props = 0;  // RecipeInfo requiredPropsMask: station props the game checks (not offered if set)
};
using Contents = std::map<uint32_t, uint32_t>;  // item id -> count

// Every input item is in the chest in the needed amount; recipes with a category input are left out.
inline bool craftable(const Recipe& r, const Contents& c) {
    if (r.category_input || r.outputs.empty() || r.required_props) return false;
    for (const auto& in : r.inputs) {
        if (in.item == 0) return false;
        const auto it = c.find(in.item);
        if (it == c.end() || it->second < in.count) return false;
    }
    return true;
}

// What a villager does at its chest: craft, farm, tend, haul or gather.
enum class JobKind : uint8_t { Craft, Farm, Tend, Haul, Gather };
inline const char* kind_name(JobKind k) {
    switch (k) {
        case JobKind::Craft: return "craft";
        case JobKind::Farm: return "farm";
        case JobKind::Tend: return "tend";
        case JobKind::Haul: return "haul";
        case JobKind::Gather: return "gather";
    }
    return "?";
}

struct Job {
    uint32_t villager = 0, chest = 0, recipe = 0;
    // Who and where, to recognise them after a reload (entity ids can change): the villager's npcId and the chest's
    // position when the job was given. 0 / no place: not known yet (an old save), learnt on the first tick.
    uint32_t npc = 0;
    bool has_place = false;
    Vec3 place;
    JobKind kind = JobKind::Craft;
    uint32_t chest2 = 0;  // haul: the chest to carry to (`chest` is the one to carry from)
    bool has_place2 = false;
    Vec3 place2;
};
constexpr double kPlaceTolerance = 0.5;  // metres: a chest further than this from its saved place is another chest
enum class JobState : uint8_t { Working, Waiting };
// What a job knows about its recipe: the product that confirms a craft (0 = any item) and how long it takes.
struct Timing {
    uint32_t output_item = 0;
    uint64_t duration_ms = 0;
};
inline Timing job_timing(const Recipe& r) {
    return {r.outputs.empty() ? 0u : r.outputs[0].item, static_cast<uint64_t>(r.duration_ns > 0 ? r.duration_ns / 1000000 : 0)};
}
// Time per item: the configured one, but never faster than the recipe itself.
inline uint64_t job_interval(uint64_t configured_ms, const Recipe& r) {
    const uint64_t d = job_timing(r).duration_ms;
    return configured_ms > d ? configured_ms : d;
}

struct JobStatus {
    Job job;
    JobState state = JobState::Working;
    uint64_t interval_ms = 30000;
    Timing timing;
    uint64_t confirm_ms = 3000;  // the product must arrive within this after a craft
    uint64_t next_craft_ms = 0;
    uint64_t last_goto_ms = 0;
    bool went = false;  // a GoTo was sent at least once
    bool awaiting = false;
    uint64_t awaiting_since_ms = 0;
    bool waiting_noticed = false;
    uint32_t made = 0;
    WorkState work;  // farm / tend / haul
};

// What the job checks about an entity: where it is and, for a villager, its npcId.
struct EntityInfo {
    Vec3 pos;
    uint32_t npc = 0;
};
// The entity, or nullopt when it does not exist or is no longer that kind of entity (after a reload a saved id can
// name something else).
using LookupFn = std::function<std::optional<EntityInfo>(uint32_t id, Kind kind)>;

// GoTo: walk to the job's chest. GoToPos: walk to `pos`. Harvest: loot stage `target` (root `aux`) into the chest.
// Collect: loot animal `target` into the chest. Feed: move animal food from the chest into feeder `target`.
// Haul: move one item kind from the chest into chest2. Gather: loot pickable `target` into the chest.
enum class ActionKind : uint8_t { GoTo, Craft, Notice, Dropped, GoToPos, Harvest, Collect, Feed, Haul, Gather };
struct Action {
    ActionKind kind;
    Job job;
    std::string text;
    uint32_t target = 0, aux = 0;
    Vec3 pos;
};

struct JobConfig {
    double work_radius = 6.0;       // crafts only within this distance of the chest
    double goto_radius = 4.0;       // further than this: walk to the chest
    uint64_t goto_every_ms = 10000;
    uint64_t confirm_ms = 3000;     // the product must arrive within this after a craft
    WorkConfig work;
};

class JobBook {
public:
    explicit JobBook(JobConfig cfg = {}) : cfg_(cfg) {}

    // A job for the villager (replaces its old one); the first craft happens as soon as it is near the chest.
    bool assign(const Job& j, uint64_t now_ms, uint64_t interval_ms, Timing timing = {}) {
        if (!j.villager || !j.chest) return false;
        if (j.kind == JobKind::Craft && !j.recipe) return false;
        if (j.kind == JobKind::Haul && (!j.chest2 || j.chest2 == j.chest)) return false;
        stop(j.villager);
        JobStatus s;
        s.job = j;
        s.interval_ms = interval_ms ? interval_ms : 30000;
        apply_timing(s, timing);
        s.next_craft_ms = now_ms;
        s.work.since_ms = now_ms;
        jobs_.push_back(s);
        changed_ = true;
        return true;
    }
    bool stop(uint32_t villager) {
        for (size_t i = 0; i < jobs_.size(); ++i)
            if (jobs_[i].job.villager == villager) {
                jobs_.erase(jobs_.begin() + static_cast<std::ptrdiff_t>(i));
                changed_ = true;
                return true;
            }
        return false;
    }
    // The recipe's timing for a job that got none when it was given (restored before its recipe could be read).
    bool set_timing(uint32_t villager, Timing timing) {
        for (auto& s : jobs_)
            if (s.job.villager == villager) {
                apply_timing(s, timing);
                return true;
            }
        return false;
    }
    const JobStatus* find(uint32_t villager) const {
        for (const auto& s : jobs_)
            if (s.job.villager == villager) return &s;
        return nullptr;
    }
    std::vector<Job> jobs() const {
        std::vector<Job> out;
        for (const auto& s : jobs_) out.push_back(s.job);
        return out;
    }
    const std::vector<JobStatus>& status() const { return jobs_; }
    // After a load: until `ms`, a job whose villager or chest is missing or not recognised is kept (the world may
    // still be filling in) instead of dropped.
    void hold_drops_until(uint64_t ms) { hold_until_ms_ = ms; }
    // True once after jobs were added or removed (save them then).
    bool changed() {
        const bool c = changed_;
        changed_ = false;
        return c;
    }

    std::vector<Action> tick(uint64_t now_ms, const LookupFn& lookup, const WorkView& view = {}) {
        std::vector<Action> out;
        for (size_t i = 0; i < jobs_.size();) {
            JobStatus& s = jobs_[i];
            const auto vi = lookup(s.job.villager, Kind::Villager);
            const auto ci = lookup(s.job.chest, Kind::Chest);
            const bool hauls = s.job.kind == JobKind::Haul;
            const auto c2i = hauls ? lookup(s.job.chest2, Kind::Chest) : std::nullopt;
            const std::string villager = "villager " + std::to_string(s.job.villager);
            const std::string chest = "chest " + std::to_string(s.job.chest);
            std::string problem;
            if (!vi)
                problem = villager + " is gone: its job ended";
            else if (!ci)
                problem = chest + " is gone: " + villager + "'s job ended";
            else if (s.job.npc && vi->npc && vi->npc != s.job.npc)
                problem = villager + " is not the villager that had the job (ids changed): its job ended";
            else if (s.job.has_place && length(sub(ci->pos, s.job.place)) > kPlaceTolerance)
                problem = chest + " is not the chosen chest (it moved, or ids changed): " + villager + "'s job ended";
            else if (hauls && !c2i)
                problem = "chest " + std::to_string(s.job.chest2) + " is gone: " + villager + "'s job ended";
            else if (hauls && s.job.has_place2 && length(sub(c2i->pos, s.job.place2)) > kPlaceTolerance)
                problem = "chest " + std::to_string(s.job.chest2) + " is not the chosen chest (it moved, or ids changed): " +
                          villager + "'s job ended";
            if (!problem.empty()) {
                if (now_ms < hold_until_ms_) {  // just loaded: maybe not there yet
                    ++i;
                    continue;
                }
                out.push_back({ActionKind::Dropped, s.job, problem});
                jobs_.erase(jobs_.begin() + static_cast<std::ptrdiff_t>(i));
                changed_ = true;
                continue;
            }
            if (!s.job.npc && vi->npc) {  // an old save: learn who and where
                s.job.npc = vi->npc;
                changed_ = true;
            }
            if (!s.job.has_place) {
                s.job.has_place = true;
                s.job.place = ci->pos;
                changed_ = true;
            }
            if (hauls && !s.job.has_place2) {
                s.job.has_place2 = true;
                s.job.place2 = c2i->pos;
                changed_ = true;
            }
            if (s.job.kind != JobKind::Craft) {
                work_tick(s, *vi, *ci, c2i ? c2i->pos : Vec3{}, now_ms, view, out);
                ++i;
                continue;
            }
            if (s.awaiting && now_ms - s.awaiting_since_ms >= s.confirm_ms) {  // nothing came: missing materials?
                s.awaiting = false;
                s.state = JobState::Waiting;
                if (!s.waiting_noticed) {
                    s.waiting_noticed = true;
                    out.push_back({ActionKind::Notice, s.job,
                                   "villager " + std::to_string(s.job.villager) +
                                       " is waiting: the chest lacks the materials (or has no room, or the recipe is not unlocked)"});
                }
            }
            const double d = length(sub(vi->pos, ci->pos));
            if (d > cfg_.goto_radius && (!s.went || now_ms - s.last_goto_ms >= cfg_.goto_every_ms)) {
                s.went = true;
                s.last_goto_ms = now_ms;
                out.push_back({ActionKind::GoTo, s.job, ""});
            }
            if (!s.awaiting && d <= cfg_.work_radius && now_ms >= s.next_craft_ms) {
                s.awaiting = true;
                s.awaiting_since_ms = now_ms;
                s.next_craft_ms = now_ms + s.interval_ms;
                out.push_back({ActionKind::Craft, s.job, ""});
            }
            ++i;
        }
        return out;
    }

    // A product arrived in `chest` (PickupEvent): the craft of the job working from it is confirmed.
    // `item` arrived in `chest` (PickupEvent): confirms the craft of one waiting job on that chest whose recipe makes
    // it (a job without timing takes any item).
    // `from`: the entity the item came from (PickupEvent); a job whose product is unknown takes only items with no
    // source or from its villager (a craft), not loot gathered or harvested into the same chest.
    void on_output(uint32_t chest, uint32_t item, uint64_t /*now_ms*/, uint32_t from = 0) {
        for (auto& s : jobs_)
            if (s.job.chest == chest && s.awaiting && (!s.timing.output_item || s.timing.output_item == item) &&
                (s.timing.output_item || from == 0 || from == s.job.villager)) {
                s.awaiting = false;
                s.state = JobState::Working;
                s.waiting_noticed = false;
                ++s.made;
                return;
            }
    }

    // A PickupEvent: `by` received an item from `from`. A farm job that harvested stage `from` into its chest counts it and
    // returns the root to re-arm (only once, only that job's stage); a tend job collecting from animal `from` counts it;
    // a gather job that looted pickable `from` counts it and rests.
    std::vector<uint32_t> on_pickup(uint32_t by, uint32_t from, uint64_t now_ms) {
        std::vector<uint32_t> roots;
        for (auto& s : jobs_) {
            WorkState& w = s.work;
            if (!w.awaiting || s.job.chest != by || !from) continue;
            if (s.job.kind == JobKind::Farm && w.stage == from) {
                roots.push_back(w.target);
                finish_loot(w);
            } else if (s.job.kind == JobKind::Tend && !w.feeding && w.target == from) {
                finish_loot(w);
            } else if (s.job.kind == JobKind::Gather && w.target == from) {
                finish_loot(w);
                w.idle_until_ms = now_ms + cfg_.work.gather_every_ms;
                looted_[from] = now_ms + cfg_.work.looted_forget_ms;
            }
        }
        return roots;
    }

    // A feed (tend) or haul (haul) finished: `moved` items went over. nothing_to_move: the source had nothing for the
    // target (no animal food in the chest / nothing the target chest takes). Ignored unless that job awaits one.
    void on_transfer(uint32_t villager, uint32_t moved, uint64_t now_ms, bool nothing_to_move = false) {
        for (auto& s : jobs_) {
            WorkState& w = s.work;
            if (s.job.villager != villager || !w.awaiting) continue;
            if (!((s.job.kind == JobKind::Tend && w.feeding) || s.job.kind == JobKind::Haul)) return;  // not a transfer
            const WorkConfig& wc = cfg_.work;
            w.awaiting = false;
            if (s.job.kind == JobKind::Tend)
                w.fed += moved;
            else
                w.done += moved;
            const std::string v = "villager " + std::to_string(villager);
            if (s.job.kind == JobKind::Tend && w.feeding) {
                if (moved) {
                    w.empty_noticed = false;
                } else {
                    w.skip[w.target] = now_ms + wc.skip_full_ms;
                    if (nothing_to_move && !w.empty_noticed) {
                        w.empty_noticed = true;
                        w.pending_note = v + " has no animal food in chest " + std::to_string(s.job.chest) +
                                         " (items named ..._Feeding) for the feeders";
                    }
                }
                w.target = 0;
            } else if (s.job.kind == JobKind::Haul) {
                w.to_target = false;
                w.since_ms = now_ms;
                w.sent_goto = false;
                if (!moved) {
                    w.idle_until_ms = now_ms + wc.idle_ms;
                    if (!nothing_to_move && !w.full_noticed) {
                        w.full_noticed = true;
                        w.pending_note = v + " cannot put anything into chest " + std::to_string(s.job.chest2) +
                                         " (is it full?)";
                    }
                } else {
                    w.full_noticed = false;
                    w.unreach_noticed = false;
                }
            }
            return;
        }
    }
    // Feeder `feeder` refused food `item` (0 added): not tried there again for refuse_food_ms.
    void refuse_food(uint32_t villager, uint32_t feeder, uint32_t item, uint64_t now_ms) {
        for (auto& s : jobs_)
            if (s.job.villager == villager) s.work.refused_food[{feeder, item}] = now_ms + cfg_.work.refuse_food_ms;
    }
    bool food_refused(uint32_t villager, uint32_t feeder, uint32_t item, uint64_t now_ms) const {
        for (const auto& s : jobs_)
            if (s.job.villager == villager) {
                auto it = s.work.refused_food.find({feeder, item});
                return it != s.work.refused_food.end() && now_ms < it->second;
            }
        return false;
    }

private:
    void finish_loot(WorkState& w) {
        w.awaiting = false;
        w.refused = 0;
        ++w.done;
        w.full_noticed = false;
        w.unreach_noticed = false;
        w.target = 0;
        w.stage = 0;
    }
    // Walk back to the chest (when further than goto_radius), at most every goto_every_ms.
    void go_to_chest(JobStatus& s, const EntityInfo& v, const EntityInfo& c, uint64_t now_ms, std::vector<Action>& out) {
        if (length(sub(v.pos, c.pos)) > cfg_.goto_radius && (!s.went || now_ms - s.last_goto_ms >= cfg_.goto_every_ms)) {
            s.went = true;
            s.last_goto_ms = now_ms;
            out.push_back({ActionKind::GoTo, s.job, ""});
        }
    }
    // Walk to a target position, at most every goto_every_ms.
    void go_to(JobStatus& s, const Vec3& pos, uint64_t now_ms, std::vector<Action>& out) {
        WorkState& w = s.work;
        if (!w.sent_goto || now_ms - w.last_goto_ms >= cfg_.work.goto_every_ms) {
            w.sent_goto = true;
            w.last_goto_ms = now_ms;
            out.push_back({ActionKind::GoToPos, s.job, "", 0, 0, pos});
        }
    }
    // Another job works on (or awaits a result from) this entity: never two loots of one stage or animal.
    bool claimed_by_other(const JobStatus& self, uint32_t id) const {
        if (!id) return false;
        for (const auto& o : jobs_)
            if (&o != &self && (o.work.target == id || o.work.stage == id)) return true;
        return false;
    }
    // The target could not be reached in time: leave it alone for a while (noted once).
    void give_up_target(JobStatus& s, uint64_t now_ms) {
        WorkState& w = s.work;
        w.skip[w.target] = now_ms + cfg_.work.skip_far_ms;
        w.target = 0;
        if (!w.unreach_noticed) {
            w.unreach_noticed = true;
            w.pending_note = "villager " + std::to_string(s.job.villager) +
                             " cannot get close to its work (it may not leave its home area): it tries the next one";
        }
    }
    void new_target(WorkState& w, uint32_t id, uint64_t now_ms) {
        w.target = id;
        w.since_ms = now_ms;
        w.sent_goto = false;
    }
    void work_tick(JobStatus& s, const EntityInfo& v, const EntityInfo& c, const Vec3& chest2, uint64_t now_ms,
                   const WorkView& view, std::vector<Action>& out) {
        WorkState& w = s.work;
        if (!w.pending_note.empty()) {
            out.push_back({ActionKind::Notice, s.job, w.pending_note});
            w.pending_note.clear();
        }
        if (s.job.kind == JobKind::Farm) farm_tick(s, v, c, now_ms, view, out);
        if (s.job.kind == JobKind::Tend) tend_tick(s, v, c, now_ms, view, out);
        if (s.job.kind == JobKind::Haul) haul_tick(s, v, c, chest2, now_ms, view, out);
        if (s.job.kind == JobKind::Gather) gather_tick(s, v, c, now_ms, view, out);
    }
    void gather_tick(JobStatus& s, const EntityInfo& v, const EntityInfo& c, uint64_t now_ms, const WorkView& view,
                     std::vector<Action>& out) {
        WorkState& w = s.work;
        const WorkConfig& wc = cfg_.work;
        if (w.awaiting) {
            if (now_ms - w.since_ms < wc.loot_wait_ms) return;
            w.awaiting = false;  // nothing arrived: the chest is full (it refuses every pickable): pause the job
            w.skip[w.target] = now_ms + wc.skip_full_ms;
            w.idle_until_ms = now_ms + wc.skip_full_ms;
            w.refused = w.target;  // after the pause, other pickables first
            w.target = 0;
            if (!w.full_noticed) {
                w.full_noticed = true;
                out.push_back({ActionKind::Notice, s.job,
                               "villager " + std::to_string(s.job.villager) + " could not put what it gathered into chest " +
                                   std::to_string(s.job.chest) + " (is it full?)"});
            }
            return;
        }
        if (now_ms < w.idle_until_ms) {
            go_to_chest(s, v, c, now_ms, out);
            return;
        }
        for (auto it = looted_.begin(); it != looted_.end();) it = now_ms >= it->second ? looted_.erase(it) : std::next(it);
        std::vector<Pickable> items;
        if (view.pickables)
            for (const auto& x : view.pickables(c.pos, wc.gather_radius))
                if ((x.id == w.target || !claimed_by_other(s, x.id)) && !looted_.count(x.id)) items.push_back(x);
        const Pickable* cur = nullptr;
        for (const auto& x : items)
            if (x.id == w.target) cur = &x;
        if (!cur) {  // no target, or it is gone (looted by someone)
            std::vector<Pickable> others;
            for (const auto& x : items)
                if (x.id != w.refused) others.push_back(x);
            cur = nearest_pickable(others, w, v.pos, now_ms);
            if (!cur) cur = nearest_pickable(items, w, v.pos, now_ms);
            if (!cur) {
                w.target = 0;
                go_to_chest(s, v, c, now_ms, out);
                return;
            }
            if (cur->id != w.target) new_target(w, cur->id, now_ms);
        }
        if (length(sub(v.pos, cur->pos)) <= wc.act_radius) {
            out.push_back({ActionKind::Gather, s.job, "", cur->id, 0, cur->pos});
            w.awaiting = true;
            w.since_ms = now_ms;
            return;
        }
        if (now_ms - w.since_ms >= wc.walk_give_up_ms) {
            give_up_target(s, now_ms);
            return;
        }
        go_to(s, cur->pos, now_ms, out);
    }
    void tend_tick(JobStatus& s, const EntityInfo& v, const EntityInfo& c, uint64_t now_ms, const WorkView& view,
                   std::vector<Action>& out) {
        WorkState& w = s.work;
        const WorkConfig& wc = cfg_.work;
        if (w.awaiting) {
            if (now_ms - w.since_ms < (w.feeding ? wc.transfer_wait_ms : wc.loot_wait_ms)) return;
            w.awaiting = false;  // no result: a full chest (collect) or a lost transfer
            w.skip[w.target] = now_ms + wc.skip_full_ms;
            if (!w.feeding && !w.full_noticed) {
                w.full_noticed = true;
                out.push_back({ActionKind::Notice, s.job,
                               "villager " + std::to_string(s.job.villager) +
                                   " could not put what the animals made into chest " + std::to_string(s.job.chest) +
                                   " (is it full?)"});
            }
            w.target = 0;
            return;
        }
        const auto animals = view.animals ? view.animals(c.pos, wc.tend_radius) : std::vector<Animal>{};
        const auto feeders = view.feeders ? view.feeders(c.pos, wc.tend_radius) : std::vector<Feeder>{};
        // Collect first, then feed; keep the current target while it still needs the villager.
        bool found = false, feeding = false;
        uint32_t id = 0;
        Vec3 pos;
        double best = 0;
        auto consider = [&](uint32_t cand, const Vec3& p, bool is_feeder) {
            if (skipped(w, cand, now_ms) || (cand != w.target && claimed_by_other(s, cand))) return;
            const double d = cand == w.target ? -1.0 : length(sub(p, v.pos));  // the current target wins
            if (!found || d < best) {
                found = true;
                best = d;
                id = cand;
                pos = p;
                feeding = is_feeder;
            }
        };
        for (const auto& a : animals)
            if (a.has_produce) consider(a.id, a.pos, false);
        const bool food = !view.has_food || view.has_food(s.job.chest);
        bool hungry = false;
        for (const auto& f : feeders) hungry |= f.items < wc.feed_low;
        if (!found && food)
            for (const auto& f : feeders)
                if (f.items < wc.feed_low) consider(f.id, f.pos, true);
        if (hungry && !food && !w.empty_noticed) {
            w.empty_noticed = true;
            out.push_back({ActionKind::Notice, s.job,
                           "villager " + std::to_string(s.job.villager) + " has no animal food in chest " +
                               std::to_string(s.job.chest) + " (items named ..._Feeding) for the feeders"});
        }
        if (!found) {
            w.target = 0;
            go_to_chest(s, v, c, now_ms, out);
            return;
        }
        if (id != w.target) new_target(w, id, now_ms);
        w.feeding = feeding;
        if (length(sub(v.pos, pos)) <= wc.act_radius) {
            out.push_back({feeding ? ActionKind::Feed : ActionKind::Collect, s.job, "", id, 0, pos});
            w.awaiting = true;
            w.since_ms = now_ms;
            return;
        }
        if (now_ms - w.since_ms >= wc.walk_give_up_ms) {
            give_up_target(s, now_ms);
            return;
        }
        go_to(s, pos, now_ms, out);
    }
    void haul_tick(JobStatus& s, const EntityInfo& v, const EntityInfo& c, const Vec3& chest2, uint64_t now_ms,
                   const WorkView& view, std::vector<Action>& out) {
        WorkState& w = s.work;
        const WorkConfig& wc = cfg_.work;
        if (w.awaiting) {
            if (now_ms - w.since_ms < wc.transfer_wait_ms) return;
            w.awaiting = false;  // no result came: start over from the source
            w.to_target = false;
            w.since_ms = now_ms;
            w.sent_goto = false;
            return;
        }
        if (now_ms < w.idle_until_ms) {  // nothing to move: wait at the source
            w.since_ms = now_ms;  // the walk timer starts when the wait ends
            go_to_chest(s, v, c, now_ms, out);
            return;
        }
        const Vec3 dest = w.to_target ? chest2 : c.pos;
        if (length(sub(v.pos, dest)) <= wc.act_radius) {
            if (!w.to_target) {  // at the source: on to the target, if it holds something the target takes
                if (view.haul_has_work && !view.haul_has_work(s.job.chest, s.job.chest2)) {
                    w.idle_until_ms = now_ms + wc.idle_ms;
                    return;
                }
                w.to_target = true;
                w.since_ms = now_ms;
                w.sent_goto = false;
                return;
            }
            out.push_back({ActionKind::Haul, s.job, "", s.job.chest2, 0, dest});
            w.awaiting = true;
            w.since_ms = now_ms;
            return;
        }
        if (now_ms - w.since_ms >= wc.walk_give_up_ms) {  // stuck: wait, then try the trip again
            w.to_target = false;
            w.since_ms = now_ms;
            w.sent_goto = false;
            w.idle_until_ms = now_ms + wc.idle_ms;
            if (!w.unreach_noticed) {
                w.unreach_noticed = true;
                w.pending_note = "villager " + std::to_string(s.job.villager) +
                                 " cannot get close to a chest it hauls between (it may not leave its home area)";
            }
            return;
        }
        go_to(s, dest, now_ms, out);
    }
    void farm_tick(JobStatus& s, const EntityInfo& v, const EntityInfo& c, uint64_t now_ms, const WorkView& view,
                   std::vector<Action>& out) {
        WorkState& w = s.work;
        const WorkConfig& wc = cfg_.work;
        if (w.awaiting) {
            if (now_ms - w.since_ms < wc.loot_wait_ms) return;
            w.awaiting = false;  // nothing arrived: the chest is full, or the loot was refused
            w.skip[w.target] = now_ms + wc.skip_full_ms;
            w.idle_until_ms = now_ms + wc.skip_full_ms;  // a full chest refuses every plant: pause the whole job
            w.refused = w.target;                        // after the pause, other plants first
            w.target = w.stage = 0;
            if (!w.full_noticed) {
                w.full_noticed = true;
                out.push_back({ActionKind::Notice, s.job,
                               "villager " + std::to_string(s.job.villager) + " could not put the harvest into chest " +
                                   std::to_string(s.job.chest) + " (is it full?)"});
            }
            return;
        }
        if (now_ms < w.idle_until_ms) {
            go_to_chest(s, v, c, now_ms, out);
            return;
        }
        std::vector<Crop> crops;
        if (view.crops)
            for (const auto& x : view.crops(c.pos, wc.farm_radius))
                if (x.root == w.target || !claimed_by_other(s, x.root)) crops.push_back(x);
        const Crop* cur = nullptr;
        for (const auto& x : crops)
            if (x.root == w.target && x.ripe && x.stage) cur = &x;
        if (!cur) {  // no target, or it was harvested / is gone
            std::vector<Crop> others;
            for (const auto& x : crops)
                if (x.root != w.refused) others.push_back(x);
            cur = nearest_ripe(others, w, v.pos, now_ms);
            if (!cur) cur = nearest_ripe(crops, w, v.pos, now_ms);
            if (!cur) {
                w.target = 0;
                go_to_chest(s, v, c, now_ms, out);
                return;
            }
            if (cur->root != w.target) new_target(w, cur->root, now_ms);
        }
        if (length(sub(v.pos, cur->pos)) <= wc.act_radius) {
            out.push_back({ActionKind::Harvest, s.job, "", cur->stage, cur->root, cur->pos});
            w.awaiting = true;
            w.stage = cur->stage;
            w.since_ms = now_ms;
            return;
        }
        if (now_ms - w.since_ms >= wc.walk_give_up_ms) {  // cannot get there
            give_up_target(s, now_ms);
            return;
        }
        go_to(s, cur->pos, now_ms, out);
    }
    void apply_timing(JobStatus& s, Timing timing) const {
        if (s.interval_ms < timing.duration_ms) s.interval_ms = timing.duration_ms;
        s.timing = timing;
        s.confirm_ms = cfg_.confirm_ms > timing.duration_ms + 2000 ? cfg_.confirm_ms : timing.duration_ms + 2000;
    }
    JobConfig cfg_;
    std::vector<JobStatus> jobs_;
    std::map<uint32_t, uint64_t> looted_;  // pickable -> not gathered again until (ms): it fades out after a loot
    bool changed_ = false;
    uint64_t hold_until_ms_ = 0;
};

inline std::string serialize(const std::vector<Job>& jobs) {
    std::string out;
    char line[192];
    for (const auto& j : jobs) {
        if (j.kind == JobKind::Haul)
            snprintf(line, sizeof(line), "haul %u %u %u %.2f %.2f %.2f %u %.2f %.2f %.2f\n", j.villager, j.chest, j.npc,
                     j.place.x, j.place.y, j.place.z, j.chest2, j.place2.x, j.place2.y, j.place2.z);
        else if (j.kind != JobKind::Craft)
            snprintf(line, sizeof(line), "%s %u %u %u %.2f %.2f %.2f\n", kind_name(j.kind), j.villager, j.chest, j.npc,
                     j.place.x, j.place.y, j.place.z);
        else if (j.has_place)
            snprintf(line, sizeof(line), "%u %u %08x %u %.2f %.2f %.2f\n", j.villager, j.chest, j.recipe, j.npc,
                     j.place.x, j.place.y, j.place.z);
        else if (j.npc)
            snprintf(line, sizeof(line), "%u %u %08x %u\n", j.villager, j.chest, j.recipe, j.npc);
        else
            snprintf(line, sizeof(line), "%u %u %08x\n", j.villager, j.chest, j.recipe);
        out += line;
    }
    return out;
}

inline std::vector<Job> parse(const std::string& text) {
    std::vector<Job> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        Job j;
        char extra = 0;
        bool ok = false;
        char word[8] = {};
        if (sscanf_s(line.c_str(), "%7s", word, 8u) == 1 && (word[0] < '0' || word[0] > '9')) {  // farm / tend / haul
            const std::string w = word;
            if (w == "haul") {
                j.kind = JobKind::Haul;
                ok = sscanf_s(line.c_str(), "haul %u %u %u %lf %lf %lf %u %lf %lf %lf %c", &j.villager, &j.chest, &j.npc,
                              &j.place.x, &j.place.y, &j.place.z, &j.chest2, &j.place2.x, &j.place2.y, &j.place2.z,
                              &extra, 1u) == 10 &&
                     j.chest2;
                j.has_place = j.has_place2 = ok;
            } else if (w == "farm" || w == "tend" || w == "gather") {
                j.kind = w == "farm" ? JobKind::Farm : w == "tend" ? JobKind::Tend : JobKind::Gather;
                const std::string fmt = w + " %u %u %u %lf %lf %lf %c";
                ok = sscanf_s(line.c_str(), fmt.c_str(), &j.villager, &j.chest, &j.npc, &j.place.x, &j.place.y,
                              &j.place.z, &extra, 1u) == 6;
                j.has_place = ok;
            }
            if (ok && j.villager && j.chest) out.push_back(j);
            continue;
        }
        if (sscanf_s(line.c_str(), "%u %u %x %u %lf %lf %lf %c", &j.villager, &j.chest, &j.recipe, &j.npc, &j.place.x,
                     &j.place.y, &j.place.z, &extra, 1u) == 7) {
            j.has_place = true;
            ok = true;
        } else {
            j = Job{};
            if (sscanf_s(line.c_str(), "%u %u %x %u %c", &j.villager, &j.chest, &j.recipe, &j.npc, &extra, 1u) == 4) {
                ok = true;
            } else {
                j = Job{};
                ok = sscanf_s(line.c_str(), "%u %u %x %c", &j.villager, &j.chest, &j.recipe, &extra, 1u) == 3;
            }
        }
        if (ok && j.villager && j.chest && j.recipe) out.push_back(j);
    }
    return out;
}

// The jobs key setting: emf::keys::parse_key (letters, digits, key names, keyboard virtual-key numbers).
using emf::keys::parse_key;

// The assignment flow behind the jobs key: a villager, then a chest (within 30 s) opens the recipe wheel; a villager
// that already works opens the stop wheel.
class Assigner {
public:
    static constexpr uint64_t kChooseMs = 30000;
    enum class Step { Idle, VillagerChosen };
    struct Result {
        enum Kind { None, Notice, OpenJobWheel, OpenStopWheel, AssignHaul } kind = None;
        uint32_t villager = 0, chest = 0, chest2 = 0;
        std::string text;
    };
    explicit Assigner(std::string key_name) : key_(std::move(key_name)) {}

    Result press(const std::optional<Candidate>& target, bool villager_has_job, uint64_t now_ms) {
        Result r;
        const bool chosen = step(now_ms) == Step::VillagerChosen;
        if (haul_from_ && now_ms - haul_at_ < kChooseMs) {  // "Haul to another chest": the target chest
            if (target && target->kind == Kind::Villager) {
                reset();
                r.kind = Result::Notice;
                r.text = "Haul cancelled";
                return r;
            }
            if (!target || target->kind != Kind::Chest) {
                r.kind = Result::Notice;
                r.text = "Look at the chest to haul to and press " + key_ + " (" + key_ + " at a villager cancels)";
                return r;
            }
            if (target->id == haul_from_) {
                r.kind = Result::Notice;
                r.text = "That is the chest it hauls from: look at another chest and press " + key_;
                return r;
            }
            r.kind = Result::AssignHaul;
            r.villager = haul_villager_;
            r.chest = haul_from_;
            r.chest2 = target->id;
            reset();
            return r;
        }
        if (!target) {
            r.kind = Result::Notice;
            r.text = chosen ? "Look at a chest and press " + key_ : "Look at a villager and press " + key_;
            return r;
        }
        if (target->kind == Kind::Villager) {
            if (villager_has_job) {
                reset();
                r.kind = Result::OpenStopWheel;
                r.villager = target->id;
                return r;
            }
            villager_ = target->id;
            chosen_at_ = now_ms;
            r.kind = Result::Notice;
            r.villager = target->id;
            r.text = "Villager " + std::to_string(target->id) + " chosen: look at a chest and press " + key_;
            return r;
        }
        if (!chosen) {
            r.kind = Result::Notice;
            r.text = "Choose a villager first: look at one and press " + key_;
            return r;
        }
        r.kind = Result::OpenJobWheel;
        r.villager = villager_;
        r.chest = target->id;
        return r;
    }
    // The job wheel's "Haul to another chest": the next chest press (within 30 s) names the target.
    void want_haul_target(uint32_t villager, uint32_t from, uint64_t now_ms) {
        villager_ = 0;
        haul_villager_ = villager;
        haul_from_ = from;
        haul_at_ = now_ms;
    }
    Step step(uint64_t now_ms) const {
        return villager_ && now_ms - chosen_at_ < kChooseMs ? Step::VillagerChosen : Step::Idle;
    }
    uint32_t villager() const { return villager_; }
    void reset() {
        villager_ = 0;
        chosen_at_ = 0;
        haul_villager_ = haul_from_ = 0;
        haul_at_ = 0;
    }

private:
    std::string key_;
    uint32_t villager_ = 0;
    uint64_t chosen_at_ = 0;
    uint32_t haul_villager_ = 0, haul_from_ = 0;
    uint64_t haul_at_ = 0;
};

}  // namespace jobs
