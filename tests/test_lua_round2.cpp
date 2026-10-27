// Grave Keeper (mods/grave), run by the Lua host from a copy of the shipped folder, with fake engine
// events, components and commands (tests/lua_mod_rig.h).
#include "lua_mod_rig.h"

// ================================ Grave Keeper ================================
namespace {
constexpr uint32_t kLogs = 0x2b0cf1d5, kFibre = 0xc5a97b68, kRunes = 0x41a792dc;

// Player 1 (account 0xAA, the host) at the origin; player 2 (0xBB) 50 m east.
struct GraveRig : ModRig {
    explicit GraveRig(const std::map<std::string, std::string>& settings = {})
        : ModRig("grave", L"grave", settings, [](Fake& f) {
              add_player(f, 1, 0xAA, 0, 10, 0);
              add_player(f, 2, 0xBB, 50, 10, 0);
              for (uint32_t p : {1u, 2u}) component<ecs::NetworkHealth>(f, p).health = 60;  // up (alive)
              f.item_names[kLogs] = "Material_T1_Wood_Logs";
              f.item_names[kFibre] = "Material_T1_Plant_Fibre";
              f.item_names[kRunes] = "Material_T1_Token_WeaponUpgrade_Rune01";
          }) {}
    // Player `id` dies where it stands; the game leaves tombstone `tomb` there `after` seconds later.
    void die(uint32_t who, uint32_t tomb, double dx = 1, int after = 2) {
        deliver(r.fake, "EntityDiedEvent", died_event(who));
        seconds(after);
        auto* t = reinterpret_cast<ecs::CurrentTransform*>(r.fake.components[{ecs::CurrentTransform::kHash, who}].data());
        add_tombstone(r.fake, tomb, t->transform.position.x / 4294967296.0 + dx, t->transform.position.y / 4294967296.0,
                      t->transform.position.z / 4294967296.0, {{kLogs, 359}, {kFibre, 20}});
        seconds(3);
    }
};
}  // namespace

TEST_CASE("grave: a tombstone that appears where a player died is theirs; grave.where says how far, which way and what is in it") {
    GraveRig k;
    k.die(1, 2900);
    place(k.r.fake, 1, 1, 10, -99);  // the player respawned 99 m south of it (north = +z, the champion's calibration)
    CHECK(k.reply("grave.where") == "Your tombstone: 99 m N, 2 stacks. /grave.claim brings them back (within 150 m).");
    // the other player has none
    CHECK(k.reply("grave.where", 2, 0xBB).rfind("No tombstone of yours is known", 0) == 0);
    // the host sees it on screen
    k.seconds(1);
    CHECK(k.hud("track") == "Tombstone: 99 m N (2 stacks)");
    // the record is kept with the world, by account
    CHECK(slurp(k.data).find("grave 0x00000000000000aa ") != std::string::npos);
}

TEST_CASE("grave: never someone else's tombstone: one already there at the death, one far away, or another player's") {
    GraveRig k;
    add_tombstone(k.r.fake, 2800, 2, 10, 0, {{kLogs, 5}});  // an older tombstone right where player 1 dies
    add_tombstone(k.r.fake, 2801, 40, 10, 0, {{kLogs, 5}}); // one 40 m away
    deliver(k.r.fake, "EntityDiedEvent", died_event(1));
    k.seconds(10);
    CHECK(k.reply("grave.where").rfind("No tombstone of yours is known", 0) == 0);
    k.die(2, 2902);  // player 2's own
    // player 1 cannot claim player 2's, also standing next to it
    place(k.r.fake, 1, 51, 10, 0);
    CHECK(k.reply("grave.claim").rfind("No tombstone of yours is known", 0) == 0);
    k.send_events();
    CHECK(loot_events(k.r.fake).empty());
    // player 2 can
    CHECK(k.reply("grave.claim", 2, 0xBB).rfind("Claiming your tombstone", 0) == 0);
    k.send_events();
    REQUIRE(loot_events(k.r.fake).size() == 1);
    CHECK(loot_events(k.r.fake)[0] == std::make_pair(2u, 2902u));
}

