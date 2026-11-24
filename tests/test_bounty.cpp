// Bounty Board (mods/bounty), run by the Lua host from a copy of the shipped folder, with fake engine
// events, components and commands (tests/lua_mod_rig.h).
#include "lua_mod_rig.h"

namespace {
constexpr uint32_t kLogs = 0x2b0cf1d5, kRunes = 0x41a792dc, kSword = 0x43aa0001, kFlint = 0x5e8e867a;
// The render model of the game's notice board (villageprops_noticeboard_01): the Notice Board players build, and very
// likely the villages' boards too.
constexpr const char* kBoardModel = "0bee6531-ef51-4925-9cd3-a40d11561165";

std::map<uint32_t, bool> g_keys;  // the fake keyboard: vk -> held

// The 1.1 tests take and claim anywhere (need_board off) unless a test says otherwise.
std::map<std::string, std::string> board_off(std::map<std::string, std::string> s) {
    s.emplace("need_board", "0");
    return s;
}

// The game stores a GUID's first three groups little-endian (see read_field in src/lua_host/fields.cpp).
std::vector<uint8_t> guid_bytes(const std::string& text) {
    auto hex = [&](size_t at) { return static_cast<uint8_t>(std::stoul(text.substr(at, 2), nullptr, 16)); };
    return {hex(6), hex(4), hex(2), hex(0), hex(11), hex(9), hex(16), hex(14),
            hex(19), hex(21), hex(24), hex(26), hex(28), hex(30), hex(32), hex(34)};
}

// Players 1 (account 0xAA, the host, level 5) and 2 (0xBB, level 5); creatures to hit and kill.
struct BountyRig : ModRig {
    explicit BountyRig(const std::map<std::string, std::string>& settings = {})
        : ModRig("bounty", L"bounty", board_off(settings), [](Fake& f) {
              add_player(f, 1, 0xAA, 0, 10, 0);
              add_player(f, 2, 0xBB, 20, 10, 0);
              for (uint32_t p : {1u, 2u}) component<ecs::Level>(f, p).dataStorage[0] = 5;
              f.item_names[kLogs] = "Material_T1_Wood_Logs";
              f.item_names[kFlint] = "Material_T1_Stone_Flint";
              f.item_names[kRunes] = "Material_T1_Token_WeaponUpgrade_Rune01";
              f.item_names[kSword] = "Weapon_T1_1H_SwordEpic_01";
              f.item_names[0x3aaa39a7] = "Material_T1_Animal_Fur";
              set_daytime(f, 12.0);
          }) {
        g_keys.clear();
        host->set_key_reader([](uint32_t k) { return g_keys[k]; });
        host->set_clock([this] { return static_cast<uint64_t>(r.fake.tick) * 1000 / 60; });  // the chat limit's clock
    }
    // Player `p`'s camera at (x, y, z) looking along `yaw_deg` (0 = +z, 90 = +x), level.
    void look(uint32_t p, double x, double y, double z, double yaw_deg) {
        auto& in = component<ecs::PlayerInput>(r.fake, p);
        auto& cam = in.fromClient.camera;
        cam.position.x = static_cast<int64_t>(x * 4294967296.0);
        cam.position.y = static_cast<int64_t>(y * 4294967296.0);
        cam.position.z = static_cast<int64_t>(z * 4294967296.0);
        const double h = yaw_deg * 3.14159265358979 / 360.0;
        cam.orientation.x = 0;
        cam.orientation.y = static_cast<float>(std::sin(h));
        cam.orientation.z = 0;
        cam.orientation.w = static_cast<float>(std::cos(h));
    }
    // Presses `vk` once on the server world (this game hosts) and lets go.
    void press(uint32_t vk) {
        g_keys[vk] = true;
        host->tick(kServer);
        g_keys[vk] = false;
        host->tick(kServer);
    }
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
    // A creature on the players' side (faction 11 PlayerAlly, as the companion, Beastmaster's beasts and Necromancer's
    // undead are) that guards `guarded` through the game's guard system (DynamicEnemy01BehaviorState.guardEntityId).
    void ally(uint32_t who, uint32_t guarded, int faction = 11) {
        place(r.fake, who, 6, 10, 6);
        component<ecs::Faction>(r.fake, who).faction = static_cast<emf::gen::FactionType>(faction);
        component<ecs::DynamicEnemy01BehaviorState>(r.fake, who).guardEntityId.id = guarded;
        add_entity(r.fake, who);
    }
    // Finishes one bounty for player 1 (their first): a starter's wood.
    void finish_one() {
        reply("bounty.give start_wood");
        deliver(r.fake, "PickupEvent", pickup_event(1, 0, kLogs, 20));
    }
};
size_t count_of(const std::string& text, const std::string& part) {
    size_t n = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
    return n;
}
// The notice lines of a /bounty.board reply ("1. Gather 20 Wood logs (3 runes)"), one per line.
std::string notices_of(const std::string& board) {
    std::string out, line;
    std::istringstream in(board);
    while (std::getline(in, line))
        if (line.size() > 2 && line[0] >= '1' && line[0] <= '9' && line[1] == '.') out += line + "\n";
    return out;
}
// The "Yours n. ..." lines of a /bounty.board reply: the bounties a player holds.
std::string yours_of(const std::string& board) {
    std::string out, line;
    std::istringstream in(board);
    while (std::getline(in, line))
        if (line.rfind("Yours", 0) == 0) out += line + "\n";
    return out;
}
// The number of the notice whose line holds `text` ("" when none).
std::string notice_number(const std::string& board, const std::string& text) {
    const std::string list = notices_of(board);
    const size_t at = list.find(text);
    if (at == std::string::npos) return "";
    const size_t start = list.rfind('\n', at);
    return list.substr(start == std::string::npos ? 0 : start + 1, 1);
}
const char* const kStarters[] = {"Gather 20 Wood logs", "Gather 20 Stone", "Gather 15 Plant fibre", "Slay 2 Wildlife",
                                 "Slay 3 Shroud creatures", "Slay 2 Scavengers"};
size_t starters_in(const std::string& text) {
    size_t n = 0;
    for (const char* quick : kStarters) n += count_of(text, quick);
    return n;
}
}  // namespace

