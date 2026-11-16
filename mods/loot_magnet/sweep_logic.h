#pragma once
// Loot Magnet 2.0's sweep: the things near a player that are collected without pressing E, by group, and how. Without
// the game (unit-tested in tests/test_loot_sweep.cpp); the mod (loot_magnet.cpp) reads each entity's components into a
// Thing (sweep_glue.h) and sends what this returns:
// - the game's own loot event {guest = player, host = it, lootAll, bypassInteraction}, as for corpses: it takes ground
//   drops, loose stones and twig piles, mushrooms, fireflies, plants and a chest's rolled loot (all seen in game);
// - a chest whose loot is not rolled yet: the game's AcceptOfferedInteractionEvent {player, chest, offerId} rolls it,
//   as a player's E does, without opening a window (seen in game), then the loot event takes it;
// - a barrel, crate or urn (RandomLootOnDestroy): a hit with its own health from the player; the game drops its loot as
//   a ground pickup, which the ground group then takes.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "magnet_logic.h"

namespace sweep {

enum class Kind { None, Ground, Collectible, Mushroom, Firefly, Plant, Chest, Smash };
constexpr size_t kKinds = 8;

inline const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Ground: return "ground";
        case Kind::Collectible: return "collectible";
        case Kind::Mushroom: return "mushroom";
        case Kind::Firefly: return "firefly";
        case Kind::Plant: return "plant";
        case Kind::Chest: return "chest";
        case Kind::Smash: return "smash";
        default: return "none";
    }
}

// What the mod read from an entity (instance or template components).
struct Thing {
    uint32_t id = 0;
    magnet::Vec3 pos;
    bool offer = false;     // InteractionOfferRuntime
    uint32_t offer_id = 0;  // its offerId (the accept must name it)
    bool offered = false;   // its isOffered (a player in the game's range; nobody else accepting)
    bool pickup = false;    // PickupItem
    std::string item;       // PickupItem's item, by debug name ("" = not named by the game yet)
    bool default_inv = false, inventory = false, setup = false, destroy_on_loot = false;
    int exclude = -1;  // AreaLootOverride.exclude: -1 none, 0, 1 (the game keeps 1 out of its own area loot)
    bool actor = false, faction = false;
    bool random_loot = false, random_container = false, loot_on_destroy = false, destructible = false;
    uint16_t max_health = 0;  // Destructible.maxHealth (0 = not read)
    bool spawn_on_destroy = false, mining_node = false;
    bool chest_excluded = false;  // any of UsedItem (a player placed it), UpgradeStorage, ItemCollectionComponent,
                                  // CreateItemLoot, DestroyOnLoot, InteractionLootRefill, AnimalTarget,
                                  // InteractionKnowledgeLock, LoreReading
    bool lock = false;            // InteractionLock (a scene chest has one; a searchable lore object does not)
    uint32_t lock_item = 0;       // its requiredItem: the accept would use up that item (a lockpick, a key)
    bool lock_item_used = false;  // DynamicInteractionLock.itemUsed: a player already opened it with the item
    std::vector<std::string> stacks;  // a chest's own Inventory's items (debug names)
};

inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline bool contains(const std::string& hay, const char* needle) { return lower(hay).find(lower(needle)) != std::string::npos; }

// The group of a pickup by its item. A scene item the game keeps out of its own area loot (exclude 1: keys, weapons,
// lore) stays unless a group asks for its kind of item.
inline Kind pickup_kind(const std::string& item, int exclude) {
    if (lower(item).rfind("collection_", 0) == 0) return Kind::Collectible;
    if (contains(item, "mushroom")) return Kind::Mushroom;
    if (contains(item, "fishingbait")) return Kind::Firefly;  // fireflies and the other bait critters
    if (exclude != 1) return Kind::Ground;
    return magnet::is_material(item, false) ? Kind::Plant : Kind::None;  // a wild plant that carries a pickup
}

inline Kind classify(const Thing& t) {
    if (t.random_loot && t.random_container && t.loot_on_destroy && t.destructible && !t.offer && !t.inventory &&
        !t.faction && !t.spawn_on_destroy && !t.mining_node)
        return Kind::Smash;
    if (t.random_loot && t.random_container && t.setup && t.inventory && t.lock && t.destructible && !t.chest_excluded)
        return Kind::Chest;
    if (!t.offer || !t.destroy_on_loot || t.actor || t.inventory || t.setup) return Kind::None;
    if (t.pickup) return pickup_kind(t.item, t.exclude);
    return t.default_inv ? Kind::Plant : Kind::None;
}

// Items a quest needs: the game has no flag for them (they are scene data), so names that look like one stay.
inline bool quest_like(const std::string& item) {
    for (const char* p : {"quest", "_npc_", "loretext", "mappiece", "key_"})
        if (contains(item, p)) return true;
    return false;
}

// The player's ignore list: item debug names, comma separated, any case; a trailing * takes every name that begins so.
struct Ignore {
    std::vector<std::string> exact, prefix;
    static Ignore parse(const std::string& text) {
        Ignore ig;
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find(',', start);
            if (end == std::string::npos) end = text.size();
            std::string w = text.substr(start, end - start);
            const size_t a = w.find_first_not_of(" \t");
            const size_t b = w.find_last_not_of(" \t");
            w = a == std::string::npos ? std::string() : lower(w.substr(a, b - a + 1));
            if (!w.empty()) {
                if (w.back() == '*') {
                    w.pop_back();
                    if (!w.empty()) ig.prefix.push_back(w);
                } else {
                    ig.exact.push_back(w);
                }
            }
            start = end + 1;
        }
        return ig;
    }
    bool match(const std::string& item) const {
        const std::string n = lower(item);
        if (std::find(exact.begin(), exact.end(), n) != exact.end()) return true;
        for (const auto& p : prefix)
            if (n.rfind(p, 0) == 0) return true;
        return false;
    }
};