TEST_CASE("grave: grave.claim sends the game's loot event (the sender, the tombstone, lootAll, bypass); a cooldown follows once items came") {
    GraveRig k;
    k.die(1, 2900);
    place(k.r.fake, 1, 0, 10, 100);
    CHECK(k.reply("grave.claim") == "Claiming your tombstone 100 m away: its 2 stacks come to you.");
    k.send_events();
    REQUIRE(loot_events(k.r.fake).size() == 1);
    CHECK(loot_events(k.r.fake)[0] == std::make_pair(1u, 2900u));
    // a second claim while that one is under way waits
    CHECK(k.reply("grave.claim").rfind("Your tombstone is being claimed", 0) == 0);
    // the game moves the stacks (PickupEvent by the player from the tombstone) and destroys the emptied tombstone
    deliver(k.r.fake, "PickupEvent", pickup_event(1, 2900, kLogs, 359));
    deliver(k.r.fake, "PickupEvent", pickup_event(1, 2900, kFibre, 20));
    remove_entity(k.r.fake, 2900);
    k.seconds(6);
    CHECK(k.noted("Grave Keeper: 2 stacks back from your tombstone"));
    CHECK(k.reply("grave.where").rfind("No tombstone of yours is known", 0) == 0);
    // the next death: claiming again within the cooldown (10 minutes of play) is refused
    k.die(1, 2901);
    CHECK(k.reply("grave.claim").rfind("You can claim again in 10 min", 0) == 0);
    k.seconds(9 * 60);
    CHECK(k.reply("grave.claim").rfind("You can claim again in 1 min", 0) == 0);
    k.seconds(60);
    CHECK(k.reply("grave.claim").rfind("Claiming your tombstone", 0) == 0);
    // the wait follows the setting as it is now: the host lowers it and the next claim may come at once
    deliver(k.r.fake, "PickupEvent", pickup_event(1, 2901, kLogs, 359));
    remove_entity(k.r.fake, 2901);  // emptied: the game removes it
    k.seconds(6);
    k.die(1, 2903);
    CHECK(k.reply("grave.claim").rfind("You can claim again in 10 min", 0) == 0);
    k.r.fake.settings["grave.claim_cooldown_minutes"] = "0";
    CHECK(k.reply("grave.claim").rfind("Claiming your tombstone", 0) == 0);
}

TEST_CASE("grave: beyond the claim range it is refused; range 0 = from anywhere; claiming off answers so") {
    {
        GraveRig k({{"claim_radius", "50"}});
        k.die(1, 2900);
        place(k.r.fake, 1, 0, 10, 100);
        CHECK(k.reply("grave.claim") == "Your tombstone is 100 m away: come within 50 m to claim it.");
        k.send_events();
        CHECK(loot_events(k.r.fake).empty());
    }
    {
        GraveRig k({{"claim_radius", "0"}});
        k.die(1, 2900);
        place(k.r.fake, 1, 0, 10, 1900);
        CHECK(k.reply("grave.claim").rfind("Claiming your tombstone 1900 m away", 0) == 0);
        CHECK(k.reply("grave.where").find("from anywhere") != std::string::npos);
    }
    {
        GraveRig k({{"claim", "0"}});
        k.die(1, 2900);
        CHECK(k.reply("grave.claim") == "Claiming tombstones is switched off here (grave.where still finds yours).");
    }
}

TEST_CASE("grave: nothing came (a full inventory): no cooldown, and a paid cost comes back") {
    GraveRig k({{"claim_cost_item", "Material_T1_Token_WeaponUpgrade_Rune01"}, {"claim_cost_count", "5"}});
    k.die(1, 2900);
    // not enough runes: refused before anything is taken
    CHECK(k.reply("grave.claim") == "Claiming costs 5 x Material_T1_Token_WeaponUpgrade_Rune01; you have 0.");
    CHECK(k.r.fake.takes.empty());
    // 7 runes in the backpack (an inventory the player's InventorySetup links)
    auto& setup = component<ecs::InventorySetup>(k.r.fake, 1);
    setup.linksEntities[0].id = 70;
    auto& inv = component<ecs::Inventory>(k.r.fake, 70);
    inv.slots[0].id.value = kRunes;
    inv.slots[0].data.count = 7;
    CHECK(k.reply("grave.claim") == "Claiming your tombstone 1 m away for 5 x Material_T1_Token_WeaponUpgrade_Rune01: its 2 stacks come to you.");
    REQUIRE(k.r.fake.takes.size() == 1);
    const EmfItemsRequest take = k.r.fake.takes[0];
    CHECK(take.entity == 1u);
    CHECK(take.item == kRunes);
    CHECK(take.count == 5u);
    k.send_events();
    CHECK(loot_events(k.r.fake).empty());  // only once paid
    take.cb(kServer, take.entity, take.item, take.count, 5, take.user);
    k.send_events();
    REQUIRE(loot_events(k.r.fake).size() == 1);
    k.seconds(6);  // no PickupEvent came: nothing fitted
    REQUIRE(k.r.fake.puts.size() == 1);  // the runes go back
    CHECK(k.r.fake.puts[0].item == kRunes);
    CHECK(k.r.fake.puts[0].count == 5u);
    CHECK(k.noted("nothing came from your tombstone"));
    // the put back fits only 2: the other 3 are dropped at the player's feet
    const EmfItemsRequest back = k.r.fake.puts[0];
    REQUIRE(back.cb);
    back.cb(kServer, back.entity, back.item, back.count, 2, back.user);
    REQUIRE(k.r.fake.gives.size() == 1);
    CHECK(k.r.fake.gives[0].count == 3u);
    CHECK(k.reply("grave.claim").rfind("Claiming your tombstone", 0) == 0);  // no cooldown
}