TEST_CASE("bounty: mod.json 1.2.1 has the board settings with the approved defaults") {
    const auto m = emf::core::parse_manifest(slurp((std::filesystem::path(EMF_SOURCE_DIR) / "mods/bounty/mod.json").wstring()));
    REQUIRE_MESSAGE(m.manifest, m.error);
    CHECK(m.manifest->version_text == "1.2.1");
    std::map<std::string, emf::core::SettingDef> s;
    for (const auto& d : m.manifest->settings) {
        CAPTURE(d.key);
        CHECK(d.problem.empty());
        CHECK(d.warning.empty());
        s[d.key] = d;
    }
    struct Want {
        const char* key;
        const char* def;
        double min, max;  // 0, 0: no range to check
    };
    for (const Want& w : {Want{"need_board", "1", 0, 0}, Want{"notice_boards", "1", 0, 0}, Want{"base_radius", "40", 10, 100},
                          Want{"bounties_per_period", "3", 2, 4}, Want{"offers_for", "everyone", 0, 0},
                          Want{"hold_max", "1", 1, 4}, Want{"reset", "game_days", 0, 0}, Want{"reset_days", "4", 1, 10},
                          Want{"read_key", "E", 0, 0}, Want{"read_range", "4", 2, 8}, Want{"chat_notices", "1", 0, 0},
                          Want{"rerolls", "1", 0, 3}}) {
        const std::string key = w.key;
        CAPTURE(key);
        REQUIRE(s.count(key) == 1);
        CHECK(s[w.key].default_value == w.def);
        if (w.max > 0) {
            CHECK(s[w.key].min == w.min);
            CHECK(s[w.key].max == w.max);
        }
    }
    CHECK(s["read_key"].type == emf::core::SettingType::Key);
    CHECK(s["reset_days"].when_key == "reset");
    CHECK(s["reset_days"].when_value == "game_days");
    CHECK(s["rerolls"].when_key == "offers_for");
    CHECK(s["rerolls"].when_value == "each_player");
    bool each_player = false;
    for (const auto& o : s["offers_for"].options) each_player = each_player || o.value == "each_player";
    CHECK(each_player);
    REQUIRE(m.manifest->depends.size() == 1);
    CHECK(m.manifest->depends[0].id == "lua");
    CHECK(m.manifest->depends[0].version.describe().find("1.8.0 or newer") == 0);
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
    CHECK(yours_of(b.board()).find("Slay 6 Vukah") == std::string::npos);  // claimed: no longer held
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
    CHECK(yours_of(b.board()).find("Gather 60 Wood logs") == std::string::npos);  // done, and claimed at once
    REQUIRE_FALSE(b.r.fake.puts.empty());
    // the reward arrives as a pickup from nowhere: it never counts for a bounty, also when it is the gathered item
    b.reply("bounty.give gather_runes");
    for (const auto& put : b.r.fake.puts)
        if (put.item == kRunes) deliver(b.r.fake, "PickupEvent", pickup_event(1, 0, kRunes, put.count));
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

TEST_CASE("bounty: new notices come with the period (game dawn, every N hours, bounty.reset); a taken bounty stays; progress is kept across a reload") {
    {
        BountyRig b({{"reset", "game_dawn"}});
        b.reply("bounty.give kill_vukah");
        b.creature(3001, 3);
        b.kill(3001, {1});
        CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);
        CHECK(b.board().find("New notices at dawn") != std::string::npos);
        const std::string before = notices_of(b.board());
        set_daytime(b.r.fake, 23.0);
        b.seconds(1);
        set_daytime(b.r.fake, 5.9);
        b.seconds(1);
        CHECK(notices_of(b.board()) == before);  // still the night
        set_daytime(b.r.fake, 6.1);
        b.seconds(1);
        const std::string fresh = b.board();  // a new day: new notices; the taken bounty stays
        CHECK(fresh.find("Slay 6 Vukah: 1/6") != std::string::npos);
        CHECK(fresh.find("4. ") == std::string::npos);
    }
    {
        BountyRig b({{"reset", "hours"}, {"reset_hours", "2"}});
        b.reply("bounty.give kill_vukah");
        b.creature(3001, 3);
        b.kill(3001, {1});
        b.seconds(3600);
        CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);
        CHECK(b.board().find("New notices in 1 h 0 min") != std::string::npos);
        b.seconds(3600);
        CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);
        CHECK(b.board().find("New notices in 2 h 0 min") != std::string::npos);
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
        CHECK(b.reply("bounty.reset") == "New notices for everyone.");
        CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);
    }
}

TEST_CASE("bounty: /bounty.reroll swaps one of your own notices (once a period by default); /bounty.top ranks the players") {
    BountyRig b({{"offers_for", "each_player"}});
    const std::string before = notices_of(b.board());
    CHECK(b.reply("bounty.reroll 2").rfind("Notice 2 is now: ", 0) == 0);
    const std::string after = notices_of(b.board());
    CHECK(after != before);
    CHECK(after.substr(0, after.find('\n')) == before.substr(0, before.find('\n')));  // notice 1 stays
    CHECK(b.reply("bounty.reroll 1") == "No rerolls left until the next notices.");
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
    CHECK(top.find("1. 0x00000000000000bb: 2 (you)") != std::string::npos);  // notices nobody took count for nobody
    CHECK(top.find("2. 0x00000000000000aa: 1") != std::string::npos);
}

TEST_CASE("bounty: notices that are the same for everyone cannot be rerolled") {
    BountyRig b;
    CHECK(b.reply("bounty.reroll 1") == "The notices are the same for everyone: they cannot be rerolled.");
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
    CHECK(players == 6);  // board, take, drop, claim, reroll, top
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
    CHECK(b.reply("bounty.top") == "Bounty Board: this world's data is not loaded yet; try again in a moment");
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
                "  {id = \"half\", kind = \"champion\", n = 1.5},\n"
                "}\n");
    b.r.fake.session = 2;  // the next world load reads it
    fire_load(1);
    b.seconds(2);
    CHECK(logged("2 bounties from emf\\config\\bounty\\pool.lua"));
    CHECK(logged("pool.lua: entry 3 skipped"));
    CHECK(logged("pool.lua: entry 4 skipped"));
    CHECK(logged("pool.lua: entry 5 skipped"));  // not a whole number
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
    BountyRig b({{"offers_for", "each_player"}});
    // every bounty of the pool held ("bounty.give" with an unknown id lists them): a reroll has nothing left to draw
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
    CHECK(b.board() == before);  // the same notices, and the reroll is still there
    CHECK(b.reply("bounty.reroll 2") == "The pool has no other bounty.");  // not "No rerolls left"
}


// ---- kills by a player's companion and summons (an ally's hit names the ally as its source, not the player) ----
TEST_CASE("bounty: a kill by a player's companion, Beastmaster beast or Necromancer undead counts for that player") {
    BountyRig b({{"share_kills", "0"}});
    b.reply("bounty.give kill_vukah");
    b.reply("bounty.give kill_vukah", 2, 0xBB);
    b.ally(5001, 1);  // player 1's companion
    b.ally(5002, 2);  // player 2's beast
    b.creature(3001, 3);
    b.kill(3001, {5001});
    CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);
    CHECK(b.board(2, 0xBB).find("Slay 6 Vukah: 0/6") != std::string::npos);
    b.creature(3002, 3);
    b.kill(3002, {1, 5002});  // the beast hit last: its owner gets it
    CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);
    CHECK(b.board(2, 0xBB).find("Slay 6 Vukah: 1/6") != std::string::npos);
    // never: a wild creature guarding its own leader, a hostile that follows a player, an ally guarding something
    // that is no player (the companion told to stay at a marker)
    b.creature(5003, 3);
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 5003).guardEntityId.id = 5009;
    b.ally(5004, 1, 3);   // a Vukah (faction 3) whose guard id names player 1
    b.ally(5005, 6000);   // guards a barrel
    for (uint32_t hitter : {5003u, 5004u, 5005u}) {
        b.creature(3010 + hitter, 3);
        b.kill(3010 + hitter, {hitter});
    }
    CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);
}

TEST_CASE("bounty: with shared kills, a player whose companion hit the creature in its last 10 s gets the kill too") {
    BountyRig b;
    b.reply("bounty.give kill_vukah");
    b.reply("bounty.give kill_vukah", 2, 0xBB);
    b.ally(5001, 1);
    b.creature(3001, 3);
    deliver(b.r.fake, "HitEvent", hit_event(5001, 3001));
    b.seconds(3);
    b.kill(3001, {2});
    CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);
    CHECK(b.board(2, 0xBB).find("Slay 6 Vukah: 1/6") != std::string::npos);
}

// ---- smaller first bounties (the pool's own start at Slay 8 and Gather 60) ----
TEST_CASE("bounty: the Tiny size (x0.2) makes every bounty quick to try; a pool without starters draws as before") {
    BountyRig b({{"difficulty", "tiny"}});
    CHECK(b.reply("bounty.give kill_vukah").rfind("Given: Slay 1 Vukah", 0) == 0);
    CHECK(b.reply("bounty.give gather_wood").rfind("Given: Gather 12 Wood logs", 0) == 0);
    CHECK(b.reply("bounty.give start_wood").rfind("Given: Gather 4 Wood logs", 0) == 0);
    BountyRig c;  // a pool with no starters (the one in pool.lua may have none): the first board is the pool's own
    c.edit_main("(when first hit)", "\nfor i = #POOL, 1, -1 do if POOL[i].starter then table.remove(POOL, i) end end");
    const std::string first = c.board(2, 0xBB);  // a board drawn after the edit (the host's is already drawn)
    for (const char* quick : kStarters) {
        CAPTURE(quick);
        CHECK(first.find(quick) == std::string::npos);
    }
    CHECK(first.find("3. ") != std::string::npos);
}

