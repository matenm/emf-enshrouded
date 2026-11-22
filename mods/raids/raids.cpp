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
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <set>
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
constexpr double kThreatRange = 25.0;  // a raider this close to a living player (or ally) is told to fight it

// ---- settings (written by the change callback, any world's thread; read by the server tick) ----
struct Config {
    raids::Settings s;
    bool custom = false;
    std::vector<raids::Template> custom_pool;
    raids::RewardKind reward = raids::RewardKind::Materials;
    raids::Targets targets = raids::Targets::Allies;
    std::string preset = "custom";  // the "Raid strength" setting
    raids::WallSettings walls;      // "Raiders break walls", "Hole size", "Broken blocks drop items" (and the difficulty)
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
    c.s.progression = emf::setting_bool(kMod, "progression", true);
    c.s.bases = raids::base_pick(emf::setting_text(kMod, "bases", "busiest"));
    c.s.base_radius = emf::setting_float(kMod, "base_radius", 30.0);
    c.reward = raids::reward_kind(emf::setting_text(kMod, "reward", "materials"));
    c.targets = raids::targets_mode(emf::setting_text(kMod, "targets", "allies"));
    c.preset = emf::setting_text(kMod, "preset", "custom");
    if (!raids::apply_preset(c.preset, c.s, c.reward)) c.preset = "custom";  // a preset replaces its settings
    c.walls = raids::wall_settings(emf::setting_bool(kMod, "break_walls", false), emf::setting_text(kMod, "hole_size", "small"),
                                   emf::setting_bool(kMod, "wall_drops", false), c.preset,
                                   emf::setting_text(kMod, "difficulty", "normal"),
                                   emf::setting_float(kMod, "wall_strength", 1.0),
                                   emf::setting_bool(kMod, "break_terrain", false));
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
struct PendingVet {  // a raider from a custom template whose level is not known: watched for 10 s for a boss bar
    uint32_t id;
    uint64_t until_ms;
    std::string guid;    // its template
    uint32_t level, altar;
};
std::vector<PendingVet> g_vet;
std::set<std::string> g_found_bosses;  // bare custom GUIDs seen to be a boss or mini-boss: Vukah warriors come instead
std::map<uint32_t, raids::RaiderId> g_ids;  // who each raider is (template, level): ids are reused once one is gone
std::map<uint32_t, uint32_t> g_targets;     // each raider's last target (it keeps it while it lives and is near)
raids::StuckWatch g_stuck;

// ---- walls ("Raiders break walls", EMF API v44): a raider held up by a wall or a closed gate breaks through it ----
raids::WallWork g_walls;
std::map<uint32_t, uint32_t> g_wall_orders;   // raider -> its enemy order on a gate or a swing target
struct SwingTarget {
    uint32_t entity = 0;
    raids::Vec3 at;    // the wall point it stands for
    raids::Vec3 base;  // where it was spawned (it never moves: an entity elsewhere is not ours any more)
};
std::map<uint32_t, SwingTarget> g_swing_targets;  // wall spot -> what its workers swing at
raids::SwingWatch g_swings;                       // the wall workers' last attack times
std::set<uint32_t> g_wall_looking;            // raiders whose gate path query is out
std::set<std::string> g_wall_said;            // refusals logged once per session
bool g_walls_v44_said = false;
constexpr double kFoeNear = 4.0;  // a worker fights a foe this near instead of the wall

bool g_walls_unavailable = false;  // a hit was refused because breaking is not there: no more wall looks this run
bool g_terrain_unavailable = false;  // a hit with terrain was refused because breaking terrain is not there (1.3.0)

// find_wall's flags: terrain a player placed too when the setting is on and the core can (API v47).
uint32_t wall_find_flags(const raids::WallSettings& s) {
    return s.terrain && !g_terrain_unavailable && emf::has_v47() ? EMF_FIND_TERRAIN : 0u;
}

void wall_say_once(const std::string& key, const std::string& line) {
    if (g_wall_said.insert(key).second) EMF_LOG(kMod, "%s", line.c_str());
}
void wall_cancel_order(uint32_t raider) {
    auto it = g_wall_orders.find(raider);
    if (it == g_wall_orders.end()) return;
    if (emf::has_v39() && emf::api()->enemy_order_cancel) emf::api()->enemy_order_cancel(it->second);
    g_wall_orders.erase(it);
}
void wall_forget(uint32_t raider) {
    wall_cancel_order(raider);
    g_walls.forget(raider);
    g_swings.forget(raider);
}

// Puts one raider of template `t` at `p` (level written soon, marching on `altar`). 0 when the game refused it.
uint32_t spawn_raider(emf::World w, const raids::Template& t, const raids::Vec3& p, uint32_t level, uint32_t altar,
                      bool vet) {
    uint8_t guid[16];
    if (!raids::parse_guid_text(t.guid, guid)) return 0;
    const uint32_t id = emf::api()->spawn(w.raw(), guid, p.x, p.y, p.z);
    if (!id) return 0;
    g_director.on_spawned(id);
    g_ids[id] = raids::identify(w, id);
    g_stuck.on_spawned(id, p, GetTickCount64());
    g_levels.push_back({id, level, GetTickCount64() + 10000});
    if (vet) g_vet.push_back({id, GetTickCount64() + 10000, t.guid, level, altar});
    order(g_guards, id, altar);  // march on the altar (seen in game)
    return id;
}

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

// ---- walls: the look, the hits, the orders ----
void wall_actions(emf::World w, const std::vector<raids::WallAction>& acts, const Config& c);

// Takes the spot's swing target away (when it is still ours: where it was spawned).
void swing_target_remove(emf::World w, uint32_t spot) {
    auto it = g_swing_targets.find(spot);
    if (it == g_swing_targets.end()) return;
    const auto p = pos_of(w, it->second.entity);
    if (p && raids::horizontal_distance(*p, it->second.base) < 0.5 && std::fabs(p->y - it->second.base.y) < 0.5) {
        emf::api()->destroy(w.raw(), it->second.entity);
        EMF_LOG(kMod, "swing target %u at (%.1f, %.1f, %.1f) taken away", it->second.entity, it->second.base.x,
                it->second.base.y, it->second.base.z);
    } else {
        EMF_LOG(kMod, "swing target %u is gone or moved: left alone", it->second.entity);
    }
    g_swing_targets.erase(it);
}
// The spot's swing target for the wall point `at`: 0.75 m out from it toward `side` (where its first worker was held
// up), at that height and turned to face it, where it can be reached (one inside the wall made raiders walk round to
// the far side and stop short, seen in game). The one there, else a new one (one left at a point the hole has gone
// deeper from goes). 0 when the game refused it.
uint32_t swing_target(emf::World w, uint32_t spot, const raids::Vec3& at, const raids::Vec3& side) {
    auto it = g_swing_targets.find(spot);
    if (it != g_swing_targets.end()) {
        if (raids::horizontal_distance(it->second.at, at) < 0.25 && pos_of(w, it->second.entity))
            return it->second.entity;
        swing_target_remove(w, spot);
    }
    uint8_t guid[16];
    if (!raids::parse_guid_text(raids::kSwingTargetGuid, guid)) return 0;
    const raids::Vec3 base = raids::front_point(at, side, raids::WallWork::kTargetOut);
    const uint32_t id = emf::spawn_facing(w, guid, base.x, base.y, base.z, float(raids::yaw_toward(base, side)));
    if (id) {
        g_swing_targets[spot] = {id, at, base};
        EMF_LOG(kMod, "swing target %u at (%.1f, %.1f, %.1f) for the wall at (%.1f, %.1f, %.1f)", id, base.x, base.y, base.z,
                at.x, at.y, at.z);
    }
    return id;
}
bool is_swing_target(uint32_t id) {
    for (const auto& [spot, t] : g_swing_targets)
        if (t.entity == id) return true;
    return false;
}

struct HitCtx {
    uint32_t spot, session;
};

// A hit's answer: damage seen holds the stuck clock; once blocks went, the spot is through when a worker's line to the
// altar is clear.
void on_wall_hit(EmfWorld* raw, uint32_t, const EmfBreakResult* res, void* user) {
    const std::unique_ptr<HitCtx> ctx(static_cast<HitCtx*>(user));
    if (!raw || !res || ctx->session != emf::server_session()) return;
    emf::World w(raw);
    g_walls.on_hit(ctx->spot, res->damaged, res->removed, GetTickCount64());
    if (res->status != EMF_BREAK_DONE || !res->removed) return;
    const raids::Vec3 altar = g_director.base().pos;
    for (const auto& sp : g_walls.spots()) {
        if (sp.id != ctx->spot) continue;
        std::optional<raids::Vec3> deeper;  // the wall is thicker than the hole: the next wall point on a worker's line
        double feet_y = 0;
        for (uint32_t r : sp.workers) {
            const auto p = pos_of(w, r);
            if (!p) continue;
            const auto line = raids::chest_line(*p, altar, raids::WallWork::kReach);
            const float f[3] = {float(line.first.x), float(line.first.y), float(line.first.z)};
            const float t[3] = {float(line.second.x), float(line.second.y), float(line.second.z)};
            if (const auto b = emf::find_wall(w, f, t, wall_find_flags(config().walls))) {
                if (!raids::breakable_wall(*b, emf::has_v45())) continue;  // a ruin behind the hole: no deeper there
                if (!deeper) {
                    deeper = raids::Vec3{b->x, b->y, b->z};
                    feet_y = p->y;
                }
                continue;
            }
            EMF_LOG(kMod, "raider %u: the wall at (%.1f, %.1f, %.1f) is open; on to the altar", r, sp.point.x, sp.point.y,
                    sp.point.z);
            wall_actions(w, g_walls.wall_clear(sp.id), config());
            return;
        }
        if (deeper && raids::horizontal_distance(*deeper, sp.point) >= 0.5) {
            if (g_walls.advance(sp.id, *deeper, feet_y, config().walls))
                EMF_LOG(kMod, "the hole at (%.1f, %.1f, %.1f) goes deeper, to (%.1f, %.1f, %.1f)", sp.point.x, sp.point.y,
                        sp.point.z, deeper->x, deeper->y, deeper->z);
        }
        return;
    }
}

void wall_assign(emf::World, uint32_t id, const raids::Vec3& p, std::optional<raids::Vec3> wall,
                 std::optional<raids::Gate> gate, const Config& c) {
    if (!c.walls.on || g_director.phase() != raids::Phase::Fighting || (!wall && !gate)) return;
    const auto spot = g_walls.assign(id, p, wall, gate, c.walls, GetTickCount64());
    if (!spot) {
        wall_say_once("spots", "a raider found a wall, but 4 wall spots are worked already");
        return;
    }
    g_stuck.hold(id, GetTickCount64());  // a minute from now to reach it and make it give
    for (const auto& sp : g_walls.spots())
        if (sp.id == *spot) {
            if (sp.gate)
                EMF_LOG(kMod, "raider %u held up at (%.1f, %.1f, %.1f): it breaks gate %u (%d added to each hit)", id, p.x,
                        p.y, p.z, sp.door, sp.gate_damage);
            else
                EMF_LOG(kMod, "raider %u held up at (%.1f, %.1f, %.1f): it breaks the wall at (%.1f, %.1f, %.1f)", id, p.x,
                        p.y, p.z, sp.point.x, sp.point.y, sp.point.z);
        }
}

struct LookCtx {
    uint32_t raider, session;
    std::optional<raids::Vec3> wall;
};
void on_gate_path(EmfWorld* raw, uint32_t, const EmfPathResult* r, const EmfPathPoint* pts, void* user) {
    const std::unique_ptr<LookCtx> ctx(static_cast<LookCtx*>(user));
    g_wall_looking.erase(ctx->raider);
    if (!raw || ctx->session != emf::server_session()) return;
    emf::World w(raw);
    if (!ours(w, ctx->raider)) return;
    const auto p = pos_of(w, ctx->raider);
    if (!p) return;
    std::optional<raids::Gate> gate;
    const uint32_t* doors = r && r->size >= offsetof(EmfPathResult, doors) + sizeof(r->doors) ? r->doors : nullptr;
    if (r && r->status == EMF_PATH_FOUND)
        if (const auto g = raids::nearest_gate(pts, r->count, doors, *p, raids::WallWork::kGateRange);
            g && !is_swing_target(g->first)) {  // a swing target is a door to the game, never a gate to break
            const uint32_t st = emf::has_v38() && emf::api()->door_state ? emf::api()->door_state(raw, g->first) : 0;
            if ((st & 0xFu) == EMF_DOOR_CLOSED) gate = raids::Gate{g->first, g->second, raids::door_max_health(w, g->first)};
        }
    wall_assign(w, ctx->raider, *p, ctx->wall, gate, config());
}

// Looks for the wall in front of a held-up raider (now) and the first closed door on its way (a path query: the answer
// comes after a step), then assigns it the nearer of the two.
void wall_look(emf::World w, uint32_t id, const raids::Vec3& p, const Config& c) {
    const raids::Vec3 altar = g_director.base().pos;
    const auto line = raids::chest_line(p, altar, raids::WallWork::kReach);
    const float f[3] = {float(line.first.x), float(line.first.y), float(line.first.z)};
    const float t[3] = {float(line.second.x), float(line.second.y), float(line.second.z)};
    std::optional<raids::Vec3> wall;
    if (const auto b = g_walls_unavailable ? std::nullopt : emf::find_wall(w, f, t, wall_find_flags(c.walls))) {
        if (raids::breakable_wall(*b, emf::has_v45()))
            wall = raids::Vec3{b->x, b->y, b->z};
        else
            wall_say_once("ruin", "a raider's wall is the world's own (a ruin) or of unknown origin: left alone (said once)");
    }
    if (emf::has_v42() && emf::api()->path_find && !g_wall_looking.count(id)) {
        EmfPathQuery q{};
        q.size = sizeof q;
        q.owner = id;
        q.from[0] = float(p.x), q.from[1] = float(p.y), q.from[2] = float(p.z);
        q.to[0] = float(altar.x), q.to[1] = float(altar.y), q.to[2] = float(altar.z);
        q.moves = EMF_PATH_WALK | EMF_PATH_OPEN_DOOR;
        q.closest = 1;
        q.max_nodes = 5000;
        auto* ctx = new LookCtx{id, emf::server_session(), wall};
        if (emf::api()->path_find(w.raw(), &q, on_gate_path, ctx)) {
            g_wall_looking.insert(id);
            return;
        }
        delete ctx;
    }
    wall_assign(w, id, p, wall, std::nullopt, c);
}

void wall_actions(emf::World w, const std::vector<raids::WallAction>& acts, const Config& c) {
    const EmfApi* a = emf::api();
    for (const auto& x : acts) {
        switch (x.kind) {
            case raids::WallAction::Hit: {
                EmfBreakBlocks r{};
                r.x = float(x.hole.centre.x), r.y = float(x.hole.centre.y), r.z = float(x.hole.centre.z);
                r.radius = float(x.hole.radius);
                r.height = float(x.hole.height);
                raids::wall_hit_damage(x.damage_scale, r.damage);
                if (c.walls.drops) r.flags |= EMF_BREAK_DROPS;
                if (wall_find_flags(c.walls)) r.flags |= EMF_BREAK_TERRAIN;  // the terrain a player placed too
                auto* ctx = new HitCtx{x.spot, emf::server_session()};
                if (!emf::break_blocks(w, r, on_wall_hit, ctx)) {
                    delete ctx;
                    if ((r.flags & EMF_BREAK_TERRAIN) && raids::terrain_unavailable(r.why)) {
                        if (!g_terrain_unavailable)
                            EMF_LOG(kMod, "breaking terrain walls is not available (%s): raiders break building blocks only",
                                    r.why);
                        g_terrain_unavailable = true;
                        break;
                    }
                    wall_say_once("hit", std::string("a wall hit was refused: ") + r.why);
                    if (raids::breaking_unavailable(r.why) && !g_walls_unavailable) {
                        g_walls_unavailable = true;
                        EMF_LOG(kMod, "wall breaking is not available (%s): raiders go for closed doors only", r.why);
                    }
                }
                break;
            }
            case raids::WallAction::OrderWall: {
                // Near the wall point: it guards the spot's swing target (in front of the wall, on the side it was
                // held up on) and gets an enemy order on it, and swings at it (its swings break the wall). Farther:
                // walked up (peacefully) first, since an order from far stops it about 5.5 m short, and guarding the
                // altar pulled workers off the wall into the altar's guard area (both seen in game). A creature that
                // never swings breaks the wall by the clock (WallWork's timed hits).
                const auto p = pos_of(w, x.raider);
                if (!p) break;
                const uint32_t target = swing_target(w, x.spot, x.at, x.side);
                if (!target) wall_say_once("target", "a swing target could not be spawned: raiders break walls by the clock");
                if (x.swing) {
                    if (!emf::has_v39() || !target) break;
                    auto* ai = w.get<gen::DynamicEnemy01BehaviorState>(x.raider);
                    if (ai && ai->guardEntityId.id != target) order(g_guards, x.raider, target);
                    EmfEnemyOrder o{};
                    o.size = sizeof o;
                    o.enemy = x.raider;
                    o.target = target;
                    o.seconds = 3.5f;
                    const bool first = !g_wall_orders.count(x.raider);
                    const uint32_t oid = a->enemy_order(w.raw(), &o);  // (it ends a walk-up still under way)
                    if (oid) g_wall_orders[x.raider] = oid;
                    else wall_say_once("swing", std::string("a raider takes no order on a swing target (") + o.why + ")");
                    if (oid && first) EMF_LOG(kMod, "raider %u swings at the wall: an order on swing target %u", x.raider, target);
                    break;
                }
                wall_cancel_order(x.raider);
                if (emf::has_v40() && !a->enemy_moving(w.raw(), x.raider)) {
                    const raids::Vec3 f = raids::front_point(x.at, x.side, raids::WallWork::kStandOut);
                    const EmfPathPoint pt{float(f.x), float(p->y), float(f.z), 0};
                    EmfMoveOrder m{};
                    m.size = sizeof m;
                    m.enemy = x.raider;
                    m.flags = EMF_MOVE_PEACEFUL;
                    m.count = 1;
                    m.points = &pt;
                    m.arrive = 0.5f;
                    m.seconds = 10.0f;
                    m.stuck_seconds = 4.0f;
                    if (!a->enemy_move(w.raw(), &m, nullptr, nullptr))
                        wall_say_once("walk", std::string("a raider takes no walk-up to a wall (") + m.why + ")");
                }
                break;
            }
            case raids::WallAction::OrderGate: {
                if (!emf::has_v39()) break;
                EmfEnemyOrder o{};
                o.size = sizeof o;
                o.enemy = x.raider;
                o.target = x.door;
                o.seconds = 3.5f;
                o.damage = x.gate_damage;
                const uint32_t oid = a->enemy_order(w.raw(), &o);
                if (oid) g_wall_orders[x.raider] = oid;
                else wall_say_once("gate", std::string("a raider takes no order on a gate (") + o.why + ")");
                break;
            }
            case raids::WallAction::Release:
                wall_cancel_order(x.raider);
                g_swings.forget(x.raider);
                order(g_guards, x.raider, g_director.base().altar);  // on to the altar again
                break;
            case raids::WallAction::GiveUp:
                swing_target_remove(w, x.spot);
                EMF_LOG(kMod, "wall spot at (%.1f, %.1f, %.1f) given up", x.at.x, x.at.y, x.at.z);
                break;
            case raids::WallAction::Done: swing_target_remove(w, x.spot); break;
        }
    }
}

// Once a second while a raid fights: looks for held-up raiders, hits, orders, and watches the gates being broken.
void walls_tick(emf::World w, uint64_t now, const std::vector<raids::Player>& players, const Config& c) {
    bool active = c.walls.on && g_director.phase() == raids::Phase::Fighting;
    if (active && !raids::walls_available()) {
        if (!g_walls_v44_said) {
            g_walls_v44_said = true;
            EMF_LOG(kMod, "Raiders break walls needs EMF API v44 (EMF 0.6.14 or newer): raids go on as before");
        }
        active = false;
    }
    raids::WallSettings s = c.walls;
    s.on = active;
    std::map<uint32_t, raids::Vec3> raiders;
    for (uint32_t id : g_director.living())
        if (const auto p = pos_of(w, id)) raiders[id] = *p;
    if (active)
        for (const auto& [id, p] : raiders)
            if (g_walls.wants_look(id, p, g_director.base().pos, c.s.base_radius, now) &&
                !raids::player_near(p, players, kFoeNear))  // fighting a player: not held up by a wall
                wall_look(w, id, p, c);
    wall_actions(w, g_walls.tick(raiders, s, now), c);
    if (!active) return;
    for (const auto& sp : g_walls.spots()) {  // a wall worker's swing (its last attack time changed) breaks the wall
        if (sp.gate) continue;
        for (uint32_t r : sp.workers) {
            const auto it = raiders.find(r);
            if (it != raiders.end() && g_swings.swung(r, raids::last_attack_time(w, r)))
                wall_actions(w, g_walls.swung(r, it->second, s, now), c);
        }
    }
    for (const auto& sp : g_walls.spots()) {
        if (!sp.gate) continue;
        if (raids::gate_gone(w, sp.door)) {  // broken by the game, taken away, or its id is someone else's now
            EMF_LOG(kMod, "gate %u is gone; on to the altar", sp.door);
            wall_actions(w, g_walls.wall_clear(sp.id), c);
            continue;
        }
        const int32_t hp = raids::door_health(w, sp.door);
        g_walls.on_gate_health(sp.id, hp, now);
        if (!raids::gate_finish_due(hp, sp.gate_damage)) continue;
        std::string why;
        if (raids::remove_gate(w, sp.door, c.walls.drops, &why)) {
            EMF_LOG(kMod, "gate %u broken%s", sp.door, c.walls.drops ? " (its item dropped)" : "");
            wall_actions(w, g_walls.wall_clear(sp.id), c);
        } else {
            wall_say_once("remove", "a gate could not be taken away (" + why + "); the raiders' hits break it");
        }
    }
}

void spawn_wave(emf::World w, const raids::Action& a, const Config& c) {
    const raids::Ring ring = raids::ring_for(c.s.base_radius);
    const int total = a.count + (a.boss ? 1 : 0);
    const auto points = raids::spawn_points(a.at, g_bases, g_crumbs, ring, c.s.base_radius, total, g_rng());
    const std::vector<raids::Template>& pool = c.custom && !c.custom_pool.empty() ? c.custom_pool : raids::verified_pool();
    int spawned = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        const bool boss = a.boss && i + 1 == points.size();
        const raids::Template t = boss ? raids::boss_template()
                                       : raids::choose_raider(pool, a.level, a.player_level, g_rng(), c.s.progression,
                                                              &g_found_bosses);
        const auto& p = points[i];
        const uint32_t id =
            spawn_raider(w, t, p, a.level, a.altar, raids::remove_unready_boss(c.s.progression, t, true));
        if (!id) continue;
        ++spawned;
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

// The progression rule: a raider from a bare custom GUID that turns out to be a boss or mini-boss is removed (no kill):
// EMF cannot tell whether the players have beaten it. Its boss components may appear a step after the spawn: watched
// for 10 s.
void vet_bosses(emf::World w, uint64_t now) {
    for (size_t i = 0; i < g_vet.size();) {
        const uint32_t id = g_vet[i].id;
        bool done = !ours(w, id) || now >= g_vet[i].until_ms;
        if (!done && raids::is_boss(w, id)) {
            const PendingVet v = g_vet[i];
            const auto at = pos_of(w, id);
            emf::api()->destroy(w.raw(), id);
            g_director.on_gone(id);
            g_stuck.forget(id);
            wall_forget(id);
            EMF_LOG(kMod, "raider %u (%s) is a boss or mini-boss from a custom template without a level: removed (Only "
                          "creatures you are ready for; write it as GUID@level, or switch the rule off)", id, v.guid.c_str());
            if (g_found_bosses.insert(v.guid).second)
                EMF_NOTIFY("Night Raids: one of your templates is a boss or mini-boss without a level; Vukah warriors come "
                           "in its place (write it as GUID@level to let it come)");
            if (at) {  // a Vukah warrior takes its place in the wave
                const uint32_t warrior = spawn_raider(w, raids::verified_pool()[0], *at, v.level, v.altar, false);
                EMF_LOG(kMod, "raider %u (%s) takes its place%s", warrior, raids::verified_pool()[0].name.c_str(),
                        warrior ? "" : ": FAILED (see the core log)");
            }
            done = true;
        }
        if (done) g_vet.erase(g_vet.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
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
                for (uint32_t id : a.ids) {
                    g_stuck.forget(id);
                    wall_forget(id);
                }
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

// Raiders: gone ones noted; the others kept marching on the altar and fighting the living players (and, with the
// "Raiders attack" setting on allies, the companion and summons) near them.
void upkeep(emf::World w, uint64_t now, const std::vector<raids::Player>& players, const Config& c) {
    for (uint32_t id : g_director.living()) {
        if (!ours(w, id)) {  // gone (or its id is someone else's now)
            g_director.on_gone(id);
            g_stuck.forget(id);
            wall_forget(id);
            continue;
        }
        const auto p = pos_of(w, id);
        if (p && raids::stuck_clock_waits(g_walls.holding(id, now), *p, players))
            g_stuck.hold(id, now);  // its wall spot takes damage, or it fights a player: not stuck
        if (p && g_director.phase() == raids::Phase::Fighting && g_stuck.stuck(id, *p, g_director.base().pos, now)) {
            emf::api()->destroy(w.raw(), id);  // fell at its spawn, or found no way in: removed, no kill
            g_director.on_gone(id);
            g_stuck.forget(id);
            wall_forget(id);
            EMF_LOG(kMod, "raider %u is stuck at (%.1f, %.1f, %.1f), %.0f m from the altar: removed (no kill)", id, p->x,
                    p->y, p->z, raids::horizontal_distance(*p, g_director.base().pos));
        }
    }
    walls_tick(w, now, players, c);
    if (now - g_guard_ms < kGuardEveryMs || g_director.phase() != raids::Phase::Fighting) return;
    g_guard_ms = now;
    const uint32_t altar = g_director.base().altar;
    std::vector<raids::Foe> foes;  // never a dead one (seen in game: a raider told to fight a dead player wandered off)
    for (const auto& pl : players) foes.push_back({pl.id, pl.pos, true, raids::has_health(w, pl.id)});
    if (c.targets == raids::Targets::Allies)
        for (const auto& a : raids::allies_near(w, g_director.base().pos,
                                                raids::ring_for(c.s.base_radius).max + 20 + kThreatRange))
            foes.push_back(a);
    std::map<uint32_t, uint32_t> targets;  // the living raiders' targets now (gone raiders drop out)
    for (uint32_t id : g_director.living()) {
        const auto p = pos_of(w, id);
        if (p && g_walls.working(id)) {  // at a wall or a gate: no guard or threat order, unless a foe is right there
            bool foe_near = false;
            for (const auto& f : foes) foe_near = foe_near || (f.alive && raids::horizontal_distance(f.pos, *p) <= kFoeNear);
            if (!foe_near) continue;
            g_walls.release(id);
            wall_cancel_order(id);
            g_swings.forget(id);
        }
        auto* ai = w.get<gen::DynamicEnemy01BehaviorState>(id);
        if (ai && ai->guardEntityId.id != altar) order(g_guards, id, altar);
        if (!p) continue;
        const auto last = g_targets.find(id);
        const uint32_t target =
            raids::pick_target(*p, foes, kThreatRange, c.targets, last == g_targets.end() ? 0 : last->second);
        if (target) order(g_threats, id, target);
        targets[id] = target;
    }
    g_targets.swap(targets);
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
    g_vet.clear();
    g_targets.clear();
    g_ids.clear();
    g_stuck.clear();
    g_walls.clear();  // the old world's raiders went with it
    g_wall_orders.clear();
    g_swing_targets.clear();  // (its swing targets too: they are never saved)
    g_swings.clear();
    g_wall_looking.clear();
    g_wall_said.clear();
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
            wall_actions(w, g_walls.tick({}, raids::WallSettings{}, now), c);  // the wall work ends, workers released
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
    vet_bosses(w, now);
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
    upkeep(w, now, players, c);
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
    const std::string strength = c.preset == "custom" ? "own settings" : c.preset + " raids";
    EMF_REPLY("Night Raids is %s in this world (%s); %d%% chance each night at %.1f h (now %s); %d wave(s) of %d",
              g_world_on ? "on" : "off", strength.c_str(), c.s.chance, c.s.start_hour, time, c.s.waves, c.s.per_wave);
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

// raids.look <raider>: for tests, the raider looks for a wall or a closed gate toward the altar now, as one held up for
// 10 s does (a test world seldom has a base closed by walls).
void cmd_look(emf::World w, const std::vector<std::string>& a) {
    sync_session(w);
    const Config c = config();
    const uint32_t id = a.size() == 2 ? static_cast<uint32_t>(std::strtoul(a[1].c_str(), nullptr, 10)) : 0;
    if (!id) {
        EMF_REPLY("usage: raids.look <raider>");
        return;
    }
    if (!c.walls.on || !raids::walls_available()) {
        EMF_REPLY("Raiders break walls is off (or EMF is older than 0.6.14)");
        return;
    }
    if (g_director.phase() != raids::Phase::Fighting || !ours(w, id)) {
        EMF_REPLY("%u is no raider of a raid that fights now", id);
        return;
    }
    const auto p = pos_of(w, id);
    if (!p) return;
    wall_look(w, id, *p, c);
    EMF_REPLY("raider %u looks for a wall or a closed gate toward the altar (the log says what it found)", id);
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
        wall_forget(e.deadEntityId.id);
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
    command("raids.look", "<raider>: it looks for a wall or a closed gate toward the altar now, as a held-up raider "
            "does (host, for tests)", cmd_look, 0);
    emf::on_tick(on_tick);
    const Config c = config();
    EMF_LOG(kMod, "ready: %d%% chance per night at %.1f h, %d wave(s) of %d; off in each world until raids.world on",
            c.s.chance, c.s.start_hour, c.s.waves, c.s.per_wave);
    return true;
}
