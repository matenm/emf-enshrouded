// Bulk Craft's game side (mods/bulk_craft/bc_glue.h and bc_recipes.h) over a fake API: which entities are stations it
// may use, the one under the crosshair or the nearest, the recipe registry's maps (by id at +0x48, by workshop at
// +0xF8), the candidates a player may bulk-craft, and what a player carries.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <map>
#include <string>
#include <vector>

#include "../mods/bulk_craft/bc_glue.h"
#include "core/manifest.h"

namespace gen = emf::gen::ecs;
using namespace bulkcraft;

namespace {
EmfWorld* const kW = reinterpret_cast<EmfWorld*>(0x5100);
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> g_comps;  // (hash, entity) -> bytes
std::vector<uint8_t>* g_registry = nullptr;
std::vector<uint8_t> g_infos;  // BasicPlayerInfos: 0xA0-byte entries, entity id at +0, account at +8
bool g_events_ok = true;
int g_events_asked = 0;
bool g_from_chat = false;
bool f_on_event(uint8_t, const char*, uint32_t, EmfWorldEventCallback, void*) {
    ++g_events_asked;
    return g_events_ok;
}
void f_log(const char*, const char*, ...) {}
bool f_sender(EmfCommandSender* out) {
    out->from_chat = g_from_chat ? 1 : 0;
    out->entity = g_from_chat ? 2 : 0;
    return true;
}
void* f_get(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size) {
    auto it = g_comps.find({hash, entity});
    if (it == g_comps.end()) return nullptr;
    if (size) *size = it->second.size();
    return it->second.data();
}
void f_each(EmfWorld*, EmfEntityCallback cb, void* user) {
    std::vector<uint32_t> ids;
    for (const auto& [k, v] : g_comps)
        if (ids.empty() || ids.back() != k.second) ids.push_back(k.second);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (uint32_t id : ids) cb(id, user);
}
void* f_singleton(EmfWorld*, const char* name) {
    if (std::strcmp(name, "BasicPlayerInfos") == 0) return g_infos.empty() ? nullptr : g_infos.data();
    return g_registry && std::strcmp(name, "g38_items::RecipeRegistry") == 0 ? g_registry->data() : nullptr;
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
        g_registry = nullptr;
        g_infos.assign(16 * 0xA0, 0);
        g_events_ok = true;
        g_events_asked = 0;
        g_from_chat = false;
        api.on_event = f_on_event;
        api.command_sender = f_sender;
        api.log = f_log;
        api.version = EMF_API_VERSION;
        api.size = sizeof api;
        api.get_component = f_get;
        api.for_each_entity = f_each;
        api.singleton = f_singleton;
        api.world_kind = f_kind;
        emf::api_ref() = &api;
    }
    ~Rig() {
        emf::api_ref() = nullptr;
        g_registry = nullptr;
    }
};
int64_t fx(double m) { return static_cast<int64_t>(std::llround(m * 4294967296.0)); }
void at(uint32_t id, double x, double y, double z) {
    auto& t = plant<gen::CurrentTransform>(id);
    t.transform.position.x = fx(x);
    t.transform.position.y = fx(y);
    t.transform.position.z = fx(z);
}
void station(uint32_t id, uint32_t workshop, double x) {
    plant<gen::Crafting>(id).workshopId.value = workshop;
    at(id, x, 0, 0);
}
// The player's camera at its feet + 1.6 m, looking along +x turned by `yaw_deg` around the up axis.
void camera(uint32_t player, double x, double yaw_deg) {
    auto& in = plant<gen::PlayerInput>(player);
    in.fromClient.camera.position.x = fx(x);
    in.fromClient.camera.position.y = fx(1.6);
    // +z turned to +x is 90 degrees around +y; then yaw more
    const double a = (90.0 + yaw_deg) * 3.14159265358979 / 180.0;
    in.fromClient.camera.orientation.x = 0;
    in.fromClient.camera.orientation.y = static_cast<float>(std::sin(a / 2));
    in.fromClient.camera.orientation.z = 0;
    in.fromClient.camera.orientation.w = static_cast<float>(std::cos(a / 2));
}

