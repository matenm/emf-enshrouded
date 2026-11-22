// Night Raids' game side (mods/raids/raids_glue.h) over a fake API: who a raider is, the level it gets, the destroy
// loop (dawn, stop) and the per-world switch's mirror setting.
#include <doctest.h>

#include <map>
#include <string>
#include <cstring>
#include <vector>

#include "../mods/raids/raids_glue.h"

namespace gen = emf::gen::ecs;

namespace {
EmfWorld* const kW = reinterpret_cast<EmfWorld*>(0x5000);
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> g_comps;
std::map<std::pair<uint32_t, uint32_t>, uintptr_t> g_tpl;
std::vector<uint32_t> g_destroyed;
std::vector<uint32_t> g_entities;
void f_each(EmfWorld*, EmfEntityCallback cb, void* user) {
    for (uint32_t id : g_entities) cb(id, user);
}
void* f_get(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size) {
    auto it = g_comps.find({hash, entity});
    if (it == g_comps.end()) return nullptr;
    if (size) *size = it->second.size();
    return it->second.data();
}
void* f_tpl(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t*) {
    auto it = g_tpl.find({hash, entity});
    return it == g_tpl.end() ? nullptr : reinterpret_cast<void*>(it->second);
}
bool f_destroy(EmfWorld*, uint32_t id) {
    g_destroyed.push_back(id);
    return true;
}
uint8_t f_kind(EmfWorld*) { return EMF_WORLD_SERVER; }
template <class T>
T& plant(uint32_t entity) {
    auto& v = g_comps[{T::kHash, entity}];
    v.assign(sizeof(T), 0);
    return *reinterpret_cast<T*>(v.data());
}
struct Rig {
    EmfApi api{};
    Rig() {
        g_comps.clear();
        g_tpl.clear();
        g_destroyed.clear();
        g_entities.clear();
        api.version = EMF_API_VERSION;
        api.size = sizeof api;
        api.for_each_entity = f_each;
        api.get_component = f_get;
        api.get_template_component = f_tpl;
        api.destroy = f_destroy;
        api.world_kind = f_kind;
        emf::api_ref() = &api;
    }
    ~Rig() { emf::api_ref() = nullptr; }
};
// A Vukah raider as spawned: its template's data, a Level and the scaling it rescales with.
void raider(uint32_t id, uintptr_t tpl = 0xA000) {
    plant<gen::CurrentTransform>(id);
    plant<gen::Level>(id).dataStorage[0] = 1;
    plant<gen::DynamicEnemyScaling>(id).nextUpdateTime.value = 12345;
    g_tpl[{gen::Enemy01BehaviorState::kHash, id}] = tpl;
}
}  // namespace

TEST_CASE("raids glue: a raider is known by its template and, once written, its level; a reused id is not it") {
    Rig r;
    emf::World w(kW);
    raider(5000);
    raids::RaiderId id = raids::identify(w, 5000);
    CHECK(id.tpl == 0xA000u);
    CHECK(raids::is_raider(w, 5000, id));
    CHECK(raids::write_level(w, 5000, 7));
    CHECK(w.get<gen::Level>(5000)->dataStorage[0] == 7u);
    CHECK(w.get<gen::DynamicEnemyScaling>(5000)->nextUpdateTime.value == 0);
    id.level = 7;
    CHECK(raids::is_raider(w, 5000, id));
    w.get<gen::Level>(5000)->dataStorage[0] = 3;  // another creature got the id (another level)
    CHECK_FALSE(raids::is_raider(w, 5000, id));
    raider(5001, 0xB000);  // another template under the id
    CHECK_FALSE(raids::is_raider(w, 5001, raids::RaiderId{0xA000, 0}));
    CHECK_FALSE(raids::is_raider(w, 9999, id));  // gone
    CHECK_FALSE(raids::write_level(w, 9999, 7));
}

