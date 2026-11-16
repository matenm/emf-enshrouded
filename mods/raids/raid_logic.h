#pragma once
// Night Raids: the raid itself without the game (unit-tested in tests/test_raids.cpp). The mod (raids.cpp) feeds it a
// Tick about once a second (the time of day, the bases with players, who is near) and carries out the actions it
// returns: notes, the HUD line, spawning a wave, a reward, destroying survivors. Spawned raiders are not saved by the
// game, so a reload (a new session) simply ends the raid.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "emf/emf.hpp"

namespace raids {

struct Vec3 {
    double x = 0, y = 0, z = 0;
};
inline double horizontal_distance(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.z - b.z); }

// ---- the night ----
// True when the clock went from `prev` to `now` (hours, wrapping at midnight) past `hour`. A jump of 12 hours or more
// (the time command going back, or into the next day) is no crossing.
inline bool crossed(double prev, double now, double hour) {
    double span = now - prev;
    if (span < 0) span += 24.0;
    if (span <= 0 || span >= 12.0) return false;
    double h = hour - prev;
    if (h <= 0) h += 24.0;
    return h <= span;
}
// Night: from the start hour until dawn.
inline bool is_night(double h, double start_hour, double dawn_hour) {
    return start_hour > dawn_hour ? (h >= start_hour || h < dawn_hour) : (h >= start_hour && h < dawn_hour);
}

// ---- bases and players ----
struct Base {
    uint32_t altar = 0;  // the flame altar entity (BaseIdComponent)
    Vec3 pos;
};
struct Player {
    uint32_t id = 0;
    Vec3 pos;
    uint32_t level = 1;
};
struct Occupied {
    Base base;
    std::vector<Player> players;  // inside the base
};

// "Inside a base": within `radius` metres of its altar, horizontally. BaseIdComponent.buildZoneSize read 0 in game,
// so the radius is a setting, not the game's zone.
inline bool inside_any_base(const Vec3& p, const std::vector<Base>& bases, double radius) {
    for (const auto& b : bases)
        if (horizontal_distance(p, b.pos) <= radius) return true;
    return false;
}
inline std::vector<Occupied> occupied_bases(const std::vector<Base>& bases, const std::vector<Player>& players,
                                            double radius) {
    std::vector<Occupied> out;
    for (const auto& b : bases) {
        Occupied o;
        o.base = b;
        for (const auto& p : players)
            if (horizontal_distance(p.pos, b.pos) <= radius) o.players.push_back(p);
        if (!o.players.empty()) out.push_back(o);
    }
    return out;
}
inline int players_near(const Vec3& center, const std::vector<Player>& players, double radius) {
    int n = 0;
    for (const auto& p : players) n += horizontal_distance(p.pos, center) <= radius ? 1 : 0;
    return n;
}
inline uint32_t strongest_level(const Occupied& o) {
    uint32_t best = 0;
    for (const auto& p : o.players) best = (std::max)(best, p.level);
    return best;
}

enum class BasePick { Busiest, Random };
inline BasePick base_pick(const std::string& v) { return v == "random" ? BasePick::Random : BasePick::Busiest; }
// The base a raid goes to: the one with the most players (the first of equals), or one at random (roll).
inline const Occupied* pick_base(const std::vector<Occupied>& o, BasePick pick, uint32_t roll) {
    if (o.empty()) return nullptr;
    if (pick == BasePick::Random) return &o[roll % o.size()];
    const Occupied* best = &o[0];
    for (const auto& x : o)
        if (x.players.size() > best->players.size()) best = &x;
    return best;
}

// ---- where raiders appear ----
// Places players stood on (sampled while they walk outside bases): walkable ground at a known height, so a raider put a
// metre above one lands on it. One spot per few metres, the newest 512.
class Breadcrumbs {
public:
    static constexpr size_t kMax = 512;
    static constexpr double kMinStep = 3.0;
    void add(const Vec3& p, uint64_t now_ms) {
        for (const auto& c : spots_)
            if (horizontal_distance(c.pos, p) < kMinStep && std::fabs(c.pos.y - p.y) < kMinStep) return;
        if (spots_.size() >= kMax) spots_.erase(spots_.begin());
        spots_.push_back({p, now_ms});
    }
    size_t size() const { return spots_.size(); }
    void clear() { spots_.clear(); }
    template <class F>
    void for_each(F&& f) const {
        for (const auto& c : spots_) f(c.pos);
    }

private:
    struct Spot {
        Vec3 pos;
        uint64_t at_ms;
    };
    std::vector<Spot> spots_;
};

// How far from the altar raiders gather: 40-60 m (seen in game: spawned 40 m out, a guard order marches them in), and
// always at least 5 m beyond the base radius, so never inside the base.
struct Ring {
    double min = 40, max = 60;
};
inline Ring ring_for(double base_radius) {
    Ring r;
    r.min = (std::max)(40.0, base_radius + 5.0);
    r.max = r.min + 20.0;
    return r;
}

inline uint32_t next_random(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}

// `count` spawn points around `center`: a walked spot from a band around the ring (from just outside the base to 40 m
// beyond the ring), moved along its own direction onto the ring, at its own height + 1 m; else a point of the ring at the
// altar's height + 2 m (seen in game: a raider put there lands and walks); the rest within a few metres of the first.
// Never inside any base. Empty when the whole ring is covered by bases.
inline std::vector<Vec3> spawn_points(const Vec3& center, const std::vector<Base>& bases, const Breadcrumbs& crumbs,
                                      const Ring& ring, double base_radius, int count, uint32_t seed) {
    std::vector<Vec3> candidates;
    crumbs.for_each([&](const Vec3& p) {
        const double d = horizontal_distance(p, center);
        if (d < base_radius + 3 || d > ring.max + 40 || inside_any_base(p, bases, base_radius)) return;
        const double to = (std::min)(ring.max, (std::max)(ring.min, d));
        const Vec3 moved{center.x + (p.x - center.x) * to / d, p.y + 1.0, center.z + (p.z - center.z) * to / d};
        if (!inside_any_base(moved, bases, base_radius)) candidates.push_back(moved);
    });
    if (candidates.empty()) {
        const double r = (ring.min + ring.max) / 2;
        for (int i = 0; i < 16; ++i) {
            const double a = i * 3.14159265358979323846 / 8;
            const Vec3 p{center.x + r * std::cos(a), center.y + 2.0, center.z + r * std::sin(a)};
            if (!inside_any_base(p, bases, base_radius)) candidates.push_back(p);
        }
    }
    std::vector<Vec3> out;
    if (candidates.empty() || count <= 0) return out;
    uint32_t s = seed;
    const Vec3 anchor = candidates[next_random(s) % candidates.size()];
    for (int i = 0; i < count; ++i) {
        Vec3 p = anchor;
        if (i > 0) {  // a small circle around the first, 2 m out
            const double a = i * 2 * 3.14159265358979323846 / count;
            p.x += 2.0 * std::cos(a);
            p.z += 2.0 * std::sin(a);
            const double d = horizontal_distance(p, center);
            if (inside_any_base(p, bases, base_radius) || d < ring.min - 4 || d > ring.max + 4) p = anchor;
        }
        out.push_back(p);
    }
    return out;
}

