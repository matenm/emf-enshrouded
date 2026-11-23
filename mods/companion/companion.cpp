// companion: an EMF mod. The order wheel key (F6 by default; host) spawns a companion next to you that follows you and
// fights at your side, through the game's own guard system (StartGuardingEvent); the same key then opens its order
// wheel. Every player in the world can have their own (owners.h): on a game you host your guests too, on a dedicated
// server every player; they order theirs with `companion.as` from the console until their own game sends their keys.
// Needs
// emf/config/companion.ini:
//   [companion]
//   enabled=1
//   guid=d5d05217-52dc-467a-9192-26b4f661d8d9   (default: Enemy_Vukah_Melee, a melee Vukah warrior)
//   faction=11          (default 11 = PlayerAlly: written on the companion right after it spawns, so it guards you and
//                        fights at full damage; 7 = Player hits for 0; -1 keeps the template's faction)
//   auto_summon=0       (1: every player in a world this game or server hosts gets one by itself)
//   respawn_seconds=0   (N: after it dies, a new one comes N seconds later; 0: press the order wheel key)
//   follow_distance=6   (optional, metres: how close it stays; changes the TEMPLATE's guardSetup, so with an enemy body
//                        every wild Vukah of that template in this session uses it too)
//   key=F6              (the order wheel: Follow me / Stay here / Attack / Gear / Dismiss; with no companion out it
//                        summons one. Every key is a setting: the mod has no key of its own)
//   key_follow= key_stay= key_attack= key_dismiss=   (optional single-key orders; empty = off)
//   marker_guid=0ce28db1-522f-4455-b358-3888fb5e089b (what "Stay here" puts down for it to guard: a small barrel)
//   gear_bonus_max=10   (the most levels gear can add on top of the player's level)
//   max_companions=8    (companions out at once in the world, every player's together; 0 = no limit)
// Plain enemy01 actors without PlayerInput work as templates; see docs/engine_parts/re_companions.md. The game does
// not save the companions; the mod writes whether one is out, its order and its gear to
// emf/config/companion/<world slot>.txt (the host's) or <world slot>-<account>.txt (another player's) and brings it
// back when that world is loaded again (reload or restart) or the player joins again.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "companion_logic.h"
#include "emf/aim.hpp"
#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "emf/keys.hpp"
#include "emf/query.hpp"
#include "emf/teleport.hpp"
#include "gear.h"
#include "orders.h"
#include "owners.h"

namespace gen = emf::gen;
using companion::Owner;