TEST_CASE("raids glue: the destroy loop (dawn, stop) removes only raiders that still exist and are still theirs") {
    Rig r;
    emf::World w(kW);
    raider(5000);
    raider(5001);
    raider(5002, 0xB000);  // its id was reused by something else
    std::map<uint32_t, raids::RaiderId> known = {{5000, {0xA000, 1}}, {5001, {0xA000, 1}}, {5002, {0xA000, 1}},
                                                 {5003, {0xA000, 1}}};  // 5003 is gone
    const int n = raids::destroy_raiders(w, {5000, 5001, 5002, 5003}, known);
    CHECK(n == 2);
    CHECK(g_destroyed == std::vector<uint32_t>{5000, 5001});
}

TEST_CASE("raids glue: allies are the players' fighters (companion, summons, tamed beasts), never players or farm animals") {
    Rig r;
    emf::World w(kW);
    const auto fighter = [](uint32_t id, emf::gen::FactionType f, double x) {
        plant<gen::CurrentTransform>(id).transform.position.x = static_cast<int64_t>(x * 4294967296.0);
        plant<gen::Faction>(id).faction = f;
        plant<gen::DynamicEnemy01BehaviorState>(id);
        plant<gen::NetworkHealth>(id).health = 100;
    };
    fighter(5000, emf::gen::FactionType::PlayerAlly, 10);  // the companion, a summon, a tamed beast
    fighter(5001, emf::gen::FactionType::Player, 20);      // a player-faction body (FakePlayer)
    fighter(5002, emf::gen::FactionType::Vukah, 5);        // a raider
    fighter(5003, emf::gen::FactionType::PlayerAlly, 500);  // an ally far away
    fighter(5004, emf::gen::FactionType::PlayerAlly, 15);
    w.get<gen::NetworkHealth>(5004)->health = 0;  // a dead summon
    plant<gen::CurrentTransform>(5005);           // a baby farm animal: PlayerAlly, but no fighting AI
    plant<gen::Faction>(5005).faction = emf::gen::FactionType::PlayerAlly;
    fighter(1, emf::gen::FactionType::Player, 3);  // a player: counted on its own, never as an ally
    plant<gen::PlayerState>(1);
    g_entities = {1, 5000, 5001, 5002, 5003, 5004, 5005};
    CHECK(raids::is_ally(w, 5000));
    CHECK(raids::is_ally(w, 5001));
    CHECK_FALSE(raids::is_ally(w, 5002));
    CHECK_FALSE(raids::is_ally(w, 5005));
    CHECK_FALSE(raids::is_ally(w, 1));
    CHECK_FALSE(raids::is_ally(w, 9999));
    fighter(5006, emf::gen::FactionType::Player, 12);  // a villager (its template has ActiveNpcState): never a target
    g_tpl[{gen::ActiveNpcState::kHash, 5006}] = 0xC000;
    CHECK_FALSE(raids::is_ally(w, 5006));
    const auto near = raids::allies_near(w, {0, 0, 0}, 100);
    REQUIRE(near.size() == 3);
    CHECK(near[0].id == 5000u);
    CHECK(near[0].pos.x == doctest::Approx(10.0));
    CHECK_FALSE(near[0].player);
    CHECK(near[0].alive);
    CHECK(near[1].id == 5001u);
    CHECK(near[2].id == 5004u);
    CHECK_FALSE(near[2].alive);
}

TEST_CASE("raids glue: a boss or mini-boss is a creature with Boss or a boss health bar (as Beastmaster and Necromancer tell)") {
    Rig r;
    emf::World w(kW);
    raider(5000);
    raider(5001);
    plant<gen::BossHealthBar>(5001);
    raider(5002);
    plant<gen::Boss>(5002);
    CHECK_FALSE(raids::is_boss(w, 5000));
    CHECK(raids::is_boss(w, 5001));
    CHECK(raids::is_boss(w, 5002));
    CHECK_FALSE(raids::is_boss(w, 9999));
}