template <class T>
void put(std::vector<uint8_t>& b, size_t off, T v) { std::memcpy(b.data() + off, &v, sizeof v); }
// A RecipeInfo blob (0xA8 bytes plus its input and output arrays behind it).
std::vector<uint8_t> recipe_blob(uint32_t id, uint32_t workshop, std::vector<std::pair<uint32_t, uint32_t>> in,
                                 std::vector<std::pair<uint32_t, uint32_t>> out, int64_t duration_ns = 0,
                                 bool upgrade = false) {
    std::vector<uint8_t> b(0xA8 + in.size() * 48 + out.size() * 24, 0);
    put<uint32_t>(b, 0x10, id);
    put<uint32_t>(b, 0x24, workshop);
    const size_t in_at = 0xA8, out_at = 0xA8 + in.size() * 48;
    put<int32_t>(b, 0x28, static_cast<int32_t>(in_at - 0x28));
    put<uint32_t>(b, 0x2C, static_cast<uint32_t>(in.size()));
    put<int32_t>(b, 0x30, static_cast<int32_t>(out_at - 0x30));
    put<uint32_t>(b, 0x34, static_cast<uint32_t>(out.size()));
    put<int64_t>(b, 0x38, duration_ns);
    put<uint32_t>(b, 0x80, 2);   // level
    put<uint32_t>(b, 0x84, 3);   // comfort
    put<uint32_t>(b, 0x50, 0xe75245d1);  // knowledge query id
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i].first == 0) {  // a category input: {0, category id}, count 1
            put<uint32_t>(b, in_at + i * 48 + 0x28, in[i].second);
            put<uint32_t>(b, in_at + i * 48 + 0x2C, 1);
            continue;
        }
        put<uint32_t>(b, in_at + i * 48 + 0x10, in[i].first);
        put<uint32_t>(b, in_at + i * 48 + 0x14, in[i].second);
    }
    for (size_t i = 0; i < out.size(); ++i) {
        put<uint32_t>(b, out_at + i * 24 + 0x10, out[i].first);
        put<uint32_t>(b, out_at + i * 24 + 0x14, out[i].second);
    }
    b[0xa0] = upgrade ? 1 : 0;
    return b;
}

// A fake g38_items::RecipeRegistry: the +0x48 map (id -> RecipeInfo*) and the +0xF8 map (workshop -> list).
struct FakeRegistry {
    std::vector<uint8_t> reg = std::vector<uint8_t>(0x150, 0);
    std::deque<std::vector<uint8_t>> blobs;  // stable addresses
    static constexpr uint64_t kCap = 16;
    std::vector<uint64_t> bits48 = std::vector<uint64_t>(1, 0), bitsF8 = std::vector<uint64_t>(1, 0);
    std::vector<uint32_t> keys48 = std::vector<uint32_t>(kCap, 0), keysF8 = std::vector<uint32_t>(kCap, 0);
    std::vector<const uint8_t*> values48 = std::vector<const uint8_t*>(kCap, nullptr);
    std::vector<uint8_t> valuesF8 = std::vector<uint8_t>(kCap * 0x1008, 0);
    std::vector<uint64_t> bitsA0 = std::vector<uint64_t>(1, 0);
    std::vector<uint32_t> keysA0 = std::vector<uint32_t>(kCap, 0);
    std::vector<const uint8_t*> valuesA0 = std::vector<const uint8_t*>(kCap, nullptr);
    // RecipeInputCategory: id +0, items BlobArray at +4 (self-relative offset, count) of {itemId, scaling float}
    void add_category(uint32_t id, std::vector<std::pair<uint32_t, float>> items) {
        std::vector<uint8_t> b(12 + items.size() * 8, 0);
        put<uint32_t>(b, 0, id);
        put<int32_t>(b, 4, 8);  // the items start at +12 = (+4) + 8
        put<uint32_t>(b, 8, static_cast<uint32_t>(items.size()));
        for (size_t i = 0; i < items.size(); ++i) {
            put<uint32_t>(b, 12 + i * 8, items[i].first);
            put<float>(b, 16 + i * 8, items[i].second);
        }
        blobs.push_back(std::move(b));
        uint64_t s = id & (kCap - 1);
        while ((bitsA0[0] >> s) & 1) s = (s + 1) & (kCap - 1);
        bitsA0[0] |= 1ull << s;
        keysA0[s] = id;
        valuesA0[s] = blobs.back().data();
    }
    void add(std::vector<uint8_t> blob) {
        blobs.push_back(std::move(blob));
        const uint8_t* p = blobs.back().data();
        uint32_t id, ws;
        std::memcpy(&id, p + 0x10, 4);
        std::memcpy(&ws, p + 0x24, 4);
        uint64_t s = id & (kCap - 1);
        while ((bits48[0] >> s) & 1) s = (s + 1) & (kCap - 1);
        bits48[0] |= 1ull << s;
        keys48[s] = id;
        values48[s] = p;
        s = ws & (kCap - 1);
        while (((bitsF8[0] >> s) & 1) && keysF8[s] != ws) s = (s + 1) & (kCap - 1);
        bitsF8[0] |= 1ull << s;
        keysF8[s] = ws;
        uint8_t* list = valuesF8.data() + s * 0x1008;
        uint16_t n;
        std::memcpy(&n, list + 0x1000, 2);
        std::memcpy(list + n * 8, &p, 8);
        ++n;
        std::memcpy(list + 0x1000, &n, 2);
    }
    void wire() {
        auto map = [&](size_t at, void* bits, void* keys, void* values) {
            put<void*>(reg, at + 0x08, bits);
            put<uint64_t>(reg, at + 0x10, kCap);
            put<void*>(reg, at + 0x28, keys);
            put<void*>(reg, at + 0x40, values);
        };
        map(0x48, bits48.data(), keys48.data(), values48.data());
        map(0xF8, bitsF8.data(), keysF8.data(), valuesF8.data());
        map(0xA0, bitsA0.data(), keysA0.data(), valuesA0.data());
        g_registry = &reg;
    }
};
constexpr uint32_t kBench = 0x4da88f73, kTwigs = 0x11, kStone = 0x13, kArrows = 0x21, kBlocks = 0x23;
}  // namespace

