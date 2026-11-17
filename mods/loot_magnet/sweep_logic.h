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
// What looks alike but must stay (from the game's template list): enemy spawners (bone piles, spider eggs: some carry
// no Faction), thrown animal bait, a crop's stage (harvesting it ends the plant), fog nest resources, anything a player
// placed, anything already taken and fading.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
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
    bool disabled = false;  // its isDisabled: the game takes no interaction now (an emptied chest, a hidden swarm)
    int toggle = -1;        // ServerToggleState.state: -1 none, 0, 1
    uint32_t last_accept_guest = 0;  // lastAcceptData.guestId: who opened it last (0 = nobody)
    bool pickup = false;    // PickupItem
    std::string item;       // PickupItem's item, by debug name ("" = not named by the game yet)
    bool default_inv = false, inventory = false, setup = false, destroy_on_loot = false;
    int exclude = -1;  // AreaLootOverride.exclude: -1 none, 0, 1 (the game keeps 1 out of its own area loot)
    bool actor = false, faction = false;
    bool random_loot = false, random_container = false, loot_on_destroy = false, destructible = false;
    uint16_t max_health = 0;  // Destructible.maxHealth (0 = not read)
    bool spawn_on_destroy = false, mining_node = false;
    bool enemy_spawner = false;  // EnemySettings, RandomSpawner, TriggerSpawner or EnemySpawnerClient
    bool disable_loot_on_destroy = false;  // DisableRandomLootOnDestroy
    bool used_item = false;      // UsedItem: a player placed it
    bool food_bait = false;      // FoodBait: thrown animal bait (taming)
    bool crop_stage = false;     // the stage a crop's DynamicGrowth root points at (set by the walk)
    bool looted = false;         // DestroyOnLootActivated or DestroyOnLoot.shouldDestroy: taken, it fades out
    bool chest_excluded = false;  // any of UsedItem (a player placed it), UpgradeStorage, ItemCollectionComponent,
                                  // CreateItemLoot, DestroyOnLoot, InteractionLootRefill, AnimalTarget,
                                  // InteractionKnowledgeLock, LoreReading
    bool lock = false;            // InteractionLock (a scene chest has one; a searchable lore object does not)
    uint32_t lock_item = 0;       // its requiredItem: the accept would use up that item (a lockpick, a key)
    bool lock_item_ref = false;   // its requiredItemRef names an item by GUID: the same
    bool lock_item_used = false;  // DynamicInteractionLock.itemUsed: a player already opened it with the item
    std::vector<std::string> stacks;  // a chest's own Inventory's items (debug names; "" = not named yet)
    uint64_t fingerprint = 0;     // its SpawnTime: another entity under a reused id has another
};

inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline bool contains(const std::string& hay, const char* needle) { return lower(hay).find(lower(needle)) != std::string::npos; }
inline bool starts(const std::string& hay, const char* prefix) { return lower(hay).rfind(lower(prefix), 0) == 0; }

// A material or food item that no plant gives: stone, ore, wood, bones, fur, meat, drinks and the like, as they lie in
// ruins and camps (the game's item names: Material_T1_Stone, Food_T1_raw_bad_meat_LeanMeat ...).
inline bool not_a_plant(const std::string& item) {
    // whole parts of the name (split at _), so a plant whose name only contains such a word stays a plant (Watermelon,
    // Barley, Woodruff, Oregano, Stonecrop, Eggplant)
    static const char* const kWords[] = {"stone", "ore", "metal", "scrap", "scraps", "cloth", "charcoal", "clay", "amber",
                                         "obsidian", "wood", "logs", "twig", "twigs", "bone", "bones", "fur", "leather",
                                         "meat", "honey", "hydration", "water", "egg", "eggs", "resin", "salt", "string",
                                         "plank", "planks", "bar", "token", "skillpoint", "cooked", "processed", "dust",
                                         "feather", "wool", "grain", "fogcore", "flint", "sulfur", "milk"};
    static const char* const kEndings[] = {"meat", "stone", "eggs"};  // WolfMeat, LeanMeat, Moonstone, BirdEggs
    static const char* const kStarts[] = {"essence"};                 // Fog_Essence01_ShroudSpores
    const std::string n = lower(item);
    size_t start = 0;
    while (start <= n.size()) {
        size_t end = n.find('_', start);
        if (end == std::string::npos) end = n.size();
        const std::string part = n.substr(start, end - start);
        for (const char* w : kWords)
            if (part == w) return true;
        for (const char* w : kEndings) {
            const size_t len = std::strlen(w);
            if (part.size() > len && part.compare(part.size() - len, len, w) == 0) return true;
        }
        for (const char* w : kStarts)
            if (part.rfind(w, 0) == 0) return true;
        start = end + 1;
    }
    return false;
}

