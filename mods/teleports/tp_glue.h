#pragma once
// Waypoint Teleports' game side, kept apart from teleports.cpp so tests can run it over a fake API: tp::Game over EMF
// (positions, death, the shroud, spawn points, the item cost, EMF's teleport) and the lines for players.
//
// The shroud (static RE of build 1273043, game and server; docs/engine_parts/re_teleport.md section 9):
// vanilla's fast-travel ban (0x140e793d0, called by player_control_action_sequence) blocks when
// EnvironmentSensor.fogType is Dangerous (2) or Death (3) and the player's actor state lacks flag 14 (InSaveZone, set
// inside a spawn point's area). The state is Actor.currentState, or while an action sequence plays (playbackContext.state
// bit 0) (currentState | setStateMask) & ~unsetStateMask, the game's own "actor has state".
// Spawn points: the player's own is SelectedTargets slot 9 (emf::player_spawn_point); else the nearest flame altar.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "emf/query.hpp"
#include "emf/teleport.hpp"
#include "tp_logic.h"

namespace tp {

namespace gen = emf::gen::ecs;
constexpr uint64_t kInSaveZone = 1ull << 14;
constexpr double kAltarSearchMetres = 20000.0;  // the whole world

inline uint64_t actor_state(const gen::Actor& a) {
    uint64_t s = a.currentState;
    const auto& p = a.currentAction.playbackContext;
    if (p.state & 1) s = (s | p.setStateMask) & ~p.unsetStateMask;
    return s;
}

// Inside the shroud as the game's fast-travel ban sees it (see above). False when the components are not there.
inline bool in_shroud(emf::World w, uint32_t player) {
    const auto* es = w.get<gen::EnvironmentSensor>(player);
    if (!es || static_cast<uint8_t>(es->fogType) < static_cast<uint8_t>(emf::gen::FogType::Dangerous)) return false;
    const auto* a = w.get<gen::Actor>(player);
    return !(a && (actor_state(*a) & kInSaveZone));
}

// Dead: the health attribute as the game reads it (a player in the server world has Health but no NetworkHealth: the
// same rule as EMF's core), else the replicated NetworkHealth.
inline bool dead(emf::World w, uint32_t player) {
    if (const auto hp = emf::attribute(w, player, EMF_ATTR_HEALTH)) return *hp <= 0;
    if (const auto* h = w.get<gen::NetworkHealth>(player)) return h->health == 0;
    return false;
}

inline std::optional<Vec> where_is(emf::World w, uint32_t entity) {
    const auto p = emf::position(w, entity);
    if (!p) return std::nullopt;
    return Vec{p->x, p->y, p->z};
}

// The nearest flame altar (a spawn point of a base) to `at`, else the nearest spawn point of any kind; 0 if none.
inline uint32_t nearest_altar(emf::World w, const Vec& at) {
    const auto all = emf::spawn_points_near(w, {at.x, at.y, at.z}, kAltarSearchMetres);
    for (const auto& n : all) {
        const auto* sp = w.get<gen::PlayerSpawnPoint>(n.id);
        if (sp && sp->target == gen::PlayerSpawnType::Base) return n.id;
    }
    return all.empty() ? 0 : all.front().id;
}

// The player's own spawn point, else the nearest flame altar.
inline uint32_t spawn_point_of(emf::World w, uint32_t player) {
    if (const uint32_t own = emf::player_spawn_point(w, player)) return own;
    const auto at = where_is(w, player);
    return at ? nearest_altar(w, *at) : 0;
}

// tp::Game over the EMF API. Every call happens on the server world's thread inside a callback; set_world() names the
// world of that callback (on_tick, a command, an event, a teleport's or a take's answer).
class ApiGame : public Game {
public:
    explicit ApiGame(const char* mod) : mod_(mod) {}
    void set_world(emf::World w) { w_ = w; }
    emf::World world() const { return w_; }
    // The clock (tests replace it).
    std::function<uint64_t()> clock;

    uint64_t now_ms() override { return clock ? clock() : 0; }
    std::vector<uint32_t> players() override { return emf::players(w_); }
    std::vector<uint32_t> players_named(const std::string& name) override {
        std::vector<uint32_t> out;
        const std::string want = lower(name);
        for (uint32_t id : emf::players(w_))
            if (const auto n = emf::player_name(w_, id); n && lower(*n) == want) out.push_back(id);
        return out;
    }
    std::string name_of(uint32_t player) override { return emf::player_name(w_, player).value_or(""); }
    bool names_available() override { return emf::has_v19() && emf::api()->player_name; }
    uint32_t session() override { return emf::server_session(); }
    std::optional<Vec> position(uint32_t p) override { return where_is(w_, p); }
    uint64_t account(uint32_t p) override {
        const uint64_t a = emf::player_account(w_, p);
        return a ? a : 0x100000000ull + p;  // no account known (a test world): the entity stands in for the session
    }
    bool dead(uint32_t p) override { return tp::dead(w_, p); }
    bool in_shroud(uint32_t p) override { return tp::in_shroud(w_, p); }
    uint32_t spawn_point(uint32_t p) override { return spawn_point_of(w_, p); }

