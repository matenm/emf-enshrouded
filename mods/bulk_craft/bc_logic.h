#pragma once
// Bulk Craft: the rules, without the game (tested in tests/test_bulk_craft.cpp). The game side is bc_glue.h.
//
// What a test in game showed (docs/engine_parts/inventory_crafting.md section 4): CraftRecipeEvent with a player as
// the operator crafts `recipeAmount` times from the player's own inventory into it, as many as the materials pay for,
// with no timer and no check of the player's unlocks, of the distance, or of the station being in use. So this mod:
//   - repeats only recipes the player crafted by hand (the game's crafting window offers only unlocked recipes), kept
//     per account and world with the window's category choice (Learned), and sends that same choice back;
//   - uses a station only within reach, with the recipe's own workshop, never a factory, the altar, an upgrade or a
//     recipe with station requirements (props, comfort, level, water...) EMF cannot check;
//   - paces each craft by the recipe's crafting time (never faster than the game), with a floor for instant recipes,
//     carried over from one order to the next of the same account;
//   - never moves an item itself: the game's craft core takes the materials and gives the output, all or nothing; and
//     it stops when a craft's materials did not leave the player's bag (the game took them from somewhere else).
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "bc_recipes.h"

namespace bulkcraft {

constexpr uint32_t kUnknown = 0xFFFFFFFFu;    // what the materials pay for is not known (category inputs only)
constexpr uint64_t kMinIntervalMs = 100;      // never two sends in the same moment
constexpr uint32_t kConfirmTicks = 45;        // a craft's output arrives within this many server steps, or none comes
constexpr size_t kMaxLearned = 200;           // recipes remembered per account (the oldest go)
constexpr uint32_t kAltarWorkshops[] = {0xb326e042u, 0x6f7d5d2fu};  // flame altar workshops (upgrades, base permission)

// ---- settings ----
enum class Pace { Paced, Instant };
struct Settings {
    Pace pace = Pace::Paced;
    double floor_s = 0.5;       // paced: seconds per craft for recipes without a crafting time
    uint32_t max_amount = 100;  // most crafts per order
    double range = 5.0;         // metres: a station farther than this is not used
    bool notes = true;
};
inline Pace pace_of(const std::string& s) { return s == "instant" ? Pace::Instant : Pace::Paced; }
// mod.json's limits (a hand-edited ini can hold anything).
inline Settings clamped(Settings s) {
    s.max_amount = (std::min)((std::max)(s.max_amount, 5u), 500u);
    s.range = (std::min)((std::max)(s.range, 2.0), 15.0);
    s.floor_s = (std::min)((std::max)(s.floor_s, 0.1), 5.0);
    return s;
}

// ---- category choices (RecipeInputCategorySelections: 4 x {category id, selected index}) ----
struct Choice {
    uint32_t category = 0;
    uint8_t index = 0;
};
using Choices = std::array<Choice, 4>;
using CategoryLookup = std::function<std::vector<CategoryItem>(uint32_t category)>;
enum class Resolve { Ok, NoChoice, BadChoice };
struct Resolved {
    Resolve result = Resolve::NoChoice;
    Recipe recipe;  // with every category input replaced by the chosen item and amount
};
// Fills each category input with the item `choices` picked (count x the item's scaling, rounded up), so the material
// check counts what the game will take. A category input without a matching choice, or with an index the category does
// not have, refuses: the game would pick an item itself (maybe a rare one).
inline Resolved resolve(const Recipe& r, const Choices& choices, const CategoryLookup& lookup) {
    Resolved out;
    out.recipe = r;
    out.recipe.inputs.clear();
    out.recipe.category_input = false;
    auto add = [&](uint32_t item, uint32_t count) {
        for (auto& s : out.recipe.inputs)
            if (s.item == item) {
                s.count += count;
                return;
            }
        out.recipe.inputs.push_back({item, count, 0});
    };
    for (const auto& in : r.inputs) {
        if (in.item) {
            add(in.item, in.count);
            continue;
        }
        const Choice* c = nullptr;
        for (const auto& ch : choices)
            if (ch.category && ch.category == in.category) c = &ch;
        if (!c) {
            out.result = Resolve::NoChoice;
            return out;
        }
        const auto items = lookup ? lookup(in.category) : std::vector<CategoryItem>{};
        if (c->index >= items.size() || !items[c->index].item || !(items[c->index].scaling > 0.0f)) {
            out.result = Resolve::BadChoice;
            return out;
        }
        const double need = std::ceil(static_cast<double>((std::max)(in.count, 1u)) * items[c->index].scaling - 1e-6);
        add(items[c->index].item, static_cast<uint32_t>((std::max)(need, 1.0)));
    }
    out.result = Resolve::Ok;
    return out;
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---- the amount ----
struct Amount {
    bool ok = false;
    bool max = false;  // "as many as the materials pay for" (up to the cap)
    uint32_t n = 0;
};
// "5", "x5", "×5", "max" or "all".
inline Amount parse_amount(const std::string& text) {
    Amount a;
    std::string s = lower(text);
    if (s == "max" || s == "all") {
        a.ok = a.max = true;
        return a;
    }
    if (!s.empty() && s[0] == 'x') s = s.substr(1);
    else if (s.size() >= 2 && static_cast<unsigned char>(s[0]) == 0xC3 && static_cast<unsigned char>(s[1]) == 0x97)
        s = s.substr(2);  // the times sign
    if (s.empty() || s.size() > 9) return a;
    for (char c : s)
        if (c < '0' || c > '9') return a;
    const unsigned long v = std::strtoul(s.c_str(), nullptr, 10);
    if (v == 0) return a;
    a.ok = true;
    a.n = static_cast<uint32_t>(v);
    return a;
}

// The crafts an order makes: the request, capped by the setting and (when known) by what the materials pay for.
inline uint32_t order_size(const Amount& a, uint32_t can_pay, uint32_t max_amount) {
    if (!a.ok || max_amount == 0) return 0;
    uint32_t want = a.max ? max_amount : (std::min)(a.n, max_amount);
    if (can_pay != kUnknown) want = (std::min)(want, can_pay);
    return want;
}

// ---- what the materials pay for ----
// Crafts the plain inputs pay for; kUnknown when the recipe has only category inputs not resolved yet (resolve() fills
// them with the chosen items first, so an order always counts them); 0 for a recipe without inputs (never bulk-crafted).
inline uint32_t can_pay(const Recipe& r, const std::map<uint32_t, uint32_t>& have) {
    if (r.inputs.empty()) return 0;
    uint32_t n = kUnknown;
    for (const auto& s : r.inputs) {
        if (!s.item || !s.count) continue;  // a category input
        auto it = have.find(s.item);
        n = (std::min)(n, (it == have.end() ? 0u : it->second) / s.count);
    }
    return n;
}
// The first plain input the player has too few of for one craft, or 0.
inline uint32_t missing_input(const Recipe& r, const std::map<uint32_t, uint32_t>& have) {
    for (const auto& s : r.inputs) {
        if (!s.item || !s.count) continue;
        auto it = have.find(s.item);
        if ((it == have.end() ? 0u : it->second) < s.count) return s.item;
    }
    return 0;
}

// ---- pacing ----
inline uint64_t duration_ms(const Recipe& r) {
    return r.duration_ns > 0 ? static_cast<uint64_t>((r.duration_ns + 999999) / 1000000) : 0;
}
// The time between two sends: paced = the crafting time, at least the floor; instant = the crafting time (0 without).
inline uint64_t interval_ms(const Recipe& r, const Settings& s) {
    const uint64_t d = duration_ms(r);
    if (s.pace == Pace::Instant) return d ? (std::max)(d, kMinIntervalMs) : 0;
    const uint64_t floor = s.floor_s > 0 ? static_cast<uint64_t>(s.floor_s * 1000.0 + 0.5) : 0;
    return (std::max)((std::max)(d, floor), kMinIntervalMs);
}
// Crafts one send asks for: all that are left in instant mode for a recipe without a crafting time, else one.
inline uint32_t batch(const Recipe& r, const Settings& s, uint32_t remaining) {
    if (s.pace == Pace::Instant && duration_ms(r) == 0) return (std::min)(remaining, 65535u);
    return remaining ? 1u : 0u;
}
inline double eta_seconds(const Recipe& r, const Settings& s, uint32_t crafts) {
    if (s.pace == Pace::Instant && duration_ms(r) == 0) return 0;
    return static_cast<double>(crafts) * static_cast<double>(interval_ms(r, s)) / 1000.0;
}

// ---- refusals ----
enum class Refusal { None, NotLearned, Upgrade, Factory, Altar, WrongStation, NoOutput, Requirements, NoChoice, Blocked };
// Station requirements the event may skip and EMF cannot check (the test in game exercised none of them).
inline bool needs_station_check(const Recipe& r) {
    return r.required_props || r.comfort || r.level || r.server_progress || r.happy_npcs || r.energy || r.water ||
           r.water_charges || r.sheltered;
}
inline Refusal check_recipe(const Recipe& r, uint32_t station_workshop, bool station_is_factory, bool learned) {
    if (station_is_factory) return Refusal::Factory;
    for (uint32_t a : kAltarWorkshops)
        if (r.workshop == a || station_workshop == a) return Refusal::Altar;
    if (r.upgrade) return Refusal::Upgrade;
    if (r.outputs.empty() || !r.outputs[0].item || !r.outputs[0].count) return Refusal::NoOutput;
    if (r.workshop != station_workshop) return Refusal::WrongStation;
    if (needs_station_check(r)) return Refusal::Requirements;
    if (!learned) return Refusal::NotLearned;
    return Refusal::None;
}
inline std::string refusal_text(Refusal x, const std::string& name) {
    switch (x) {
        case Refusal::NotLearned:
            return "Bulk craft repeats only what you crafted by hand: craft one " + name +
                   " in the game's crafting window first";
        case Refusal::Upgrade: return "Bulk craft does not do upgrades (" + name + ")";
        case Refusal::Factory: return "This station works by itself: put the materials in it";
        case Refusal::Altar: return "Bulk craft does not use the flame altar";
        case Refusal::WrongStation: return name + " is not made at this station";
        case Refusal::NoOutput: return name + " makes nothing Bulk craft can count";
        case Refusal::Requirements:
            return name + " needs a station check EMF can't do (comfort, crafting props, level, water...): craft it by hand";
        case Refusal::NoChoice:
            return "Bulk craft can't tell which items " + name +
                   " would use: craft one by hand in the game's crafting window, choosing the items";
        case Refusal::Blocked:
            return "Bulk craft stopped making " + name +
                   " at this kind of station: the game took its materials from outside your bag (a base chest?)";
        default: return "";
    }
}

// ---- the command's words ----
struct CraftArgs {
    bool ok = false;
    Amount amount;
    std::string filter;  // lower case; empty = the recipe you crafted last here
    std::string error;
};
inline CraftArgs parse_craft_args(const std::vector<std::string>& a) {
    CraftArgs c;
    if (a.size() < 2) {
        c.error = "usage: craft <how many | max> [part of the name]: craft that many of what you crafted by hand at the "
                  "station you look at (or the nearest)";
        return c;
    }
    c.amount = parse_amount(a[1]);
    if (!c.amount.ok) {
        c.error = "'" + a[1] + "' is not an amount: a number like 10, or max";
        return c;
    }
    for (size_t i = 2; i < a.size(); ++i) c.filter += (c.filter.empty() ? "" : " ") + lower(a[i]);
    c.ok = true;
    return c;
}

// ---- names ----
// "Material_T1_Wood_Logs" -> "Wood Logs": the category word, tier tokens and the CRAFTING / REWARD tags of building
// blocks go, unless one word would be left.
inline std::string pretty_item(const std::string& name) {
    std::vector<std::string> words;
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('_', start);
        if (end == std::string::npos) end = name.size();
        words.push_back(name.substr(start, end - start));
        start = end + 1;
    }
    if (words.size() <= 1) return name;
    auto tier = [](const std::string& w) { return w.size() == 2 && w[0] == 'T' && w[1] >= '0' && w[1] <= '9'; };
    auto marker = [](const std::string& w) { return w == "CRAFTING" || w == "REWARD"; };  // building blocks' tags
    std::vector<std::string> rest;
    for (size_t i = 1; i < words.size(); ++i)
        if (!tier(words[i]) && !marker(words[i]) && !words[i].empty()) rest.push_back(words[i]);
    if (rest.size() < 2) rest.insert(rest.begin(), words[0]);
    std::string out;
    for (const auto& w : rest) out += (out.empty() ? "" : " ") + w;
    return out;
}
// Every word of `filter` is part of the debug name or the readable one (ignoring case).
inline bool name_matches(const std::string& debug_name, const std::string& filter) {
    const std::string a = lower(debug_name), b = lower(pretty_item(debug_name));
    std::istringstream in(lower(filter));
    std::string w;
    while (in >> w)
        if (a.find(w) == std::string::npos && b.find(w) == std::string::npos) return false;
    return true;
}

// ---- the recipes each player crafted by hand (the knowledge check) ----
enum class LearnResult { None, Recency, Choice, New };  // what changed; worth a save: New and Choice
class Learned {
public:
    // A recipe seen crafted by hand (with the crafting window's category choice), or taught (`choices` null: an older
    // choice stays).
    LearnResult learn(uint64_t account, uint32_t recipe, const Choices* choices = nullptr) {
        if (!account || !recipe) return LearnResult::None;
        auto& v = by_account_[account];
        auto it = std::find_if(v.begin(), v.end(), [&](const Entry& e) { return e.recipe == recipe; });
        if (it == v.end()) {
            v.insert(v.begin(), Entry{recipe, choices ? *choices : Choices{}});
            if (v.size() > kMaxLearned) v.resize(kMaxLearned);
            return LearnResult::New;
        }
        LearnResult r = LearnResult::None;
        if (choices && !same(it->choices, *choices)) {
            it->choices = *choices;
            r = LearnResult::Choice;
        }
        if (it != v.begin()) {
            const Entry e = *it;
            v.erase(it);
            v.insert(v.begin(), e);
            if (r == LearnResult::None) r = LearnResult::Recency;
        }
        return r;
    }
    bool knows(uint64_t account, uint32_t recipe) const { return find(account, recipe) != nullptr; }
    // The category choice kept with a learned recipe (all empty when none was seen); nothing when not learned.
    std::optional<Choices> choices_of(uint64_t account, uint32_t recipe) const {
        const Entry* e = find(account, recipe);
        if (!e) return std::nullopt;
        return e->choices;
    }
    std::vector<uint32_t> of(uint64_t account) const {
        std::vector<uint32_t> out;
        auto it = by_account_.find(account);
        if (it != by_account_.end())
            for (const auto& e : it->second) out.push_back(e.recipe);
        return out;
    }
    void clear() { by_account_.clear(); }
    // One line per account: "acct <16 hex> <recipe>[/<category hex>.<index>]...", newest first.
    std::string text() const {
        std::string out;
        char b[40];
        for (const auto& [acct, v] : by_account_) {
            if (v.empty()) continue;
            snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(acct));
            out += std::string("acct ") + b;
            for (const auto& e : v) {
                snprintf(b, sizeof b, " %08x", e.recipe);
                out += b;
                for (const auto& c : e.choices)
                    if (c.category) {
                        snprintf(b, sizeof b, "/%08x.%u", c.category, static_cast<unsigned>(c.index));
                        out += b;
                    }
            }
            out += "\n";
        }
        return out;
    }
    // Reads text() back; what this session learned before stays in front. Bad words are skipped.
    void parse(const std::string& text) {
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream in(line);
            std::string word, acct;
            if (!(in >> word) || word != "acct" || !(in >> acct)) continue;
            uint64_t account = 0;
            if (!hex(acct, 16, &account) || !account) continue;
            auto& v = by_account_[account];
            while (in >> word) {
                std::vector<std::string> parts;
                size_t start = 0;
                while (true) {
                    const size_t slash = word.find('/', start);
                    parts.push_back(word.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
                    if (slash == std::string::npos) break;
                    start = slash + 1;
                }
                uint64_t r = 0;
                if (!hex(parts[0], 8, &r) || !r) continue;
                Entry e{static_cast<uint32_t>(r), Choices{}};
                size_t k = 0;
                for (size_t i = 1; i < parts.size() && k < e.choices.size(); ++i) {
                    const size_t dot = parts[i].find('.');
                    uint64_t cat = 0;
                    if (dot == std::string::npos || !hex(parts[i].substr(0, dot), 8, &cat) || !cat) continue;
                    const std::string idx = parts[i].substr(dot + 1);
                    if (idx.empty() || idx.size() > 3 || idx.find_first_not_of("0123456789") != std::string::npos) continue;
                    const unsigned long n = std::strtoul(idx.c_str(), nullptr, 10);
                    if (n > 255) continue;
                    e.choices[k++] = Choice{static_cast<uint32_t>(cat), static_cast<uint8_t>(n)};
                }
                const bool have = std::any_of(v.begin(), v.end(), [&](const Entry& x) { return x.recipe == e.recipe; });
                if (!have && v.size() < kMaxLearned) v.push_back(e);
            }
        }
    }

private:
    struct Entry {
        uint32_t recipe = 0;
        Choices choices{};
    };
    static bool same(const Choices& a, const Choices& b) {
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].category != b[i].category || (a[i].category && a[i].index != b[i].index)) return false;
        return true;
    }
    const Entry* find(uint64_t account, uint32_t recipe) const {
        auto it = by_account_.find(account);
        if (!account || it == by_account_.end()) return nullptr;
        for (const auto& e : it->second)
            if (e.recipe == recipe) return &e;
        return nullptr;
    }
    static bool hex(const std::string& s, size_t max_len, uint64_t* out) {
        if (s.empty() || s.size() > max_len) return false;
        for (char c : s)
            if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        *out = std::strtoull(s.c_str(), nullptr, 16);
        return true;
    }
    std::map<uint64_t, std::vector<Entry>> by_account_;
};

