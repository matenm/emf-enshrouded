// Loot Magnet 2.0's sweep (mods/loot_magnet/sweep_logic.h): which entities near a player are collected, by group, and
// how (the game's loot event, the chest's accept that rolls its loot, a hit that smashes a barrel), without the game.
#include <doctest.h>

#include <string>
#include <vector>

#include "../mods/loot_magnet/sweep_logic.h"

using namespace sweep;

namespace {
Thing pickup(uint32_t id, const std::string& item, int exclude = -1, double x = 2) {
    Thing t;
    t.id = id;
    t.pos = {x, 0, 0};
    t.offer = true;
    t.destroy_on_loot = true;
    t.pickup = true;
    t.item = item;
    t.exclude = exclude;
    return t;
}
Thing plant(uint32_t id, double x = 2) {
    Thing t;
    t.id = id;
    t.pos = {x, 0, 0};
    t.offer = true;
    t.destroy_on_loot = true;
    t.default_inv = true;
    t.destructible = true;  // a plant can be destroyed (364 of the game's 369 gathering spots without a pickup)
    return t;
}
Thing chest(uint32_t id, double x = 2) {
    Thing t;
    t.id = id;
    t.pos = {x, 0, 0};
    t.offer = true;
    t.offered = true;
    t.offer_id = 1;
    t.random_loot = t.random_container = true;
    t.setup = t.inventory = true;
    t.lock = true;
    t.destructible = true;
    return t;
}
Thing barrel(uint32_t id, double x = 2) {
    Thing t;
    t.id = id;
    t.pos = {x, 0, 0};
    t.random_loot = t.random_container = t.loot_on_destroy = true;
    t.destructible = true;
    t.max_health = 8;
    return t;
}
Thing hive(uint32_t id, double x = 2) {  // a bee hive: what it spawns when destroyed is the game's hive loot dropper
    Thing t;
    t.id = id;
    t.pos = {x, 0, 0};
    t.offer = true;
    t.offered = true;
    t.destructible = true;
    t.max_health = 1;
    t.spawn_on_destroy = true;
    t.bee_hive = true;
    return t;
}
SweepRules all_on() {
    SweepRules r;
    for (auto& g : r.groups) g.on = true;
    r.reach = 8;
    return r;
}
std::vector<Player> me(bool on = true) { return {{1, {0, 0, 0}, on}}; }
}  // namespace

TEST_CASE("sweep: what each entity is, by the game's components and its item") {
    CHECK(classify(pickup(1, "Material_T1_Metal_Scraps")) == Kind::Ground);           // a drop, no AreaLootOverride
    CHECK(classify(pickup(1, "Build_TerrainMaterial_T1_Stone", 0)) == Kind::Ground);  // a loose stone, exclude 0
    CHECK(classify(pickup(1, "Collection_T1_Coin_01", 1)) == Kind::Collectible);
    CHECK(classify(pickup(1, "Food_T1_raw_mushroom_Mushroom_brown", 1)) == Kind::Mushroom);
    CHECK(classify(pickup(1, "Material_T1_Fire_Fly_FishingBait", 1)) == Kind::Firefly);
    CHECK(classify(pickup(1, "Material_T1_Flax", 1)) == Kind::Plant);   // a wild plant that carries a pickup
    CHECK(classify(pickup(1, "Weapon_T1_1H_Sword_01", 1)) == Kind::None);  // a scene item the game keeps out of its area loot
    CHECK(classify(plant(1)) == Kind::Plant);
    CHECK(classify(chest(1)) == Kind::Chest);
    CHECK(classify(barrel(1)) == Kind::Smash);
}

TEST_CASE("sweep: never a player's chest, a spawner or nest, a tree or rock, a creature, a bag or gear on the ground") {
    Thing c = chest(1);
    c.chest_excluded = true;  // UsedItem (a player placed it), UpgradeStorage, a lore object ...
    CHECK(classify(c) == Kind::None);
    c = chest(1);
    c.lock = false;  // no InteractionLock: a searchable lore skeleton
    CHECK(classify(c) == Kind::None);
    Thing b = barrel(1);
    b.faction = true;  // an enemy spawner or nest
    CHECK(classify(b) == Kind::None);
    b = barrel(1);
    b.spawn_on_destroy = true;  // a bee hive
    CHECK(classify(b) == Kind::None);
    b = barrel(1);
    b.mining_node = true;
    CHECK(classify(b) == Kind::None);
    b = barrel(1);
    b.offer = true;
    CHECK(classify(b) == Kind::None);
    Thing p = pickup(1, "Material_T1_Metal_Scraps");
    p.actor = true;
    CHECK(classify(p) == Kind::None);
    p = pickup(1, "Armor_T1_Chest_01");
    p.inventory = true;  // a pickup with its own inventory: a dropped bag or gear
    CHECK(classify(p) == Kind::None);
    Thing q = plant(1);
    q.inventory = true;  // a loot container (corpses are Loot Magnet's other part)
    CHECK(classify(q) == Kind::None);
    q = plant(1);
    q.destroy_on_loot = false;
    CHECK(classify(q) == Kind::None);
}

TEST_CASE("sweep: quest-like item names and the ignore list keep an item where it is") {
    for (const char* n : {"Quest_T1_Letter", "Item_NPC_Necklace", "LoreText_T1_01", "Map_MapPiece_03", "Key_T1_Crypt"}) {
        CAPTURE(n);
        CHECK(quest_like(n));
    }
    CHECK_FALSE(quest_like("Material_T1_Twigs"));
    const Ignore ig = Ignore::parse(" material_t1_twigs , Food_T1_raw_*,  ");
    CHECK(ig.match("Material_T1_Twigs"));
    CHECK(ig.match("Food_T1_raw_mushroom_Mushroom_brown"));
    CHECK_FALSE(ig.match("Material_T1_Twigs_Bundle"));
    CHECK_FALSE(ig.match("Food_T2_cooked"));
    CHECK_FALSE(Ignore::parse("").match("Anything"));
}

