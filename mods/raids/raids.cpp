// raids (Night Raids): an EMF mod. At nightfall, by chance, waves of enemies gather 40-60 m outside a base where
// players are and march on its flame altar (the game's own guard order, StartGuardingEvent), at the players' level. A
// warning first, a HUD line with the raiders left, a reward when the raid is repelled; survivors leave at dawn.
// Off in every world until switched on there (raids.world on, or the "Raids in the world you are in" setting); the
// switch is kept per world in emf\config\raids\<slot>.txt. Runs on the host or the dedicated server: players who joined
// need nothing. The raid's logic (tested without the game) is raid_logic.h; this file feeds it and carries out its
// actions. Spawned raiders are not saved by the game: a reload ends the raid.
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "emf/query.hpp"
#include "emf/storage.hpp"
#include "raid_logic.h"
#include "raids_glue.h"

namespace gen = emf::gen::ecs;

namespace {
constexpr const char* kMod = "raids";
constexpr const char* kHudLine = "raids.status";
constexpr double kFixed = 4294967296.0;
constexpr uint64_t kGuardEveryMs = 3000;
constexpr double kThreatRange = 25.0;  // a raider this close to a living player is told to fight that player

// ---- settings (written by the change callback, any world's thread; read by the server tick) ----
struct Config {
    raids::Settings s;
    bool custom = false;
    std::vector<raids::Template> custom_pool;
    raids::RewardKind reward = raids::RewardKind::Materials;
    uint32_t corner = EMF_HUD_TOP_RIGHT;
    uint32_t rgba = 0;
};
std::mutex g_config_lock;
Config g_config;
std::atomic<bool> g_switched_off{false};  // the mod's switch went off in the Mods tab (the DLL stays until restart)
std::atomic<bool> g_redraw{false};        // the HUD's corner or colour changed
std::atomic<bool> g_hud_visible{false};   // the raid line is on screen (the client tick clears it when no hosted world runs)
std::atomic<uint64_t> g_server_seen{0};   // GetTickCount64 of the last server world step

// The mod's own switch, as the Mods tab saved it (emf\config\raids.ini, [raids] enabled; the API has no getter).
bool switch_on() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring ini(exe);
    ini = ini.substr(0, ini.find_last_of(L"\\/")) + L"\\emf\\config\\raids.ini";
    return GetPrivateProfileIntW(L"raids", L"enabled", 1, ini.c_str()) != 0;
}

Config config() {
    std::lock_guard<std::mutex> l(g_config_lock);
    return g_config;
}

void read_settings() {
    Config c;
    c.s.chance = static_cast<int>(emf::setting_int(kMod, "chance", 35));
    c.s.start_hour = emf::setting_float(kMod, "start_hour", 20.0);
    c.s.warning_s = static_cast<int>(emf::setting_int(kMod, "warning_seconds", 60));
    c.s.cooldown_nights = static_cast<int>(emf::setting_int(kMod, "cooldown_nights", 0));
    c.s.waves = static_cast<int>(emf::setting_int(kMod, "waves", 2));
    c.s.per_wave = static_cast<int>(emf::setting_int(kMod, "per_wave", 4));
    c.s.wave_gap_s = static_cast<int>(emf::setting_int(kMod, "wave_gap_seconds", 60));
    c.s.level_offset = raids::difficulty_offset(emf::setting_text(kMod, "difficulty", "normal"));
    c.s.boss = emf::setting_bool(kMod, "boss", false);
    c.s.bases = raids::base_pick(emf::setting_text(kMod, "bases", "busiest"));
    c.s.base_radius = emf::setting_float(kMod, "base_radius", 30.0);
    c.reward = raids::reward_kind(emf::setting_text(kMod, "reward", "materials"));
    c.custom = emf::setting_text(kMod, "templates", "verified") == "custom";
    std::vector<std::string> bad;
    c.custom_pool = raids::parse_custom_templates(emf::setting_text(kMod, "custom_templates", ""), &bad);
    for (const auto& b : bad) EMF_LOG(kMod, "custom_templates: '%s' is not a template GUID: skipped", b.c_str());
    if (c.custom && c.custom_pool.empty())
        EMF_LOG(kMod, "templates = custom, but custom_templates holds no GUID: the tested Vukah come instead");
    const std::string corner = emf::setting_text(kMod, "corner", "top_right");
    c.corner = corner == "bottom_right" ? EMF_HUD_BOTTOM_RIGHT : corner == "bottom_left" ? EMF_HUD_BOTTOM_LEFT
                                                                                          : EMF_HUD_TOP_RIGHT;
    c.rgba = emf::setting_rgba(kMod, "color", 0);
    {
        std::lock_guard<std::mutex> l(g_config_lock);
        g_config = c;
    }
    g_redraw = true;
}

