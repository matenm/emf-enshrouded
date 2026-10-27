#pragma once
// Loot Magnet: which corpse is looted into which player, without the game (unit-tested in tests/test_loot_magnet.cpp).
// The mod (loot_magnet.cpp) records deaths (EntityDiedEvent), describes the dead bodies (a Body from their components)
// and sends the game's own bypass loot event {guest = player, host = corpse, lootAll} for each Loot this returns: the
// event confirmed in game (the corpse's stacks moved into player 1, a PickupEvent each).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace magnet {

struct Vec3 {
    double x = 0, y = 0, z = 0;
};
inline double horizontal_distance(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.z - b.z); }
inline double distance(const Vec3& a, const Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// What the mod read from a dead entity's components.
struct Body {
    uint32_t id = 0;
    bool exists = false;         // still in the world (has a position)
    Vec3 pos;
    bool player = false;         // PlayerState
    bool has_health = false;     // Health: a creature, not a chest or a prop
    bool has_inventory = false;  // Inventory: the loot event's "real inventory" path (the game adds it at death)
    bool has_offer = false;      // InteractionOfferRuntime: read for the log only (a corpse need not have it: G1)
    bool dead = false;           // NetworkHealth.health 0 (else the Health slot 0): a corpse, not a living creature
    uintptr_t tpl = 0;           // its template's data (Enemy01BehaviorState): who it is; 0 = unknown
    uint32_t owner = 0;          // OwnerRelationship.ownerEntityId
    int faction = -1;            // FactionType; -1 = none
    bool tame = false;           // IsTame
    bool villager = false;       // a villager (ActiveNpcState template)
    std::vector<std::string> items;  // the debug names of the stacks it holds ("" = not named by the game yet)
};

struct Death {
    uint32_t id = 0;
    Vec3 pos;            // where it died
    uint64_t at_ms = 0;  // when
    uint32_t session = 0;
    uintptr_t tpl = 0;   // its template's data when it died (0 = unknown: not compared)
};

struct Rules {
    double radius = 8.0;         // metres from the player
    uint64_t delay_ms = 1000;    // after the death, before the loot comes
    uint64_t window_ms = 15000;  // after the death: the magnet keeps looking this long (the game adds a corpse's
                                 // Inventory 1-3 s after the death); an older corpse is left for the player (exploring)
    double moved_max = 5.0;      // a body farther than this from where it died is not that corpse (an id reused)
    double base_radius = 30.0;   // nothing inside a base: within this of a flame altar (horizontally)
    bool materials_only = true;  // only corpses whose every stack is a material
    bool include_ammo = true;    // arrows count as materials
};

enum class Why { Ok, TooSoon, TooLate, TooFar, Looted, Protected, Moved, Gone, NoLoot, Empty, InBase, Filtered, Alive, Reused };

// For the decision log.
inline const char* why_text(Why w) {
    switch (w) {
        case Why::Ok: return "looted";
        case Why::TooSoon: return "too soon after the death";
        case Why::TooLate: return "too long after the death";
        case Why::TooFar: return "nobody with a magnet within reach";
        case Why::Looted: return "already looted";
        case Why::Protected: return "a player's, an ally's, a farm animal's, an owned or tame body, or a villager";
        case Why::Moved: return "more than 5 m from where it died (not that corpse)";
        case Why::Gone: return "gone (the game removed it)";
        case Why::NoLoot: return "no Health or no inventory (yet): nothing the loot event can take";
        case Why::Empty: return "empty, or an item the game does not name yet";
        case Why::InBase: return "inside a base";
        case Why::Filtered: return "holds something that is not a material (materials only)";
        case Why::Alive: return "not dead (alive again, or the id belongs to a living creature now)";
        case Why::Reused: return "the id belongs to another kind of entity now";
    }
    return "?";
}

// A material by its debug name, the rule life_skills uses for raw materials: material_, food_, _food_,
// build_terrainmaterial_ (any case); ammo_ when include_ammo.
inline bool is_material(const std::string& name, bool include_ammo) {
    std::string n = name;
    for (auto& c : n)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    for (const char* p : {"material_", "food_", "_food_", "build_terrainmaterial_"})
        if (n.rfind(p, 0) == 0) return true;
    return include_ammo && n.rfind("ammo_", 0) == 0;
}

// Factions never looted: Player 7, AutomatedPlayer 8, PlayerAlly 11 (summons, the companion), FarmAnimal 12.
inline bool protected_faction(int f) { return f == 7 || f == 8 || f == 11 || f == 12; }

// Whether a verdict is for good (the corpse is forgotten) or the magnet looks again on its next round: a corpse gets its
// loot (its Inventory) 1-3 s after the death, so "no inventory", "empty", "alive", "too soon" and "nobody near" may still
// change; who it belongs to, a base, a non-material in it, or the time running out do not.
inline bool is_final(Why w) {
    return w != Why::TooSoon && w != Why::TooFar && w != Why::NoLoot && w != Why::Empty && w != Why::Alive;
}

// May corpse `b` (died as `d`) be looted now into a player standing at `player`?
inline Why judge(const Death& d, const Body& b, const Vec3& player, const std::vector<Vec3>& bases, const Rules& r,
                 uint64_t now_ms, bool looted_before) {
    if (looted_before) return Why::Looted;
    if (!b.exists) return Why::Gone;
    if (now_ms < d.at_ms + r.delay_ms) return Why::TooSoon;
    if (now_ms > d.at_ms + r.window_ms) return Why::TooLate;
    // lasting reasons first: these never change
    if (d.tpl && b.tpl && b.tpl != d.tpl) return Why::Reused;
    if (b.player || protected_faction(b.faction) || b.owner || b.tame || b.villager) return Why::Protected;
    if (horizontal_distance(b.pos, d.pos) > r.moved_max) return Why::Moved;
    for (const auto& base : bases)
        if (horizontal_distance(b.pos, base) <= r.base_radius) return Why::InBase;
    // then what may still change: its loot comes a moment after the death
    if (!b.has_health || !b.has_inventory) return Why::NoLoot;
    if (!b.dead) return Why::Alive;
    if (distance(b.pos, player) > r.radius) return Why::TooFar;
    if (b.items.empty()) return Why::Empty;
    if (r.materials_only) {
        for (const auto& item : b.items)
            if (item.empty()) return Why::Empty;  // a name the game does not give yet: ask again next time
        for (const auto& item : b.items)
            if (!is_material(item, r.include_ammo)) return Why::Filtered;
    }
    return Why::Ok;
}

struct Looter {
    uint32_t id = 0;
    Vec3 pos;
    bool on = false;  // this player's magnet
};
struct Loot {
    uint32_t corpse = 0, player = 0;
    int tries = 1;  // loot events sent for this corpse so far (overdue)
};
// What became of a corpse the magnet watched (one per corpse, when it stops watching it), for the decision log.
struct Decision {
    uint32_t corpse = 0;
    Why why = Why::Ok;       // Ok: looted into `player`
    uint32_t player = 0;
    double distance = -1;    // to `player` (Ok), or to the nearest player with a magnet on (TooLate), -1 = none
    Why waited = Why::Ok;    // TooLate: what it was waiting for at the end
    bool offer = false;      // the body had an InteractionOfferRuntime (for the log)
};
using Lookup = std::function<Body(uint32_t id)>;

class Magnet {
public:
    static constexpr size_t kMaxWatched = 256;
    static constexpr uint64_t kConfirmMs = 2000;  // a PickupEvent from the corpse is expected within this
    static constexpr int kMaxTries = 5;           // a loot event nothing came from is sent again, this often at most

    void on_death(uint32_t id, const Vec3& pos, uint64_t now_ms, uint32_t session, uintptr_t tpl = 0) {
        if (session == session_) looted_.erase(id);  // a death of a looted id: a new creature reused it
        if (deaths_.size() >= kMaxWatched) deaths_.pop_front();
        deaths_.push_back({{id, pos, now_ms, session, tpl}, Why::TooSoon, -1});
    }

    // The corpses to loot now, each into the nearest player whose magnet is on and who is within the radius. A corpse
    // returned here is never returned again; corpses that can never be looted are forgotten, the others wait.
    std::vector<Loot> tick(uint64_t now_ms, uint32_t session, const std::vector<Looter>& players, const Lookup& body,
                           const std::vector<Vec3>& bases, const Rules& r, std::vector<Decision>* decisions = nullptr) {
        if (session != session_) {  // ids are reused after a reload: what we knew means nothing now
            session_ = session;
            looted_.clear();
            pending_.clear();
        }
        std::vector<Loot> out;
        auto decide = [&](const Watched& w, Why why, uint32_t player, double dist, bool offer) {
            if (decisions) decisions->push_back({w.d.id, why, player, dist, why == Why::TooLate ? w.waited : Why::Ok, offer});
        };
        for (auto it = deaths_.begin(); it != deaths_.end();) {
            if (it->d.session != session) {
                it = deaths_.erase(it);
                continue;
            }
            const Body b = body(it->d.id);
            // the corpse's own verdict first (a player standing on it), then who is near enough
            const Why corpse = judge(it->d, b, b.pos, bases, r, now_ms, looted_.count(it->d.id) != 0);
            if (!is_final(corpse)) {  // look again next round
                it->waited = corpse;
                ++it;
                continue;
            }
            if (corpse != Why::Ok) {
                decide(*it, corpse, 0, corpse == Why::TooLate ? it->nearest : -1, b.has_offer);
                it = deaths_.erase(it);
                continue;
            }
            const Looter* best = nullptr;
            double best_d = r.radius, nearest = -1;
            for (const auto& p : players) {
                if (!p.on) continue;
                const double dd = distance(p.pos, b.pos);
                if (nearest < 0 || dd < nearest) nearest = dd;
                if (dd <= best_d) {
                    best = &p;
                    best_d = dd;
                }
            }
            if (!best) {
                it->waited = Why::TooFar;
                it->nearest = nearest;
                ++it;
                continue;
            }
            out.push_back({it->d.id, best->id});
            decide(*it, Why::Ok, best->id, best_d, b.has_offer);
            looted_.insert(it->d.id);
            it = deaths_.erase(it);
        }
        return out;
    }

    // The loot event for `corpse` went out for `player`; a PickupEvent (by player, from corpse) confirms it.
    void on_sent(uint32_t corpse, uint32_t player, uint64_t now_ms, uint32_t session, int tries = 1) {
        pending_.push_back({corpse, player, now_ms, session, tries});
    }
    // Nothing came from the corpse (no PickupEvent) while it still holds loot: send again? (A corpse may become
    // lootable a moment after it died; nothing moved, so nothing is taken twice.)
    bool should_retry(const Loot& late) const { return late.tries < kMaxTries; }
    void on_pickup(uint32_t by, uint32_t from) {
        for (auto it = pending_.begin(); it != pending_.end();)
            it = it->player == by && it->corpse == from ? pending_.erase(it) : it + 1;
    }
    // Loot sent more than 2 s ago without a pickup (the mod then checks whether the corpse still holds loot: no room).
    std::vector<Loot> overdue(uint64_t now_ms) {
        std::vector<Loot> out;
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (now_ms > it->at_ms + kConfirmMs) {
                out.push_back({it->corpse, it->player, it->tries});
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }
    // "No room" is told once per player and session.
    bool should_tell_full(uint32_t player, uint32_t session) { return told_full_.insert({session, player}).second; }

    size_t watched() const { return deaths_.size(); }

private:
    struct Sent {
        uint32_t corpse, player;
        uint64_t at_ms;
        uint32_t session;
        int tries;
    };
    struct Watched {
        Death d;
        Why waited;      // what it waits for (TooSoon, NoLoot, Empty, Alive, TooFar)
        double nearest;  // TooFar: the nearest player with a magnet on, -1 = none
    };
    std::deque<Watched> deaths_;
    std::set<uint32_t> looted_;
    std::vector<Sent> pending_;
    std::set<std::pair<uint32_t, uint32_t>> told_full_;
    uint32_t session_ = 0;
};

// Each player's magnet switch, by account (the same in every session), kept in the world's file: "0x<16 hex> on|off"
// per line. Players without a line get the host's default.
class Toggles {
public:
    bool on(uint64_t account, bool fallback) const {
        auto it = map_.find(account);
        return it == map_.end() ? fallback : it->second;
    }
    void set(uint64_t account, bool on) { map_[account] = on; }
    void clear() { map_.clear(); }
    std::string text() const {
        std::string out;
        char b[48];
        for (const auto& [account, on] : map_) {
            snprintf(b, sizeof b, "0x%016llx %s\n", static_cast<unsigned long long>(account), on ? "on" : "off");
            out += b;
        }
        return out;
    }
    void parse(const std::string& text) {
        map_.clear();
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            at = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t space = line.find(' ');
            if (space == std::string::npos || line.compare(0, 2, "0x") != 0) continue;
            const std::string hex = line.substr(2, space - 2), word = line.substr(space + 1);
            if (hex.empty() || hex.size() > 16 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) continue;
            if (word != "on" && word != "off") continue;
            map_[std::stoull(hex, nullptr, 16)] = word == "on";
        }
    }

private:
    std::map<uint64_t, bool> map_;
};

}  // namespace magnet