// ---- what a kill leaves counts as gathered: its loot spot, or a corpse turned into loot ----
TEST_CASE("bounty: fur and meat from a kill's loot spot or a corpse turned into loot count as gathered; chests and graves never") {
    BountyRig b({{"bounties_per_period", "1"}});
    b.reply("bounty.give gather_fur");
    const uint32_t kFur = 0x3aaa39a7;
    // 4100: the loot spot a kill leaves (LootContainer_Enemy_*), whether its loot is taken by hand or by Loot Magnet
    component<ecs::InventorySetup>(b.r.fake, 4100);
    component<ecs::Inventory>(b.r.fake, 4100);
    component<ecs::DefaultInventory>(b.r.fake, 4100);
    component<ecs::DestroyOnLoot>(b.r.fake, 4100);
    component<ecs::DoNotSave>(b.r.fake, 4100);
    // 4101: a corpse the game turned into loot (transform_to_loot: Health stays, InventorySetup and DestroyOnLoot come)
    component<ecs::Health>(b.r.fake, 4101);
    component<ecs::InventorySetup>(b.r.fake, 4101);
    component<ecs::DestroyOnLoot>(b.r.fake, 4101);
    // 4102: a loot chest of the world (saved: no DestroyOnLoot, no DoNotSave)
    component<ecs::InventorySetup>(b.r.fake, 4102);
    component<ecs::DefaultInventory>(b.r.fake, 4102);
    // 4103: a loot spot that carries the tombstone mark: a grave
    for (uint32_t id : {4103u}) {
        component<ecs::InventorySetup>(b.r.fake, id);
        component<ecs::Inventory>(b.r.fake, id);
        component<ecs::DefaultInventory>(b.r.fake, id);
        component<ecs::DestroyOnLoot>(b.r.fake, id);
        component<ecs::DoNotSave>(b.r.fake, id);
        component<ecs::CreateItemLoot>(b.r.fake, id);
    }
    // 4104: a player's storage chest
    component<ecs::InventorySetup>(b.r.fake, 4104);
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 4100, kFur, 3));
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 4101, kFur, 2));
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 4102, kFur, 10));
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 4103, kFur, 10));
    deliver(b.r.fake, "PickupEvent", pickup_event(1, 4104, kFur, 10));
    CHECK(b.board().find("Gather 10 Fur: 5/10") != std::string::npos);
}

TEST_CASE("bounty: a pool with fewer starters than bounties per player fills the first board from the rest of the pool") {
    for (const char* mode : {"everyone", "each_player"}) {
        CAPTURE(mode);
        BountyRig c({{"offers_for", mode}});
        c.edit_main("(when first hit)",
                    "\nfor i = #POOL, 1, -1 do if POOL[i].starter and POOL[i].id ~= \"start_wood\" then table.remove(POOL, i) end end");
        const std::string first = notices_of(c.board(2, 0xBB));
        CHECK(first.find("Gather 20 Wood logs") != std::string::npos);  // the one starter
        CHECK(first.find("3. ") != std::string::npos);                  // and two from the rest of the pool
        CHECK(first.find("4. ") == std::string::npos);
        if (std::string(mode) == "each_player") {
            // the reroll of the starter finds no other starter: it takes one from the rest of the pool
            const std::string number = notice_number(first, "Gather 20 Wood logs");
            CHECK(c.reply("bounty.reroll " + number, 2, 0xBB).rfind("Notice " + number + " is now: ", 0) == 0);
        }
    }
}

// ---- 1.2: notices on the board, bounties a player takes and holds ----
TEST_CASE("bounty: the board posts shared notices: every player sees the same list; with each_player everyone has their own") {
    {
        BountyRig b;
        const std::string mine = b.board();
        const std::string list = notices_of(mine);
        CHECK(count_of(list, "\n") == 3);
        CHECK(list.rfind("1. ", 0) == 0);
        CHECK(list.find("4. ") == std::string::npos);
        CHECK(mine.find("New notices in ") != std::string::npos);
        CHECK(b.board() == mine);                       // the same each time
        CHECK(notices_of(b.board(2, 0xBB)) == list);    // and the same for every player
    }
    {
        BountyRig b({{"offers_for", "each_player"}});
        const std::string theirs = notices_of(b.board(2, 0xBB));
        CHECK(count_of(theirs, "\n") == 3);
        CHECK(notices_of(b.board()) != theirs);
    }
}

TEST_CASE("bounty: /bounty.take makes a notice yours (hold 1 by default); only taken bounties count; /bounty.drop frees the slot") {
    BountyRig b;
    CHECK(b.board().find("You hold no bounty") != std::string::npos);
    CHECK(b.reply("bounty.take 1").rfind("Taken: ", 0) == 0);
    CHECK(b.board().find("Yours 1. ") != std::string::npos);
    CHECK(b.reply("bounty.take 2").find("You hold 1 of 1") != std::string::npos);
    CHECK(b.reply("bounty.take 9") == "Usage: /bounty.take <1-3>");
    // what nobody took counts for nobody: player 2 gathers and slays what the starter notices ask for
    deliver(b.r.fake, "PickupEvent", pickup_event(2, 0, kLogs, 30));
    b.creature(3001, 5);
    b.kill(3001, {2});
    CHECK(b.board(2, 0xBB).find("You hold no bounty") != std::string::npos);
    CHECK_FALSE(b.noted("Bounty done"));
    CHECK(b.reply("bounty.drop 1").rfind("Dropped: ", 0) == 0);
    CHECK(b.board().find("Yours 1. ") == std::string::npos);
    CHECK(b.reply("bounty.take 1").find("took that one already") != std::string::npos);  // once a period
    CHECK(b.reply("bounty.take 2").rfind("Taken: ", 0) == 0);
    BountyRig c({{"hold_max", "2"}});
    CHECK(c.reply("bounty.take 1").rfind("Taken: ", 0) == 0);
    CHECK(c.reply("bounty.take 2").rfind("Taken: ", 0) == 0);
    CHECK(c.reply("bounty.take 3").find("You hold 2 of 2") != std::string::npos);
    CHECK(c.board().find("you hold 2 of 2") != std::string::npos);
}

TEST_CASE("bounty: a taken bounty stays across new notices until finished or dropped; a held bounty cannot be taken twice") {
    BountyRig b;
    b.reply("bounty.give kill_vukah");
    b.creature(3001, 3);
    b.kill(3001, {1});
    CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);
    const std::string before = notices_of(b.board());
    bool changed = false;
    for (int i = 0; i < 5 && !changed; ++i) {  // new notices (a draw may repeat a list now and then)
        CHECK(b.reply("bounty.reset") == "New notices for everyone.");
        changed = notices_of(b.board()) != before;
    }
    CHECK(changed);
    CHECK(b.board().find("Slay 6 Vukah: 1/6") != std::string::npos);  // still yours
    // a pool of two bounties and no starters: the board offers Slay 6 Vukah again, which player 1 holds
    b.seconds(1);
    b.edit_main("(when first hit)", "\nfor i = #POOL, 1, -1 do if POOL[i].id ~= \"kill_vukah\" and POOL[i].id ~= "
                                    "\"gather_wood\" then table.remove(POOL, i) end end");
    const std::string board = b.board();
    CHECK(board.find("Slay 6 Vukah: 1/6") != std::string::npos);  // kept across the reload
    const std::string n = notice_number(board, "Slay 6 Vukah");
    REQUIRE_FALSE(n.empty());
    CHECK(b.reply("bounty.take " + n).find("You hold that bounty already") != std::string::npos);
}

