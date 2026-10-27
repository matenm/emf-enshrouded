#pragma once
// quick_stack (Quick Stack and Chest Machines): what to move where (no game; unit-tested).
//   - Quick stack: one key sends the items you carry into the nearby chests that already hold the same item
//     (Valheim/Terraria "quick stack"). Gear with its own data entity is never moved, and slots the player keeps
//     (the action bar by default, and the filters) stay put.
//   - Sorting chest: every few seconds each plain stack in it goes to the nearest chest that already holds that item
//     (or, with overflow, to an overflow chest).
//   - Recycler: a crafted item goes back into a share of its recipe's inputs, rounded down (never more than it cost).
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace quickstack {

// One stack the player carries (or one stack in a chest).
struct Carried {
    uint64_t slot = 0;   // InventorySlotId (inventory entity | slot << 32)
    uint32_t item = 0;
    uint32_t count = 0;
    bool gear = false;   // has its own data entity (level, perks): never moved
    bool kept = false;   // in a kept inventory (the action bar) or kept by a filter: never moved
};
// A chest near the player and what it holds.
struct Chest {
    uint32_t id = 0;
    double distance = 0;
    std::map<uint32_t, uint32_t> contents;  // item -> count
    bool overflow = false;                  // an overflow chest: takes from a sorting chest what nobody holds
    bool room_known = false;                // `room` says how much more of an item fits (else: no limit is known)
    std::map<uint32_t, uint64_t> room;      // item -> how many more fit (free slots and part-filled stacks)
};
// Take `count` of `item` from `slot` and put it into `chest`.
struct Move {
    uint32_t chest = 0;
    uint32_t item = 0;
    uint32_t count = 0;
    uint64_t slot = 0;
};

namespace detail {
// Sends `s` into the candidates nearest first, as much as each has room for (a chest whose room is not known takes
// all); what fits nowhere stays where it is (so nothing is taken only to come back). `room` is used up as it goes.
inline void place(const Carried& s, std::vector<Chest*>& cands, std::vector<Move>& out) {
    std::sort(cands.begin(), cands.end(), [](const Chest* a, const Chest* b) { return a->distance < b->distance; });
    uint32_t left = s.count;
    for (Chest* ch : cands) {
        if (!left) break;
        uint32_t n = left;
        if (ch->room_known) {
            uint64_t& room = ch->room[s.item];
            n = static_cast<uint32_t>(room < left ? room : left);
            room -= n;
        }
        if (!n) continue;
        out.push_back({ch->id, s.item, n, s.slot});
        left -= n;
    }
}
}  // namespace detail

// Every movable carried stack goes to the nearest chest that already holds that item (nothing else is touched); with
// the chests' room known, only as much as fits, the rest into the next nearest holder or left with the player.
inline std::vector<Move> plan(const std::vector<Carried>& carried, std::vector<Chest> chests) {
    std::vector<Move> out;
    for (const auto& c : carried) {
        if (c.gear || c.kept || !c.item || !c.count) continue;
        std::vector<Chest*> holders;
        for (auto& ch : chests)
            if (ch.contents.count(c.item)) holders.push_back(&ch);
        detail::place(c, holders, out);
    }
    return out;
}

// The note after a quick stack: what moved, and the stacks whose take was refused or found nothing (busy queue, or
// the slot changed): pressing again picks those up.
inline std::string summary(uint32_t moved, size_t chests, size_t skipped) {
    const std::string busy = std::to_string(skipped) + " stack(s)";
    if (moved) {
        std::string s = "Quick stack: " + std::to_string(moved) + " item(s) into " + std::to_string(chests) + " chest(s)";
        if (skipped) s += "; " + busy + " skipped (busy): press again";
        return s;
    }
    if (skipped) return "Quick stack: " + busy + " could not be taken right now: press again";
    return "Quick stack: nothing fitted (are the chests full?)";
}

// ---- filters for the key and /qs ----
struct Filters {
    bool keep_ammo = false;            // arrows (Ammo_...) stay with the player
    bool keep_food = false;            // food (Food_..., ..._Food_...) stays
    std::vector<std::string> exclude;  // parts of item names (lower case) that stay
};

inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "torch, resin bait" -> {"torch", "resin", "bait"}: commas, semicolons or spaces between, case ignored, empty parts
// dropped.
inline std::vector<std::string> parse_exclude(const std::string& text) {
    std::vector<std::string> out;
    std::string word;
    for (char c : text + ",") {
        if (c == ',' || c == ' ' || c == ';' || c == '\t') {
            if (!word.empty()) out.push_back(lower(word));
            word.clear();
        } else {
            word += c;
        }
    }
    return out;
}

// True when the item (by its debug name) stays with the player.
inline bool filtered(const std::string& item_name, const Filters& f) {
    const std::string n = lower(item_name);
    if (f.keep_ammo && n.rfind("ammo_", 0) == 0) return true;
    if (f.keep_food && (n.rfind("food_", 0) == 0 || n.find("_food_") != std::string::npos)) return true;
    for (const auto& part : f.exclude)
        if (!part.empty() && n.find(part) != std::string::npos) return true;
    return false;
}

// ---- marked chests ----
enum class Mark : uint8_t { None, Sort, Recycle, Overflow };

inline const char* mark_name(Mark m) {
    switch (m) {
        case Mark::Sort: return "sort";
        case Mark::Recycle: return "recycle";
        case Mark::Overflow: return "overflow";
        default: return "none";
    }
}
inline const char* mark_label(Mark m) {
    switch (m) {
        case Mark::Sort: return "sorting chest";
        case Mark::Recycle: return "recycler";
        case Mark::Overflow: return "overflow chest";
        default: return "normal chest";
    }
}
inline Mark mark_of(const std::string& word) {
    const std::string w = lower(word);
    if (w == "sort" || w == "sorting") return Mark::Sort;
    if (w == "recycle" || w == "recycler") return Mark::Recycle;
    if (w == "overflow") return Mark::Overflow;
    return Mark::None;
}

struct Vec3 {
    double x = 0, y = 0, z = 0;
};
inline double dist(const Vec3& a, const Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

constexpr double kPlaceTolerance = 0.5;  // metres: a chest further than this from a mark's place is another chest

// The marks of a world, by the chest's place (entity ids change with a reload). Saved as text, one per line:
// "sort 12.50 40.00 -3.25".
class Marks {
public:
    struct Record {
        Mark mark = Mark::None;
        Vec3 pos;
    };
    // The chest at `pos` gets `mark` (None takes its mark off); returns the mark it had.
    Mark set(const Vec3& pos, Mark mark) {
        for (auto it = all_.begin(); it != all_.end(); ++it) {
            if (dist(it->pos, pos) > kPlaceTolerance) continue;
            const Mark old = it->mark;
            if (mark == Mark::None) all_.erase(it);
            else it->mark = mark;
            return old;
        }
        if (mark != Mark::None) all_.push_back({mark, pos});
        return Mark::None;
    }
    Mark at(const Vec3& pos) const {
        for (const auto& r : all_)
            if (dist(r.pos, pos) <= kPlaceTolerance) return r.mark;
        return Mark::None;
    }
    const std::vector<Record>& all() const { return all_; }
    void clear() { all_.clear(); }
    // Numbers with a '.' whatever the C or C++ locale says (to_chars / from_chars never use one).
    std::string text() const {
        std::string out;
        for (const auto& r : all_) {
            out += mark_name(r.mark);
            for (double v : {r.pos.x, r.pos.y, r.pos.z}) {
                char num[48];
                const auto res = std::to_chars(num, num + sizeof num, v, std::chars_format::fixed, 2);
                out += ' ';
                out.append(num, res.ec == std::errc() ? res.ptr : num);
            }
            out += '\n';
        }
        return out;
    }
    // Reads the text (bad lines are skipped); returns how many marks were read.
    size_t parse(const std::string& text) {
        all_.clear();
        size_t from = 0;
        while (from < text.size()) {
            size_t to = text.find('\n', from);
            if (to == std::string::npos) to = text.size();
            std::vector<std::string> words;
            for (size_t i = from; i < to;) {
                while (i < to && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r')) ++i;
                const size_t start = i;
                while (i < to && text[i] != ' ' && text[i] != '\t' && text[i] != '\r') ++i;
                if (i > start) words.push_back(text.substr(start, i - start));
            }
            from = to + 1;
            Vec3 p;
            if (words.size() < 4 || !number(words[1], p.x) || !number(words[2], p.y) || !number(words[3], p.z)) continue;
            const Mark m = mark_of(words[0]);
            if (m != Mark::None) set(p, m);
        }
        return all_.size();
    }

private:
    // The whole word is one number ("12.50"; "1,50" is not).
    static bool number(const std::string& word, double& out) {
        const char* end = word.data() + word.size();
        const auto res = std::from_chars(word.data(), end, out);
        return res.ec == std::errc() && res.ptr == end;
    }
    std::vector<Record> all_;
};

// When a marked chest counts as gone (taken down): only after it was looked for and missed, with a player near its
// place (the place is loaded then), on looks spanning 30 s, and never in the first 30 s after a world load (chests may
// still be streaming in). A look that finds it, or one with nobody near, starts the clock over.
class MissBook {
public:
    static constexpr uint64_t kGraceMs = 30000;
    void reset(uint64_t now_ms) {
        loaded_ms_ = now_ms;
        first_.clear();
    }
    void found(const Vec3& pos) { forget(pos); }
    // True when the chest at `pos` is gone.
    bool missing(const Vec3& pos, bool player_near, uint64_t now_ms) {
        if (now_ms - loaded_ms_ < kGraceMs) return false;
        if (!player_near) {
            forget(pos);
            return false;
        }
        for (const auto& [p, since] : first_)
            if (dist(p, pos) <= kPlaceTolerance) return now_ms - since >= kGraceMs;
        first_.push_back({pos, now_ms});
        return false;
    }

private:
    void forget(const Vec3& pos) {
        first_.erase(std::remove_if(first_.begin(), first_.end(),
                                    [&](const std::pair<Vec3, uint64_t>& e) { return dist(e.first, pos) <= kPlaceTolerance; }),
                     first_.end());
    }
    uint64_t loaded_ms_ = 0;
    std::vector<std::pair<Vec3, uint64_t>> first_;
};

// Which chest a mark found at its place this session. A mark belongs to a place; if another chest turns up there (the
// host took the marked chest down and put another one in its spot), the mark is dropped: the new chest must never
// become a recycler unasked. After a load (a new session) ids change, so the first chest found is taken.
class MarkBinder {
public:
    enum Result { First, Same, Swapped };
    void reset() { seen_.clear(); }
    Result seen(const Vec3& pos, uint32_t id) {
        for (auto& [p, known] : seen_)
            if (dist(p, pos) <= kPlaceTolerance) {
                if (known == id) return Same;
                known = id;
                return Swapped;
            }
        seen_.push_back({pos, id});
        return First;
    }
    void forget(const Vec3& pos) {
        seen_.erase(std::remove_if(seen_.begin(), seen_.end(),
                                   [&](const std::pair<Vec3, uint32_t>& e) { return dist(e.first, pos) <= kPlaceTolerance; }),
                    seen_.end());
    }

private:
    std::vector<std::pair<Vec3, uint32_t>> seen_;
};

// A quick stack moves at most this many stacks per press (the game's item queue holds 64 requests and runs 8 a step);
// the note says how many are left: press again.
constexpr size_t kMaxQuickMoves = 24;
inline std::string left_note(size_t left) {
    return left ? "; " + std::to_string(left) + " more stack(s): press again for the rest" : "";
}
// Keeps at most kMaxQuickMoves moves, and no more than `queue_free` (the mod's room in the item queue: a move holds one
// place from its take to its end); how many were cut (left for the next press).
inline size_t cap_moves(std::vector<Move>& moves, int queue_free) {
    const size_t cap = (std::min)(kMaxQuickMoves, static_cast<size_t>((std::max)(0, queue_free)));
    if (moves.size() <= cap) return 0;
    const size_t left = moves.size() - cap;
    moves.resize(cap);
    return left;
}

// A chest a recycler's materials may go to: its distance, its free slots, whether it holds the first material.
struct Spot {
    uint32_t id = 0;
    double distance = 0;
    uint32_t free = 0;
    bool holds = false;
};
// Where a recycler's materials go (never the recycler: it would recycle them again, losing more each time): the
// nearest sorting chest with room for `need` stacks (`to_sorter`), else the nearest chest that holds the first material
// and has room, else the nearest chest with room; 0 = none (the stack stays in the recycler until there is room).
inline uint32_t recycle_target(std::vector<Spot> sorters, std::vector<Spot> chests, uint32_t need, bool to_sorter) {
    auto nearest = [](const Spot& a, const Spot& b) { return a.distance < b.distance; };
    std::stable_sort(sorters.begin(), sorters.end(), nearest);
    std::stable_sort(chests.begin(), chests.end(), nearest);
    if (to_sorter)
        for (const auto& c : sorters)
            if (c.free >= need) return c.id;
    for (const auto& c : chests)
        if (c.holds && c.free >= need) return c.id;
    for (const auto& c : chests)
        if (c.free >= need) return c.id;
    return 0;
}

// ---- stack sizes ----
// The items' largest stack sizes as the game last said (0: not known yet). Big Stacks (and the console's stack
// command) change them while the game runs, so a size is read again once it is kMaxAgeMs old, when a stack bigger
// than it is seen, and after clear() (a new session). A size of 0 is never kept: it is asked again next time.
class StackSizes {
public:
    static constexpr uint64_t kMaxAgeMs = 10000;
    uint32_t get(uint32_t item, uint64_t now_ms, const std::function<uint32_t(uint32_t)>& read) {
        auto it = known_.find(item);
        if (it != known_.end() && now_ms - it->second.read_ms < kMaxAgeMs) return it->second.size;
        const uint32_t size = read(item);
        if (size) known_[item] = {size, now_ms};
        else known_.erase(item);
        return size;
    }
    // A stack of `count` was seen. True when it is bigger than the known size: the size is forgotten (read again).
    bool saw(uint32_t item, uint32_t count) {
        auto it = known_.find(item);
        if (it == known_.end() || count <= it->second.size) return false;
        known_.erase(it);
        return true;
    }
    void clear() { known_.clear(); }

private:
    struct Known {
        uint32_t size = 0;
        uint64_t read_ms = 0;
    };
    std::map<uint32_t, Known> known_;
};

// ---- sorting ----
// (chest, item) pairs that took nothing on the last try (full): skipped for that item for a minute, so a full target
// does not bounce the same stack back every few seconds.
class FullBook {
public:
    static constexpr uint64_t kMs = 60000;
    void mark(uint32_t chest, uint32_t item, uint64_t now_ms) { until_[{chest, item}] = now_ms + kMs; }
    bool full(uint32_t chest, uint32_t item, uint64_t now_ms) const {
        auto it = until_.find({chest, item});
        return it != until_.end() && now_ms < it->second;
    }
    void clear() { until_.clear(); }

private:
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> until_;
};

// Each plain stack of a sorting chest goes to the nearest target that already holds that item (with the room known:
// as much as fits, then the next holder); what nobody holds goes to the nearest overflow target. Gear never moves.
// `targets`: the chests in reach, never sorting chests or recyclers.
inline std::vector<Move> sort_plan(const std::vector<Carried>& stacks, std::vector<Chest> targets, const FullBook& full,
                                   uint64_t now_ms) {
    std::vector<Move> out;
    for (const auto& s : stacks) {
        if (s.gear || s.kept || !s.item || !s.count) continue;
        std::vector<Chest*> holders, spare;
        for (auto& ch : targets) {
            if (full.full(ch.id, s.item, now_ms)) continue;
            if (ch.contents.count(s.item)) holders.push_back(&ch);
            else if (ch.overflow) spare.push_back(&ch);
        }
        detail::place(s, holders.empty() ? spare : holders, out);
    }
    return out;
}

inline std::string sort_note(size_t stacks, size_t chests) {
    return "Sorting chest: " + std::to_string(stacks) + " stack(s) put away into " + std::to_string(chests) + " chest(s)";
}

// ---- recycling ----
struct Stack {
    uint32_t item = 0;
    uint32_t count = 0;
};
// A recipe as the game's RecipeInfo has it: inputs (item 0 = a category input, "any wood"), outputs, and two flags.
struct Recipe {
    uint32_t id = 0;
    std::vector<Stack> inputs;
    std::vector<Stack> outputs;
    bool upgrade = false;  // isUpgrade (+0xa0): upgrades gear
    bool cooking = false;  // isCookingRecipe (+0x9f)
};

enum class Skip { None, Gear, NoRecipe, Ambiguous, Upgrade, Cooking, Category, MultiOutput, TooFew, NothingBack };
inline const char* skip_text(Skip s) {
    switch (s) {
        case Skip::Gear: return "gear (salvage it at a workbench for runes)";
        case Skip::NoRecipe: return "no recipe makes it";
        case Skip::Ambiguous: return "several recipes with different materials make it";
        case Skip::Upgrade: return "an upgrade";
        case Skip::Cooking: return "cooked food";
        case Skip::Category: return "its recipe takes any of a kind (a category)";
        case Skip::MultiOutput: return "its recipe makes several things";
        case Skip::TooFew: return "fewer than its recipe makes at once";
        case Skip::NothingBack: return "too few to give anything back at this refund";
        default: return "";
    }
}

constexpr uint64_t kMaxMove = 65535;  // the most one take_items / put_items_ex carries

struct Recycle {
    Skip skip = Skip::None;
    uint32_t take = 0;          // how many of the stack are recycled (whole batches)
    std::vector<Stack> refund;  // what comes back (none of 0)
    uint32_t recipe = 0;
};

namespace detail {
inline bool same_materials(const Recipe& a, const Recipe& b) {
    auto key = [](const Recipe& r) {
        std::vector<std::pair<uint32_t, uint32_t>> v;
        for (const auto& s : r.inputs) v.push_back({s.item, s.count});
        std::sort(v.begin(), v.end());
        return std::make_pair(r.outputs.empty() ? 0u : r.outputs[0].count, v);
    };
    return key(a) == key(b);
}
}  // namespace detail

// What recycling `stack` gives at `refund_pct` % (capped at 100): batches = count / the recipe's output count; each
// input comes back as floor(batches * input count * refund / 100). `recipes`: every recipe whose outputs hold the item.
inline Recycle recycle_plan(const Carried& stack, const std::vector<Recipe>& recipes, int refund_pct) {
    Recycle r;
    if (stack.gear) {
        r.skip = Skip::Gear;
        return r;
    }
    if (recipes.empty()) {
        r.skip = Skip::NoRecipe;
        return r;
    }
    std::vector<const Recipe*> plain;
    Skip excluded = Skip::None;
    auto note = [&](Skip s) {
        if (excluded == Skip::None || static_cast<int>(s) < static_cast<int>(excluded)) excluded = s;
    };
    bool single = false;
    for (const auto& rec : recipes) {
        if (rec.outputs.size() != 1 || rec.outputs[0].item != stack.item || rec.outputs[0].count == 0) continue;
        single = true;
        bool category = false;
        for (const auto& in : rec.inputs) category = category || in.item == 0;
        if (rec.upgrade) note(Skip::Upgrade);
        else if (rec.cooking) note(Skip::Cooking);
        else if (category) note(Skip::Category);
        else plain.push_back(&rec);
    }
    if (!single) {
        r.skip = Skip::MultiOutput;
        return r;
    }
    if (plain.empty()) {
        r.skip = excluded;
        return r;
    }
    for (const Recipe* p : plain)
        if (!detail::same_materials(*p, *plain[0])) {
            r.skip = Skip::Ambiguous;
            return r;
        }
    const Recipe& rec = *plain[0];
    r.recipe = rec.id;
    const uint32_t per = rec.outputs[0].count;
    const uint64_t pct = static_cast<uint64_t>(std::clamp(refund_pct, 0, 100));
    // one take and each put carry at most 65535: fewer batches at once (the rest waits for the next pass)
    uint64_t batches = (std::min<uint64_t>)(stack.count, kMaxMove) / per;
    for (const auto& in : rec.inputs)
        if (in.count && pct) batches = (std::min<uint64_t>)(batches, kMaxMove * 100 / (static_cast<uint64_t>(in.count) * pct));
    if (!batches) {
        r.skip = stack.count >= per ? Skip::NothingBack : Skip::TooFew;
        return r;
    }
    for (const auto& in : rec.inputs) {
        const uint64_t back = batches * in.count * pct / 100;
        if (back) r.refund.push_back({in.item, static_cast<uint32_t>(back)});
    }
    if (r.refund.empty()) {
        r.skip = Skip::NothingBack;
        return r;
    }
    r.take = static_cast<uint32_t>(batches * per);
    return r;
}

// Inventory slots a refund needs, in stacks of each item's maximum (an unknown maximum of 0 counts each item alone).
inline uint32_t slots_needed(const std::vector<Stack>& refund, const std::function<uint32_t(uint32_t)>& max_stack) {
    uint32_t n = 0;
    for (const auto& s : refund) {
        const uint32_t m = (std::max)(1u, max_stack(s.item));
        n += (s.count + m - 1) / m;
    }
    return n;
}

inline std::string recycle_note(const std::string& item, uint32_t count,
                                const std::vector<std::pair<std::string, uint32_t>>& back) {
    std::string s = "Recycler: " + std::to_string(count) + " x " + item + " into ";
    for (size_t i = 0; i < back.size(); ++i) s += (i ? ", " : "") + std::to_string(back[i].second) + " x " + back[i].first;
    return s;
}

// ---- the key: a tap quick stacks (on release), holding it opens the wheel ----
class TapHold {
public:
    enum Event { None, Tap, Hold };
    explicit TapHold(uint64_t hold_ms = 350) : hold_ms_(hold_ms) {}
    Event update(bool down, uint64_t now_ms) {
        switch (state_) {
            case State::Up:
                if (down) {
                    state_ = State::Down;
                    since_ = now_ms;
                }
                return None;
            case State::Down:
                if (!down) {
                    state_ = State::Up;
                    return Tap;
                }
                if (now_ms - since_ >= hold_ms_) {
                    state_ = State::Held;
                    return Hold;
                }
                return None;
            case State::Held:
                if (!down) state_ = State::Up;  // the release after the wheel: no tap
                return None;
        }
        return None;
    }

private:
    enum class State { Up, Down, Held };
    State state_ = State::Up;
    uint64_t since_ = 0;
    uint64_t hold_ms_;
};

}  // namespace quickstack
