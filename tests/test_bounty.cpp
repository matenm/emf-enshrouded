// Bounty Board (mods/bounty), run by the Lua host from a copy of the shipped folder, with fake engine
// events, components and commands (tests/lua_mod_rig.h).
#include "lua_mod_rig.h"

namespace {
constexpr uint32_t kLogs = 0x2b0cf1d5, kRunes = 0x41a792dc, kSword = 0x43aa0001, kFlint = 0x5e8e867a;

// Players 1 (account 0xAA, the host, level 5) and 2 (0xBB, level 5); creatures to hit and kill.
struct BountyRig : ModRig {
    explicit BountyRig(const std::map<std::string, std::string>& settings = {})
        : ModRig("bounty", L"bounty", settings, [](Fake& f) {
              add_player(f, 1, 0xAA, 0, 10, 0);
              add_player(f, 2, 0xBB, 20, 10, 0);
              for (uint32_t p : {1u, 2u}) component<ecs::Level>(f, p).dataStorage[0] = 5;
              f.item_names[kLogs] = "Material_T1_Wood_Logs";
              f.item_names[kFlint] = "Material_T1_Stone_Flint";
              f.item_names[kRunes] = "Material_T1_Token_WeaponUpgrade_Rune01";
              f.item_names[kSword] = "Weapon_T1_1H_SwordEpic_01";
              f.item_names[0x3aaa39a7] = "Material_T1_Animal_Fur";
              set_daytime(f, 12.0);
          }) {}
    // A creature of `faction` (FactionType) at `level` with `max_health`.
    void creature(uint32_t who, int faction, uint32_t level = 3, uint32_t max_health = 200) {
        place(r.fake, who, 5, 10, 5);
        component<ecs::Faction>(r.fake, who).faction = static_cast<emf::gen::FactionType>(faction);
        component<ecs::Level>(r.fake, who).dataStorage[0] = level;
        auto& h = component<ecs::NetworkHealth>(r.fake, who);
        h.health = max_health;
        h.healthMax = max_health;
        add_entity(r.fake, who);
    }
    void kill(uint32_t who, std::initializer_list<uint32_t> hitters) {
        for (uint32_t h : hitters) deliver(r.fake, "HitEvent", hit_event(h, who));
        deliver(r.fake, "EntityDiedEvent", died_event(who));
    }
    std::string board(uint32_t player = 0, uint64_t account = 0) { return reply("bounty.board", player, account); }
};
size_t count_of(const std::string& text, const std::string& part) {
    size_t n = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
    return n;
}
}  // namespace

TEST_CASE("bounty: each player gets 3 bounties from the pool for the period, the same each time they look; others get their own") {
    BountyRig b;
    const std::string mine = b.board();
    CHECK(count_of(mine, "\n") >= 3);
    CHECK(mine.find("1. ") != std::string::npos);
    CHECK(mine.find("3. ") != std::string::npos);
    CHECK(mine.find("4. ") == std::string::npos);
    CHECK(mine.find("New bounties in ") != std::string::npos);
    CHECK(b.board() == mine);  // the same list
    const std::string theirs = b.board(2, 0xBB);
    CHECK(theirs != mine);
    CHECK(theirs.find("1. ") != std::string::npos);
    // kept with the world, by account
    b.seconds(2);
    CHECK(slurp(b.data).find("p 0x00000000000000aa ") != std::string::npos);
    CHECK(slurp(b.data).find("p 0x00000000000000bb ") != std::string::npos);
}

