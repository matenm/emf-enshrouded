// Quick Stack / Chest Machines' game side (mods/quick_stack/qs_glue.h) over a fake API: what counts as a chest, what a
// player carries (the action bar and the filters kept), a chest's stacks and free slots, and the game's recipes by
// what they make (the RecipeRegistry's recipe map at +0x48).
#include <doctest.h>

#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <map>
#include <string>
#include <vector>

#include "../mods/quick_stack/qs_glue.h"
#include "core/manifest.h"
#include "core/settings.h"
#include "emf/keys.hpp"

namespace gen = emf::gen::ecs;
using namespace quickstack;

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
// An 8-slot inventory entity with stacks {item, count, pide}.
void inventory(uint32_t id, std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> stacks) {
    auto& inv = plant<gen::Inventory>(id);
    for (size_t i = 0; i < stacks.size(); ++i) {
        inv.slots[i].id.value = std::get<0>(stacks[i]);
        inv.slots[i].data.count = std::get<1>(stacks[i]);
        inv.slots[i].data.pide.id = std::get<2>(stacks[i]);
    }
}
template <class T>
void put(std::vector<uint8_t>& b, size_t off, T v) { std::memcpy(b.data() + off, &v, sizeof v); }
// A RecipeInfo blob (0xA8 bytes plus its input and output arrays behind it).
std::vector<uint8_t> recipe_blob(uint32_t id, std::vector<std::pair<uint32_t, uint32_t>> in,
                                 std::vector<std::pair<uint32_t, uint32_t>> out, bool cooking = false, bool upgrade = false) {
    std::vector<uint8_t> b(0xA8 + in.size() * 48 + out.size() * 24, 0);
    put<uint32_t>(b, 0x10, id);
    const size_t in_at = 0xA8, out_at = 0xA8 + in.size() * 48;
    put<int32_t>(b, 0x28, static_cast<int32_t>(in_at - 0x28));
    put<uint32_t>(b, 0x2C, static_cast<uint32_t>(in.size()));
    put<int32_t>(b, 0x30, static_cast<int32_t>(out_at - 0x30));
    put<uint32_t>(b, 0x34, static_cast<uint32_t>(out.size()));
    for (size_t i = 0; i < in.size(); ++i) {
        put<uint32_t>(b, in_at + i * 48 + 0x10, in[i].first);
        put<uint32_t>(b, in_at + i * 48 + 0x14, in[i].second);
    }
    for (size_t i = 0; i < out.size(); ++i) {
        put<uint32_t>(b, out_at + i * 24 + 0x10, out[i].first);
        put<uint32_t>(b, out_at + i * 24 + 0x14, out[i].second);
    }
    b[0x9f] = cooking ? 1 : 0;
    b[0xa0] = upgrade ? 1 : 0;
    return b;
}
}  // namespace

TEST_CASE("quick stack glue: a chest is an initialized storage that is no player, animal or villager") {
    Rig r;
    emf::World w(kW);
    plant<gen::InventorySetup>(500).isInitialized = true;
    CHECK(is_chest(w, 500));
    plant<gen::InventorySetup>(501);  // not initialized yet
    CHECK_FALSE(is_chest(w, 501));
    plant<gen::InventorySetup>(1).isInitialized = true;
    plant<gen::PlayerState>(1);
    CHECK_FALSE(is_chest(w, 1));
    plant<gen::InventorySetup>(502).isInitialized = true;
    plant<gen::AnimalSaved>(502);
    CHECK_FALSE(is_chest(w, 502));
    plant<gen::InventorySetup>(503).isInitialized = true;
    g_tpl[{kActiveNpcStateTemplateHash, 503}] = 0x1234;  // a villager's template data
    CHECK_FALSE(is_chest(w, 503));
    CHECK_FALSE(is_chest(w, 999));
    // a tombstone has an initialized InventorySetup too (seen in game: entity 1039), but it vanishes when emptied or
    // when its time runs out: never a chest that receives items
    for (int which = 0; which < 3; ++which) {
        const uint32_t id = 510 + which;
        plant<gen::InventorySetup>(id).isInitialized = true;
        if (which == 0) plant<gen::DestroyOnLoot>(id);
        if (which == 1) plant<gen::LifeTime>(id);
        if (which == 2) plant<gen::CreateItemLoot>(id);
        CHECK_FALSE(is_chest(w, id));
    }
}