    bool teleport(uint32_t p, const TeleportAsk& ask, std::function<void(const Outcome&)> done) override {
        emf::TeleportOptions o;
        o.ground = ask.ground;
        o.spawn_point = ask.spawn_point;
        return emf::teleport(w_, p, {ask.to.x, ask.to.y, ask.to.z}, o,
                             [this, done = std::move(done)](emf::World w, const emf::TeleportResult& r) {
                                 set_world(w);
                                 Outcome out;
                                 out.moved = r.moved();
                                 out.pos = {r.pos.x, r.pos.y, r.pos.z};
                                 out.raw_target = r.raw_target;
                                 out.reason = r.reason;
                                 done(out);
                             });
    }

    bool take_cost(uint32_t p, const std::string& item, int count, std::function<void(bool)> done) override {
        const uint32_t id = emf::api()->find_item(w_.raw(), item.c_str());
        if (!id) {
            log("the /home cost item '" + item + "' is not an item name (the items command lists them)");
            return false;
        }
        const auto n = static_cast<uint32_t>(count);
        return emf::take_items(w_, p, id, n, 0, [this, n, done = std::move(done)](emf::World w, uint32_t taken) {
            set_world(w);
            done(taken == n);
        });
    }

    // Gives back exactly what a /home took: into the player's inventory; what does not fit at their feet; a player who
    // left gets it dropped at the nearest other player (from where they stood); nobody: logged LOST. An answer that
    // comes in another session gives nothing (the take may not have been saved).
    void give_back(uint32_t p, const std::string& item, int count) override {
        const uint32_t id = emf::api()->find_item(w_.raw(), item.c_str());
        const auto n = static_cast<uint32_t>(count);
        if (!id || !n) {
            log("LOST: " + std::to_string(count) + " x " + item + " for player " + std::to_string(p) + " (not an item name)");
            return;
        }
        const auto at = where_is(w_, p);
        const Vec from = at.value_or(Vec{});
        const bool known = at.has_value();
        const uint32_t session = emf::server_session();
        if (emf::put_items_ex(w_, p, id, n, 0, 0xFF, [this, p, id, n, item, from, known, session](emf::World w, uint32_t added) {
                if (emf::server_session() != session) {
                    log("not given back to player " + std::to_string(p) + ": the session ended (" + std::to_string(n) +
                        " x " + item + ")");
                    return;
                }
                if (added < n) drop(w, p, id, n - added, item, from, known);
            }))
            return;
        drop(w_, p, id, n, item, from, known);
    }

    void tell(uint32_t p, const std::string& text) override {
        EMF_LOG(mod_, "to player %u: %s", p, text.c_str());
        // A chat line only they see (API v19; the host of a game you host gets it on screen). Without players' chat the
        // host still gets a note; a guest then sees only the command's answer and the teleport itself.
        if (p && emf::tell(w_, p, text)) return;
        if (p && p == emf::local_player()) EMF_NOTIFY("%s", text.c_str());
    }
    void log(const std::string& text) override { EMF_LOG(mod_, "%s", text.c_str()); }

private:
    // The rest of a refund: at the player's feet, else (they left) at the nearest other player, else logged LOST.
    void drop(emf::World w, uint32_t p, uint32_t id, uint32_t n, const std::string& item, const Vec& from, bool known) {
        if (emf::api()->give_item(w.raw(), p, id, n)) return;
        std::vector<std::pair<double, uint32_t>> closest;
        for (uint32_t q : emf::players(w))
            if (q != p)
                if (const auto pos = where_is(w, q)) closest.push_back({known ? dist(*pos, from) : 0.0, q});
        std::sort(closest.begin(), closest.end());
        for (const auto& [d, q] : closest)
            if (emf::api()->give_item(w.raw(), q, id, n)) {
                log(std::to_string(n) + " x " + item + " for player " + std::to_string(p) + " (who left) dropped at player " +
                    std::to_string(q));
                return;
            }
        log("LOST: " + std::to_string(n) + " x " + item + " for player " + std::to_string(p) +
            " could not be given back (nobody to drop it at)");
    }

    const char* mod_;
    emf::World w_;
};

}  // namespace tp