TEST_CASE("bounty: a kill bounty counts the player's own kills of that faction (last hit); the reward waits for /bounty.claim") {
    BountyRig b({{"bounties_per_period", "1"}, {"share_kills", "0"}});
    CHECK(b.reply("bounty.give kill_vukah").rfind("Given: Slay 6 Vukah", 0) == 0);
    b.creature(3001, 3);
    b.creature(3002, 4);  // a scavenger
    b.creature(3003, 3);
    b.kill(3001, {1});
    b.kill(3002, {1});         // the wrong faction
    b.kill(3003, {1, 2});      // player 2 hit last
    CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);
    for (uint32_t id = 3010; id < 3015; ++id) {
        b.creature(id, 3);
        b.kill(id, {1});
    }
    CHECK(b.board().find("Slay 6 Vukah: done") != std::string::npos);
    CHECK(b.noted("Bounty done: Slay 6 Vukah"));
    CHECK(b.r.fake.puts.empty());  // nothing until claimed
    CHECK(b.reply("bounty.claim").rfind("Claimed: 8 x Material_T1_Token_WeaponUpgrade_Rune01", 0) == 0);
    REQUIRE(b.r.fake.puts.size() == 1);
    CHECK(b.r.fake.puts[0].entity == 1u);
    CHECK(b.r.fake.puts[0].item == kRunes);
    CHECK(b.r.fake.puts[0].count == 8u);
    CHECK(b.board().find("Slay 6 Vukah: claimed") != std::string::npos);
    CHECK(b.reply("bounty.claim") == "No finished bounty to claim.");
    CHECK(b.r.fake.puts.size() == 1u);
}

TEST_CASE("bounty: shared kills count for every player who hit it within 10 s; switched off, only the last hitter") {
    {
        BountyRig b;
        b.reply("bounty.give kill_vukah");
        b.reply("bounty.give kill_vukah", 2, 0xBB);
        b.creature(3001, 3);
        deliver(b.r.fake, "HitEvent", hit_event(2, 3001));
        b.seconds(3);
        b.kill(3001, {1});
        CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);
        CHECK(b.board(2, 0xBB).find("Slay 6 Vukah: 1/6") != std::string::npos);
        // a hit 11 s before the death is too old
        b.creature(3002, 3);
        deliver(b.r.fake, "HitEvent", hit_event(2, 3002));
        b.seconds(11);
        b.kill(3002, {1});
        CHECK(b.board(2, 0xBB).find("Slay 6 Vukah: 1/6") != std::string::npos);
        CHECK(b.board().find("Slay 6 Vukah: 2/6") != std::string::npos);
    }
    {
        BountyRig b({{"share_kills", "0"}});
        b.reply("bounty.give kill_vukah");
        b.reply("bounty.give kill_vukah", 2, 0xBB);
        b.creature(3001, 3);
        b.kill(3001, {2, 1});
        CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);
        CHECK(b.board(2, 0xBB).find("Slay 6 Vukah: 0/6") != std::string::npos);
    }
}

TEST_CASE("bounty: gathering counts what a player picks up from the world, never out of a chest, a tombstone, or a bounty reward") {
    BountyRig b({{"bounties_per_period", "1"}, {"auto_claim", "1"}});
    // the pool at the top of main.lua is live: a runes bounty is added (runes are also a reward)
    b.edit_main("local POOL = {", "\n  {id = \"gather_runes\", kind = \"gather\", match = \"rune\", name = \"Runes\", n = 20, "
                                  "reward = {{item = \"Material_T1_Animal_Fur\", count = 2}}},");
    b.reply("bounty.give gather_wood");
    component<ecs::InventorySetup>(b.r.fake, 4000);             // a chest
    add_tombstone(b.r.fake, 4001, 1, 10, 0, {{kLogs, 50}});      // a tombstone (an Inventory, no Health)
    component<ecs::PickupItem>(b.r.fake, 4002);                  // a loose drop lying in the world ...
    component<ecs::Inventory>(b.r.fake, 4002);                   // ... that holds its item in an Inventory
    component<ecs::Inventory>(b.r.fake, 4003);                   // a plant or rock whose loot sits in an Inventory
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 0, kLogs, 20));      // a tree's logs
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 4000, kLogs, 100));  // out of the chest
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 4001, kLogs, 50));   // a claimed tombstone
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 0, kFlint, 30));     // another material
    deliver(b.r.fake, "PickupEvent", pickup_event(3000, 0, kLogs, 30));   // not a player
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 4002, kLogs, 3));    // the loose drop
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 4003, kLogs, 2));    // the plant
    CHECK(b.board().find("Gather 60 Wood logs: 25/60") != std::string::npos);
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 0, kLogs, 40));
    CHECK(b.board().find("Gather 60 Wood logs: claimed") != std::string::npos);  // done, and claimed at once
    REQUIRE_FALSE(b.r.fake.puts.empty());
    // the reward arrives as a pickup from nowhere: it never counts for a bounty, also when it is the gathered item
    b.reply("bounty.give gather_runes");
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 0, kRunes, b.r.fake.puts[0].count));
    CHECK(b.board().find("Gather 20 Runes: 0/20") != std::string::npos);
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 0, kRunes, 3));  // runes found later do count
    CHECK(b.board().find("Gather 20 Runes: 3/20") != std::string::npos);
}

