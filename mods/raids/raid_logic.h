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

private:
    struct Track {
        Vec3 spawn;
        uint64_t spawned_ms;
        double best;  // the nearest it came to the altar (-1: not seen yet)
        uint64_t best_ms;
    };
    std::map<uint32_t, Track> map_;
};

// ---- who comes ----
struct Template {
    std::string name;
    std::string guid;
    uint32_t min_level = 1, max_level = 99;
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
// (seen in game).
inline const Template& boss_template() {
    static const Template k{"Vukah brawler", "9c8e2008-7259-4114-b6fc-615f0f181c04", 1, 99};
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
// The custom_templates setting: GUIDs separated by commas; anything else goes to *bad.
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
            if (parse_guid_text(t, g)) out.push_back({"custom", t, 1, 99});
            else if (bad) bad->push_back(t);
        }
        at = end + 1;
    }
    return out;
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
    BasePick bases = BasePick::Busiest;
};

enum class Phase { Idle, Warning, Fighting };

struct Action {
    enum Kind { Notice, Log, Hud, Spawn, Reward, Destroy } kind = Log;
    std::string text;           // Notice, Log, Hud ("" removes the line)
    int count = 0;              // Spawn: raiders (the boss comes on top); Reward: raiders killed
    uint32_t level = 1;         // Spawn
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
                a.boss = s_.boss && last;
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

}  // namespace raids