TEST_CASE("bounty: players who finished none see the starter notices; after their first, the pool's own") {
    BountyRig b;
    const std::string first = notices_of(b.board());
    for (const char* big : {"Gather 60", "Gather 40", "Slay 6", "Slay 8", "champion"}) {
        CAPTURE(big);
        CHECK(first.find(big) == std::string::npos);
    }
    CHECK(starters_in(first) == 3);
    b.finish_one();
    CHECK(b.noted("Bounty done: Gather 20 Wood logs"));
    const std::string later = notices_of(b.board());
    CHECK(starters_in(later) == 0);
    CHECK(count_of(later, "\n") == 3);
    CHECK(notices_of(b.board(2, 0xBB)) == first);  // player 2 has finished none yet
    // with notices of their own, a reroll of a starter gives another starter
    BountyRig c({{"offers_for", "each_player"}});
    const std::string mine = notices_of(c.board());
    CHECK(c.reply("bounty.reroll 1").rfind("Notice 1 is now: ", 0) == 0);
    const std::string rerolled = notices_of(c.board());
    CHECK(starters_in(rerolled) == 3);
    CHECK(rerolled != mine);
}

// ---- 1.2: the world file ----

TEST_CASE("bounty: a 1.1.0 world file migrates: started and finished bounties become yours, nothing earned is lost") {
    BountyRig b({{"reset", "game_dawn"}});
    b.edit_main("local POOL = {", "\n  {id = \"gather_hemp\", kind = \"gather\", match = \"hemp\", name = \"Hemp\", n = 40, "
                                  "reward = {{item = \"Material_T1_Animal_Fur\", count = 2}}},");
    write(b.data, "offset 2\ndawns 7\nplay 900\n"
                  "p 0x00000000000000aa d2026300.2 1 4\n"
                  "b 0x00000000000000aa kill_vukah 6 2 0\n"
                  "b 0x00000000000000aa gather_wood 60 60 1\n"
                  "b 0x00000000000000aa kill_drak 4 4 2\n"
                  "b 0x00000000000000aa gone_id 5 1 0\n"
                  "b 0x00000000000000aa kill_scavenger 6 0 0\n"
                  "p 0x00000000000000bb d2026300.2 0 0\n"
                  "b 0x00000000000000bb gather_hemp 40 3 0\n"
                  "owe 0x00000000000000aa Material_T1_Animal_Fur 6 0 -\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    const std::string held = yours_of(b.board());
    CHECK(held.find("Slay 6 Vukah: 2/6") != std::string::npos);        // open: yours, with its progress
    CHECK(held.find("Gather 60 Wood logs: done") != std::string::npos);  // finished, not claimed: still to claim
    CHECK(held.find("Drak") == std::string::npos);                       // claimed in 1.1: nothing left to keep
    CHECK(held.find("Scavengers") == std::string::npos);                 // untouched (0/6): nothing earned, not held
    CHECK(count_of(held, "\n") == 2);                                    // (an id no longer in the pool is left out)
    CHECK(b.board().find("you hold 2 of 1") != std::string::npos);       // more than the hold limit: kept
    CHECK(b.board().find("1 reward waiting") != std::string::npos);
    const std::string claimed = b.reply("bounty.claim");
    CHECK(claimed.find("6 x Material_T1_Animal_Fur") != std::string::npos);
    CHECK(claimed.find("5 x Material_T1_Token_WeaponUpgrade_Rune01") != std::string::npos);
    CHECK(b.reply("bounty.top").find("1. 0x00000000000000aa: 4 (you)") != std::string::npos);
    CHECK(yours_of(b.board(2, 0xBB)).find("Gather 40 Hemp: 3/40") != std::string::npos);
    b.seconds(1);
    const std::string file = slurp(b.data);
    CHECK(file.rfind("format 2\n", 0) == 0);
    CHECK(file.find("\noffset 2\n") != std::string::npos);
    CHECK(file.find("\ndawns 7\n") != std::string::npos);
    CHECK(file.find("\nplay 90") != std::string::npos);
    CHECK(file.find("\nt 0x00000000000000aa kill_vukah 6 2 0\n") != std::string::npos);
    CHECK(file.find("\nt 0x00000000000000bb gather_hemp 40 3 0\n") != std::string::npos);
    CHECK(file.find("\nb ") == std::string::npos);
    CHECK(file.find("\nowe ") == std::string::npos);  // claimed now
    // the rewritten file loads the same
    const std::string before = b.board();
    b.r.fake.session = 3;
    fire_load(1);
    b.seconds(1);
    CHECK(b.board() == before);
}

// ---- 1.2: new notices every few in-game days ----
namespace {
// One in-game night: evening, midnight, dawn, noon (one server step each).
void night(BountyRig& b) {
    for (double h : {23.0, 2.0, 6.5, 12.0}) {
        set_daytime(b.r.fake, h);
        b.seconds(1);
    }
}
// A number line of the saved world file ("dawns 3" -> 3; -1 when there is none).
int saved_number(BountyRig& b, const std::string& name) {
    b.seconds(1);
    const std::string f = slurp(b.data);
    const size_t at = f.find("\n" + name + " ");
    return at == std::string::npos ? -1 : std::atoi(f.c_str() + at + name.size() + 2);
}
}  // namespace

TEST_CASE("bounty: new notices every reset_days in-game days at dawn; the board says how many days are left") {
    BountyRig b({{"reset_days", "2"}});
    CHECK(b.board().find("New notices in 2 days") != std::string::npos);
    const std::string first = notices_of(b.board());
    night(b);
    CHECK(b.board().find("New notices at the next dawn") != std::string::npos);
    CHECK(notices_of(b.board()) == first);
    CHECK(saved_number(b, "posted") == 0);
    night(b);
    CHECK(saved_number(b, "dawns") == 2);
    CHECK(saved_number(b, "posted") == 2);  // posted at the second dawn
    CHECK(b.board().find("New notices in 2 days") != std::string::npos);
    bool changed = notices_of(b.board()) != first;
    for (int i = 0; i < 4 && !changed; ++i) {  // a draw may repeat a list now and then
        night(b);
        night(b);
        changed = notices_of(b.board()) != first;
    }
    CHECK(changed);
}

TEST_CASE("bounty: a dawn counts once when the clock jumps past midnight to 6:00 or later") {
    BountyRig b({{"reset_days", "1"}});
    CHECK(saved_number(b, "dawns") == 0);
    set_daytime(b.r.fake, 22.0);
    b.seconds(1);
    set_daytime(b.r.fake, 8.0);  // the host sets the morning, or everyone slept through the night
    b.seconds(1);
    CHECK(saved_number(b, "dawns") == 1);
    CHECK(saved_number(b, "posted") == 1);
    set_daytime(b.r.fake, 23.9);  // a night as it runs: midnight, then 6:00
    b.seconds(1);
    set_daytime(b.r.fake, 0.1);
    b.seconds(1);
    set_daytime(b.r.fake, 6.1);
    b.seconds(1);
    CHECK(saved_number(b, "dawns") == 2);
    set_daytime(b.r.fake, 10.0);  // later the same day
    b.seconds(1);
    set_daytime(b.r.fake, 12.0);
    b.seconds(1);
    CHECK(saved_number(b, "dawns") == 2);
    CHECK(saved_number(b, "posted") == 2);
    set_daytime(b.r.fake, 7.0);  // the clock set back a few hours, then a little: no new day
    b.seconds(1);
    set_daytime(b.r.fake, 6.5);
    b.seconds(1);
    CHECK(saved_number(b, "dawns") == 2);
}

// ---- 1.2: which props are Bounty Boards ----
namespace {
// A flame altar (BaseIdComponent) at (x, 10, z).
void altar(BountyRig& b, uint32_t id, double x, double z) {
    place(b.r.fake, id, x, 10, z);
    component<ecs::BaseIdComponent>(b.r.fake, id);
    add_entity(b.r.fake, id);
}
// A placed prop with a render model (its template's ModelResource; the notice board's by default) at (x, y, z).
void prop(BountyRig& b, uint32_t id, double x, double y, double z, const char* model = kBoardModel) {
    place(b.r.fake, id, x, y, z);
    if (model) b.r.fake.templates[{ecs::ModelResource::kHash, id}] = guid_bytes(model);
    component<ecs::StaticCollider>(b.r.fake, id);
    add_entity(b.r.fake, id);
}
// Long enough for a new search of the world for boards (one every 30 s).
void search(BountyRig& b) { b.seconds(32); }
}  // namespace

TEST_CASE("bounty: a Notice Board within base_radius of a flame altar is a Bounty Board; one outside every base is not") {
    BountyRig b;
    altar(b, 900, 0, 0);
    prop(b, 950, 15, 10, 0);
    prop(b, 951, 300, 10, 0);                                         // a village's board, far from every base
    prop(b, 952, 12, 10, 0, "91e2d4cc-01c9-4203-a92e-b08769364522");  // another prop in the base
    CHECK(b.reply("bounty.boards").find("No Bounty Board in this world yet") != std::string::npos);  // not searched yet
    search(b);
    const std::string boards = b.reply("bounty.boards");
    CHECK(boards.find("1. Notice Board at 15, 10, 0 (15 m from you)") != std::string::npos);
    CHECK(boards.find("300") == std::string::npos);
    CHECK(boards.find("2. ") == std::string::npos);
    b.r.fake.settings["bounty.base_radius"] = "10";
    search(b);
    CHECK(b.reply("bounty.boards").find("No Bounty Board") != std::string::npos);
    b.r.fake.settings["bounty.base_radius"] = "40";
    b.r.fake.settings["bounty.notice_boards"] = "0";
    search(b);
    CHECK(b.reply("bounty.boards").find("No Bounty Board") != std::string::npos);
}

TEST_CASE("bounty: on the host, a board whose model only the client world has is found") {
    BountyRig b;
    altar(b, 900, 0, 0);
    prop(b, 950, 10, 10, 0);
    // render models are client-side: the model is there while the host's client world steps, not on the server's
    const auto model = b.r.fake.templates[{ecs::ModelResource::kHash, 950}];
    for (int i = 0; i < 32; ++i) {
        b.r.fake.templates[{ecs::ModelResource::kHash, 950}] = model;
        b.host->tick(kClient);
        b.r.fake.templates.erase({ecs::ModelResource::kHash, 950});
        b.seconds(1);
    }
    CHECK(b.reply("bounty.boards").find("1. Notice Board at 10, 10, 0") != std::string::npos);
}

TEST_CASE("bounty: bounty.mark makes the prop the host looks at a Bounty Board, kept with the world by its place") {
    BountyRig b;
    prop(b, 960, 0, 10, 5, nullptr);  // a sign post, say: no notice board, its model unknown here
    b.look(1, 0, 11.5, 0, 0);          // looking along +z at it
    CHECK(b.reply("bounty.mark").rfind("Marked: ", 0) == 0);
    b.seconds(1);
    CHECK(slurp(b.data).find("\nmark 0.00 10.00 5.00 -\n") != std::string::npos);
    CHECK(b.reply("bounty.boards").find("1. marked prop at 0, 10, 5") != std::string::npos);
    b.r.fake.session = 2;  // a reload: the mark comes back with the world's data, by its place
    fire_load(1);
    b.seconds(1);
    CHECK(b.reply("bounty.boards").find("1. marked prop at 0, 10, 5") != std::string::npos);
    CHECK(b.reply("bounty.mark") == "That is a Bounty Board already.");
    CHECK(b.reply("bounty.unmark").rfind("Unmarked: ", 0) == 0);
    CHECK(b.reply("bounty.boards").find("No Bounty Board") != std::string::npos);
    CHECK(b.reply("bounty.unmark").rfind("No marked prop within 8 m", 0) == 0);
    b.look(1, 0, 11.5, 0, 180);  // looking away
    CHECK(b.reply("bounty.mark").rfind("Look at a prop", 0) == 0);
    for (const auto& c : b.r.fake.commands)  // the host's and admins' commands, not every player's
        if (c.name == "bounty.mark" || c.name == "bounty.unmark" || c.name == "bounty.boards") CHECK(c.flags == 0u);
}

TEST_CASE("bounty: a marked prop that is gone loses its mark when a player was near its place, not right after a load") {
    BountyRig b;
    prop(b, 960, 0, 10, 5, nullptr);
    b.look(1, 0, 11.5, 0, 0);
    REQUIRE(b.reply("bounty.mark").rfind("Marked: ", 0) == 0);
    b.seconds(1);
    remove_entity(b.r.fake, 960);  // taken down
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(20);  // the world may still be streaming in: kept
    CHECK(b.reply("bounty.boards").find("1. marked prop at 0, 10, 5") != std::string::npos);
    b.seconds(15);  // one search after that missed it with player 1 beside its place: kept (they may have just come)
    CHECK(b.reply("bounty.boards").find("1. marked prop at 0, 10, 5") != std::string::npos);
    b.seconds(30);  // a second one in a row: gone
    CHECK(b.reply("bounty.boards").find("No Bounty Board") != std::string::npos);
    CHECK(slurp(b.data).find("\nmark ") == std::string::npos);
    // far from every player a missing prop may just not be loaded: kept
    prop(b, 961, 0, 10, 5, nullptr);
    REQUIRE(b.reply("bounty.mark").rfind("Marked: ", 0) == 0);
    remove_entity(b.r.fake, 961);
    place(b.r.fake, 1, 500, 10, 0);
    place(b.r.fake, 2, 520, 10, 0);
    b.seconds(70);
    CHECK(b.reply("bounty.boards").find("1. marked prop at 0, 10, 5") != std::string::npos);
}

// ---- 1.2: take and claim at a board ----
TEST_CASE("bounty: with need_board on, take and claim work only near a Bounty Board; off, anywhere") {
    BountyRig b({{"need_board", "1"}});
    altar(b, 900, 20, 0);
    prop(b, 950, 20, 10, 5);  // beside player 2 (at 20, 10, 0), 20 m from the host
    search(b);
    CHECK(b.reply("bounty.take 1") == "Take and claim bounties at a Bounty Board.");
    CHECK(b.board().find("Notices: at a Bounty Board.") != std::string::npos);
    CHECK(notices_of(b.board()).empty());
    CHECK(count_of(notices_of(b.board(2, 0xBB)), "\n") == 3);
    CHECK(b.reply("bounty.take 1", 2, 0xBB).rfind("Taken: ", 0) == 0);
    // a bounty finished away from the board is claimed at it
    b.reply("bounty.give kill_vukah");
    for (uint32_t id = 3010; id < 3016; ++id) {
        b.creature(id, 3);
        b.kill(id, {1});
    }
    CHECK(b.reply("bounty.claim") == "Take and claim bounties at a Bounty Board.");
    CHECK(b.r.fake.puts.empty());
    CHECK(yours_of(b.board()).find("Slay 6 Vukah: done") != std::string::npos);  // your bounties show anywhere
    place(b.r.fake, 1, 18, 10, 2);                                                // the host walks to the board
    CHECK(b.reply("bounty.claim").rfind("Claimed: ", 0) == 0);
    place(b.r.fake, 1, 0, 10, 0);
    b.reply("bounty.give kill_scavenger");
    CHECK(b.reply("bounty.drop 1").rfind("Dropped: ", 0) == 0);  // giving one up works anywhere
    b.r.fake.settings["bounty.need_board"] = "0";                // off: anywhere, as in 1.1
    CHECK(b.reply("bounty.take 2").rfind("Taken: ", 0) == 0);
    CHECK(count_of(notices_of(b.board()), "\n") == 3);
}

// ---- 1.2: the host reads a board with E ----
namespace {
int index_of(const std::vector<std::string>& items, const std::string& start) {
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].rfind(start, 0) == 0) return static_cast<int>(i);
    return -1;
}
// A base with a Notice Board 3 m in front of the host (player 1 at 0, 10, 0, looking along +z), searched.
void board_ahead(BountyRig& b) {
    altar(b, 900, 0, 0);
    prop(b, 950, 0, 10, 3);
    search(b);
    b.look(1, 0, 11.5, 0, 0);
}
}  // namespace

