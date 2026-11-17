// loot_magnet (Loot Magnet): an EMF mod. On the host or a dedicated server, for every player whose magnet is on, the
// corpses of creatures killed near them in the last seconds are looted into them: the game's own loot event
// {guest = the player, host = the corpse, lootAll, bypassInteraction} (seen in game: the corpse's stacks moved into the
// player, a PickupEvent each). Players who joined need no EMF: it runs where the world runs. Each player switches their
// own magnet with /loot.toggle (a command for every player, API v17); the host's default and key are settings. Never: a
// corpse twice, a player's or an ally's body, anything owned or tame, anything inside a base. Most creatures leave their
// loot in a loot container where they died (the glowing loot spot) and fade away: that container is looted then. The
// rules (tested without the game) are magnet_logic.h.
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "emf/inventory.hpp"
#include "emf/keys.hpp"
#include "emf/query.hpp"
#include "emf/storage.hpp"
#include "magnet_glue.h"
#include "magnet_logic.h"
#include "sweep_glue.h"
#include "sweep_logic.h"

namespace gen = emf::gen::ecs;

namespace {
constexpr const char* kMod = "loot_magnet";
constexpr double kFixed = 4294967296.0;

// ---- settings (the change callback may run on any world's thread) ----
struct Config {
    magnet::Rules rules;
    sweep::SweepRules sweep;  // 2.0: what else near a player comes to them (ground items, plants, chests, smashing)
    bool default_on = true;
    std::string key;         // the host's toggle key ("" = none)
    bool debug_log = true;   // one log line per death noted and per corpse decided
};
std::mutex g_config_lock;
Config g_config;
std::atomic<bool> g_switched_off{false};  // the mod's switch went off in the Mods tab (the DLL stays until restart)

// The mod's own switch, as the Mods tab saved it (emf\config\loot_magnet.ini; the API has no getter).
bool switch_on() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring ini(exe);
    ini = ini.substr(0, ini.find_last_of(L"\\/")) + L"\\emf\\config\\loot_magnet.ini";
    return GetPrivateProfileIntW(L"loot_magnet", L"enabled", 1, ini.c_str()) != 0;
}

Config config() {
    std::lock_guard<std::mutex> l(g_config_lock);
    return g_config;
}
// The host's toggle key alone (read every frame; the whole Config is copied only when the magnet works, twice a second).
std::string config_key() {
    std::lock_guard<std::mutex> l(g_config_lock);
    return g_config.key;
}
void read_settings() {
    Config c;
    c.rules.radius = emf::setting_float(kMod, "radius", 8.0);
    c.rules.delay_ms = static_cast<uint64_t>(emf::setting_float(kMod, "delay", 1.0) * 1000.0);
    // how long after the death it keeps looking (at least 8 s: the game adds the loot 1-3 s after the death), and at
    // least 2 s past the delay
    const double window = (std::max)(emf::setting_float(kMod, "window", 15.0), 8.0);
    c.rules.window_ms = (std::max)(static_cast<uint64_t>(window * 1000.0), c.rules.delay_ms + 2000);
    c.rules.materials_only = emf::setting_text(kMod, "categories", "materials") == "materials";
    c.rules.include_ammo = emf::setting_bool(kMod, "include_ammo", true);
    c.rules.base_radius = emf::setting_float(kMod, "base_radius", 30.0);
    c.default_on = emf::setting_bool(kMod, "default_on", true);
    c.key = emf::setting_text(kMod, "toggle_key", "");
    c.debug_log = emf::setting_bool(kMod, "debug_log", true);
    // the sweep's groups: a switch and a reach each (0 = the general reach)
    const auto group = [](const char* key, bool def) {
        sweep::Group g;
        g.on = emf::setting_bool(kMod, key, def);
        g.reach = emf::setting_float(kMod, (std::string(key) + "_reach").c_str(), 0.0);
        return g;
    };
    auto& gr = c.sweep.groups;
    gr[static_cast<size_t>(sweep::Kind::Ground)] = group("pick_ground", true);
    gr[static_cast<size_t>(sweep::Kind::Collectible)] = group("pick_collectibles", true);
    gr[static_cast<size_t>(sweep::Kind::Mushroom)] = group("pick_mushrooms", false);
    gr[static_cast<size_t>(sweep::Kind::Firefly)] = group("pick_fireflies", false);
    gr[static_cast<size_t>(sweep::Kind::Plant)] = group("pick_plants", false);
    gr[static_cast<size_t>(sweep::Kind::Chest)] = group("open_chests", false);
    gr[static_cast<size_t>(sweep::Kind::Smash)] = group("smash", false);
    gr[static_cast<size_t>(sweep::Kind::Hive)] = group("smash_hives", false);
    c.sweep.reach = c.rules.radius;
    c.sweep.base_radius = c.rules.base_radius;
    c.sweep.ignore = sweep::Ignore::parse(emf::setting_text(kMod, "ignore", ""));
    std::lock_guard<std::mutex> l(g_config_lock);
    g_config = c;
}