namespace {
constexpr const char* kMod = "companion";
constexpr const char* kDefaultGuid = "d5d05217-52dc-467a-9192-26b4f661d8d9";  // Enemy_Vukah_Melee
constexpr double kFixed = 4294967296.0;                                        // WorldPosition: 32.32 fixed point
constexpr int kActorReadyForGameplay = 50;                                     // keen::actor::StateFlag

uint8_t g_guid[16] = {};
uint64_t g_last_check_ms = 0;
float g_follow_distance = 0.0f;   // 0 = the template's own guard distances
int g_faction = 11;               // keen FactionType written on the companion (-1 = keep the template's)
companion::SummonPolicy g_policy_defaults;  // auto_summon and respawn_ms from the ini: each player's policy starts so
companion::CatchUp g_catch_defaults;        // follow_distance's limits: each player's catch-up starts so
uint32_t g_gear_bonus_max = 10;    // ini gear_bonus_max
uint32_t g_max_companions = 8;     // ini max_companions (0: no limit)
bool g_dedicated = false;          // inside enshrouded_server.exe: nobody is the host
std::atomic<uint16_t> g_slot{0};   // world slot of the loaded save (0 = none yet this session)
std::atomic<uint32_t> g_loaded_session{0};  // the server session whose save has loaded (on_load)
uint32_t g_owners_session = 0;     // the session the owners belong to (server tick only)
uint64_t g_session_start_ms = 0;   // when the server tick first saw that session
std::set<uint64_t> g_was_out;      // accounts whose companion was out when the last session ended (server tick only)
companion::Owners g_owners;        // one set per player (owners.h)
uint8_t g_marker_guid[16] = {};    // what "Stay here" spawns for the companion to guard
constexpr const char* kDefaultMarker = "0ce28db1-522f-4455-b358-3888fb5e089b";  // Prop_Deco_Steppeprops_Barrel_01
constexpr uint32_t kActiveNpcStateHash = 0x1a8b5909u;  // villagers' template data
constexpr int kFactionPlayer = 7, kFactionPlayerAlly = 11;
struct Hotkey {
    uint32_t vk = 0;
    std::string name;
    emf::keys::Edge edge;
};
Hotkey g_key_wheel, g_key_follow, g_key_stay, g_key_attack, g_key_dismiss;
// What calls the companion, for notes and log lines: the order wheel key's name.
std::string call_key() { return companion::call_key_text(g_key_wheel.vk ? g_key_wheel.name : std::string()); }
std::mutex g_m;                    // guards the owners' threat boxes and g_dead (server tick <-> the program's thread)
companion::DeathLog g_dead;       // entities whose EntityDiedEvent was seen, per session (attack targets end on them)
uint32_t g_client_session = 0;     // the session the owners' client checks belong to (client tick only)

std::string format(const char* fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    return b;
}
// A note for the companion's player: on the host's screen as before, for another player a line only they see.
void note(emf::World w, const Owner& o, const std::string& text) {
    if (o.host) {
        emf::api()->notify("%s", text.c_str());
        return;
    }
    if (!o.player) return;
    const std::string head = "Companion: ";
    emf::tell(w, o.player, "[Companion] " + (text.rfind(head, 0) == 0 ? text.substr(head.size()) : text), true);
}

// Writes the configured faction on the companion in world `w` (server: authoritative; client: what your own screen
// shows). Logs each change.
void apply_faction(emf::World w, Owner& o, uint32_t id, const char* world_name) {
    auto* f = w.get<gen::ecs::Faction>(id);
    if (!f) {
        if (g_faction >= 0 && w.is_server() && o.no_faction_said != id) {
            o.no_faction_said = id;
            EMF_LOG(kMod, "companion %u has no Faction component: faction=%d cannot be applied to this body", id,
                    g_faction);
        }
        return;
    }
    const auto value = companion::faction_to_write(g_faction, static_cast<uint8_t>(f->faction));
    if (!value) return;
    const int before = static_cast<int>(f->faction);
    f->faction = static_cast<gen::FactionType>(*value);
    EMF_LOG(kMod, "companion %u: faction %d -> %d in the %s world", id, before, static_cast<int>(*value), world_name);
}

// Logs the companion template's guard distances and, with follow_distance set, changes them (template data: shared
// by every entity of the template in this world, until the game restarts).
void apply_follow(emf::World w, Owner& o, uint32_t id) {
    auto* tpl = w.get_template<gen::ecs::Enemy01BehaviorState>(id);
    if (!tpl) {
        EMF_LOG(kMod, "companion %u: no Enemy01BehaviorState template data (get_template_component)", id);
        o.follow_applied = id;
        return;
    }
    const auto& g = tpl->guardSetup;
    EMF_LOG(kMod, "companion %u template guardSetup: start %.1f m, target %.1f m, arrive %.1f m, canGuardPlayer %d", id,
            g.startDistance, g.targetDistance, g.maxArriveDistance, g.canGuardPlayer ? 1 : 0);
    float start = g.startDistance, target = g.targetDistance;
    if (companion::apply_follow_distance(g_follow_distance, &start, &target)) {
        if (!emf::write_value(&g.startDistance, start) || !emf::write_value(&g.targetDistance, target)) {
            EMF_LOG(kMod, "companion %u: could not write the template data; follow_distance not applied", id);
        } else {
            EMF_LOG(kMod, "companion %u: follow_distance %.1f m -> start %.1f m, target %.1f m", id, g_follow_distance,
                    g.startDistance, g.targetDistance);
        }
    }
    o.follow_applied = id;
}

bool entity_pos(emf::World w, uint32_t id, double out[3]) {
    auto* t = w.get<gen::ecs::CurrentTransform>(id);
    if (!t) return false;
    out[0] = t->transform.position.x / kFixed;
    out[1] = t->transform.position.y / kFixed;
    out[2] = t->transform.position.z / kFixed;
    return true;
}

double distance(const double a[3], const double b[3]) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ECS program on the server world (game worker thread): sends each player's requested events, watches deaths.
void guard_program(void* ctx) {
    const uint32_t session = emf::server_session();
    auto lock = g_owners.lock();
    g_owners.each([&](Owner& o) {
        uint32_t guard = 0, guarded = 0;
        if (o.companion.take_start(&guard, &guarded)) {
            if (auto* e = emf::emit<gen::ecs::StartGuardingEvent>(ctx)) {
                e->guardingEntityId.id = guard;
                e->guardedEntityId.id = guarded;
                EMF_LOG(kMod, "StartGuardingEvent sent: entity %u guards %u", guard, guarded);
            } else {
                EMF_LOG(kMod, "StartGuardingEvent could not be sent (see the core log)");
            }
        }
        if (o.companion.take_stop(&guard, &guarded)) {
            if (auto* e = emf::emit<gen::ecs::StopGuardingEvent>(ctx)) {
                e->guardingEntityId.id = guard;
                e->guardedEntityId.id = guarded;
                EMF_LOG(kMod, "StopGuardingEvent sent: entity %u stops guarding %u", guard, guarded);
            }
        }
        const uint32_t mine = o.companion.entity();
        if (!mine) return;
        std::vector<uint32_t> threats;
        {
            std::lock_guard<std::mutex> l(g_m);
            threats = o.threats.take(mine, session);  // a target queued for an earlier companion or session is dropped
        }
        for (uint32_t target : threats)
            if (auto* e = emf::emit<gen::ecs::AddThreatTargetEvent>(ctx)) {
                e->attackerId.id = mine;
                e->targetId.id = target;
                e->threatValue = 1000.0f;
            }
    });
    emf::read<gen::ecs::EntityDiedEvent>(ctx, [session](const gen::ecs::EntityDiedEvent& e) {
        const uint32_t dead = e.deadEntityId.id;
        Owner* o = g_owners.owner_of_companion(dead);
        if (!o) {
            std::lock_guard<std::mutex> l(g_m);
            g_dead.record(session, dead);
            return;
        }
        o->companion.on_died(dead);
        o->died = true;
        EMF_LOG(kMod, "companion %u died", dead);
    });
}

enum class Summon { Ok, NoPlayer, Failed, Limit };

// Spawns a companion for `o`'s player and makes it an ally: at `at` (metres), else 2 m from the player. `by_hand`:
// the player called it. NoPlayer (logged when `loud`) while the player's position is unknown, Failed (logged) when the
// game refused the spawn, Limit (a note when `loud`) when the world holds max_companions already.
Summon summon(emf::World w, Owner& o, uint64_t now, const char* why, bool loud,
              const std::optional<emf::aim::Vec3>& at = std::nullopt, bool by_hand = false) {
    const uint32_t player = o.player;
    double p[3];
    if (!player || !entity_pos(w, player, p)) {
        if (loud) EMF_LOG(kMod, "%s: player position unknown", why);
        return Summon::NoPlayer;
    }
    if (!g_owners.may_summon(g_max_companions, o.policy.coming())) {
        EMF_LOG(kMod, "%s: %u companions are out in this world (max_companions): none for player %u", why,
                g_max_companions, player);
        if (loud) note(w, o, format("Companion: this world allows %u companions at once", g_max_companions));
        return Summon::Limit;
    }
    const emf::aim::Vec3 spot = at ? *at : emf::aim::Vec3{p[0] + 2.0, p[1], p[2]};
    const uint32_t id = emf::api()->spawn(w.raw(), g_guid, spot.x, spot.y, spot.z);
    if (!id) {
        EMF_LOG(kMod, "%s: spawn failed (see the core log)", why);
        return Summon::Failed;
    }
    apply_faction(w, o, id, "server");  // before its AI picks targets
    o.body_tpl = reinterpret_cast<uintptr_t>(
        emf::api()->get_template_component(w.raw(), gen::ecs::Enemy01BehaviorState::kHash, id, nullptr));
    const auto* f = w.get<gen::ecs::Faction>(id);
    o.body_faction = f ? static_cast<int>(f->faction) : -1;
    o.companion.on_spawned(id, player, now, reinterpret_cast<uintptr_t>(w.raw()));
    o.policy.on_summoned(by_hand);
    o.catch_up.reset();
    o.orders.on_companion_spawned();  // the current order (e.g. stay) applies to the new companion too
    EMF_LOG(kMod, "%s: companion %u spawned at (%.1f, %.1f, %.1f), player %u at (%.1f, %.1f, %.1f)", why, id, spot.x,
            spot.y, spot.z, player, p[0], p[1], p[2]);
    return Summon::Ok;
}

// The companion is gone without dying (or could not come back): it comes back by itself (where: come_back()), or stays
// away after too many losses. One note for the player either way.
void lose(emf::World w, Owner& o, uint64_t now, uint32_t id, companion::LostNotice what, const char* how) {
    const bool armed = o.policy.on_lost(now);
    const bool staying = o.orders.mode() == companion::Mode::Stay;
    const std::string key = call_key();
    EMF_LOG(kMod, "companion %u %s: %s", id, how,
            !armed ? "lost too often in 2 minutes, it stays away until called"
                   : staying ? "it comes back at its stay spot once you are near it" : "a new one comes to you");
    note(w, o, companion::lost_notice(what, armed, staying, key));
}

// A new companion in place of the current one, on the player's own spot (its gear and order stay: they are the
// player's). Said once when it cannot come now; it then comes back as a lost one.
void replace_companion(emf::World w, Owner& o, uint64_t now) {
    const uint32_t old = o.companion.entity();
    if (!old) return;
    emf::api()->destroy(w.raw(), old);
    o.companion.on_gone();
    double p[3];
    std::optional<emf::aim::Vec3> at;
    if (entity_pos(w, o.player, p)) at = companion::return_spot({p[0], p[1], p[2]});
    if (summon(w, o, now, "catch-up", true, at) != Summon::Ok)
        lose(w, o, now, old, companion::LostNotice::ReplaceFailed, "could not be replaced next to you");
}

// Moves the companion next to its player (left behind or stuck): the game's own teleport (API v20), near the player so
// the game keeps it. On an older core, or when the teleport fails, a new companion comes in its place.
void catch_up(emf::World w, Owner& o, uint64_t now, uint32_t id, const double from[3], const double player[3]) {
    EMF_LOG(kMod, "companion %u is %.0f m from its player (left behind or stuck): bringing it to them", id,
            distance(from, player));
    emf::TeleportOptions opts;
    opts.ground = true;
    const uint32_t session = emf::server_session();
    const uint64_t account = o.account;
    const bool queued = emf::teleport(w, id, companion::catch_up_target({player[0], player[1], player[2]}), opts,
                                      [id, session, account](emf::World w2, const emf::TeleportResult& r) {
        Owner* me = g_owners.find(account);
        if (r.session_ended || session != emf::server_session() || !me || me->companion.entity() != id) return;
        if (r.moved()) {
            EMF_LOG(kMod, "companion %u is with its player again (%.1f m from the spot)", id, r.distance);
            return;
        }
        EMF_LOG(kMod, "companion %u could not be moved (%s): a new one comes in its place", id, r.reason.c_str());
        replace_companion(w2, *me, GetTickCount64());
    });
    if (!queued) replace_companion(w, o, now);
}

void summon_or_dismiss(emf::World w, Owner& o, uint64_t now, const char* why) {
    if (o.companion.phase() == companion::Companion::Phase::Active) {
        o.companion.request_stop();
        o.policy.on_dismissed();
        if (const uint32_t marker = o.orders.release_marker())  // a stay marker goes with it, now (its id is still ours)
            emf::api()->destroy(w.raw(), marker);
        EMF_LOG(kMod, "%s: dismissing companion %u", why, o.companion.entity());
        return;
    }
    if (o.companion.phase() != companion::Companion::Phase::None) return;  // a dismissal is in progress
    summon(w, o, now, why, true, std::nullopt, true);
}

void check_companion(emf::World w, Owner& o, uint64_t now) {
    const uint32_t id = o.companion.entity();
    if (!id || o.companion.phase() != companion::Companion::Phase::Active) return;
    auto* ai = w.get<gen::ecs::DynamicEnemy01BehaviorState>(id);
    companion::BodySeen seen;  // a death resets the companion first (the program reads EntityDiedEvent)
    seen.exists = w.get<gen::ecs::CurrentTransform>(id) != nullptr;
    seen.tpl = reinterpret_cast<uintptr_t>(
        emf::api()->get_template_component(w.raw(), gen::ecs::Enemy01BehaviorState::kHash, id, nullptr));
    if (const auto* f = w.get<gen::ecs::Faction>(id)) seen.faction = static_cast<int>(f->faction);
    seen.ai = ai != nullptr;
    if (const auto* nh = w.get<gen::ecs::NetworkHealth>(id)) seen.health = nh->health;
    else if (const auto* h = w.get<gen::ecs::Health>(id)) seen.health = h->dataStorage[0];
    switch (companion::presence(seen, o.body_tpl, o.body_faction)) {
        case companion::Presence::Ours: break;
        case companion::Presence::Dead: {
            o.companion.on_gone();
            const bool again = o.policy.on_died(now);  // its body is there, dead: a death after all
            EMF_LOG(kMod, "companion %u died: %s", id,
                    companion::death_message(again, o.policy.respawn_ms / 1000ull, call_key()).c_str());
            return;
        }
        case companion::Presence::Gone:
            o.companion.on_gone();
            lose(w, o, now, id, companion::LostNotice::Lost,
                 "is gone (the game removes a companion left far from every player)");
            return;
        case companion::Presence::Taken:  // never touched: another creature has the id now
            o.companion.on_gone();
            lose(w, o, now, id, companion::LostNotice::Lost, "is gone (its id holds another creature now, left alone)");
            return;
        case companion::Presence::Body:
            emf::api()->destroy(w.raw(), id);  // our body without its AI: removed, so no second one stands around
            o.companion.on_gone();
            lose(w, o, now, id, companion::LostNotice::Lost, "lost its AI (its body was removed)");
            return;
    }
    if (o.companion.on_player(o.player, now))
        EMF_LOG(kMod, "its player is entity %u now: companion %u guards it", o.companion.player(), id);
    apply_faction(w, o, id, "server");  // in case the game set it back
    // An "automated player" template (Player_Vukah_Melee) has PlayerInput. The game marks such an actor ready for
    // gameplay (Actor IsReadyForGameplay, without which its AI never leaves Spawning) only once that input says the
    // client is ready. There is no client, so say it for it.
    if (auto* input = w.get<gen::ecs::PlayerInput>(id); input && !input->fromClientIsReadyForGameplay) {
        input->fromClientIsReadyForGameplay = true;
        EMF_LOG(kMod, "companion %u has player input: marked it ready for gameplay", id);
    }
    if (o.follow_applied != id) apply_follow(w, o, id);
    const uint32_t target = ai->guardEntityId.id;
    if (o.companion.wants_regard(target, now))
        EMF_LOG(kMod, "companion %u guards %u, not its player: sending StartGuardingEvent again", id, target);
    double at[3], you[3];
    if (entity_pos(w, id, at) && entity_pos(w, o.companion.player(), you)) {
        companion::CatchUp::Look look;
        look.now_ms = now;
        look.companion = {at[0], at[1], at[2]};
        look.player = {you[0], you[1], you[2]};
        look.following = o.orders.mode() != companion::Mode::Stay;
        look.fighting = ai->state == gen::enemy01::State::Combat;
        if (o.catch_up.tick(look)) {
            catch_up(w, o, now, id, at, you);
            return;
        }
    }
    if (now - o.last_report_ms >= 5000) {
        o.last_report_ms = now;
        double a[3], b[3];
        const uint32_t player = o.companion.player();
        const bool have = entity_pos(w, id, a) && entity_pos(w, player, b);
        auto* actor = w.get<gen::ecs::Actor>(id);
        const bool ready = actor && ((actor->currentState >> kActorReadyForGameplay) & 1);
        EMF_LOG(kMod, "companion %u: guards %u, AI state %u, %s, %.1f m from player %u", id, target,
                static_cast<unsigned>(ai->state), ready ? "ready" : "NOT ready for gameplay", have ? distance(a, b) : -1.0,
                player);
    }
}

// Client world (the host's own screen; entity ids are shared with the server world): the faction each companion's
// client copy has; written there too when it differs, so your own targeting and health bars treat it as an ally.
void check_client(emf::World w) {
    const uint32_t session = emf::server_session();
    const bool fresh = session != g_client_session;  // ids are reused across sessions
    g_client_session = session;
    auto lock = g_owners.lock();
    g_owners.each([&](Owner& o) {
        if (fresh) o.client_checked = 0;
        const uint32_t id = o.companion.entity();
        if (!id || id == o.client_checked) return;
        auto* f = w.get<gen::ecs::Faction>(id);
        if (!f) return;  // not replicated to the client world yet
        o.client_checked = id;
        EMF_LOG(kMod, "companion %u: client world faction %d", id, static_cast<int>(f->faction));
        apply_faction(w, o, id, "client");
    });
}

std::wstring state_dir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    return dir.substr(0, dir.find_last_of(L"\\/")) + L"\\emf\\config\\companion";
}
std::wstring state_file(uint16_t slot, const Owner& o) {
    return state_dir() + L"\\" + companion::Owners::file_name(slot, o.account, o.host);
}
std::string read_file(const std::wstring& path) {
    std::string text;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
    }
    return text;
}
// Writes a player's file when its contents changed (server tick, every second, and after a give or a take).
void save_state(Owner& o) {
    const uint16_t slot = g_slot;
    if (!o.loaded || !slot) return;  // never another world's file, nor one not read yet
    const std::string text = companion::serialize(o.saved());
    if (text == o.saved_text) return;
    CreateDirectoryW(state_dir().c_str(), nullptr);
    if (companion::write_text_atomic(state_file(slot, o), text))
        o.saved_text = text;
    else
        EMF_LOG(kMod, "could not write %ls", state_file(slot, o).c_str());
}
// Once per session for each player, when the world's save has loaded and they are in it: their file decides gear,
// order and whether one comes back.
void load_state(Owner& o, uint64_t now) {
    const uint16_t slot = g_slot;
    const std::string text = slot ? read_file(state_file(slot, o)) : std::string();
    o.saved_text = text;
    const auto s = companion::parse_saved(text);
    o.load(s, now);
    if (!text.empty())
        EMF_LOG(kMod, "world slot %04x, %s: %s, order %s, %zu gear piece(s)", slot,
                o.host ? "the host" : format("account %016llx", static_cast<unsigned long long>(o.account)).c_str(),
                s.out ? "a companion was out" : "none out", s.order.c_str(), o.gear.held().size());
}