TEST_CASE("raids glue: the per-world switch's setting follows the world's file; a change of the setting in a loaded world switches it") {
    using raids::switch_step;
    CHECK_FALSE(switch_step(false, 1, false).has_value());  // no world file yet: nothing to switch
    CHECK_FALSE(switch_step(true, -1, false).has_value());  // the setting is not declared (no mod.json): never guessed
    CHECK_FALSE(switch_step(true, 0, false).has_value());   // in step
    CHECK_FALSE(switch_step(true, 1, true).has_value());
    REQUIRE(switch_step(true, 1, false).has_value());       // switched on in the Mods tab
    CHECK(*switch_step(true, 1, false) == true);
    CHECK(*switch_step(true, 0, true) == false);
}


// ---- walls (API v44) ----
namespace {
std::vector<EmfRemoveBuilding> g_removes;
bool f_remove(EmfWorld*, EmfRemoveBuilding* r) {
    g_removes.push_back(*r);
    return true;
}
}  // namespace

TEST_CASE("raids glue: chest_line starts 1.2 m up and points at the altar, flat") {
    const auto [from, to] = raids::chest_line({0, 10, 0}, {10, 50, 0}, 4.0);
    CHECK(from.x == 0);
    CHECK(from.y == doctest::Approx(11.2));
    CHECK(to.x == doctest::Approx(4.0));
    CHECK(to.y == doctest::Approx(11.2));
    CHECK(to.z == doctest::Approx(0.0));
    const auto [f2, t2] = raids::chest_line({5, 0, 5}, {5, 0, 5}, 4.0);  // at the altar: no direction
    CHECK(t2.x == f2.x);
    CHECK(t2.z == f2.z);
}

TEST_CASE("raids glue: nearest_gate takes the first door point within 6 m, skips a farther one and points without a door") {
    const EmfPathPoint pts[4] = {{1, 0, 0, 0}, {2, 0, 0, 0}, {3, 0, 0, 5}, {8, 0, 0, 5}};
    const uint32_t doors[4] = {0, 55, 77, 88};  // 55 on a point that is no door point: ignored
    const auto g = raids::nearest_gate(pts, 4, doors, {0, 0, 0}, 6.0);
    REQUIRE(g);
    CHECK(g->first == 77);
    CHECK(g->second.x == 3);
    CHECK_FALSE(raids::nearest_gate(pts, 4, doors, {0, 0, 0}, 2.5));
    CHECK_FALSE(raids::nearest_gate(pts, 4, nullptr, {0, 0, 0}, 6.0));
    const auto far = raids::nearest_gate(pts, 4, doors, {6, 0, 0}, 6.0);
    REQUIRE(far);
    CHECK(far->first == 77);  // the first door point along the path within range
}

TEST_CASE("raids glue: a door's health: its Health once hit, else its template's maximum") {
    Rig rig;
    emf::World w(kW);
    alignas(4) static uint8_t destructible[2];
    const uint16_t max = 600;
    std::memcpy(destructible, &max, 2);
    g_tpl[{gen::Destructible::kHash, 40}] = reinterpret_cast<uintptr_t>(destructible);
    CHECK(raids::door_max_health(w, 40) == 600);
    CHECK(raids::door_health(w, 40) == 600);  // not hit yet: full
    plant<gen::Health>(40).dataStorage[0] = 450;
    CHECK(raids::door_health(w, 40) == 450);
    CHECK(raids::door_max_health(w, 41) == 0);
    CHECK(raids::door_health(w, 41) == 0);
}