// ---- loot events (and a chest's accept) for the program (server tick -> a game worker thread) ----
using Request = sweep::Request;
std::mutex g_requests_lock;
std::vector<Request> g_requests;

void magnet_program(void* ctx) {
    std::vector<Request> mine;
    {
        std::lock_guard<std::mutex> l(g_requests_lock);
        mine.swap(g_requests);
    }
    const uint32_t session = emf::server_session();
    for (const auto& r : mine)
        if (r.session == session) sweep::send(ctx, r);
}

// ---- server tick state ----
magnet::Magnet g_magnet;
magnet::Toggles g_toggles;
emf::WorldFile g_file(kMod);
// The chests the magnet emptied, per world (emf\config\loot_magnet_chests\<slot>.txt): after a reload the game offers an
// emptied chest again, and an accept would open it for nothing.
emf::WorldFile g_chests("loot_magnet_chests");
std::vector<magnet::Vec3> g_bases;
uint64_t g_last_ms = 0, g_bases_ms = 0;
uint32_t g_session = 0;
std::map<uint32_t, std::string> g_item_names;  // item id -> debug name (the game's names do not change)
emf::keys::Edge g_key;
int g_looted = 0;  // corpses looted this session (for loot.status)
sweep::Sweeper g_sweeper;
sweep::BaseWatch g_base_watch;
uint64_t g_sweep_ms = 0, g_walk_said_ms = 0;
bool g_told_no_smash = false;  // the log said once that this core cannot hit (smashing needs API v18)
std::map<sweep::Kind, int> g_swept;  // what the sweep collected this session, by group (for loot.status)
bool g_told_gear = false;   // the host heard "gear stays" this session
bool g_told_no_time = false;  // the log said once that the game gave a death no time

std::optional<magnet::Vec3> pos_of(emf::World w, uint32_t id) {
    auto* t = w.get<gen::CurrentTransform>(id);
    if (!t) return std::nullopt;
    return magnet::Vec3{t->transform.position.x / kFixed, t->transform.position.y / kFixed,
                        t->transform.position.z / kFixed};
}

const std::string& item_name(emf::World w, uint32_t item) {
    auto it = g_item_names.find(item);
    if (it != g_item_names.end()) return it->second;
    EmfItemInfo info{};
    info.size = sizeof info;
    std::string name = emf::api()->item_info(w.raw(), item, &info) ? info.name : "";
    if (name.empty()) {  // not known yet: the corpse waits (materials only), and it is asked again next time
        static const std::string unknown;
        return unknown;
    }
    return g_item_names.emplace(item, name).first->second;
}

magnet::Body body_of(emf::World w, uint32_t id) {
    return magnet::read_body(w, id, [w](uint32_t item) { return item_name(w, item); });
}

// How far from a death the magnet looks for loot containers: their reach, plus how far the body may slide before it fades.
double scan_radius(const magnet::Rules& r) { return r.container_reach + r.moved_max; }

double nearest_base(const magnet::Vec3& p) {
    double best = -1;
    for (const auto& b : g_bases) {
        const double d = magnet::horizontal_distance(p, b);
        if (best < 0 || d < best) best = d;
    }
    return best;
}