TEST_CASE("bulk_craft glue: a station is a crafting entity that is no player, factory, altar or hand crafter") {
    Rig r;
    emf::World w(kW);
    station(500, kBench, 2);
    CHECK(is_station(w, 500));
    station(1, 0, 0);
    plant<gen::PlayerState>(1);
    CHECK_FALSE(is_station(w, 1));
    station(501, 0, 3);  // a villager with workshop 0: hand crafting
    CHECK_FALSE(is_station(w, 501));
    station(502, 0x2546ae49, 3);
    plant<gen::FactoryStation>(502);
    CHECK_FALSE(is_station(w, 502));
    station(503, 0xb326e042, 3);  // the flame altar
    CHECK_FALSE(is_station(w, 503));
    CHECK_FALSE(is_station(w, 999));
    CHECK(is_player(w, 1));
    CHECK_FALSE(is_player(w, 500));
}

TEST_CASE("bulk_craft glue: the station under the crosshair, else the nearest within reach, else none") {
    Rig r;
    emf::World w(kW);
    plant<gen::PlayerState>(1);
    at(1, 0, 0, 0);
    station(500, kBench, 2);             // 2 m along +x
    station(501, 0x99, 0);               // the second one stands 4 m along +z
    at(501, 0, 0, 4);
    station(502, kBench, 9);             // out of reach
    auto st = station_for(w, 1, 5.0, true);
    REQUIRE(st);
    CHECK(st->id == 500);                // no camera: the nearest
    CHECK(st->workshop == kBench);
    camera(1, 0, -90);                   // looking along +z: at 501
    st = station_for(w, 1, 5.0, true);
    REQUIRE(st);
    CHECK(st->id == 501);
    CHECK(st->workshop == 0x99);
    st = station_for(w, 1, 5.0, false);  // the crosshair not asked: the nearest
    REQUIRE(st);
    CHECK(st->id == 500);
    camera(1, 0, 180);                   // looking along -x at nothing: the nearest
    st = station_for(w, 1, 5.0, true);
    REQUIRE(st);
    CHECK(st->id == 500);
    CHECK_FALSE(station_for(w, 1, 1.0, true));
    CHECK_FALSE(station_for(w, 42, 5.0, true));  // a player without a position
}

TEST_CASE("bulk_craft glue: recipes from the registry, by id and by workshop, with every field") {
    Rig r;
    emf::World w(kW);
    FakeRegistry reg;
    reg.add(recipe_blob(0x278240d5, 0, {{kTwigs, 2}}, {{kArrows, 10}}));
    reg.add(recipe_blob(0x852c5432, kBench, {{kStone, 2}}, {{kBlocks, 100}}, 3000000000ll));
    reg.add(recipe_blob(0x0000aa01, kBench, {{0, 0}, {kStone, 1}}, {{kBlocks, 1}}, 0, true));
    reg.wire();
    const auto a = recipe_by_id(w, 0x852c5432);
    REQUIRE(a);
    CHECK(a->workshop == kBench);
    CHECK(a->duration_ns == 3000000000ll);
    CHECK(a->level == 2);
    CHECK(a->comfort == 3);
    CHECK(a->knowledge_id == 0xe75245d1);
    REQUIRE(a->inputs.size() == 1);
    CHECK(a->inputs[0].item == kStone);
    CHECK(a->outputs[0].count == 100);
    CHECK_FALSE(recipe_by_id(w, 0x12345678));
    CHECK(station_recipes(w, kBench).size() == 2);
    CHECK(station_recipes(w, 0).size() == 1);
    CHECK(station_recipes(w, 0x77).empty());
    const auto up = recipe_by_id(w, 0x0000aa01);
    REQUIRE(up);
    CHECK(up->upgrade);
    CHECK(up->category_input);
}

