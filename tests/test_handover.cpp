// mods/handover (Hand-over): the shipped Lua mod run by the real Lua host over the fake API
// (tests/lua_test_fake.h): matching, the receiver, the take -> put -> give back -> drop flow, gear, the cooldown, the
// who-list and /handover.to, the host's wheel.
#include <windows.h>

#include <doctest.h>

#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/manifest.h"
#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "lua_host/host.h"
#include "lua_test_fake.h"

using emf::lua::Host;
namespace ecs = emf::gen::ecs;

namespace {
constexpr uint32_t kLogs = 0x21, kTwigs = 0x22, kStone = 0x23, kSword = 0x31;

uint32_t host_is_1() { return 1; }
uint32_t no_local_player() { return 0; }
uint32_t give_nothing(EmfWorld*, uint32_t, uint32_t, uint32_t) { return 0; }

struct Carry {
    uint32_t item, count, pide = 0;
};

// The shipped mod (loaded from mods/handover in the source tree: it writes no files), its settings at mod.json's
// defaults (the fake does not know them) plus `settings`; the host is player 1.
struct HandoverRig {
    Rig r{L"handover"};
    std::unique_ptr<Host> host;
    bool key_held = false;
    explicit HandoverRig(const std::map<std::string, std::string>& settings = {}) {
        r.api.local_player = host_is_1;
        const std::string src = EMF_SOURCE_DIR "/mods/handover/mod.json";
        std::ifstream f(src, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const auto m = emf::core::parse_manifest(ss.str());
        REQUIRE_MESSAGE(m.manifest, m.error);
        for (const auto& d : m.manifest->settings) r.fake.settings["handover." + d.key] = d.default_value;
        for (const auto& [k, v] : settings) r.fake.settings["handover." + k] = v;
        EmfModInfo info{};
        info.size = sizeof info;
        info.status = EMF_MOD_SCRIPT;
        std::snprintf(info.id, sizeof info.id, "handover");
        std::snprintf(info.source, sizeof info.source, "handover\\");
        std::snprintf(info.main, sizeof info.main, "main.lua");
        r.fake.mods.push_back(info);
        r.fake.item_names[kLogs] = "Material_T1_Wood_Logs";
        r.fake.item_names[kTwigs] = "Material_T1_Wood_Twigs";
        r.fake.item_names[kStone] = "Material_T1_Stone";
        r.fake.item_names[kSword] = "Weapon_T1_1H_Sword";
        const std::string mods = EMF_SOURCE_DIR "/mods";
        host = std::make_unique<Host>(&r.api, std::wstring(mods.begin(), mods.end()));
        host->set_key_reader([this](uint32_t) { return key_held; });
        host->load_all();
        REQUIRE(host->status("handover") == "running");
    }
    // A player entity at (x, 0, 0) with an account and (optionally) a name the game knows.
    void player(uint32_t id, double x, uint64_t account, const char* name = nullptr) {
        r.fake.components[{ecs::PlayerState::kHash, id}] = std::vector<uint8_t>(ecs::PlayerState::kSize, 0);
        place(id, x);
        r.fake.player_info(id - 1, id, account);
        if (name) r.fake.names[id] = name;
    }
    void place(uint32_t id, double x) {
        std::vector<uint8_t> t(sizeof(ecs::CurrentTransform), 0);
        reinterpret_cast<ecs::CurrentTransform*>(t.data())->transform.position.x = static_cast<int64_t>(x * 4294967296.0);
        r.fake.components[{ecs::CurrentTransform::kHash, id}] = t;
    }
    // `entity` carries these stacks in one backpack inventory (entity * 100 + 1) linked from its InventorySetup;
    // `worn` go into an equipment inventory (entity * 100 + 2).
    void carry(uint32_t entity, const std::vector<Carry>& stacks, const std::vector<Carry>& worn = {}) {
        auto inventory = [&](uint32_t inv_id, const std::vector<Carry>& list) {
            std::vector<uint8_t> b(sizeof(ecs::Inventory), 0);
            auto* inv = reinterpret_cast<ecs::Inventory*>(b.data());
            for (size_t i = 0; i < list.size() && i < 8; ++i) {
                inv->slots[i].id.value = list[i].item;
                inv->slots[i].data.count = list[i].count;
                inv->slots[i].data.pide.id = list[i].pide;
            }
            r.fake.components[{ecs::Inventory::kHash, inv_id}] = b;
        };
        std::vector<uint8_t> s(sizeof(ecs::InventorySetup), 0);
        auto* setup = reinterpret_cast<ecs::InventorySetup*>(s.data());
        setup->linksEntities[0].id = entity * 100 + 1;
        setup->linksCategories[0] = emf::gen::InventoryCategory::Generic;
        setup->linksEntities[1].id = entity * 100 + 2;
        setup->linksCategories[1] = emf::gen::InventoryCategory::Equipment;
        setup->isInitialized = true;
        r.fake.components[{ecs::InventorySetup::kHash, entity}] = s;
        inventory(entity * 100 + 1, stacks);
        inventory(entity * 100 + 2, worn);
    }
    void item_state(uint32_t pide, uint8_t level, uint8_t rarity) {
        std::vector<uint8_t> b(sizeof(ecs::ItemState), 0);
        auto* st = reinterpret_cast<ecs::ItemState*>(b.data());
        st->itemLevelUi = level;
        st->itemRarityUi = static_cast<emf::gen::ItemRarity>(rarity);
        r.fake.components[{ecs::ItemState::kHash, pide}] = b;
    }
    // The next commands come from `entity`'s chat (account as the core reports it).
    void chat_from(uint32_t entity, uint64_t account) {
        r.fake.sender = EmfCommandSender{};
        r.fake.sender.size = sizeof(EmfCommandSender);
        r.fake.sender.entity = entity;
        r.fake.sender.account = account;
        r.fake.sender.from_chat = 1;
    }
    std::string reply(const std::string& command) {
        r.fake.replies.clear();
        r.run_command(command);
        return r.fake.replies.empty() ? "" : r.fake.replies.back();
    }
    // The game finishes a take / put request one step later with `done` items.
    void finish_take(size_t i, uint32_t done) {
        const EmfItemsRequest q = r.fake.takes.at(i);
        q.cb(kServer, q.entity, q.item, q.count, done, q.user);
    }
    void finish_put(size_t i, uint32_t done) {
        const EmfItemsRequest q = r.fake.puts.at(i);
        q.cb(kServer, q.entity, q.item, q.count, done, q.user);
    }
    bool told(uint32_t player, const std::string& part) const {
        for (const auto& [p, text] : r.fake.tells)
            if (p == player && text.find(part) != std::string::npos) return true;
        return false;
    }
    void seconds(int n) {
        r.fake.tick += 60 * n;
        host->tick(kServer);
    }
};

// Alice (1, chatting) with logs and stone, Bob (2) 3 m away, Cleo (3) 4 m away.
struct Three : HandoverRig {
    explicit Three(const std::map<std::string, std::string>& settings = {}) : HandoverRig(settings) {
        player(1, 0, 0xA1, "Alice");
        player(2, 3, 0xB2, "Bob");
        player(3, -4, 0xC3, "Cleo");
        carry(1, {{kLogs, 40}, {kLogs, 12}, {kStone, 7}});
        carry(2, {});
        chat_from(1, 0xA1);
    }
};
}  // namespace

TEST_CASE("handover: /handover hands the biggest matching stack to the nearest other player; both are told when it arrived") {
    Three t;
    CHECK(t.reply("handover logs") == "Handing 40 x Material_T1_Wood_Logs to Bob");
    REQUIRE(t.r.fake.takes.size() == 1);
    CHECK(t.r.fake.takes[0].entity == 1);
    CHECK(t.r.fake.takes[0].item == kLogs);
    CHECK(t.r.fake.takes[0].count == 40);
    CHECK(t.r.fake.takes[0].slot == 101u);  // exactly the stack it looked at (inventory 101, slot 0)
    CHECK(t.r.fake.takes[0].flags == EMF_ITEMS_EXACT_SLOT);  // never another inventory, such as worn equipment
    CHECK(t.r.fake.puts.empty());  // the put waits for the take
    t.finish_take(0, 40);
    REQUIRE(t.r.fake.puts.size() == 1);
    CHECK(t.r.fake.puts[0].entity == 2);
    CHECK(t.r.fake.puts[0].count == 40);
    CHECK(t.r.fake.puts[0].flags == 0);  // a plain stack: the game's own level and rarity
    t.finish_put(0, 40);
    CHECK(t.told(2, "Alice handed you 40 x Material_T1_Wood_Logs"));
    CHECK(t.told(1, "Handed 40 x Material_T1_Wood_Logs to Bob"));
    CHECK(logged("Alice (1) -> Bob (2): 40 x Material_T1_Wood_Logs: delivered"));
    CHECK(t.r.fake.gives.empty());
    bool players = false;
    for (const auto& c : t.r.fake.commands) players |= c.name == "handover" && c.flags == EMF_COMMAND_PLAYERS;
    CHECK(players);  // the bare /handover (lua 1.6: a command named like the mod), for every player
}

TEST_CASE("handover: a count and 'all' (every stack of the item); more than carried is refused") {
    Three t;
    CHECK(t.reply("handover logs 5") == "Handing 5 x Material_T1_Wood_Logs to Bob");
    t.seconds(3);
    CHECK(t.reply("handover logs all") == "Handing 52 x Material_T1_Wood_Logs to Bob");
    t.seconds(3);
    CHECK(t.reply("handover logs 60") == "you carry only 52 x Material_T1_Wood_Logs");
    REQUIRE(t.r.fake.takes.size() == 3);  // 5 from the biggest stack; then all: 40 and 12, each from its own slot
    CHECK(t.r.fake.takes[0].count == 5);
    CHECK(t.r.fake.takes[1].count == 40);
    CHECK(t.r.fake.takes[1].slot == 101u);
    CHECK(t.r.fake.takes[2].count == 12);
    CHECK(t.r.fake.takes[2].slot == (101ull | (1ull << 32)));
}

TEST_CASE("handover: an exact name wins over parts; several matches list the first five; nothing matching says so") {
    HandoverRig h;
    h.player(1, 0, 0xA1, "Alice");
    h.player(2, 2, 0xB2, "Bob");
    h.r.fake.item_names[0x41] = "Plank_A";
    h.r.fake.item_names[0x42] = "Plank_B";
    h.r.fake.item_names[0x43] = "Plank_C";
    h.r.fake.item_names[0x44] = "Plank_D";
    h.r.fake.item_names[0x45] = "Plank_E";
    h.r.fake.item_names[0x46] = "Plank_F";
    h.carry(1, {{0x11, 30}, {kLogs, 10}, {0x41, 1}, {0x42, 1}, {0x43, 1}, {0x44, 1}, {0x45, 1}, {0x46, 1}});
    h.chat_from(1, 0xA1);
    CHECK(h.reply("handover wood") == "Handing 30 x Wood to Bob");  // "Wood" is exact; the logs only contain it
    h.seconds(3);
    CHECK(h.reply("handover plank") == "which one? Plank_A, Plank_B, Plank_C, Plank_D, Plank_E (and 1 more)");
    CHECK(h.reply("handover stone") == "you carry nothing named like 'stone'");
    CHECK(h.r.fake.takes.size() == 1);
}

TEST_CASE("handover: nobody in reach is refused; the giver never receives; the reach is a setting") {
    HandoverRig h;
    h.player(1, 0, 0xA1, "Alice");
    h.carry(1, {{kLogs, 10}});
    h.chat_from(1, 0xA1);
    CHECK(h.reply("handover logs") == "nobody is within 5 m of you");  // alone: never to herself
    h.player(2, 9, 0xB2, "Bob");
    CHECK(h.reply("handover logs") == "nobody is within 5 m of you");
    h.r.fake.settings["handover.range"] = "10";
    CHECK(h.reply("handover logs") == "Handing 10 x Material_T1_Wood_Logs to Bob");
}

TEST_CASE("handover: the debug target (a chest) receives every hand-over; no receiver note for it") {
    Three t;
    CHECK(t.reply("handover.debug_target 777") == "entity 777 has no inventory");
    t.carry(600, {});
    CHECK(t.reply("handover.debug_target 600").find("debug target: entity 600") == 0);
    CHECK(t.reply("handover stone") == "Handing 7 x Material_T1_Stone to entity 600 [debug target]");
    t.finish_take(0, 7);
    REQUIRE(t.r.fake.puts.size() == 1);
    CHECK(t.r.fake.puts[0].entity == 600);
    t.finish_put(0, 7);
    CHECK(t.told(1, "Handed 7 x Material_T1_Stone to entity 600"));
    for (const auto& [p, text] : t.r.fake.tells) CHECK(p == 1);
    CHECK(t.reply("handover.debug_target off") == "debug target off: hand-overs go to players again");
    t.seconds(3);
    CHECK(t.reply("handover logs") == "Handing 40 x Material_T1_Wood_Logs to Bob");
}

TEST_CASE("handover: what the receiver has no room for goes back to the giver") {
    Three t;
    t.reply("handover logs");
    t.finish_take(0, 40);
    t.finish_put(0, 25);  // room for 25
    REQUIRE(t.r.fake.puts.size() == 2);
    CHECK(t.r.fake.puts[1].entity == 1);
    CHECK(t.r.fake.puts[1].count == 15);
    t.finish_put(1, 15);
    CHECK(t.told(2, "Alice handed you 25 x Material_T1_Wood_Logs"));
    CHECK(t.told(1, "Bob had room for 25 of 40 x Material_T1_Wood_Logs; the rest came back to you"));
    CHECK(logged("delivered 25, 15 back to the giver"));
    CHECK(t.r.fake.gives.empty());
}

TEST_CASE("handover: what cannot go back drops at the giver's feet; only if that fails too is it LOST (logged)") {
    SUBCASE("dropped") {
        Three t;
        t.reply("handover logs");
        t.finish_take(0, 40);
        t.finish_put(0, 0);   // no room at all
        t.finish_put(1, 5);   // only 5 fit back
        REQUIRE(t.r.fake.gives.size() == 1);
        CHECK(t.r.fake.gives[0].player == 1);
        CHECK(t.r.fake.gives[0].item == kLogs);
        CHECK(t.r.fake.gives[0].count == 35);
        CHECK(t.told(1, "35 x Material_T1_Wood_Logs did not fit back into your inventory: they were dropped on the ground"));
        CHECK(logged("delivered 0, 5 back to the giver, 35 dropped"));
        CHECK_FALSE(logged("LOST"));
        CHECK_FALSE(t.told(2, "handed you"));  // Bob got nothing: no note
    }
    SUBCASE("the giver is gone: dropped at the receiver, nothing lost") {
        Three t;
        t.r.fake.give_fails = {1};
        t.reply("handover logs");
        t.finish_take(0, 40);
        t.finish_put(0, 0);
        t.finish_put(1, 0);
        REQUIRE(t.r.fake.gives.size() == 1);
        CHECK(t.r.fake.gives[0].player == 2);
        CHECK(t.r.fake.gives[0].count == 40);
        CHECK_FALSE(logged("LOST"));
    }
    SUBCASE("nobody at all can take them: only then LOST, logged") {
        Three t;
        t.r.api.give_item = give_nothing;
        t.reply("handover logs");
        t.finish_take(0, 40);
        t.finish_put(0, 0);
        t.finish_put(1, 0);
        CHECK(logged("LOST 40 of item"));
        CHECK(logged("LOST 40 (see the log)"));
    }
    SUBCASE("puts refused at once: straight to the feet") {
        Three t;
        g_put_ok = false;
        t.reply("handover logs");
        t.finish_take(0, 40);
        g_put_ok = true;
        REQUIRE(t.r.fake.gives.size() == 1);
        CHECK(t.r.fake.gives[0].count == 40);
    }
}

TEST_CASE("handover: a take that finds nothing hands nothing over and says so") {
    Three t;
    t.reply("handover logs");
    t.finish_take(0, 0);
    CHECK(t.r.fake.puts.empty());
    CHECK(t.told(1, "Nothing was handed over"));
}

TEST_CASE("handover: gear is refused; with allow_gear one piece goes, at its level and rarity, with a warning") {
    SUBCASE("refused") {
        Three t;
        t.carry(1, {{kSword, 1, 5001}});
        CHECK(t.reply("handover sword") == "gear loses its gems and perks when handed over; use a chest");
        CHECK(t.r.fake.takes.empty());
    }
    SUBCASE("allowed") {
        Three t({{"allow_gear", "1"}});
        t.carry(1, {{kSword, 1, 5001}});
        t.item_state(5001, 7, 2);
        CHECK(t.reply("handover sword") == "Handing 1 x Weapon_T1_1H_Sword to Bob (gear: its gems and perks are lost)");
        REQUIRE(t.r.fake.takes.size() == 1);
        CHECK(t.r.fake.takes[0].count == 1);
        t.finish_take(0, 1);
        REQUIRE(t.r.fake.puts.size() == 1);
        CHECK(t.r.fake.puts[0].level == 7);
        CHECK(t.r.fake.puts[0].rarity == 2);
        CHECK((t.r.fake.puts[0].flags & EMF_ITEMS_RARITY) != 0);
        CHECK(logged("gear handed over by Alice: Weapon_T1_1H_Sword"));
    }
    SUBCASE("worn equipment is never offered") {
        Three t({{"allow_gear", "1"}});
        t.carry(1, {{kLogs, 3}}, {{kSword, 1, 5002}});
        CHECK(t.reply("handover sword") == "you carry nothing named like 'sword'");
    }
}

TEST_CASE("handover: a cooldown per giver") {
    Three t;
    CHECK(t.reply("handover logs 1").find("Handing") == 0);
    CHECK(t.reply("handover logs 1") == "wait 2 s before the next hand-over");
    t.chat_from(3, 0xC3);  // another giver is not held up
    t.carry(3, {{kStone, 2}});
    CHECK(t.reply("handover stone").find("Handing 2 x Material_T1_Stone to") == 0);
    t.chat_from(1, 0xA1);
    t.seconds(2);
    CHECK(t.reply("handover logs 1").find("Handing") == 0);
}

TEST_CASE("handover: /handover.who numbers the players in reach; /handover.to takes #k or a name") {
    Three t;
    CHECK(t.reply("handover.who") == "#1 Bob 3 m, #2 Cleo 4 m");
    CHECK(t.reply("handover.to #2 stone") == "Handing 7 x Material_T1_Stone to Cleo");
    CHECK(t.r.fake.takes.back().count == 7);
    t.seconds(3);
    CHECK(t.reply("handover.to #3 stone").find("no #3") == 0);
    CHECK(t.reply("handover.to bob logs 4") == "Handing 4 x Material_T1_Wood_Logs to Bob");
    t.seconds(3);
    CHECK(t.reply("handover.to cl logs 1") == "Handing 1 x Material_T1_Wood_Logs to Cleo");  // a unique start
    t.seconds(3);
    CHECK(t.reply("handover.to zed logs") == "no player named 'zed'");
    t.player(4, 30, 0xD4, "Dora");
    CHECK(t.reply("handover.to dora logs") == "Dora is 30 m away (reach 5 m)");
    t.r.fake.names.clear();
    CHECK(t.reply("handover.who") == "#1 player #2 3 m, #2 player #3 4 m");
    CHECK(t.reply("handover.to bob logs") ==
          "player names are not known here: use /handover.who and /handover.to #<number>");
}

TEST_CASE("handover: the host's own console (no chat sender) gives as the host; a dedicated server's console has no player") {
    Three t;
    t.r.fake.sender = EmfCommandSender{};  // the console or the host's own chat
    CHECK(t.reply("handover stone") == "Handing 7 x Material_T1_Stone to Bob");
    CHECK(t.r.fake.takes.back().entity == 1);
    t.r.api.local_player = no_local_player;
    CHECK(t.reply("handover stone") == "there is no player here: type it in the game's chat");
    t.chat_from(2, 0xB2);
    t.r.fake.sender.entity = 0;
    CHECK(t.reply("handover stone") == "EMF could not tell which player you are; try again in a moment");
}

TEST_CASE("handover: handover.log keeps the last hand-overs; notes off keeps the chat quiet") {
    Three t({{"notes", "0"}});
    t.reply("handover stone");
    t.finish_take(0, 7);
    t.finish_put(0, 7);
    CHECK(t.r.fake.tells.empty());
    t.r.fake.sender = EmfCommandSender{};
    CHECK(t.reply("handover.log") == "Alice (1) -> Bob (2): 7 x Material_T1_Stone: delivered");
}

TEST_CASE("handover: the host's wheel: a player, then a stack, then how many; it runs on the next server step") {
    Three t;
    t.r.fake.sender = EmfCommandSender{};
    t.key_held = true;
    t.host->tick(kServer);  // the key's press reaches the mod on the server world
    t.key_held = false;
    REQUIRE(t.r.fake.wheels.size() == 1);
    CHECK(t.r.fake.wheels[0].title == "Hand over to");
    REQUIRE(t.r.fake.wheels[0].items.size() == 2);
    CHECK(t.r.fake.wheels[0].items[0] == "Bob (3 m)");
    t.r.fake.wheels[0].cb(kClient, 0, t.r.fake.wheels[0].user);  // Bob
    REQUIRE(t.r.fake.wheels.size() == 2);
    CHECK(t.r.fake.wheels[1].items[0] == "Material_T1_Stone (x7)");
    CHECK(t.r.fake.wheels[1].items[1] == "Material_T1_Wood_Logs (x52)");
    t.r.fake.wheels[1].cb(kClient, 1, t.r.fake.wheels[1].user);  // the logs
    REQUIRE(t.r.fake.wheels.size() == 3);
    REQUIRE(t.r.fake.wheels[2].items.size() == 4);
    CHECK(t.r.fake.wheels[2].items[2] == "Half (26)");
    t.r.fake.wheels[2].cb(kClient, 2, t.r.fake.wheels[2].user);  // half
    CHECK(t.r.fake.takes.empty());  // the wheel answered on the client world: the take waits for the server step
    t.host->tick(kServer);
    REQUIRE(t.r.fake.takes.size() == 1);
    CHECK(t.r.fake.takes[0].count == 26);
    CHECK(t.r.fake.notes.back() == "Hand-over: Handing 26 x Material_T1_Wood_Logs to Bob");
}

TEST_CASE("handover: on a host without chat lines (an older core) the hand-over still works, without notes") {
    Three t;
    t.r.api.chat_tell = nullptr;
    t.reply("handover stone");
    t.finish_take(0, 7);
    t.finish_put(0, 7);
    CHECK(t.r.fake.tells.empty());
    CHECK(logged("Alice (1) -> Bob (2): 7 x Material_T1_Stone: delivered"));
}

TEST_CASE("handover: an answer after the session changed gives nothing back into an id that names another entity now") {
    Three t;
    t.reply("handover logs");
    t.finish_take(0, 40);
    ++t.r.fake.session;  // a reload: the waiting put is answered with 0 in the new world
    t.finish_put(0, 0);
    CHECK(t.r.fake.puts.size() == 1);
    CHECK(t.r.fake.gives.empty());
}

TEST_CASE("handover: /handover.block: a player refuses hand-overs; the giver is told; off accepts them again") {
    Three t;
    t.chat_from(2, 0xB2);
    CHECK(t.reply("handover.block").rfind("You no longer accept hand-overs", 0) == 0);
    t.chat_from(1, 0xA1);
    CHECK(t.reply("handover logs") == "Bob does not accept hand-overs");
    CHECK(t.reply("handover.to bob logs") == "Bob does not accept hand-overs");
    CHECK(t.r.fake.takes.empty());
    t.chat_from(2, 0xB2);
    CHECK(t.reply("handover.block off").rfind("You accept hand-overs again", 0) == 0);
    t.chat_from(1, 0xA1);
    CHECK(t.reply("handover logs") == "Handing 40 x Material_T1_Wood_Logs to Bob");
}

TEST_CASE("handover: two players with the same name: /handover.to asks for #k instead of picking one") {
    Three t;
    t.r.fake.names[3] = "Bob";
    CHECK(t.reply("handover.to bob logs") == "more than one player is named 'bob': use /handover.who and #<number>");
    CHECK(t.r.fake.takes.empty());
}

TEST_CASE("handover: a wheel pick checks its receiver again when it runs (gone, or out of reach)") {
    Three t;
    t.r.fake.sender = EmfCommandSender{};
    t.key_held = true;
    t.host->tick(kServer);
    t.key_held = false;
    REQUIRE(t.r.fake.wheels.size() == 1);
    t.r.fake.wheels[0].cb(kClient, 0, t.r.fake.wheels[0].user);  // Bob
    t.r.fake.wheels[1].cb(kClient, 1, t.r.fake.wheels[1].user);  // the logs
    t.r.fake.wheels[2].cb(kClient, 0, t.r.fake.wheels[2].user);  // 1
    t.place(2, 30);  // Bob walked off before the server step
    t.host->tick(kServer);
    CHECK(t.r.fake.takes.empty());
    CHECK(t.r.fake.notes.back() == "Hand-over: Bob is out of reach now (30 m)");
}

TEST_CASE("handover: ships switched off (it lets players put items into other players' bags)") {
    const std::string src = EMF_SOURCE_DIR "/mods/handover/mod.json";
    std::ifstream f(src, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const auto m = emf::core::parse_manifest(ss.str());
    REQUIRE(m.manifest);
    CHECK_FALSE(m.manifest->enabled_default);
}