TEST_CASE("raids glue: a gate at or below one hit is removed with the drop setting's refund") {
    Rig rig;
    rig.api.remove_building = f_remove;
    g_removes.clear();
    emf::World w(kW);
    CHECK(raids::gate_finish_due(100, 100));
    CHECK(raids::gate_finish_due(1, 100));
    CHECK_FALSE(raids::gate_finish_due(101, 100));
    CHECK_FALSE(raids::gate_finish_due(0, 100));  // already broken (or no health known): the game handles it
    std::string why;
    CHECK(raids::remove_gate(w, 40, true, &why));
    REQUIRE(g_removes.size() == 1);
    CHECK(g_removes[0].entity == 40);
    CHECK(g_removes[0].refund == EMF_REFUND_DROP);
    CHECK(g_removes[0].flags == EMF_REMOVE_FORCE);
    CHECK(raids::remove_gate(w, 41, false, &why));
    CHECK(g_removes[1].refund == EMF_REFUND_NONE);
}

TEST_CASE("raids glue: breaking walls needs EMF API v44") {
    Rig rig;
    CHECK(raids::walls_available());
    rig.api.size = static_cast<uint32_t>(offsetof(EmfApi, break_blocks));
    CHECK_FALSE(raids::walls_available());
}

TEST_CASE("raids glue: the hit's damage is the default pickaxe times the scale, whole and capped") {
    int32_t d[5];
    raids::wall_hit_damage(1.0, d);
    const int32_t def[5] = EMF_BREAK_DEFAULT_DAMAGE;
    for (int i = 0; i < 5; ++i) CHECK(d[i] == def[i]);
    raids::wall_hit_damage(2.5, d);
    CHECK(d[0] == def[0] * 5 / 2);
    raids::wall_hit_damage(1e9, d);
    CHECK(d[0] == 100000);
}

TEST_CASE("raids glue: a weak hit is at least 1 in each class (all 0 would mean the full default pickaxe)") {
    int32_t d[5];
    raids::wall_hit_damage(0.001, d);
    for (int i = 0; i < 5; ++i) CHECK(d[i] == 1);
    raids::wall_hit_damage(0.05, d);  // 100, 100, 95, 60, 50 x 0.05 = 5, 5, 4.75, 3, 2.5
    CHECK(d[0] == 5);
    CHECK(d[3] == 3);
    CHECK(d[4] == 3);
}

TEST_CASE("raids glue: a raider with a player within 4 m is fighting, not held up by a wall") {
    const std::vector<raids::Player> players = {{1, {10, 0, 10}, 5}, {2, {30, 0, 30}, 5}};
    CHECK(raids::player_near({12, 0, 12}, players, 4.0));        // 2.8 m
    CHECK_FALSE(raids::player_near({15, 0, 10}, players, 4.0));  // 5 m
    CHECK(raids::player_near({30, 9, 33}, players, 4.0));        // across only: 3 m
    CHECK_FALSE(raids::player_near({0, 0, 0}, {}, 4.0));
}

TEST_CASE("raids glue (1.3.0): a raider's stuck clock waits while its wall takes damage or a player is within 8 m") {
    const std::vector<raids::Player> players = {{1, {10, 0, 10}, 5}};
    CHECK(raids::stuck_clock_waits(false, {15, 0, 10}, players));        // 5 m: fighting, not stuck
    CHECK_FALSE(raids::stuck_clock_waits(false, {19, 0, 10}, players));  // 9 m
    CHECK(raids::stuck_clock_waits(true, {50, 0, 50}, players));         // its wall spot took damage lately
    CHECK_FALSE(raids::stuck_clock_waits(false, {0, 0, 0}, {}));
}

namespace {
uint32_t g_door_state = EMF_DOOR_CLOSED;
uint32_t f_door_state(EmfWorld*, uint32_t) { return g_door_state; }
}  // namespace

TEST_CASE("raids glue (1.3.0): a raider's last attack time comes from its enemy AI; none without one") {
    Rig rig;
    emf::World w(kW);
    CHECK_FALSE(raids::last_attack_time(w, 30));
    auto& ai = plant<gen::DynamicEnemy01BehaviorState>(30);
    ai.combatData.lastAttackTime.value = 1030383353941;
    REQUIRE(raids::last_attack_time(w, 30));
    CHECK(*raids::last_attack_time(w, 30) == 1030383353941);
}