// The decision log: what became of each corpse the magnet watched (looted lines always; the rest with debug_log).
void log_decision(const magnet::Decision& d, const Config& c) {
    if (d.why == magnet::Why::Ok) {
        if (d.container)
            EMF_LOG(kMod, "corpse %u looted into player %u from the loot it left where it died (loot container %u, %.1f m)",
                    d.corpse, d.player, d.container, d.distance);
        else
            EMF_LOG(kMod, "corpse %u looted into player %u (%.1f m; interaction offer %s)", d.corpse, d.player, d.distance,
                    d.offer ? "yes" : "no");
        return;
    }
    if (!c.debug_log) return;
    if (d.why == magnet::Why::TooLate) {
        if (d.waited == magnet::Why::NoLoot || d.waited == magnet::Why::Gone || d.waited == magnet::Why::Moved ||
            d.waited == magnet::Why::Reused)
            EMF_LOG(kMod, "corpse %u left: no loot appeared within %.0f s of its death (none on it, and no loot container "
                          "within %.0f m of where it died)", d.corpse, c.rules.window_ms / 1000.0, c.rules.container_reach);
        else if (d.waited == magnet::Why::TooFar && d.distance >= 0)
            EMF_LOG(kMod, "corpse %u left: its time ran out while %s (the nearest player with a magnet was %.1f m away, reach %.0f m)",
                    d.corpse, magnet::why_text(d.waited), d.distance, c.rules.radius);
        else
            EMF_LOG(kMod, "corpse %u left: its time ran out while waiting (%s)", d.corpse, magnet::why_text(d.waited));
        return;
    }
    if (d.container)
        EMF_LOG(kMod, "corpse %u skipped: the loot it left (loot container %u) %s", d.corpse, d.container,
                magnet::why_text(d.why));
    else
        EMF_LOG(kMod, "corpse %u skipped: %s", d.corpse, magnet::why_text(d.why));
}

void refresh_bases(emf::World w) {
    g_bases.clear();
    w.for_each_entity([&](uint32_t id) {
        if (!w.get<gen::BaseIdComponent>(id)) return;
        if (const auto p = pos_of(w, id)) g_bases.push_back(*p);
    });
}

bool magnet_on(emf::World w, uint32_t player, const Config& c) {
    const uint64_t account = emf::player_account(w, player);
    return account ? g_toggles.on(account, c.default_on) : c.default_on;
}

// Switches `player`'s magnet (to `want`, or the other way); false when their account is not known.
bool toggle(emf::World w, uint32_t player, int want, bool* now_on) {
    const uint64_t account = emf::player_account(w, player);
    if (!account) return false;
    const bool on = want < 0 ? !g_toggles.on(account, config().default_on) : want != 0;
    g_toggles.set(account, on);
    if (!g_file.save(g_toggles.text())) EMF_LOG(kMod, "the switch could not be saved yet (no world file); it lasts this session");
    *now_on = on;
    return true;
}