TEST_CASE("bounty: E at a Bounty Board opens the wheel with the notices; a pick takes it on the server's next step") {
    BountyRig b({{"need_board", "1"}});
    board_ahead(b);
    b.press('E');
    REQUIRE(b.r.fake.wheels.size() == 1);
    const auto w = b.r.fake.wheels[0];
    CHECK(w.title == "Bounty Board: new notices in 4 days");
    CHECK(w.hold == 0u);  // click to choose
    REQUIRE(w.items.size() == 3);
    const std::string first = w.items[0];
    CHECK(notices_of(b.board()).rfind("1. " + first + "\n", 0) == 0);  // the notices, as /bounty.board lists them
    w.cb(kClient, 0, w.user);  // the wheel answers on the client world
    CHECK(yours_of(b.board()).empty());
    b.seconds(1);              // the take runs on the server's next step
    CHECK(b.noted("Taken: "));
    CHECK(count_of(yours_of(b.board()), "\n") == 1);
    b.press('E');
    REQUIRE(b.r.fake.wheels.size() == 2);
    const auto& again = b.r.fake.wheels[1];
    CHECK(std::find(again.items.begin(), again.items.end(), first) == again.items.end());  // taken: off the wheel
    CHECK(index_of(again.items, "Drop: ") >= 0);
}