TEST_CASE("sweep: each group within its own reach, or the general one; only for a player whose magnet is on") {
    SweepRules r = all_on();
    r.groups[static_cast<size_t>(Kind::Ground)].reach = 3;
    Sweeper s;
    auto acts = s.tick(1000, 1, me(), {pickup(10, "Material_T1_Twigs", -1, 2.5), pickup(11, "Material_T1_Twigs", -1, 4),
                                       plant(12, 7.5), plant(13, 9)},
                       {}, r);
    REQUIRE(acts.size() == 2);
    CHECK(acts[0].entity == 10);
    CHECK(acts[0].act == Act::Loot);
    CHECK(acts[0].player == 1);
    CHECK(acts[1].entity == 12);
    Sweeper off;
    CHECK(off.tick(1000, 1, me(false), {pickup(10, "Material_T1_Twigs")}, {}, r).empty());
    SweepRules none;  // every group off by default but the defaults below
    CHECK(none.groups[static_cast<size_t>(Kind::Ground)].on);
    CHECK(none.groups[static_cast<size_t>(Kind::Collectible)].on);
    CHECK_FALSE(none.groups[static_cast<size_t>(Kind::Mushroom)].on);
    CHECK_FALSE(none.groups[static_cast<size_t>(Kind::Firefly)].on);
    CHECK_FALSE(none.groups[static_cast<size_t>(Kind::Plant)].on);
    CHECK_FALSE(none.groups[static_cast<size_t>(Kind::Chest)].on);
    CHECK_FALSE(none.groups[static_cast<size_t>(Kind::Smash)].on);
    Sweeper d;
    CHECK(d.tick(1000, 1, me(), {plant(12)}, {}, none).empty());  // plants off
}

TEST_CASE("sweep: nothing inside a base; smashing keeps 10 m more away") {
    SweepRules r = all_on();
    r.base_radius = 30;
    Sweeper s;
    const std::vector<magnet::Vec3> bases = {{0, 0, 35}};
    auto acts = s.tick(1000, 1, me(), {pickup(10, "Material_T1_Twigs"), barrel(11)}, bases, r);
    REQUIRE(acts.size() == 1);  // the pickup is 35 m from the altar; the barrel too, but within 30 + 10
    CHECK(acts[0].entity == 10);
    Sweeper t;
    CHECK(t.tick(1000, 1, me(), {pickup(10, "Material_T1_Twigs")}, {{0, 0, 20}}, r).empty());
}

TEST_CASE("sweep: a pickup is taken once its item is named, never a quest-like or ignored item") {
    SweepRules r = all_on();
    r.ignore = Ignore::parse("Material_T1_Twigs");
    Sweeper s;
    auto acts = s.tick(1000, 1, me(), {pickup(10, ""), pickup(11, "Material_T1_Twigs"), pickup(12, "Key_T1_Crypt"),
                                       pickup(13, "Material_T1_Metal_Scraps")},
                       {}, r);
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].entity == 13);
    acts = s.tick(3000, 1, me(), {pickup(10, "Material_T1_Wood")}, {}, r);  // named now
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].entity == 10);
}

TEST_CASE("sweep: a pickup that stays after the loot event gets 5 tries 2 s apart (a full bag), then it is left") {
    Sweeper s;
    const auto r = all_on();
    const std::vector<Thing> things = {pickup(10, "Material_T1_Twigs")};
    int sent = 0;
    for (uint64_t t = 1000; t <= 20000; t += 500) sent += static_cast<int>(s.tick(t, 1, me(), things, {}, r).size());
    CHECK(sent == Sweeper::kMaxTries);
}

TEST_CASE("sweep: a chest: accept while offered (it rolls its loot), then the loot event; empty 3 s later: done") {
    Sweeper s;
    const auto r = all_on();
    Thing c = chest(20);
    auto acts = s.tick(1000, 1, me(), {c}, {}, r);
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].act == Act::Accept);
    CHECK(acts[0].offer_id == 1);
    CHECK(s.tick(1500, 1, me(), {c}, {}, r).empty());  // waiting for the roll, no second accept
    c.stacks = {"Armor_T0_Rags_Chest"};
    c.offer_id = 2;
    acts = s.tick(2000, 1, me(), {c}, {}, r);
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].act == Act::Loot);
    Thing empty = chest(21, 3);  // chests never share a place (the magnet remembers emptied ones by it)
    CHECK(s.tick(1000, 1, me(), {empty}, {}, r).size() == 1);   // the accept
    CHECK(s.tick(4500, 1, me(), {empty}, {}, r).empty());       // nothing rolled yet: asked again later
    Thing taken = chest(22, 4);
    taken.offered = false;  // not offered (another player's turn, or out of the game's range): wait
    CHECK(s.tick(1000, 1, me(), {taken}, {}, r).empty());
}

TEST_CASE("sweep: a chest whose lock wants an item (a lockpick, a key) is never opened, nor one that holds a quest item") {
    Sweeper s;
    const auto r = all_on();
    Thing locked = chest(20);
    locked.lock_item = 0x1234;
    CHECK(s.tick(1000, 1, me(), {locked}, {}, r).empty());
    locked.lock_item_used = true;  // a player opened it with the item already
    CHECK(s.tick(1000, 1, me(), {locked}, {}, r).size() == 1);
    Thing quest = chest(21);
    quest.stacks = {"Material_T1_Twigs", "Quest_T1_Letter"};
    CHECK(s.tick(1000, 1, me(), {quest}, {}, r).empty());
    Thing ignored = chest(22);
    SweepRules ri = all_on();
    ri.ignore = Ignore::parse("Material_T1_Twigs");
    ignored.stacks = {"Material_T1_Twigs"};
    CHECK(s.tick(1000, 1, me(), {ignored}, {}, ri).empty());
}