// 2.0: what else lies near the players whose magnet is on (ground items, collectibles, mushrooms, fireflies, plants,
// chests, barrels to smash), once a second: the loot event or a chest's accept for the program, a hit for a barrel.
void sweep_tick(emf::World w, const Config& c, const std::vector<magnet::Looter>& looters, uint32_t session, uint64_t now) {
    double reach = 0;
    for (size_t k = 1; k < sweep::kKinds; ++k)
        if (c.sweep.groups[k].on) reach = (std::max)(reach, c.sweep.reach_of(static_cast<sweep::Kind>(k)));
    std::vector<sweep::Player> players;
    std::vector<magnet::Vec3> places;
    for (const auto& l : looters) {
        players.push_back({l.id, l.pos, l.on, l.on && sweep::has_room(w, l.id)});
        if (l.on) places.push_back(l.pos);
    }
    if (reach <= 0 || places.empty()) return;
    // a player who arrived or moved far (a fast travel, a join): their base may have come into the world with them
    if (g_base_watch.moved(players)) refresh_bases(w);
    sweep::Want want;
    want.smash = c.sweep.group(sweep::Kind::Smash).on;
    want.chests = c.sweep.group(sweep::Kind::Chest).on;
    want.hives = c.sweep.group(sweep::Kind::Hive).on;
    if ((want.smash || want.hives) && !emf::has_v18()) {
        want.smash = want.hives = false;
        if (!g_told_no_smash) {
            g_told_no_smash = true;
            EMF_LOG(kMod, "sweep: smashing needs EMF API v18 (hits); this core is older, so barrels and hives are left alone");
        }
    }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    const auto things = sweep::things_near(w, places, reach, [w](uint32_t item) { return item_name(w, item); }, want);
    QueryPerformanceCounter(&t1);
    const double walk_ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
    if (c.debug_log && now - g_walk_said_ms >= 60000) {
        g_walk_said_ms = now;
        EMF_LOG(kMod, "sweep: %zu things near the players read in %.1f ms (once a second)", things.size(), walk_ms);
    }
    size_t room = 0;
    {
        std::lock_guard<std::mutex> lock(g_requests_lock);
        room = g_requests.size() < 256 ? 256 - g_requests.size() : 0;  // never an action booked without a place
    }
    const auto acts = g_sweeper.tick(now, session, players, things, g_bases, c.sweep, room);
    std::map<sweep::Kind, int> looted;
    int accepted = 0, smashed = 0, hives = 0;
    for (const auto& a : acts) {
        if (a.act == sweep::Act::Smash) {
            if (emf::damage(w, a.player, a.entity, a.amount)) {
                ++(a.kind == sweep::Kind::Hive ? hives : smashed);
                if (a.first) ++g_swept[a.kind];
            }
            continue;
        }
        const Request r = sweep::request_of(a, session);
        {
            std::lock_guard<std::mutex> lock(g_requests_lock);
            g_requests.push_back(r);
        }
        if (r.accept)
            ++accepted;
        else
            ++looted[a.kind];
        if (!r.accept && a.first) ++g_swept[a.kind];  // what loot.status counts: one per thing, not per try
    }
    if (c.debug_log && (accepted || smashed || hives || !looted.empty())) {
        std::string what;
        for (const auto& [k, n] : looted) what += (what.empty() ? "" : ", ") + std::to_string(n) + " " + sweep::kind_name(k);
        if (accepted) what += (what.empty() ? "" : ", ") + std::to_string(accepted) + " chest(s) opened";
        if (smashed) what += (what.empty() ? "" : ", ") + std::to_string(smashed) + " smashed";
        if (hives) what += (what.empty() ? "" : ", ") + std::to_string(hives) + " bee hive(s) smashed";
        EMF_LOG(kMod, "sweep: %s", what.c_str());
    }
}