TEST_CASE("quick stack glue: what a player carries: backpack and action bar (kept), gear marked, filters by name") {
    Rig r;
    emf::World w(kW);
    auto& setup = plant<gen::InventorySetup>(1);
    setup.linksEntities[0].id = 70;  // backpack
    setup.linksCategories[0] = emf::gen::InventoryCategory::Generic;
    setup.linksEntities[1].id = 71;  // action bar
    setup.linksCategories[1] = emf::gen::InventoryCategory::Generic;
    setup.linksEntities[2].id = 72;  // equipment: never
    setup.linksCategories[2] = emf::gen::InventoryCategory::Equipment;
    plant<gen::SlotSelection>(1).actionbarEntityIds[0].id = 71;
    inventory(70, {{0x11, 30, 0}, {0x22, 4, 0}, {0x33, 1, 9001}, {0x44, 12, 0}});
    inventory(71, {{0x11, 5, 0}});
    inventory(72, {{0x55, 1, 9002}});
    const auto names = [](uint32_t item) -> std::string {
        return item == 0x22 ? "Ammo_Arrow_Wood" : item == 0x44 ? "Material_T1_Resin" : "Material_T1_Wood_Logs";
    };
    Filters f;
    f.keep_ammo = true;
    f.exclude = {"resin"};
    auto c = carried(w, 1, true, f, names);
    REQUIRE(c.size() == 5);
    CHECK(c[0].item == 0x11);
    CHECK(c[0].slot == emf::slot_id(70, 0));
    CHECK_FALSE(c[0].kept);
    CHECK(c[1].kept);   // arrows: keep_ammo
    CHECK(c[2].gear);   // a sword with its own data
    CHECK(c[3].kept);   // resin: excluded by name
    CHECK(c[4].kept);   // the action bar
    c = carried(w, 1, false, Filters{}, names);
    CHECK_FALSE(c[4].kept);
    CHECK_FALSE(c[1].kept);
    CHECK(carried(w, 2, true, f, names).empty());  // no InventorySetup
}

TEST_CASE("quick stack glue: a chest's stacks (own and linked inventories) and its free generic slots") {
    Rig r;
    emf::World w(kW);
    auto& setup = plant<gen::InventorySetup>(500);
    setup.isInitialized = true;
    setup.linksEntities[0].id = 80;
    setup.linksCategories[0] = emf::gen::InventoryCategory::Generic;
    setup.linksEntities[1].id = 81;
    setup.linksCategories[1] = emf::gen::InventoryCategory::Generic;
    inventory(80, {{0x11, 50, 0}, {0x33, 1, 9001}});
    inventory(81, {{0x22, 20, 0}});
    const auto s = chest_stacks(w, 500);
    REQUIRE(s.size() == 3);
    CHECK(s[0].slot == emf::slot_id(80, 0));
    CHECK(s[1].gear);
    CHECK(s[2].item == 0x22);
    CHECK(free_slots(w, 500) == 13u);  // 16 slots, 3 used
    CHECK(free_slots(w, 999) == 0u);
}

TEST_CASE("quick stack glue: the game's recipes indexed by what they make (RecipeRegistry +0x48: recipe id -> RecipeInfo*)") {
    auto wall = recipe_blob(0xA1, {{0x11, 4}, {0x12, 1}}, {{0x500, 1}});
    auto stew = recipe_blob(0xA4, {{0x13, 2}}, {{0x600, 1}}, true);
    auto up = recipe_blob(0xA9, {{0x14, 3}}, {{0x500, 1}}, false, true);
    std::vector<const void*> values(8, nullptr);
    uint32_t keys[8] = {};
    uint64_t bits = 0;
    for (auto [slot, rec, id] : {std::tuple<size_t, const void*, uint32_t>{1, wall.data(), 0xA1},
                                 {4, stew.data(), 0xA4}, {6, up.data(), 0xA9}}) {
        values[slot] = rec;
        keys[slot] = id;
        bits |= 1ull << slot;
    }
    std::vector<uint8_t> reg(0x150, 0);
    put<const void*>(reg, 0x48 + 0x08, &bits);
    put<uint64_t>(reg, 0x48 + 0x10, 8);
    put<const void*>(reg, 0x48 + 0x28, keys);
    put<const void*>(reg, 0x48 + 0x40, values.data());
    const auto by = recipes_by_output(reg.data(), plain_copy);
    REQUIRE(by.count(0x500));
    REQUIRE(by.at(0x500).size() == 2);
    CHECK(by.at(0x500)[0].id == 0xA1);
    REQUIRE(by.at(0x500)[0].inputs.size() == 2);
    CHECK(by.at(0x500)[0].inputs[0].item == 0x11);
    CHECK(by.at(0x500)[0].inputs[0].count == 4u);
    CHECK(by.at(0x500)[1].upgrade);
    REQUIRE(by.count(0x600));
    CHECK(by.at(0x600)[0].cooking);
    CHECK(recipes_by_output(nullptr, plain_copy).empty());
    // with these the recycler gives back half a wall's wood and leaves the stew
    CHECK(recycle_plan({1, 0x500, 2}, by.at(0x500), 50).refund[0].count == 4u);
    CHECK(recycle_plan({1, 0x600, 2}, by.at(0x600), 50).skip == Skip::Cooking);
}

namespace {
struct OldIni : emf::core::IniBackend {
    std::map<std::string, std::string> values;  // "mod.key" -> text, as quick_stack 1.1.0 left them
    std::optional<std::string> read(const std::string& mod, const std::string& key) override {
        auto it = values.find(mod + "." + key);
        if (it == values.end()) return std::nullopt;
        return it->second;
    }
    bool write(const std::string& mod, const std::string& key, const std::string& value) override {
        values[mod + "." + key] = value;
        return true;
    }
};
}  // namespace

