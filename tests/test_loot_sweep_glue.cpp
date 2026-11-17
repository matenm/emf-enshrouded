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
    const auto near = sweep::things_near(r.w(), {{0, 0, 0}}, 10, names, {true, true});
    std::vector<uint32_t> ids;
    for (const auto& t : near) ids.push_back(t.id);
    std::sort(ids.begin(), ids.end());
    CHECK(ids == std::vector<uint32_t>{40, 42});
    CHECK(sweep::things_near(r.w(), {}, 10, names, {true, true}).empty());  // nobody: no walk
}

TEST_CASE("sweep glue: the accept event names the player, the chest and its offer") {
    gen::AcceptOfferedInteractionEvent e{};
    sweep::fill_accept(e, 1, 20, 7);
    CHECK(e.guestEntityId.id == 1u);
    CHECK(e.hostEntityId.id == 20u);
    CHECK(e.offerId == 7u);
}

namespace {
int g_tpl_calls = 0;
void* f_tpl_counted(EmfWorld* w, uint32_t hash, uint32_t entity, uint64_t* size) {
    ++g_tpl_calls;
    return f_tpl(w, hash, entity, size);
}
}  // namespace

TEST_CASE("sweep glue: the review's marks are read: spawner, bait, player's object, fading, who opened it, a fingerprint") {
    Rig r;
    at(50, 0, 0, 0);
    comp<gen::InteractionOfferRuntime>(50).lastAcceptData.guestId.id = 2;
    tpl<gen::EnemySettings>(50);
    tpl<gen::FoodBait>(50);
    tpl<gen::UsedItem>(50);
    comp<gen::DestroyOnLootActivated>(50);
    tpl<gen::DisableRandomLootOnDestroy>(50);
    comp<gen::SpawnTime>(50).spawnTime.value = 12345;
    tpl<gen::InteractionLock>(50).requiredItemRef.bytes[3] = 7;
    tpl<gen::RandomLoot>(50);  // a chest's and a barrel's parts are read only for random loot
    tpl<gen::RandomLootContainer>(50);
    comp<gen::Inventory>(50);
    const sweep::Thing t = sweep::read_thing(r.w(), 50, names);
    CHECK(t.enemy_spawner);
    CHECK(t.food_bait);
    CHECK(t.used_item);
    CHECK(t.looted);
    CHECK(t.disable_loot_on_destroy);
    CHECK(t.last_accept_guest == 2u);
    CHECK(t.fingerprint == 12345u);
    CHECK(t.lock_item_ref);
    at(51, 0, 0, 0);
    comp<gen::DestroyOnLoot>(51).shouldDestroy = true;
    CHECK(sweep::read_thing(r.w(), 51, names).looted);
}

TEST_CASE("sweep glue: a crop's stage (the entity its DynamicGrowth root's DynamicUpgrade points at) is marked") {
    Rig r;
    at(60, 1, 0, 0);  // the root
    comp<gen::DynamicGrowth>(60);
    comp<gen::DynamicUpgrade>(60).entityId.id = 61;
    at(61, 1, 0, 0);  // its stage: looks like a plant
    comp<gen::InteractionOfferRuntime>(61);
    comp<gen::DefaultInventory>(61);
    comp<gen::DestroyOnLoot>(61);
    at(62, 2, 0, 0);  // a wild plant
    comp<gen::InteractionOfferRuntime>(62);
    comp<gen::DefaultInventory>(62);
    comp<gen::DestroyOnLoot>(62);
    const auto near = sweep::things_near(r.w(), {{0, 0, 0}}, 10, names, {true, true});
    bool stage = false, wild = false;
    for (const auto& t : near) {
        if (t.id == 61) stage = t.crop_stage;
        if (t.id == 62) wild = !t.crop_stage;
    }
    CHECK(stage);
    CHECK(wild);
}

TEST_CASE("sweep glue: with smashing and chests off, the walk makes no template lookup for entities without an offer") {
    Rig r;
    r.api.get_template_component = f_tpl_counted;
    for (uint32_t id = 70; id < 90; ++id) {
        at(id, 1, 0, 0);
        tpl<gen::Destructible>(id);  // props, no offer
    }
    g_tpl_calls = 0;
    CHECK(sweep::things_near(r.w(), {{0, 0, 0}}, 10, names, {false, false}).empty());
    CHECK(g_tpl_calls == 0);
    at(95, 50, 0, 0);  // out of reach: one look at its position, nothing more
    comp<gen::InteractionOfferRuntime>(95);
    g_tpl_calls = 0;
    CHECK(sweep::things_near(r.w(), {{0, 0, 0}}, 10, names, {true, true}).size() == 0);
}

