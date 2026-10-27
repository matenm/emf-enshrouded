// The shipped Lua example mods, run by the host from mods/ with fake engine events.
#include <windows.h>

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>

#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/commands.h"
#include "core/console.h"
#include "core/manifest.h"
#include "core/mod_registry.h"
#include "core/mods.h"
#include "core/settings.h"
#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "lua_host/host.h"
#include "lua_test_fake.h"

using emf::lua::Host;
namespace ecs = emf::gen::ecs;

namespace {
uint32_t one_player() { return 1; }

void send(Fake& f, const char* event, const std::vector<uint8_t>& bytes) {
    for (const auto& s : f.subs)
        if (s.name == event) s.cb(kServer, bytes.data(), static_cast<uint32_t>(bytes.size()), s.user);
}

std::vector<uint8_t> hit(uint32_t source, uint32_t target) {
    std::vector<uint8_t> b(ecs::HitEvent::kSize, 0);
    std::memcpy(&b[offsetof(ecs::HitEvent, rootSourceId)], &source, 4);
    std::memcpy(&b[offsetof(ecs::HitEvent, targetId)], &target, 4);
    return b;
}

std::vector<uint8_t> died(uint32_t entity) {
    std::vector<uint8_t> b(ecs::EntityDiedEvent::kSize, 0);
    std::memcpy(&b[offsetof(ecs::EntityDiedEvent, deadEntityId)], &entity, 4);
    return b;
}
}  // namespace

TEST_CASE("lua example kill_counter: your last hit before a death counts; others' kills and stray deaths do not") {
    Rig r(L"examples");
    r.api.local_player = one_player;
    EmfModInfo info{};
    info.size = sizeof info;
    info.status = EMF_MOD_SCRIPT;
    std::snprintf(info.id, sizeof info.id, "kill_counter");
    std::snprintf(info.source, sizeof info.source, "kill_counter\\");
    std::snprintf(info.main, sizeof info.main, "main.lua");
    r.fake.mods.push_back(info);
    r.fake.settings["kill_counter.corner"] = "0";
    const std::string src = EMF_SOURCE_DIR "/mods";
    Host host(&r.api, std::wstring(src.begin(), src.end()));
    host.load_all();
    REQUIRE(host.status("kill_counter") == "running");
    CHECK(r.fake.hud["kill_counter.count"] == "Kills: 0");
    send(r.fake, "HitEvent", hit(1, 500));   // you hit 500
    send(r.fake, "HitEvent", hit(1, 501));
    send(r.fake, "HitEvent", hit(77, 501));  // someone else hits 501 last
    send(r.fake, "EntityDiedEvent", died(500));
    send(r.fake, "EntityDiedEvent", died(501));
    send(r.fake, "EntityDiedEvent", died(999));  // never hit
    CHECK(r.fake.hud["kill_counter.count"] == "Kills: 1");
    r.run_command("kill_counter.reset");
    CHECK(r.fake.hud["kill_counter.count"] == "Kills: 0");
    REQUIRE_FALSE(r.fake.replies.empty());
    CHECK(r.fake.replies.back() == "kill counter reset");
}

namespace {
std::string read_text(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
}  // namespace

TEST_CASE("lua example kill_counter: the count is kept per world and comes back when that world loads again") {
    Rig r(L"examples_world");
    r.api.local_player = one_player;
    // the shipped mod, copied into the test's mods folder (its world data goes beside it, not into the source tree)
    const std::wstring folder = r.dir + L"\\kill_counter";
    CreateDirectoryW(folder.c_str(), nullptr);
    const std::string src = EMF_SOURCE_DIR "/mods/kill_counter/";
    for (const char* name : {"main.lua", "mod.json"}) {
        const std::string from = src + name;
        REQUIRE(CopyFileW(std::wstring(from.begin(), from.end()).c_str(),
                          (folder + L"\\" + std::wstring(name, name + std::strlen(name))).c_str(), FALSE));
    }
    EmfModInfo info{};
    info.size = sizeof info;
    info.status = EMF_MOD_SCRIPT;
    std::snprintf(info.id, sizeof info.id, "kill_counter");
    std::snprintf(info.source, sizeof info.source, "kill_counter\\");
    std::snprintf(info.main, sizeof info.main, "main.lua");
    r.fake.mods.push_back(info);
    r.fake.settings["kill_counter.corner"] = "0";
    const std::wstring saved = r.dir + L"\\..\\config\\kill_counter\\0001.txt";
    DeleteFileW(saved.c_str());
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(host.status("kill_counter") == "running");
    fire_load(1);
    host.tick(kServer);
    CHECK(r.fake.hud["kill_counter.count"] == "Kills: 0");
    send(r.fake, "HitEvent", hit(1, 500));
    send(r.fake, "EntityDiedEvent", died(500));
    CHECK(r.fake.hud["kill_counter.count"] == "Kills: 1");
    CHECK(read_text(saved) == "kills=1");
    // later the same world loads again in a new session, with 7 kills saved
    write(saved, "kills=7");
    r.fake.session = 2;
    fire_load(1);
    host.tick(kServer);
    CHECK(r.fake.hud["kill_counter.count"] == "Kills: 7");
    DeleteFileW(saved.c_str());
}

// ---- life_skills ----
namespace {
constexpr uint32_t kLogs = 0x21, kBait = 0x22, kFibre = 0x23;

std::vector<uint8_t> pickup(uint32_t by, uint32_t from, uint32_t item, uint32_t count) {
    std::vector<uint8_t> b(ecs::PickupEvent::kSize, 0);
    std::memcpy(&b[offsetof(ecs::PickupEvent, itemId)], &item, 4);
    std::memcpy(&b[offsetof(ecs::PickupEvent, itemCount)], &count, 4);
    std::memcpy(&b[offsetof(ecs::PickupEvent, pickedUpByEntityId)], &by, 4);
    std::memcpy(&b[offsetof(ecs::PickupEvent, pickedUpFromEntityId)], &from, 4);
    return b;
}

// The shipped mod copied into the rig's mods folder (its world data stays in the test's folder), players 1 (0xAA)
// and 2 (0xBB), the items it classifies.
struct SkillsRig {
    Rig r{L"life_skills"};
    std::wstring data;
    SkillsRig() {
        r.api.local_player = one_player;
        const std::wstring folder = r.dir + L"\\life_skills";
        CreateDirectoryW(folder.c_str(), nullptr);
        const std::string src = EMF_SOURCE_DIR "/mods/life_skills/";
        for (const char* name : {"main.lua", "mod.json"}) {
            const std::string from = src + name;
            REQUIRE(CopyFileW(std::wstring(from.begin(), from.end()).c_str(),
                              (folder + L"\\" + std::wstring(name, name + std::strlen(name))).c_str(), FALSE));
        }
        EmfModInfo info{};
        info.size = sizeof info;
        info.status = EMF_MOD_SCRIPT;
        std::snprintf(info.id, sizeof info.id, "life_skills");
        std::snprintf(info.source, sizeof info.source, "life_skills\\");
        std::snprintf(info.main, sizeof info.main, "main.lua");
        r.fake.mods.push_back(info);
        r.fake.settings["life_skills.corner"] = "0";
        r.fake.settings["life_skills.hud"] = "1";  // the fake does not know mod.json's defaults
        for (uint32_t p : {1u, 2u}) r.fake.components[{ecs::PlayerState::kHash, p}] = std::vector<uint8_t>(ecs::PlayerState::kSize, 0);
        r.fake.components[{ecs::InventorySetup::kHash, 500}] = std::vector<uint8_t>(ecs::InventorySetup::kSize, 0);
        r.fake.player_info(0, 1, 0xAA);
        r.fake.player_info(1, 2, 0xBB);
        r.fake.item_names[kLogs] = "Material_T1_Wood_Logs";
        r.fake.item_names[kBait] = "Material_T0_simple_FishingBait";
        r.fake.item_names[kFibre] = "Material_T1_Plant_Fibre";
        data = r.dir + L"\\..\\config\\life_skills\\0001.txt";
        // the folder a test writes saved skills into before the mod has saved anything (alone, no earlier test made it)
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(data).parent_path().lexically_normal(), ec);
        DeleteFileW(data.c_str());
    }
    ~SkillsRig() { DeleteFileW(data.c_str()); }
    std::string hud() { return r.fake.hud["life_skills.skills"]; }
    std::string skills_reply(const std::string& skill) {
        r.fake.replies.clear();
        r.run_command("life_skills.skills");
        for (const auto& line : r.fake.replies)
            if (line.rfind(skill, 0) == 0) return line;
        return "";
    }
};
bool noted(Fake& f, const std::string& needle) {
    for (const auto& n : f.notes)
        if (n.find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

TEST_CASE("lua example life_skills: 50 wood logs from the world make woodcutting level 2, with a note and the HUD line") {
    SkillsRig k;
    Host host(&k.r.api, k.r.dir);
    host.load_all();
    REQUIRE(host.status("life_skills") == "running");
    fire_load(1);
    host.tick(kServer);
    CHECK(k.hud() == "Wood 1 | Mine 1 | Forage 1 | Hunt 1 | Fish 1");
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 30));
    CHECK(k.hud().rfind("Wood 1 ", 0) == 0);
    CHECK_FALSE(noted(k.r.fake, "Woodcutting"));
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 20));
    CHECK(k.hud().rfind("Wood 2 ", 0) == 0);
    CHECK(noted(k.r.fake, "Woodcutting reached level 2"));
    CHECK(k.skills_reply("Woodcutting") == "Woodcutting 2: 50/200 XP");
    bool for_players = false;  // every player may ask for their own skills from the chat (lua 1.4)
    for (const auto& c : k.r.fake.commands) for_players |= c.name == "life_skills.skills" && c.flags == EMF_COMMAND_PLAYERS;
    CHECK(for_players);
}

TEST_CASE("lua example life_skills: a chest, fishing bait or someone who is not a player gives no XP") {
    SkillsRig k;
    Host host(&k.r.api, k.r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);
    send(k.r.fake, "PickupEvent", pickup(1, 500, kLogs, 40));  // taken out of a chest
    send(k.r.fake, "PickupEvent", pickup(1, 0, kBait, 40));    // not a gathered item
    send(k.r.fake, "PickupEvent", pickup(77, 0, kLogs, 40));   // not a player
    CHECK(k.skills_reply("Woodcutting") == "Woodcutting 1: 0/50 XP");
    CHECK(k.skills_reply("Fishing") == "Fishing 1: 0/50 XP");
}

TEST_CASE("lua example life_skills: from level 2 a pickup can be given once more; the gift gives no XP") {
    SkillsRig k;
    k.r.fake.settings["life_skills.bonus_per_level"] = "100";  // (level - 1) * 100 %: always from level 2
    Host host(&k.r.api, k.r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 50));  // level 1 -> 2: no bonus at level 1
    CHECK(k.r.fake.puts.empty());
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 5));
    REQUIRE(k.r.fake.puts.size() == 1);
    CHECK(k.r.fake.puts[0].entity == 1u);
    CHECK(k.r.fake.puts[0].item == kLogs);
    CHECK(k.r.fake.puts[0].count == 5u);
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 5));  // the game reports the gift as a pickup
    CHECK(k.r.fake.puts.size() == 1);                        // no bonus on the bonus
    CHECK(k.skills_reply("Woodcutting") == "Woodcutting 2: 55/200 XP");
}