TEST_CASE("sweep: a barrel is smashed with its own health (up to 3 hits 3 s apart if it stays); its drop is a pickup") {
    Sweeper s;
    const auto r = all_on();
    const Thing b = barrel(30);
    auto acts = s.tick(1000, 1, me(), {b}, {}, r);
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].act == Act::Smash);
    CHECK(acts[0].amount == 8);
    int hits = 1;
    for (uint64_t t = 1500; t <= 20000; t += 500) hits += static_cast<int>(s.tick(t, 1, me(), {b}, {}, r).size());
    CHECK(hits == Sweeper::kMaxSmashes);
    Thing tough = barrel(31);
    tough.max_health = 0;  // no health read: a big hit
    acts = s.tick(1000, 1, me(), {tough}, {}, r);
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].amount == Sweeper::kDefaultSmash);
}

TEST_CASE("sweep: the nearest player whose magnet is on gets it; at most 16 actions a sweep") {
    Sweeper s;
    const auto r = all_on();
    const std::vector<Player> two = {{1, {0, 0, 0}, true}, {2, {5, 0, 0}, true}};
    auto acts = s.tick(1000, 1, two, {pickup(10, "Material_T1_Twigs", -1, 4)}, {}, r);
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].player == 2);
    std::vector<Thing> many;
    for (uint32_t i = 0; i < 40; ++i) many.push_back(pickup(100 + i, "Material_T1_Twigs"));
    Sweeper m;
    CHECK(m.tick(1000, 1, me(), many, {}, r).size() == Sweeper::kPerPlayer);  // one player's share a sweep
    CHECK(m.tick(1500, 1, me(), many, {}, r).size() == Sweeper::kPerPlayer);  // the next ones, the first wait 2 s
}

TEST_CASE("sweep: a new session forgets every entity (ids are reused after a reload)") {
    Sweeper s;
    const auto r = all_on();
    Thing empty = chest(21);
    CHECK(s.tick(1000, 1, me(), {empty}, {}, r).size() == 1);
    CHECK(s.tick(4500, 1, me(), {empty}, {}, r).empty());  // done
    CHECK(s.tick(5000, 2, me(), {empty}, {}, r).size() == 1);  // another session: a new chest
}

// The review's finds (2026-11-16): the game's template list showed look-alikes the first rules took.

TEST_CASE("sweep: never an enemy spawner (bone piles, spider eggs carry no Faction), an NPC, a player's object") {
    Thing b = barrel(1);
    b.enemy_spawner = true;  // EnemySettings, RandomSpawner, TriggerSpawner or EnemySpawnerClient
    CHECK(classify(b) == Kind::None);
    for (int i = 0; i < 4; ++i) {
        Thing x = barrel(1);
        if (i == 0) x.actor = true;
        if (i == 1) x.setup = true;
        if (i == 2) x.used_item = true;
        if (i == 3) x.disable_loot_on_destroy = true;
        CAPTURE(i);
        CHECK(classify(x) == Kind::None);
    }
    Thing p = pickup(1, "Material_T1_Metal_Scraps");
    p.used_item = true;  // a player placed it
    CHECK(classify(p) == Kind::None);
}

TEST_CASE("sweep: thrown animal bait stays where it was thrown (taming)") {
    Thing bait = pickup(1, "Food_T1_Goat_Bait");
    bait.food_bait = true;
    CHECK(classify(bait) == Kind::None);
}

TEST_CASE("sweep: never a crop's stage (a farm outside the base radius), a fog nest's resource, a faction's thing") {
    Thing crop = plant(1);
    crop.crop_stage = true;  // the stage a DynamicGrowth root points at: harvesting it ends the plant
    CHECK(classify(crop) == Kind::None);
    Thing shroom = pickup(1, "Food_T1_raw_mushroom_Mushroom_brown", 1);
    shroom.crop_stage = true;
    CHECK(classify(shroom) == Kind::None);
    Thing nest = plant(1);
    nest.spawn_on_destroy = true;
    CHECK(classify(nest) == Kind::None);
    Thing f = plant(1);
    f.faction = true;
    CHECK(classify(f) == Kind::None);
    Thing fading = plant(1);
    fading.looted = true;  // DestroyOnLootActivated, or DestroyOnLoot.shouldDestroy: already taken, it fades out
    CHECK(classify(fading) == Kind::None);
}

TEST_CASE("sweep: a drop is a ground item whatever its name; names choose the group only for the game's scene pickups") {
    CHECK(classify(pickup(1, "Material_T2_MushroomDrop_01")) == Kind::Ground);       // chopped from a giant mushroom
    CHECK(classify(pickup(1, "Ammo_T2_FishingBait_Improved")) == Kind::Ground);     // dropped crafted bait
    CHECK(classify(pickup(1, "Collection_T1_Coin_01")) == Kind::Collectible);
    CHECK(classify(pickup(1, "Food_T1_raw_mushroom_Mushroom_brown", 1)) == Kind::Mushroom);
}

TEST_CASE("sweep: quest-like names: a Key_ at the start, prop components; turkey bait is no key") {
    CHECK(quest_like("Key_T1_Crypt"));
    CHECK(quest_like("Prop_Component_AltarUpgrade_02"));
    CHECK_FALSE(quest_like("Food_T3_Turkey_Bait"));
    CHECK_FALSE(quest_like("Material_T3_Turkey_Feeding"));
}