// ---- requests for the raids program (server tick -> the program on a game worker thread) ----
struct Order {
    uint32_t who, whom, session;
};
std::mutex g_orders_lock;
std::vector<Order> g_guards, g_threats;

void order(std::vector<Order>& list, uint32_t who, uint32_t whom) {
    std::lock_guard<std::mutex> l(g_orders_lock);
    if (list.size() < 256) list.push_back({who, whom, emf::server_session()});
}

void raids_program(void* ctx) {
    std::vector<Order> guards, threats;
    {
        std::lock_guard<std::mutex> l(g_orders_lock);
        guards.swap(g_guards);
        threats.swap(g_threats);
    }
    const uint32_t session = emf::server_session();
    for (const auto& g : guards)
        if (g.session == session)
            if (auto* e = emf::emit<gen::StartGuardingEvent>(ctx)) {
                e->guardingEntityId.id = g.who;
                e->guardedEntityId.id = g.whom;
            }
    for (const auto& t : threats)
        if (t.session == session)
            if (auto* e = emf::emit<gen::AddThreatTargetEvent>(ctx)) {
                e->attackerId.id = t.who;
                e->targetId.id = t.whom;
                e->threatValue = 1000.0f;
            }
}

// ---- server tick state ----
raids::Director g_director;
emf::WorldFile g_file(kMod);
bool g_world_on = false;  // this world's switch (from its file)
raids::Breadcrumbs g_crumbs;
std::vector<raids::Base> g_bases;
uint64_t g_bases_ms = 0, g_last_ms = 0, g_crumb_ms = 0, g_guard_ms = 0;
uint32_t g_session = 0;
std::string g_hud_shown;
bool g_stopped_for_switch = false;  // the mod was switched off: everything ended once
std::mt19937 g_rng(static_cast<uint32_t>(GetTickCount64()));
struct PendingLevel {
    uint32_t id, level;
    uint64_t until_ms;
};
std::vector<PendingLevel> g_levels;
std::map<uint32_t, raids::RaiderId> g_ids;  // who each raider is (template, level): ids are reused once one is gone
raids::StuckWatch g_stuck;

std::optional<raids::Vec3> pos_of(emf::World w, uint32_t id) {
    auto* t = w.get<gen::CurrentTransform>(id);
    if (!t) return std::nullopt;
    return raids::Vec3{t->transform.position.x / kFixed, t->transform.position.y / kFixed,
                       t->transform.position.z / kFixed};
}

uint32_t level_of(emf::World w, uint32_t id) {
    auto* l = w.get<gen::Level>(id);
    return l ? l->dataStorage[0] : 0;
}

// A player with health left (the replicated health, else the first Health slot: the current health in game tests).
bool alive(emf::World w, uint32_t id) {
    if (auto* n = w.get<gen::NetworkHealth>(id)) return n->health > 0;
    auto* h = w.get<gen::Health>(id);
    return h && h->dataStorage[0] > 0;
}

void refresh_bases(emf::World w) {
    g_bases.clear();
    w.for_each_entity([&](uint32_t id) {
        if (!w.get<gen::BaseIdComponent>(id)) return;
        if (const auto p = pos_of(w, id)) g_bases.push_back({id, *p});
    });
}

std::vector<raids::Player> read_players(emf::World w) {
    std::vector<raids::Player> out;
    for (uint32_t id : emf::players(w))
        if (const auto p = pos_of(w, id)) out.push_back({id, *p, std::max(level_of(w, id), 1u)});
    return out;
}

void show(const std::string& text, const Config& c) {
    if (text == g_hud_shown && !g_redraw.exchange(false)) return;
    g_hud_shown = text;
    g_hud_visible = !text.empty();
    emf::hud(kHudLine, text, c.corner, c.rgba);
}

