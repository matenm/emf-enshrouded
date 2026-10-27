// quick_stack (Quick Stack and Chest Machines): an EMF mod. Host or dedicated server; players who join need nothing.
//   - Quick stack: tap the key (default L): what you carry goes into the chests within `radius` metres that already
//     hold the same item (the nearest such chest), through the game's own inventory transactions. Gear with its own
//     data (level, perks) never moves, the action bar stays (keep_action_bar), and so do arrows, food and named items
//     when the filters say so; anything a chest has no room for comes back. /qs (or /quick_stack) does the same for
//     the player who types it, also from the game's chat (API v17).
//   - Chest Machines: hold the key while looking at a chest for the chest wheel:
//       sorting chest: every `sort_seconds` each plain stack in it goes to the nearest chest within `sort_radius`
//         that already holds that item (guests just drop things in through the game's own chest window);
//       recycler (off until `recycler` is on): crafted items go back into `refund` % of their recipe's materials,
//         rounded down (never more than they cost); gear, cooking, upgrades and ambiguous items stay;
//       overflow chest (with `overflow`): takes from a sorting chest what no chest holds.
//     Marks are kept per world by the chest's place (emf\config\quick_stack\<slot>.txt); a marked chest that is gone
//     loses its mark once a player comes near its place. qs.mark / qs.list (admins), /qs.chests (every player).
// The rules (tested without the game) are quickstack.h; the game side is qs_glue.h.
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "emf/aim.hpp"
#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "emf/inventory.hpp"
#include "emf/keys.hpp"
#include "emf/query.hpp"
#include "emf/storage.hpp"
#include "qs_glue.h"
#include "qs_moves.h"
#include "quickstack.h"

namespace gen = emf::gen;
namespace qs = quickstack;

namespace {
constexpr const char* kMod = "quick_stack";
constexpr double kPickRange = 8.0;      // metres: the chest wheel's chest is within this of the player ...
constexpr double kPickRay = 1.2;        // ... and this close to the crosshair's ray
constexpr double kChatMarkRange = 3.0;  // qs.mark from the chat: the nearest chest this close to the sender
constexpr double kGoneRadius = 40.0;    // a player this close to a mark's place and no chest there: it is gone
constexpr double kChestsRange = 50.0;   // /qs.chests lists marked chests this close
constexpr uint64_t kResolveMs = 5000;   // marks are matched to chests this often
constexpr size_t kMaxSortMoves = 16;    // moves per sorting pass, over every sorting chest (the item queue is small)

// ---- settings (the change callback may run on any world's thread) ----
struct Config {
    uint32_t key_vk = 'L';
    std::string key_name = "L";
    double radius = 10.0;
    bool keep_bar = true;
    qs::Filters filters;
    bool only_in_base = false;
    double base_radius = 40.0;
    uint64_t sort_ms = 10000;
    double sort_radius = 30.0;
    bool overflow = false;
    bool recycler = false;
    int refund = 50;
    uint64_t recycle_ms = 15000;
    bool recycle_to_sorter = true;
    std::string notes = "mine";
};
std::mutex g_config_lock;
Config g_config;
std::atomic<bool> g_switched_off{false};

// The mod's own switch, as the Mods tab saved it (emf\config\quick_stack.ini, [quick_stack] enabled).
bool switch_on() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring ini(exe);
    ini = ini.substr(0, ini.find_last_of(L"\\/")) + L"\\emf\\config\\quick_stack.ini";
    return GetPrivateProfileIntW(L"quick_stack", L"enabled", 1, ini.c_str()) != 0;
}

