// companion: an EMF mod. G or F6 (host) spawns a companion next to you that follows you and fights at your side,
// through the game's own guard system (StartGuardingEvent); G then opens its order wheel. Needs
// emf/config/companion.ini:
//   [companion]
//   enabled=1
//   guid=d5d05217-52dc-467a-9192-26b4f661d8d9   (default: Enemy_Vukah_Melee, a melee Vukah warrior)
//   faction=11          (default 11 = PlayerAlly: written on the companion right after it spawns, so it guards you and
//                        fights at full damage; 7 = Player hits for 0; -1 keeps the template's faction)
//   auto_summon=0       (1: summon one by itself once a world you host is running)
//   respawn_seconds=0   (N: after it dies, a new one comes N seconds later; 0: press F6)
//   follow_distance=6   (optional, metres: how close it stays; changes the TEMPLATE's guardSetup, so with an enemy body
//                        every wild Vukah of that template in this session uses it too)
//   key=G               (the order wheel: Follow me / Stay here / Attack / Gear / Dismiss; with no companion out it
//                        summons one)
//   key_follow= key_stay= key_attack= key_dismiss=   (optional single-key orders; empty = off)
//   marker_guid=0ce28db1-522f-4455-b358-3888fb5e089b (what "Stay here" puts down for it to guard: a small barrel)
//   gear_bonus_max=10   (the most levels gear can add on top of the player's level)
// Plain enemy01 actors without PlayerInput work as templates; see docs/engine_parts/re_companions.md. The game does
// not save the companion; the mod writes whether it is out, its order and its gear to
// emf/config/companion/<world slot>.txt and brings it back when that world is loaded again (reload or restart).
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <mutex>
#include <optional>
#include <vector>

#include "companion_logic.h"
#include "emf/aim.hpp"
#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "emf/keys.hpp"
#include "gear.h"
#include "orders.h"

namespace gen = emf::gen;