uint32_t player_level(emf::World w, const Owner& o) {
    auto* l = w.get<gen::ecs::Level>(o.player);
    return l ? l->dataStorage[0] : 1;
}
// The companion's level: its player's level plus its gear bonus (written only on the player's side).
void apply_level(emf::World w, Owner& o) {
    const uint32_t id = o.companion.entity();
    auto* l = w.get<gen::ecs::Level>(id);
    auto* f = w.get<gen::ecs::Faction>(id);
    auto* pl_level = w.get<gen::ecs::Level>(o.player);
    if (!l || !pl_level) return;  // the player's level is not readable (yet): keep the companion's as it is
    const uint32_t pl = pl_level->dataStorage[0];
    const uint32_t target = companion::companion_level(pl, o.gear.bonus(pl, g_gear_bonus_max));
    const bool ally = f && static_cast<int>(f->faction) == kFactionPlayerAlly;
    const auto value = companion::level_to_write(l->dataStorage[0], target, ally);
    if (!value) return;
    const uint32_t before = l->dataStorage[0];
    l->dataStorage[0] = *value;
    if (auto* d = w.get<gen::ecs::DynamicEnemyScaling>(id)) d->nextUpdateTime.value = 0;  // rescale health now
    EMF_LOG(kMod, "companion %u: level %u -> %u (player %u + gear %u)", id, before, *value, pl,
            o.gear.bonus(pl, g_gear_bonus_max));
}