TEST_CASE("bounty: E with no board in front opens nothing; Claim and Drop act; a pick after new notices takes nothing") {
    BountyRig b({{"need_board", "1"}});
    board_ahead(b);
    CHECK(b.noted("Bounty Board: press E to read"));  // said when the host came up to it
    b.r.fake.notes.clear();
    b.look(1, 0, 11.5, 0, 180);  // turned away from it
    b.press('E');
    CHECK(b.r.fake.wheels.empty());
    CHECK(b.r.fake.notes.empty());  // E is the game's key too: nothing is said
    place(b.r.fake, 1, 0, 10, -6);  // 9 m away, facing it: out of reading distance
    b.look(1, 0, 11.5, -6, 0);
    b.press('E');
    CHECK(b.r.fake.wheels.empty());
    place(b.r.fake, 1, 0, 10, 0);
    b.look(1, 0, 11.5, 0, 0);
    // a pick that comes after new notices were posted takes nothing
    b.press('E');
    REQUIRE(b.r.fake.wheels.size() == 1);
    const auto stale = b.r.fake.wheels[0];
    b.reply("bounty.reset");
    stale.cb(kClient, 0, stale.user);
    b.seconds(1);
    CHECK(b.noted("The notices changed: look at the board again."));
    CHECK(yours_of(b.board()).empty());
    // Claim
    b.reply("bounty.give kill_vukah");
    for (uint32_t id = 3010; id < 3016; ++id) {
        b.creature(id, 3);
        b.kill(id, {1});
    }
    b.press('E');
    const auto w = b.r.fake.wheels.back();
    const int claim = index_of(w.items, "Claim: 8 runes");
    REQUIRE(claim >= 0);
    w.cb(kClient, claim, w.user);
    b.seconds(1);
    CHECK(b.r.fake.puts.size() == 1);
    CHECK(b.noted("Claimed: 8 x Material_T1_Token_WeaponUpgrade_Rune01"));
    // Drop
    b.reply("bounty.give kill_scavenger");
    b.press('E');
    const auto w2 = b.r.fake.wheels.back();
    const int drop = index_of(w2.items, "Drop: Scavengers 0/6");
    REQUIRE(drop >= 0);
    w2.cb(kClient, drop, w2.user);
    b.seconds(1);
    CHECK(b.noted("Dropped: Slay 6 Scavengers"));
    CHECK(yours_of(b.board()).empty());
    // a wheel closed without a choice does nothing
    b.press('E');
    const auto w3 = b.r.fake.wheels.back();
    const size_t notes = b.r.fake.notes.size();
    w3.cb(kClient, -1, w3.user);
    b.seconds(1);
    CHECK(b.r.fake.notes.size() == notes);
}

// ---- 1.2: players who joined read the board in the chat ----
namespace {
uint32_t no_local_player() { return 0; }  // a dedicated server has no player of its own
// The chat lines sent to `player` so far.
std::vector<std::string> tells_to(BountyRig& b, uint32_t player) {
    std::vector<std::string> out;
    for (const auto& [p, text] : b.r.fake.tells)
        if (p == player) out.push_back(text);
    return out;
}
}  // namespace

TEST_CASE("bounty: a player who walks up to a board gets the notices in the chat; again after 8 m away when they changed, or after 10 minutes") {
    {
        BountyRig b({{"need_board", "1"}});
        altar(b, 900, 20, 0);
        prop(b, 950, 20, 10, 4);  // 4 m in front of where player 2 will stand (20, 10, 0)
        place(b.r.fake, 2, 20, 10, -10);
        search(b);
        CHECK(b.r.fake.tells.empty());
        place(b.r.fake, 2, 20, 10, 0);  // walks up, looking at it
        b.look(2, 20, 11.5, 0, 0);
        b.seconds(2);
        REQUIRE(tells_to(b, 2).size() == 1);
        const std::string line = tells_to(b, 2)[0];
        CHECK(line.rfind("Bounty Board: your starter notices (until your first bounty is done); new notices in 4 days\n1. ", 0) == 0);
        CHECK(count_of(line, "\n") <= 9);  // at most 10 lines
        CHECK(line.find("\nType /bounty.take 1-3 here") != std::string::npos);
        b.seconds(10);                   // staying there: nothing more
        place(b.r.fake, 2, 20, 10, -1);  // 5 m away, then back
        b.seconds(2);
        place(b.r.fake, 2, 20, 10, 0);
        b.seconds(2);
        CHECK(tells_to(b, 2).size() == 1);
        place(b.r.fake, 2, 20, 10, -5);  // 9 m away and back: the same notices, not again (a hub board floods no chat)
        b.seconds(2);
        place(b.r.fake, 2, 20, 10, 0);
        b.seconds(62);
        CHECK(tells_to(b, 2).size() == 1);
        b.reply("bounty.reset");  // new notices: told at once (they went away since the last line)
        b.seconds(2);
        REQUIRE(tells_to(b, 2).size() == 2);
        place(b.r.fake, 2, 20, 10, -5);  // away and back, then 10 minutes after the last line: told again
        b.seconds(2);
        place(b.r.fake, 2, 20, 10, 0);
        b.seconds(300);
        CHECK(tells_to(b, 2).size() == 2);
        b.seconds(300);
        CHECK(tells_to(b, 2).size() == 3);
        // the host walking up gets a note (the wheel is theirs), no chat line; the note at most every 10 minutes
        place(b.r.fake, 1, 20, 10, 1);
        b.look(1, 20, 11.5, 1, 0);
        b.seconds(2);
        CHECK(b.noted("Bounty Board: press E to read"));
        CHECK(tells_to(b, 1).empty());
        b.r.fake.notes.clear();
        place(b.r.fake, 1, 20, 10, -20);
        b.seconds(70);
        place(b.r.fake, 1, 20, 10, 1);
        b.seconds(2);
        CHECK_FALSE(b.noted("press E"));
        // on a dedicated server (no player of its own) everyone reads it in the chat
        b.r.api.local_player = no_local_player;
        place(b.r.fake, 1, 20, 10, -20);
        b.seconds(2);
        place(b.r.fake, 1, 20, 10, 1);
        b.seconds(2);
        CHECK(tells_to(b, 1).size() == 1);
    }
    {
        BountyRig c({{"need_board", "1"}, {"chat_notices", "0"}});
        altar(c, 900, 20, 0);
        prop(c, 950, 20, 10, 4);
        search(c);
        c.look(2, 20, 11.5, 0, 0);
        c.seconds(2);
        CHECK(c.r.fake.tells.empty());
    }
}