// A raider that fell at its spawn (more than 10 m up or down in its first 10 s: into a hole, through the ground), or that
// has not come 3 m closer to the altar in 60 s while still more than 12 m from it, is stuck: the mod removes it, and it
// is no kill.
class StuckWatch {
public:
    static constexpr double kFall = 10.0, kProgress = 3.0, kArrived = 12.0;
    static constexpr uint64_t kSettleMs = 10000, kNoProgressMs = 60000;
    void on_spawned(uint32_t id, const Vec3& at, uint64_t now_ms) { map_[id] = {at, now_ms, -1.0, now_ms}; }
    void forget(uint32_t id) { map_.erase(id); }
    void clear() { map_.clear(); }
    bool stuck(uint32_t id, const Vec3& now_at, const Vec3& altar, uint64_t now_ms) {
        auto it = map_.find(id);
        if (it == map_.end()) return false;
        Track& t = it->second;
        if (now_ms - t.spawned_ms <= kSettleMs && std::fabs(now_at.y - t.spawn.y) > kFall) return true;
        const double d = horizontal_distance(now_at, altar);
        if (d <= kArrived) return false;
        if (t.best < 0) {  // the first look: the 60 s count from its spawn
            t.best = d;
            return false;
        }
        if (d < t.best - kProgress) {
            t.best = d;
            t.best_ms = now_ms;
            return false;
        }
        return now_ms - t.best_ms > kNoProgressMs;
    }
    // The clock starts again from now (a raider whose wall spot takes damage is not stuck).
    void hold(uint32_t id, uint64_t now_ms) {
        auto it = map_.find(id);
        if (it != map_.end()) it->second.best_ms = now_ms;
    }

private:
    struct Track {
        Vec3 spawn;
        uint64_t spawned_ms;
        double best;  // the nearest it came to the altar (-1: not seen yet)
        uint64_t best_ms;
    };
    std::map<uint32_t, Track> map_;
};

// ---- walls (the "Raiders break walls" setting; EMF API v44) ----
// A raider held up on its way to the altar breaks the wall (or the closed gate) in front of it. The mod finds the wall
// (emf::find_wall on a line from the raider's chest toward the altar) and the gate (the first door on a path that may
// open doors) and feeds them in; WallWork decides who works where and when a hit lands, and the mod carries out the
// actions (break_blocks for a hit, enemy orders, releasing a raider to march on).
struct WallSettings {
    bool on = false;
    double hole_radius = 1.0;   // "Hole size"
    bool drops = false;         // "Broken blocks drop items"
    double damage_scale = 1.0;  // from the difficulty, divided by "Wall strength"
};
// The "Hole size" setting: small (about a door), medium, large.
inline double hole_radius_of(const std::string& v) {
    if (v == "medium") return 1.75;
    if (v == "large") return 2.5;
    return 1.0;
}
// How hard raiders hit walls and gates, by the (effective) difficulty.
inline double wall_damage_scale(const std::string& difficulty) {
    if (difficulty == "easy") return 0.5;
    if (difficulty == "hard") return 1.5;
    if (difficulty == "deadly") return 2.0;
    return 1.0;
}

// A hole: spheres of `radius` from `centre` up to `height` above it (break_blocks' column).
struct Hole {
    Vec3 centre;
    double radius = 1.0, height = 0.0;
};
// The hole at a wall point for a raider standing at feet_y: low enough to reach the floor (0.75 r over the feet) and
// at least 2.2 m high (a small hole is a column of two spheres).
inline Hole hole_at(const Vec3& wall, double feet_y, double r) {
    Hole h;
    h.centre = {wall.x, feet_y + 0.75 * r, wall.z};
    h.radius = r;
    h.height = (std::max)(0.0, 2.2 - 1.5 * r);
    return h;
}
// Damage added to each raider hit on a gate: it falls in about 6 hits at normal (12 easy, 4 hard, 3 deadly); 40 x
// scale when its health is not known.
inline int32_t gate_damage(int32_t max_health, double scale) {
    const double d = max_health > 0 ? std::ceil(static_cast<double>(max_health) * scale / 6.0) : std::ceil(40.0 * scale);
    return static_cast<int32_t>((std::min)(100000.0, (std::max)(1.0, d)));
}

// The wall settings plus the scale of the (effective) difficulty (a preset's while one is chosen) divided by "Wall
// strength" (x hits, 0.5..10: 2 = every wall and gate takes twice the hits).
inline WallSettings wall_settings(bool on, const std::string& hole_size, bool drops, const std::string& preset_name,
                                  const std::string& difficulty, double strength = 1.0);

struct Gate {
    uint32_t door = 0;
    Vec3 pos;
    int32_t max_health = 0;
};

struct WallAction {
    enum Kind {
        Hit,        // break_blocks at `hole` with the default damage x damage_scale
        OrderWall,  // keep `raider` at the wall point `at` (renewed every 3 s; the mod walks it up from more than 3 m)
        OrderGate,  // send `raider` at `door` with gate_damage added to its hits, for 3 s
        Release,    // `raider` stops working: cancel its order, it marches on the altar again
        GiveUp,     // spot `spot` ended without getting through (or nobody works it)
        Done,       // spot `spot` is through (the wall is open, the gate is gone)
    } kind = Hit;
    uint32_t spot = 0, raider = 0, door = 0;
    Hole hole;
    Vec3 at;
    double damage_scale = 1.0;
    int32_t gate_damage = 0;
};