void on_tick(emf::World w) {
    if (!w.is_server() || g_switched_off) return;  // switched off in the Mods tab: no more looting from now on
    g_file.poll([](const std::string& text) { g_toggles.parse(text); });
    g_chests.poll([](const std::string& text) {
        g_sweeper.load_emptied(text);
        if (g_sweeper.emptied_count())
            EMF_LOG(kMod, "%zu chest(s) the magnet emptied in this world are remembered (loot.chests forget clears them)",
                    g_sweeper.emptied_count());
    });
    const uint32_t session = emf::server_session();
    if (session != g_session) {
        g_session = session;
        g_bases_ms = 0;
        g_looted = 0;
        g_swept.clear();
        g_told_gear = false;
        if (!g_file.ready()) g_toggles.clear();
        if (!g_chests.ready()) g_sweeper.load_emptied("");  // not the last world's list until this world's file is read
    }
    const uint64_t now = GetTickCount64();
    if (const std::string key = config_key(); !key.empty()) {  // the host's own key
        const uint32_t vk = emf::keys::parse_key(std::wstring(key.begin(), key.end()));
        if (g_key.pressed(emf::keys::down(vk)) && emf::local_player()) {
            bool on = false;
            if (toggle(w, emf::local_player(), -1, &on)) EMF_NOTIFY("Loot Magnet %s", on ? "on" : "off");
        }
    }
    if (now - g_last_ms < 500) return;
    g_last_ms = now;
    const Config c = config();
    if (now - g_bases_ms >= 10000) {
        g_bases_ms = now;
        refresh_bases(w);
    }
    {
        std::lock_guard<std::mutex> lock(g_requests_lock);
        if (g_requests.size() >= 192) return;  // the program has not sent the last ones yet: decide nothing now
    }
    std::vector<magnet::Looter> looters;
    for (uint32_t id : emf::players(w))
        if (const auto p = pos_of(w, id)) looters.push_back({id, *p, magnet_on(w, id, c)});
    // the loot containers near the deaths it watches (one walk of the world, only while it watches a death)
    const auto containers = magnet::containers_near(w, g_magnet.watched_places(), scan_radius(c.rules),
                                                    [w](uint32_t item) { return item_name(w, item); });
    std::vector<magnet::Decision> decisions;
    const auto loots = g_magnet.tick(now, session, looters, [w](uint32_t id) { return body_of(w, id); }, g_bases, c.rules,
                                     &decisions, containers);
    for (const auto& d : decisions) log_decision(d, c);
    if (c.debug_log)  // the loot spots near a death that were not its loot, and why (each once)
        for (const auto& k : g_magnet.take_skips()) {
            if (k.why == magnet::Skip::Before && k.seconds > 0)
                EMF_LOG(kMod, "corpse %u: loot container %u (%.1f m) not taken: %s, %.1f s before it", k.corpse, k.container,
                        k.distance, magnet::skip_text(k.why), k.seconds);
            else
                EMF_LOG(kMod, "corpse %u: loot container %u (%.1f m) not taken: %s", k.corpse, k.container, k.distance,
                        magnet::skip_text(k.why));
        }
    else
        g_magnet.take_skips();
    for (const auto& l : loots) {
        // its own Inventory: the game's loot event; plain stacks in linked inventories (which that event does not
        // reach): taken and put; gear there stays
        const magnet::LootPlan plan = magnet::plan_loot(w, l.corpse);
        if (plan.event) {
            {
                std::lock_guard<std::mutex> lock(g_requests_lock);
                if (g_requests.size() < 256) g_requests.push_back({l.player, l.corpse, session, false, 0});
            }
            g_magnet.on_sent(l.corpse, l.player, now, session);
        }
        for (const auto& st : plan.takes) {
            const uint32_t corpse = l.corpse, player = l.player;
            const bool started = magnet::move_linked(w, corpse, player, st, [corpse, player](uint32_t came, uint32_t dropped) {
                if (dropped)
                    EMF_LOG(kMod, "corpse %u: %u came into player %u, %u did not fit and lie at their feet", corpse, came,
                            player, dropped);
            });
            if (!started) EMF_LOG(kMod, "corpse %u: a stack in a linked inventory could not be taken (see the core log)", l.corpse);
        }
        if (plan.gear_left) {
            EMF_LOG(kMod, "corpse %u: %d gear stack(s) stay in it (taken out of a linked inventory, gear would lose its "
                          "gems and perks): open it by hand", l.corpse, plan.gear_left);
            if (l.player == emf::local_player() && !g_told_gear) {
                g_told_gear = true;
                EMF_NOTIFY("Loot Magnet: gear stays in the loot; open it by hand");
            }
        }
        ++g_looted;
    }
    // nothing came from the loot event within 2 s and its own Inventory still holds loot: send again (it may become
    // lootable a moment after it died), at most 5 times; then the player has no room (or the game refused), and the loot
    // stays there. Linked inventories are not the event's: their plain stacks were moved above, their gear stays.
    for (const auto& late : g_magnet.overdue(now)) {
        if (!magnet::holds_own(w, late.corpse)) continue;  // it came (the confirmation was another stack's)
        if (g_magnet.should_retry(late)) {
            if (c.debug_log)
                EMF_LOG(kMod, "corpse %u: nothing came from the loot event yet (try %d of %d): sent again", late.corpse,
                        late.tries, magnet::Magnet::kMaxTries);
            std::lock_guard<std::mutex> lock(g_requests_lock);
            g_requests.push_back({late.player, late.corpse, session, false, 0});
            g_magnet.on_sent(late.corpse, late.player, now, session, late.tries + 1);
            continue;
        }
        EMF_LOG(kMod, "corpse %u still holds loot after %d loot events: player %u has no room, or the game refused (the loot "
                      "stays there)", late.corpse, late.tries, late.player);
        if (g_magnet.should_tell_full(late.player, session) && late.player == emf::local_player())
            EMF_NOTIFY("Loot Magnet: the loot did not come (is your inventory full?); it stays where it was");
    }
    if (now - g_sweep_ms >= 1000) {
        g_sweep_ms = now;
        sweep_tick(w, c, looters, session, now);
        if (g_sweeper.emptied_dirty() && g_chests.save(g_sweeper.emptied_text())) g_sweeper.emptied_saved();
    }
}