TEST_CASE("bounty: many players at once: the lines wait and never pass 30 in 10 s") {
    BountyRig b({{"need_board", "1"}});
    altar(b, 900, 50, 50);
    prop(b, 950, 50, 10, 50);
    search(b);
    REQUIRE(b.r.fake.tells.empty());
    for (uint32_t p = 2; p <= 9; ++p) add_player(b.r.fake, p, 0xC0 + p, 49.0 + p % 3, 10, 47.0 + 0.2 * p);
    std::vector<std::pair<int, int>> sent;  // (second, lines)
    for (int s = 0; s < 40; ++s) {
        const size_t before = b.r.fake.tells.size();
        b.seconds(1);
        for (size_t i = before; i < b.r.fake.tells.size(); ++i)
            sent.push_back({s, 1 + static_cast<int>(count_of(b.r.fake.tells[i].second, "\n"))});
    }
    REQUIRE(sent.size() == 8);
    CHECK(sent.back().first >= 10);  // 40 lines: the last ones waited for the next 10 s
    for (const auto& [at, lines] : sent) {
        int in_window = 0;
        for (const auto& [at2, lines2] : sent)
            if (at2 >= at && at2 < at + 10) in_window += lines2;
        CHECK(in_window <= 30);
    }
    for (uint32_t p = 2; p <= 9; ++p) {
        CAPTURE(p);
        CHECK(tells_to(b, p).size() == 1);  // every one of them, once
    }
    CHECK_FALSE(logged("refused"));
}

TEST_CASE("bounty: a player who joined is told when a bounty of theirs is done") {
    BountyRig b;
    b.reply("bounty.give kill_vukah", 2, 0xBB);
    for (uint32_t id = 3010; id < 3016; ++id) {
        b.creature(id, 3);
        b.kill(id, {2});
    }
    b.seconds(1);
    const auto lines = tells_to(b, 2);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].rfind("Bounty done: Slay 6 Vukah!", 0) == 0);
    CHECK_FALSE(b.noted("Bounty done"));  // the host's note is for the host's own bounties
}

TEST_CASE("bounty: a wheel pick made just before a world switch does nothing in the next world") {
    BountyRig b({{"need_board", "1"}});
    board_ahead(b);
    b.press('E');
    REQUIRE(b.r.fake.wheels.size() == 1);
    const auto w = b.r.fake.wheels[0];
    w.cb(kClient, 0, w.user);  // picked; the world changes before the server's next step
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    CHECK_FALSE(b.noted("Taken: "));
    CHECK(yours_of(b.board()).empty());
}