class WallWork {
public:
    static constexpr uint64_t kLookMs = 10000;      // no 3 m of progress this long: look for a wall
    static constexpr double kProgress = 3.0;
    static constexpr double kNearBase = 20.0;       // look only within the base radius + this
    static constexpr double kReach = 4.0;           // the wall search's length from the chest
    static constexpr double kChest = 1.2;
    static constexpr double kWorkRange = 3.0;       // a worker this near its spot counts for its hits
    static constexpr uint64_t kHitEveryMs = 1500;
    static constexpr uint64_t kOrderEveryMs = 3000; // the orders last 3 s and are renewed
    static constexpr uint64_t kGiveUpMs = 120000;
    static constexpr size_t kMaxSpots = 4;
    static constexpr double kGateRange = 6.0;       // a gate is looked for this near the raider
    static constexpr uint64_t kHoldMs = 3000;       // the 60 s clock waits while the spot took damage this recently
    static constexpr double kMaxAdvance = 3.0;      // a hole goes at most this far into a wall from where it began

    struct SpotInfo {
        uint32_t id = 0;
        bool gate = false;
        uint32_t door = 0;
        Vec3 point;  // the wall point, or the gate's position
        Hole hole;
        int32_t gate_damage = 0;
        std::vector<uint32_t> workers;
    };

    // Once per look: true when the raider made no 3 m of progress toward the altar for 10 s, is within the base radius
    // + 20 m of it, works no spot and did not look in the last 10 s. After a spot it worked was given up, never until it
    // got 3 m further (so the 60 s removal decides, as without walls).
    bool wants_look(uint32_t id, const Vec3& pos, const Vec3& altar, double base_radius, uint64_t now) {
        if (working(id)) return false;
        const double d = horizontal_distance(pos, altar);
        auto it = track_.find(id);
        if (d > base_radius + kNearBase) {
            track_[id] = {d, now, it == track_.end() ? 0 : it->second.last_look};
            return false;
        }
        if (it == track_.end()) {
            track_[id] = {d, now, 0};
            return false;
        }
        Track& t = it->second;
        if (t.gave_up) {  // its spot was given up: today's 60 s rule decides, unless it gets 3 m further
            if (t.gave_up_at < 0) t.gave_up_at = d;
            if (d >= t.gave_up_at - kProgress) return false;
            t.gave_up = false;
            t.best = d;
            t.best_ms = now;
            return false;
        }
        if (d < t.best - kProgress) {
            t.best = d;
            t.best_ms = now;
            return false;
        }
        if (now - t.best_ms < kLookMs) return false;
        if (t.last_look && now - t.last_look < kLookMs) return false;
        t.last_look = now;
        return true;
    }

    // What the look found (nothing: nullopt): the raider works the nearer of the two. A wall within the hole radius +
    // 0.5 m of a spot's point joins that spot, a gate the spot of that door; a new spot only while fewer than 4.
    std::optional<uint32_t> assign(uint32_t raider, const Vec3& pos, std::optional<Vec3> wall, std::optional<Gate> gate,
                                   const WallSettings& s, uint64_t now) {
        if (auto mine = spot_of(raider)) return mine;
        if (wall && gate) {
            if (horizontal_distance(pos, gate->pos) < horizontal_distance(pos, *wall))
                wall.reset();
            else
                gate.reset();
        }
        Spot* spot = nullptr;
        if (gate) {
            for (auto& sp : spots_)
                if (sp.gate && sp.door == gate->door) spot = &sp;
            if (!spot) {
                if (spots_.size() >= kMaxSpots) return std::nullopt;
                Spot n;
                n.id = next_id_++;
                n.gate = true;
                n.door = gate->door;
                n.point = gate->pos;
                n.gate_damage = gate_damage(gate->max_health, s.damage_scale);
                n.started = now;
                spots_.push_back(n);
                spot = &spots_.back();
            }
        } else if (wall) {
            for (auto& sp : spots_)
                if (!sp.gate && horizontal_distance(sp.point, *wall) <= s.hole_radius + 0.5) spot = &sp;
            if (!spot) {
                if (spots_.size() >= kMaxSpots) return std::nullopt;
                Spot n;
                n.id = next_id_++;
                n.point = n.origin = *wall;
                n.hole = hole_at(*wall, pos.y, s.hole_radius);
                n.started = now;
                spots_.push_back(n);
                spot = &spots_.back();
            }
        } else {
            return std::nullopt;
        }
        spot->workers[raider] = std::nullopt;  // its order goes out at the next tick
        return spot->id;
    }