Config config() {
    std::lock_guard<std::mutex> l(g_config_lock);
    return g_config;
}
void read_settings() {
    Config c;
    const std::string key = emf::setting_text(kMod, "key", "L");
    if (const uint32_t vk = emf::keys::parse_key(std::wstring(key.begin(), key.end()), &c.key_name)) {
        c.key_vk = vk;
    } else {
        c.key_vk = 0;  // no key (cleared): the console command and /qs still work
        c.key_name = "(no key)";
    }
    c.radius = static_cast<double>(emf::setting_int(kMod, "radius", 10));
    c.keep_bar = emf::setting_bool(kMod, "keep_action_bar", true);
    c.filters.keep_ammo = emf::setting_bool(kMod, "keep_ammo", false);
    c.filters.keep_food = emf::setting_bool(kMod, "keep_food", false);
    c.filters.exclude = qs::parse_exclude(emf::setting_text(kMod, "exclude", ""));
    c.only_in_base = emf::setting_bool(kMod, "only_in_base", false);
    c.base_radius = static_cast<double>(emf::setting_int(kMod, "base_radius", 40));
    c.sort_ms = static_cast<uint64_t>(emf::setting_int(kMod, "sort_seconds", 10)) * 1000;
    c.sort_radius = static_cast<double>(emf::setting_int(kMod, "sort_radius", 30));
    c.overflow = emf::setting_bool(kMod, "overflow", false);
    c.recycler = emf::setting_bool(kMod, "recycler", false);
    c.refund = static_cast<int>(emf::setting_int(kMod, "refund", 50));
    c.recycle_ms = static_cast<uint64_t>(emf::setting_int(kMod, "recycle_seconds", 15)) * 1000;
    c.recycle_to_sorter = emf::setting_bool(kMod, "recycle_to_sorter", true);
    c.notes = emf::setting_text(kMod, "notes", "mine");
    std::lock_guard<std::mutex> l(g_config_lock);
    g_config = c;
}

// ---- server tick state (the server world's thread) ----
emf::WorldFile g_file(kMod);
qs::Marks g_marks;
uint32_t g_session = 0;
qs::TapHold g_key(350);
qs::FullBook g_full;
qs::MissBook g_miss;
qs::MarkBinder g_binder;  // which chest each mark found this session (another chest at its place drops the mark)
qs::Requests g_req;       // this mod's places in the core's item queue (it holds 64 and runs 8 a step)
std::vector<qs::Resolved> g_resolved;
uint64_t g_resolve_ms = 0, g_sort_ms = 0, g_recycle_ms = 0;
std::set<uint32_t> g_busy;  // sorting chests and recyclers whose last pass is still under way
std::set<std::tuple<uint32_t, uint32_t, int>> g_noted;  // (chest, item, reason) a recycler skipped, logged once
std::map<uint32_t, std::string> g_names;                // item id -> debug name
qs::StackSizes g_sizes;  // item id -> its stack size (Big Stacks changes them while the game runs: read again)
const void* g_recipes_from = nullptr;
std::map<uint32_t, std::vector<qs::Recipe>> g_recipes;  // item -> the recipes that make it

qs::Vec3 v3(const emf::aim::Vec3& p) { return {p.x, p.y, p.z}; }
std::optional<qs::Vec3> pos_of(emf::World w, uint32_t id) {
    const auto p = emf::position(w, id);
    if (!p) return std::nullopt;
    return v3(*p);
}

std::string item_name(emf::World w, uint32_t item) {
    auto it = g_names.find(item);
    if (it != g_names.end()) return it->second;
    EmfItemInfo info{};
    info.size = sizeof info;
    if (!emf::api()->item_info(w.raw(), item, &info) || !info.name[0]) {
        char b[24];
        snprintf(b, sizeof b, "item %08x", item);
        return b;  // not known yet: asked again next time
    }
    return g_names.emplace(item, info.name).first->second;
}
uint32_t max_stack(emf::World w, uint32_t item) {
    return g_sizes.get(item, GetTickCount64(), [w](uint32_t id) {
        EmfItemInfo info{};
        info.size = sizeof info;
        return emf::api()->item_info(w.raw(), id, &info) ? static_cast<uint32_t>(info.max_stack) : 0u;
    });
}

bool notes_all(const Config& c) { return c.notes == "all"; }
bool notes_mine(const Config& c) { return c.notes == "mine" || c.notes == "all"; }