TEST_CASE("sweep: a chest another player opened is theirs: only one the magnet accepted, or this player's own, is emptied") {
    Sweeper s;
    const auto r = all_on();
    Thing c = chest(20);
    c.stacks = {"Armor_T0_Rags_Chest"};  // rolled by someone else's E
    c.last_accept_guest = 2;
    CHECK(s.tick(1000, 1, me(), {c}, {}, r).empty());
    CHECK(s.tick(5000, 1, me(), {c}, {}, r).empty());  // waits, never takes it
    c.last_accept_guest = 1;  // the player opened it themselves
    auto acts = s.tick(6000, 1, me(), {c}, {}, r);
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].act == Act::Loot);
}

TEST_CASE("sweep: a chest or a barrel nearer to a player whose magnet is off is left to them") {
    Sweeper s;
    const auto r = all_on();
    const std::vector<Player> two = {{1, {0, 0, 0}, true}, {2, {3, 0, 0}, false}};
    CHECK(s.tick(1000, 1, two, {chest(20, 4), barrel(21, 4)}, {}, r).empty());
    auto acts = s.tick(1500, 1, two, {pickup(22, "Material_T1_Twigs", -1, 4)}, {}, r);
    CHECK(acts.size() == 1);  // a ground item still comes to the magnet
}

TEST_CASE("sweep: a chest's stack the game has not named yet waits; a lock that names an item by its GUID counts") {
    Sweeper s;
    const auto r = all_on();
    Thing c = chest(20);
    CHECK(s.tick(1000, 1, me(), {c}, {}, r).size() == 1);  // accepted
    c.stacks = {"Material_T1_Twigs", ""};
    CHECK(s.tick(1500, 1, me(), {c}, {}, r).empty());
    c.stacks = {"Material_T1_Twigs", "Material_T1_Wood"};
    CHECK(s.tick(2000, 1, me(), {c}, {}, r).size() == 1);
    Thing locked = chest(21);
    locked.lock_item_ref = true;
    CHECK(s.tick(1000, 1, me(), {locked}, {}, r).empty());
}

TEST_CASE("sweep: a reused id (another entity) starts afresh: the old one's tries do not count") {
    Sweeper s;
    const auto r = all_on();
    Thing a = pickup(10, "Material_T1_Twigs");
    a.fingerprint = 111;
    for (uint64_t t = 1000; t <= 20000; t += 500) s.tick(t, 1, me(), {a}, {}, r);  // 5 tries, then done
    CHECK(s.tick(21000, 1, me(), {a}, {}, r).empty());
    Thing b = a;
    b.fingerprint = 222;  // a new drop under the same id
    CHECK(s.tick(21500, 1, me(), {b}, {}, r).size() == 1);
}

TEST_CASE("sweep: each player's nearest things first, at most 8 actions a player a sweep, and the room given") {
    Sweeper s;
    const auto r = all_on();
    std::vector<Thing> many;
    for (uint32_t i = 0; i < 20; ++i) many.push_back(pickup(100 + i, "Material_T1_Twigs", -1, 7.5 - i * 0.3));
    const std::vector<Player> two = {{1, {0, 0, 0}, true}, {2, {0, 0, 100}, true}};
    many.push_back(pickup(200, "Material_T1_Twigs", -1, 0));
    many.back().pos = {0, 0, 101};  // by player 2
    auto acts = s.tick(1000, 1, two, many, {}, r);
    size_t p1 = 0, p2 = 0;
    for (const auto& a : acts) (a.player == 1 ? p1 : p2)++;
    CHECK(p1 == Sweeper::kPerPlayer);
    CHECK(p2 == 1);  // not starved by the other's field
    const Action* first1 = nullptr;
    for (const auto& a : acts)
        if (!first1 && a.player == 1) first1 = &a;
    REQUIRE(first1);
    CHECK(first1->entity == 119);  // the nearest first
    CHECK(first1->first);
    Sweeper t;
    CHECK(t.tick(1000, 1, two, many, {}, r, 3).size() == 3);  // room for 3 requests
}

TEST_CASE("sweep: a chest that rolled nothing is asked once more after 30 s (a daytime or toggle lock), then left") {
    Sweeper s;
    const auto r = all_on();
    Thing c = chest(20);
    CHECK(s.tick(1000, 1, me(), {c}, {}, r).size() == 1);
    CHECK(s.tick(4500, 1, me(), {c}, {}, r).empty());
    CHECK(s.tick(20000, 1, me(), {c}, {}, r).empty());
    auto acts = s.tick(35000, 1, me(), {c}, {}, r);
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].act == Act::Accept);
    CHECK(s.tick(39000, 1, me(), {c}, {}, r).empty());
    CHECK(s.tick(80000, 1, me(), {c}, {}, r).empty());  // twice is enough
}

TEST_CASE("sweep: the bases are read again when a player arrived or moved far since the last sweep") {
    BaseWatch bw;
    CHECK(bw.moved({{1, {0, 0, 0}, true}}));     // a new player
    CHECK_FALSE(bw.moved({{1, {10, 0, 0}, true}}));
    CHECK(bw.moved({{1, {200, 0, 0}, true}}));   // a fast travel
    CHECK_FALSE(bw.moved({{1, {210, 0, 0}, true}}));
    CHECK(bw.moved({{1, {210, 0, 0}, true}, {2, {0, 0, 0}, true}}));  // someone joined
}