// Runs the order logic's actions (server tick).
void run_orders(emf::World w, Owner& o, uint64_t now) {
    double pp[3] = {};
    if (!o.player || !entity_pos(w, o.player, pp)) return;
    const auto actions = o.orders.tick(now, {pp[0], pp[1], pp[2]}, [&](uint32_t id) -> std::optional<companion::EntityState> {
        double p[3];
        if (!id || !entity_pos(w, id, p)) return std::nullopt;
        std::lock_guard<std::mutex> l(g_m);
        return companion::EntityState{{p[0], p[1], p[2]}, !g_dead.dead(emf::server_session(), id)};
    });
    for (const auto& a : actions) {
        switch (a.kind) {
            case companion::OrderAction::GuardPlayer: o.companion.request_guard(0, now); break;
            case companion::OrderAction::GuardEntity: o.companion.request_guard(a.id, now); break;
            case companion::OrderAction::SpawnMarker: {
                const uint32_t id = emf::api()->spawn(w.raw(), g_marker_guid, a.pos.x, a.pos.y, a.pos.z);
                if (id) {
                    o.orders.on_marker_spawned(id);
                } else {
                    EMF_LOG(kMod, "the stay marker could not be spawned (marker_guid?): following instead");
                    o.orders.follow();
                }
                break;
            }
            case companion::OrderAction::DestroyMarker: emf::api()->destroy(w.raw(), a.id); break;
            case companion::OrderAction::Threat: {
                std::lock_guard<std::mutex> l(g_m);
                o.threats.push(o.companion.entity(), emf::server_session(), a.id);
                break;
            }
            case companion::OrderAction::Notice: note(w, o, a.text); break;
        }
    }
}

// The creature under the player's crosshair within 40 m that the companion may attack: has Health, is not a player,
// the companion, a villager, or on the players' side.
std::optional<uint32_t> attack_target(emf::World w, const Owner& o) {
    const uint32_t player = o.player;
    double pp[3];
    if (!player || !entity_pos(w, player, pp)) return std::nullopt;
    const auto ray = emf::aim::camera_ray(w, player);
    if (!ray) return std::nullopt;
    const emf::aim::Vec3 from{pp[0], pp[1], pp[2]};
    std::vector<uint32_t> ids;
    std::vector<emf::aim::Vec3> points;
    const uint32_t mine = o.companion.entity();
    w.for_each_entity([&](uint32_t id) {
        if (id == player || id == mine) return;
        double p[3];
        if (!entity_pos(w, id, p)) return;
        const emf::aim::Vec3 at{p[0], p[1] + 0.8, p[2]};
        if (emf::aim::length(emf::aim::sub(at, from)) > companion::Orders::kAttackRange) return;
        if (!w.get<gen::ecs::Health>(id)) return;
        if (auto* f = w.get<gen::ecs::Faction>(id)) {
            const int fac = static_cast<int>(f->faction);
            if (fac == kFactionPlayer || fac == kFactionPlayerAlly) return;
        }
        if (emf::api()->get_template_component(w.raw(), kActiveNpcStateHash, id, nullptr)) return;  // a villager
        ids.push_back(id);
        points.push_back(at);
    });
    const auto i = emf::aim::pick(*ray, from, points, {companion::Orders::kAttackRange, 1.5, 0.25});
    if (!i) return std::nullopt;
    return ids[*i];
}

void order_follow(emf::World w, Owner& o) {
    o.orders.follow();
    note(w, o, "Companion: following you");
}
void order_stay(emf::World w, Owner& o) {
    double p[3];
    if (!entity_pos(w, o.companion.entity(), p)) {
        note(w, o, "Companion: its position is not known yet: try Stay again in a moment");
        return;
    }
    o.orders.stay({p[0], p[1], p[2]});
    note(w, o, "Companion: staying here");
}
void order_attack(emf::World w, Owner& o, uint64_t now) {
    const auto t = attack_target(w, o);
    if (!t) {
        note(w, o, "Companion: nothing to attack there (look at a creature within 40 m)");
        return;
    }
    o.orders.attack(*t, now);
    note(w, o, format("Companion: attacking entity %u", *t));
}