// A new hosted session (another world, a reload): ids of the old one mean other things now.
void new_session() {
    g_session = emf::server_session();
    g_resolved.clear();
    g_busy.clear();
    g_full.clear();
    g_noted.clear();
    g_resolve_ms = 0;
    g_recipes_from = nullptr;
    g_recipes.clear();
    g_sizes.clear();  // another world, or Big Stacks set up again: the sizes are read again
    g_miss.reset(GetTickCount64());
    g_binder.reset();  // after a load ids change: the first chest at a mark's place is taken
    if (!g_file.ready()) g_marks.clear();
}

bool save_marks() { return g_file.save(g_marks.text()); }

// Matches every mark to the chest at its place (qs_glue.h resolve_marks); a mark whose chest is gone, or whose place
// holds another chest now, is dropped: logged, the host told, the marks saved.
void resolve(emf::World w, uint64_t now, bool force = false) {
    if (!force && now - g_resolve_ms < kResolveMs) return;
    g_resolve_ms = now;
    std::vector<qs::DroppedMark> dropped;
    g_resolved = qs::resolve_marks(w, g_marks, g_binder, g_miss, now, kGoneRadius, &dropped);
    for (const auto& d : dropped) {
        EMF_LOG(kMod, "the %s at (%.1f, %.1f, %.1f) lost its mark: %s", qs::mark_label(d.mark), d.pos.x, d.pos.y, d.pos.z,
                d.why.c_str());
        EMF_NOTIFY("Quick stack: the %s at (%.0f, %.0f, %.0f) lost its mark: %s", qs::mark_label(d.mark), d.pos.x, d.pos.y,
                   d.pos.z, d.why.c_str());
    }
    if (!dropped.empty()) save_marks();
}

// The chests within `radius` of `center` that may receive items: not `except`, not sorting chests or recyclers (they
// pass things on); overflow chests flagged when `overflow` is on; with the room each has for the `items` that may
// move, so nothing is sent only to come back.
std::vector<qs::Chest> chests_near(emf::World w, const qs::Vec3& center, double radius, uint32_t except, bool overflow,
                                   const std::set<uint32_t>& items) {
    std::vector<qs::Chest> out;
    for (const auto& n : emf::entities_near(w, {center.x, center.y, center.z}, radius,
                                            [&](uint32_t id) { return id != except && qs::is_chest(w, id); })) {
        const qs::Mark m = g_marks.at(v3(n.pos));
        if (m == qs::Mark::Sort || m == qs::Mark::Recycle) continue;
        qs::Chest c{n.id, n.distance, emf::inventory_contents(w, n.id), overflow && m == qs::Mark::Overflow};
        c.room_known = true;
        for (uint32_t item : items) {
            uint32_t biggest = 0;
            c.room[item] = qs::room_for(w, n.id, item, max_stack(w, item), &biggest);
            if (g_sizes.saw(item, biggest))  // a stack above the size known: it changed (Big Stacks); count again
                c.room[item] = qs::room_for(w, n.id, item, max_stack(w, item));
        }
        out.push_back(c);
    }
    return out;
}

std::set<uint32_t> items_of(const std::vector<qs::Carried>& stacks) {
    std::set<uint32_t> out;
    for (const auto& s : stacks)
        if (!s.gear && !s.kept && s.item) out.insert(s.item);
    return out;
}

bool near_base(emf::World w, const qs::Vec3& p, double radius) {
    bool found = false;
    w.for_each_entity([&](uint32_t id) {
        if (found || !w.get<gen::ecs::BaseIdComponent>(id)) return;
        if (const auto b = pos_of(w, id)) found = qs::dist(*b, p) <= radius;
    });
    return found;
}

// ---- quick stack ----
struct Tally {
    size_t pending = 0;
    uint32_t moved = 0;
    std::set<uint32_t> chests;
    uint32_t session = 0;  // the hosted session of the key press
    size_t skipped = 0;    // takes refused at once or that found nothing
    size_t left = 0;       // stacks left for the next press (at most kMaxQuickMoves a press)
    uint32_t player = 0;
    bool notify = false;   // the host's own: a note when done (a chat player got a reply at once)
};
std::map<uint32_t, std::weak_ptr<Tally>> g_running;  // per player: the quick stack still in flight
void done(const std::shared_ptr<Tally>& t) {
    if (--t->pending || t->session != emf::server_session()) return;
    const std::string s = qs::summary(t->moved, t->chests.size(), t->skipped) + qs::left_note(t->left);
    if (t->notify) emf::api()->notify("%s", s.c_str());
    else EMF_LOG(kMod, "player %u: %s", t->player, s.c_str());
}