TEST_CASE("lua example life_skills: two players keep their own skills, saved with the world and restored on the next load") {
    SkillsRig k;
    Host host(&k.r.api, k.r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 60));
    send(k.r.fake, "PickupEvent", pickup(2, 0, kFibre, 10));
    for (int i = 0; i < 700; ++i) host.tick(kServer);  // saved at most every 10 s (600 server steps)
    std::ifstream f(k.data, std::ios::binary);
    const std::string saved((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    CHECK(saved.find("0x00000000000000aa woodcutting 60") != std::string::npos);
    CHECK(saved.find("0x00000000000000bb foraging 10") != std::string::npos);
    k.r.fake.sender = EmfCommandSender{sizeof(EmfCommandSender), 2, 0xBB, 1, {}};  // player 2 asks in the chat
    CHECK(k.skills_reply("Foraging") == "Foraging 1: 10/50 XP");
    k.r.fake.sender = EmfCommandSender{};
    // the same world loads again later, with more saved
    write(k.data, "0x00000000000000aa woodcutting 800\n0x00000000000000bb foraging 60\n");
    k.r.fake.session = 2;
    fire_load(1);
    host.tick(kServer);
    CHECK(k.hud().rfind("Wood 5 ", 0) == 0);
    k.r.fake.sender = EmfCommandSender{sizeof(EmfCommandSender), 2, 0xBB, 1, {}};
    CHECK(k.skills_reply("Foraging") == "Foraging 2: 60/200 XP");  // player 2 restored too
    k.r.fake.sender = EmfCommandSender{};
}

namespace {
uint32_t g_me = 0;
uint32_t me_or_nobody() { return g_me; }
}  // namespace

TEST_CASE("lua example life_skills: the HUD line appears once the game knows the local player (seen live: 0 right after a load)") {
    SkillsRig k;
    k.r.api.local_player = me_or_nobody;
    g_me = 0;  // the world's data arrives before the game has set the local player
    Host host(&k.r.api, k.r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);
    CHECK(k.hud().empty());
    g_me = 1;
    host.tick(kServer);
    CHECK(k.hud() == "Wood 1 | Mine 1 | Forage 1 | Hunt 1 | Fish 1");
}

TEST_CASE("lua example life_skills: only raw materials count, each to its skill (real item names that fooled the first lists)") {
    SkillsRig k;
    const std::vector<std::pair<uint32_t, const char*>> items = {
        {0x31, "Weapon_T5_Lore_Sword_Epic"},               // "ore_" in "Lore_": no
        {0x32, "Material_T4_Sulfur"},                      // mining, not hunting ("fur")
        {0x33, "Prop_Furniture_Chair_Wood"},               // not a material
        {0x34, "Build_TerrainMaterial_T2_TallTree_Bark"},  // woodcutting
        {0x35, "_Food_raw_T3_fish_common_03"},             // a caught fish
        {0x36, "_Food_grilled_T3_fish_common_03"},         // cooked: no
        {0x37, "Material_T1_Charcoal"},                    // made in a kiln: no
        {0x38, "Material_T2_Copper_Bar"},                  // smelted: no
        {0x39, "Block_T1_Stone_CRAFTING"},                 // a building block: no
        {0x3A, "Food_T1_raw_bad_meat_LeanMeat"},           // hunting
        {0x3B, "Food_T2_raw_bad_mushroom_AzureRussula"},   // foraging
    };
    for (const auto& [id, name] : items) k.r.fake.item_names[id] = name;
    Host host(&k.r.api, k.r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);
    for (const auto& [id, name] : items) send(k.r.fake, "PickupEvent", pickup(1, 0, id, 1));
    CHECK(k.skills_reply("Woodcutting") == "Woodcutting 1: 1/50 XP");
    CHECK(k.skills_reply("Mining") == "Mining 1: 1/50 XP");
    CHECK(k.skills_reply("Fishing") == "Fishing 1: 1/50 XP");
    CHECK(k.skills_reply("Hunting") == "Hunting 1: 1/50 XP");
    CHECK(k.skills_reply("Foraging") == "Foraging 1: 1/50 XP");
}

TEST_CASE("lua example life_skills: a source whose components cannot be read counts as a container (fails closed)") {
    SkillsRig k;
    k.r.fake.components[{ecs::InventorySetup::kHash, 600}] = std::vector<uint8_t>(1, 0);  // a size the mod refuses
    Host host(&k.r.api, k.r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);
    send(k.r.fake, "PickupEvent", pickup(1, 600, kLogs, 40));
    CHECK(host.status("life_skills") == "running");
    CHECK(k.skills_reply("Woodcutting") == "Woodcutting 1: 0/50 XP");
}

// ---- life_skills: edge cases ----
TEST_CASE("lua example life_skills: a genuine world pickup right after a bonus is not taken for the bonus") {
    SkillsRig k;
    k.r.fake.settings["life_skills.bonus_per_level"] = "100";
    Host host(&k.r.api, k.r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 50));     // level 2
    send(k.r.fake, "PickupEvent", pickup(1, 9001, kLogs, 5));   // from a tree: bonus 5 given
    REQUIRE(k.r.fake.puts.size() == 1);
    send(k.r.fake, "PickupEvent", pickup(1, 9002, kLogs, 5));   // another tree, same step: genuine
    CHECK(k.skills_reply("Woodcutting") == "Woodcutting 2: 60/200 XP");
}

TEST_CASE("lua example life_skills: a bonus the game refused is not remembered as given") {
    SkillsRig k;
    k.r.fake.settings["life_skills.bonus_per_level"] = "100";
    Host host(&k.r.api, k.r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 50));
    g_put_ok = false;
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 5));  // the bonus put is refused (inventory full)
    g_put_ok = true;
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 5));  // a real pickup of the same size
    CHECK(k.skills_reply("Woodcutting") == "Woodcutting 2: 60/200 XP");
}

TEST_CASE("lua example life_skills: a level-up is saved at once, not up to 10 s later") {
    SkillsRig k;
    Host host(&k.r.api, k.r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 50));
    host.tick(kServer);
    std::ifstream f(k.data, std::ios::binary);
    const std::string saved((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(saved.find("woodcutting 50") != std::string::npos);
}

TEST_CASE("lua example life_skills: the HUD waits for the local player's account; a new session clears it") {
    SkillsRig k;
    k.r.fake.player_infos.clear();  // the game has not filled in the players yet
    write(k.data, "0x00000000000000aa woodcutting 800\n");
    Host host(&k.r.api, k.r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);
    k.r.fake.player_info(0, 1, 0xAA);
    host.tick(kServer);
    CHECK(k.hud().rfind("Wood 5 ", 0) == 0);  // shown under the account once it is known
    k.r.fake.session = 2;                      // another world is being loaded
    host.tick(kServer);
    CHECK(k.hud().empty());                    // not the old world's levels
}

TEST_CASE("lua example life_skills: an item the game could not name yet is looked up again later") {
    SkillsRig k;
    k.r.fake.item_names.erase(kLogs);
    Host host(&k.r.api, k.r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 10));  // unknown: no XP
    k.r.fake.item_names[kLogs] = "Material_T1_Wood_Logs";
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 10));
    CHECK(k.skills_reply("Woodcutting") == "Woodcutting 1: 10/50 XP");
}

TEST_CASE("lua example life_skills: another player's level-up is saved at once too (a dedicated server has no local player)") {
    SkillsRig k;
    Host host(&k.r.api, k.r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);
    send(k.r.fake, "PickupEvent", pickup(2, 0, kFibre, 50));
    host.tick(kServer);
    std::ifstream f(k.data, std::ios::binary);
    const std::string saved((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(saved.find("0x00000000000000bb foraging 50") != std::string::npos);
}

TEST_CASE("lua example life_skills: a bonus the game could not add (it says so a step later) is forgotten") {
    SkillsRig k;
    k.r.fake.settings["life_skills.bonus_per_level"] = "100";
    Host host(&k.r.api, k.r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 50));
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 5));  // the bonus is queued
    REQUIRE(k.r.fake.puts.size() == 1);
    const EmfItemsRequest& put = k.r.fake.puts[0];
    REQUIRE(put.cb);
    put.cb(kServer, put.entity, put.item, put.count, 0, put.user);  // the inventory was full: nothing added
    send(k.r.fake, "PickupEvent", pickup(1, 0, kLogs, 5));  // a real pickup of the same size
    CHECK(k.skills_reply("Woodcutting") == "Woodcutting 2: 60/200 XP");
}