// The learned recipes (newest first) that this station makes, whose output matches `filter` (empty: all).
template <class NameOf>
std::vector<Recipe> learned_here(const std::vector<uint32_t>& learned, const std::vector<Recipe>& station,
                                 const std::string& filter, NameOf&& name_of) {
    std::vector<Recipe> out;
    for (uint32_t id : learned)
        for (const auto& r : station)
            if (r.id == id) {
                bool match = filter.empty();
                for (const auto& o : r.outputs)
                    if (!match && o.item) match = name_matches(name_of(o.item), filter);
                if (match) out.push_back(r);
                break;
            }
    return out;
}

// ---- the mod's own sends (the game's crafts teach recipes; the mod's do not) ----
class OwnSends {
public:
    void sent(uint32_t op, uint32_t recipe) { ++n_[{op, recipe}]; }
    // True when a seen CraftRecipeEvent was one of the mod's (consumes it).
    bool take(uint32_t op, uint32_t recipe) {
        auto it = n_.find({op, recipe});
        if (it == n_.end()) return false;
        if (--it->second == 0) n_.erase(it);
        return true;
    }
    void clear() { n_.clear(); }

private:
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> n_;
};

// ---- orders ----
struct Order {
    uint32_t player = 0;
    uint64_t account = 0;  // who asked: an order is theirs only while this account holds the player entity
    uint32_t station = 0;  // the player itself for hand crafting
    Recipe recipe;         // resolved: category inputs filled with the chosen items
    Choices choices{};     // the crafting window's category choice, sent with every craft
    uint32_t want = 0;
    uint32_t made = 0;
    Settings settings;
    bool chat = false;      // asked from the game's chat (answered with chat lines)
    uint32_t session = 0;
    // runner state
    bool awaiting = false;
    uint32_t asked = 0;
    uint32_t sent_tick = 0;
    uint64_t sent_ms = 0;
    uint64_t next_ms = 0;
    uint64_t got_items = 0;
    std::map<uint32_t, uint32_t> before;  // the bag at the send (the material check)
};
enum class Outcome { Done, GameMadeNoMore, Stopped };
struct Action {
    enum Kind { Send, Finished } kind = Send;
    uint32_t player = 0, station = 0, recipe = 0, amount = 0;  // Send
    Choices choices{};                                         // Send
    Order order;                                               // Finished
    Outcome outcome = Outcome::Done;
    std::string why;
    bool quiet = false;    // nobody to tell (the player's slot belongs to someone else now)
    bool foreign = false;  // the materials did not leave the bag: refuse this recipe at this station kind
};
// Before each send: an empty `why` to go on, else why the order stops ("you walked away from the station").
struct Stop {
    std::string why;
    bool quiet = false;
};
using StopCheck = std::function<Stop(const Order&)>;
// What the order's player carries now (item -> count), for the material check; null skips it.
using ContentsFn = std::function<std::map<uint32_t, uint32_t>(const Order&)>;