// The player a command means: a chat sender's entity, else the host.
uint32_t sender_player() {
    EmfCommandSender s{};
    if (emf::command_sender(&s) && s.from_chat) return s.entity;
    return emf::local_player();
}

void cmd_toggle(emf::World w, const std::vector<std::string>& a) {
    int want = -1;
    if (a.size() > 1) {
        if (a[1] == "on") want = 1;
        else if (a[1] == "off") want = 0;
        else {
            EMF_REPLY("usage: loot.toggle [on|off]");
            return;
        }
    }
    const uint32_t player = sender_player();
    bool on = false;
    if (!player || !toggle(w, player, want, &on)) {
        EMF_REPLY("Loot Magnet: EMF could not tell which player you are; try again in a moment");
        return;
    }
    const Config c = config();
    EMF_REPLY("Loot Magnet is %s for you (corpses within %.0f m, %s; and what else lies near you: /loot.status)",
              on ? "on" : "off", c.rules.radius, c.rules.materials_only ? "materials only" : "everything");
}

// The host's (and the chat admins') look at the chests the magnet remembers as emptied in this world, and "forget": the
// list belongs to the save slot, so a world started anew in the same slot (a wiped dedicated server) would inherit it.
void cmd_chests(emf::World, const std::vector<std::string>& a) {
    if (a.size() > 1 && a[1] == "forget") {
        const size_t n = g_sweeper.emptied_count();
        g_sweeper.forget_emptied();
        const bool saved = g_chests.save(g_sweeper.emptied_text());
        if (saved) g_sweeper.emptied_saved();
        EMF_REPLY("Loot Magnet forgot the %zu chest(s) it emptied in this world%s", n,
                  saved ? "" : " (the world's file is not read yet: try again in a moment)");
        return;
    }
    if (a.size() > 1) {
        EMF_REPLY("usage: loot.chests [forget]");
        return;
    }
    EMF_REPLY("Loot Magnet remembers %zu chest(s) it emptied in this world and does not open them again; "
              "loot.chests forget clears the list (for a world started anew in the same save slot)",
              g_sweeper.emptied_count());
}