// ---- template_lua: the Lua starter (docs/modding/first_mod.md) ----
namespace {
std::map<uint32_t, bool> g_template_keys;
bool template_key(uint32_t vk) { return g_template_keys[vk]; }

// The starter as a player gets it: run from the repository's template_lua folder, its settings at mod.json's defaults
// (the core declares them from the same file), the local player standing at (10.4, 20.6, -3.2) m.
struct StarterRig {
    Rig r{L"template_lua"};
    std::unique_ptr<Host> host;
    StarterRig() {
        g_template_keys.clear();
        r.api.local_player = one_player;
        std::ifstream f(EMF_SOURCE_DIR "/template_lua/mod.json", std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const auto m = emf::core::parse_manifest(ss.str());
        REQUIRE_MESSAGE(m.manifest, m.error);
        for (const auto& d : m.manifest->settings) r.fake.settings[m.manifest->id + "." + d.key] = d.default_value;
        EmfModInfo info{};
        info.size = sizeof info;
        info.status = EMF_MOD_SCRIPT;
        std::snprintf(info.id, sizeof info.id, "%s", m.manifest->id.c_str());
        std::snprintf(info.source, sizeof info.source, "template_lua\\");
        std::snprintf(info.main, sizeof info.main, "main.lua");
        r.fake.mods.push_back(info);
        std::vector<uint8_t> t(sizeof(ecs::CurrentTransform), 0);
        auto* ct = reinterpret_cast<ecs::CurrentTransform*>(t.data());
        ct->transform.position.x = static_cast<int64_t>(10.4 * 4294967296.0);
        ct->transform.position.y = static_cast<int64_t>(20.6 * 4294967296.0);
        ct->transform.position.z = static_cast<int64_t>(-3.2 * 4294967296.0);
        r.fake.components[{ecs::CurrentTransform::kHash, 1}] = t;
        const std::string src = EMF_SOURCE_DIR;
        host = std::make_unique<Host>(&r.api, std::wstring(src.begin(), src.end()));
        host->set_key_reader(template_key);
        host->load_all();
    }
    std::string line() { return r.fake.hud.count("my_first_mod.line") ? r.fake.hud["my_first_mod.line"] : ""; }
    std::pair<uint32_t, uint32_t> style() { return r.fake.hud_style["my_first_mod.line"]; }
    void changed(const char* key) {  // the core tells the mod a setting changed (Mods tab, console)
        for (auto& [mod, cb] : r.fake.setting_watchers)
            if (mod == "my_first_mod") cb.first(kClient, "my_first_mod", key, cb.second);
    }
    void press(uint32_t vk) {
        g_template_keys[vk] = true;
        host->tick(kClient);
        g_template_keys[vk] = false;
        host->tick(kClient);
    }
};
}  // namespace

TEST_CASE("lua template: the starter shows its text and your position in the corner and colour from its settings") {
    StarterRig x;
    REQUIRE(x.host->status("my_first_mod") == "running");
    x.host->tick(kServer);  // the server world draws nothing
    CHECK(x.line().empty());
    x.host->tick(kClient);
    CHECK(x.line() == "Hello from my first mod!  (10, 21, -3)");
    CHECK(x.style() == std::make_pair(uint32_t(EMF_HUD_BOTTOM_LEFT), 0xFFD070FFu));
    x.r.fake.settings["my_first_mod.decimals"] = "1";  // the slider moved in the Mods tab
    x.changed("decimals");
    x.host->tick(kClient);
    CHECK(x.line() == "Hello from my first mod!  (10.4, 20.6, -3.2)");
    x.r.fake.settings["my_first_mod.corner"] = "top_right";
    x.r.fake.settings["my_first_mod.color"] = "#00ff80";
    x.changed("color");
    x.host->tick(kClient);
    CHECK(x.style() == std::make_pair(uint32_t(EMF_HUD_TOP_RIGHT), 0x00FF80FFu));
    x.r.fake.settings["my_first_mod.text"] = "Hi";
    x.host->tick(kClient);
    CHECK(x.line().rfind("Hi  (", 0) == 0);
}

TEST_CASE("lua template: the key in its keybind setting hides and shows the line; a new key works at once") {
    StarterRig x;
    x.host->tick(kClient);
    REQUIRE_FALSE(x.line().empty());
    x.press(VK_F7);
    CHECK(x.r.fake.settings["my_first_mod.show"] == "0");  // saved through the settings, so the Mods tab shows it
    CHECK(x.line().empty());
    x.press(VK_F7);
    CHECK(x.r.fake.settings["my_first_mod.show"] == "1");
    CHECK_FALSE(x.line().empty());
    x.r.fake.settings["my_first_mod.toggle_key"] = "G";  // bound to another key in the Mods tab
    x.press(VK_F7);
    CHECK_FALSE(x.line().empty());
    x.press('G');
    CHECK(x.line().empty());
}

TEST_CASE("lua template: its command answers in the console") {
    StarterRig x;
    x.r.run_command("my_first_mod.hello");
    REQUIRE_FALSE(x.r.fake.replies.empty());
    CHECK(x.r.fake.replies.back() == "hello from my_first_mod");
    REQUIRE_FALSE(x.r.fake.notes.empty());
    CHECK(x.r.fake.notes.back() == "Hello from my_first_mod!");
}

// docs/modding/first_mod.md, steps 3 and 5, applied as written to a copy of the starter
namespace {
std::string read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void replace_once(std::string& text, const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    REQUIRE_MESSAGE(at != std::string::npos, from);
    CHECK_MESSAGE(text.find(from, at + 1) == std::string::npos, from);  // the guide's "find this line" is unique
    text.replace(at, from.size(), to);
}
std::string guide_block(const std::string& guide, const std::string& after, const char* fence) {
    // the first code block of kind `fence` ("lua", "json") after the text `after`
    const size_t start = guide.find(after);
    REQUIRE_MESSAGE(start != std::string::npos, after);
    const std::string open = std::string("```") + fence;
    const size_t a = guide.find(open, start);
    REQUIRE(a != std::string::npos);
    const size_t body = guide.find('\n', a) + 1;
    const size_t b = guide.find("```", body);
    std::string block = guide.substr(body, b - body);
    // the guide indents blocks inside a numbered list by 3 spaces
    std::string out, line;
    for (size_t i = 0; i <= block.size(); ++i) {
        if (i == block.size() || block[i] == '\n') {
            out += (line.rfind("   ", 0) == 0 && guide.compare(a - 3, 3, "   ") == 0 ? line.substr(3) : line) + "\n";
            line.clear();
        } else if (block[i] != '\r') {
            line += block[i];
        }
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}
}  // namespace

namespace {
// The real settings store behind the fake API's setting functions (the guide test goes through the core's path).
emf::core::SettingsStore* g_live = nullptr;
int64_t live_int(const char* mod, const char* key, int64_t fallback) {
    return g_live->has(mod, key) ? g_live->get_int(mod, key, fallback) : fallback;
}
double live_float(const char* mod, const char* key, double fallback) {
    return g_live->has(mod, key) ? g_live->get_float(mod, key, fallback) : fallback;
}
uint32_t live_text(const char* mod, const char* key, char* buf, uint32_t size) {
    if (!g_live->has(mod, key)) {
        if (size) buf[0] = 0;
        return 0;
    }
    const std::string v = g_live->get(mod, key);
    std::snprintf(buf, size, "%s", v.c_str());
    return static_cast<uint32_t>(v.size());
}
bool live_set(const char* mod, const char* key, const char* value) {
    const auto r = g_live->set(mod, key, value);
    return r == emf::core::SettingsStore::SetResult::Ok || r == emf::core::SettingsStore::SetResult::Unchanged;
}
struct LiveIni : emf::core::IniBackend {
    std::map<std::string, std::string> values;
    std::optional<std::string> read(const std::string& mod, const std::string& key) override {
        auto it = values.find(mod + "." + key);
        return it == values.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    bool write(const std::string& mod, const std::string& key, const std::string& value) override {
        values[mod + "." + key] = value;
        return true;
    }
};
void write_later(const std::wstring& path, const std::string& text, int step) {
    write(path, text);
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ft.dwHighDateTime += static_cast<DWORD>(step);
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}
}  // namespace

TEST_CASE("lua template: the first-mod guide's edits work as written (brackets, a mistake, a new setting)") {
    const std::string guide = read_all(EMF_SOURCE_DIR "/docs/modding/first_mod.md");
    const std::string original_lua = read_all(EMF_SOURCE_DIR "/template_lua/main.lua");
    const std::string original_json = read_all(EMF_SOURCE_DIR "/template_lua/mod.json");
    std::string lua = original_lua;
    std::string json = original_json;
    // step 3: change the text line
    const std::string old_line = guide_block(guide, "find this line near the top", "lua");
    const std::string new_line = guide_block(guide, "Change it to:", "lua");
    replace_once(lua, old_line, new_line);
    // step 5: a new setting after "settings": [ and the position only when it is on
    const std::string setting = guide_block(guide, "find the line `\"settings\": [`", "json");
    const size_t list = json.find("\"settings\": [");
    REQUIRE(list != std::string::npos);
    const size_t next_line = json.find('\n', list) + 1;  // CRLF or LF, as the checkout made it
    json.insert(next_line, setting + "\n");
    replace_once(lua, guide_block(guide, "In `main.lua`, find:", "lua"), guide_block(guide, "and change it to:", "lua"));

    // the starter as copied in step 1, its settings declared by the core at the game's start
    Rig r(L"first_mod_guide");
    r.api.local_player = one_player;
    LiveIni ini;
    emf::core::SettingsStore store(&ini);
    g_live = &store;
    r.api.setting_int = live_int;
    r.api.setting_float = live_float;
    r.api.setting_text = live_text;
    r.api.setting_set = live_set;
    const std::wstring folder = r.dir + L"\\my_first_mod";
    CreateDirectoryW(folder.c_str(), nullptr);
    write(folder + L"\\main.lua", original_lua);
    write(folder + L"\\mod.json", original_json);
    const auto m = emf::core::parse_manifest(original_json);
    REQUIRE_MESSAGE(m.manifest, m.error);
    for (const auto& d : m.manifest->settings) REQUIRE(store.declare("my_first_mod", d));
    EmfModInfo info{};
    info.size = sizeof info;
    info.status = EMF_MOD_SCRIPT;
    std::snprintf(info.id, sizeof info.id, "my_first_mod");
    std::snprintf(info.source, sizeof info.source, "my_first_mod\\");
    std::snprintf(info.main, sizeof info.main, "main.lua");
    r.fake.mods.push_back(info);
    std::vector<uint8_t> t(sizeof(ecs::CurrentTransform), 0);
    reinterpret_cast<ecs::CurrentTransform*>(t.data())->transform.position.x = static_cast<int64_t>(123.0 * 4294967296.0);
    r.fake.components[{ecs::CurrentTransform::kHash, 1}] = t;
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(host.status("my_first_mod") == "running");
    host.tick(kClient);
    CHECK(r.fake.hud["my_first_mod.line"] == "Hello from my first mod!  (123, 0, 0)");
    emf::core::ManifestWatch watch;
    emf::core::ModRecord rec;
    rec.id = "my_first_mod";
    rec.name = "My First Mod";
    rec.source = "my_first_mod\\";
    rec.has_manifest = true;
    rec.status = emf::core::ModStatus::Script;
    CHECK(watch.poll(r.dir, {rec}, store).empty());  // the core's first look notes mod.json

    // steps 3 and 5, saved while the game runs: the core declares the new setting, the Lua host reloads the mod
    write_later(folder + L"\\main.lua", lua, 1);
    write_later(folder + L"\\mod.json", json, 1);
    const auto lines = watch.poll(r.dir, {rec}, store);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "mod my_first_mod\\: mod.json changed: 7 setting(s) declared again (the Mods tab shows them now)");
    CHECK(store.get("my_first_mod", "show_position") == "1");
    host.check_reload_now();
    REQUIRE(host.status("my_first_mod") == "running");
    host.tick(kClient);
    CHECK(r.fake.hud["my_first_mod.line"] == "[Hello from my first mod!]  (123, 0, 0)");
    // "Switch it off: the numbers go, the text stays" (the Mods tab saves it through the store)
    REQUIRE(store.set("my_first_mod", "show_position", "0") == emf::core::SettingsStore::SetResult::Ok);
    host.tick(kClient);
    CHECK(r.fake.hud["my_first_mod.line"] == "[Hello from my first mod!]");
    CHECK(ini.values["my_first_mod.show_position"] == "0");