TEST_CASE("grave: after a reload the tombstone is found again by its place (ids change); a looted one is forgotten") {
    GraveRig k;
    k.die(1, 2900);
    const std::string saved = slurp(k.data);
    REQUIRE(saved.find("grave 0x00000000000000aa") != std::string::npos);
    // the world loads again: the tombstone has another id now
    remove_entity(k.r.fake, 2900);
    add_tombstone(k.r.fake, 3500, 1, 10, 0, {{kLogs, 359}});
    k.r.fake.session = 2;
    fire_load(1);
    k.seconds(2);
    CHECK(k.reply("grave.where").rfind("Your tombstone: 1 m", 0) == 0);
    // someone opened it and took everything: the game removed it. Right after a load the place may still be streaming
    // in, and one miss proves nothing: it is forgotten after misses over 10 s, from 30 s after the load
    remove_entity(k.r.fake, 3500);
    CHECK(k.reply("grave.where").rfind("Your tombstone: 1 m", 0) == 0);  // kept: "that place is not loaded now"
    k.seconds(20);
    CHECK(slurp(k.data).find("grave 0x00000000000000aa") != std::string::npos);
    k.seconds(25);
    CHECK(k.reply("grave.where").rfind("No tombstone of yours is known", 0) == 0);
    CHECK(slurp(k.data).find("grave 0x00000000000000aa") == std::string::npos);
}

TEST_CASE("grave: a player's death with no tombstone (revived, or tombstones off) leaves nothing to claim; only `keep` per player") {
    GraveRig k({{"keep", "2"}});
    deliver(k.r.fake, "EntityDiedEvent", died_event(1));
    k.seconds(301);  // no tombstone appeared within 5 minutes
    CHECK(slurp(k.data).find("grave 0x00000000000000aa") == std::string::npos);
    k.die(1, 2900);
    place(k.r.fake, 1, 30, 10, 0);
    k.die(1, 2901);
    place(k.r.fake, 1, 60, 10, 0);
    k.die(1, 2902);  // the third: the oldest goes from the list
    std::string text = slurp(k.data);
    size_t n = 0;
    for (size_t at = text.find("grave 0x00000000000000aa"); at != std::string::npos; at = text.find("grave 0x00000000000000aa", at + 1)) ++n;
    CHECK(n == 2);
    CHECK(k.reply("grave.where").find("1 older") != std::string::npos);  // the newest first, then the count of others
    // grave.claim takes the newest
    CHECK(k.reply("grave.claim").rfind("Claiming your tombstone 1 m away", 0) == 0);
    k.send_events();
    CHECK(loot_events(k.r.fake).back() == std::make_pair(1u, 2902u));
}