uint32_t companion_level(emf::World w, const Owner& o) {
    auto* l = w.get<gen::ecs::Level>(o.companion.entity());
    return l ? l->dataStorage[0] : 0;
}

// Gear the player could give: weapons and armour in the backpack / action bar (Generic inventories) whose item could
// come back the same way (no socketed gem, not tinted: the design's rulings).
struct GearCandidate {
    uint32_t item = 0, level = 0, inventory = 0, pide = 0;
    uint8_t rarity = 0, slot_index = 0;
    companion::Slot slot = companion::Slot::Weapon;
    std::string name;
};
constexpr uint16_t kItemTinted = 1u << 9;  // ItemState.itemState (ItemStateFlag::IsTinted)
const char* rarity_name(uint8_t r) {
    static const char* const kNames[] = {"Common", "Uncommon", "Rare", "Epic", "Legendary"};
    return r < 5 ? kNames[r] : "?";
}
std::string item_name(emf::World w, uint32_t item) {
    EmfItemInfo info{};
    info.size = sizeof(info);
    if (emf::api()->item_info(w.raw(), item, &info) && info.name[0]) return info.name;
    char b[16];
    snprintf(b, sizeof(b), "item %08x", item);
    return b;
}
std::vector<GearCandidate> gear_candidates(emf::World w, uint32_t player) {
    std::vector<GearCandidate> out;
    auto* setup = w.get<gen::ecs::InventorySetup>(player);
    if (!setup) return out;
    for (int i = 0; i < 16 && out.size() < companion::kGearWheelPieces; ++i) {
        const uint32_t inv_id = setup->linksEntities[i].id;
        if (!inv_id || setup->linksCategories[i] != gen::InventoryCategory::Generic) continue;
        auto* inv = w.get<gen::ecs::Inventory>(inv_id);
        if (!inv) continue;
        for (uint8_t s = 0; s < 8 && out.size() < companion::kGearWheelPieces; ++s) {
            const auto& stack = inv->slots[s];
            const uint32_t pide = stack.data.pide.id;
            if (!stack.id.value || !pide) continue;
            auto* st = w.get<gen::ecs::ItemState>(pide);
            if (!st || (st->itemState & kItemTinted)) continue;
            if (auto* gem = w.get<gen::ecs::WeaponGemSlot>(pide); gem && gem->weaponGem.id.value) continue;
            GearCandidate c;
            c.name = item_name(w, stack.id.value);
            const auto slot = companion::slot_from_name(c.name);
            if (!slot) continue;
            c.item = stack.id.value;
            c.level = st->itemLevelUi;
            c.rarity = static_cast<uint8_t>(st->itemRarityUi);
            c.slot = *slot;
            c.inventory = inv_id;
            c.slot_index = s;
            c.pide = pide;
            out.push_back(c);
        }
    }
    return out;
}

// What putting a piece back came to, as a note.
void note_put_outcome(emf::World w, const Owner& o, companion::PutOutcome outcome, const std::string& name) {
    switch (outcome) {
        case companion::PutOutcome::Returned: note(w, o, "Companion: " + name + " is back in your inventory"); break;
        case companion::PutOutcome::WornAgain: note(w, o, "Companion: no room for " + name + "; it keeps wearing it"); break;
        case companion::PutOutcome::Waiting:
            note(w, o, "Companion: no room for " + name + "; make room and use Take all gear back");
            break;
        case companion::PutOutcome::Unknown: break;
    }
}
// Puts a returning piece back into the player's inventory as a new item with its level and rarity.
void return_piece(emf::World w, Owner& o, const companion::Piece& p) {
    const uint32_t item = p.item, token = p.token;
    const std::string name = item_name(w, item);
    const uint64_t account = o.account;
    const uint32_t session = emf::server_session();
    const bool queued = emf::put_items_ex(w, o.player, item, 1, p.level, p.rarity,
                                          [token, name, account, session](emf::World w2, uint32_t added) {
                                              // an answer for the session before (EMF answers 0 when the next starts)
                                              // must not touch this session's tokens
                                              if (session != emf::server_session()) return;
                                              Owner* me = g_owners.find(account);
                                              if (!me) return;
                                              note_put_outcome(w2, *me,
                                                               me->gear.on_put_result(token, added > 0, GetTickCount64()),
                                                               name);
                                              save_state(*me);
                                          });
    if (!queued) {  // refused at once (the log says why): not a matter of room
        o.gear.on_put_result(token, false, GetTickCount64());
        note(w, o, "Companion: could not give " + name + " back right now (the log says why); it keeps it");
        save_state(o);
    }
}

// Takes the chosen piece from the player's inventory (that exact slot first); worn once the game confirmed it.
void give_piece(emf::World w, Owner& o, const GearCandidate& c) {
    if (!o.loaded || !g_slot) {
        note(w, o, "Companion: gear cannot be given until this world's companion file is known (see the log)");
        EMF_LOG(kMod, "a give was refused: the world slot is not known (no on_load on this game build?), so it could "
                      "not be saved");
        return;
    }
    auto* inv = w.get<gen::ecs::Inventory>(c.inventory);
    if (!inv || inv->slots[c.slot_index].id.value != c.item || inv->slots[c.slot_index].data.pide.id != c.pide) {
        note(w, o, "Companion: " + c.name + " is no longer in that slot");
        return;
    }
    companion::Piece p;
    p.item = c.item;
    p.level = c.level;
    p.rarity = c.rarity;
    p.slot = c.slot;
    if (!o.gear.give(p, GetTickCount64()).ok) {
        note(w, o, "Companion: already handing over something for that slot");
        return;
    }
    save_state(o);
    const companion::Slot slot = c.slot;
    const std::string name = c.name;
    const uint64_t from = (static_cast<uint64_t>(c.slot_index) << 32) | c.inventory;
    const uint32_t pide = c.pide;
    const uint64_t account = o.account;
    const uint32_t session = emf::server_session();
    const bool queued = emf::take_items(w, o.player, c.item, 1, from, [slot, name, pide, account, session](
                                                                           emf::World w2, uint32_t taken) {
        if (session != emf::server_session()) return;  // an answer for the session before
        Owner* me = g_owners.find(account);
        if (!me) return;
        const auto back = me->gear.on_take_result(slot, taken > 0, GetTickCount64());
        if (taken && pide) {
            me->taken_pide = pide;
            me->taken_check_ms = GetTickCount64() + 3000;
        }
        note(w2, *me, taken ? "Companion: now wears " + name : "Companion: could not take " + name + " from your inventory");
        for (const auto& old : back) return_piece(w2, *me, old);
        save_state(*me);
    }, true);  // that slot only: never another copy of the item
    if (!queued) {
        o.gear.on_take_result(slot, false, GetTickCount64());
        note(w, o, "Companion: taking items is not available (the log says why)");
    }
}