// Quick stack for `player`. `chat`: a player in the game's chat (answered with replies, no note). At most
// kMaxQuickMoves stacks a press, and never more than the mod's room in the item queue: the rest at the next press.
void quick_stack(emf::World w, uint32_t player, bool chat) {
    const Config c = config();
    auto say = [&](const std::string& text) {
        if (chat) EMF_REPLY("%s", text.c_str());
        else if (notes_mine(c)) emf::api()->notify("%s", text.c_str());
    };
    const auto pos = pos_of(w, player);
    if (!player || !pos) {
        if (chat) EMF_REPLY("Quick stack: EMF could not tell where you are; try again in a moment");
        return;
    }
    if (c.only_in_base && !near_base(w, *pos, c.base_radius)) {
        say("Quick stack works only at a base (within " + std::to_string(static_cast<int>(c.base_radius)) +
            " m of a flame altar)");
        return;
    }
    if (const auto running = g_running[player].lock(); running && running->pending) {
        say("Quick stack: still putting things away");
        return;
    }
    const auto stacks = qs::carried(w, player, c.keep_bar, c.filters, [w](uint32_t item) { return item_name(w, item); });
    auto moves = qs::plan(stacks, chests_near(w, *pos, c.radius, player, false, items_of(stacks)));
    if (moves.empty()) {
        char b[160];
        snprintf(b, sizeof b, "Quick stack: nothing to put away (no chest within %.0f m holds what you carry, or they are full)",
                 c.radius);
        say(b);
        return;
    }
    auto tally = std::make_shared<Tally>();
    tally->left = qs::cap_moves(moves, g_req.free());
    if (moves.empty()) {
        say("Quick stack: busy putting other things away; press again in a moment");
        return;
    }
    tally->pending = moves.size();
    tally->session = emf::server_session();
    tally->player = player;
    tally->notify = !chat && notes_mine(c);
    g_running[player] = tally;
    if (chat) {
        std::set<uint32_t> into;
        for (const auto& m : moves) into.insert(m.chest);
        EMF_REPLY("Quick stack: %zu stack(s) on the way into %zu chest(s)%s", moves.size(), into.size(),
                  qs::left_note(tally->left).c_str());
    }
    for (const auto& m : moves) {
        const bool queued = qs::move_stack(w, player, m, tally->session, &g_req, kMod, [m, tally](emf::World, uint32_t added, uint32_t taken) {
            if (!taken) ++tally->skipped;
            tally->moved += added;
            if (added) tally->chests.insert(m.chest);
            done(tally);
        });
        if (!queued) {
            ++tally->skipped;
            done(tally);
        }
    }
}

// ---- sorting chests ----
struct PassTally {
    size_t pending = 0;
    size_t stacks = 0;
    std::set<uint32_t> chests;
    uint32_t chest = 0;
    uint32_t session = 0;
};
void pass_done(const std::shared_ptr<PassTally>& t) {
    if (--t->pending) return;
    if (t->session != emf::server_session()) return;
    g_busy.erase(t->chest);
    if (!t->stacks) return;
    const std::string s = qs::sort_note(t->stacks, t->chests.size());
    EMF_LOG(kMod, "%s (sorting chest %u)", s.c_str(), t->chest);
    if (notes_all(config())) emf::api()->notify("%s", s.c_str());
}

