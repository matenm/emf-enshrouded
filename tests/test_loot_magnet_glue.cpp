// Loot Magnet's game side (mods/loot_magnet/magnet_glue.h): what it reads from a dead entity's components, and the
// loot event it sends, over a fake API.
#include <doctest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../mods/loot_magnet/magnet_glue.h"

namespace gen = emf::gen::ecs;

namespace {
EmfWorld* const kW = reinterpret_cast<EmfWorld*>(0x5000);
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> g_comps;  // (hash, entity) -> bytes
std::map<std::pair<uint32_t, uint32_t>, uintptr_t> g_tpl;               // (hash, entity) -> template data
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
        api.version = EMF_API_VERSION;
        api.size = sizeof api;
        api.get_component = f_get;
        api.get_template_component = f_tpl;
        api.world_kind = f_kind;
        emf::api_ref() = &api;
    }
    ~Rig() { emf::api_ref() = nullptr; }
};
std::string names(uint32_t item) { return item == 0x11 ? "Material_T1_Animal_Fur" : item == 0x22 ? "Weapon_T1_1H_SwordEpic_01" : ""; }

// The Vukah brawler's corpse as the game leaves it: its template's components that matter, an Inventory added at
// death holding loot, Health 0, no interaction offer.
void brawler_corpse(uint32_t id) {
    auto& t = plant<gen::CurrentTransform>(id);
    t.transform.position.x = int64_t(3739) << 32;
    plant<gen::Health>(id);
    plant<gen::NetworkHealth>(id).healthMax = 1498;  // health 0: dead
    plant<gen::Faction>(id).faction = emf::gen::FactionType::Vukah;
    auto& inv = plant<gen::Inventory>(id);
    inv.slots[0].id.value = 0x11;
    inv.slots[0].data.count = 8;
    g_tpl[{gen::Enemy01BehaviorState::kHash, id}] = 0xABC000;
}
}  // namespace

TEST_CASE("loot magnet glue: a corpse is read as dead, with its stacks' names, its template, and no offer needed") {
    Rig r;
    brawler_corpse(6938);
    const auto b = magnet::read_body(emf::World(kW), 6938, names);
    CHECK(b.exists);
    CHECK(b.dead);
    CHECK(b.has_health);
    CHECK(b.has_inventory);
    CHECK_FALSE(b.has_offer);
    CHECK_FALSE(b.player);
    CHECK(b.faction == 3);
    CHECK(b.tpl == 0xABC000u);
    CHECK(b.items == std::vector<std::string>{"Material_T1_Animal_Fur"});
    CHECK(b.pos.x == doctest::Approx(3739.0));
    const magnet::Death d{6938, b.pos, 0, 1, magnet::template_of(emf::World(kW), 6938)};
    CHECK(magnet::judge(d, b, b.pos, {}, magnet::Rules{}, 2000, false) == magnet::Why::Ok);
}

TEST_CASE("loot magnet glue: owner, tame, villager, player and a living body are read as such") {
    Rig r;
    brawler_corpse(7000);
    plant<gen::OwnerRelationship>(7000).ownerEntityId.id = 1;
    CHECK(magnet::read_body(emf::World(kW), 7000, names).owner == 1u);
    brawler_corpse(7001);
    plant<gen::IsTame>(7001);
    CHECK(magnet::read_body(emf::World(kW), 7001, names).tame);
    brawler_corpse(7002);
    g_tpl[{magnet::kActiveNpcStateHash, 7002}] = 0x1;
    CHECK(magnet::read_body(emf::World(kW), 7002, names).villager);
    brawler_corpse(1);
    plant<gen::PlayerState>(1);
    CHECK(magnet::read_body(emf::World(kW), 1, names).player);
    brawler_corpse(7003);
    plant<gen::NetworkHealth>(7003).health = 50;
    CHECK_FALSE(magnet::read_body(emf::World(kW), 7003, names).dead);
    g_comps.erase({gen::NetworkHealth::kHash, 7003});
    CHECK(magnet::read_body(emf::World(kW), 7003, names).dead);  // no replicated health: the Health slot 0 (0) says dead
    plant<gen::Health>(7003).dataStorage[0] = 50;
    CHECK_FALSE(magnet::read_body(emf::World(kW), 7003, names).dead);
    g_comps.erase({gen::Health::kHash, 7003});
    CHECK_FALSE(magnet::read_body(emf::World(kW), 7003, names).dead);  // no health to read at all: not known dead
    CHECK_FALSE(magnet::read_body(emf::World(kW), 9999, names).exists);
    brawler_corpse(7004);
    plant<gen::Inventory>(7004).slots[2].id.value = 0x77;  // an item the game does not name yet
    const auto b = magnet::read_body(emf::World(kW), 7004, names);
    REQUIRE(b.items.size() == 1);
    CHECK(b.items[0].empty());
}

TEST_CASE("loot magnet glue: the loot event goes guest = the player, host = the corpse, offer 0, loot all, bypass") {
    gen::LootInteractionEvent e{};
    magnet::fill_loot_event(e, 1, 6938);
    CHECK(e.guestId.id == 1u);
    CHECK(e.hostId.id == 6938u);
    CHECK(e.offerId == 0u);
    CHECK(e.lootAll);
    CHECK(e.bypassInteraction);
}