TEST_CASE("bounty: a champion is a strong creature (2+ levels above the player, 1000+ health); a weak kill is not") {
    BountyRig b({{"bounties_per_period", "1"}});
    b.reply("bounty.give champion");
    b.creature(3001, 3, 9, 300);   // high level, weak
    b.creature(3002, 3, 5, 2000);  // strong, the player's level
    b.kill(3001, {1});
    b.kill(3002, {1});
    CHECK(b.board().find("Slay a champion: 0/1") != std::string::npos);
    b.creature(3003, 3, 7, 1498);  // the Vukah brawler at level 7 against a level 5 player
    b.kill(3003, {1});
    CHECK(b.board().find("Slay a champion: done") != std::string::npos);
    CHECK(b.reply("bounty.claim").find("Weapon_T1_1H_SwordEpic_01") != std::string::npos);
    bool gear = false;
    for (const auto& p : b.r.fake.puts)
        if (p.item == kSword) {
            gear = true;
            CHECK(p.level == 5u);  // at the player's level
            CHECK((p.flags & EMF_ITEMS_RARITY) != 0);
            CHECK(p.rarity == 1);  // uncommon (the default)
        }
    CHECK(gear);
}

TEST_CASE("bounty: new bounties come with the period (game dawn, every N hours, bounty.reset); progress is kept across a reload") {
    {
        BountyRig b({{"reset", "game_dawn"}});
        b.reply("bounty.give kill_vukah");
        b.creature(3001, 3);
        b.kill(3001, {1});
        CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);
        CHECK(b.board().find("New bounties at dawn") != std::string::npos);
        set_daytime(b.r.fake, 23.0);
        b.seconds(1);
        set_daytime(b.r.fake, 5.9);
        b.seconds(1);
        CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);  // still the night
        set_daytime(b.r.fake, 6.1);
        b.seconds(1);
        const std::string fresh = b.board();  // a new day: a new list of 3, nothing done yet
        CHECK(fresh.find("1/6") == std::string::npos);
        CHECK(fresh.find("4. ") == std::string::npos);
    }
    {
        BountyRig b({{"reset", "hours"}, {"reset_hours", "2"}});
        b.reply("bounty.give kill_vukah");
        b.creature(3001, 3);
        b.kill(3001, {1});
        b.seconds(3600);
        CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);
        CHECK(b.board().find("New bounties in 1 h 0 min") != std::string::npos);
        b.seconds(3600);
        CHECK(b.board().find("Slay 6 Vukah: 1/6") == std::string::npos);
    }
    {
        BountyRig b;
        b.reply("bounty.give kill_vukah");
        b.creature(3001, 3);
        b.kill(3001, {1});
        b.seconds(1);
        // the world loads again in a new session: the progress comes back from the world's data
        b.r.fake.session = 2;
        fire_load(1);
        b.seconds(2);
        CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);
        CHECK(b.reply("bounty.reset") == "New bounties for everyone.");
        CHECK(b.board().find("Slay 6 Vukah: 1/6") == std::string::npos);
    }
}