void take_back_gear(emf::World w, Owner& o) {
    const auto back = o.gear.take_back_all(GetTickCount64());
    if (back.empty()) {
        note(w, o, "Companion: it wears no gear");
        return;
    }
    for (const auto& p : back) return_piece(w, o, p);
    save_state(o);
}

// The gear wheel, on the host's screen (the host's own companion).
void open_gear_wheel(emf::World w, Owner& o) {
    const auto cands = gear_candidates(w, o.player);
    std::vector<std::string> pieces;
    for (const auto& c : cands) {
        char b[160];
        snprintf(b, sizeof(b), "%s (level %u, %s)", c.name.c_str(), c.level, rarity_name(c.rarity));
        pieces.push_back(b);
    }
    const std::vector<std::string> labels = companion::gear_wheel_labels(pieces);
    const uint32_t pl = player_level(w, o);
    char title[64];
    snprintf(title, sizeof(title), "Gear (+%u levels, %zu worn)", o.gear.bonus(pl, g_gear_bonus_max), o.gear.held().size());
    const uint64_t account = o.account;
    const bool ok = emf::wheel(title, labels, g_key_wheel.vk, EMF_WORLD_SERVER, [cands, account](emf::World w2, int choice) {
        Owner* me = g_owners.find(account);
        if (choice < 0 || !me) return;
        if (static_cast<size_t>(choice) >= companion::gear_wheel_offered(cands.size())) {
            take_back_gear(w2, *me);
            return;
        }
        give_piece(w2, *me, cands[static_cast<size_t>(choice)]);
    });
    if (!ok) emf::api()->notify("The gear wheel could not open (the log says why)");
}

// The order wheel, on the host's screen (the host's own companion).
void open_order_wheel(emf::World w, Owner& o) {
    char title[64];
    snprintf(title, sizeof(title), "Companion (level %u)", companion_level(w, o));
    const uint64_t account = o.account;
    const bool ok = emf::wheel(title, {"Follow me", "Stay here", "Attack (what you look at)", "Gear", "Dismiss"},
                               g_key_wheel.vk, EMF_WORLD_SERVER, [account](emf::World w2, int choice) {
                                   Owner* me = g_owners.find(account);
                                   if (!me || me->companion.phase() != companion::Companion::Phase::Active) return;
                                   const uint64_t now = GetTickCount64();
                                   switch (choice) {
                                       case 0: order_follow(w2, *me); break;
                                       case 1: order_stay(w2, *me); break;
                                       case 2: order_attack(w2, *me, now); break;
                                       case 3: open_gear_wheel(w2, *me); break;
                                       case 4: summon_or_dismiss(w2, *me, now, "wheel"); break;
                                       default: break;
                                   }
                               });
    if (!ok) emf::api()->notify("The companion wheel could not open (the console may be open; the log says why)");
}

// The host's keys order the host's own companion.
void read_keys(emf::World w, Owner& o, uint64_t now) {
    const bool active = o.companion.phase() == companion::Companion::Phase::Active;
    if (g_key_wheel.edge.pressed(emf::keys::down(g_key_wheel.vk))) {
        switch (companion::wheel_press(o.companion.phase())) {
            case companion::WheelPress::OpenWheel: open_order_wheel(w, o); break;
            case companion::WheelPress::Summon: summon(w, o, now, "wheel key", true, std::nullopt, true); break;
            case companion::WheelPress::Nothing: break;  // a dismissal is under way
        }
    }
    if (g_key_follow.edge.pressed(emf::keys::down(g_key_follow.vk)) && active) order_follow(w, o);
    if (g_key_stay.edge.pressed(emf::keys::down(g_key_stay.vk)) && active) order_stay(w, o);
    if (g_key_attack.edge.pressed(emf::keys::down(g_key_attack.vk)) && active) order_attack(w, o, now);
    if (g_key_dismiss.edge.pressed(emf::keys::down(g_key_dismiss.vk)) && active) summon_or_dismiss(w, o, now, "dismiss key");
}

// The players in the server world now: a set for each one first seen (their file read once the world's save has
// loaded), a new entity for one who came back, and a companion dismissed for one who left (it comes back with them).
void update_players(emf::World w, uint64_t now, uint32_t session) {
    const uint32_t host = g_dedicated ? 0 : emf::local_player();
    const bool world_loaded = g_loaded_session == session;
    const std::vector<uint32_t> players = emf::players(w);
    // On a game, whose file is the old one waits until the host's own player is among the world's players and the save
    // has loaded (or a minute passed without a load notice): right after a reload local_player() may still name the
    // old entity.
    if (!g_dedicated && (!host || std::find(players.begin(), players.end(), host) == players.end() ||
                         (!world_loaded && now - g_session_start_ms < companion::SummonPolicy::kLoadFallbackMs)))
        return;
    std::set<uint64_t> present;
    for (uint32_t p : players) {
        const uint64_t account = emf::player_account(w, p);
        if (!account) continue;
        present.insert(account);
        Owner* o = g_owners.find(account);
        if (!o) {
            o = &g_owners.get(account, p == host, g_policy_defaults, g_catch_defaults);
            o->begin(session, now, g_was_out.count(account) > 0);
            o->player = p;
            EMF_LOG(kMod, "player %u (%s) is in the world", p,
                    o->host ? "the host" : format("account %016llx", static_cast<unsigned long long>(account)).c_str());
        } else if (o->player != p) {
            if (!o->player) {
                o->on_back(p, now);
                EMF_LOG(kMod, "player %u (account %016llx) is back in the world", p, static_cast<unsigned long long>(account));
            } else {
                o->player = p;
            }
        }
        o->seen();
        if (world_loaded && !o->loaded) load_state(*o, now);
    }
    g_owners.each([&](Owner& o) {
        if (!o.player || present.count(o.account) || !o.missing()) return;  // two checks in a row without them
        const uint32_t was = o.player;
        for (const uint32_t marker : o.on_left()) emf::api()->destroy(w.raw(), marker);
        EMF_LOG(kMod, "player %u (account %016llx) left: %s", was, static_cast<unsigned long long>(o.account),
                o.away_out ? "their companion goes and comes back with them" : "no companion out");
        save_state(o);
    });
}