// The group of a pickup by its item. A drop (exclude absent or 0) is a ground item whatever it is (a collection item
// stays a collectible); the game's scene pickups it keeps out of its own area loot (exclude 1: keys, weapons, lore,
// mushrooms, fireflies, wild plants, materials in ruins) are taken only by the group of their kind of item.
inline Kind pickup_kind(const std::string& item, int exclude) {
    const bool collection = starts(item, "collection_");
    if (exclude != 1) return collection ? Kind::Collectible : Kind::Ground;
    if (collection) return Kind::Collectible;
    if (contains(item, "mushroom")) return Kind::Mushroom;
    if (contains(item, "fishingbait")) return Kind::Firefly;  // fireflies and the other bait critters
    if (!magnet::is_material(item, false)) return Kind::None;  // keys, weapons, lore: left to the player
    return not_a_plant(item) ? Kind::Ground : Kind::Plant;      // a wild plant that carries a pickup
}

inline Kind classify(const Thing& t) {
    if (t.disabled) return Kind::None;  // the game takes no interaction now (an emptied chest, a hidden swarm)
    if (t.random_loot && t.random_container && t.loot_on_destroy && t.destructible && !t.offer && !t.inventory &&
        !t.setup && !t.faction && !t.actor && !t.enemy_spawner && !t.spawn_on_destroy && !t.mining_node && !t.used_item &&
        !t.disable_loot_on_destroy)
        return Kind::Smash;
    if (t.random_loot && t.random_container && t.setup && t.inventory && t.lock && t.destructible && !t.chest_excluded &&
        !t.enemy_spawner)
        return Kind::Chest;
    if (!t.offer || !t.destroy_on_loot || t.actor || t.inventory || t.setup || t.faction || t.spawn_on_destroy ||
        t.enemy_spawner || t.used_item || t.food_bait || t.crop_stage || t.looted || t.disabled || t.toggle == 0)
        return Kind::None;
    if (t.pickup) return pickup_kind(t.item, t.exclude);
    if (!t.default_inv) return Kind::None;
    // without a pickup: plants can be destroyed; a bat's fur patch or a skeleton's bones (a creature's drop) cannot
    return t.destructible ? Kind::Plant : Kind::Ground;
}

// Items a quest needs: the game has no flag for them (they are scene data), so names that look like one stay.
inline bool quest_like(const std::string& item) {
    for (const char* p : {"quest", "_npc_", "loretext", "mappiece"})
        if (contains(item, p)) return true;
    return starts(item, "key_") || starts(item, "prop_component_");
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
    bool first = false;     // the first try for this entity (what loot.status counts)
};
struct Player {
    uint32_t id = 0;
    magnet::Vec3 pos;
    bool on = false;  // their magnet
    bool room = true;  // an empty slot in their bag (a pickup comes only then)
};

class Sweeper {
public:
    static constexpr int kMaxTries = 5;         // loot events for one entity (a full bag: it stays)
    static constexpr uint64_t kRetryMs = 2000;
    static constexpr int kMaxSmashes = 3;       // hits for one barrel that stays
    static constexpr uint64_t kSmashRetryMs = 3000;
    static constexpr int32_t kDefaultSmash = 1000;  // when its health was not read
    static constexpr uint64_t kRollMs = 3000;   // a chest still empty this long after the accept: nothing rolled yet
    static constexpr uint64_t kReacceptMs = 30000;  // then it is asked once more (a daytime or toggle lock may lift)
    static constexpr int kMaxAccepts = 2;
    static constexpr size_t kBudget = 16;       // actions a sweep
    static constexpr size_t kPerPlayer = 8;     // actions for one player a sweep (one in a field cannot starve others)
    static constexpr uint64_t kForgetMs = 60000;