TEST_CASE("bounty: /bounty.reroll swaps one bounty for another (once a period by default); /bounty.top ranks the players") {
    BountyRig b;
    const std::string before = b.board();
    CHECK(b.reply("bounty.reroll 2").rfind("Bounty 2 is now: ", 0) == 0);
    const std::string after = b.board();
    CHECK(after != before);
    CHECK(after.substr(0, after.find('\n')) == before.substr(0, before.find('\n')));  // bounty 1 stays
    CHECK(b.reply("bounty.reroll 1") == "No rerolls left until the next bounties.");
    CHECK(b.reply("bounty.reroll 9", 2, 0xBB) == "Usage: /bounty.reroll <1-3>");
    // two bounties done by player 2, one by player 1
    b.reply("bounty.give kill_vukah", 2, 0xBB);
    b.reply("bounty.give kill_scavenger", 2, 0xBB);
    b.reply("bounty.give kill_vukah");
    for (uint32_t i = 0; i < 6; ++i) {
        b.creature(3100 + i, 3);
        b.kill(3100 + i, {2});
        b.creature(3200 + i, 4);
        b.kill(3200 + i, {2});
        b.creature(3300 + i, 3);
        b.kill(3300 + i, {1});
    }
    const std::string top = b.reply("bounty.top", 2, 0xBB);
    CHECK(top.find("1. 0x00000000000000bb: 2 (you)") != std::string::npos);
    CHECK(top.find("2. 0x00000000000000aa: 1") != std::string::npos);
}

TEST_CASE("bounty: a reward that does not fit is dropped at the player's feet; the host sees the bounties on screen") {
    BountyRig b({{"bounties_per_period", "1"}});
    b.reply("bounty.give kill_vukah");
    for (uint32_t id = 3010; id < 3016; ++id) {
        b.creature(id, 3);
        b.kill(id, {1});
    }
    b.seconds(1);
    CHECK(b.hud("board").find("Vukah 6/6") != std::string::npos);
    b.reply("bounty.claim");
    REQUIRE(b.r.fake.puts.size() == 1);
    const EmfItemsRequest put = b.r.fake.puts[0];
    put.cb(kServer, put.entity, put.item, put.count, 3, put.user);  // 3 of 8 fitted
    REQUIRE(b.r.fake.gives.size() == 1);
    CHECK(b.r.fake.gives[0].count == 5u);
    CHECK(b.r.fake.gives[0].player == 1u);
    // the players' commands
    int players = 0;
    for (const auto& c : b.r.fake.commands)
        if (c.flags == EMF_COMMAND_PLAYERS) ++players;
    CHECK(players == 4);  // board, claim, reroll, top
}

TEST_CASE("bounty: items moving between inventories (a chest takes them, the rest comes back) are no gathering") {
    BountyRig b({{"bounties_per_period", "1"}});
    b.reply("bounty.give gather_wood");
    component<ecs::InventorySetup>(b.r.fake, 4000);  // a chest
    // a mod puts 30 logs into the chest (the chest's pickup from nothing) and gives the 20 that did not fit back
    deliver(b.r.fake, "PickupEvent", pickup_event(4000, 0, kLogs, 30));
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 0, kLogs, 20));
    CHECK(b.board().find("Gather 60 Wood logs: 0/60") != std::string::npos);
    b.seconds(3);
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 0, kLogs, 20));  // logs from a tree a moment later
    CHECK(b.board().find("Gather 60 Wood logs: 20/60") != std::string::npos);
}