TEST_CASE("grave: grave.adopt (admin) gives the nearest tombstone within 10 m to the one who types it; grave.list lists them") {
    GraveRig k;
    add_tombstone(k.r.fake, 2800, 3, 10, 4, {{kLogs, 5}});  // from before Grave Keeper ran
    // the world is searched a slice at a time, never all at once in a command: the first ask starts a search
    CHECK(k.reply("grave.adopt").rfind("No tombstone within 10 m of you (as last searched", 0) == 0);
    k.seconds(1);
    CHECK(k.reply("grave.adopt") == "Tombstone 2800 (5 m from you, 1 stack) is yours now: /grave.claim brings it back.");
    CHECK(k.reply("grave.where").rfind("Your tombstone: 5 m", 0) == 0);
    CHECK(k.reply("grave.list").find("0x00000000000000aa: 5 m from you, 1 stack") != std::string::npos);
    // nothing near
    place(k.r.fake, 1, 500, 10, 0);
    CHECK(k.reply("grave.adopt").rfind("No tombstone within 10 m of you", 0) == 0);
    // only the players' commands are for every player
    bool where = false, claim = false, adopt_players = false;
    for (const auto& c : k.r.fake.commands) {
        where |= c.name == "grave.where" && c.flags == EMF_COMMAND_PLAYERS;
        claim |= c.name == "grave.claim" && c.flags == EMF_COMMAND_PLAYERS;
        adopt_players |= c.name == "grave.adopt" && c.flags == EMF_COMMAND_PLAYERS;
    }
    CHECK(where);
    CHECK(claim);
    CHECK_FALSE(adopt_players);
}

TEST_CASE("grave: a fresh tombstone (its stacks in linked inventories, as seen in game) is claimed stack by stack; gear stays") {
    GraveRig k;
    deliver(k.r.fake, "EntityDiedEvent", died_event(1));
    k.seconds(2);
    add_tombstone(k.r.fake, 2950, 0, 10, 1, {{kLogs, 250}, {kLogs, 180}, {kFibre, 3}}, true);
    reinterpret_cast<ecs::Inventory*>(k.r.fake.components[{ecs::Inventory::kHash, 2951}].data())->slots[3] = {};
    auto& gear = reinterpret_cast<ecs::Inventory*>(k.r.fake.components[{ecs::Inventory::kHash, 2951}].data())->slots[3];
    gear.id.value = kRunes;  // stands in for a gear item: it has its own data entity (pide)
    gear.data.count = 1;
    gear.data.pide.id = 7777;
    k.seconds(3);
    CHECK(k.reply("grave.where").rfind("Your tombstone: 1 m N, 4 stacks", 0) == 0);
    CHECK(k.reply("grave.claim") ==
          "Claiming your tombstone 1 m away: its 3 stacks come to you (1 piece of gear stays: take it from the tombstone by hand).");
    k.send_events();
    CHECK(loot_events(k.r.fake).empty());  // the game's loot event takes only a tombstone's own Inventory: none here
    REQUIRE(k.r.fake.takes.size() == 3);   // the plain stacks, out of the tombstone
    for (const auto& t : k.r.fake.takes) CHECK(t.entity == 2950u);
    for (size_t i = 0; i < 3; ++i) {
        const EmfItemsRequest t = k.r.fake.takes[i];
        t.cb(kServer, t.entity, t.item, t.count, t.count, t.user);
    }
    REQUIRE(k.r.fake.puts.size() == 3);    // into the player
    for (const auto& pu : k.r.fake.puts) CHECK(pu.entity == 1u);
    const EmfItemsRequest last = k.r.fake.puts[2];
    for (size_t i = 0; i < 2; ++i) {
        const EmfItemsRequest pu = k.r.fake.puts[i];
        pu.cb(kServer, pu.entity, pu.item, pu.count, pu.count, pu.user);
    }
    last.cb(kServer, last.entity, last.item, last.count, 1, last.user);  // only 1 of the 3 fibre fits
    REQUIRE(k.r.fake.gives.size() == 1);   // the other 2 are dropped at the player's feet
    CHECK(k.r.fake.gives[0].count == 2u);
    remove_entity(k.r.fake, 2950);  // the game removes the emptied tombstone (the gear stays in this fake)
    k.seconds(6);
    CHECK(k.noted("Grave Keeper: 3 stacks back from your tombstone"));
}

TEST_CASE("grave: two players die near each other: each new tombstone goes to the death nearest to it, whichever appears first") {
    GraveRig k;
    place(k.r.fake, 2, 8, 10, 0);  // player 2 stands 8 m from player 1
    deliver(k.r.fake, "EntityDiedEvent", died_event(1));
    deliver(k.r.fake, "EntityDiedEvent", died_event(2));
    k.seconds(2);
    add_tombstone(k.r.fake, 2902, 8.5, 10, 0, {{kLogs, 7}});  // player 2's appears first
    k.seconds(3);
    add_tombstone(k.r.fake, 2901, 0.5, 10, 0, {{kLogs, 9}, {kFibre, 1}});
    k.seconds(3);
    CHECK(k.reply("grave.where").rfind("Your tombstone: 1 m E, 2 stacks", 0) == 0);
    CHECK(k.reply("grave.where", 2, 0xBB).rfind("Your tombstone: 1 m E, 1 stack", 0) == 0);
}