struct Group {
    bool on = false;
    double reach = 0;  // metres; 0 = the general reach
};
struct SweepRules {
    // ground items and collectibles on, the rest off, as the owner chose
    std::array<Group, kKinds> groups = {Group{}, Group{true, 0}, Group{true, 0}, Group{}, Group{}, Group{}, Group{}, Group{}};
    double reach = 8;          // the general reach (Loot Magnet's "Reach (m)")
    double base_radius = 30;   // nothing within this of a flame altar
    double smash_margin = 10;  // smashing keeps this much more away from a base
    Ignore ignore;
    const Group& group(Kind k) const { return groups[static_cast<size_t>(k)]; }
    double reach_of(Kind k) const { return group(k).reach > 0 ? group(k).reach : reach; }
};

enum class Act { Loot, Accept, Smash };
struct Action {
    Act act = Act::Loot;
    Kind kind = Kind::None;
    uint32_t player = 0, entity = 0;
    int32_t amount = 0;     // Smash: the hit's damage
    uint32_t offer_id = 0;  // Accept: the offer it accepts
};
struct Player {
    uint32_t id = 0;
    magnet::Vec3 pos;
    bool on = false;  // their magnet
};

class Sweeper {
public:
    static constexpr int kMaxTries = 5;         // loot events for one entity (a full bag: it stays)
    static constexpr uint64_t kRetryMs = 2000;
    static constexpr int kMaxSmashes = 3;       // hits for one barrel that stays
    static constexpr uint64_t kSmashRetryMs = 3000;
    static constexpr int32_t kDefaultSmash = 1000;  // when its health was not read
    static constexpr uint64_t kRollMs = 3000;   // a chest still empty this long after the accept: nothing in it
    static constexpr size_t kBudget = 16;       // actions a sweep
    static constexpr uint64_t kForgetMs = 60000;

    std::vector<Action> tick(uint64_t now, uint32_t session, const std::vector<Player>& players,
                             const std::vector<Thing>& things, const std::vector<magnet::Vec3>& bases,
                             const SweepRules& r) {
        if (session != session_) {
            session_ = session;
            st_.clear();
        }
        std::vector<Action> out;
        for (const Thing& t : things) {
            if (out.size() >= kBudget) break;
            const Kind k = classify(t);
            if (k == Kind::None || !r.group(k).on) continue;
            const double base = r.base_radius + (k == Kind::Smash ? r.smash_margin : 0);
            bool in_base = false;
            for (const auto& b : bases) in_base = in_base || magnet::horizontal_distance(t.pos, b) <= base;
            if (in_base) continue;
            const Player* who = nearest(players, t.pos, r.reach_of(k));
            if (!who) continue;
            State& s = st_[t.id];
            s.seen_ms = now;
            if (s.done || now < s.next_ms) continue;
            if (k == Kind::Smash) {
                if (s.tries >= kMaxSmashes) {
                    s.done = true;
                    continue;
                }
                ++s.tries;
                s.next_ms = now + kSmashRetryMs;
                out.push_back({Act::Smash, k, who->id, t.id, t.max_health ? t.max_health : kDefaultSmash, 0});
                continue;
            }
            if (k == Kind::Chest) {
                // the accept would use up a lockpick or a key: it waits for a player to open it with the item
                if (t.lock_item && !t.lock_item_used) continue;
                if (t.stacks.empty()) {
                    if (s.accepted_ms) {
                        if (now - s.accepted_ms >= kRollMs) s.done = true;  // nothing rolled: an empty chest
                        continue;
                    }
                    if (!t.offered) continue;  // not offered now (another player opens it): later
                    s.accepted_ms = now;
                    out.push_back({Act::Accept, k, who->id, t.id, 0, t.offer_id});
                    continue;
                }
                bool keep = false;
                for (const auto& item : t.stacks) keep = keep || quest_like(item) || r.ignore.match(item);
                if (keep) {  // a quest item or an ignored one: the whole chest is left to the player
                    s.done = true;
                    continue;
                }
            } else if (t.pickup) {
                if (t.item.empty()) continue;  // the game names it a moment later
                if (quest_like(t.item) || r.ignore.match(t.item)) {
                    s.done = true;
                    continue;
                }
            }
            if (s.tries >= kMaxTries) {  // it stays: no room for it (or the game refused)
                s.done = true;
                continue;
            }
            ++s.tries;
            s.next_ms = now + kRetryMs;
            out.push_back({Act::Loot, k, who->id, t.id, 0, 0});
        }
        for (auto it = st_.begin(); it != st_.end();)
            it = now - it->second.seen_ms > kForgetMs ? st_.erase(it) : std::next(it);
        return out;
    }

    size_t watched() const { return st_.size(); }

private:
    struct State {
        uint64_t seen_ms = 0, next_ms = 0, accepted_ms = 0;
        int tries = 0;
        bool done = false;
    };
    static const Player* nearest(const std::vector<Player>& players, const magnet::Vec3& p, double reach) {
        const Player* best = nullptr;
        double best_d = reach;
        for (const auto& pl : players) {
            if (!pl.on) continue;
            const double d = magnet::distance(pl.pos, p);
            if (d <= best_d) {
                best_d = d;
                best = &pl;
            }
        }
        return best;
    }
    std::map<uint32_t, State> st_;
    uint32_t session_ = 0;
};

}  // namespace sweep