    // About once a second while the raid fights. raiders: the living raiders and where they are.
    std::vector<WallAction> tick(const std::map<uint32_t, Vec3>& raiders, const WallSettings& s, uint64_t now) {
        std::vector<WallAction> out;
        for (size_t i = 0; i < spots_.size();) {
            Spot& sp = spots_[i];
            for (auto w = sp.workers.begin(); w != sp.workers.end();)
                w = raiders.count(w->first) ? std::next(w) : sp.workers.erase(w);
            const bool expired = now - sp.started >= kGiveUpMs;
            if (!s.on || expired || sp.workers.empty()) {
                for (const auto& w : sp.workers) {
                    out.push_back(action(WallAction::Release, sp, w.first));
                    if (expired) {
                        Track& t = track_[w.first];
                        t.gave_up = true;
                        t.gave_up_at = -1;
                    }
                }
                out.push_back(action(WallAction::GiveUp, sp, 0));
                spots_.erase(spots_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            int in_range = 0;
            for (auto& [id, ordered] : sp.workers) {
                const Vec3& p = raiders.at(id);
                if (horizontal_distance(p, sp.point) <= kWorkRange && std::fabs(p.y - sp.point.y) <= kWorkRange) ++in_range;
                if (!ordered || now - *ordered >= kOrderEveryMs) {
                    ordered = now;
                    WallAction a = action(sp.gate ? WallAction::OrderGate : WallAction::OrderWall, sp, id);
                    out.push_back(a);
                }
            }
            if (!sp.gate && in_range > 0 && (!sp.last_hit || now - *sp.last_hit >= kHitEveryMs)) {
                sp.last_hit = now;
                WallAction a = action(WallAction::Hit, sp, 0);
                a.damage_scale = s.damage_scale * in_range;
                out.push_back(a);
            }
            ++i;
        }
        return out;
    }

    // The wall is thicker than the hole: the next wall point on the worker's line becomes the spot's point (the hole
    // goes deeper), while it stays within kMaxAdvance across of where the spot began. False: no such spot, or too far.
    bool advance(uint32_t spot, const Vec3& point, double feet_y, const WallSettings& s) {
        Spot* sp = find(spot);
        if (!sp || sp->gate || horizontal_distance(point, sp->origin) > kMaxAdvance) return false;
        sp->point = point;
        sp->hole = hole_at(point, feet_y, s.hole_radius);
        return true;
    }

    // A hit's answer: the spot took damage when anything was damaged or removed.
    void on_hit(uint32_t spot, uint32_t damaged, uint32_t removed, uint64_t now) {
        if (Spot* sp = find(spot); sp && (damaged || removed)) sp->last_damage = now;
    }
    // A gate's health now: a drop since the last reading is damage.
    void on_gate_health(uint32_t spot, int32_t health, uint64_t now) {
        Spot* sp = find(spot);
        if (!sp) return;
        if (sp->last_health >= 0 && health < sp->last_health) sp->last_damage = now;
        sp->last_health = health;
    }
    // Through: nothing blocks the line any more (or the gate is gone). Its workers go back to marching.
    std::vector<WallAction> wall_clear(uint32_t spot) {
        std::vector<WallAction> out;
        for (size_t i = 0; i < spots_.size(); ++i) {
            if (spots_[i].id != spot) continue;
            for (const auto& w : spots_[i].workers) out.push_back(action(WallAction::Release, spots_[i], w.first));
            out.push_back(action(WallAction::Done, spots_[i], 0));
            spots_.erase(spots_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
        return out;
    }
    // It works a spot that took damage within the last 3 s (the stuck clock waits meanwhile).
    bool holding(uint32_t raider, uint64_t now) const {
        for (const auto& sp : spots_)
            if (sp.workers.count(raider)) return sp.last_damage && now - *sp.last_damage < kHoldMs;
        return false;
    }
    // It stops working (a fight, the mod's choice); a spot left without workers ends at the next tick.
    void release(uint32_t raider) {
        for (auto& sp : spots_) sp.workers.erase(raider);
    }
    // Gone, dead or removed: it is forgotten altogether.
    void forget(uint32_t raider) {
        release(raider);
        track_.erase(raider);
    }
    void clear() {
        spots_.clear();
        track_.clear();
    }
    bool working(uint32_t raider) const { return spot_of(raider).has_value(); }
    std::optional<uint32_t> spot_of(uint32_t raider) const {
        for (const auto& sp : spots_)
            if (sp.workers.count(raider)) return sp.id;
        return std::nullopt;
    }
    std::vector<SpotInfo> spots() const {
        std::vector<SpotInfo> out;
        for (const auto& sp : spots_) {
            SpotInfo i{sp.id, sp.gate, sp.door, sp.point, sp.hole, sp.gate_damage, {}};
            for (const auto& w : sp.workers) i.workers.push_back(w.first);
            out.push_back(i);
        }
        return out;
    }

private:
    struct Track {
        double best = 0;
        uint64_t best_ms = 0, last_look = 0;
        bool gave_up = false;     // its spot was given up after 2 minutes
        double gave_up_at = -1;   // its distance to the altar then (-1: not seen yet)
    };
    struct Spot {
        uint32_t id = 0;
        bool gate = false;
        uint32_t door = 0;
        Vec3 point;
        Vec3 origin;  // where the spot began (advance stays near it)
        Hole hole;
        int32_t gate_damage = 0;
        uint64_t started = 0;
        std::optional<uint64_t> last_hit, last_damage;
        int32_t last_health = -1;  // the gate's last reading (-1: none yet)
        std::map<uint32_t, std::optional<uint64_t>> workers;  // raider -> when its order last went out
    };
    static WallAction action(WallAction::Kind k, const Spot& sp, uint32_t raider) {
        WallAction a;
        a.kind = k;
        a.spot = sp.id;
        a.raider = raider;
        a.door = sp.door;
        a.hole = sp.hole;
        a.at = sp.point;
        a.gate_damage = sp.gate_damage;
        return a;
    }
    Spot* find(uint32_t id) {
        for (auto& sp : spots_)
            if (sp.id == id) return &sp;
        return nullptr;
    }
    std::vector<Spot> spots_;
    std::map<uint32_t, Track> track_;
    uint32_t next_id_ = 1;
};

// ---- whom they fight ----
// The "Raiders attack" setting: players only, or players and their allies (the companion, summons, tamed beasts).
enum class Targets { Players, Allies };
inline Targets targets_mode(const std::string& v) { return v == "players" ? Targets::Players : Targets::Allies; }
struct Foe {
    uint32_t id = 0;
    Vec3 pos;
    bool player = false;  // a player; else an ally of the players
    bool alive = true;    // health left
};
// The one a raider at `at` is told to fight: `keep` (its last target) while that one is alive, wanted and within
// `range` metres, so it does not flip between the player and the companion; else the nearest living foe within range
// (horizontally). Allies count only with Targets::Allies. 0 when there is none: the raider keeps marching on the altar.
inline uint32_t pick_target(const Vec3& at, const std::vector<Foe>& foes, double range, Targets mode, uint32_t keep = 0) {
    const auto wanted = [&](const Foe& f) { return f.alive && (f.player || mode == Targets::Allies); };
    if (keep)
        for (const auto& f : foes)
            if (f.id == keep && wanted(f) && horizontal_distance(f.pos, at) <= range) return keep;
    uint32_t best = 0;
    double best_d = range;
    for (const auto& f : foes) {
        if (!wanted(f)) continue;
        const double d = horizontal_distance(f.pos, at);
        if (d <= best_d) {
            best_d = d;
            best = f.id;
        }
    }
    return best;
}

// ---- who comes ----
// min_level / max_level: the band of levels it comes for. With the progression rule ("Only creatures you are ready
// for") min_level is also the player level it waits for: the level at which players meet it in the game.
struct Template {
    std::string name;
    std::string guid;
    uint32_t min_level = 1, max_level = 99;
    bool known = true;  // its level is known (EMF's own, or "GUID@level"); a bare custom GUID's is not
};
// Templates seen to wake, walk to the altar and fight when spawned away from their camp. Only these by default: some
// templates spawn asleep (the heavy wolf mini-boss did in game). Add others with the custom_templates setting.
inline const std::vector<Template>& verified_pool() {
    static const std::vector<Template> k = {
        // Enemy_Vukah_Melee: fought and killed the player (docs/ENGINE_MAP.md); seen in game to land and march in
        {"Vukah warrior", "d5d05217-52dc-467a-9192-26b4f661d8d9", 1, 99},
    };
    return k;
}
// The boss wave's leader: Enemy_Midboss_Vukah_Bigfoot_BossHealthBar woke, came for the player and showed its boss bar
// (seen in game). A Springlands mini-boss: with the progression rule it waits for a player of level 5 at the base (the
// level players meet it at; not checked in game).
inline const Template& boss_template() {
    static const Template k{"Vukah brawler", "9c8e2008-7259-4114-b6fc-615f0f181c04", 5, 99};
    return k;
}
inline bool parse_guid_text(const std::string& text, uint8_t out[16]) { return emf::parse_guid(text, out); }

// A pool entry for `level`: one whose band holds it (by `rnd`), else the one with the lowest band.
inline const Template& pick_template(const std::vector<Template>& pool, uint32_t level, uint32_t rnd) {
    std::vector<const Template*> fit;
    for (const auto& t : pool)
        if (t.min_level <= level && level <= t.max_level) fit.push_back(&t);
    if (!fit.empty()) return *fit[rnd % fit.size()];
    const Template* low = &pool[0];
    for (const auto& t : pool)
        if (t.min_level < low->min_level) low = &t;
    return *low;
}
// The custom_templates setting: GUIDs separated by commas, each optionally with "@level" (the player level it comes
// from, 1-99: what the progression rule checks); anything else goes to *bad.
inline std::vector<Template> parse_custom_templates(const std::string& text, std::vector<std::string>* bad) {
    std::vector<Template> out;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find(',', at);
        if (end == std::string::npos) end = text.size();
        std::string t = text.substr(at, end - at);
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(0, 1);
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        uint8_t g[16];
        if (!t.empty()) {
            const auto trim = [](std::string v) {
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
                while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
                return v;
            };
            const size_t at_sign = t.find('@');
            const std::string guid = trim(t.substr(0, at_sign));
            uint32_t level = 1;
            bool ok = parse_guid_text(guid, g), known = false;
            if (ok && at_sign != std::string::npos) {
                const std::string lv = trim(t.substr(at_sign + 1));
                ok = !lv.empty() && lv.size() <= 2 && lv.find_first_not_of("0123456789") == std::string::npos;
                if (ok) level = static_cast<uint32_t>(std::stoul(lv));
                ok = ok && level >= 1 && level <= 99;
                known = true;
            }
            if (ok) out.push_back({"custom", guid, level, 99, known});
            else if (bad) bad->push_back(t);
        }
        at = end + 1;
    }
    return out;
}
// ---- the progression rule ("Only creatures you are ready for", on by default) ----
// The game keeps which zones a player has seen (the map, on their own PC) and which bosses they beat (knowledge, in a
// part of the save EMF cannot read), so the rule goes by levels: a template comes only once the strongest player at the
// base has reached its own level (min_level), and a boss or mini-boss likewise.
//
// A raider for this wave: with the rule, only templates whose level the player has reached (the tested Vukah warrior
// when none has), by the band holding the raiders' level; without it, as pick_template. `found_bosses`: bare custom
// GUIDs already seen to be a boss or mini-boss (remove_unready_boss), skipped with the rule.
inline Template choose_raider(const std::vector<Template>& pool, uint32_t raider_level, uint32_t player_level,
                              uint32_t rnd, bool progression, const std::set<std::string>* found_bosses = nullptr) {
    if (!progression) return pick_template(pool, raider_level, rnd);
    std::vector<Template> ready;
    for (const auto& t : pool)
        if (t.min_level <= (std::max)(player_level, 1u) && (t.known || !found_bosses || !found_bosses->count(t.guid)))
            ready.push_back(t);
    if (ready.empty()) ready.push_back(verified_pool()[0]);
    return pick_template(ready, raider_level, rnd);
}
// With the rule, a creature that turned out to be a boss or mini-boss from a template whose level is not known (a bare
// custom GUID) is removed: EMF cannot tell whether the players have beaten it.
inline bool remove_unready_boss(bool progression, const Template& t, bool is_boss) {
    return progression && is_boss && !t.known;
}

// The raiders' level: the strongest player's at the base plus the difficulty, 1-99 (0 = unknown counts as 1).
inline uint32_t raider_level(uint32_t strongest, int offset) {
    const int v = static_cast<int>((std::max)(strongest, 1u)) + offset;
    return static_cast<uint32_t>((std::min)(99, (std::max)(1, v)));
}
inline int difficulty_offset(const std::string& v) {
    if (v == "easy") return -5;
    if (v == "hard") return 3;
    if (v == "deadly") return 6;
    return 0;
}

// ---- what they leave ----
enum class RewardKind { None, Materials, Rare };
inline RewardKind reward_kind(const std::string& v) {
    if (v == "none") return RewardKind::None;
    if (v == "rare") return RewardKind::Rare;
    return RewardKind::Materials;
}
struct RewardItem {
    std::string item;  // the game's debug name
    uint32_t count = 1;
    uint32_t level = 0;      // 0 = the game's default
    uint8_t rarity = 0xFF;   // 0xFF = the game's default; 2 = Rare
};
// For each player at the base after a repelled raid: weapon upgrade runes and furs, one each per raider killed (at
// least 2); "rare" adds one weapon at the player's level, Rare.
inline std::vector<RewardItem> reward_items(RewardKind kind, int kills, uint32_t level, uint32_t rnd) {
    std::vector<RewardItem> out;
    if (kind == RewardKind::None) return out;
    const auto n = static_cast<uint32_t>((std::max)(2, kills));
    out.push_back({"Material_T1_Token_WeaponUpgrade_Rune01", n});
    out.push_back({"Material_T1_Animal_Fur", n});
    if (kind == RewardKind::Rare) {
        static const char* const kWeapons[] = {"Weapon_T1_1H_SwordEpic_01", "Weapon_T1_2H_BowEpic_01",
                                               "Weapon_T1_1H_WandEpic_01_Fire", "Weapon_T1_2H_GreatAxeEpic_01"};
        out.push_back({kWeapons[rnd % 4], 1, (std::max)(level, 1u), 2});
    }
    return out;
}

// ---- the world's switch (per world, in emf\config\raids\<slot>.txt) ----
inline bool parse_world_on(const std::string& text) {
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "on=1") return true;
        at = end + 1;
    }
    return false;
}
inline std::string world_text(bool on) { return on ? "on=1\n" : "on=0\n"; }

// ---- the raid ----
struct Settings {
    int chance = 35;           // % per night
    double start_hour = 20.0;  // nightfall for raids
    double dawn_hour = 6.0;    // the game's day start
    int waves = 2;
    int per_wave = 4;
    int wave_gap_s = 60;
    int warning_s = 60;
    int level_offset = 0;
    int cooldown_nights = 0;
    double base_radius = 30;
    int max_alive = 24;
    bool boss = false;
    bool progression = true;  // only creatures the players are ready for (boss_template()'s level and the like)
    BasePick bases = BasePick::Busiest;
};

// ---- presets (the "Raid strength" setting) ----
// How often and how hard raids come, in one choice. "custom" (the default, so an update never overrides what a player
// chose) uses the settings one by one; a preset replaces exactly the settings in preset_keys(), which mod.json greys out
// meanwhile. Standard is the mod's defaults.
struct Preset {
    int chance, warning_s, cooldown_nights, waves, per_wave, wave_gap_s;
    std::string difficulty;  // the difficulty setting's value
    bool boss;
    std::string reward;  // the reward setting's value
};
inline const std::vector<std::string>& preset_keys() {
    static const std::vector<std::string> k = {"chance",   "warning_seconds",  "cooldown_nights", "waves", "per_wave",
                                               "wave_gap_seconds", "difficulty", "boss",            "reward"};
    return k;
}
// The preset by its setting value; nothing for "custom" or an unknown value.
inline std::optional<Preset> preset(const std::string& name) {
    // gentle: for the early game: now and then one small wave, 5 levels below the players, a long warning, a quiet night
    // after each raid
    if (name == "gentle") return Preset{25, 120, 1, 1, 3, 90, "easy", false, "materials"};
    if (name == "standard") return Preset{35, 60, 0, 2, 4, 60, "normal", false, "materials"};
    // hard: often, three waves of 6 three levels above the players, the boss wave, a rare weapon when repelled
    if (name == "hard") return Preset{50, 45, 0, 3, 6, 45, "hard", true, "rare"};
    return std::nullopt;
}
// The preset's values put into the raid's settings and the reward; false and nothing changed for "custom".
inline bool apply_preset(const std::string& name, Settings& s, RewardKind& reward) {
    const auto p = preset(name);
    if (!p) return false;
    s.chance = p->chance;
    s.warning_s = p->warning_s;
    s.cooldown_nights = p->cooldown_nights;
    s.waves = p->waves;
    s.per_wave = p->per_wave;
    s.wave_gap_s = p->wave_gap_s;
    s.level_offset = difficulty_offset(p->difficulty);
    s.boss = p->boss;
    reward = reward_kind(p->reward);
    return true;
}

enum class Phase { Idle, Warning, Fighting };

struct Action {
    enum Kind { Notice, Log, Hud, Spawn, Reward, Destroy } kind = Log;
    std::string text;           // Notice, Log, Hud ("" removes the line)
    int count = 0;              // Spawn: raiders (the boss comes on top); Reward: raiders killed
    uint32_t level = 1;         // Spawn: the raiders' level
    uint32_t player_level = 1;  // Spawn: the strongest player's at the base (what the progression rule checks)
    bool boss = false;          // Spawn: one Vukah brawler on top
    uint32_t altar = 0;         // Spawn, Reward
    Vec3 at;                    // Spawn, Reward: the altar's position
    std::vector<uint32_t> ids;  // Destroy
};

struct Tick {
    uint64_t now_ms = 0;
    std::optional<double> tod;       // hours; unknown: no nightfall, no dawn
    bool world_on = false;           // the per-world switch
    uint32_t session = 0;            // emf::server_session()
    std::vector<Occupied> occupied;  // bases with players inside
    int defenders = 0;                   // players near the raid's base (within the ring + 20 m)
    uint32_t roll = 0;               // a random number; the chance is roll % 100 < chance
};

class Director {
public:
    static constexpr uint64_t kBreathMs = 5000;        // after a wave died, the next one comes this much later
    static constexpr uint64_t kAbandonMs = 60000;      // nobody near the base this long: the raiders leave
    static constexpr uint64_t kMaxRaidMs = 20 * 60000; // a raid never lasts longer
    static constexpr uint64_t kHudLingerMs = 10000;    // "Raid repelled!" stays this long