TEST_CASE("bulk_craft glue: no registry yet (or a bad one): no recipes, no crash") {
    Rig r;
    emf::World w(kW);
    CHECK_FALSE(recipe_by_id(w, 0x278240d5));
    CHECK(station_recipes(w, 0).empty());
    std::vector<uint8_t> bad(0x150, 0);
    put<uint64_t>(bad, 0x48 + 0x10, 12);  // not a power of two
    g_registry = &bad;
    CHECK_FALSE(recipe_by_id(w, 0x278240d5));
    CHECK(station_recipes(w, 0).empty());
}

TEST_CASE("bulk_craft glue: candidates: the station's learned recipes first, then learned hand recipes") {
    Rig r;
    emf::World w(kW);
    FakeRegistry reg;
    reg.add(recipe_blob(0x278240d5, 0, {{kTwigs, 2}}, {{kArrows, 10}}));
    reg.add(recipe_blob(0x852c5432, kBench, {{kStone, 2}}, {{kBlocks, 100}}));
    reg.add(recipe_blob(0x852c5433, kBench, {{kStone, 3}}, {{kBlocks, 50}}));  // not learned
    reg.wire();
    auto name = [](uint32_t item) -> std::string { return item == kArrows ? "Ammo_T0_Arrow_Wooden" : "Block_T1_Stone_Rough"; };
    const std::vector<uint32_t> learned{0x278240d5, 0x852c5432};
    auto c = candidates(w, learned, Station{500, kBench, 2.0}, "", name);
    REQUIRE(c.size() == 2);
    CHECK(c[0].id == 0x852c5432);
    CHECK(c[1].id == 0x278240d5);
    c = candidates(w, learned, std::nullopt, "", name);  // no station: hand recipes only
    REQUIRE(c.size() == 1);
    CHECK(c[0].id == 0x278240d5);
    c = candidates(w, learned, Station{500, kBench, 2.0}, "arrow", name);
    REQUIRE(c.size() == 1);
    CHECK(c[0].id == 0x278240d5);
}

TEST_CASE("bulk_craft glue: what a player carries counts every linked inventory") {
    Rig r;
    emf::World w(kW);
    auto& setup = plant<gen::InventorySetup>(1);
    setup.linksEntities[0].id = 70;
    setup.linksEntities[1].id = 71;
    auto& a = plant<gen::Inventory>(70);
    a.slots[0].id.value = kTwigs;
    a.slots[0].data.count = 7;
    auto& b = plant<gen::Inventory>(71);
    b.slots[3].id.value = kTwigs;
    b.slots[3].data.count = 4;
    b.slots[4].id.value = kStone;
    b.slots[4].data.count = 2;
    const auto have = carried(w, 1);
    CHECK(have.at(kTwigs) == 11);
    CHECK(have.at(kStone) == 2);
}

TEST_CASE("bulk_craft: mod.json parses with every setting clean; off by default, server side, the U key, paced") {
    std::ifstream f(std::string(EMF_SOURCE_DIR) + "/mods/bulk_craft/mod.json", std::ios::binary);
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    auto r = emf::core::parse_manifest(ss.str());
    REQUIRE_MESSAGE(r.manifest, r.error);
    CHECK(r.manifest->dll == "bulk_craft.dll");
    CHECK(r.manifest->emf == 17);
    CHECK_FALSE(r.manifest->enabled_default);
    CHECK(ss.str().find("\"side\": \"server\"") != std::string::npos);
    std::map<std::string, std::string> defaults;
    for (const auto& s : r.manifest->settings) {
        CAPTURE(s.key);
        CHECK(s.problem.empty());
        CHECK(s.warning.empty());
        defaults[s.key] = s.default_value;
    }
    CHECK(defaults["key"] == "U");
    CHECK(defaults["pace"] == "paced");
    CHECK(defaults.count("max_amount"));
    CHECK(defaults.count("range"));
    CHECK(defaults.count("seconds"));
}