    // `room`: how many more requests the program's queue takes (the actions booked are never more).
    std::vector<Action> tick(uint64_t now, uint32_t session, const std::vector<Player>& players,
                             const std::vector<Thing>& things, const std::vector<magnet::Vec3>& bases,
                             const SweepRules& r, size_t room = static_cast<size_t>(-1)) {
        if (session != session_) {
            session_ = session;
            st_.clear();
        }
        struct Cand {
            const Thing* t;
            Kind k;
            const Player* who;
            double d;
        };
        std::vector<Cand> cands;
        for (const Thing& t : things) {
            const Kind k = classify(t);
            if (k == Kind::None || !r.group(k).on) continue;
            const double base = r.base_radius + (k == Kind::Smash ? r.smash_margin : 0);
            bool in_base = false;
            for (const auto& b : bases) in_base = in_base || magnet::horizontal_distance(t.pos, b) <= base;
            if (in_base) continue;
            double d = 0;
            const Player* who = nearest(players, t.pos, r.reach_of(k), true, &d);
            if (!who) continue;
            // a chest or a barrel nearer to a player whose magnet is off is theirs to open
            if (k == Kind::Chest || k == Kind::Smash) {
                double d_any = 0;
                const Player* any = nearest(players, t.pos, r.reach_of(k), false, &d_any);
                if (any && !any->on) continue;
            }
            cands.push_back({&t, k, who, d});
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
        std::vector<Action> out;
        std::map<uint32_t, size_t> per_player;
        const size_t cap = (std::min)(kBudget, room);
        for (const Cand& c : cands) {
            if (out.size() >= cap) break;
            const Thing& t = *c.t;
            State& s = st_[t.id];
            if (s.fingerprint != t.fingerprint) s = State{};  // another entity under a reused id
            s.fingerprint = t.fingerprint;
            s.seen_ms = now;
            if (s.done || now < s.next_ms || per_player[c.who->id] >= kPerPlayer) continue;
            Action a;
            if (!decide(t, c.k, *c.who, now, s, r, &a)) continue;
            out.push_back(a);
            ++per_player[c.who->id];
        }
        for (const Thing& t : things)  // seen this sweep, also those not decided: they stay known
            if (auto it = st_.find(t.id); it != st_.end()) it->second.seen_ms = now;
        for (auto it = st_.begin(); it != st_.end();)
            it = now - it->second.seen_ms > kForgetMs ? st_.erase(it) : std::next(it);
        return out;
    }

    size_t watched() const { return st_.size(); }

    // The chests the magnet emptied in this world, as the mod's world file keeps them (one "x y z" in decimetres a line).
    void load_emptied(const std::string& text) {
        emptied_.clear();
        dirty_ = false;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream l(line);
            long x = 0, y = 0, z = 0;
            if (l >> x >> y >> z) emptied_.insert({x, y, z});
        }
    }
    std::string emptied_text() const {
        std::string out;
        for (const auto& [x, y, z] : emptied_) out += std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(z) + "\n";
        return out;
    }
    bool emptied_dirty() const { return dirty_; }  // a chest was emptied since the text was last saved
    void emptied_saved() { dirty_ = false; }
    size_t emptied_count() const { return emptied_.size(); }
    // The host's "forget": a world started anew in the same save slot would inherit the list (it belongs to the slot).
    void forget_emptied() {
        emptied_.clear();
        dirty_ = true;
    }

private:
    struct State {
        uint64_t seen_ms = 0, next_ms = 0, accepted_ms = 0;
        int tries = 0, accepts = 0;
        bool done = false;
        bool emptied = false;  // a chest: its loot event was sent
        uint64_t fingerprint = 0;
    };

    using Place = std::tuple<long, long, long>;  // a chest's position in decimetres
    static Place place_of(const magnet::Vec3& p) {
        return {std::lround(p.x * 10), std::lround(p.y * 10), std::lround(p.z * 10)};
    }