TEST_CASE("grave: a tombstone about as near to two deaths is nobody's (grave.adopt hands it over)") {
    GraveRig k;
    place(k.r.fake, 2, 1, 10, 0);
    deliver(k.r.fake, "EntityDiedEvent", died_event(1));
    deliver(k.r.fake, "EntityDiedEvent", died_event(2));
    k.seconds(2);
    add_tombstone(k.r.fake, 2903, 0.5, 10, 0.2, {{kLogs, 7}});
    k.seconds(4);
    CHECK(k.reply("grave.where").rfind("No tombstone of yours is known", 0) == 0);
    CHECK(k.reply("grave.where", 2, 0xBB).rfind("No tombstone of yours is known", 0) == 0);
    CHECK(logged("about as near to two players' deaths"));
    CHECK(k.reply("grave.adopt").rfind("Tombstone 2903", 0) == 0);
}

TEST_CASE("grave: a death waits as long as its player lies on the death screen (past 5 minutes), then 60 s after the respawn") {
    GraveRig k;
    component<ecs::NetworkHealth>(k.r.fake, 1).health = 0;  // down
    deliver(k.r.fake, "EntityDiedEvent", died_event(1));
    k.seconds(400);  // a long time on the death screen
    component<ecs::NetworkHealth>(k.r.fake, 1).health = 60;  // respawned: the game leaves the tombstone now
    add_tombstone(k.r.fake, 2960, 1, 10, 0, {{kLogs, 9}});
    k.seconds(4);
    CHECK(k.reply("grave.where").rfind("Your tombstone: 1 m E, 1 stack", 0) == 0);
    // another death: up at once, and no tombstone within 60 s of being up: it stops waiting
    place(k.r.fake, 1, 30, 10, 0);
    deliver(k.r.fake, "EntityDiedEvent", died_event(1));
    k.seconds(62);
    add_tombstone(k.r.fake, 2961, 31, 10, 0, {{kLogs, 3}});
    k.seconds(4);
    CHECK(logged("no tombstone appeared for 0x00000000000000aa's death"));
    CHECK(k.reply("grave.list").find("31 m") == std::string::npos);  // nobody's: grave.adopt hands it over
}

TEST_CASE("grave: two players' deaths 3 m apart: a tombstone not clearly nearer to one (4 m margin) is nobody's") {
    GraveRig k;
    place(k.r.fake, 2, 3, 10, 0);
    deliver(k.r.fake, "EntityDiedEvent", died_event(1));
    deliver(k.r.fake, "EntityDiedEvent", died_event(2));
    k.seconds(2);
    add_tombstone(k.r.fake, 2970, 0.5, 10, 0, {{kLogs, 7}});  // 0.5 m from one death, 2.5 m from the other
    k.seconds(4);
    CHECK(k.reply("grave.where").rfind("No tombstone of yours is known", 0) == 0);
    CHECK(k.reply("grave.where", 2, 0xBB).rfind("No tombstone of yours is known", 0) == 0);
    CHECK(logged("tombstone 2970 at"));
}

TEST_CASE("grave: a tombstone dropped from the list (keep) is never bound again, also by a death still waiting") {
    GraveRig k({{"keep", "1"}});
    component<ecs::NetworkHealth>(k.r.fake, 1).health = 0;  // down for all three deaths
    for (int i = 0; i < 3; ++i) deliver(k.r.fake, "EntityDiedEvent", died_event(1));
    k.seconds(2);
    add_tombstone(k.r.fake, 2981, 1, 10, 0, {{kLogs, 1}});
    k.seconds(3);
    add_tombstone(k.r.fake, 2982, 1, 10, 0.5, {{kLogs, 2}});
    k.seconds(3);  // 2982 is bound and 2981 drops from the list (keep 1)
    k.seconds(6);  // a third death still waits next to 2981
    size_t bound_2981 = 0;
    for (const auto& l : k.r.fake.log)
        if (l.find("tombstone 2981 (") != std::string::npos) ++bound_2981;
    CHECK(bound_2981 == 1u);
    CHECK(slurp(k.data).find("grave 0x00000000000000aa 1.00 10.00 0.50") != std::string::npos);
}