    explicit Director(Settings s = {}) : s_(s) {}
    void set_settings(const Settings& s) { s_ = s; }
    const Settings& settings() const { return s_; }

    // A new hosted session (a reload, another world): the raiders went with the old world (never saved) and their ids
    // mean something else now, so the raid is forgotten, nothing destroyed. The mod calls it before anything uses an
    // id (its tick, commands, events); tick() does it too.
    std::vector<Action> adopt_session(uint32_t session) {
        std::vector<Action> out;
        if (session == session_) return out;
        if (session_ != 0) {
            reset();
            out.push_back(hud(""));
        }
        session_ = session;
        prev_tod_.reset();
        return out;
    }

    std::vector<Action> tick(const Tick& t) {
        std::vector<Action> out = adopt_session(t.session);
        if (!t.world_on) {
            if (phase_ != Phase::Idle) append(out, stop("Night Raids is off in this world: the raiders are gone"));
        } else if (t.tod && prev_tod_ && crossed(*prev_tod_, *t.tod, s_.start_hour) &&
                   is_night(*t.tod, s_.start_hour, s_.dawn_hour)) {  // slept through it: no raid at dawn
            append(out, nightfall(t));
        }
        if (t.tod) prev_tod_ = t.tod;
        switch (phase_) {
            case Phase::Idle:
                if (hud_clear_ms_ && t.now_ms >= hud_clear_ms_) {
                    hud_clear_ms_ = 0;
                    out.push_back(hud(""));
                }
                break;
            case Phase::Warning: warning(t, out); break;
            case Phase::Fighting: fighting(t, out); break;
        }
        return out;
    }