void sort_pass(emf::World w, uint64_t now, const Config& c) {
    size_t budget = (std::min)(kMaxSortMoves, static_cast<size_t>((std::max)(0, g_req.free())));
    for (const auto& r : g_resolved) {
        if (!budget) break;
        if (r.mark != qs::Mark::Sort || !r.id || g_busy.count(r.id)) continue;
        const auto stacks = qs::chest_stacks(w, r.id);
        if (stacks.empty()) continue;
        auto moves = qs::sort_plan(stacks, chests_near(w, r.pos, c.sort_radius, r.id, c.overflow, items_of(stacks)), g_full, now);
        if (moves.size() > budget) moves.resize(budget);  // the rest in the next pass
        budget -= moves.size();
        if (moves.empty()) continue;
        auto t = std::make_shared<PassTally>();
        t->pending = moves.size();
        t->chest = r.id;
        t->session = emf::server_session();
        g_busy.insert(r.id);
        for (const auto& m : moves) {
            const bool queued = qs::move_stack(w, r.id, m, t->session, &g_req, kMod, [m, t, now](emf::World, uint32_t added, uint32_t taken) {
                if (taken && !added && t->session == emf::server_session()) g_full.mark(m.chest, m.item, now);
                if (added) {
                    ++t->stacks;
                    t->chests.insert(m.chest);
                }
                pass_done(t);
            });
            if (!queued) pass_done(t);
        }
    }
}

// ---- recyclers ----
const std::map<uint32_t, std::vector<qs::Recipe>>& recipes(emf::World w) {
    const void* reg = w.singleton("g38_items::RecipeRegistry");
    if (reg != g_recipes_from || g_recipes.empty()) {
        g_recipes_from = reg;
        g_recipes = qs::recipes_by_output(static_cast<const uint8_t*>(reg), qs::guarded_copy);
        if (!g_recipes.empty()) EMF_LOG(kMod, "recipes read: %zu items have a recipe", g_recipes.size());
        // empty: the registry is not loaded yet; read again next time
    }
    return g_recipes;
}

void note_skip(emf::World w, uint32_t chest, uint32_t item, int reason, const std::string& why) {
    if (!g_noted.insert({chest, item, reason}).second) return;
    EMF_LOG(kMod, "recycler %u: %s stays (%s)", chest, item_name(w, item).c_str(), why.c_str());
}

// The candidates for a recycler's materials (quickstack.h recycle_target picks one): the sorting chests and the
// other chests within reach, with their free slots.
uint32_t recycle_target(emf::World w, const qs::Resolved& r, uint32_t first_material, uint32_t need, const Config& c) {
    std::vector<qs::Spot> sorters, chests;
    for (const auto& s : g_resolved)
        if (s.mark == qs::Mark::Sort && s.id && qs::dist(s.pos, r.pos) <= c.sort_radius)
            sorters.push_back({s.id, qs::dist(s.pos, r.pos), qs::free_slots(w, s.id), false});
    for (const auto& ch : chests_near(w, r.pos, c.sort_radius, r.id, false, {}))
        chests.push_back({ch.id, ch.distance, qs::free_slots(w, ch.id), ch.contents.count(first_material) > 0});
    return qs::recycle_target(sorters, chests, need, c.recycle_to_sorter);
}

void recycle_pass(emf::World w, const Config& c) {
    const auto& index = recipes(w);
    static const std::vector<qs::Recipe> none;
    for (const auto& r : g_resolved) {
        if (r.mark != qs::Mark::Recycle || !r.id || g_busy.count(r.id)) continue;
        for (const auto& st : qs::chest_stacks(w, r.id)) {
            auto it = index.find(st.item);
            const qs::Recycle plan = qs::recycle_plan(st, it == index.end() ? none : it->second, c.refund);
            if (plan.skip != qs::Skip::None) {
                note_skip(w, r.id, st.item, static_cast<int>(plan.skip), qs::skip_text(plan.skip));
                continue;
            }
            const uint32_t need = qs::slots_needed(plan.refund, [w](uint32_t item) { return max_stack(w, item); });
            const uint32_t target = recycle_target(w, r, plan.refund.empty() ? 0 : plan.refund[0].item, need, c);
            if (!target) {
                note_skip(w, r.id, st.item, 101, "no chest within " + std::to_string(static_cast<int>(c.sort_radius)) +
                                                     " m with room for its materials (put a chest or a sorting chest near it)");
                continue;
            }
            const uint32_t recycler = r.id, item = st.item, session = emf::server_session();
            const qs::Recycle p = plan;
            g_busy.insert(recycler);  // busy until every material put has answered
            const bool queued = qs::recycle_stack(w, recycler, target, item, st.slot, p, session, &g_req, kMod,
                                                  [recycler, target, item, p, session](emf::World w2, bool recycled) {
                if (session != emf::server_session()) return;  // an old world's: its ids are other entities now
                g_busy.erase(recycler);
                if (!recycled) return;
                std::vector<std::pair<std::string, uint32_t>> back;
                for (const auto& s : p.refund) back.push_back({item_name(w2, s.item), s.count});
                const std::string note = qs::recycle_note(item_name(w2, item), p.take, back);
                EMF_LOG(kMod, "%s (recycler %u, materials into %u)", note.c_str(), recycler, target);
                if (notes_all(config())) emf::api()->notify("%s", note.c_str());
            });
            if (!queued) g_busy.erase(recycler);  // no room in the item queue now: the next pass tries again
            break;  // one stack per recycler at a time
        }
    }
}