// Each player's set, once a second: the body checks, gear, level, file and a companion coming back.
void check_owner(emf::World w, Owner& o, uint64_t now) {
    check_companion(w, o, now);
    o.gear.tick(now);
    if (o.taken_pide && now >= o.taken_check_ms) {
        const uint32_t pide = o.taken_pide.exchange(0);
        if (w.get<gen::ecs::ItemState>(pide)) {
            EMF_LOG(kMod, "the taken item's data entity %u is still there (the game did not remove it, or the id was "
                          "reused at once)", pide);
            note(w, o, format("Companion: the game kept the taken item's data (entity %u); see the log", pide));
        } else {
            EMF_LOG(kMod, "the taken item's data entity %u was removed by the game", pide);
        }
    }
    if (o.return_after_load && o.player) {
        o.return_after_load = false;
        for (const auto& p : o.gear.unreturned()) return_piece(w, o, p);
    }
    if (o.companion.phase() == companion::Companion::Phase::Active && now - o.last_level_ms >= 2000) {
        o.last_level_ms = now;
        apply_level(w, o);
    }
    save_state(o);
    double you[3];
    const bool here = o.player && entity_pos(w, o.player, you);
    if (here) o.catch_up.watch_player(now, {you[0], you[1], you[2]});
    if (o.companion.phase() == companion::Companion::Phase::None && here) {
        // a lost one comes to its player's own spot once they stand (never into the air beside them while they glide),
        // or, when it was staying, at its stay spot once they are back near it
        const bool lost = o.policy.lost_pending();
        const bool staying = o.orders.mode() == companion::Mode::Stay;
        const emf::aim::Vec3 player{you[0], you[1], you[2]};
        const emf::aim::Vec3 spot = o.orders.stay_pos();
        const auto where =
            companion::come_back(lost, staying, o.catch_up.player_settled(), emf::aim::length(emf::aim::sub(spot, player)));
        std::optional<emf::aim::Vec3> at;
        if (where == companion::ComeBack::AtStaySpot) at = emf::aim::Vec3{spot.x, spot.y + 0.5, spot.z};
        else if (lost) at = companion::return_spot(player);
        if (where != companion::ComeBack::Wait && o.policy.due(now)) {
            const Summon s = summon(w, o, now, !lost ? "auto" : at && staying ? "back at its stay spot" : "back after it got lost",
                                    false, at);
            if (s == Summon::Failed || s == Summon::Limit) o.policy.on_spawn_failed(now);  // try again after a back-off
        }
    }
}

void on_tick(emf::World w) {
    if (w.is_client()) {
        check_client(w);
        return;
    }
    if (!w.is_server()) return;
    const uint64_t now = GetTickCount64();
    const uint32_t session = emf::server_session();
    if (session != g_owners_session) {  // a new session (another world, a reload): nobody's set carries over, but a
        g_owners_session = session;      // companion that was out is wanted again (as after a reload)
        g_session_start_ms = now;
        g_was_out = g_owners.clear();
        g_last_check_ms = 0;
    }
    g_owners.each([&](Owner& o) {
        o.companion.on_session(session);
        if (o.died.exchange(false)) {
            const bool again = o.policy.on_died(now);
            EMF_LOG(kMod, "the companion of player %u died: %s", o.player,
                    companion::death_message(again, o.policy.respawn_ms / 1000ull, call_key()).c_str());
        }
        if (o.host && o.player) read_keys(w, o, now);
        if (o.companion.phase() == companion::Companion::Phase::Active) run_orders(w, o, now);
        if (const uint32_t dead = o.companion.take_destroy()) {
            const bool ok = emf::api()->destroy(w.raw(), dead);
            EMF_LOG(kMod, "companion %u dismissed (%s)", dead, ok ? "destroyed" : "destroy FAILED");
        }
    });
    if (now - g_last_check_ms < 1000) return;
    g_last_check_ms = now;
    update_players(w, now, session);
    g_owners.each([&](Owner& o) { check_owner(w, o, now); });
}

// companion.as <player entity> <word>: a player's order run from the console (for tests and admins).
void as_command(emf::World w, const std::vector<std::string>& args) {
    const auto order = companion::parse_as(args);
    if (!order) {
        EMF_REPLY("companion.as <player entity> summon|dismiss|follow|stay|attack|gear");
        return;
    }
    const uint32_t player = order->player;
    const uint64_t account = emf::player_account(w, player);
    Owner* o = account ? g_owners.find(account) : nullptr;
    if (!o || o->player != player) {
        EMF_REPLY("no player %u in this world yet (players are looked up once a second)", player);
        return;
    }
    const std::string& word = order->word;
    const uint64_t now = GetTickCount64();
    const bool active = o->companion.phase() == companion::Companion::Phase::Active;
    if (word == "summon") {
        if (active) {
            EMF_REPLY("player %u's companion is out already (entity %u)", player, o->companion.entity());
            return;
        }
        if (o->companion.phase() != companion::Companion::Phase::None) {
            EMF_REPLY("player %u's companion is being dismissed", player);
            return;
        }
        const Summon s = summon(w, *o, now, "companion.as", true, std::nullopt, true);
        EMF_REPLY("%s", s == Summon::Ok ? format("companion %u for player %u", o->companion.entity(), player).c_str()
                        : s == Summon::Limit ? "refused: max_companions are out"
                        : s == Summon::NoPlayer ? "the player's position is not known"
                                                : "the spawn failed (see the log)");
    } else if (word == "gear") {
        const uint32_t pl = player_level(w, *o);
        EMF_REPLY("player %u's companion: level %u = %u + gear %u", player,
                  companion::companion_level(pl, o->gear.bonus(pl, g_gear_bonus_max)), pl, o->gear.bonus(pl, g_gear_bonus_max));
        for (const auto& piece : o->gear.held())
            EMF_REPLY("  %s: %s (level %u, %s)", companion::slot_name(piece.slot), item_name(w, piece.item).c_str(),
                      piece.level, rarity_name(piece.rarity));
    } else if (!active) {
        EMF_REPLY("player %u has no companion out (companion.as %u summon)", player, player);
    } else if (word == "dismiss") {
        summon_or_dismiss(w, *o, now, "companion.as");
        EMF_REPLY("dismissing player %u's companion", player);
    } else if (word == "follow") {
        order_follow(w, *o);
        EMF_REPLY("player %u's companion follows", player);
    } else if (word == "stay") {
        order_stay(w, *o);
        EMF_REPLY("player %u's companion: %s", player, o->orders.mode() == companion::Mode::Stay ? "stays" : "see the note");
    } else if (word == "attack") {
        order_attack(w, *o, now);
        EMF_REPLY("player %u's companion: %s", player,
                  o->orders.mode() == companion::Mode::Attack ? "attacks what they look at" : "nothing to attack there");
    } else {
        EMF_REPLY("companion.as <player entity> summon|dismiss|follow|stay|attack|gear");
    }
}

std::wstring ini_path() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring ini(exe);
    return ini.substr(0, ini.find_last_of(L"\\/")) + L"\\emf\\config\\companion.ini";
}