// ---- edge cases ----
namespace {
void account(uint32_t player, uint64_t acct) {
    put<uint32_t>(g_infos, (player - 1) * 0xA0, player);
    put<uint64_t>(g_infos, (player - 1) * 0xA0 + 8, acct);
}
Order order_at(uint32_t player, uint64_t acct, uint32_t station, const Recipe& r) {
    Order o;
    o.player = player;
    o.account = acct;
    o.station = station;
    o.recipe = r;
    o.want = 5;
    return o;
}
std::string names(uint32_t) { return "Stone"; }
}  // namespace

TEST_CASE("bulk_craft glue fix: a category input's id and count, and the category's items, from the registry") {
    Rig r;
    emf::World w(kW);
    FakeRegistry reg;
    reg.add(recipe_blob(0x7e4e353a, 0, {{0, 0xC0C0}}, {{0x22, 1}}));
    reg.add_category(0xC0C0, {{0x12, 3.0f}, {0x14, 2.0f}});
    reg.wire();
    const auto rec = recipe_by_id(w, 0x7e4e353a);
    REQUIRE(rec);
    REQUIRE(rec->inputs.size() == 1);
    CHECK(rec->inputs[0].item == 0);
    CHECK(rec->inputs[0].category == 0xC0C0);
    CHECK(rec->inputs[0].count == 1);
    CHECK(rec->category_input);
    const auto items = category_items(w, 0xC0C0);
    REQUIRE(items.size() == 2);
    CHECK(items[1].item == 0x14);
    CHECK(items[1].scaling == doctest::Approx(2.0f));
    CHECK(category_items(w, 0xBEEF).empty());
    Choices c{};
    c[0] = {0xC0C0, 1};
    const auto res = resolve(*rec, c, category_lookup(w));
    REQUIRE(res.result == Resolve::Ok);
    CHECK(res.recipe.inputs[0].item == 0x14);
    CHECK(res.recipe.inputs[0].count == 2);
}

TEST_CASE("bulk_craft glue fix: an order stops quietly when another account has the player's slot") {
    Rig r;
    emf::World w(kW);
    plant<gen::PlayerState>(1);
    at(1, 0, 0, 0);
    station(500, kBench, 2);
    account(1, 0xA1);
    const Recipe rec = [] {
        Recipe x;
        x.id = 1;
        x.workshop = kBench;
        x.outputs = {{kBlocks, 100}};
        return x;
    }();
    Settings s;
    Order o = order_at(1, 0xA1, 500, rec);
    o.settings = s;
    CHECK(order_check(w, o, names).why.empty());
    account(1, 0xB2);  // the player left; someone else joined into entity 1
    const Stop st = order_check(w, o, names);
    CHECK_FALSE(st.why.empty());
    CHECK(st.quiet);
    account(1, 0xA1);
    at(1, 20, 0, 0);  // walked away
    const Stop far = order_check(w, o, names);
    CHECK(far.why == "you walked away from the station");
    CHECK_FALSE(far.quiet);
}

TEST_CASE("bulk_craft glue fix: the material check reads the bag over the API (base chests are not the bag)") {
    Rig r;
    emf::World w(kW);
    auto& setup = plant<gen::InventorySetup>(1);
    setup.linksEntities[0].id = 70;
    auto& inv = plant<gen::Inventory>(70);
    inv.slots[0].id.value = kStone;
    inv.slots[0].data.count = 10;
    Recipe rec;
    rec.id = 0x852c5432;
    rec.inputs = {{kStone, 2}};
    rec.outputs = {{kBlocks, 100}};
    const auto before = carried(w, 1);
    CHECK_FALSE(materials_from_bag(rec, before, carried(w, 1), 1));  // the craft came, the bag still holds 10
    reinterpret_cast<gen::Inventory*>(g_comps[{gen::Inventory::kHash, 70}].data())->slots[0].data.count = 8;
    CHECK(materials_from_bag(rec, before, carried(w, 1), 1));        // 2 stone left the bag
}

TEST_CASE("bulk_craft glue fix: when the game refuses an event subscription, orders are refused") {
    Rig r;
    CHECK(watch_events([](emf::World, const gen::CraftRecipeEvent&) {}, [](emf::World, const gen::PickupEvent&) {}));
    CHECK(g_events_asked == 2);
    g_events_ok = false;
    CHECK_FALSE(watch_events([](emf::World, const gen::CraftRecipeEvent&) {}, [](emf::World, const gen::PickupEvent&) {}));
    CHECK_FALSE(blind_text().empty());
}

TEST_CASE("bulk_craft glue fix: craft.teach is for the host only, never from the game's chat") {
    Rig r;
    CHECK_FALSE(sender_from_chat());
    g_from_chat = true;
    CHECK(sender_from_chat());
}