// Still our raider (it exists, its template and the level we wrote are its own).
bool ours(emf::World w, uint32_t id) {
    auto it = g_ids.find(id);
    return it != g_ids.end() && raids::is_raider(w, id, it->second);
}

void spawn_wave(emf::World w, const raids::Action& a, const Config& c) {
    const raids::Ring ring = raids::ring_for(c.s.base_radius);
    const int total = a.count + (a.boss ? 1 : 0);
    const auto points = raids::spawn_points(a.at, g_bases, g_crumbs, ring, c.s.base_radius, total, g_rng());
    const std::vector<raids::Template>& pool = c.custom && !c.custom_pool.empty() ? c.custom_pool : raids::verified_pool();
    int spawned = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        const bool boss = a.boss && i + 1 == points.size();
        const raids::Template& t = boss ? raids::boss_template() : raids::pick_template(pool, a.level, g_rng());
        uint8_t guid[16];
        if (!raids::parse_guid_text(t.guid, guid)) continue;
        const auto& p = points[i];
        const uint32_t id = emf::api()->spawn(w.raw(), guid, p.x, p.y, p.z);
        if (!id) continue;
        ++spawned;
        g_director.on_spawned(id);
        g_ids[id] = raids::identify(w, id);
        g_stuck.on_spawned(id, p, GetTickCount64());
        g_levels.push_back({id, a.level, GetTickCount64() + 10000});
        order(g_guards, id, a.altar);  // march on the altar (seen in game)
        EMF_LOG(kMod, "raider %u (%s, level %u) at (%.1f, %.1f, %.1f), %.0f m from altar %u", id, t.name.c_str(), a.level,
                p.x, p.y, p.z, raids::horizontal_distance(p, a.at), a.altar);
    }
    g_director.on_wave_spawned(spawned);
    EMF_LOG(kMod, "wave %d: %d of %d raider(s) spawned", g_director.wave(), spawned, total);
}