void cmd_status(emf::World w, const std::vector<std::string>&) {
    const Config c = config();
    const uint32_t player = sender_player();
    if (!player || !emf::player_account(w, player)) {
        EMF_REPLY("Loot Magnet: EMF could not tell which player you are; try again in a moment");
        return;
    }
    EMF_REPLY("Loot Magnet is %s for you: corpses up to %.0f s after the kill within %.0f m, from %.1f s after it, %s; never in a base",
              magnet_on(w, player, c) ? "on" : "off", c.rules.window_ms / 1000.0, c.rules.radius,
              c.rules.delay_ms / 1000.0, c.rules.materials_only ? (c.rules.include_ammo ? "materials and ammo only" : "materials only")
                                                                : "everything");
    std::string groups;
    for (size_t k = 1; k < sweep::kKinds; ++k) {
        const auto kind = static_cast<sweep::Kind>(k);
        if (!c.sweep.groups[k].on) continue;
        char part[64];
        std::snprintf(part, sizeof part, "%s%s (%.0f m)", groups.empty() ? "" : ", ", sweep::kind_name(kind), c.sweep.reach_of(kind));
        groups += part;
    }
    EMF_REPLY("Also: %s", groups.empty() ? "nothing else (every group is off)" : groups.c_str());
    std::string swept;
    for (const auto& [k, n] : g_swept) swept += (swept.empty() ? "" : ", ") + std::to_string(n) + " " + sweep::kind_name(k);
    EMF_REPLY("%d corpse(s) looted this session%s%s (sent; what did not fit stays); /loot.toggle switches yours", g_looted,
              swept.empty() ? "" : ", ", swept.c_str());
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
    if (!emf::add_program("loot_magnet", EMF_WORLD_SERVER, {}, {"LootInteractionEvent", "AcceptOfferedInteractionEvent"},
                          magnet_program)) {
        EMF_LOG(kMod, "could not add the loot_magnet program: off");
        return false;
    }
    emf::on_event<gen::EntityDiedEvent>(EMF_WORLD_SERVER, [](emf::World w, const gen::EntityDiedEvent& e) {
        if (g_switched_off) return;
        const uint32_t id = e.deadEntityId.id;
        const auto p = pos_of(w, id);
        if (!p) {
            if (config().debug_log) EMF_LOG(kMod, "death of %u: it has no position any more; not watched", id);
            return;
        }
        const magnet::Rules rules = config().rules;
        magnet::Death d{id, *p, GetTickCount64(), emf::server_session(), magnet::template_of(w, id)};
        d.guarded = magnet::guarded(body_of(w, id));  // a player's, an ally's...: nothing it leaves is taken
        // what appeared at or after the death is its loot, by the game's clock (a loot spot's SpawnTime); without any
        // time, the loot spots near it now are not its loot
        d.time = magnet::death_time(e.timeStamp.value, w.ecs_time_ns());
        if (!d.guarded && d.time <= 0) {
            d.before = magnet::container_ids_near(w, *p, scan_radius(rules));
            if (!g_told_no_time) {
                g_told_no_time = true;
                EMF_LOG(kMod, "death %u has no time and the world's clock is not known on this build: loot spots near "
                              "it now are taken as not its loot", id);
            }
        }
        if (config().debug_log)
            EMF_LOG(kMod, "death of %u noted at (%.1f, %.1f, %.1f), %.0f m from the nearest flame altar%s", id, p->x, p->y,
                    p->z, nearest_base(*p), d.guarded ? " (a player's, an ally's, owned or tame: nothing is taken)" : "");
        g_magnet.on_death(std::move(d));
    });
    emf::on_event<gen::PickupEvent>(EMF_WORLD_SERVER, [](emf::World, const gen::PickupEvent& e) {
        g_magnet.on_pickup(e.pickedUpByEntityId.id, e.pickedUpFromEntityId.id);
    });
    g_file.attach();
    g_chests.attach();
    if (!emf::add_command("loot.toggle", "[on|off]: your Loot Magnet (the loot of kills and what lies near you come to you)",
                          cmd_toggle, EMF_COMMAND_PLAYERS))
        EMF_LOG(kMod, "the command loot.toggle could not be added (see the core log)");
    if (!emf::add_command("loot.status", ": is your Loot Magnet on, and its rules", cmd_status, EMF_COMMAND_PLAYERS))
        EMF_LOG(kMod, "the command loot.status could not be added (see the core log)");
    if (!emf::add_command("loot.chests", "[forget]: the chests Loot Magnet emptied in this world (host); forget clears them",
                          cmd_chests, 0))
        EMF_LOG(kMod, "the command loot.chests could not be added (see the core log)");
    emf::on_tick(on_tick);
    const Config c = config();
    std::string groups;
    for (size_t k = 1; k < sweep::kKinds; ++k)
        if (c.sweep.groups[k].on) groups += std::string(groups.empty() ? "" : ", ") + sweep::kind_name(static_cast<sweep::Kind>(k));
    EMF_LOG(kMod, "ready: corpses within %.0f m come to players whose magnet is on (default %s), and %s; /loot.toggle",
            c.rules.radius, c.default_on ? "on" : "off", groups.empty() ? "nothing else" : groups.c_str());
    return true;
}