    // raids.start: a raid now at the busiest base, after `warning_s` seconds (by day too). Empty + *why when refused.
    std::vector<Action> start(const Tick& t, int warning_s, std::string* why) {
        if (phase_ != Phase::Idle) {
            if (why) *why = "a raid is already on";
            return {};
        }
        const Occupied* o = pick_base(t.occupied, BasePick::Busiest, t.roll);
        if (!o) {
            if (why) *why = "nobody is at a base (inside a flame altar's radius)";
            return {};
        }
        if (session_ == 0) session_ = t.session;
        return begin(t, *o, warning_s, t.tod && is_night(*t.tod, s_.start_hour, s_.dawn_hour));
    }

    // Ends a raid now (raids.stop, the world or the mod switched off): every living raider is destroyed.
    std::vector<Action> stop(const std::string& notice) {
        if (phase_ == Phase::Idle) return {};
        std::vector<Action> out;
        leave(out, notice);
        return out;
    }

    // The mod spawned one raider of the wave just asked for.
    void on_spawned(uint32_t id) { raiders_.push_back({id, true, false}); }
    // How many raiders the wave just asked for really appeared (0: no place found; the raid ends).
    void on_wave_spawned(int n) {
        spawn_pending_ = false;
        spawned_total_ += n;
        if (n <= 0) failed_wave_ = true;
    }
    // A player (or an ally: the companion) hit this raider: its death is a kill (raids.kill credits its own).
    void credit(uint32_t id) {
        for (auto& r : raiders_)
            if (r.id == id) r.credited = true;
    }
    // A raider died: a kill only when credited (a fall or the like pays nothing).
    void on_died(uint32_t id) {
        for (auto& r : raiders_)
            if (r.id == id && r.alive) {
                r.alive = false;
                if (r.credited) ++kills_;
            }
    }
    // A raider no longer exists without a death (the game removed it): gone, no kill.
    void on_gone(uint32_t id) {
        for (auto& r : raiders_)
            if (r.id == id) r.alive = false;
    }