TEST_CASE("raids glue (1.3.0): the swing target is the invisible collision of a door (a template GUID)") {
    uint8_t g[16];
    CHECK(raids::parse_guid_text(raids::kSwingTargetGuid, g));
    CHECK(g[0] == 0x46);  // 217bfc46-...: the bytes as the game stores them
}

TEST_CASE("raids glue (review): a gate is gone when its entity is, or when the id is no door any more") {
    Rig rig;
    rig.api.door_state = f_door_state;
    emf::World w(kW);
    CHECK(raids::gate_gone(w, 40));  // no entity
    plant<gen::CurrentTransform>(40);
    g_door_state = EMF_DOOR_CLOSED;
    CHECK_FALSE(raids::gate_gone(w, 40));
    g_door_state = EMF_DOOR_NONE;  // the id belongs to something else now
    CHECK(raids::gate_gone(w, 40));
}

TEST_CASE("raids glue: a wall raiders may work on: any building block before API v45; from v45 not the world's own (a ruin) nor one of unknown origin") {
    EmfBlockInfo b{};
    b.size = sizeof b;
    b.kind = EMF_BLOCK_BUILDING;
    b.world = 0;
    CHECK(raids::breakable_wall(b, true));
    b.world = 1;
    CHECK_FALSE(raids::breakable_wall(b, true));
    b.world = -1;
    CHECK_FALSE(raids::breakable_wall(b, true));
    CHECK(raids::breakable_wall(b, false));  // a v44 core never wrote world
}

// A v44 core whose breaking is not there (a block signature lost on a game update) or went off after a fault still finds
// walls: raiders assigned to them would wait 60 s on hits that are all refused. Such a refusal stops wall looks.
TEST_CASE("raids glue: a break refused because breaking is not available or off stops wall looks; other refusals do not") {
    CHECK(raids::breaking_unavailable("not available on this game version"));
    CHECK(raids::breaking_unavailable("off (the game's building kernel faulted); breaks are refused until a restart"));
    CHECK_FALSE(raids::breaking_unavailable("radius must be 0.5 to 8"));
    CHECK_FALSE(raids::breaking_unavailable("64 breaks wait already"));
    CHECK_FALSE(raids::breaking_unavailable(""));
}

// Night Raids 1.3.0 (API v47): walls of terrain blocks a player placed (Stone, Granite ...), with the setting.
TEST_CASE("raids glue: a terrain wall a player placed is breakable; the world's own ground never; a terrain refusal stops terrain only") {
    EmfBlockInfo b{};
    b.size = sizeof b;
    b.kind = EMF_BLOCK_TERRAIN;
    b.world = 0;  // placed by a player
    b.hardness = 3;
    b.health = 5;
    CHECK(raids::breakable_wall(b, true));
    b.hardness = 5;  // Unbreakable: hits would do nothing
    CHECK_FALSE(raids::breakable_wall(b, true));
    b.hardness = 3;
    b.health = 0;  // no config known
    CHECK_FALSE(raids::breakable_wall(b, true));
    b.health = 5;
    b.world = 1;  // the world's own ground
    CHECK_FALSE(raids::breakable_wall(b, true));
    b.world = 0;
    CHECK_FALSE(raids::breakable_wall(b, false));  // a core before v45 cannot say whose terrain is: never
    CHECK(raids::terrain_unavailable("breaking terrain is not available on this game version"));
    CHECK(raids::terrain_unavailable("breaking terrain is off (the game's terrain config lookup faulted) until a restart"));
    CHECK(raids::terrain_unavailable("unknown flags"));  // a core before v47
    CHECK_FALSE(raids::terrain_unavailable("not available on this game version"));
    CHECK_FALSE(raids::terrain_unavailable("64 break requests are already waiting"));
    CHECK_FALSE(raids::breaking_unavailable("breaking terrain is not available on this game version"));
}