// Did `crafts` crafts take their inputs from the bag? Each input's count x crafts must have left it between `before`
// and `after`. If not, the game found the materials somewhere else (base storage, maybe another player's chest).
inline bool materials_from_bag(const Recipe& r, const std::map<uint32_t, uint32_t>& before,
                               const std::map<uint32_t, uint32_t>& after, uint32_t crafts) {
    for (const auto& in : r.inputs) {
        if (!in.item || !in.count) continue;
        auto b = before.find(in.item);
        auto a = after.find(in.item);
        const uint64_t was = b == before.end() ? 0 : b->second, now = a == after.end() ? 0 : a->second;
        const uint64_t left = was > now ? was - now : 0;
        if (left < static_cast<uint64_t>(in.count) * crafts) return false;
    }
    return true;
}

class Runner {
public:
    // The first send waits for the account's pace from its last send (another order, or a stopped one).
    bool start(Order o, uint64_t now_ms) {
        if (orders_.count(o.player) || o.want == 0 || o.recipe.outputs.empty()) return false;
        o.next_ms = (std::max)(now_ms, ready_at(o.account));
        o.awaiting = false;
        o.made = 0;
        orders_[o.player] = std::move(o);
        return true;
    }
    std::optional<Order> stop(uint32_t player) {
        auto it = orders_.find(player);
        if (it == orders_.end()) return std::nullopt;
        Order o = std::move(it->second);
        orders_.erase(it);
        return o;
    }
    const Order* running(uint32_t player) const {
        auto it = orders_.find(player);
        return it == orders_.end() ? nullptr : &it->second;
    }
    bool empty() const { return orders_.empty(); }
    void clear() {
        orders_.clear();
        ready_at_.clear();
    }
    // When the account may send its next craft (0: now).
    uint64_t ready_at(uint64_t account) const {
        auto it = ready_at_.find(account);
        return it == ready_at_.end() ? 0 : it->second;
    }
    // A PickupEvent: the player received `count` of `item` from `from` (0 for a craft's output, as seen in game).
    void on_pickup(uint32_t player, uint32_t item, uint32_t count, uint32_t from) {
        auto it = orders_.find(player);
        if (from || it == orders_.end() || !it->second.awaiting || item != it->second.recipe.outputs[0].item) return;
        it->second.got_items += count;
    }
    std::vector<Action> step(uint64_t now_ms, uint32_t tick, const StopCheck& check, const ContentsFn& contents = nullptr) {
        std::vector<Action> out;
        for (auto it = orders_.begin(); it != orders_.end();) {
            Order& o = it->second;
            bool finished = false;
            if (o.awaiting) {
                const uint32_t per = (std::max)(1u, o.recipe.outputs[0].count);
                const uint64_t crafts = o.got_items / per;
                const bool all = crafts >= o.asked, timed_out = tick - o.sent_tick > kConfirmTicks;
                if (all || timed_out) {
                    const uint32_t got = all ? o.asked : static_cast<uint32_t>(crafts);
                    o.made += got;
                    o.awaiting = false;
                    o.next_ms = (std::max)(now_ms, ready_at(o.account));
                    if (got && contents && !materials_from_bag(o.recipe, o.before, contents(o), got)) {
                        finished = finish(out, o, Outcome::Stopped,
                                          "the game took the materials from outside your bag (a base chest?)", false, true);
                    } else if (o.made >= o.want) {
                        finished = finish(out, o, Outcome::Done, "");
                    } else if (!all) {
                        finished = finish(out, o, Outcome::GameMadeNoMore, "");
                    }
                }
            }
            if (!finished && !o.awaiting && now_ms >= o.next_ms) {
                const Stop st = check ? check(o) : Stop{};
                if (!st.why.empty()) {
                    finished = finish(out, o, Outcome::Stopped, st.why, st.quiet);
                } else {
                    Action a;
                    a.kind = Action::Send;
                    a.player = o.player;
                    a.station = o.station;
                    a.recipe = o.recipe.id;
                    a.choices = o.choices;
                    a.amount = batch(o.recipe, o.settings, o.want - o.made);
                    o.asked = a.amount;
                    o.awaiting = true;
                    o.sent_tick = tick;
                    o.sent_ms = now_ms;
                    o.got_items = 0;
                    if (contents) o.before = contents(o);
                    ready_at_[o.account] = now_ms + interval_ms(o.recipe, o.settings);
                    out.push_back(a);
                }
            }
            it = finished ? orders_.erase(it) : std::next(it);
        }
        return out;
    }

private:
    static bool finish(std::vector<Action>& out, const Order& o, Outcome outcome, const std::string& why,
                       bool quiet = false, bool foreign = false) {
        Action a;
        a.kind = Action::Finished;
        a.player = o.player;
        a.order = o;
        a.outcome = outcome;
        a.why = why;
        a.quiet = quiet;
        a.foreign = foreign;
        out.push_back(std::move(a));
        return true;
    }
    std::map<uint32_t, Order> orders_;
    std::map<uint64_t, uint64_t> ready_at_;  // account -> its last send + that recipe's interval
};

// The note when an order ends. `label` = the output's readable name.
inline std::string finish_text(const Order& o, Outcome outcome, const std::string& why, const std::string& label) {
    std::string s = "Bulk craft: ";
    if (o.made == 0) {
        s += "made nothing";
    } else {
        const uint64_t items = static_cast<uint64_t>(o.made) * (o.recipe.outputs.empty() ? 1 : o.recipe.outputs[0].count);
        s += "made " + std::to_string(items) + " " + label + " (";
        if (o.made < o.want) s += std::to_string(o.made) + " of " + std::to_string(o.want);
        else s += std::to_string(o.made);
        s += o.want == 1 && o.made == 1 ? " craft)" : " crafts)";
    }
    if (outcome == Outcome::GameMadeNoMore)
        s += "; the game made no more (out of materials, no room, or a requirement not met)";
    else if (outcome == Outcome::Stopped)
        s += "; stopped: " + why;
    if (outcome == Outcome::Stopped && o.awaiting) s += " (the craft on its way may still arrive)";
    return s;
}

}  // namespace bulkcraft