    bool decide(const Thing& t, Kind k, const Player& who, uint64_t now, State& s, const SweepRules& r, Action* a) {
        *a = Action{};
        a->kind = k;
        a->player = who.id;
        a->entity = t.id;
        if (k == Kind::Smash) {
            if (s.tries >= kMaxSmashes) {
                s.done = true;
                return false;
            }
            a->first = s.tries == 0;
            ++s.tries;
            s.next_ms = now + kSmashRetryMs;
            a->act = Act::Smash;
            a->amount = t.max_health ? t.max_health : kDefaultSmash;
            return true;
        }
        if (k == Kind::Chest) {
            // the accept would use up a lockpick or a key: it waits for a player to open it with the item
            if ((t.lock_item || t.lock_item_ref) && !t.lock_item_used) return false;
            if (t.stacks.empty()) {
                if (s.emptied || emptied_.count(place_of(t.pos))) {  // the magnet took its loot: done, never opened again
                    // seen empty after the magnet's loot event: remembered for the world (a chest that kept part of
                    // its loot, a full bag, is not: the next session leaves it to the player)
                    if (s.emptied && emptied_.insert(place_of(t.pos)).second) dirty_ = true;
                    s.done = true;
                    return false;
                }
                if (s.accepted_ms && now - s.accepted_ms < kRollMs) return false;  // the roll comes
                if (s.accepts >= kMaxAccepts) {  // asked twice, nothing rolled: an empty chest, or one that is closed
                    s.done = true;
                    return false;
                }
                if (s.accepted_ms && now - s.accepted_ms < kReacceptMs) return false;
                if (!t.offered) return false;  // not offered now (another player opens it): later
                a->first = s.accepts == 0;
                s.accepted_ms = now;
                ++s.accepts;
                a->act = Act::Accept;
                a->offer_id = t.offer_id;
                return true;
            }
            // rolled loot: only the magnet's own roll, or a chest this player opened themselves (another player's
            // open chest stays theirs)
            if (!s.accepted_ms && t.last_accept_guest != who.id) return false;
            bool keep = false, unnamed = false;
            for (const auto& item : t.stacks) {
                if (item.empty()) unnamed = true;
                keep = keep || quest_like(item) || r.ignore.match(item);
            }
            if (unnamed) return false;  // the game names it a moment later
            if (keep) {  // a quest item or an ignored one: the whole chest is left to the player
                s.done = true;
                return false;
            }
            s.emptied = true;
        } else if (t.pickup) {
            if (t.item.empty()) return false;  // the game names it a moment later
            if (!who.room) return false;       // a part that did not fit could be lost: wait for an empty slot
            if (quest_like(t.item) || r.ignore.match(t.item)) {
                s.done = true;
                return false;
            }
        }
        if (s.tries >= kMaxTries) {  // it stays: no room for it (or the game refused)
            s.done = true;
            return false;
        }
        a->first = s.tries == 0;
        ++s.tries;
        s.next_ms = now + kRetryMs;
        a->act = Act::Loot;
        return true;
    }

    // The nearest player within `reach`: with `on_only`, only those whose magnet is on.
    static const Player* nearest(const std::vector<Player>& players, const magnet::Vec3& p, double reach, bool on_only,
                                 double* dist) {
        const Player* best = nullptr;
        double best_d = reach;
        for (const auto& pl : players) {
            if (on_only && !pl.on) continue;
            const double d = magnet::distance(pl.pos, p);
            if (d <= best_d) {
                best_d = d;
                best = &pl;
            }
        }
        if (dist) *dist = best_d;
        return best;
    }
    std::map<uint32_t, State> st_;
    uint32_t session_ = 0;
    std::set<Place> emptied_;  // the chests the magnet emptied in this world (its file)
    bool dirty_ = false;
};

// When the flame altars must be read again before a sweep: a player arrived, or moved far since the last sweep (a
// fast travel, a join): the base list is refreshed every 10 s otherwise, and an altar that came into the world with
// them would not be known yet.
struct BaseWatch {
    static constexpr double kFar = 50.0;
    bool moved(const std::vector<Player>& players) {
        bool moved = false;
        std::map<uint32_t, magnet::Vec3> now;
        for (const auto& p : players) {
            now[p.id] = p.pos;
            auto it = last_.find(p.id);
            if (it == last_.end() || magnet::horizontal_distance(it->second, p.pos) > kFar) moved = true;
        }
        last_ = std::move(now);
        return moved;
    }

private:
    std::map<uint32_t, magnet::Vec3> last_;
};

}  // namespace sweep