    // step 3's mistake: the last ")" of the changed line taken out
    std::string broken = lua;
    replace_once(broken, new_line, new_line.substr(0, new_line.rfind(')')) + new_line.substr(new_line.rfind(')') + 1));
    write_later(folder + L"\\main.lua", broken, 2);
    host.check_reload_now();
    const std::string why = host.status("my_first_mod");
    CHECK(why.rfind("paused: my_first_mod/main.lua:", 0) == 0);
    CHECK_MESSAGE(guide.find(why.substr(8, why.find(' ', 8) - 8)) != std::string::npos, why);  // the guide shows that place
    g_live = nullptr;
}

// ---- champion (Wandering Champion): a gameplay mod in Lua, run by the host from a copy of mods/champion ----
namespace {
constexpr uint32_t kRunes = 0x41, kSword = 0x43;

void place_entity(Fake& f, uint32_t id, double x, double y, double z) {
    std::vector<uint8_t> t(sizeof(ecs::CurrentTransform), 0);
    auto* ct = reinterpret_cast<ecs::CurrentTransform*>(t.data());
    ct->transform.position.x = static_cast<int64_t>(x * 4294967296.0);
    ct->transform.position.y = static_cast<int64_t>(y * 4294967296.0);
    ct->transform.position.z = static_cast<int64_t>(z * 4294967296.0);
    f.components[{ecs::CurrentTransform::kHash, id}] = t;
}
void give_level(Fake& f, uint32_t id, uint32_t level) {
    std::vector<uint8_t> l(sizeof(ecs::Level), 0);
    reinterpret_cast<ecs::Level*>(l.data())->dataStorage[0] = level;
    f.components[{ecs::Level::kHash, id}] = l;
}
uint32_t level_now(Fake& f, uint32_t id) {
    return reinterpret_cast<ecs::Level*>(f.components[{ecs::Level::kHash, id}].data())->dataStorage[0];
}
void daytime(Fake& f, double hours) {
    std::vector<uint8_t> d(0x70, 0);
    const auto ns = static_cast<int64_t>(hours * 3600e9);
    std::memcpy(d.data() + 0x48, &ns, 8);
    f.singletons["g38_daytime::Daytime"] = d;
}

// The shipped mod copied into the rig's mods folder (its world data stays in the test's folder), its settings at
// mod.json's defaults; player 1 (account 0xAA, level 5) at the origin, at night (22:00), no flame altar near.
struct ChampionRig {
    Rig r{L"champion"};
    std::unique_ptr<Host> host;
    std::wstring folder, data;
    explicit ChampionRig(const std::map<std::string, std::string>& settings = {}) {
        r.api.local_player = one_player;
        folder = r.dir + L"\\champion";
        CreateDirectoryW(folder.c_str(), nullptr);
        const std::string src = EMF_SOURCE_DIR "/mods/champion/";
        for (const char* name : {"main.lua", "mod.json"}) {
            const std::string from = src + name;
            REQUIRE(CopyFileW(std::wstring(from.begin(), from.end()).c_str(),
                              (folder + L"\\" + std::wstring(name, name + std::strlen(name))).c_str(), FALSE));
        }
        std::ifstream f(src + "mod.json", std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const auto m = emf::core::parse_manifest(ss.str());
        REQUIRE_MESSAGE(m.manifest, m.error);
        for (const auto& d : m.manifest->settings) r.fake.settings["champion." + d.key] = d.default_value;
        for (const auto& [k, v] : settings) r.fake.settings["champion." + k] = v;
        EmfModInfo info{};
        info.size = sizeof info;
        info.status = EMF_MOD_SCRIPT;
        std::snprintf(info.id, sizeof info.id, "champion");
        std::snprintf(info.source, sizeof info.source, "champion\\");
        std::snprintf(info.main, sizeof info.main, "main.lua");
        r.fake.mods.push_back(info);
        r.fake.components[{ecs::PlayerState::kHash, 1}] = std::vector<uint8_t>(ecs::PlayerState::kSize, 0);
        r.fake.player_info(0, 1, 0xAA);
        place_entity(r.fake, 1, 0, 10, 0);
        give_level(r.fake, 1, 5);
        give_level(r.fake, 1234, 1);  // the first spawn (the champion) has a Level to write, and health
        std::vector<uint8_t> nh(sizeof(ecs::NetworkHealth), 0);
        reinterpret_cast<ecs::NetworkHealth*>(nh.data())->health = 1498;
        r.fake.components[{ecs::NetworkHealth::kHash, 1234}] = nh;
        r.fake.item_names[kRunes] = "Material_T1_Token_WeaponUpgrade_Rune01";
        r.fake.item_names[kSword] = "Weapon_T1_1H_SwordEpic_01";
        daytime(r.fake, 22.0);
        data = r.dir + L"\\..\\config\\champion\\0001.txt";
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(data).parent_path().lexically_normal(), ec);
        DeleteFileW(data.c_str());
        host = std::make_unique<Host>(&r.api, r.dir);
        host->load_all();
        REQUIRE(host->status("champion") == "running");
        fire_load(1);
        seconds(1);
    }
    ~ChampionRig() { DeleteFileW(data.c_str()); }
    void seconds(int n) {
        for (int i = 0; i < n; ++i) {
            r.fake.tick += 60;
            host->tick(kServer);
        }
    }
    // The player walks along x from 0 to `to` in 10 m steps, a second each (the mod notes where they walked).
    void walk(double to) {
        for (double x = 0; x <= to; x += 10) {
            place_entity(r.fake, 1, x, 10, 0);
            seconds(1);
        }
    }
    std::string hud() { return r.fake.hud.count("champion.track") ? r.fake.hud["champion.track"] : ""; }
    std::string reply(const std::string& command) {
        r.fake.replies.clear();
        r.run_command(command);
        return r.fake.replies.empty() ? "" : r.fake.replies.back();
    }
    void send_events() {  // the host's program sends what the mod emitted
        for (auto& p : r.fake.programs) p.fn(nullptr, p.user);
    }
    bool emitted_guard(uint32_t who, uint32_t whom) {
        for (const auto& [name, b] : r.fake.emitted) {
            if (name != "StartGuardingEvent") continue;
            uint32_t a = 0, c = 0;
            std::memcpy(&a, &b[offsetof(ecs::StartGuardingEvent, guardingEntityId)], 4);
            std::memcpy(&c, &b[offsetof(ecs::StartGuardingEvent, guardedEntityId)], 4);
            if (a == who && c == whom) return true;
        }
        return false;
    }
};
double flat_distance(const Fake::Spawn& s, double x, double z) { return std::hypot(s.x - x, s.z - z); }
}  // namespace

TEST_CASE("lua example champion: summoned 80-150 m away on a spot the player walked, at their level + 2, announced, on the HUD") {
    ChampionRig c;
    c.walk(200);  // now at x = 200: the spots from x = 50 to 120 are 80-150 m behind
    CHECK(c.reply("champion.summon").find("summoned Vukah Brawler") == 0);
    REQUIRE(c.r.fake.spawns.size() == 2);  // the champion and its patrol marker
    const auto& s = c.r.fake.spawns[0];
    CHECK(s.guid[0] == 0x08);  // 9c8e2008-...: Enemy_Midboss_Vukah_Bigfoot_BossHealthBar (Windows GUID layout)
    CHECK(flat_distance(s, 200, 0) >= 80.0);
    CHECK(flat_distance(s, 200, 0) <= 150.0);
    CHECK(s.y == doctest::Approx(11.0));  // a metre above the walked ground
    CHECK(level_now(c.r.fake, 1234) == 7u);
    CHECK(noted(c.r.fake, "A champion roams: Vukah Brawler (level 7)"));
    CHECK(c.hud().rfind("Champion: Vukah Brawler (level 7) - ", 0) == 0);
    c.send_events();
    CHECK(c.emitted_guard(1234, 1235));  // it patrols: it guards a marker on a walked spot
}

TEST_CASE("lua example champion: slain by a player's hit, the slayer gets the bonus (gear at their level, rare) and a tally") {
    ChampionRig c;
    c.walk(200);
    c.r.run_command("champion.summon");
    c.r.fake.puts.clear();
    send(c.r.fake, "HitEvent", hit(1, 1234));
    send(c.r.fake, "EntityDiedEvent", died(1234));
    REQUIRE(c.r.fake.puts.size() == 2);
    CHECK(c.r.fake.puts[0].item == kRunes);
    CHECK(c.r.fake.puts[0].count == 10u);
    CHECK(c.r.fake.puts[1].item == kSword);
    CHECK(c.r.fake.puts[1].entity == 1u);
    CHECK(c.r.fake.puts[1].level == 5u);
    CHECK(c.r.fake.puts[1].rarity == 2);
    CHECK((c.r.fake.puts[1].flags & EMF_ITEMS_RARITY) != 0);
    CHECK(noted(c.r.fake, "Vukah Brawler slain by player 1!"));
    CHECK(std::find(c.r.fake.destroyed.begin(), c.r.fake.destroyed.end(), 1235u) != c.r.fake.destroyed.end());  // marker
    CHECK(c.hud().empty());
    CHECK(read_text(c.data).find("slain 0x00000000000000aa 1") != std::string::npos);
    CHECK(c.reply("champion.stats") == "Champions slain in this world: 1 by you, 1 in all");
}

TEST_CASE("lua example champion: after the interval outdoors at night a champion comes (chance 100); by day with night_only none") {
    ChampionRig c({{"interval_minutes", "5"}, {"chance", "100"}});
    daytime(c.r.fake, 12.0);
    c.seconds(400);
    CHECK(c.r.fake.spawns.empty());  // by day: not even the time counts
    daytime(c.r.fake, 22.0);
    c.seconds(290);
    CHECK(c.r.fake.spawns.empty());
    c.seconds(15);
    REQUIRE_FALSE(c.r.fake.spawns.empty());
    const auto& s = c.r.fake.spawns[0];
    CHECK(flat_distance(s, 0, 0) >= 80.0);  // nowhere walked that far: in a random direction at the player's height + 3
    CHECK(flat_distance(s, 0, 0) <= 150.0);
    CHECK(s.y == doctest::Approx(13.0));
}

TEST_CASE("lua example champion: it leaves after its lifetime, at dawn, and when everyone is far away") {
    {
        ChampionRig c({{"lifetime_minutes", "2"}});
        c.walk(200);
        c.r.run_command("champion.summon");
        place_entity(c.r.fake, 1234, 120, 10, 0);
        c.seconds(110);
        CHECK(std::find(c.r.fake.destroyed.begin(), c.r.fake.destroyed.end(), 1234u) == c.r.fake.destroyed.end());
        c.seconds(15);
        CHECK(std::find(c.r.fake.destroyed.begin(), c.r.fake.destroyed.end(), 1234u) != c.r.fake.destroyed.end());
        CHECK(noted(c.r.fake, "The champion has left"));
        CHECK(c.reply("champion.where") == "No champion roams now");
    }
    {
        ChampionRig c;
        c.r.run_command("champion.summon");
        place_entity(c.r.fake, 1234, 100, 10, 0);
        daytime(c.r.fake, 6.5);
        c.seconds(1);
        CHECK(noted(c.r.fake, "fades with the dawn"));
    }
    {
        ChampionRig c;
        c.r.run_command("champion.summon");
        place_entity(c.r.fake, 1234, 100, 10, 0);
        place_entity(c.r.fake, 1, 500, 10, 0);
        c.seconds(1);
        CHECK(noted(c.r.fake, "wandered off"));
    }
}

TEST_CASE("lua example champion: a player within 30 m is hunted (guard + threat); champion.where answers a player in chat") {
    ChampionRig c;
    c.r.run_command("champion.summon");
    place_entity(c.r.fake, 1234, 20, 10, 0);
    c.seconds(1);
    c.send_events();
    CHECK(c.emitted_guard(1234, 1));
    bool threat = false;
    for (const auto& [name, b] : c.r.fake.emitted) threat |= name == "AddThreatTargetEvent";
    CHECK(threat);
    // another player asks from the chat
    c.r.fake.components[{ecs::PlayerState::kHash, 2}] = std::vector<uint8_t>(ecs::PlayerState::kSize, 0);
    c.r.fake.player_info(1, 2, 0xBB);
    place_entity(c.r.fake, 2, 20, 10, 100);
    c.r.fake.sender = EmfCommandSender{sizeof(EmfCommandSender), 2, 0xBB, 1, {}};
    CHECK(c.reply("champion.where") == "The champion (Vukah Brawler, level 7) is 100 m S of you");
    c.r.fake.sender = EmfCommandSender{};
    bool for_players = false;
    for (const auto& cmd : c.r.fake.commands) for_players |= cmd.name == "champion.where" && cmd.flags == EMF_COMMAND_PLAYERS;
    CHECK(for_players);
}

TEST_CASE("lua example champion: the table at the top of main.lua is live: edit a GUID, save, the next one is that template") {
    ChampionRig c;
    std::string lua = read_text(c.folder + L"\\main.lua");
    const std::string from = "9c8e2008-7259-4114-b6fc-615f0f181c04", to = "d5d05217-52dc-467a-9192-26b4f661d8d9";
    REQUIRE(lua.find(from) != std::string::npos);
    lua.replace(lua.find(from), from.size(), to);
    write_later(c.folder + L"\\main.lua", lua, 1);
    c.host->check_reload_now();
    REQUIRE(c.host->status("champion") == "running");
    c.seconds(1);
    c.r.run_command("champion.summon");
    REQUIRE_FALSE(c.r.fake.spawns.empty());
    CHECK(c.r.fake.spawns[0].guid[0] == 0x17);  // d5d05217-...
}

TEST_CASE("lua example champion: a reload of the file keeps the champion out (same session); a new session forgets it") {
    ChampionRig c;
    c.walk(200);
    c.r.run_command("champion.summon");
    REQUIRE(c.r.fake.spawns.size() == 2);
    write_later(c.folder + L"\\main.lua", read_text(c.folder + L"\\main.lua") + "\n-- edited\n", 1);
    c.host->check_reload_now();
    c.seconds(2);
    CHECK(c.r.fake.spawns.size() == 2);  // taken over, not summoned again
    CHECK(c.reply("champion.where").rfind("The champion (Vukah Brawler, level 7) is ", 0) == 0);
    CHECK(c.hud().rfind("Champion: Vukah Brawler", 0) == 0);
    c.r.fake.session = 2;  // a reload of the world: the champion was not saved
    c.seconds(1);
    CHECK(c.hud().empty());
    CHECK(c.reply("champion.where") == "No champion roams now");
    CHECK(c.r.fake.destroyed.empty());  // ids of the new session are someone else's
}

TEST_CASE("lua example champion: champion.kill kills it without a hit; the nearest player gets the bonus") {
    ChampionRig c;
    c.r.run_command("champion.summon");
    place_entity(c.r.fake, 1234, 30, 10, 0);
    std::vector<uint8_t> h(sizeof(ecs::Health), 0);
    reinterpret_cast<ecs::Health*>(h.data())->dataStorage[0] = 1498;
    c.r.fake.components[{ecs::Health::kHash, 1234}] = h;
    std::vector<uint8_t> n(sizeof(ecs::NetworkHealth), 0);
    reinterpret_cast<ecs::NetworkHealth*>(n.data())->health = 1498;
    c.r.fake.components[{ecs::NetworkHealth::kHash, 1234}] = n;
    CHECK(c.reply("champion.kill") == "the champion dies");
    CHECK(reinterpret_cast<ecs::Health*>(c.r.fake.components[{ecs::Health::kHash, 1234}].data())->dataStorage[0] == 0);
    c.r.fake.puts.clear();
    send(c.r.fake, "EntityDiedEvent", died(1234));  // the game's death program, a step later
    REQUIRE(c.r.fake.puts.size() == 2);
    CHECK(c.r.fake.puts[1].entity == 1u);
    CHECK(noted(c.r.fake, "slain by player 1"));
}

TEST_CASE("lua example champion: a dead player near it is not hunted (a threat toward the dead made a raider wander off)") {
    ChampionRig c;
    std::vector<uint8_t> n(sizeof(ecs::NetworkHealth), 0);  // player 1 is dead (health 0)
    c.r.fake.components[{ecs::NetworkHealth::kHash, 1}] = n;
    c.r.run_command("champion.summon");
    place_entity(c.r.fake, 1234, 20, 10, 0);
    c.seconds(1);
    c.send_events();
    CHECK_FALSE(c.emitted_guard(1234, 1));
    for (const auto& [name, b] : c.r.fake.emitted) CHECK(name != "AddThreatTargetEvent");
    CHECK(c.reply("champion.where").rfind("The champion (Vukah Brawler, level 7) is 20 m", 0) == 0);  // it is still out
}

TEST_CASE("lua example champion: summoned by day for a test it stays (only a champion that saw the night fades at dawn)") {
    ChampionRig c;
    daytime(c.r.fake, 12.0);
    c.r.run_command("champion.summon");
    place_entity(c.r.fake, 1234, 100, 10, 0);
    c.seconds(5);
    CHECK_FALSE(noted(c.r.fake, "fades with the dawn"));
    CHECK(c.reply("champion.where").rfind("The champion (Vukah Brawler, level 7)", 0) == 0);
    daytime(c.r.fake, 22.0);  // night falls, then day comes: now it fades
    c.seconds(1);
    daytime(c.r.fake, 6.5);
    c.seconds(1);
    CHECK(noted(c.r.fake, "fades with the dawn"));
}

TEST_CASE("lua example champion: each patrol hop is saved (a reload takes the current marker over); a smashed marker is not destroyed") {
    ChampionRig c;
    c.walk(200);
    c.r.run_command("champion.summon");  // champion 1234, marker 1235
    place_entity(c.r.fake, 1234, 100, 10, 0);
    c.seconds(61);  // the next patrol spot: marker 1236
    REQUIRE(c.r.fake.spawns.size() == 3);
    CHECK(read_text(c.data).find(" 1234 1236 ") != std::string::npos);
    c.r.fake.components.erase({ecs::CurrentTransform::kHash, 1236});  // a player smashed the barrel
    c.r.fake.destroyed.clear();
    c.r.run_command("champion.dismiss");
    CHECK(std::find(c.r.fake.destroyed.begin(), c.r.fake.destroyed.end(), 1236u) == c.r.fake.destroyed.end());
    CHECK(std::find(c.r.fake.destroyed.begin(), c.r.fake.destroyed.end(), 1234u) != c.r.fake.destroyed.end());
}

TEST_CASE("lua example champion: a command right after a world reload touches no id of the new session") {
    ChampionRig c;
    c.r.run_command("champion.summon");
    c.r.fake.session = 2;  // the world reloaded; no tick has run yet
    c.r.fake.destroyed.clear();
    CHECK(c.reply("champion.kill") == "No champion roams now");
    CHECK(c.reply("champion.dismiss") == "No champion roams now");
    CHECK(c.r.fake.destroyed.empty());
}

// ---- champion: edge cases ----
namespace {
void living(Fake& f, uint32_t id, uint32_t level, uint32_t health) {
    give_level(f, id, level);
    std::vector<uint8_t> n(sizeof(ecs::NetworkHealth), 0);
    reinterpret_cast<ecs::NetworkHealth*>(n.data())->health = health;
    f.components[{ecs::NetworkHealth::kHash, id}] = n;
}
std::string live_token(const std::string& data) {  // the token of the saved "live <token> ..." line
    const size_t at = data.find("live ");
    if (at == std::string::npos) return "";
    const size_t end = data.find(' ', at + 5);
    return data.substr(at + 5, end - at - 5);
}
}  // namespace

TEST_CASE("lua example champion: a champion saved by an earlier run of the game is never taken over; its stale line goes") {
    ChampionRig c;
    place_entity(c.r.fake, 4000, 5, 10, 5);  // after a restart the ids are someone else's: a prop
    place_entity(c.r.fake, 4001, 6, 10, 6);  // and a living creature
    living(c.r.fake, 4001, 7, 100);
    write(c.data, "slain 0x00000000000000aa 2\nlive 1 4000 4001 999999 7 Vukah_Brawler 6.0 10.0 6.0\n");
    fire_load(1);  // the same world loads again in session 1 of this run (sessions start at 1 in every run)
    c.seconds(2);
    CHECK(c.reply("champion.where") == "No champion roams now");
    CHECK(c.r.fake.destroyed.empty());
    CHECK(read_text(c.data).find("live") == std::string::npos);  // rewritten without the stale line
    CHECK(read_text(c.data).find("slain 0x00000000000000aa 2") != std::string::npos);
}

TEST_CASE("lua example champion: a takeover checks who it is: alive, its saved level; never a corpse; the marker only where it was put") {
    ChampionRig c;
    c.r.run_command("champion.summon");
    const std::string token = live_token(read_text(c.data));
    REQUIRE_FALSE(token.empty());
    c.r.run_command("champion.dismiss");
    c.r.fake.destroyed.clear();
    place_entity(c.r.fake, 4000, 5, 10, 5);   // no Health, no Level: a prop
    place_entity(c.r.fake, 4002, 7, 10, 7);
    living(c.r.fake, 4002, 3, 100);           // alive, another level
    place_entity(c.r.fake, 4003, 8, 10, 8);
    living(c.r.fake, 4003, 7, 0);             // the right level, but dead: a corpse
    for (uint32_t id : {4000u, 4002u, 4003u}) {
        CAPTURE(id);
        write(c.data, "live " + token + " " + std::to_string(id) + " 0 999999 7 Vukah_Brawler 0 0 0\n");
        fire_load(1);
        c.seconds(1);
        CHECK(c.reply("champion.where") == "No champion roams now");
    }
    CHECK(c.r.fake.destroyed.empty());
    // the real one: alive at level 7; its marker id now holds something that is not where the barrel was put
    place_entity(c.r.fake, 4004, 9, 10, 9);
    living(c.r.fake, 4004, 7, 500);
    place_entity(c.r.fake, 4005, 50, 10, 50);
    write(c.data, "live " + token + " 4004 4005 999999 7 Vukah_Brawler 9.0 10.5 9.0\n");
    fire_load(1);
    c.seconds(1);
    CHECK(c.reply("champion.where").rfind("The champion (Vukah Brawler, level 7)", 0) == 0);
    c.r.run_command("champion.dismiss");
    CHECK(std::find(c.r.fake.destroyed.begin(), c.r.fake.destroyed.end(), 4004u) != c.r.fake.destroyed.end());
    CHECK(std::find(c.r.fake.destroyed.begin(), c.r.fake.destroyed.end(), 4005u) == c.r.fake.destroyed.end());
}

TEST_CASE("lua example champion: after a takeover it patrols again once players have walked") {
    ChampionRig c;
    c.walk(200);
    c.r.run_command("champion.summon");
    write_later(c.folder + L"\\main.lua", read_text(c.folder + L"\\main.lua") + "\n-- edited\n", 1);
    c.host->check_reload_now();  // the file reloads: the walked spots are gone with the old state
    c.seconds(2);
    const size_t before = c.r.fake.spawns.size();
    c.walk(200);  // players walk again
    c.seconds(61);
    CHECK(c.r.fake.spawns.size() > before);  // a new patrol marker
}

TEST_CASE("lua example champion: a fallback spot is never inside a base") {
    ChampionRig c;
    place_entity(c.r.fake, 900, 0, 10, 0);  // a flame altar where the player stands
    c.r.fake.components[{ecs::BaseIdComponent::kHash, 900}] = std::vector<uint8_t>(sizeof(ecs::BaseIdComponent), 0);
    c.r.fake.entities = {900};
    c.seconds(61);  // the altars are read once a minute
    CHECK(c.reply("champion.summon 30").find("not summoned: no place outside the bases") == 0);
    CHECK(c.r.fake.spawns.empty());
    c.r.fake.components.erase({ecs::BaseIdComponent::kHash, 900});
    c.seconds(61);
    CHECK(c.reply("champion.summon 30").rfind("summoned", 0) == 0);
}

TEST_CASE("lua example champion: a bonus that does not fit (or is refused) is dropped at the slayer's feet, not lost") {
    ChampionRig c;
    c.r.run_command("champion.summon");
    c.r.fake.puts.clear();
    g_put_ok = false;  // the put is refused at once
    send(c.r.fake, "HitEvent", hit(1, 1234));
    send(c.r.fake, "EntityDiedEvent", died(1234));
    g_put_ok = true;
    REQUIRE(c.r.fake.gives.size() == 2);
    CHECK(c.r.fake.gives[0].item == kRunes);
    CHECK(c.r.fake.gives[0].count == 10u);
    CHECK(c.r.fake.gives[1].item == kSword);
    CHECK(c.r.fake.gives[1].player == 1u);
    // a put that only partly fits: the rest is dropped when the game says so
    ChampionRig d;
    d.r.run_command("champion.summon");
    d.r.fake.puts.clear();
    send(d.r.fake, "HitEvent", hit(1, 1234));
    send(d.r.fake, "EntityDiedEvent", died(1234));
    REQUIRE(d.r.fake.puts.size() == 2);
    const EmfItemsRequest& runes = d.r.fake.puts[0];
    REQUIRE(runes.cb);
    runes.cb(kServer, runes.entity, runes.item, runes.count, 4, runes.user);  // 4 of 10 fitted
    REQUIRE(d.r.fake.gives.size() == 1);
    CHECK(d.r.fake.gives[0].count == 6u);
}

TEST_CASE("lua example champion: its line on screen goes when no hosted world runs any more (joining someone else's game, M9)") {
    ChampionRig c;
    c.r.run_command("champion.summon");
    c.seconds(1);
    REQUIRE_FALSE(c.hud().empty());
    for (int i = 0; i < 200; ++i) c.host->tick(kClient);  // only a client world steps now
    CHECK(c.hud().empty());
}

// ---- examples/lua: small example mods for modders, one API area each ----
namespace {
std::map<uint32_t, bool> g_ex_keys;
bool ex_key(uint32_t vk) { return g_ex_keys[vk]; }
int g_hud_calls = 0;  // how often a mod set a HUD line (a widget redraws only when its text changes)
bool counting_hud(const char* id, const char* text, uint32_t place, uint32_t rgba) {
    ++g_hud_calls;
    return f_hud(id, text, place, rgba);
}
// Every folder in examples/lua (read from disk, so a new example is never left out of the checks below).
std::vector<std::string> lua_examples() {
    std::vector<std::string> out;
    for (const auto& e : std::filesystem::directory_iterator(EMF_SOURCE_DIR "/examples/lua"))
        if (e.is_directory()) out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}
// The examples that have their own tests in this file. A new folder fails the first test until it is added here (with
// tests of its own).
const std::set<std::string> kTestedExamples = {"chat_command", "hotkey_wheel", "hud_widget",
                                               "loot_ticker",  "pet_follow",   "settings_tour"};

EmfCommandSender sender_of(uint32_t entity, uint64_t account, bool chat, bool admin) {
    EmfCommandSender s{};
    s.size = sizeof s;
    s.entity = entity;
    s.account = account;
    s.from_chat = chat ? 1 : 0;
    s.admin = admin ? 1 : 0;
    return s;
}

// examples/lua/<id> as a player installs it: copied into the rig's mods folder (what it writes lands in the test's
// folder, which stands for <game>\emf), its settings declared from its mod.json by the core's settings store (changes
// reach the mod as the core sends them: after the next world step), player 1 (account 0xAA) at (10, 20, 30) m.
struct ExampleRig {
    std::string id;
    Rig r;
    LiveIni ini;
    emf::core::SettingsStore store{&ini};
    std::unique_ptr<Host> host;
    std::wstring folder, emf_dir;
    explicit ExampleRig(const std::string& example) : id(example), r(std::wstring(example.begin(), example.end()).c_str()) {
        g_ex_keys.clear();
        g_hud_calls = 0;
        g_live = &store;
        r.api.local_player = one_player;
        r.api.setting_int = live_int;
        r.api.setting_float = live_float;
        r.api.setting_text = live_text;
        r.api.setting_set = live_set;
        r.api.hud_set = counting_hud;
        folder = r.dir + L"\\" + std::wstring(id.begin(), id.end());
        emf_dir = r.dir.substr(0, r.dir.find_last_of(L'\\'));
        DeleteFileW((emf_dir + L"\\commands.txt").c_str());
        DeleteFileW((emf_dir + L"\\commands.done.txt").c_str());
        CreateDirectoryW(folder.c_str(), nullptr);
        const std::string src = EMF_SOURCE_DIR "/examples/lua/" + id + "/";
        for (const char* name : {"main.lua", "mod.json"}) {
            const std::string from = src + name;
            REQUIRE(CopyFileW(std::wstring(from.begin(), from.end()).c_str(),
                              (folder + L"\\" + std::wstring(name, name + std::strlen(name))).c_str(), FALSE));
        }
        const auto m = emf::core::parse_manifest(read_all(src + "mod.json"));
        REQUIRE_MESSAGE(m.manifest, m.error);
        REQUIRE(m.manifest->id == id);  // the folder's name is the mod's id
        for (const auto& d : m.manifest->settings) {
            CAPTURE(d.key);
            CHECK(d.problem.empty());
            std::string why;
            REQUIRE(store.declare(id, d, &why));
            CHECK(why.empty());
        }
        store.take_changes();
        EmfModInfo info{};
        info.size = sizeof info;
        info.status = EMF_MOD_SCRIPT;
        std::snprintf(info.id, sizeof info.id, "%s", id.c_str());
        std::snprintf(info.source, sizeof info.source, "%s\\", id.c_str());
        std::snprintf(info.main, sizeof info.main, "main.lua");
        r.fake.mods.push_back(info);
        r.fake.components[{ecs::PlayerState::kHash, 1}] = std::vector<uint8_t>(ecs::PlayerState::kSize, 0);
        r.fake.player_info(0, 1, 0xAA);
        place_entity(r.fake, 1, 10, 20, 30);
        host = std::make_unique<Host>(&r.api, r.dir);
        host->set_key_reader(ex_key);
        host->load_all();
        REQUIRE(host->status(id) == "running");
    }
    ~ExampleRig() {
        host.reset();
        g_live = nullptr;
    }
    // The core tells the mod about each setting change (the Mods tab, set, or the mod's own set_setting) after a step.
    void pump(EmfWorld* world) {
        for (const auto& [mod, key] : store.take_changes())
            for (auto& [m, cb] : r.fake.setting_watchers)
                if (m == mod) cb.first(world, mod.c_str(), key.c_str(), cb.second);
    }
    void tick(EmfWorld* world) {
        host->tick(world);
        pump(world);
    }
    void seconds(int n, EmfWorld* world = kServer) {
        for (int i = 0; i < n * 60; ++i) {
            ++r.fake.tick;
            tick(world);
        }
    }
    void set(const std::string& key, const std::string& value) {  // a player changes it in the Mods tab
        REQUIRE(store.set(id, key, value) == emf::core::SettingsStore::SetResult::Ok);
    }
    void press(uint32_t vk, EmfWorld* world) {
        g_ex_keys[vk] = true;
        tick(world);
        g_ex_keys[vk] = false;
        tick(world);
    }
    std::string hud(const std::string& line) {
        const std::string k = id + "." + line;
        return r.fake.hud.count(k) ? r.fake.hud[k] : "";
    }
    std::pair<uint32_t, uint32_t> style(const std::string& line) { return r.fake.hud_style[id + "." + line]; }
    std::vector<std::string> replies(const std::string& command) {
        r.fake.replies.clear();
        r.run_command(command);
        pump(kServer);
        return r.fake.replies;
    }
    std::string reply(const std::string& command) {
        const auto all = replies(command);
        return all.empty() ? "" : all.back();
    }
    bool for_players(const std::string& command) {
        for (const auto& c : r.fake.commands)
            if (c.name == command) return c.flags == EMF_COMMAND_PLAYERS;
        return false;
    }
    void send_events() {  // the host's program sends what the mod emitted
        for (auto& p : r.fake.programs) p.fn(nullptr, p.user);
    }
    int guards(uint32_t who, uint32_t whom) {
        int n = 0;
        for (const auto& [name, b] : r.fake.emitted) {
            if (name != "StartGuardingEvent") continue;
            uint32_t a = 0, c = 0;
            std::memcpy(&a, &b[offsetof(ecs::StartGuardingEvent, guardingEntityId)], 4);
            std::memcpy(&c, &b[offsetof(ecs::StartGuardingEvent, guardedEntityId)], 4);
            n += a == who && c == whom;
        }
        return n;
    }
};
}  // namespace

TEST_CASE("lua examples: every folder in examples/lua loads, has its own tests here and follows the emf-field rule") {
    const auto found = lua_examples();
    CHECK(std::set<std::string>(found.begin(), found.end()) == kTestedExamples);
    for (const auto& id : found) {
        CAPTURE(id);
        ExampleRig x(id);  // its mod.json parses, every setting declares, main.lua runs
        x.tick(kServer);
        x.tick(kClient);
        CHECK(x.host->status(id) == "running");
        // the rule (docs/modding/README.md, "Examples"): "emf" is the API version of the Lua host it depends on
        const std::string json = read_all(EMF_SOURCE_DIR "/examples/lua/" + id + "/mod.json");
        const bool needs_14 = json.find(R"("lua": ">=1.4")") != std::string::npos;
        CHECK((needs_14 || json.find(R"("lua": ">=1.3")") != std::string::npos));
        CHECK(json.find(needs_14 ? R"("emf": 17)" : R"("emf": 14)") != std::string::npos);
    }
}

TEST_CASE("lua examples: a README that says how to install it (at most 60 lines), main.lua at most 170, listed in the docs") {
    const std::string docs = read_all(EMF_SOURCE_DIR "/docs/modding/README.md");
    for (const auto& id : lua_examples()) {
        CAPTURE(id);
        const std::string dir = EMF_SOURCE_DIR "/examples/lua/" + id + "/";
        const std::string readme = read_all(dir + "README.md");
        CHECK(readme.find("emf\\mods\\") != std::string::npos);  // copy the folder into <game>\emf\mods\ ...
        CHECK(readme.find("## What it teaches") != std::string::npos);
        CHECK(readme.find("## Try it") != std::string::npos);
        CHECK(std::count(readme.begin(), readme.end(), '\n') <= 60);
        const std::string lua = read_all(dir + "main.lua");
        CHECK(std::count(lua.begin(), lua.end(), '\n') <= 170);  // a screen or two; the champion is the big example
        CHECK(docs.find("examples/lua/" + id) != std::string::npos);
    }
    CHECK(docs.find("## Examples") != std::string::npos);
    CHECK(docs.find("examples/cpp/waypoints") != std::string::npos);
}

// settings_tour: one setting of every kind
TEST_CASE("lua example settings_tour: one setting of every kind; the screen shows their values in the corner and colour chosen") {
    ExampleRig x("settings_tour");
    const auto m = emf::core::parse_manifest(read_all(EMF_SOURCE_DIR "/examples/lua/settings_tour/mod.json"));
    REQUIRE(m.manifest);
    std::set<std::string> kinds, groups;
    bool when = false, not_live = false, max_length = false;
    for (const auto& d : m.manifest->settings) {
        std::string k = emf::core::setting_type_name(d.type);
        if (d.slider) k += d.type == emf::core::SettingType::Int ? " slider" : " decimal slider";
        kinds.insert(k);
        if (!d.group.empty()) groups.insert(d.group);
        when |= !d.when_key.empty();
        not_live |= !d.live;
        max_length |= d.type == emf::core::SettingType::Text && d.max_length > 0;
    }
    for (const char* k : {"bool", "int", "float", "int slider", "float decimal slider", "choice", "select", "color", "key", "text"})
        CHECK_MESSAGE(kinds.count(k), k);
    CHECK(groups.size() >= 3);
    CHECK(when);
    CHECK(not_live);  // the Mods tab says "next start" for it
    CHECK(max_length);
    x.tick(kServer);  // the server world draws nothing
    CHECK(x.hud("01").empty());
    x.tick(kClient);
    CHECK(x.hud("01") == "Tour  count 3 | scale 1.50 | volume 0.50");
    CHECK(x.hud("02") == "mood calm (0) | wildness 5");
    CHECK(x.hud("03") == "corner bottom_left | colour #80d0ff | mood key F4");
    CHECK(x.style("01") == std::make_pair(uint32_t(EMF_HUD_BOTTOM_LEFT), 0x80D0FFFFu));
}

TEST_CASE("lua example settings_tour: a change in the Mods tab is logged and shown at once; switched off, the lines go") {
    ExampleRig x("settings_tour");
    x.tick(kClient);
    x.set("count", "7");
    x.tick(kClient);
    CHECK(logged("[settings_tour] count is now 7"));
    x.tick(kClient);
    CHECK(x.hud("01") == "Tour  count 7 | scale 1.50 | volume 0.50");
    x.set("mood", "wild");
    x.set("corner", "top_right");
    x.set("color", "#ff8000");
    x.tick(kClient);
    x.tick(kClient);
    CHECK(logged("[settings_tour] mood is now 2 (wild)"));
    CHECK(x.hud("02") == "mood wild (2) | wildness 5");
    CHECK(x.style("03") == std::make_pair(uint32_t(EMF_HUD_TOP_RIGHT), 0xFF8000FFu));
    x.set("show", "0");
    x.tick(kClient);
    x.tick(kClient);
    CHECK(x.hud("01").empty());
    CHECK(x.hud("02").empty());
    CHECK(x.hud("03").empty());
}

TEST_CASE("lua example settings_tour: its command changes a setting with set_setting, checked as the Mods tab checks it") {
    ExampleRig x("settings_tour");
    CHECK(x.reply("settings_tour.set count 12") == "count is now 12");
    CHECK(x.store.get("settings_tour", "count") == "12");
    CHECK(x.reply("settings_tour.set wildness 99").rfind("refused:", 0) == 0);  // a slider from 1 to 10
    CHECK(x.store.get("settings_tour", "wildness") == "5");
    CHECK(x.reply("settings_tour.set label far-too-long-for-it").rfind("refused:", 0) == 0);  // maxLength 12
    CHECK(x.reply("settings_tour.set nothing 1").rfind("no setting nothing (", 0) == 0);
    CHECK(x.reply("settings_tour.set").rfind("usage:", 0) == 0);
    CHECK(logged("[settings_tour] count is now 12"));  // its own change comes back through on_setting_changed
}

TEST_CASE("lua example settings_tour: its key cycles the mood; a key bound anew in the Mods tab works at once") {
    ExampleRig x("settings_tour");
    x.press(VK_F4, kClient);
    CHECK(x.store.get("settings_tour", "mood") == "1");
    x.press(VK_F4, kClient);
    CHECK(x.store.get("settings_tour", "mood") == "2");
    x.set("mood_key", "G");
    x.tick(kClient);
    x.press(VK_F4, kClient);
    CHECK(x.store.get("settings_tour", "mood") == "2");
    x.press('G', kClient);
    CHECK(x.store.get("settings_tour", "mood") == "0");
    x.tick(kClient);
    CHECK(x.hud("02") == "mood calm (0) | wildness 5");
    CHECK(x.hud("03") == "corner bottom_left | colour #80d0ff | mood key G");
}

TEST_CASE("lua example settings_tour: the greeting is read once when the mod loads (not live: \"next start\")") {
    ExampleRig x("settings_tour");
    x.tick(kClient);
    REQUIRE_FALSE(x.r.fake.notes.empty());
    CHECK(x.r.fake.notes.back() == "Welcome to the settings tour");
    x.set("greeting", "Hello again");
    for (int i = 0; i < 5; ++i) x.tick(kClient);
    CHECK_FALSE(noted(x.r.fake, "Hello again"));
    CHECK(x.r.fake.notes.size() == 1);
}

TEST_CASE("lua example settings_tour: the mod's own switch (\"enabled\") is not logged as a setting") {
    ExampleRig x("settings_tour");
    for (auto& [mod, cb] : x.r.fake.setting_watchers)
        if (mod == "settings_tour") cb.first(kClient, "settings_tour", "enabled", cb.second);
    CHECK(x.host->status("settings_tour") == "running");
    CHECK_FALSE(logged("enabled is now"));
}

// chat_command: commands any player may run, answered to the sender
namespace {
void altar(Fake& f, uint32_t id, double x, double y, double z) {
    place_entity(f, id, x, y, z);
    f.components[{ecs::BaseIdComponent::kHash, id}] = std::vector<uint8_t>(sizeof(ecs::BaseIdComponent), 0);
    f.entities.push_back(id);
}
uint32_t nobody() { return 0; }
}  // namespace

TEST_CASE("lua example chat_command: where and home answer the host at the console from the host's position") {
    ExampleRig x("chat_command");
    altar(x.r.fake, 900, 110, 20, 30);  // 100 m from player 1
    altar(x.r.fake, 901, 10, 5, 330);   // 300 m
    place_entity(x.r.fake, 50, 11, 20, 30);  // something else, no altar
    x.r.fake.entities.push_back(50);
    x.r.fake.sender = sender_of(0, 0, false, true);
    const auto where = x.replies("chat_command.where");
    REQUIRE(where.size() == 2);
    CHECK(where[0] == "You are at 10, 20, 30");
    CHECK(where[1] == "asked by the host (the console or their own chat)");
    CHECK(x.reply("chat_command.home") == "The nearest flame altar (anyone's) is 100 m away (2 in this world)");
    CHECK(x.for_players("chat_command.where"));
    CHECK(x.for_players("chat_command.home"));
}

TEST_CASE("lua example chat_command: a player in the chat gets their own answers; only admins may list every altar") {
    ExampleRig x("chat_command");
    altar(x.r.fake, 900, 110, 20, 30);
    altar(x.r.fake, 901, 10, 5, 330);
    x.r.fake.components[{ecs::PlayerState::kHash, 2}] = std::vector<uint8_t>(ecs::PlayerState::kSize, 0);
    x.r.fake.player_info(1, 2, 0xBB);
    place_entity(x.r.fake, 2, 110, 50, 30);  // above altar 900
    x.r.fake.sender = sender_of(2, 0xBB, true, false);
    const auto where = x.replies("chat_command.where");
    REQUIRE(where.size() == 2);
    CHECK(where[0] == "You are at 110, 50, 30");
    CHECK(where[1] == "asked by player 2 in the chat (you may run the commands for players: help lists them)");
    CHECK(x.reply("chat_command.home") == "The nearest flame altar (anyone's) is 0 m away, 30 m below you (2 in this world)");
    const std::string refused =
        "Only admins may list every flame altar: the list says where every base is, other players' too";
    CHECK(x.reply("chat_command.home all") == refused);
    x.r.fake.sender = EmfCommandSender{};  // no sender known (an older core): refused too (fails closed)
    CHECK(x.reply("chat_command.home all") == refused);
    x.r.fake.sender = sender_of(2, 0xBB, true, true);  // now listed in [chat] admins
    CHECK(x.replies("chat_command.where")[1] == "asked by player 2 in the chat (an admin: every command is yours)");
    const auto all = x.replies("chat_command.home all");
    REQUIRE(all.size() == 3);
    CHECK(all[0] == "2 flame altars, nearest first:");
    CHECK(all[1] == "0 m: entity 900 at 110, 20, 30");
    CHECK(all[2] == "316 m: entity 901 at 10, 5, 330");  // the distance on the ground (100 m across, 300 m along)
}

TEST_CASE("lua example chat_command: a long answer stays within the 10 lines the chat shows per command") {
    ExampleRig x("chat_command");
    for (uint32_t i = 0; i < 14; ++i) altar(x.r.fake, 900 + i, 10 + 10.0 * (i + 1), 20, 30);
    x.r.fake.sender = sender_of(0, 0, false, true);
    const auto all = x.replies("chat_command.home all");
    REQUIRE(all.size() == 10);
    CHECK(all[0] == "14 flame altars, nearest first:");
    CHECK(all[1] == "10 m: entity 900 at 20, 20, 30");
    CHECK(all[9] == "... and 6 more (a chat answer shows 10 lines)");
}

TEST_CASE("lua example chat_command: the altars are looked up at most every 5 s (a lookup walks every entity)") {
    ExampleRig x("chat_command");
    altar(x.r.fake, 900, 110, 20, 30);
    altar(x.r.fake, 901, 10, 5, 330);
    x.r.fake.sender = sender_of(0, 0, false, true);
    x.r.fake.tick = 1000;
    CHECK(x.reply("chat_command.home").find("(2 in this world)") != std::string::npos);
    altar(x.r.fake, 902, 10, 20, 40);  // a new base, 10 m away
    x.r.fake.tick = 1200;              // 3.3 s later: the list from before
    CHECK(x.reply("chat_command.home").find("(2 in this world)") != std::string::npos);
    x.r.fake.tick = 1300;  // 5 s later: looked up again
    CHECK(x.reply("chat_command.home") == "The nearest flame altar (anyone's) is 10 m away (3 in this world)");
    altar(x.r.fake, 903, 10, 20, 35);
    x.r.fake.session = 2;  // another world loaded: its ids are other entities, so the list is read again at once
    CHECK(x.reply("chat_command.home").find("(4 in this world)") != std::string::npos);
}

TEST_CASE("lua example chat_command: an unknown chat sender or a console without a player gets a clear answer") {
    ExampleRig x("chat_command");
    x.r.fake.sender = sender_of(0, 0xCC, true, false);  // EMF could not tell the player from the chat line
    CHECK(x.reply("chat_command.where") == "EMF could not tell which player you are");
    CHECK(x.reply("chat_command.home") == "EMF could not tell which player you are");
    x.r.api.local_player = nobody;  // a dedicated server's console (emf\commands.txt): no local player
    x.r.fake.sender = sender_of(0, 0, false, true);
    CHECK(x.reply("chat_command.where") == "The console has no player here: ask from the game's chat");
    x.r.api.local_player = one_player;
    CHECK(x.reply("chat_command.home") == "No flame altar in this world yet");
}

// hud_widget: a tidy corner widget
TEST_CASE("lua example hud_widget: where you are, a day/night bar and the time to dusk or dawn, in a corner") {
    ExampleRig x("hud_widget");
    daytime(x.r.fake, 13.5);
    x.tick(kServer);  // the widget is drawn by the client world (every player's own screen)
    CHECK(x.hud("01").empty());
    x.tick(kClient);
    CHECK(x.hud("01") == "X 10  Y 20  Z 30");
    CHECK(x.hud("02") == "Day   [#######.....] 13:30");
    CHECK(x.hud("03") == "Dusk in 4h 30m");
    CHECK(x.style("02") == std::make_pair(uint32_t(EMF_HUD_TOP_RIGHT), 0xE8E8E8FFu));
    daytime(x.r.fake, 22.0);
    x.tick(kClient);
    CHECK(x.hud("02") == "Night [####........] 22:00");
    CHECK(x.hud("03") == "Dawn in 8h 00m");
    daytime(x.r.fake, 5.75);
    x.tick(kClient);
    CHECK(x.hud("02") == "Night [###########.] 05:45");
    CHECK(x.hud("03") == "Dawn in 15m");
}

TEST_CASE("lua example hud_widget: it sets a line only when its text changes") {
    ExampleRig x("hud_widget");
    daytime(x.r.fake, 13.5);
    x.tick(kClient);
    const int first = g_hud_calls;
    CHECK(first == 3);
    for (int i = 0; i < 10; ++i) x.tick(kClient);
    CHECK(g_hud_calls == first);  // nothing changed: nothing sent
    place_entity(x.r.fake, 1, 12, 20, 30);
    x.tick(kClient);
    CHECK(g_hud_calls == first + 1);  // only the position line
    daytime(x.r.fake, 13.6);
    x.tick(kClient);
    CHECK(g_hud_calls == first + 3);  // the clock and the time to dusk
}

TEST_CASE("lua example hud_widget: the corner and colour come from its settings at once; the position line can go") {
    ExampleRig x("hud_widget");
    daytime(x.r.fake, 13.5);
    x.tick(kClient);
    x.set("corner", "bottom_left");
    x.set("color", "#00ff80");
    x.tick(kClient);
    x.tick(kClient);
    CHECK(x.style("01") == std::make_pair(uint32_t(EMF_HUD_BOTTOM_LEFT), 0x00FF80FFu));
    CHECK(x.style("03") == std::make_pair(uint32_t(EMF_HUD_BOTTOM_LEFT), 0x00FF80FFu));
    x.set("show_position", "0");
    x.tick(kClient);
    x.tick(kClient);
    CHECK(x.hud("01").empty());
    CHECK(x.hud("02") == "Day   [#######.....] 13:30");
}

TEST_CASE("lua example hud_widget: a world without a known time of day says so instead of failing") {
    ExampleRig x("hud_widget");
    x.tick(kClient);
    CHECK(x.host->status("hud_widget") == "running");
    CHECK(x.hud("02") == "Time of day unknown");
    CHECK(x.hud("03").empty());
}

// hotkey_wheel: a keybind opens a radial wheel
namespace {
std::string commands_txt(ExampleRig& x) {  // emf\commands.txt as the core reads it (Lua's text mode writes CR LF)
    std::string t = read_text(x.emf_dir + L"\\commands.txt");
    t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
    return t;
}
std::vector<std::string> g_console_ran;  // what the core's command center ran from emf\commands.txt
void console_ran(EmfWorld*, int argc, const char* const* argv, void*) {
    std::string line;
    for (int i = 0; i < argc; ++i) line += (i ? " " : "") + std::string(argv[i]);
    g_console_ran.push_back(line);
}
}  // namespace

TEST_CASE("lua example hotkey_wheel: the host's key opens the wheel; a pick runs EMF's time command through commands.txt") {
    ExampleRig x("hotkey_wheel");
    x.tick(kServer);  // this game hosts a world
    x.press(VK_F3, kServer);
    REQUIRE(x.r.fake.wheels.size() == 1);
    const auto& w = x.r.fake.wheels[0];
    CHECK(w.title == "Time of day");
    CHECK(w.items == std::vector<std::string>{"Dawn (06:00)", "Noon (12:00)", "Dusk (18:00)", "Midnight (00:00)"});
    CHECK(w.hold == static_cast<uint32_t>(VK_F3));  // held when it opens: releasing it picks
    w.cb(kClient, 0, w.user);                       // Dawn
    CHECK(commands_txt(x) == "time 6\n");
    CHECK(noted(x.r.fake, "Time of day: Dawn"));
    x.press(VK_F3, kServer);
    REQUIRE(x.r.fake.wheels.size() == 2);
    x.r.fake.wheels[1].cb(kClient, 3, x.r.fake.wheels[1].user);  // Midnight
    CHECK(commands_txt(x) == "time 6\ntime 0\n");
    x.press(VK_F3, kServer);
    x.r.fake.wheels[2].cb(kClient, -1, x.r.fake.wheels[2].user);  // Esc: nothing
    CHECK(commands_txt(x) == "time 6\ntime 0\n");
    // the core's own reader and command center take the file as written: the built-in "time" runs with each hour
    emf::core::CommandFile file(x.emf_dir + L"\\commands.txt", x.emf_dir + L"\\commands.done.txt");
    emf::core::CommandCenter center;
    std::string why;
    REQUIRE(center.add("time", "", console_ran, nullptr, &why));
    g_console_ran.clear();
    CHECK(file.poll(center) == 2);
    CHECK(center.run_pending(kServer, [](const std::string&) {}) == 2);
    CHECK(g_console_ran == std::vector<std::string>{"time 6", "time 0"});
}

TEST_CASE("lua example hotkey_wheel: emf.dir\\..\\..\\commands.txt is the file the core reads (<game>\\emf\\commands.txt)") {
    // The Lua host runs mods from <game>\emf\mods\<id> (emf.dir), and the core reads <game>\emf\commands.txt. If either
    // moves, the example (and its README) must change: this fails first.
    CHECK(read_all(EMF_SOURCE_DIR "/mods/lua/lua_mod.cpp").find(R"(+ L"\\emf\\mods")") != std::string::npos);
    CHECK(read_all(EMF_SOURCE_DIR "/src/core/core.cpp").find(R"(dir + L"\\emf\\commands.txt")") != std::string::npos);
    CHECK(read_all(EMF_SOURCE_DIR "/examples/lua/hotkey_wheel/main.lua").find(R"(emf.dir .. "\\..\\..\\commands.txt")") !=
          std::string::npos);
}

TEST_CASE("lua example hotkey_wheel: in someone else's game the key says only the host may; a new key works at once") {
    ExampleRig x("hotkey_wheel");
    x.press(VK_F3, kClient);  // no server world steps: a guest
    CHECK(x.r.fake.wheels.empty());
    CHECK(noted(x.r.fake, "Only the host can change the time of day"));
    x.set("wheel_key", "G");
    x.tick(kServer);
    x.press(VK_F3, kServer);
    CHECK(x.r.fake.wheels.empty());
    x.press('G', kServer);
    REQUIRE(x.r.fake.wheels.size() == 1);
    CHECK(x.r.fake.wheels[0].hold == static_cast<uint32_t>('G'));
}

// pet_follow: spawn, write a component, send an event
namespace {
void faction(Fake& f, uint32_t id, uint8_t value) { f.components[{ecs::Faction::kHash, id}] = std::vector<uint8_t>(1, value); }
uint8_t faction_now(Fake& f, uint32_t id) { return f.components[{ecs::Faction::kHash, id}][0]; }
void health(Fake& f, uint32_t id, int32_t value) {
    std::vector<uint8_t> n(sizeof(ecs::NetworkHealth), 0);
    reinterpret_cast<ecs::NetworkHealth*>(n.data())->health = value;
    f.components[{ecs::NetworkHealth::kHash, id}] = n;
}
// What the companion rabbit is right after it spawns: the template's faction (Player, 7) and full health.
void rabbit_body(Fake& f, uint32_t id) {
    faction(f, id, 7);
    health(f, id, 40);
}
void guarding(Fake& f, uint32_t id, uint32_t target) {
    std::vector<uint8_t> ai(sizeof(ecs::DynamicEnemy01BehaviorState), 0);
    std::memcpy(&ai[offsetof(ecs::DynamicEnemy01BehaviorState, guardEntityId)], &target, 4);
    f.components[{ecs::DynamicEnemy01BehaviorState::kHash, id}] = ai;
}
bool destroyed(Fake& f, uint32_t id) { return std::find(f.destroyed.begin(), f.destroyed.end(), id) != f.destroyed.end(); }
}  // namespace

TEST_CASE("lua example pet_follow: a rabbit 2 m from you, made an ally (Faction 11), sent to guard you") {
    ExampleRig x("pet_follow");
    rabbit_body(x.r.fake, 1234);
    CHECK(x.reply("pet_follow.summon") ==
          "A rabbit follows you (entity 1234). Spawned creatures are not saved: it is gone after a reload");
    REQUIRE(x.r.fake.spawns.size() == 1);
    const auto& s = x.r.fake.spawns[0];
    uint8_t guid[16];
    REQUIRE(emf::parse_guid("6b264270-bc49-447d-918a-89ffcb143a06", guid));  // Enemy_Companion_Bunny
    CHECK(std::memcmp(s.guid, guid, 16) == 0);
    CHECK(s.x == doctest::Approx(12.0));
    CHECK(s.y == doctest::Approx(20.0));
    CHECK(s.z == doctest::Approx(30.0));
    CHECK(faction_now(x.r.fake, 1234) == 11);  // PlayerAlly
    x.send_events();
    CHECK(x.guards(1234, 1) == 1);
    CHECK(x.reply("pet_follow.summon") == "You already have a pet (pet_follow.dismiss sends it away)");
    CHECK(x.r.fake.spawns.size() == 1);
    CHECK(x.reply("pet_follow.dismiss") == "Your pet is removed from the world");
    CHECK(x.r.fake.destroyed == std::vector<uint32_t>{1234});
    CHECK(x.reply("pet_follow.dismiss") == "You have no pet");
}

TEST_CASE("lua example pet_follow: each player gets their own pet; a new session forgets them without touching its ids") {
    ExampleRig x("pet_follow");
    rabbit_body(x.r.fake, 1234);
    rabbit_body(x.r.fake, 1235);
    x.r.fake.components[{ecs::PlayerState::kHash, 2}] = std::vector<uint8_t>(ecs::PlayerState::kSize, 0);
    place_entity(x.r.fake, 2, 50, 20, 30);
    x.r.fake.sender = sender_of(2, 0xBB, true, true);
    x.r.run_command("pet_follow.summon");
    REQUIRE(x.r.fake.spawns.size() == 1);
    CHECK(x.r.fake.spawns[0].x == doctest::Approx(52.0));
    x.send_events();
    CHECK(x.guards(1234, 2) == 1);
    x.r.fake.sender = sender_of(0, 0, false, true);  // the host at the console
    x.r.run_command("pet_follow.summon");
    CHECK(x.r.fake.spawns.size() == 2);
    x.r.fake.session = 2;  // the world reloaded: the pets were not saved, ids 1234 and 1235 are someone else's now
    CHECK(x.reply("pet_follow.dismiss") == "You have no pet");
    CHECK(x.r.fake.destroyed.empty());
}

TEST_CASE("lua example pet_follow: every 5 s it keeps its pet an ally that follows; a pet whose player left is removed") {
    ExampleRig x("pet_follow");
    rabbit_body(x.r.fake, 1234);
    x.r.run_command("pet_follow.summon");
    x.send_events();
    guarding(x.r.fake, 1234, 1);
    x.seconds(5);
    x.send_events();
    CHECK(x.guards(1234, 1) == 1);  // it guards you: nothing to do
    faction(x.r.fake, 1234, 7);     // the game set its faction back; it still guards you, so it is ours
    x.seconds(5);
    CHECK(faction_now(x.r.fake, 1234) == 11);
    guarding(x.r.fake, 1234, 0);  // still an ally, but it lost you (a fight, a long run)
    x.seconds(5);
    x.send_events();
    CHECK(x.guards(1234, 1) == 2);
    x.r.fake.components.erase({ecs::CurrentTransform::kHash, 1});  // you left the game
    x.seconds(5);
    CHECK(destroyed(x.r.fake, 1234));
    CHECK(logged("[pet_follow] player 1 left: their pet is removed"));
}

TEST_CASE("lua example pet_follow: a pet that died is forgotten; its id, reused by something else, is never touched") {
    ExampleRig x("pet_follow");
    rabbit_body(x.r.fake, 1234);
    x.r.run_command("pet_follow.summon");
    x.send_events();
    // it dies the way the game does it: health 0, EntityDiedEvent the next step; the corpse keeps its place
    health(x.r.fake, 1234, 0);
    send(x.r.fake, "EntityDiedEvent", died(1234));
    CHECK(logged("[pet_follow] the pet of player 1 died"));
    // later in the same session the corpse is gone and id 1234 is a villager: faction 2, alive, no guard of yours
    faction(x.r.fake, 1234, 2);
    health(x.r.fake, 1234, 100);
    guarding(x.r.fake, 1234, 0);
    x.seconds(10);
    x.send_events();
    CHECK(faction_now(x.r.fake, 1234) == 2);  // not made an ally
    CHECK(x.guards(1234, 1) == 1);            // no second guard order
    CHECK(x.reply("pet_follow.dismiss") == "You have no pet");
    CHECK_FALSE(destroyed(x.r.fake, 1234));
}

TEST_CASE("lua example pet_follow: without the death event, dead (health 0) or not its own (faction, guard) means gone") {
    {
        ExampleRig x("pet_follow");  // a corpse: health 0, the transform kept, the event missed (the mod reloaded)
        rabbit_body(x.r.fake, 1234);
        x.r.run_command("pet_follow.summon");
        health(x.r.fake, 1234, 0);
        CHECK(x.reply("pet_follow.dismiss") == "You have no pet");
        CHECK_FALSE(destroyed(x.r.fake, 1234));
    }
    {
        ExampleRig x("pet_follow");  // the id now holds a chest: no health at all
        rabbit_body(x.r.fake, 1234);
        x.r.run_command("pet_follow.summon");
        x.r.fake.components.erase({ecs::NetworkHealth::kHash, 1234});
        x.seconds(5);
        CHECK(logged("[pet_follow] the pet of player 1 is gone"));
        CHECK(x.reply("pet_follow.dismiss") == "You have no pet");
        CHECK_FALSE(destroyed(x.r.fake, 1234));
    }
    {
        ExampleRig x("pet_follow");  // alive, but neither our ally nor guarding us: another creature now
        rabbit_body(x.r.fake, 1234);
        x.r.run_command("pet_follow.summon");
        faction(x.r.fake, 1234, 4);
        guarding(x.r.fake, 1234, 77);
        x.seconds(5);
        x.send_events();
        CHECK(faction_now(x.r.fake, 1234) == 4);
        CHECK(x.guards(1234, 1) == 1);
        CHECK(x.reply("pet_follow.dismiss") == "You have no pet");
        CHECK_FALSE(destroyed(x.r.fake, 1234));
    }
}

// loot_ticker: PickupEvent, the last pickups on screen, fading
TEST_CASE("lua example loot_ticker: your last 5 pickups on screen, newest first; the same item again adds up") {
    ExampleRig x("loot_ticker");
    x.r.fake.item_names[kLogs] = "Material_T1_Wood_Logs";
    x.r.fake.item_names[kBait] = "Material_T0_simple_FishingBait";
    x.r.fake.item_names[kFibre] = "Material_T1_Plant_Fibre";
    x.tick(kClient);
    send(x.r.fake, "PickupEvent", pickup(1, 0, kLogs, 30));
    x.tick(kClient);
    CHECK(x.hud("01") == "+30 Wood Logs");
    send(x.r.fake, "PickupEvent", pickup(1, 0, kLogs, 20));  // more of the same right after: one line
    send(x.r.fake, "PickupEvent", pickup(2, 0, kFibre, 5));  // another player's: not yours
    x.tick(kClient);
    CHECK(x.hud("01") == "+50 Wood Logs");
    CHECK(x.hud("02").empty());
    send(x.r.fake, "PickupEvent", pickup(1, 0, kBait, 2));
    x.tick(kClient);
    CHECK(x.hud("01") == "+2 simple Fishing Bait");
    CHECK(x.hud("02") == "+50 Wood Logs");
    for (uint32_t n = 1; n <= 5; ++n) send(x.r.fake, "PickupEvent", pickup(1, 0, n % 2 ? kFibre : kLogs, n));
    x.tick(kClient);
    CHECK(x.hud("01") == "+5 Plant Fibre");
    CHECK(x.hud("05") == "+1 Plant Fibre");
    CHECK(x.hud("06").empty());
    CHECK(x.style("01") == std::make_pair(uint32_t(EMF_HUD_BOTTOM_RIGHT), 0xFFFFFFFFu));
}

TEST_CASE("lua example loot_ticker: a line fades out over its last 2 seconds and then goes") {
    ExampleRig x("loot_ticker");
    x.r.fake.item_names[kLogs] = "Material_T1_Wood_Logs";
    x.tick(kClient);
    send(x.r.fake, "PickupEvent", pickup(1, 0, kLogs, 30));
    x.seconds(5, kClient);
    CHECK(x.style("01").second == 0xFFFFFFFFu);  // fully there
    x.seconds(2, kClient);                       // 7 of 8 s: half faded
    const uint32_t alpha = x.style("01").second & 0xFF;
    CHECK(alpha < 0xC0);
    CHECK(alpha > 0x40);
    CHECK(x.hud("01") == "+30 Wood Logs");
    x.seconds(2, kClient);
    CHECK(x.hud("01").empty());
    x.set("seconds", "20");  // a longer life from the settings
    x.tick(kClient);
    send(x.r.fake, "PickupEvent", pickup(1, 0, kLogs, 1));
    x.seconds(10, kClient);
    CHECK(x.hud("01") == "+1 Wood Logs");
}

TEST_CASE("lua example loot_ticker: a black line fades to black, never to the usual colour (rgba 0 means the default)") {
    ExampleRig x("loot_ticker");
    x.r.fake.item_names[kLogs] = "Material_T1_Wood_Logs";
    x.set("color", "#000000");
    x.tick(kClient);
    send(x.r.fake, "PickupEvent", pickup(1, 0, kLogs, 30));
    bool zero = false;
    for (int i = 0; i < 9 * 60; ++i) {
        x.tick(kClient);
        if (!x.hud("01").empty()) zero |= x.style("01").second == 0;
    }
    CHECK_FALSE(zero);
    CHECK(x.hud("01").empty());
}