TEST_CASE("quick stack 1.2: the settings of 1.1.0 (its ini file) keep their values under the new kinds") {
    std::ifstream f(EMF_SOURCE_DIR "/mods/quick_stack/mod.json", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const auto m = emf::core::parse_manifest(ss.str());
    REQUIRE_MESSAGE(m.manifest, m.error);
    OldIni ini;
    ini.values = {{"quick_stack.key", "0x72"}, {"quick_stack.radius", "25"}, {"quick_stack.keep_action_bar", "0"},
                  {"quick_stack.enabled", "1"}};
    emf::core::SettingsStore store(&ini);
    for (const auto& d : m.manifest->settings) {
        std::string why;
        CHECK_MESSAGE(store.declare("quick_stack", d, &why), d.key, why);
    }
    CHECK(emf::keys::parse_key(std::wstring(L"0x72")) == emf::keys::parse_key(L"F3"));
    const std::string key = store.get("quick_stack", "key");
    CHECK(emf::keys::parse_key(std::wstring(key.begin(), key.end())) == 0x72u);
    CHECK(store.get_int("quick_stack", "radius", -1) == 25);
    CHECK(store.get_int("quick_stack", "keep_action_bar", -1) == 0);
    // and with no ini at all the defaults are 1.1.0's
    OldIni empty;
    emf::core::SettingsStore fresh(&empty);
    for (const auto& d : m.manifest->settings) fresh.declare("quick_stack", d);
    CHECK(fresh.get("quick_stack", "key") == "L");
    CHECK(fresh.get_int("quick_stack", "radius", -1) == 10);
    CHECK(fresh.get_int("quick_stack", "keep_action_bar", -1) == 1);
    CHECK(fresh.get_int("quick_stack", "recycler", -1) == 0);  // the recycler starts off
    CHECK(m.manifest->version_text == "1.2.0");
}

TEST_CASE("manifest: Chest Machines, Sleep Through the Day, Bounty Board and Grave Keeper parse with every setting clean; only Chest Machines is on by default") {
    struct Want {
        const char* id;
        const char* dll;  // "" for a Lua mod
        uint32_t emf;
        bool on;
    };
    for (const Want& m : {Want{"quick_stack", "quick_stack.dll", 17, true}, Want{"sleep", "sleep.dll", 17, false},
                          Want{"bounty", "", 14, false}, Want{"grave", "", 14, false}}) {
        std::ifstream f(std::string(EMF_SOURCE_DIR) + "/mods/" + m.id + "/mod.json", std::ios::binary);
        REQUIRE_MESSAGE(f.good(), m.id);
        std::stringstream ss;
        ss << f.rdbuf();
        auto r = emf::core::parse_manifest(ss.str());
        const std::string why = std::string(m.id) + ": " + r.error;
        REQUIRE_MESSAGE(r.manifest, why);
        CHECK(r.manifest->dll == m.dll);
        CHECK(r.manifest->emf == m.emf);
        CHECK(r.manifest->enabled_default == m.on);
        CHECK(ss.str().find("\"side\": \"server\"") != std::string::npos);
        for (const auto& s : r.manifest->settings) {
            CAPTURE(m.id);
            CAPTURE(s.key);
            CHECK(s.problem.empty());
            CHECK(s.warning.empty());
        }
    }
}

TEST_CASE("quick stack glue: free slots stop at the chest's available slots; an item's room counts free slots and part-filled stacks") {
    Rig r;
    emf::World w(kW);
    auto& setup = plant<gen::InventorySetup>(500);
    setup.isInitialized = true;
    for (int i = 0; i < 3; ++i) {
        setup.linksEntities[i].id = 80 + i;
        setup.linksCategories[i] = emf::gen::InventoryCategory::Generic;
    }
    setup.availableSlotCount = 20;  // 3 inventories of 8 slots, of which 20 can be used
    inventory(80, {{0x11, 200, 0}, {0x11, 250, 0}, {0x22, 9, 0}});
    inventory(81, {});
    inventory(82, {});
    CHECK(free_slots(w, 500) == 17u);  // 20 usable - 3 used (not 21)
    CHECK(room_for(w, 500, 0x11, 250) == 17u * 250u + 50u);
    CHECK(room_for(w, 500, 0x33, 100) == 17u * 100u);
    // the biggest plain stack of the item is reported: one above the size told means the size changed (Big Stacks)
    uint32_t biggest = 0;
    CHECK(room_for(w, 500, 0x11, 100, &biggest) == 17u * 100u);
    CHECK(biggest == 250u);
    CHECK(room_for(w, 500, 0x33, 100, &biggest) == 17u * 100u);
    CHECK(biggest == 0u);
    // a chest without the count (0) uses its slots as they are
    setup.availableSlotCount = 0;
    CHECK(free_slots(w, 500) == 21u);
}