// ---- the key and the chest wheel ----
std::optional<std::pair<uint32_t, qs::Vec3>> chest_under_crosshair(emf::World w, uint32_t player) {
    const auto pp = emf::position(w, player);
    const auto ray = emf::aim::camera_ray(w, player);
    if (!pp || !ray) return std::nullopt;
    std::vector<uint32_t> ids;
    std::vector<emf::aim::Vec3> feet, points;
    for (const auto& n : emf::entities_near(w, *pp, kPickRange + 1.0, [&](uint32_t id) { return id != player && qs::is_chest(w, id); })) {
        ids.push_back(n.id);
        feet.push_back(n.pos);
        points.push_back({n.pos.x, n.pos.y + 0.4, n.pos.z});
    }
    const auto i = emf::aim::pick(*ray, *pp, points, {kPickRange, kPickRay, 0.25});
    if (!i) return std::nullopt;
    return std::make_pair(ids[*i], v3(feet[*i]));
}

std::string mark_chest(const qs::Vec3& pos, qs::Mark mark, const Config& c) {
    if (!g_file.ready()) return "Chest marks need a world this game hosts";
    const qs::Mark old = g_marks.set(pos, mark);
    g_resolve_ms = 0;  // matched again at the next step
    const std::string label = qs::mark_label(mark);
    const std::string a = label[0] == 'o' ? "an " : "a ";  // "an overflow chest"
    std::string text = old == mark ? "This chest already is " + a + label : "This chest is now " + a + label;
    if (!save_marks()) text += " (could not be saved: see the log)";
    if (mark == qs::Mark::Recycle && !c.recycler) text += "; recyclers are off in the settings (Recyclers)";
    if (mark == qs::Mark::Overflow && !c.overflow) text += "; overflow chests are off in the settings (Overflow chests)";
    EMF_LOG(kMod, "chest at (%.1f, %.1f, %.1f): %s -> %s", pos.x, pos.y, pos.z, qs::mark_name(old), qs::mark_name(mark));
    return text;
}

void open_chest_wheel(emf::World w, uint32_t chest, const qs::Vec3& pos, const Config& c) {
    const qs::Mark now = g_marks.at(pos);
    const std::string title = "Chest " + std::to_string(chest) + " (" + qs::mark_label(now) + ")";
    const std::vector<std::string> items = {
        "Quick stack now",
        now == qs::Mark::Sort ? "Sorting chest (it is one)" : "Make this a sorting chest (things dropped in are put away)",
        c.recycler ? (now == qs::Mark::Recycle ? "Recycler (it is one)" : "Make this a recycler (crafted items back into materials)")
                   : "Make this a recycler (recyclers are off in the settings)",
        c.overflow ? (now == qs::Mark::Overflow ? "Overflow chest (it is one)" : "Make this an overflow chest")
                   : "Make this an overflow chest (off in the settings)",
        "Make this a normal chest",
    };
    const bool ok = emf::wheel(title.c_str(), items, c.key_vk, EMF_WORLD_SERVER, [pos](emf::World w2, int choice) {
        const Config c2 = config();
        const qs::Mark marks[] = {qs::Mark::None, qs::Mark::Sort, qs::Mark::Recycle, qs::Mark::Overflow, qs::Mark::None};
        if (choice == 0) {
            quick_stack(w2, emf::local_player(), false);
        } else if (choice >= 1 && choice <= 4) {
            const std::string text = mark_chest(pos, marks[choice], c2);
            if (notes_mine(c2)) emf::api()->notify("%s", text.c_str());
        }
    });
    if (!ok) quick_stack(w, emf::local_player(), false);  // the wheel is busy (the console is open): quick stack
}