std::string read_guid_text() {
    wchar_t wide[64] = {};
    GetPrivateProfileStringW(L"companion", L"guid", L"", wide, 64, ini_path().c_str());
    std::string text;
    for (int i = 0; i < 63 && wide[i]; ++i) text += static_cast<char>(wide[i]);
    return text.empty() ? kDefaultGuid : text;
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;
    if (GetPrivateProfileIntW(L"companion", L"enabled", 1, ini_path().c_str()) == 0) {
        EMF_LOG(kMod, "disabled (set [companion] enabled=1 in emf\\config\\companion.ini)");
        return true;
    }
    const std::string guid = read_guid_text();
    if (!emf::parse_guid(guid, g_guid)) {
        EMF_LOG(kMod, "invalid guid '%s': disabled", guid.c_str());
        return true;
    }
    EmfGameBuild build{};
    build.size = sizeof(build);
    g_dedicated = emf::has_v14() && api->game_build && api->game_build(&build) && build.server;
    wchar_t dist[32] = {};
    GetPrivateProfileStringW(L"companion", L"follow_distance", L"0", dist, 32, ini_path().c_str());
    g_follow_distance = static_cast<float>(std::wcstod(dist, nullptr));
    g_catch_defaults = companion::CatchUp(g_follow_distance > 50.0f ? 50.0 : g_follow_distance);  // its limits move out with it
    const std::wstring ini = ini_path();
    wchar_t fac[32] = {};  // read as text: GetPrivateProfileInt turns a negative value (-1 = keep) into 0
    GetPrivateProfileStringW(L"companion", L"faction", L"11", fac, 32, ini.c_str());
    const companion::FactionSetting faction = companion::parse_faction(fac);
    g_faction = faction.value;
    if (!faction.ok) EMF_LOG(kMod, "faction '%ls' is not a number from -1 to 15: using 11 (PlayerAlly)", fac);
    g_policy_defaults.auto_summon = GetPrivateProfileIntW(L"companion", L"auto_summon", 0, ini.c_str()) != 0;
    g_policy_defaults.respawn_ms = 1000ull * GetPrivateProfileIntW(L"companion", L"respawn_seconds", 0, ini.c_str());
    g_max_companions = GetPrivateProfileIntW(L"companion", L"max_companions", 8, ini.c_str());
    auto read_key = [&ini](const wchar_t* name, const wchar_t* def, Hotkey& k) {
        wchar_t text[32] = {};
        GetPrivateProfileStringW(L"companion", name, def, text, 32, ini.c_str());
        if (!text[0]) return;
        k.vk = emf::keys::parse_key(text, &k.name);
        if (!k.vk) EMF_LOG(kMod, "%ls=%ls is not a letter, a digit or a keyboard key's virtual-key number: off", name, text);
    };
    const std::string wheel_default = companion::kWheelKeyDefault;
    read_key(L"key", std::wstring(wheel_default.begin(), wheel_default.end()).c_str(), g_key_wheel);
    read_key(L"key_follow", L"", g_key_follow);
    read_key(L"key_stay", L"", g_key_stay);
    read_key(L"key_attack", L"", g_key_attack);
    read_key(L"key_dismiss", L"", g_key_dismiss);
    wchar_t marker[64] = {};
    GetPrivateProfileStringW(L"companion", L"marker_guid", L"", marker, 64, ini.c_str());
    std::string marker_text;
    for (int i = 0; i < 63 && marker[i]; ++i) marker_text += static_cast<char>(marker[i]);
    if (marker_text.empty() || !emf::parse_guid(marker_text, g_marker_guid)) emf::parse_guid(kDefaultMarker, g_marker_guid);
    const bool program = emf::add_program("companion_guard", EMF_WORLD_SERVER, {"EntityDiedEvent"},
                                          {"StartGuardingEvent", "StopGuardingEvent", "AddThreatTargetEvent"},
                                          guard_program);
    if (!program) {
        EMF_LOG(kMod, "could not add the companion_guard program: disabled");
        return false;
    }
    g_gear_bonus_max = GetPrivateProfileIntW(L"companion", L"gear_bonus_max", 10, ini.c_str());
    emf::api()->on_load([](uint16_t slot, void*) {
        g_slot = slot;
        g_loaded_session = emf::server_session();
    }, nullptr);
    emf::add_command("companion.gear", ": what your companion wears and the level it gets", [](emf::World w,
                                                                                              const std::vector<std::string>&) {
        Owner* o = nullptr;
        g_owners.each([&](Owner& x) {
            if (x.host) o = &x;
        });
        if (!o) {
            EMF_REPLY("no host's companion in this world (on a dedicated server: companion.as <player> gear)");
            return;
        }
        const uint32_t pl = player_level(w, *o);
        const uint32_t bonus = o->gear.bonus(pl, g_gear_bonus_max);
        EMF_REPLY("companion level %u = you %u + gear %u (at most +%u)", companion::companion_level(pl, bonus), pl, bonus,
                  g_gear_bonus_max);
        for (const auto& piece : o->gear.held())
            EMF_REPLY("  %s: %s (level %u, %s)", companion::slot_name(piece.slot), item_name(w, piece.item).c_str(),
                      piece.level, rarity_name(piece.rarity));
        if (o->gear.held().empty())
            EMF_REPLY("  no gear (%s, then Gear, gives some from your backpack)", call_key().c_str());
        for (const auto& piece : o->gear.unreturned())
            EMF_REPLY(piece.put_failed ? "  waiting for room in your inventory (then Take all gear back): %s (level %u, %s)"
                                       : "  on the way back to you: %s (level %u, %s)",
                      item_name(w, piece.item).c_str(), piece.level, rarity_name(piece.rarity));
    });
    emf::add_command("companion.as", "<player entity> summon|dismiss|follow|stay|attack|gear: a player's companion order "
                     "from the console", [](emf::World w, const std::vector<std::string>& args) { as_command(w, args); });
    emf::add_command("companion.aim", ": the camera ray and the creatures near it (why Attack picks what it picks)",
                     [](emf::World w, const std::vector<std::string>&) {
        const uint32_t player = emf::local_player();
        double pp[3];
        const auto ray = emf::aim::camera_ray(w, player);
        if (!player || !entity_pos(w, player, pp) || !ray) {
            EMF_REPLY("no player position or camera on this world");
            return;
        }
        EMF_REPLY("player (%.1f, %.1f, %.1f); camera (%.1f, %.1f, %.1f) looking (%.2f, %.2f, %.2f)", pp[0], pp[1], pp[2],
                  ray->origin.x, ray->origin.y, ray->origin.z, ray->dir.x, ray->dir.y, ray->dir.z);
        int shown = 0;
        w.for_each_entity([&](uint32_t id) {
            double p[3];
            if (id == player || shown >= 12 || !entity_pos(w, id, p) || !w.get<gen::ecs::Health>(id)) return;
            const emf::aim::Vec3 at{p[0], p[1] + 0.8, p[2]};
            const double dist = emf::aim::length(emf::aim::sub(at, {pp[0], pp[1], pp[2]}));
            if (dist > companion::Orders::kAttackRange) return;
            const auto d = emf::aim::sub(at, ray->origin);
            const double t = emf::aim::dot(d, ray->dir);
            const double perp = emf::aim::length(emf::aim::sub(d, emf::aim::scale(ray->dir, t)));
            auto* f = w.get<gen::ecs::Faction>(id);
            EMF_REPLY("  %u: %.1f m away, %.1f m along the ray, %.1f m off it, faction %d", id, dist, t, perp,
                      f ? static_cast<int>(f->faction) : -1);
            ++shown;
        });
    });
    emf::on_tick(on_tick);
    EMF_LOG(kMod, "ready: %s summons a companion, then opens its order wheel (template %s, faction %d, "
            "auto_summon %d, respawn %llu s, max_companions %u%s)", g_key_wheel.vk ? g_key_wheel.name.c_str() : "no key (none set)",
            guid.c_str(), g_faction, g_policy_defaults.auto_summon ? 1 : 0, g_policy_defaults.respawn_ms / 1000ull,
            g_max_companions, g_dedicated ? ", dedicated server" : "");
    return true;
}