TEST_CASE("bounty: a reward that can be neither put nor dropped (the player left) is owed, kept with the world, and comes with the next claim") {
    BountyRig b({{"bounties_per_period", "1"}});
    b.reply("bounty.give kill_vukah");
    for (uint32_t id = 3010; id < 3016; ++id) {
        b.creature(id, 3);
        b.kill(id, {1});
    }
    b.reply("bounty.claim");
    REQUIRE(b.r.fake.puts.size() == 1);
    b.r.fake.give_fails = {1};  // the player left before the answer came
    const EmfItemsRequest put = b.r.fake.puts[0];
    put.cb(kServer, put.entity, put.item, put.count, 3, put.user);  // 3 of 8 fitted; the other 5 cannot be dropped
    CHECK(b.r.fake.gives.empty());
    b.seconds(1);
    CHECK(slurp(b.data).find("owe 0x00000000000000aa Material_T1_Token_WeaponUpgrade_Rune01 5") != std::string::npos);
    CHECK(b.board().find("1 reward waiting: /bounty.claim") != std::string::npos);
    // back again: the next claim brings it
    b.r.fake.give_fails = {};
    CHECK(b.reply("bounty.claim").rfind("Claimed: 5 x Material_T1_Token_WeaponUpgrade_Rune01", 0) == 0);
    REQUIRE(b.r.fake.puts.size() == 2);
    CHECK(b.r.fake.puts[1].count == 5u);
    b.seconds(1);
    CHECK(slurp(b.data).find("owe ") == std::string::npos);
}

TEST_CASE("bounty: after a world switch no command acts until the new world's data has come") {
    BountyRig b;
    b.reply("bounty.give kill_vukah");
    b.r.fake.session = 2;
    b.seconds(1);
    CHECK(b.board() == "Bounty Board: this world's data is not loaded yet; try again in a moment");
    CHECK(b.reply("bounty.claim") == "Bounty Board: this world's data is not loaded yet; try again in a moment");
    fire_load(1);
    b.seconds(2);
    CHECK(b.board().find("1. ") != std::string::npos);
}

TEST_CASE("bounty: loose drops a mod made are no gathering: this mod's reward dropped at a full player's feet, a chest's leftovers") {
    constexpr uint32_t kFur = 0x3aaa39a7;
    BountyRig b({{"bounties_per_period", "1"}});
    b.reply("bounty.give kill_wildlife");
    for (uint32_t id = 3010; id < 3014; ++id) {
        b.creature(id, 5);
        b.kill(id, {1});
    }
    b.reply("bounty.give gather_fur");
    b.reply("bounty.give gather_wood");
    b.reply("bounty.claim");  // 6 furs; the player is full: none fit, so they are dropped at the player's feet
    REQUIRE(b.r.fake.puts.size() == 1);
    const EmfItemsRequest put = b.r.fake.puts[0];
    put.cb(kServer, put.entity, put.item, put.count, 0, put.user);
    REQUIRE(b.r.fake.gives.size() == 1);
    const uint32_t drop = 900;  // the pickup entity the fake's give_item makes
    component<ecs::PickupItem>(b.r.fake, drop);
    place(b.r.fake, drop, 0, 11, 0);
    b.seconds(30);  // the player makes room and picks them up later
    deliver(b.r.fake, "PickupEvent", pickup_event(1, drop, kFur, 6));
    CHECK(b.board().find("Gather 10 Fur: 0/10") != std::string::npos);
    component<ecs::PickupItem>(b.r.fake, 4010);  // fur lying in the world, dropped by a creature
    place(b.r.fake, 4010, 60, 10, 0);
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 4010, kFur, 2));
    CHECK(b.board().find("Gather 10 Fur: 2/10") != std::string::npos);

    // a mod puts logs into a chest (quick stack, a sorting chest); what did not fit there or back was dropped beside it
    auto& chest = component<ecs::InventorySetup>(b.r.fake, 4000);
    chest.isInitialized = true;
    place(b.r.fake, 4000, 5, 10, 5);
    deliver(b.r.fake, "PickupEvent", pickup_event(4000, 0, kLogs, 30));
    b.seconds(20);
    component<ecs::PickupItem>(b.r.fake, 4011);
    place(b.r.fake, 4011, 8, 10, 2);  // beside the chest
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 4011, kLogs, 7));
    CHECK(b.board().find("Gather 60 Wood logs: 0/60") != std::string::npos);
    component<ecs::PickupItem>(b.r.fake, 4012);
    place(b.r.fake, 4012, 80, 10, 0);  // logs dropped far from that chest: gathered
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 4012, kLogs, 3));
    CHECK(b.board().find("Gather 60 Wood logs: 3/60") != std::string::npos);
    b.seconds(120);  // long after the chest was filled, a drop beside it counts again
    component<ecs::PickupItem>(b.r.fake, 4013);
    place(b.r.fake, 4013, 6, 10, 6);
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 4013, kLogs, 5));
    CHECK(b.board().find("Gather 60 Wood logs: 8/60") != std::string::npos);
}