// A pickup that fits only in part may lose the rest (the game's own area loot asks first whether it fits): a pickup
// comes only to a player with an empty slot, where a whole stack always fits. Plants and chests move all or nothing,
// or slot by slot.
TEST_CASE("sweep: a pickup waits (no try spent) while the player's bag has no empty slot") {
    Sweeper s;
    const auto r = all_on();
    std::vector<Player> full = {{1, {0, 0, 0}, true, false}};
    CHECK(s.tick(1000, 1, full, {pickup(10, "Material_T1_Twigs")}, {}, r).empty());
    CHECK(s.tick(1000, 1, full, {plant(11)}, {}, r).size() == 1);  // a plant: all or nothing
    full[0].room = true;
    auto acts = s.tick(1500, 1, full, {pickup(10, "Material_T1_Twigs")}, {}, r);
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].first);
}

// Seen in game: a chest the magnet emptied was empty again 30 s later and was opened a second time (as if it had
// rolled nothing): emptied by the magnet, it is done.
TEST_CASE("sweep: a chest the magnet emptied is done, never opened again") {
    Sweeper s;
    const auto r = all_on();
    Thing c = chest(20);
    CHECK(s.tick(1000, 1, me(), {c}, {}, r).size() == 1);  // accept
    c.stacks = {"Material_T1_Twigs"};
    auto acts = s.tick(2000, 1, me(), {c}, {}, r);
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].act == Act::Loot);
    c.stacks.clear();
    CHECK(s.tick(2500, 1, me(), {c}, {}, r).empty());
    CHECK(s.tick(40000, 1, me(), {c}, {}, r).empty());  // no second accept
}

// The game disables an offer it no longer takes: a chest it emptied (remove_interaction_on_empty_inventory), a swarm
// it hid. Such a thing is left while disabled, and comes once enabled again.
TEST_CASE("sweep: a thing whose offer the game disabled is left while it stays disabled") {
    Sweeper s;
    const auto r = all_on();
    Thing p = pickup(10, "Material_T1_Twigs");
    Thing c = chest(11);
    Thing pl = plant(12);
    p.disabled = c.disabled = pl.disabled = true;
    CHECK(classify(p) == Kind::None);
    CHECK(classify(c) == Kind::None);
    CHECK(s.tick(1000, 1, me(), {p, c, pl}, {}, r).empty());
    p.disabled = false;
    CHECK(s.tick(1500, 1, me(), {p}, {}, r).size() == 1);
}

// The game's scene pickups it keeps out of its own area loot (exclude 1) that are materials or food: a plant's item is
// a plant; stones, ore, wood, bones, fur, meat, drinks and the like that lie in ruins and camps are ground items.
TEST_CASE("sweep: a scene pickup that is not a plant (stone, ore, wood, bones, meat, a drink) is a ground item") {
    for (const char* item : {"Material_T1_Stone", "Material_T1_Wood_Logs", "Material_T1_Bones", "Material_T1_Animal_Fur",
                             "Food_T1_processed_meat_Grilled_WolfMeat", "Food_T1_raw_bad_meat_LeanMeat",
                             "Food_T1_raw_hydration_Milk", "Material_T1_Metal_Scraps", "Material_T1_Charcoal",
                             "Material_T4_Iron_Ore", "Material_T1_Torn_Cloth", "Material_T1_Resin", "Food_T3_raw_egg_BirdEggs",
                             "Material_T7_Moonstone", "Material_T1_Fog_Essence01_ShroudSpores", "Food_T1_raw_bad_meat_WolfMeat"})
        CHECK_MESSAGE(pickup_kind(item, 1) == Kind::Ground, std::string(item));
    for (const char* item : {"Material_T1_Plant_Fibre", "Food_T1_raw_fruit_Red_Berries", "Material_T5_Wolfsbane",
                             "Material_T1_Flax", "Food_T7_raw_vegetable_Eggplant", "Food_T6_raw_fruit_Watermelon",
                             "Material_T3_Barley", "Material_T2_Woodruff", "Food_T5_raw_herb_Oregano", "Material_T4_Stonecrop"})
        CHECK_MESSAGE(pickup_kind(item, 1) == Kind::Plant, std::string(item));
}

// Gathering spots without a pickup: plants carry Destructible; a bat's fur patch and a skeleton's bones (creature
// drops that lie on the ground) do not.
TEST_CASE("sweep: a gathering spot without a pickup is a plant when it can be destroyed, else a ground item") {
    Thing drop = plant(1);
    drop.destructible = false;
    CHECK(classify(drop) == Kind::Ground);
    CHECK(classify(plant(1)) == Kind::Plant);
}

// ServerToggleState: a swarm's state stayed true by day and by night (seen in game, 2026-11-17); one the game
// switched off or put out (isExtinguished) is not there to take.
TEST_CASE("sweep: a swarm the game switched off or put out is left") {
    Thing f = pickup(1, "Material_T1_Fire_Fly_FishingBait", 1);
    f.toggle = 0;
    CHECK(classify(f) == Kind::None);
    f.toggle = 1;
    CHECK(classify(f) == Kind::Firefly);
    f.toggle = -1;  // no toggle at all: as before
    CHECK(classify(f) == Kind::Firefly);
}