void on_key(emf::World w, const Config& c, uint64_t now) {
    const uint32_t player = emf::local_player();
    if (!player || !c.key_vk) return;
    switch (g_key.update(emf::keys::down(c.key_vk), now)) {
        case qs::TapHold::Tap: quick_stack(w, player, false); break;
        case qs::TapHold::Hold:
            if (const auto chest = chest_under_crosshair(w, player)) open_chest_wheel(w, chest->first, chest->second, c);
            else quick_stack(w, player, false);
            break;
        default: break;
    }
}

void on_tick(emf::World w) {
    if (!w.is_server()) return;
    if (emf::server_session() != g_session) new_session();
    g_file.poll([](const std::string& text) {
        g_marks.parse(text);
        g_resolve_ms = 0;
        g_miss.reset(GetTickCount64());
        if (!g_marks.all().empty())
            EMF_LOG(kMod, "this world (slot %04x): %zu marked chest(s)", g_file.slot(), g_marks.all().size());
    });
    if (g_switched_off) return;  // switched off in the Mods tab: nothing more until the next start
    const Config c = config();
    const uint64_t now = GetTickCount64();
    on_key(w, c, now);
    if (g_marks.all().empty()) return;
    resolve(w, now);
    if (now - g_sort_ms >= c.sort_ms) {
        g_sort_ms = now;
        sort_pass(w, now, c);
    }
    if (c.recycler && now - g_recycle_ms >= c.recycle_ms) {
        g_recycle_ms = now;
        recycle_pass(w, c);
    }
}

// ---- commands ----
// The player a command means: a chat sender's entity, else the host.
uint32_t sender_player(bool* chat) {
    EmfCommandSender s{};
    s.size = sizeof s;
    const bool from_chat = emf::command_sender(&s) && s.from_chat;
    if (chat) *chat = from_chat;
    return from_chat ? s.entity : emf::local_player();
}

void cmd_stack(emf::World w, const std::vector<std::string>&) {
    if (g_switched_off) {
        EMF_REPLY("Quick stack is switched off");
        return;
    }
    if (emf::server_session() != g_session) new_session();
    bool chat = false;
    const uint32_t player = sender_player(&chat);
    if (!player) {
        EMF_REPLY("Quick stack: EMF could not tell which player you are; try again in a moment");
        return;
    }
    quick_stack(w, player, chat);
}