TEST_CASE("bounty: emf\\config\\bounty\\pool.lua (kept when EMF is installed again) replaces main.lua's pool; bad entries are skipped") {
    BountyRig b({{"bounties_per_period", "1"}});
    const std::wstring pool = b.data.substr(0, b.data.find_last_of(L"\\/")) + L"\\pool.lua";
    struct Remove {
        std::wstring path;
        ~Remove() { DeleteFileW(path.c_str()); }
    } remove{pool};
    write(pool, "-- my server's bounties\n"
                "return {\n"
                "  {id = \"gather_flint\", kind = \"gather\", match = \"flint\", name = \"Flint\", n = 10,\n"
                "   reward = {{item = \"Material_T1_Animal_Fur\", count = 3}}},\n"
                "  {id = \"kill_vukah_big\", kind = \"kill\", faction = 3, name = \"Vukah\", n = 20},\n"
                "  {id = \"no kind\", n = 5},\n"
                "  {id = \"gather_none\", kind = \"gather\", n = 5},\n"
                "}\n");
    b.r.fake.session = 2;  // the next world load reads it
    fire_load(1);
    b.seconds(2);
    CHECK(logged("2 bounties from emf\\config\\bounty\\pool.lua"));
    CHECK(logged("pool.lua: entry 3 skipped"));
    CHECK(logged("pool.lua: entry 4 skipped"));
    CHECK(b.reply("bounty.give kill_vukah").rfind("Unknown bounty; the pool has: gather_flint, kill_vukah_big", 0) == 0);
    CHECK(b.reply("bounty.give gather_flint") == "Given: Gather 10 Flint (reward: 3 x Material_T1_Animal_Fur)");
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 0, kFlint, 4));
    CHECK(b.board().find("Gather 10 Flint: 4/10") != std::string::npos);
    // a file that does not run: main.lua's pool, said in the log
    write(pool, "return {{id = \"broken\"");
    b.r.fake.session = 3;
    fire_load(1);
    b.seconds(2);
    CHECK(logged("pool.lua could not be read"));
    CHECK(b.reply("bounty.give kill_vukah").rfind("Given: Slay 6 Vukah", 0) == 0);
}

TEST_CASE("bounty: a reroll that finds no other bounty in the pool is not used up") {
    BountyRig b;
    // every bounty of the pool on the board ("bounty.give" with an unknown id lists them): nothing is left to draw
    const std::string listed = b.reply("bounty.give nothing_like_this");
    const std::string head = "Unknown bounty; the pool has: ";
    REQUIRE(listed.rfind(head, 0) == 0);
    size_t ids = 0;
    for (size_t at = head.size(); at < listed.size();) {
        size_t end = listed.find(", ", at);
        if (end == std::string::npos) end = listed.size();
        REQUIRE(b.reply("bounty.give " + listed.substr(at, end - at)).rfind("Given: ", 0) == 0);
        ++ids;
        at = end + 2;
    }
    CHECK(ids >= 10);
    const std::string before = b.board();
    CHECK(before.find("1 reroll left") != std::string::npos);
    CHECK(b.reply("bounty.reroll 1") == "The pool has no other bounty.");
    CHECK(b.board() == before);  // the same bounties, and the reroll is still there
    CHECK(b.reply("bounty.reroll 2") == "The pool has no other bounty.");  // not "No rerolls left"
}

