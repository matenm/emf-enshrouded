// Loot Magnet 2.0's game side (mods/loot_magnet/sweep_glue.h): what it reads from an entity's instance and template
// components into a Thing, which entities it looks at near the players, and the chest's accept event, over a fake API.
#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../mods/loot_magnet/sweep_glue.h"

namespace gen = emf::gen::ecs;

namespace {
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> g_comps;  // (hash, entity) -> instance bytes
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> g_tpls;   // (hash, entity) -> template bytes
void* f_get(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size) {
    auto it = g_comps.find({hash, entity});
    if (it == g_comps.end()) return nullptr;
    if (size) *size = it->second.size();
    return it->second.data();
}
void* f_tpl(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size) {
    auto it = g_tpls.find({hash, entity});
    if (it == g_tpls.end()) return nullptr;
    if (size) *size = it->second.size();
    return it->second.data();
}
uint8_t f_kind(EmfWorld*) { return EMF_WORLD_SERVER; }
void f_each(EmfWorld*, EmfEntityCallback cb, void* user) {
    std::vector<uint32_t> ids;
    for (const auto& [key, bytes] : g_comps) ids.push_back(key.second);
    for (const auto& [key, bytes] : g_tpls) ids.push_back(key.second);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (uint32_t id : ids) cb(id, user);
}
template <class T>
T& comp(uint32_t entity) {
    auto& v = g_comps[{T::kHash, entity}];
    v.assign(sizeof(T), 0);
    return *reinterpret_cast<T*>(v.data());
}
template <class T>
T& tpl(uint32_t entity) {
    auto& v = g_tpls[{T::kHash, entity}];
    v.assign(sizeof(T), 0);
    return *reinterpret_cast<T*>(v.data());
}
void at(uint32_t entity, double x, double y, double z) {
    auto& t = comp<gen::CurrentTransform>(entity);
    t.transform.position.x = static_cast<int64_t>(x * 4294967296.0);
    t.transform.position.y = static_cast<int64_t>(y * 4294967296.0);
    t.transform.position.z = static_cast<int64_t>(z * 4294967296.0);
}
struct Rig {
    EmfApi api{};
    Rig() {
        g_comps.clear();
        g_tpls.clear();
        api.version = EMF_API_VERSION;
        api.size = sizeof api;
        api.get_component = f_get;
        api.get_template_component = f_tpl;
        api.world_kind = f_kind;
        api.for_each_entity = f_each;
        emf::api_ref() = &api;
    }
    ~Rig() { emf::api_ref() = nullptr; }
    emf::World w() const { return emf::World(reinterpret_cast<EmfWorld*>(0x5000)); }
};
std::string names(uint32_t item) {
    switch (item) {
        case 0x11: return "Food_T1_raw_mushroom_Mushroom_brown";
        case 0x22: return "Armor_T0_Rags_Chest";
        case 0x33: return "Material_T1_Metal_Scraps";
        default: return "";
    }
}
}  // namespace

TEST_CASE("sweep glue: a mushroom as the game has it: its pickup item, exclude, offer, from instance and template") {
    Rig r;
    at(10, 1, 2, 3);
    auto& offer = comp<gen::InteractionOfferRuntime>(10);
    offer.offerId = 7;
    offer.isOffered = true;
    comp<gen::PickupItem>(10).itemId.value = 0x11;
    comp<gen::DefaultInventory>(10);
    tpl<gen::DestroyOnLoot>(10);  // a template part
    comp<gen::AreaLootOverride>(10).exclude = true;
    const sweep::Thing t = sweep::read_thing(r.w(), 10, names);
    CHECK(t.id == 10);
    CHECK(t.pos.x == doctest::Approx(1));
    CHECK(t.pos.z == doctest::Approx(3));
    CHECK(t.offer);
    CHECK(t.offer_id == 7);
    CHECK(t.offered);
    CHECK(t.pickup);
    CHECK(t.item == "Food_T1_raw_mushroom_Mushroom_brown");
    CHECK(t.default_inv);
    CHECK(t.destroy_on_loot);
    CHECK(t.exclude == 1);
    CHECK(sweep::classify(t) == sweep::Kind::Mushroom);
}