namespace {
constexpr const char* kMod = "companion";
constexpr const char* kDefaultGuid = "d5d05217-52dc-467a-9192-26b4f661d8d9";  // Enemy_Vukah_Melee
constexpr double kFixed = 4294967296.0;                                        // WorldPosition: 32.32 fixed point
constexpr int kActorReadyForGameplay = 50;                                     // keen::actor::StateFlag

companion::Companion g_companion;
companion::KeyEdge g_f6;
uint8_t g_guid[16] = {};
uint64_t g_last_check_ms = 0;
uint64_t g_last_report_ms = 0;
float g_follow_distance = 0.0f;   // 0 = the template's own guard distances
uint32_t g_follow_applied = 0;    // the companion whose template was last changed
int g_faction = 11;               // keen FactionType written on the companion (-1 = keep the template's)
companion::SummonPolicy g_policy;  // auto summon / respawn / back after a reload (server tick only)
std::atomic<bool> g_died{false};   // set by the program when the companion died; taken by the server tick
companion::Orders g_orders;        // follow / stay / attack (server tick only)
companion::GearBook g_gear;        // what it wears (server tick only; the world's, see the design rulings)
uint32_t g_gear_bonus_max = 10;    // ini gear_bonus_max
std::atomic<uint16_t> g_slot{0};   // world slot of the loaded save (0 = none yet this session)
uint32_t g_file_session = 0;       // the session whose file was read
std::string g_saved_text;          // what the file holds now
uint64_t g_last_level_ms = 0;
bool g_return_after_load = false;  // pieces left "returning" by a crash are put back once the player exists
// The data entity of the last item taken for the companion and when to check that the game removed it
// (docs/engine_parts/re_take_items.md: the commit destroys it).
std::atomic<uint32_t> g_taken_pide{0};
uint64_t g_taken_check_ms = 0;
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
std::mutex g_m;                    // guards the two below (server tick <-> the program's worker thread)
companion::ThreatBox g_threats;   // targets to send AddThreatTargetEvent for (per companion and session)
companion::DeathLog g_dead;       // entities whose EntityDiedEvent was seen, per session (attack targets end on them)
std::atomic<uint32_t> g_loaded_session{0};  // the server session whose save has loaded (on_load)
uint32_t g_client_checked = 0;     // the companion whose client-world faction was last checked (client tick only)
uint32_t g_client_session = 0;     // the session g_client_checked belongs to (client tick only)

// Writes the configured faction on the companion in world `w` (server: authoritative; client: what your own screen
// shows). Logs each change.
uint32_t g_no_faction_said = 0;  // the companion whose missing Faction was logged (one line per companion)

void apply_faction(emf::World w, uint32_t id, const char* world_name) {
    auto* f = w.get<gen::ecs::Faction>(id);
    if (!f) {
        if (g_faction >= 0 && w.is_server() && g_no_faction_said != id) {
            g_no_faction_said = id;
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
void apply_follow(emf::World w, uint32_t id) {
    auto* tpl = w.get_template<gen::ecs::Enemy01BehaviorState>(id);
    if (!tpl) {
        EMF_LOG(kMod, "companion %u: no Enemy01BehaviorState template data (get_template_component)", id);
        g_follow_applied = id;
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
    g_follow_applied = id;
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

// ECS program on the server world (game worker thread): sends the requested events, watches deaths.
void guard_program(void* ctx) {
    uint32_t guard = 0, guarded = 0;
    if (g_companion.take_start(&guard, &guarded)) {
        if (auto* e = emf::emit<gen::ecs::StartGuardingEvent>(ctx)) {
            e->guardingEntityId.id = guard;
            e->guardedEntityId.id = guarded;
            EMF_LOG(kMod, "StartGuardingEvent sent: entity %u guards %u", guard, guarded);
        } else {
            EMF_LOG(kMod, "StartGuardingEvent could not be sent (see the core log)");
        }
    }
    if (g_companion.take_stop(&guard, &guarded)) {
        if (auto* e = emf::emit<gen::ecs::StopGuardingEvent>(ctx)) {
            e->guardingEntityId.id = guard;
            e->guardedEntityId.id = guarded;
            EMF_LOG(kMod, "StopGuardingEvent sent: entity %u stops guarding %u", guard, guarded);
        }
    }
    const uint32_t mine = g_companion.entity();
    if (!mine) return;
    const uint32_t session = emf::server_session();
    std::vector<uint32_t> threats;
    {
        std::lock_guard<std::mutex> l(g_m);
        threats = g_threats.take(mine, session);  // a target queued for an earlier companion or session is dropped
    }
    for (uint32_t target : threats)
        if (auto* e = emf::emit<gen::ecs::AddThreatTargetEvent>(ctx)) {
            e->attackerId.id = mine;
            e->targetId.id = target;
            e->threatValue = 1000.0f;
        }
    emf::read<gen::ecs::EntityDiedEvent>(ctx, [mine, session](const gen::ecs::EntityDiedEvent& e) {
        if (e.deadEntityId.id != mine) {
            std::lock_guard<std::mutex> l(g_m);
            g_dead.record(session, e.deadEntityId.id);
            return;
        }
        g_companion.on_died(mine);
        g_died = true;
        EMF_LOG(kMod, "companion %u died", mine);
    });
}

enum class Summon { Ok, NoPlayer, Failed };

// Spawns a companion 2 m from the local player and makes it an ally. NoPlayer (logged when `loud`) while the player's
// position is unknown, Failed (logged) when the game refused the spawn.
Summon summon(emf::World w, uint64_t now, const char* why, bool loud) {
    const uint32_t player = emf::local_player();
    double p[3];
    if (!player || !entity_pos(w, player, p)) {
        if (loud) EMF_LOG(kMod, "%s: player position unknown", why);
        return Summon::NoPlayer;
    }
    const uint32_t id = emf::api()->spawn(w.raw(), g_guid, p[0] + 2.0, p[1], p[2]);
    if (!id) {
        EMF_LOG(kMod, "%s: spawn failed (see the core log)", why);
        return Summon::Failed;
    }
    apply_faction(w, id, "server");  // before its AI picks targets
    g_companion.on_spawned(id, player, now, reinterpret_cast<uintptr_t>(w.raw()));
    g_policy.on_summoned();
    g_orders.on_companion_spawned();  // the current order (e.g. stay) applies to the new companion too
    EMF_LOG(kMod, "%s: companion %u spawned 2 m from player %u at (%.1f, %.1f, %.1f)", why, id, player, p[0], p[1], p[2]);
    return Summon::Ok;
}

void summon_or_dismiss(emf::World w, uint64_t now, const char* why) {
    if (g_companion.phase() == companion::Companion::Phase::Active) {
        g_companion.request_stop();
        g_policy.on_dismissed();
        if (const uint32_t marker = g_orders.release_marker())  // a stay marker goes with it, now (its id is still ours)
            emf::api()->destroy(w.raw(), marker);
        EMF_LOG(kMod, "%s: dismissing companion %u", why, g_companion.entity());
        return;
    }
    if (g_companion.phase() != companion::Companion::Phase::None) return;  // a dismissal is in progress
    summon(w, now, why, true);
}

void check_companion(emf::World w, uint64_t now) {
    const uint32_t id = g_companion.entity();
    if (!id || g_companion.phase() != companion::Companion::Phase::Active) return;
    auto* ai = w.get<gen::ecs::DynamicEnemy01BehaviorState>(id);
    if (!ai) {
        g_companion.on_gone();
        const bool again = g_policy.on_died(now);
        EMF_LOG(kMod, "companion %u is gone: %s", id,
                companion::death_message(again, g_policy.respawn_ms / 1000ull).c_str());
        return;
    }
    apply_faction(w, id, "server");  // in case the game set it back
    // An "automated player" template (Player_Vukah_Melee) has PlayerInput. The game marks such an actor ready for
    // gameplay (Actor IsReadyForGameplay, without which its AI never leaves Spawning) only once that input says the
    // client is ready. There is no client, so say it for it.
    if (auto* input = w.get<gen::ecs::PlayerInput>(id); input && !input->fromClientIsReadyForGameplay) {
        input->fromClientIsReadyForGameplay = true;
        EMF_LOG(kMod, "companion %u has player input: marked it ready for gameplay", id);
    }
    if (g_follow_applied != id) apply_follow(w, id);
    const uint32_t target = ai->guardEntityId.id;
    if (g_companion.wants_regard(target, now))
        EMF_LOG(kMod, "companion %u guards %u, not the player: sending StartGuardingEvent again", id, target);
    if (now - g_last_report_ms >= 5000) {
        g_last_report_ms = now;
        double a[3], b[3];
        const uint32_t player = g_companion.player();
        const bool have = entity_pos(w, id, a) && entity_pos(w, player, b);
        auto* actor = w.get<gen::ecs::Actor>(id);
        const bool ready = actor && ((actor->currentState >> kActorReadyForGameplay) & 1);
        EMF_LOG(kMod, "companion %u: guards %u, AI state %u, %s, %.1f m from player %u", id, target,
                static_cast<unsigned>(ai->state), ready ? "ready" : "NOT ready for gameplay", have ? distance(a, b) : -1.0,
                player);
    }
}

// Client world (the host's own screen): the faction the client copy of the companion has; written there too when it
// differs, so your own targeting and health bars treat it as an ally.
void check_client(emf::World w) {
    const uint32_t session = emf::server_session();
    if (session != g_client_session) {  // ids are reused across sessions
        g_client_session = session;
        g_client_checked = 0;
    }
    const uint32_t id = g_companion.entity();
    if (!id || id == g_client_checked) return;
    auto* f = w.get<gen::ecs::Faction>(id);
    if (!f) return;  // not replicated to the client world yet
    g_client_checked = id;
    EMF_LOG(kMod, "companion %u: client world faction %d", id, static_cast<int>(f->faction));
    apply_faction(w, id, "client");
}

std::wstring state_dir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    return dir.substr(0, dir.find_last_of(L"\\/")) + L"\\emf\\config\\companion";
}
std::wstring state_file(uint16_t slot) {
    wchar_t name[16];
    swprintf(name, 16, L"\\%04x.txt", slot);
    return state_dir() + name;
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
// Writes the world's file when its contents changed (server tick, every second).
void save_state() {
    const uint16_t slot = g_slot;
    if (!companion::world_file_known(slot, g_file_session, emf::server_session())) return;  // never another world's file
    companion::SavedCompanion s;
    s.out = g_companion.phase() == companion::Companion::Phase::Active || g_policy.coming();
    s.order = g_orders.serialize();
    s.gear = g_gear.serialize();
    const std::string text = companion::serialize(s);
    if (text == g_saved_text) return;
    CreateDirectoryW(state_dir().c_str(), nullptr);
    if (companion::write_text_atomic(state_file(slot), text))
        g_saved_text = text;
    else
        EMF_LOG(kMod, "could not write %ls", state_file(slot).c_str());
}
// Once per session, when its save has loaded: the world's file decides gear, order and whether one comes back.
void load_state() {
    const uint16_t slot = g_slot;
    const std::string text = slot ? read_file(state_file(slot)) : std::string();
    g_saved_text = text;
    const auto s = companion::parse_saved(text);
    g_gear.load(s.gear);
    if (!g_orders.parse(s.order)) g_orders.follow();
    if (s.out) g_policy.want_on_load();
    g_return_after_load = !g_gear.unreturned().empty();
    if (!text.empty())
        EMF_LOG(kMod, "world slot %04x: %s, order %s, %zu gear piece(s)", slot, s.out ? "a companion was out" : "none out",
                s.order.c_str(), g_gear.held().size());
}

uint32_t player_level(emf::World w) {
    auto* l = w.get<gen::ecs::Level>(emf::local_player());
    return l ? l->dataStorage[0] : 1;
}
// The companion's level: the player's level plus its gear bonus (written only on the player's side).
void apply_level(emf::World w) {
    const uint32_t id = g_companion.entity();
    auto* l = w.get<gen::ecs::Level>(id);
    auto* f = w.get<gen::ecs::Faction>(id);
    auto* pl_level = w.get<gen::ecs::Level>(emf::local_player());
    if (!l || !pl_level) return;  // the player's level is not readable (yet): keep the companion's as it is
    const uint32_t pl = pl_level->dataStorage[0];
    const uint32_t target = companion::companion_level(pl, g_gear.bonus(pl, g_gear_bonus_max));
    const bool ally = f && static_cast<int>(f->faction) == kFactionPlayerAlly;
    const auto value = companion::level_to_write(l->dataStorage[0], target, ally);
    if (!value) return;
    const uint32_t before = l->dataStorage[0];
    l->dataStorage[0] = *value;
    if (auto* d = w.get<gen::ecs::DynamicEnemyScaling>(id)) d->nextUpdateTime.value = 0;  // rescale health now
    EMF_LOG(kMod, "companion %u: level %u -> %u (you %u + gear %u)", id, before, *value, pl,
            g_gear.bonus(pl, g_gear_bonus_max));
}

// Runs the order logic's actions (server tick).
void run_orders(emf::World w, uint64_t now) {
    const uint32_t player = emf::local_player();
    double pp[3] = {};
    if (!player || !entity_pos(w, player, pp)) return;
    const auto actions = g_orders.tick(now, {pp[0], pp[1], pp[2]}, [&](uint32_t id) -> std::optional<companion::EntityState> {
        double p[3];
        if (!id || !entity_pos(w, id, p)) return std::nullopt;
        std::lock_guard<std::mutex> l(g_m);
        return companion::EntityState{{p[0], p[1], p[2]}, !g_dead.dead(emf::server_session(), id)};
    });
    for (const auto& a : actions) {
        switch (a.kind) {
            case companion::OrderAction::GuardPlayer: g_companion.request_guard(0, now); break;
            case companion::OrderAction::GuardEntity: g_companion.request_guard(a.id, now); break;
            case companion::OrderAction::SpawnMarker: {
                const uint32_t id = emf::api()->spawn(w.raw(), g_marker_guid, a.pos.x, a.pos.y, a.pos.z);
                if (id) {
                    g_orders.on_marker_spawned(id);
                } else {
                    EMF_LOG(kMod, "the stay marker could not be spawned (marker_guid?): following instead");
                    g_orders.follow();
                }
                break;
            }
            case companion::OrderAction::DestroyMarker: emf::api()->destroy(w.raw(), a.id); break;
            case companion::OrderAction::Threat: {
                std::lock_guard<std::mutex> l(g_m);
                g_threats.push(g_companion.entity(), emf::server_session(), a.id);
                break;
            }
            case companion::OrderAction::Notice: emf::api()->notify("%s", a.text.c_str()); break;
        }
    }
}

// The creature under the crosshair within 40 m that the companion may attack: has Health, is not the player, the
// companion, a villager, or on the player's side.
std::optional<uint32_t> attack_target(emf::World w) {
    const uint32_t player = emf::local_player();
    double pp[3];
    if (!player || !entity_pos(w, player, pp)) return std::nullopt;
    const auto ray = emf::aim::camera_ray(w, player);
    if (!ray) return std::nullopt;
    const emf::aim::Vec3 from{pp[0], pp[1], pp[2]};
    std::vector<uint32_t> ids;
    std::vector<emf::aim::Vec3> points;
    const uint32_t mine = g_companion.entity();
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

void order_follow() {
    g_orders.follow();
    emf::api()->notify("Companion: following you");
}
void order_stay(emf::World w) {
    double p[3];
    if (!entity_pos(w, g_companion.entity(), p)) {
        emf::api()->notify("Companion: its position is not known yet: try Stay again in a moment");
        return;
    }
    g_orders.stay({p[0], p[1], p[2]});
    emf::api()->notify("Companion: staying here");
}
void order_attack(emf::World w, uint64_t now) {
    const auto t = attack_target(w);
    if (!t) {
        emf::api()->notify("Companion: nothing to attack there (look at a creature within 40 m)");
        return;
    }
    g_orders.attack(*t, now);
    emf::api()->notify("Companion: attacking entity %u", *t);
}

uint32_t companion_level(emf::World w) {
    auto* l = w.get<gen::ecs::Level>(g_companion.entity());
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
void note_put_outcome(companion::PutOutcome outcome, const std::string& name) {
    switch (outcome) {
        case companion::PutOutcome::Returned: emf::api()->notify("Companion: %s is back in your inventory", name.c_str()); break;
        case companion::PutOutcome::WornAgain:
            emf::api()->notify("Companion: no room for %s; it keeps wearing it", name.c_str());
            break;
        case companion::PutOutcome::Waiting:
            emf::api()->notify("Companion: no room for %s; make room and use Take all gear back", name.c_str());
            break;
        case companion::PutOutcome::Unknown: break;
    }
}
// Puts a returning piece back into the player's inventory as a new item with its level and rarity.
void return_piece(emf::World w, const companion::Piece& p) {
    const uint32_t item = p.item, token = p.token;
    const std::string name = item_name(w, item);
    const bool queued = emf::put_items_ex(w, emf::local_player(), item, 1, p.level, p.rarity,
                                          [token, name](emf::World, uint32_t added) {
                                              note_put_outcome(g_gear.on_put_result(token, added > 0, GetTickCount64()),
                                                               name);
                                              save_state();
                                          });
    if (!queued) {  // refused at once (the log says why): not a matter of room
        g_gear.on_put_result(token, false, GetTickCount64());
        emf::api()->notify("Companion: could not give %s back right now (the log says why); it keeps it",
                           name.c_str());
        save_state();
    }
}

// Takes the chosen piece from the player's inventory (that exact slot first); worn once the game confirmed it.
void give_piece(emf::World w, const GearCandidate& c) {
    if (!companion::world_file_known(g_slot, g_file_session, emf::server_session())) {
        emf::api()->notify("Companion: gear cannot be given until this world's companion file is known (see the log)");
        EMF_LOG(kMod, "a give was refused: the world slot is not known (no on_load on this game build?), so it could "
                      "not be saved");
        return;
    }
    auto* inv = w.get<gen::ecs::Inventory>(c.inventory);
    if (!inv || inv->slots[c.slot_index].id.value != c.item || inv->slots[c.slot_index].data.pide.id != c.pide) {
        emf::api()->notify("Companion: %s is no longer in that slot", c.name.c_str());
        return;
    }
    companion::Piece p;
    p.item = c.item;
    p.level = c.level;
    p.rarity = c.rarity;
    p.slot = c.slot;
    if (!g_gear.give(p, GetTickCount64()).ok) {
        emf::api()->notify("Companion: already handing over something for that slot");
        return;
    }
    save_state();
    const companion::Slot slot = c.slot;
    const std::string name = c.name;
    const uint64_t from = (static_cast<uint64_t>(c.slot_index) << 32) | c.inventory;
    const uint32_t pide = c.pide;
    const bool queued = emf::take_items(w, emf::local_player(), c.item, 1, from, [slot, name, pide](emf::World w,
                                                                                               uint32_t taken) {
        const auto back = g_gear.on_take_result(slot, taken > 0, GetTickCount64());
        if (taken && pide) {
            g_taken_pide = pide;
            g_taken_check_ms = GetTickCount64() + 3000;
        }
        if (taken)
            emf::api()->notify("Companion: now wears %s", name.c_str());
        else
            emf::api()->notify("Companion: could not take %s from your inventory", name.c_str());
        for (const auto& old : back) return_piece(w, old);
        save_state();
    }, true);  // that slot only: never another copy of the item
    if (!queued) {
        g_gear.on_take_result(slot, false, GetTickCount64());
        emf::api()->notify("Companion: taking items is not available (the log says why)");
    }
}

void take_back_gear(emf::World w) {
    const auto back = g_gear.take_back_all(GetTickCount64());
    if (back.empty()) {
        emf::api()->notify("Companion: it wears no gear");
        return;
    }
    for (const auto& p : back) return_piece(w, p);
    save_state();
}

void open_gear_wheel(emf::World w) {
    const uint32_t player = emf::local_player();
    const auto cands = gear_candidates(w, player);
    std::vector<std::string> pieces;
    for (const auto& c : cands) {
        char b[160];
        snprintf(b, sizeof(b), "%s (level %u, %s)", c.name.c_str(), c.level, rarity_name(c.rarity));
        pieces.push_back(b);
    }
    const std::vector<std::string> labels = companion::gear_wheel_labels(pieces);
    const uint32_t pl = player_level(w);
    char title[64];
    snprintf(title, sizeof(title), "Gear (+%u levels, %zu worn)", g_gear.bonus(pl, g_gear_bonus_max), g_gear.held().size());
    const bool ok = emf::wheel(title, labels, g_key_wheel.vk, EMF_WORLD_SERVER, [cands](emf::World w, int choice) {
        if (choice < 0) return;
        if (static_cast<size_t>(choice) >= companion::gear_wheel_offered(cands.size())) {
            take_back_gear(w);
            return;
        }
        give_piece(w, cands[static_cast<size_t>(choice)]);
    });
    if (!ok) emf::api()->notify("The gear wheel could not open (the log says why)");
}

void open_order_wheel(emf::World w) {
    char title[64];
    snprintf(title, sizeof(title), "Companion (level %u)", companion_level(w));
    const bool ok = emf::wheel(title, {"Follow me", "Stay here", "Attack (what you look at)", "Gear", "Dismiss"},
                               g_key_wheel.vk, EMF_WORLD_SERVER, [](emf::World w, int choice) {
                                   if (g_companion.phase() != companion::Companion::Phase::Active) return;
                                   const uint64_t now = GetTickCount64();
                                   switch (choice) {
                                       case 0: order_follow(); break;
                                       case 1: order_stay(w); break;
                                       case 2: order_attack(w, now); break;
                                       case 3: open_gear_wheel(w); break;
                                       case 4: summon_or_dismiss(w, now, "wheel"); break;
                                       default: break;
                                   }
                               });
    if (!ok) emf::api()->notify("The companion wheel could not open (the console may be open; the log says why)");
}

void read_keys(emf::World w, uint64_t now) {
    const bool active = g_companion.phase() == companion::Companion::Phase::Active;
    if (g_key_wheel.edge.pressed(emf::keys::down(g_key_wheel.vk))) {
        if (active)
            open_order_wheel(w);
        else if (g_companion.phase() == companion::Companion::Phase::None)
            summon(w, now, "wheel key", true);
    }
    if (g_key_follow.edge.pressed(emf::keys::down(g_key_follow.vk)) && active) order_follow();
    if (g_key_stay.edge.pressed(emf::keys::down(g_key_stay.vk)) && active) order_stay(w);
    if (g_key_attack.edge.pressed(emf::keys::down(g_key_attack.vk)) && active) order_attack(w, now);
    if (g_key_dismiss.edge.pressed(emf::keys::down(g_key_dismiss.vk)) && active) summon_or_dismiss(w, now, "dismiss key");
}

void on_tick(emf::World w) {
    if (w.is_client()) {
        check_client(w);
        return;
    }
    if (!w.is_server()) return;
    const uint64_t now = GetTickCount64();
    const uint32_t session = emf::server_session();
    g_companion.on_session(session);  // a new session (a reload keeps the world's address) forgets the old companion
    g_policy.on_session(session, now);
    g_orders.on_session(session);  // marker ids of the old world mean nothing now
    if (session != g_file_session && g_loaded_session != session && g_slot) {  // a new world: its own file decides
        g_slot = 0;
        g_gear = companion::GearBook{};
    }
    if (g_loaded_session == session && g_file_session != session) {
        g_file_session = session;
        load_state();
    }
    if (g_loaded_session == session) g_policy.on_loaded(now);
    if (g_died.exchange(false)) {
        const bool again = g_policy.on_died(now);
        EMF_LOG(kMod, "the companion died: %s", companion::death_message(again, g_policy.respawn_ms / 1000ull).c_str());
    }
    if (g_f6.pressed(emf::keys::down(VK_F6))) summon_or_dismiss(w, now, "F6");
    read_keys(w, now);
    if (g_companion.phase() == companion::Companion::Phase::Active) run_orders(w, now);
    if (const uint32_t dead = g_companion.take_destroy()) {
        const bool ok = emf::api()->destroy(w.raw(), dead);
        EMF_LOG(kMod, "companion %u dismissed (%s)", dead, ok ? "destroyed" : "destroy FAILED");
    }
    if (now - g_last_check_ms >= 1000) {
        g_last_check_ms = now;
        check_companion(w, now);
        g_gear.tick(now);
        if (g_taken_pide && now >= g_taken_check_ms) {
            const uint32_t pide = g_taken_pide.exchange(0);
            if (w.get<gen::ecs::ItemState>(pide)) {
                EMF_LOG(kMod, "the taken item's data entity %u is still there (the game did not remove it, or the id was "
                              "reused at once)", pide);
                emf::api()->notify("Companion: the game kept the taken item's data (entity %u); see the log", pide);
            } else {
                EMF_LOG(kMod, "the taken item's data entity %u was removed by the game", pide);
            }
        }
        if (g_return_after_load && emf::local_player()) {
            g_return_after_load = false;
            for (const auto& p : g_gear.unreturned()) return_piece(w, p);
        }
        if (g_companion.phase() == companion::Companion::Phase::Active && now - g_last_level_ms >= 2000) {
            g_last_level_ms = now;
            apply_level(w);
        }
        save_state();
        if (g_companion.phase() == companion::Companion::Phase::None && g_policy.due(now) &&
            summon(w, now, "auto", false) == Summon::Failed)
            g_policy.on_spawn_failed(now);  // try again after a back-off instead of every second
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
    wchar_t dist[32] = {};
    GetPrivateProfileStringW(L"companion", L"follow_distance", L"0", dist, 32, ini_path().c_str());
    g_follow_distance = static_cast<float>(std::wcstod(dist, nullptr));
    const std::wstring ini = ini_path();
    wchar_t fac[32] = {};  // read as text: GetPrivateProfileInt turns a negative value (-1 = keep) into 0
    GetPrivateProfileStringW(L"companion", L"faction", L"11", fac, 32, ini.c_str());
    const companion::FactionSetting faction = companion::parse_faction(fac);
    g_faction = faction.value;
    if (!faction.ok) EMF_LOG(kMod, "faction '%ls' is not a number from -1 to 15: using 11 (PlayerAlly)", fac);
    g_policy.auto_summon = GetPrivateProfileIntW(L"companion", L"auto_summon", 0, ini.c_str()) != 0;
    g_policy.respawn_ms = 1000ull * GetPrivateProfileIntW(L"companion", L"respawn_seconds", 0, ini.c_str());
    auto read_key = [&ini](const wchar_t* name, const wchar_t* def, Hotkey& k) {
        wchar_t text[32] = {};
        GetPrivateProfileStringW(L"companion", name, def, text, 32, ini.c_str());
        if (!text[0]) return;
        k.vk = emf::keys::parse_key(text, &k.name);
        if (!k.vk) EMF_LOG(kMod, "%ls=%ls is not a letter, a digit or a keyboard key's virtual-key number: off", name, text);
    };
    read_key(L"key", L"G", g_key_wheel);
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
    emf::add_command("companion.gear", ": what the companion wears and the level it gets", [](emf::World w,
                                                                                           const std::vector<std::string>&) {
        const uint32_t pl = player_level(w);
        const uint32_t bonus = g_gear.bonus(pl, g_gear_bonus_max);
        EMF_REPLY("companion level %u = you %u + gear %u (at most +%u)", companion::companion_level(pl, bonus), pl, bonus,
                  g_gear_bonus_max);
        for (const auto& piece : g_gear.held())
            EMF_REPLY("  %s: %s (level %u, %s)", companion::slot_name(piece.slot), item_name(w, piece.item).c_str(),
                      piece.level, rarity_name(piece.rarity));
        if (g_gear.held().empty()) EMF_REPLY("  no gear (G -> Gear gives some from your backpack)");
        for (const auto& piece : g_gear.unreturned())
            EMF_REPLY(piece.put_failed ? "  waiting for room in your inventory (then Take all gear back): %s (level %u, %s)"
                                       : "  on the way back to you: %s (level %u, %s)",
                      item_name(w, piece.item).c_str(), piece.level, rarity_name(piece.rarity));
    });
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
    EMF_LOG(kMod, "ready: F6 summons/dismisses a companion, %s opens its order wheel (template %s, faction %d, "
            "auto_summon %d, respawn %llu s)", g_key_wheel.vk ? g_key_wheel.name.c_str() : "no key", guid.c_str(),
            g_faction, g_policy.auto_summon ? 1 : 0, g_policy.respawn_ms / 1000ull);
    return true;
}