void cmd_chests(emf::World w, const std::vector<std::string>&) {
    if (emf::server_session() != g_session) new_session();
    const auto pos = pos_of(w, sender_player(nullptr));
    if (!pos) {
        EMF_REPLY("EMF could not tell where you are; try again in a moment");
        return;
    }
    resolve(w, GetTickCount64(), true);
    std::vector<std::pair<double, qs::Resolved>> close_by;
    for (const auto& r : g_resolved) {
        const double d = qs::dist(r.pos, *pos);
        if (d <= kChestsRange) close_by.push_back({d, r});
    }
    std::sort(close_by.begin(), close_by.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (close_by.empty()) {
        EMF_REPLY("No sorting chests, recyclers or overflow chests within %.0f m", kChestsRange);
        return;
    }
    const Config c = config();
    for (size_t i = 0; i < close_by.size() && i < 8; ++i) {
        const qs::Resolved& r = close_by[i].second;
        const bool off = (r.mark == qs::Mark::Recycle && !c.recycler) || (r.mark == qs::Mark::Overflow && !c.overflow);
        EMF_REPLY("%s %.0f m away%s%s", qs::mark_label(r.mark), close_by[i].first, r.id ? "" : " (not loaded)",
                  off ? " (off in the settings)" : "");
    }
}

void cmd_mark(emf::World w, const std::vector<std::string>& a) {
    if (emf::server_session() != g_session) new_session();
    const qs::Mark mark = a.size() >= 2 ? qs::mark_of(a[1]) : qs::Mark::None;
    if (a.size() < 2 || a.size() > 3 || (mark == qs::Mark::None && qs::lower(a[1]) != "none")) {
        EMF_REPLY("usage: qs.mark sort|recycle|overflow|none [chest entity] (without one: the chest you look at; from the "
                  "chat: the nearest within 3 m)");
        return;
    }
    if (a.size() == 3) {  // a chest by its entity id (near lists them)
        const uint32_t id = static_cast<uint32_t>(std::strtoul(a[2].c_str(), nullptr, 10));
        const auto at = id ? pos_of(w, id) : std::nullopt;
        if (!at || !qs::is_chest(w, id)) {
            EMF_REPLY("%s is not a chest of this world", a[2].c_str());
            return;
        }
        EMF_REPLY("%s", mark_chest(*at, mark, config()).c_str());
        return;
    }
    bool chat = false;
    const uint32_t player = sender_player(&chat);
    const auto pos = pos_of(w, player);
    if (!pos) {
        EMF_REPLY("EMF could not tell where you are");
        return;
    }
    std::optional<qs::Vec3> chest;
    if (!chat)
        if (const auto c = chest_under_crosshair(w, player)) chest = c->second;
    if (!chest) {
        const auto list = emf::entities_near(w, {pos->x, pos->y, pos->z}, kChatMarkRange,
                                             [&](uint32_t id) { return id != player && qs::is_chest(w, id); });
        if (!list.empty()) chest = v3(list[0].pos);
    }
    if (!chest) {
        EMF_REPLY("No chest under the crosshair or within %.0f m of you", kChatMarkRange);
        return;
    }
    EMF_REPLY("%s", mark_chest(*chest, mark, config()).c_str());
}

void cmd_list(emf::World w, const std::vector<std::string>&) {
    if (emf::server_session() != g_session) new_session();
    resolve(w, GetTickCount64(), true);
    if (g_resolved.empty()) {
        EMF_REPLY("No marked chests in this world (hold %s at a chest, or qs.mark)", config().key_name.c_str());
        return;
    }
    for (const auto& r : g_resolved) {
        const std::string where = r.id ? "chest " + std::to_string(r.id) : std::string("not loaded");
        EMF_REPLY("%s at (%.1f, %.1f, %.1f): %s", qs::mark_label(r.mark), r.pos.x, r.pos.y, r.pos.z, where.c_str());
    }
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;
    if (!emf::has_v17()) {
        EMF_LOG(kMod, "needs EMF API v17 or newer");
        return false;
    }
    read_settings();
    emf::on_setting_changed(kMod, [](emf::World, const std::string& key) {
        if (key == "enabled") g_switched_off = !switch_on();
        read_settings();
    });
    g_file.attach();
    emf::on_tick(on_tick);
    const auto command = [](const char* name, const char* help, void (*f)(emf::World, const std::vector<std::string>&),
                            uint32_t flags) {
        if (!emf::add_command(name, help, f, flags)) EMF_LOG(kMod, "the command %s could not be added (see the core log)", name);
    };
    command("quick_stack", ": put what you carry into the nearby chests that hold it", cmd_stack, EMF_COMMAND_PLAYERS);
    command("qs", ": put what you carry into the nearby chests that hold it", cmd_stack, EMF_COMMAND_PLAYERS);
    command("qs.chests", ": the sorting chests, recyclers and overflow chests near you", cmd_chests, EMF_COMMAND_PLAYERS);
    command("qs.mark", "sort|recycle|overflow|none [chest]: mark the chest you look at, or that entity (host; chat: the nearest within 3 m)",
            cmd_mark, 0);
    command("qs.list", ": every marked chest of this world (host)", cmd_list, 0);
    const Config c = config();
    EMF_LOG(kMod, "ready: %s quick stacks into chests within %.0f m%s; hold it at a chest for the chest wheel; recyclers %s",
            c.key_name.c_str(), c.radius, c.keep_bar ? " (the action bar stays)" : "", c.recycler ? "on" : "off");
    return true;
}