// After a world reload the game enables an emptied chest's offer again and an accept rolls nothing (seen in game):
// the chests the magnet emptied are kept per world (Loot Magnet's file), by where they stand (scene chests never move).
TEST_CASE("sweep: a chest the magnet emptied is remembered for the world and not opened in a later session") {
    Sweeper s;
    const auto r = all_on();
    Thing c = chest(20);
    c.pos = {2.04, 0.0, -3.0};
    CHECK_FALSE(s.emptied_dirty());
    REQUIRE(s.tick(1000, 1, me(), {c}, {}, r).size() == 1);  // the accept
    CHECK_FALSE(s.emptied_dirty());  // asked, not emptied yet
    c.stacks = {"Material_T1_Twigs"};
    REQUIRE(s.tick(2000, 1, me(), {c}, {}, r).size() == 1);  // the loot event
    CHECK_FALSE(s.emptied_dirty());  // sent; not seen empty yet
    c.stacks.clear();
    CHECK(s.tick(4500, 1, me(), {c}, {}, r).empty());  // seen empty after the magnet's loot (its retry time): remembered
    CHECK(s.emptied_dirty());
    const std::string text = s.emptied_text();
    CHECK(text == "20 0 -30\n");  // decimetres
    s.emptied_saved();
    CHECK_FALSE(s.emptied_dirty());

    Sweeper next;  // the next session: the file read again
    next.load_emptied("junk\n" + text);
    Thing again = chest(99);  // another id in that session, the same place
    again.pos = {2.0, 0.02, -3.01};
    CHECK(next.tick(1000, 2, me(), {again}, {}, r).empty());
    CHECK(next.tick(40000, 2, me(), {again}, {}, r).empty());
    Thing other = chest(98);
    other.pos = {5, 0, -3};
    CHECK(next.tick(41000, 2, me(), {other}, {}, r).size() == 1);  // a chest elsewhere is opened
    CHECK_FALSE(next.emptied_dirty());
    next.load_emptied("");  // another world without a file
    CHECK(next.emptied_text().empty());
}

// A full bag: the loot event takes what fits and the chest keeps the rest; such a chest is not remembered (the next
// session leaves it to the player rather than skipping loot that was never taken).
TEST_CASE("sweep: a chest that kept part of its loot is not remembered as emptied") {
    Sweeper s;
    const auto r = all_on();
    Thing c = chest(20);
    REQUIRE(s.tick(1000, 1, me(), {c}, {}, r).size() == 1);
    c.stacks = {"Material_T1_Twigs", "Material_T1_Stone"};
    REQUIRE(s.tick(2000, 1, me(), {c}, {}, r).size() == 1);
    c.stacks = {"Material_T1_Stone"};  // what did not fit
    CHECK(s.tick(4500, 1, me(), {c}, {}, r).size() == 1);  // asked again this session
    CHECK_FALSE(s.emptied_dirty());
}

// The record belongs to a save slot: a world started anew in the same slot (a wiped dedicated server) would inherit it.
// The host clears it with a command; the count is logged when the file is read.
TEST_CASE("sweep: the remembered chests can be counted and forgotten") {
    Sweeper s;
    s.load_emptied("1 2 3\n4 5 6\n");
    CHECK(s.emptied_count() == 2);
    s.forget_emptied();
    CHECK(s.emptied_count() == 0);
    CHECK(s.emptied_dirty());  // the empty list is saved
    CHECK(s.emptied_text().empty());
}

// Bee hives (seen in game): one hit of their health destroys them, honey and wax drop on the ground, and a wasp swarm
// comes out. Their parts also fit fog nest resources and the Flame Power capsules; what they spawn tells them apart.
TEST_CASE("sweep: a bee hive is smashed with its own group (off by default), nothing else that spawns on destroy") {
    CHECK(classify(hive(1)) == Kind::Hive);
    CHECK(std::string(kind_name(Kind::Hive)) == "bee hive");
    Thing other = hive(1);
    other.bee_hive = false;  // a fog nest's resource, a Flame Power capsule, a campfire
    CHECK(classify(other) == Kind::None);
    for (int i = 0; i < 6; ++i) {
        Thing h = hive(1);
        if (i == 0) h.faction = true;
        if (i == 1) h.used_item = true;
        if (i == 2) h.hive_triggered = true;  // already broken open
        if (i == 3) h.disabled = true;
        if (i == 4) h.inventory = true;  // a hive that holds things (a player's beehive makes honey into one)
        if (i == 5) h.setup = true;
        CHECK(classify(h) == Kind::None);
    }
    CHECK_FALSE(SweepRules{}.group(Kind::Hive).on);
    Sweeper s;
    const auto acts = s.tick(1000, 1, me(), {hive(5)}, {}, all_on());
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].act == Act::Smash);
    CHECK(acts[0].kind == Kind::Hive);
    CHECK(acts[0].amount == 1);
    CHECK(acts[0].first);
    auto off = all_on();
    off.groups[static_cast<size_t>(Kind::Hive)].on = false;
    Sweeper s2;
    CHECK(s2.tick(1000, 1, me(), {hive(5)}, {}, off).empty());
}

TEST_CASE("sweep: a bee hive keeps the smashing distance from a base and is left to a nearer player without the magnet") {
    const auto r = all_on();  // base 30 m, smashing 10 m more
    Sweeper s;
    CHECK(s.tick(1000, 1, me(), {hive(5)}, {{-36, 0, 0}}, r).empty());  // 38 m from the altar: inside 40
    CHECK(s.tick(1000, 1, me(), {hive(6)}, {{-45, 0, 0}}, r).size() == 1);  // 47 m: smashed
    std::vector<Player> two = {{1, {0, 0, 0}, true}, {2, {2, 0, 0.5}, false}};
    Sweeper s3;
    CHECK(s3.tick(1000, 1, two, {hive(7)}, {}, r).empty());
}

// ---- 2.2: limits (Gotik0's request): how many of an item a player carries before the magnet leaves more of it ----

namespace {
Thing item(uint32_t id, const std::string& name, uint32_t item_id, int category, double x = 2) {
    Thing t = pickup(id, name, -1, x);
    t.item_id = item_id;
    t.item_category = category;
    return t;
}
LootRef ref(uint8_t b) {
    LootRef r{};
    r[0] = b;
    return r;
}
Thing plant_of(uint32_t id, uint8_t kind, double x = 2) {
    Thing t = plant(id, x);
    t.loot_ref = ref(kind);
    return t;
}
}  // namespace