// Level + an immediate rescale (DynamicEnemyScaling.nextUpdateTime = 0), as the companion does (health 119 -> 417 at
// level 10, re_companion_orders.md). The components may appear a step after the spawn: retried for 10 s.
void apply_levels(emf::World w, uint64_t now) {
    for (size_t i = 0; i < g_levels.size();) {
        auto& p = g_levels[i];
        if (!ours(w, p.id)) {  // gone, or its id is someone else's now: never written onto another entity
            g_levels.erase(g_levels.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (raids::write_level(w, p.id, p.level)) {
            g_ids[p.id].level = p.level;
        } else if (now < p.until_ms) {
            ++i;
            continue;
        } else {
            EMF_LOG(kMod, "raider %u has no Level: it keeps the template's strength", p.id);
        }
        g_levels.erase(g_levels.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void reward(emf::World w, const raids::Action& a, const Config& c) {
    const raids::Ring ring = raids::ring_for(c.s.base_radius);
    for (const auto& pl : read_players(w)) {
        if (raids::horizontal_distance(pl.pos, a.at) > ring.max + 20) continue;
        for (const auto& item : raids::reward_items(c.reward, a.count, pl.level, g_rng())) {
            const uint32_t id = emf::api()->find_item(w.raw(), item.item.c_str());
            if (!id) {
                EMF_LOG(kMod, "reward: the game has no item %s", item.item.c_str());
                continue;
            }
            const std::string name = item.item;
            const uint32_t player = pl.id, count = item.count;
            // what does not fit (or a put refused at once) is dropped at the player's feet: never lost
            const bool queued = emf::put_items_ex(w, player, id, count, item.level, item.rarity,
                                                  [name, player, count, id](emf::World w2, uint32_t added) {
                EMF_LOG(kMod, "reward: %u of %u x %s for player %u", added, count, name.c_str(), player);
                if (added < count) {
                    const uint32_t dropped = emf::api()->give_item(w2.raw(), player, id, count - added);
                    EMF_LOG(kMod, "reward: the other %u dropped at player %u's feet%s", count - added, player,
                            dropped ? "" : " FAILED (see the core log)");
                }
            });
            if (!queued) {
                const uint32_t dropped = emf::api()->give_item(w.raw(), player, id, count);
                EMF_LOG(kMod, "reward: the put of %u x %s for player %u was refused (the core log says why); dropped at "
                              "their feet%s", count, name.c_str(), player, dropped ? "" : " FAILED");
            }
        }
    }
}

void run(emf::World w, const std::vector<raids::Action>& actions, const Config& c) {
    for (const auto& a : actions) {
        switch (a.kind) {
            case raids::Action::Notice:
                EMF_LOG(kMod, "%s", a.text.c_str());
                EMF_NOTIFY("Night Raids: %s", a.text.c_str());
                break;
            case raids::Action::Log: EMF_LOG(kMod, "%s", a.text.c_str()); break;
            case raids::Action::Hud: show(a.text, c); break;
            case raids::Action::Spawn: spawn_wave(w, a, c); break;
            case raids::Action::Reward: reward(w, a, c); break;
            case raids::Action::Destroy: {
                const int n = raids::destroy_raiders(w, a.ids, g_ids);
                for (uint32_t id : a.ids) g_stuck.forget(id);
                EMF_LOG(kMod, "%d of %zu raider(s) removed (the rest were gone, or their ids are someone else's now)", n,
                        a.ids.size());
                break;
            }
        }
    }
}

raids::Tick make_tick(emf::World w, uint64_t now, const std::vector<raids::Player>& players, const Config& c) {
    raids::Tick t;
    t.now_ms = now;
    t.tod = emf::time_of_day(w);
    t.world_on = g_world_on;
    t.session = emf::server_session();
    t.occupied = raids::occupied_bases(g_bases, players, c.s.base_radius);
    if (g_director.phase() != raids::Phase::Idle)
        t.defenders = raids::players_near(g_director.base().pos, players, raids::ring_for(c.s.base_radius).max + 20);
    t.roll = g_rng();
    return t;
}

// Raiders: gone ones noted; the others kept marching on the altar and fighting living players near them.
void upkeep(emf::World w, uint64_t now, const std::vector<raids::Player>& players) {
    for (uint32_t id : g_director.living()) {
        if (!ours(w, id)) {  // gone (or its id is someone else's now)
            g_director.on_gone(id);
            g_stuck.forget(id);
            continue;
        }
        const auto p = pos_of(w, id);
        if (p && g_director.phase() == raids::Phase::Fighting && g_stuck.stuck(id, *p, g_director.base().pos, now)) {
            emf::api()->destroy(w.raw(), id);  // fell at its spawn, or found no way in: removed, no kill
            g_director.on_gone(id);
            g_stuck.forget(id);
            EMF_LOG(kMod, "raider %u is stuck at (%.1f, %.1f, %.1f), %.0f m from the altar: removed (no kill)", id, p->x,
                    p->y, p->z, raids::horizontal_distance(*p, g_director.base().pos));
        }
    }
    if (now - g_guard_ms < kGuardEveryMs || g_director.phase() != raids::Phase::Fighting) return;
    g_guard_ms = now;
    const uint32_t altar = g_director.base().altar;
    for (uint32_t id : g_director.living()) {
        auto* ai = w.get<gen::DynamicEnemy01BehaviorState>(id);
        if (ai && ai->guardEntityId.id != altar) order(g_guards, id, altar);
        const auto p = pos_of(w, id);
        if (!p) continue;
        uint32_t target = 0;
        double best = kThreatRange;
        for (const auto& pl : players) {
            const double d = raids::horizontal_distance(pl.pos, *p);
            if (d <= best && alive(w, pl.id)) {  // never a dead player (seen in game: the raider then wandered off)
                best = d;
                target = pl.id;
            }
        }
        if (target) order(g_threats, id, target);
    }
}

// The per-world switch: the world's file decides; the setting mirrors it (switching the setting while the world is
// loaded switches the world).
void sync_switch() {
    const auto want = raids::switch_step(g_file.ready(), emf::setting_int(kMod, "this_world", -1), g_world_on);
    if (!want) return;
    g_world_on = *want;
    g_file.save(raids::world_text(g_world_on));
    EMF_NOTIFY("Night Raids: %s for this world", g_world_on ? "on" : "off");
    EMF_LOG(kMod, "switched %s for world slot %04x (setting)", g_world_on ? "on" : "off", g_file.slot());
}

// A new hosted session (another world, a reload): what we knew of the old one means nothing, and its raiders' ids
// belong to other entities now. Runs before anything uses an id: the tick, the commands, the death event.
void sync_session(emf::World w) {
    const uint32_t session = emf::server_session();
    if (session == g_session) return;
    g_session = session;
    g_crumbs.clear();
    g_levels.clear();
    g_ids.clear();
    g_stuck.clear();
    g_bases_ms = 0;
    {
        std::lock_guard<std::mutex> l(g_orders_lock);
        g_guards.clear();
        g_threats.clear();
    }
    if (!g_file.ready()) g_world_on = false;
    run(w, g_director.adopt_session(session), config());
}

void on_tick(emf::World w) {
    const uint64_t now = GetTickCount64();
    if (w.is_client()) {  // no hosted world steps (someone else's game, or the menu): the raid line goes
        if (now - g_server_seen.load() > 3000 && g_hud_visible.exchange(false)) {
            emf::hud(kHudLine, "");
            g_redraw = true;  // drawn again when a hosted world steps
        }
        return;
    }
    if (!w.is_server()) return;
    g_server_seen = now;
    const Config c = config();
    sync_session(w);
    if (g_switched_off) {  // switched off in the Mods tab: end everything once, then rest until switched on
        if (!g_stopped_for_switch) {
            g_stopped_for_switch = true;
            run(w, g_director.stop("Night Raids was switched off"), c);
            show("", c);
        }
        return;
    }
    g_stopped_for_switch = false;
    g_file.poll([](const std::string& text) {
        g_world_on = raids::parse_world_on(text);
        emf::set_setting(kMod, "this_world", g_world_on ? "1" : "0");
        EMF_LOG(kMod, "this world (slot %04x): raids %s", g_file.slot(), g_world_on ? "on" : "off (raids.world on)");
    });
    sync_switch();
    apply_levels(w, now);
    if (now - g_last_ms < 1000) return;
    g_last_ms = now;
    if (now - g_bases_ms >= 10000) {
        g_bases_ms = now;
        refresh_bases(w);
    }
    g_director.set_settings(c.s);
    const auto players = read_players(w);
    if (now - g_crumb_ms >= 2000) {  // walkable spots outside the bases, for spawn points
        g_crumb_ms = now;
        for (const auto& p : players)
            if (!raids::inside_any_base(p.pos, g_bases, c.s.base_radius)) g_crumbs.add(p.pos, now);
    }
    upkeep(w, now, players);
    run(w, g_director.tick(make_tick(w, now, players, c)), c);
}

// ---- commands ----
void cmd_status(emf::World w, const std::vector<std::string>&) {
    sync_session(w);
    const Config c = config();
    const auto tod = emf::time_of_day(w);
    char time[16] = "?";
    if (tod) snprintf(time, sizeof time, "%02d:%02d", static_cast<int>(*tod), static_cast<int>((*tod - static_cast<int>(*tod)) * 60));
    if (!g_file.ready()) {
        EMF_REPLY("Night Raids: no world loaded that this game hosts");
        return;
    }
    EMF_REPLY("Night Raids is %s in this world; %d%% chance each night at %.1f h (now %s); %d wave(s) of %d", g_world_on ? "on" : "off",
              c.s.chance, c.s.start_hour, time, c.s.waves, c.s.per_wave);
    EMF_REPLY("%s", g_director.status().c_str());
}

void cmd_start(emf::World w, const std::vector<std::string>& a) {
    sync_session(w);
    if (g_switched_off) {
        EMF_REPLY("Night Raids is switched off (Mods tab): switch it on first");
        return;
    }
    if (!g_world_on) {
        EMF_REPLY("Night Raids is off in this world: raids.world on first");
        return;
    }
    const Config c = config();
    const int warning = a.size() > 1 ? std::atoi(a[1].c_str()) : 10;
    if (g_bases.empty()) refresh_bases(w);
    const auto players = read_players(w);
    std::string why;
    const auto actions = g_director.start(make_tick(w, GetTickCount64(), players, c), warning, &why);
    if (actions.empty()) {
        EMF_REPLY("not started: %s", why.c_str());
        return;
    }
    run(w, actions, c);
    EMF_REPLY("raid started at altar %u: the raiders come in %d s", g_director.base().altar, warning);
}

void cmd_stop(emf::World w, const std::vector<std::string>&) {
    sync_session(w);
    const auto actions = g_director.stop("The raid was called off");
    if (actions.empty()) {
        EMF_REPLY("no raid is on");
        return;
    }
    run(w, actions, config());
    EMF_REPLY("raid stopped; its raiders are gone");
}

void cmd_world(emf::World, const std::vector<std::string>& a) {
    if (a.size() != 2 || (a[1] != "on" && a[1] != "off")) {
        EMF_REPLY("usage: raids.world on|off (now %s)", g_world_on ? "on" : "off");
        return;
    }
    if (!g_file.ready()) {
        EMF_REPLY("no world this game hosts is loaded");
        return;
    }
    g_world_on = a[1] == "on";
    g_file.save(raids::world_text(g_world_on));
    emf::set_setting(kMod, "this_world", g_world_on ? "1" : "0");
    EMF_REPLY("Night Raids %s for this world (slot %04x)", g_world_on ? "on" : "off", g_file.slot());
}

// For tests: every living raider dies at once (no attacker, no kill credit); the raid then ends as repelled.
void cmd_kill(emf::World w, const std::vector<std::string>&) {
    sync_session(w);
    int n = 0;
    for (uint32_t id : g_director.living()) {
        if (!ours(w, id)) continue;
        g_director.credit(id);  // a test kill counts, as if the players had done it
        n += emf::kill(w, id) ? 1 : 0;
    }
    EMF_REPLY("%d raider(s) killed", n);
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
    if (!emf::add_program("raids", EMF_WORLD_SERVER, {}, {"StartGuardingEvent", "AddThreatTargetEvent"}, raids_program)) {
        EMF_LOG(kMod, "could not add the raids program: off");
        return false;
    }
    emf::on_event<gen::EntityDiedEvent>(EMF_WORLD_SERVER, [](emf::World w, const gen::EntityDiedEvent& e) {
        sync_session(w);
        if (g_director.is_raider(e.deadEntityId.id)) g_director.on_died(e.deadEntityId.id);
        g_stuck.forget(e.deadEntityId.id);
    });
    // a kill counts for the reward only when a player or an ally (the companion, summons) hit the raider
    emf::on_event<gen::HitEvent>(EMF_WORLD_SERVER, [](emf::World w, const gen::HitEvent& e) {
        sync_session(w);
        const uint32_t target = e.targetId.id, source = e.rootSourceId.id;
        if (!target || !g_director.is_raider(target) || !source) return;
        bool by_players = w.get<gen::PlayerState>(source) != nullptr;
        if (auto* f = w.get<gen::Faction>(source)) {
            const auto fac = f->faction;
            by_players = by_players || fac == emf::gen::FactionType::Player || fac == emf::gen::FactionType::PlayerAlly;
        }
        if (by_players) g_director.credit(target);
    });
    g_file.attach();
    const auto command = [](const char* name, const char* help, void (*f)(emf::World, const std::vector<std::string>&),
                            uint32_t flags) {
        if (!emf::add_command(name, help, f, flags)) EMF_LOG(kMod, "the command %s could not be added (see the core log)", name);
    };
    command("raids.status", ": is Night Raids on in this world, and the raid going on", cmd_status, EMF_COMMAND_PLAYERS);
    command("raids.start", "[warning seconds]: a raid now at the base with the most players (host, for tests)", cmd_start,
            0);
    command("raids.stop", ": end the raid; its raiders are removed (host)", cmd_stop, 0);
    command("raids.world", "on|off: Night Raids in this world (host; every world starts off)", cmd_world, 0);
    command("raids.kill", ": every living raider dies (host, for tests; they count as slain)", cmd_kill, 0);
    emf::on_tick(on_tick);
    const Config c = config();
    EMF_LOG(kMod, "ready: %d%% chance per night at %.1f h, %d wave(s) of %d; off in each world until raids.world on",
            c.s.chance, c.s.start_hour, c.s.waves, c.s.per_wave);
    return true;
}