// ---- 1.2: the world file from 1.1 and for 1.1 ----
TEST_CASE("bounty: a 1.1 player's untouched bounties are not held after the update: they can take a notice at once") {
    BountyRig b({{"reset", "game_dawn"}});
    write(b.data, "offset 0\ndawns 2\nplay 100\np 0x00000000000000aa d2026300.0 0 0\n"
                  "b 0x00000000000000aa kill_vukah 6 0 0\nb 0x00000000000000aa gather_wood 60 0 0\n"
                  "b 0x00000000000000aa champion 1 0 0\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    CHECK(b.board().find("You hold no bounty") != std::string::npos);
    CHECK(b.reply("bounty.take 1").rfind("Taken: ", 0) == 0);
}

TEST_CASE("bounty: a world file from a newer Bounty Board is read but never written over") {
    BountyRig b;
    const std::string newer = "format 3\noffset 0\nseed 7\nposted 0\np 0x00000000000000aa - 0 2\nsomething new 1 2 3\n";
    write(b.data, newer);
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    CHECK(b.reply("bounty.top").find("1. 0x00000000000000aa: 2 (you)") != std::string::npos);  // read
    b.reply("bounty.give kill_vukah");
    b.reply("bounty.reset");
    b.seconds(2);
    CHECK(slurp(b.data) == newer);  // never written
    int said = 0;
    for (const auto& l : b.r.fake.log) said += l.find("written by a newer Bounty Board") != std::string::npos;
    CHECK(said == 1);
}

TEST_CASE("bounty: the finished counts are saved in a line 1.1 reads too") {
    BountyRig b;
    b.finish_one();
    b.seconds(1);
    CHECK(slurp(b.data).find("\np 0x00000000000000aa - 0 1\n") != std::string::npos);  // as 1.1 wrote "p" lines
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    CHECK(b.reply("bounty.top").find("1. 0x00000000000000aa: 1 (you)") != std::string::npos);
}

TEST_CASE("bounty: until the world's first Bounty Board, take and claim work anywhere; then only at a board, also when its area is unloaded") {
    BountyRig b({{"need_board", "1"}, {"hold_max", "2"}});
    write(b.data, "offset 0\ndawns 0\nplay 10\np 0x00000000000000aa d2026300.0 0 0\n"
                  "b 0x00000000000000aa gather_wood 60 60 1\n");  // a world from 1.1, with a finished bounty
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    const std::string board = b.board();
    CHECK(board.find("This world has no Bounty Board yet") != std::string::npos);
    CHECK(count_of(notices_of(board), "\n") == 3);
    CHECK(b.reply("bounty.claim").rfind("Claimed: 5 x Material_T1_Token_WeaponUpgrade_Rune01", 0) == 0);  // anywhere
    CHECK(b.reply("bounty.take 1").rfind("Taken: ", 0) == 0);
    altar(b, 900, 20, 0);  // the first board: from now on at a board
    prop(b, 950, 20, 10, 5);
    search(b);
    CHECK(slurp(b.data).find("\nboarded 1\n") != std::string::npos);
    CHECK(b.reply("bounty.take 2") == "Take and claim bounties at a Bounty Board.");
    CHECK(b.board().find("Notices: at a Bounty Board.") != std::string::npos);
    // everyone far away, the board's area unloaded, a reload: still at a board, never "build one"
    remove_entity(b.r.fake, 950);
    remove_entity(b.r.fake, 900);
    place(b.r.fake, 1, 500, 10, 0);
    place(b.r.fake, 2, 520, 10, 0);
    b.r.fake.session = 3;
    fire_load(1);
    b.seconds(65);
    CHECK(b.reply("bounty.take 2") == "Take and claim bounties at a Bounty Board.");
    CHECK(b.board().find("no Bounty Board yet") == std::string::npos);
}

// ---- 1.2: boards are remembered by place ----
namespace {
// Steps `n` seconds of a host whose client world holds the board's model only while `client_has` (the server world
// never has render models): the model is put in for each client step and taken out for the server's.
void host_seconds(BountyRig& b, int n, uint32_t board, const std::vector<uint8_t>& model, bool client_has) {
    for (int i = 0; i < n; ++i) {
        if (client_has) b.r.fake.templates[{ecs::ModelResource::kHash, board}] = model;
        b.host->tick(kClient);
        b.r.fake.templates.erase({ecs::ModelResource::kHash, board});
        b.seconds(1);
    }
}
}  // namespace

TEST_CASE("bounty: on the host a Notice Board keeps counting when the host leaves its area, and after a reload") {
    BountyRig b({{"need_board", "1"}});
    altar(b, 900, 0, 0);
    prop(b, 950, 10, 10, 0);
    const auto model = b.r.fake.templates[{ecs::ModelResource::kHash, 950}];
    host_seconds(b, 32, 950, model, true);  // the host walks by: its client world shows the board
    REQUIRE(b.reply("bounty.boards").find("1. Notice Board at 10, 10, 0") != std::string::npos);
    // the host goes far away (its client world no longer holds the board); a friend stays at it
    place(b.r.fake, 1, 500, 10, 0);
    place(b.r.fake, 2, 10, 10, 2);
    host_seconds(b, 70, 950, model, false);
    CHECK(b.reply("bounty.boards").find("1. Notice Board at 10, 10, 0") != std::string::npos);
    CHECK(b.reply("bounty.take 1", 2, 0xBB).rfind("Taken: ", 0) == 0);
    CHECK(slurp(b.data).find("\nboard 10.00 10.00 0.00\n") != std::string::npos);
    // a reload, before the host's client world saw the board again: it counts by its place
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    CHECK(b.reply("bounty.boards").find("1. Notice Board at 10, 10, 0") != std::string::npos);
    host_seconds(b, 70, 950, model, false);  // the friend still at it: the entity is there, so it is no gone board
    CHECK(b.reply("bounty.boards").find("1. Notice Board at 10, 10, 0") != std::string::npos);
}

TEST_CASE("bounty: what the host's client world showed counts only for the same entity at the same place") {
    BountyRig b;
    altar(b, 900, 0, 0);
    prop(b, 950, 10, 10, 0);
    const auto model = b.r.fake.templates[{ecs::ModelResource::kHash, 950}];
    host_seconds(b, 32, 950, model, true);
    REQUIRE(b.reply("bounty.boards").find("Notice Board at 10, 10, 0") != std::string::npos);
    // the board is taken down and its id comes back as another prop in another base, the players far from both
    remove_entity(b.r.fake, 950);
    altar(b, 901, 200, 0);
    prop(b, 950, 205, 10, 0, nullptr);
    place(b.r.fake, 1, 500, 10, 0);
    place(b.r.fake, 2, 520, 10, 0);
    host_seconds(b, 32, 950, model, false);
    CHECK(b.reply("bounty.boards").find("205") == std::string::npos);
}

TEST_CASE("bounty: a Notice Board that was taken down is forgotten after two searches with a player near its place") {
    BountyRig b;
    altar(b, 900, 0, 0);
    prop(b, 950, 10, 10, 0);
    search(b);
    REQUIRE(b.reply("bounty.boards").find("1. Notice Board at 10, 10, 0") != std::string::npos);
    remove_entity(b.r.fake, 950);
    place(b.r.fake, 1, 10, 10, 3);
    search(b);
    CHECK(b.reply("bounty.boards").find("1. Notice Board at 10, 10, 0") != std::string::npos);  // one miss: kept
    search(b);
    CHECK(b.reply("bounty.boards").find("No Bounty Board") != std::string::npos);
    CHECK(slurp(b.data).find("\nboard ") == std::string::npos);
}

TEST_CASE("bounty: a marked prop stays when the player arrives while a search runs (fast travel, a respawn)") {
    BountyRig b;
    prop(b, 960, 0, 10, 5, nullptr);
    b.look(1, 0, 11.5, 0, 0);
    REQUIRE(b.reply("bounty.mark").rfind("Marked: ", 0) == 0);
    remove_entity(b.r.fake, 960);  // everyone leaves: its area is not loaded
    place(b.r.fake, 1, 500, 10, 0);
    place(b.r.fake, 2, 520, 10, 0);
    b.seconds(70);
    place(b.r.fake, 1, 0, 10, 0);  // the host arrives after a search began: the prop is not in that search yet
    search(b);
    CHECK(b.reply("bounty.boards").find("1. marked prop at 0, 10, 5") != std::string::npos);
    prop(b, 960, 0, 10, 5, nullptr);  // streamed in by the next search
    search(b);
    search(b);
    CHECK(b.reply("bounty.boards").find("1. marked prop at 0, 10, 5") != std::string::npos);
}

TEST_CASE("bounty: E opens the wheel only with the crosshair on the board; a chest or a station beside it gets E alone") {
    BountyRig b({{"need_board", "1"}});
    board_ahead(b);
    b.look(1, 0, 11.5, 0, 25);  // at something about 1.4 m beside the board
    b.press('E');
    CHECK(b.r.fake.wheels.empty());
    b.look(1, 0, 11.5, 0, 10);  // the crosshair on the board's edge
    b.press('E');
    CHECK(b.r.fake.wheels.size() == 1);
}

TEST_CASE("bounty: a waiting chat line goes only to the player it was for; a refused one is tried once more") {
    BountyRig b({{"need_board", "1"}});
    altar(b, 900, 50, 50);
    prop(b, 950, 50, 10, 50);
    search(b);
    for (uint32_t p = 2; p <= 9; ++p) add_player(b.r.fake, p, 0xC0 + p, 49.0 + p % 3, 10, 47.0 + 0.2 * p);
    b.seconds(1);  // 30 lines go now; the lines for players 8 and 9 wait
    REQUIRE(b.r.fake.tells.size() == 6);
    b.r.fake.player_info(8, 9, 0xEE);  // player 9 left; entity 9 is someone else now, far from the board
    place(b.r.fake, 9, 500, 10, 0);
    b.seconds(12);
    CHECK(tells_to(b, 8).size() == 1);
    CHECK(tells_to(b, 9).empty());  // never someone else's line
    // players' chat refuses a line once: it is sent at the next step
    b.reply("bounty.give kill_vukah", 2, 0xC2);
    b.r.fake.chat_ok = false;
    for (uint32_t id = 3010; id < 3016; ++id) {
        b.creature(id, 3);
        b.kill(id, {2});
    }
    b.seconds(1);
    b.r.fake.chat_ok = true;
    b.seconds(1);
    bool done = false;
    for (const auto& line : tells_to(b, 2)) done = done || line.rfind("Bounty done: Slay 6 Vukah", 0) == 0;
    CHECK(done);
    // refused twice: left out, and the log says so
    b.reply("bounty.give kill_scavenger", 2, 0xC2);
    b.r.fake.chat_ok = false;
    for (uint32_t id = 3110; id < 3116; ++id) {
        b.creature(id, 4);
        b.kill(id, {2});
    }
    b.seconds(3);
    b.r.fake.chat_ok = true;
    b.seconds(2);
    for (const auto& line : tells_to(b, 2)) CHECK(line.find("Bounty done: Slay 6 Scavengers") == std::string::npos);
    CHECK(logged("was not sent"));
}

// ---- 1.2: the host's and admins' board commands ----
TEST_CASE("bounty: bounty.boards works from a dedicated server's console") {
    BountyRig b;
    altar(b, 900, 0, 0);
    prop(b, 950, 10, 10, 0);
    search(b);
    b.r.api.local_player = no_local_player;  // no player of its own: no distance to say
    CHECK(b.reply("bounty.boards") == "1. Notice Board at 10, 10, 0");
}

TEST_CASE("bounty: bounty.mark without camera data takes the prop nearest to the sender within 3 m and says which") {
    BountyRig b;
    prop(b, 960, 0, 10, 2, nullptr);  // 2 m from the host, whose camera the server does not know
    prop(b, 961, 0, 10, 6, nullptr);
    CHECK(b.reply("bounty.mark") == "Marked: the prop nearest to you (at 0, 10, 2) is a Bounty Board now.");
    place(b.r.fake, 1, 50, 10, 0);  // nothing within 3 m
    CHECK(b.reply("bounty.mark").rfind("Look at a prop", 0) == 0);
}

TEST_CASE("bounty: a flame altar that was abandoned makes no base for a Notice Board") {
    BountyRig b;
    altar(b, 900, 0, 0);
    component<ecs::BaseIdComponent>(b.r.fake, 900).wasAbandoned = true;
    prop(b, 950, 10, 10, 0);
    search(b);
    CHECK(b.reply("bounty.boards").find("No Bounty Board") != std::string::npos);
}

TEST_CASE("bounty: the chat names starter notices; a player who finished one gets the board's own") {
    BountyRig b({{"need_board", "1"}});
    altar(b, 900, 20, 0);
    prop(b, 950, 20, 10, 4);
    place(b.r.fake, 2, 20, 10, -30);  // away from the board while it is found
    search(b);
    b.reply("bounty.give start_wood", 2, 0xBB);  // player 2 finishes their first bounty
    deliver(b.r.fake, "PickupEvent", pickup_event(2, 0, kLogs, 20));
    place(b.r.fake, 2, 20, 10, 0);  // then walks up to it
    b.look(2, 20, 11.5, 0, 0);
    b.seconds(2);
    std::string notices;
    for (const auto& line : tells_to(b, 2))
        if (line.rfind("Bounty Board:", 0) == 0) notices = line;
    CHECK(notices.rfind("Bounty Board: new notices in 4 days\n", 0) == 0);
    CHECK(notices.find("starter") == std::string::npos);
}