TEST_CASE("sweep limits: a category's limit, an item's own line first (exact, then the longest prefix), 0 = no limit") {
    CHECK_FALSE(Limits{}.any());
    CHECK_FALSE(Limits::parse("Material_T1_Stone=0").any());  // a 0 line limits nothing
    Limits l = Limits::parse(" Material_T1_Wood = 200, Food_T1_raw_*=20, food_t1_raw_meat*=5 ,Material_T1_Stone=0, "
                             "bad, x=, =3, y=abc, z=-4, w=12x");
    CHECK(l.any());  // own lines alone
    CHECK(l.own_lines() == 4);  // the bad entries are left out
    l.materials = 50;
    l.consumables = 10;
    l.other = 3;
    CHECK(l.of({1, "Material_T1_Wood", 9}) == 200);
    CHECK(l.of({2, "Material_T1_Metal_Scraps", 9}) == 50);
    CHECK(l.of({3, "Material_T1_Stone", 9}) == 0);  // its own 0: no limit, though materials have one
    CHECK(l.of({4, "Food_T1_raw_berries", 7}) == 20);
    CHECK(l.of({5, "Food_T1_raw_meat_LeanMeat", 7}) == 5);  // the longer prefix
    CHECK(l.of({6, "Potion_T1_Health", 7}) == 10);
    CHECK(l.of({7, "Ammo_T1_Arrow", 8}) == 0);  // arrows: no limit set
    CHECK(l.of({8, "Collection_T1_Coin_01", 12}) == 3);  // everything else
    CHECK(l.of({9, "Unknown", -1}) == 0);  // its category not read: only an own line limits it
}

TEST_CASE("sweep limits: a ground item at its limit stays (no try spent); below it, it comes") {
    Sweeper s;
    SweepRules r = all_on();
    r.limits.materials = 50;
    std::vector<Player> p = {{1, {0, 0, 0}, true, true}};
    p[0].carried[0x10] = 50;
    const std::vector<Thing> wood = {item(10, "Material_T1_Wood", 0x10, 9)};
    for (uint64_t t = 1000; t <= 10000; t += 1000) CHECK(s.tick(t, 1, p, wood, {}, r).empty());
    p[0].carried[0x10] = 49;  // used some: the next one comes (a stack comes whole, so 49 + 5 makes 54)
    auto acts = s.tick(11000, 1, p, wood, {}, r);
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].act == Act::Loot);
    CHECK(acts[0].first);
    // stone has its own count: it comes while wood is at the limit
    p[0].carried[0x10] = 80;
    CHECK(s.tick(12000, 1, p, {item(11, "Material_T1_Stone", 0x11, 9)}, {}, r).size() == 1);
    // no limit for its category: it comes whatever the player carries
    p[0].carried[0x12] = 9999;
    CHECK(s.tick(13000, 1, p, {item(12, "Food_T1_raw_berries", 0x12, 7)}, {}, r).size() == 1);
}

TEST_CASE("sweep limits: chests are emptied and barrels smashed whatever the limits") {
    Sweeper s;
    SweepRules r = all_on();
    r.limits = Limits::parse("Material_T1_Metal_Scraps=1");
    r.limits.materials = r.limits.consumables = r.limits.ammo = r.limits.other = 1;
    std::vector<Player> p = {{1, {0, 0, 0}, true, true}};
    p[0].carried[0x33] = 500;
    Thing c = chest(20);
    c.stacks = {"Material_T1_Metal_Scraps"};
    c.last_accept_guest = 1;  // this player opened it
    auto acts = s.tick(1000, 1, p, {c, barrel(21, 3)}, {}, r);
    REQUIRE(acts.size() == 2);
    CHECK(acts[0].act == Act::Loot);
    CHECK(acts[1].act == Act::Smash);
}

TEST_CASE("sweep limits: an item at the nearest player's limit goes to the next player in reach below it") {
    Sweeper s;
    SweepRules r = all_on();
    r.limits.materials = 50;
    std::vector<Player> two = {{1, {0, 0, 0}, true, true}, {2, {6, 0, 0}, true, true}};
    two[0].carried[0x10] = 50;
    two[1].carried[0x10] = 10;
    auto acts = s.tick(1000, 1, two, {item(10, "Material_T1_Wood", 0x10, 9, 2)}, {}, r);
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].player == 2);
    two[1].carried[0x10] = 60;  // both have enough: it stays
    Sweeper t;
    CHECK(t.tick(1000, 1, two, {item(10, "Material_T1_Wood", 0x10, 9, 2)}, {}, r).empty());
}

TEST_CASE("sweep limits: what a plant gives is learned from the game's pickups; then one of its kind at the limit stays") {
    Sweeper s;
    SweepRules r = all_on();
    r.limits.consumables = 10;
    std::vector<Player> p = {{1, {0, 0, 0}, true, true}};
    p[0].carried[0x20] = 10;
    // not known yet: picked, so the magnet learns what this kind gives
    REQUIRE(s.tick(1000, 1, p, {plant_of(1, 5)}, {}, r).size() == 1);
    s.picked(1, {0x20, "Food_T1_raw_berries", 7}, 1100);
    CHECK(s.items_learned() == 1);
    CHECK(s.tick(2000, 1, p, {plant_of(2, 5)}, {}, r).empty());      // another plant of the kind: enough berries
    CHECK(s.tick(2000, 1, p, {plant_of(3, 6)}, {}, r).size() == 1);  // another kind: not known, picked
    p[0].carried[0x20] = 3;
    CHECK(s.tick(3000, 1, p, {plant_of(2, 5)}, {}, r).size() == 1);  // below the limit again
}