TEST_CASE("grave: the cooldown starts when the claim is sent; a tombstone the claim emptied keeps the cost, even with no pickup reported") {
    GraveRig k({{"claim_cost_item", "Material_T1_Token_WeaponUpgrade_Rune01"}, {"claim_cost_count", "5"}});
    auto& setup = component<ecs::InventorySetup>(k.r.fake, 1);
    setup.linksEntities[0].id = 70;
    auto& inv = component<ecs::Inventory>(k.r.fake, 70);
    inv.slots[0].id.value = kRunes;
    inv.slots[0].data.count = 7;
    k.die(1, 2990);
    k.reply("grave.claim");
    const EmfItemsRequest take = k.r.fake.takes[0];
    take.cb(kServer, take.entity, take.item, take.count, 5, take.user);  // paid
    k.send_events();
    REQUIRE(loot_events(k.r.fake).size() == 1);
    remove_entity(k.r.fake, 2990);  // the loot event emptied it and the game removed it; no PickupEvent named it
    k.seconds(6);
    CHECK(k.r.fake.puts.empty());  // no refund: something came
    CHECK(k.noted("back from your tombstone"));
    k.die(1, 2991);
    CHECK(k.reply("grave.claim").rfind("You can claim again in", 0) == 0);
}

TEST_CASE("grave: after a world switch no command acts until the new world's data has come (old ids mean other things)") {
    GraveRig k;
    k.die(1, 2900);
    k.r.fake.session = 2;  // another world loads; its data comes a moment later
    k.seconds(1);
    CHECK(k.reply("grave.claim") == "Grave Keeper: this world's data is not loaded yet; try again in a moment");
    CHECK(k.reply("grave.where") == "Grave Keeper: this world's data is not loaded yet; try again in a moment");
    k.send_events();
    CHECK(loot_events(k.r.fake).empty());
    fire_load(1);
    k.seconds(2);
    CHECK(k.reply("grave.where").rfind("Your tombstone: 1 m", 0) == 0);
}

TEST_CASE("grave: leftovers for a player who left are dropped at the tombstone, else at the nearest player, never lost silently") {
    GraveRig k;
    deliver(k.r.fake, "EntityDiedEvent", died_event(1));
    k.seconds(2);
    add_tombstone(k.r.fake, 2995, 0, 10, 1, {{kLogs, 250}, {kFibre, 3}}, true);
    k.seconds(3);
    k.reply("grave.claim");
    REQUIRE(k.r.fake.takes.size() == 2);
    for (size_t i = 0; i < 2; ++i) {
        const EmfItemsRequest t = k.r.fake.takes[i];
        t.cb(kServer, t.entity, t.item, t.count, t.count, t.user);
    }
    REQUIRE(k.r.fake.puts.size() == 2);
    k.r.fake.give_fails = {1};  // the player left: nothing can be dropped at them
    const EmfItemsRequest a = k.r.fake.puts[0], b = k.r.fake.puts[1];
    a.cb(kServer, a.entity, a.item, a.count, 0, a.user);  // nothing fitted
    REQUIRE(k.r.fake.gives.size() == 1);
    CHECK(k.r.fake.gives[0].player == 2995u);  // back at the tombstone
    CHECK(k.r.fake.gives[0].count == 250u);
    k.r.fake.give_fails = {1, 2995};  // the tombstone is gone too
    b.cb(kServer, b.entity, b.item, b.count, 0, b.user);
    REQUIRE(k.r.fake.gives.size() == 2);
    CHECK(k.r.fake.gives[1].player == 2u);  // the nearest player still here
    CHECK(k.r.fake.gives[1].count == 3u);
    k.r.fake.give_fails = {1, 2, 2995};
}

TEST_CASE("grave: the world is searched a slice at a time (1000 entities a step), never all at once in one step") {
    GraveRig k;
    for (uint32_t id = 10000; id < 14500; ++id) k.r.fake.entities.push_back(id);  // 4,500 other entities
    deliver(k.r.fake, "EntityDiedEvent", died_event(1));
    k.seconds(7);  // the search that gives the death its "before" (nothing near): 5 steps of 1000
    add_tombstone(k.r.fake, 20000, 1, 10, 0, {{kLogs, 9}});  // the game leaves it now
    k.seconds(3);  // the next search has not got through the 4,500 entities to it yet
    CHECK_FALSE(logged("tombstone 20000 ("));
    k.seconds(10);
    CHECK(logged("tombstone 20000 ("));
}
