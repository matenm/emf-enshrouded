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
    Thing empty = chest(21);
    CHECK(s.tick(1000, 1, me(), {empty}, {}, r).size() == 1);   // the accept
    CHECK(s.tick(4500, 1, me(), {empty}, {}, r).empty());       // nothing rolled yet: asked again later
    Thing taken = chest(22);
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