TEST_CASE("sweep glue: a scene chest: random loot, its lock's item, what it holds; a player's chest is excluded") {
    Rig r;
    at(20, 0, 0, 0);
    comp<gen::InteractionOfferRuntime>(20).offerId = 1;
    tpl<gen::RandomLoot>(20);
    tpl<gen::RandomLootContainer>(20);
    comp<gen::InventorySetup>(20);
    auto& inv = comp<gen::Inventory>(20);
    inv.slots[0].id.value = 0x22;
    tpl<gen::InteractionLock>(20).requiredItem.value = 0x99;
    comp<gen::DynamicInteractionLock>(20).itemUsed = true;
    tpl<gen::Destructible>(20).maxHealth = 50;
    sweep::Thing t = sweep::read_thing(r.w(), 20, names);
    CHECK(t.random_loot);
    CHECK(t.random_container);
    CHECK(t.setup);
    CHECK(t.inventory);
    CHECK(t.lock);
    CHECK(t.lock_item == 0x99u);
    CHECK(t.lock_item_used);
    CHECK(t.destructible);
    REQUIRE(t.stacks.size() == 1);
    CHECK(t.stacks[0] == "Armor_T0_Rags_Chest");
    CHECK_FALSE(t.chest_excluded);
    CHECK(sweep::classify(t) == sweep::Kind::Chest);
    tpl<gen::UsedItem>(20);  // a player placed it
    t = sweep::read_thing(r.w(), 20, names);
    CHECK(t.chest_excluded);
    CHECK(sweep::classify(t) == sweep::Kind::None);
}

TEST_CASE("sweep glue: a barrel: its health from the template; a spawner's faction, a hive, a mining node are read") {
    Rig r;
    at(30, 0, 0, 0);
    tpl<gen::RandomLoot>(30);
    tpl<gen::RandomLootContainer>(30);
    tpl<gen::RandomLootOnDestroy>(30);
    tpl<gen::Destructible>(30).maxHealth = 8;
    sweep::Thing t = sweep::read_thing(r.w(), 30, names);
    CHECK(t.loot_on_destroy);
    CHECK(t.max_health == 8);
    CHECK(sweep::classify(t) == sweep::Kind::Smash);
    comp<gen::Faction>(30);
    tpl<gen::SpawnEntityOnDestroy>(30);
    tpl<gen::MiningNode>(30);
    tpl<gen::Actor>(30);
    t = sweep::read_thing(r.w(), 30, names);
    CHECK(t.faction);
    CHECK(t.spawn_on_destroy);
    CHECK(t.mining_node);
    CHECK(t.actor);
}

TEST_CASE("sweep glue: things near the players: offers and barrels within reach; the rest is never read") {
    Rig r;
    at(40, 3, 0, 0);  // a drop near
    comp<gen::InteractionOfferRuntime>(40);
    comp<gen::PickupItem>(40).itemId.value = 0x33;
    comp<gen::DestroyOnLoot>(40);
    at(41, 50, 0, 0);  // a drop far
    comp<gen::InteractionOfferRuntime>(41);
    at(42, 2, 0, 1);  // a barrel near
    tpl<gen::RandomLootOnDestroy>(42);
    at(43, 1, 0, 0);  // a plain prop near: no offer, no RandomLootOnDestroy
    tpl<gen::Destructible>(43);
    const auto near = sweep::things_near(r.w(), {{0, 0, 0}}, 10, names);
    std::vector<uint32_t> ids;
    for (const auto& t : near) ids.push_back(t.id);
    std::sort(ids.begin(), ids.end());
    CHECK(ids == std::vector<uint32_t>{40, 42});
    CHECK(sweep::things_near(r.w(), {}, 10, names).empty());  // nobody: no walk
}

TEST_CASE("sweep glue: the accept event names the player, the chest and its offer") {
    gen::AcceptOfferedInteractionEvent e{};
    sweep::fill_accept(e, 1, 20, 7);
    CHECK(e.guestEntityId.id == 1u);
    CHECK(e.hostEntityId.id == 20u);
    CHECK(e.offerId == 7u);
}