TEST_CASE("sweep glue: a player has room while one of their generic slots (backpack, bar) is empty") {
    Rig r;
    auto& setup = comp<gen::InventorySetup>(1);
    setup.linksEntities[0].id = 100;
    setup.linksCategories[0] = emf::gen::InventoryCategory::Generic;
    setup.linksEntities[1].id = 101;
    setup.linksCategories[1] = emf::gen::InventoryCategory::Equipment;
    auto& bag = comp<gen::Inventory>(100);
    for (auto& s : bag.slots) {
        s.id.value = 0x33;
        s.data.count = 1;
    }
    comp<gen::Inventory>(101);  // equipment slots, empty: not room for loot
    CHECK_FALSE(sweep::has_room(r.w(), 1));
    bag.slots[5].id.value = 0;
    CHECK(sweep::has_room(r.w(), 1));
    setup.availableSlotCount = 3;  // only 3 usable slots and 7 used: no room, though a slot is empty
    CHECK_FALSE(sweep::has_room(r.w(), 1));
    CHECK_FALSE(sweep::has_room(r.w(), 2));  // no inventory setup: no room known
}

// Seen in game: some barrels carry RandomLoot, RandomLootContainer and RandomLootOnDestroy as their own components,
// not their template's; the walk looked at the template only and never saw them.
TEST_CASE("sweep glue: a barrel whose RandomLootOnDestroy is its own component (not its template's) is found") {
    Rig r;
    at(30, 1, 0, 0);
    comp<gen::RandomLoot>(30);
    comp<gen::RandomLootContainer>(30);
    comp<gen::RandomLootOnDestroy>(30);
    tpl<gen::Destructible>(30).maxHealth = 8;
    const auto near = sweep::things_near(r.w(), {{0, 0, 0}}, 10, names, {true, false});
    REQUIRE(near.size() == 1);
    CHECK(sweep::classify(near[0]) == sweep::Kind::Smash);
}

TEST_CASE("sweep glue: an offer's isDisabled, a swarm's toggle state and a plant's Destructible are read") {
    Rig r;
    comp<gen::InteractionOfferRuntime>(60).isDisabled = true;
    comp<gen::DestroyOnLoot>(60);
    tpl<gen::DefaultInventory>(60);
    tpl<gen::Destructible>(60).maxHealth = 5;
    comp<gen::ServerToggleState>(60).state = true;
    sweep::Thing t = sweep::read_thing(r.w(), 60, names);
    CHECK(t.disabled);
    CHECK(t.destructible);
    CHECK(t.toggle == 1);
    comp<gen::ServerToggleState>(60).state = false;
    CHECK(sweep::read_thing(r.w(), 60, names).toggle == 0);
    auto& lit = comp<gen::ServerToggleState>(60);
    lit.state = true;
    lit.isExtinguished = true;  // put out: off
    CHECK(sweep::read_thing(r.w(), 60, names).toggle == 0);
    g_comps.erase({gen::ServerToggleState::kHash, 60});
    CHECK(sweep::read_thing(r.w(), 60, names).toggle == -1);
}

namespace {
std::string g_emitted;
std::vector<uint8_t> g_event(64);
void* f_emit(void*, const char* name, uint32_t size) {
    g_emitted = name;
    g_event.assign(size, 0);
    return g_event.data();
}
}  // namespace

TEST_CASE("sweep glue: an accept becomes the game's AcceptOfferedInteractionEvent, a loot its LootInteractionEvent") {
    Rig r;
    r.api.emit_event = f_emit;
    sweep::Action a;
    a.act = sweep::Act::Accept;
    a.player = 1;
    a.entity = 40;
    a.offer_id = 7;
    const sweep::Request req = sweep::request_of(a, 3);
    CHECK(req.accept);
    CHECK(req.offer_id == 7);
    CHECK(req.session == 3);
    sweep::send(reinterpret_cast<void*>(0x1), req);
    CHECK(g_emitted == "AcceptOfferedInteractionEvent");
    const auto* e = reinterpret_cast<const gen::AcceptOfferedInteractionEvent*>(g_event.data());
    CHECK(e->guestEntityId.id == 1);
    CHECK(e->hostEntityId.id == 40);
    CHECK(e->offerId == 7);
    a.act = sweep::Act::Loot;
    sweep::send(reinterpret_cast<void*>(0x1), sweep::request_of(a, 3));
    CHECK(g_emitted == "LootInteractionEvent");
}

TEST_CASE("sweep glue: a bee hive is told by the template it spawns when destroyed (the game's hive loot dropper)") {
    Rig r;
    comp<gen::InteractionOfferRuntime>(70);
    tpl<gen::Destructible>(70).maxHealth = 1;
    auto& spawn = comp<gen::SpawnEntityOnDestroy>(70);
    REQUIRE(emf::parse_guid("8abfb80f-14b1-47c3-a51d-fe83d956c9fe", spawn.spawnEntity.bytes));
    sweep::Thing t = sweep::read_thing(r.w(), 70, names);
    CHECK(t.bee_hive);
    CHECK_FALSE(t.hive_triggered);
    CHECK(t.destructible);
    CHECK(t.max_health == 1);
    sweep::Want no_hives;
    no_hives.hives = false;
    CHECK_FALSE(sweep::read_thing(r.w(), 70, names, no_hives).bee_hive);
    spawn.triggered = true;
    CHECK(sweep::read_thing(r.w(), 70, names).hive_triggered);
    REQUIRE(emf::parse_guid("a81ccc89-4a06-4b45-8d4c-f5dc24676440", comp<gen::SpawnEntityOnDestroy>(70).spawnEntity.bytes));
    CHECK_FALSE(sweep::read_thing(r.w(), 70, names).bee_hive);  // a campfire's pickup
}