TEST_CASE("sweep limits: a plant that gives two items comes while one of them is below its limit") {
    Sweeper s;
    SweepRules r = all_on();
    r.limits.materials = 50;
    r.limits.consumables = 10;
    std::vector<Player> p = {{1, {0, 0, 0}, true, true}};
    REQUIRE(s.tick(1000, 1, p, {plant_of(1, 7)}, {}, r).size() == 1);
    s.picked(1, {0x20, "Food_T1_raw_berries", 7}, 1100);
    s.picked(1, {0x21, "Material_T1_Plant_Fiber", 9}, 1100);
    s.picked(1, {0x21, "Material_T1_Plant_Fiber", 9}, 1150);  // the same item twice: one entry
    CHECK(s.items_learned() == 2);
    p[0].carried[0x20] = 10;
    CHECK(s.tick(2000, 1, p, {plant_of(2, 7)}, {}, r).size() == 1);  // fiber below its limit
    p[0].carried[0x21] = 50;
    CHECK(s.tick(3000, 1, p, {plant_of(3, 7)}, {}, r).empty());  // both at their limits
}

TEST_CASE("sweep limits: only the magnet's own loot teaches (soon after it was sent); a new session forgets the waits") {
    Sweeper s;
    SweepRules r = all_on();
    r.limits.consumables = 10;
    std::vector<Player> p = {{1, {0, 0, 0}, true, true}};
    s.picked(9, {0x20, "Food_T1_raw_berries", 7}, 500);  // from something the magnet did not loot
    CHECK(s.items_learned() == 0);
    REQUIRE(s.tick(1000, 1, p, {plant_of(1, 5)}, {}, r).size() == 1);
    s.picked(1, {0x20, "Food_T1_raw_berries", 7}, 1000 + Yields::kWaitMs + 1);  // too late: not this loot's
    CHECK(s.items_learned() == 0);
    REQUIRE(s.tick(20000, 1, p, {plant_of(2, 5)}, {}, r).size() == 1);
    s.tick(21000, 2, p, {}, {}, r);  // a new session: the ids mean other entities now
    s.picked(2, {0x20, "Food_T1_raw_berries", 7}, 21100);
    CHECK(s.items_learned() == 0);
}

TEST_CASE("sweep limits: the never-pick-up list covers a plant once its items are known; a pickup's item id counts") {
    Sweeper s;
    SweepRules r = all_on();
    r.ignore = Ignore::parse("Food_T1_raw_*");
    std::vector<Player> p = {{1, {0, 0, 0}, true, true}};
    REQUIRE(s.tick(1000, 1, p, {plant_of(1, 5)}, {}, r).size() == 1);
    s.picked(1, {0x20, "Food_T1_raw_berries", 7}, 1100);
    CHECK(s.tick(2000, 1, p, {plant_of(2, 5)}, {}, r).empty());
    // an own line for an item and no category limits: the pickup's item id is what is counted
    r.ignore = Ignore{};
    r.limits = Limits::parse("Material_T1_Wood=5");
    p[0].carried[0x10] = 5;
    CHECK(s.tick(3000, 1, p, {item(10, "Material_T1_Wood", 0x10, -1)}, {}, r).empty());
    p[0].carried[0x10] = 4;
    CHECK(s.tick(4000, 1, p, {item(10, "Material_T1_Wood", 0x10, -1)}, {}, r).size() == 1);
}

// Seen in game: a field of 8 plants of kinds not known yet was taken whole in one sweep, before the game's first
// PickupEvent could tell what they give.
TEST_CASE("sweep limits: of a kind not known yet, one plant at a time is picked until the game's pickup teaches it") {
    SweepRules r = all_on();
    r.limits.consumables = 10;
    std::vector<Player> p = {{1, {0, 0, 0}, true, true}};
    p[0].carried[0x20] = 10;
    const std::vector<Thing> field = {plant_of(1, 5, 2), plant_of(2, 5, 3), plant_of(3, 5, 4), plant_of(4, 6, 5)};
    Sweeper s;
    auto acts = s.tick(1000, 1, p, field, {}, r);
    REQUIRE(acts.size() == 2);  // the nearest of each kind
    CHECK(acts[0].entity == 1);
    CHECK(acts[1].entity == 4);
    CHECK(s.tick(1500, 1, p, {field[1], field[2]}, {}, r).empty());  // kind 5 is being learned
    s.picked(1, {0x20, "Food_T1_raw_berries", 7}, 1600);
    CHECK(s.tick(2000, 1, p, {field[1], field[2]}, {}, r).empty());  // learned: enough berries
    // nothing came from it (the game told nothing): its own retry 2 s later, not the next one; after the wait, the
    // next one of the kind is tried
    Sweeper t;
    REQUIRE(t.tick(1000, 1, p, field, {}, r).size() == 2);
    CHECK(t.tick(1500, 1, p, {field[1]}, {}, r).empty());
    acts = t.tick(3100, 1, p, {field[0], field[1]}, {}, r);
    REQUIRE(acts.size() == 1);
    CHECK(acts[0].entity == 1);
    CHECK(t.tick(3100 + Yields::kWaitMs, 1, p, {field[1]}, {}, r).empty());  // the retry waits again
    CHECK(t.tick(3100 + Yields::kWaitMs + 1, 1, p, {field[1]}, {}, r).size() == 1);
    // with the never-pick-up list alone the same; with neither, a field comes at once as before
    SweepRules ig = all_on();
    ig.ignore = Ignore::parse("Food_T1_raw_*");
    Sweeper u;
    CHECK(u.tick(1000, 1, p, field, {}, ig).size() == 2);
    Sweeper v;
    CHECK(v.tick(1000, 1, p, field, {}, all_on()).size() == 4);
}