    Phase phase() const { return phase_; }
    int alive() const {
        int n = 0;
        for (const auto& r : raiders_) n += r.alive ? 1 : 0;
        return n;
    }
    std::vector<uint32_t> living() const {
        std::vector<uint32_t> out;
        for (const auto& r : raiders_)
            if (r.alive) out.push_back(r.id);
        return out;
    }
    bool is_raider(uint32_t id) const {
        for (const auto& r : raiders_)
            if (r.id == id) return true;
        return false;
    }
    int wave() const { return waves_spawned_; }
    int kills() const { return kills_; }
    const Base& base() const { return base_; }
    int cooldown_left() const { return cooldown_left_; }
    // For raids.status.
    std::string status() const {
        char b[160];
        switch (phase_) {
            case Phase::Warning: return "raiders are gathering outside the base";
            case Phase::Fighting:
                snprintf(b, sizeof b, "a raid is on: wave %d/%d, %d raider(s) left, %d killed", waves_spawned_, s_.waves,
                         alive(), kills_);
                return b;
            default:
                if (cooldown_left_ > 0) {
                    snprintf(b, sizeof b, "no raid now (resting %d more night(s))", cooldown_left_);
                    return b;
                }
                return "no raid now";
        }
    }

private:
    struct Raider {
        uint32_t id;
        bool alive;
        bool credited;  // hit by a player or an ally (or killed by raids.kill)
    };
    static Action hud(const std::string& text) {
        Action a;
        a.kind = Action::Hud;
        a.text = text;
        return a;
    }
    static Action note(Action::Kind k, const std::string& text) {
        Action a;
        a.kind = k;
        a.text = text;
        return a;
    }
    static void append(std::vector<Action>& out, std::vector<Action> more) {
        for (auto& a : more) out.push_back(std::move(a));
    }

    void reset() {
        phase_ = Phase::Idle;
        raiders_.clear();
        spawn_pending_ = failed_wave_ = false;
        waves_spawned_ = spawned_total_ = kills_ = 0;
        cooldown_left_ = 0;
        hud_clear_ms_ = 0;
    }

    std::vector<Action> nightfall(const Tick& t) {
        std::vector<Action> out;
        if (phase_ != Phase::Idle) return out;
        if (cooldown_left_ > 0) {
            --cooldown_left_;
            out.push_back(note(Action::Log, "nightfall: the raiders rest tonight (cooldown)"));
            return out;
        }
        const uint32_t roll = t.roll % 100;
        if (static_cast<int>(roll) >= s_.chance) {
            out.push_back(note(Action::Log, "nightfall: no raid tonight (rolled " + std::to_string(roll) + ", needs below " +
                                                std::to_string(s_.chance) + ")"));
            return out;
        }
        const Occupied* o = pick_base(t.occupied, s_.bases, t.roll / 100);
        if (!o) {
            out.push_back(note(Action::Log, "nightfall: a raid was due, but nobody is at a base"));
            return out;
        }
        return begin(t, *o, s_.warning_s, true);
    }

    std::vector<Action> begin(const Tick& t, const Occupied& o, int warning_s, bool night) {
        reset();
        phase_ = Phase::Warning;
        base_ = o.base;
        level_ = strongest_level(o);
        night_raid_ = night;
        started_ms_ = t.now_ms;
        warn_until_ms_ = t.now_ms + static_cast<uint64_t>((std::max)(0, warning_s)) * 1000;
        std::vector<Action> out;
        out.push_back(note(Action::Notice, "The Shroud stirs: raiders gather outside your base and attack in " +
                                               std::to_string((std::max)(0, warning_s)) + " s!"));
        return out;
    }

    void warning(const Tick& t, std::vector<Action>& out) {
        if (t.now_ms < warn_until_ms_) {
            const uint64_t left = (warn_until_ms_ - t.now_ms + 999) / 1000;
            out.push_back(hud("Raid in " + std::to_string(left) + " s"));
            return;
        }
        phase_ = Phase::Fighting;
        last_presence_ms_ = t.now_ms;
        dead_since_ms_ = 0;
        fighting(t, out);
    }

    void fighting(const Tick& t, std::vector<Action>& out) {
        if (night_raid_ && t.tod && !is_night(*t.tod, s_.start_hour, s_.dawn_hour)) {
            leave(out, "The raiders flee at dawn");
            return;
        }
        if (t.defenders > 0) last_presence_ms_ = t.now_ms;
        if (t.now_ms - last_presence_ms_ >= kAbandonMs) {
            leave(out, "The raiders leave: no one defends the base");
            return;
        }
        if (t.now_ms - started_ms_ >= kMaxRaidMs) {
            leave(out, "The raiders give up");
            return;
        }
        if (failed_wave_) {
            out.push_back(note(Action::Log, "a wave found no place to gather (every spot outside the base is blocked or "
                                            "the spawns were refused): the raid ends"));
            leave(out, "The raiders turn back");
            return;
        }
        const int living = alive();
        if (living > 0) dead_since_ms_ = 0;
        else if (!dead_since_ms_ && !spawn_pending_ && waves_spawned_ > 0) dead_since_ms_ = t.now_ms;
        if (!spawn_pending_ && waves_spawned_ < s_.waves) {
            const bool last = waves_spawned_ + 1 == s_.waves;
            const int size = s_.per_wave + (s_.boss && last ? 1 : 0);
            const bool due = waves_spawned_ == 0 || t.now_ms >= last_wave_ms_ + static_cast<uint64_t>(s_.wave_gap_s) * 1000 ||
                             (dead_since_ms_ && t.now_ms >= dead_since_ms_ + kBreathMs);
            if (due && living + size <= s_.max_alive) {
                uint32_t level = level_;
                for (const auto& o : t.occupied)
                    if (o.base.altar == base_.altar) level = (std::max)(level, strongest_level(o));
                Action a;
                a.kind = Action::Spawn;
                a.count = s_.per_wave;
                a.level = raider_level(level, s_.level_offset);
                a.player_level = (std::max)(level, 1u);
                a.boss = s_.boss && last;
                if (a.boss && s_.progression && a.player_level < boss_template().min_level) {
                    a.boss = false;
                    out.push_back(note(Action::Log, "the boss stays away: the strongest player at the base is level " +
                                                        std::to_string(a.player_level) + ", the " + boss_template().name +
                                                        " comes from level " + std::to_string(boss_template().min_level) +
                                                        " (Only creatures you are ready for)"));
                }
                a.altar = base_.altar;
                a.at = base_.pos;
                out.push_back(a);
                ++waves_spawned_;
                last_wave_ms_ = t.now_ms;
                spawn_pending_ = true;
                dead_since_ms_ = 0;
            }
        }
        if (!spawn_pending_ && waves_spawned_ >= s_.waves && living == 0 && spawned_total_ > 0) {
            if (kills_ > 0) {
                Action r;
                r.kind = Action::Reward;
                r.count = kills_;
                r.altar = base_.altar;
                r.at = base_.pos;
                out.push_back(r);
                out.push_back(note(Action::Notice, "Raid repelled! " + std::to_string(kills_) + " raider(s) slain"));
                out.push_back(hud("Raid repelled!"));
                end(t.now_ms + kHudLingerMs);
            } else {
                leave(out, "The raiders are gone");
            }
            return;
        }
        char b[96];
        snprintf(b, sizeof b, "Raid: wave %d/%d - %d left", waves_spawned_, s_.waves, living);
        out.push_back(hud(b));
    }

    // The survivors go (destroyed), the HUD line goes, the raid ends.
    void leave(std::vector<Action>& out, const std::string& notice) {
        const auto ids = living();
        if (!ids.empty()) {
            Action d;
            d.kind = Action::Destroy;
            d.ids = ids;
            out.push_back(d);
        }
        out.push_back(note(Action::Notice, notice));
        out.push_back(hud(""));
        end(0);
    }

    void end(uint64_t hud_clear_ms) {
        phase_ = Phase::Idle;
        raiders_.clear();
        spawn_pending_ = failed_wave_ = false;
        cooldown_left_ = s_.cooldown_nights;
        hud_clear_ms_ = hud_clear_ms;
    }

    Settings s_;
    Phase phase_ = Phase::Idle;
    uint32_t session_ = 0;
    std::optional<double> prev_tod_;
    Base base_;
    uint32_t level_ = 1;
    bool night_raid_ = false;
    uint64_t started_ms_ = 0, warn_until_ms_ = 0, last_wave_ms_ = 0, last_presence_ms_ = 0, dead_since_ms_ = 0;
    uint64_t hud_clear_ms_ = 0;
    std::vector<Raider> raiders_;
    bool spawn_pending_ = false, failed_wave_ = false;
    int waves_spawned_ = 0, spawned_total_ = 0, kills_ = 0, cooldown_left_ = 0;
};

inline WallSettings wall_settings(bool on, const std::string& hole_size, bool drops, const std::string& preset_name,
                                  const std::string& difficulty, double strength) {
    WallSettings s;
    s.on = on;
    s.hole_radius = hole_radius_of(hole_size);
    s.drops = drops;
    const auto p = preset(preset_name);
    if (!(strength == strength)) strength = 1.0;  // NaN
    strength = (std::min)(10.0, (std::max)(0.5, strength));
    s.damage_scale = wall_damage_scale(p ? p->difficulty : difficulty) / strength;
    return s;
}

}  // namespace raids
