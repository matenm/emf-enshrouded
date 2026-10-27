// Tests for the header-only SDK helpers: emf/query.hpp, emf/inventory.hpp, emf/storage.hpp, and emf::init's refusal.
#include <doctest.h>

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "emf/inventory.hpp"
#include "emf/query.hpp"
#include "emf/storage.hpp"
#include "test_scratch.h"

namespace {
namespace gen = emf::gen::ecs;
EmfWorld* const kW = reinterpret_cast<EmfWorld*>(0x5000);

std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> g_store;  // (hash, entity) -> bytes
std::vector<uint32_t> g_entities;
std::vector<std::string> g_logged;
EmfSaveCallback g_load_cb = nullptr;
void* g_load_user = nullptr;
uint32_t g_session = 1;

void* f_get(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size) {
    auto it = g_store.find({hash, entity});
    if (it == g_store.end()) return nullptr;
    if (size) *size = it->second.size();
    return it->second.data();
}
void f_each(EmfWorld*, EmfEntityCallback cb, void* user) {
    for (uint32_t id : g_entities) cb(id, user);
}
uint8_t g_kind = EMF_WORLD_SERVER;
uint8_t f_kind(EmfWorld*) { return g_kind; }
std::map<std::string, std::vector<uint8_t>> g_singletons;
void* f_singleton(EmfWorld*, const char* name) {
    auto it = g_singletons.find(name);
    return it == g_singletons.end() ? nullptr : it->second.data();
}
void f_log(const char* mod, const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    g_logged.push_back(std::string("[") + mod + "] " + b);
}
void f_on_load(EmfSaveCallback cb, void* user) {
    g_load_cb = cb;
    g_load_user = user;
}
uint32_t f_session() { return g_session; }

EmfApi fake_api() {
    EmfApi a{};
    a.version = EMF_API_VERSION;
    a.size = sizeof(EmfApi);
    a.log = f_log;
    a.get_component = f_get;
    a.for_each_entity = f_each;
    a.world_kind = f_kind;
    a.on_load = f_on_load;
    a.server_session = f_session;
    a.singleton = f_singleton;
    return a;
}
template <class T>
T& plant(uint32_t entity) {
    auto& v = g_store[{T::kHash, entity}];
    v.assign(sizeof(T), 0);
    return *reinterpret_cast<T*>(v.data());
}
void place(uint32_t entity, double x, double y, double z) {
    auto& t = plant<gen::CurrentTransform>(entity);
    t.transform.position.x = static_cast<int64_t>(x * 4294967296.0);
    t.transform.position.y = static_cast<int64_t>(y * 4294967296.0);
    t.transform.position.z = static_cast<int64_t>(z * 4294967296.0);
}
void reset() {
    g_kind = EMF_WORLD_SERVER;
    g_singletons.clear();
    g_store.clear();
    g_entities.clear();
    g_logged.clear();
    g_load_cb = nullptr;
    g_session = 1;
}
// %TEMP%\<name>_<run tag>: this run's own, never another emf_tests' running at the same time
std::wstring temp_dir(const wchar_t* name) { return test_scratch::temp_path(name); }
}  // namespace

TEST_CASE("emf::init says why it refuses a core older than the SDK") {
    reset();
    static EmfApi old = fake_api();
    old.version = 9;
    old.size = 200;  // a v9 core's table is shorter
    CHECK_FALSE(emf::init(&old));
    REQUIRE(g_logged.size() == 1);
    CHECK(g_logged[0].find("built for EMF API v" + std::to_string(EMF_API_VERSION)) != std::string::npos);
    CHECK(g_logged[0].find("the core is v9") != std::string::npos);
}

TEST_CASE("query: position in metres and the entities near a point, nearest first, filtered") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    g_entities = {1, 2, 3, 4};
    place(1, 0, 0, 0);
    place(2, 3, 0, 4);  // 5 m
    place(3, 1, 0, 0);  // 1 m
    plant<gen::Health>(3);
    // 4 has no position
    emf::World w(kW);
    const auto p = emf::position(w, 2);
    REQUIRE(p);
    CHECK(p->x == doctest::Approx(3.0));
    CHECK_FALSE(emf::position(w, 4));
    const auto around = emf::entities_near(w, {0, 0, 0}, 6.0);
    REQUIRE(around.size() == 3);
    CHECK(around[0].id == 1);
    CHECK(around[1].id == 3);
    CHECK(around[2].id == 2);
    CHECK(around[2].distance == doctest::Approx(5.0));
    const auto creatures = emf::entities_near(w, {0, 0, 0}, 6.0, [&](uint32_t id) { return w.get<gen::Health>(id) != nullptr; });
    REQUIRE(creatures.size() == 1);
    CHECK(creatures[0].id == 3);
}

TEST_CASE("inventory: stacks of the entity's own and linked inventories, contents summed, gear left out on request") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    auto& setup = plant<gen::InventorySetup>(100);
    setup.linksEntities[0].id = 500;
    setup.linksEntities[1].id = 501;
    auto& a = plant<gen::Inventory>(500);
    a.slots[0].id.value = 0x11;
    a.slots[0].data.count = 5;
    a.slots[3].id.value = 0x22;  // a sword with its data entity
    a.slots[3].data.count = 1;
    a.slots[3].data.pide.id = 900;
    auto& b = plant<gen::Inventory>(501);
    b.slots[7].id.value = 0x11;
    b.slots[7].data.count = 2;
    emf::World w(kW);
    const auto stacks = emf::inventory_stacks(w, 100);
    REQUIRE(stacks.size() == 3);
    CHECK(stacks[1].inventory == 500);
    CHECK(stacks[1].slot == 3);
    CHECK(stacks[1].data_entity == 900);
    CHECK(emf::slot_id(stacks[1].inventory, stacks[1].slot) == ((uint64_t(3) << 32) | 500));
    const auto all = emf::inventory_contents(w, 100);
    CHECK(all.at(0x11) == 7);
    CHECK(all.at(0x22) == 1);
    const auto plain = emf::inventory_contents(w, 100, true);
    CHECK(plain.count(0x22) == 0);
    CHECK(emf::inventory_stacks(w, 77).empty());
}

TEST_CASE("storage: WorldFile loads once per session after the save, saves atomically, never into another world's file") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    const std::wstring dir = temp_dir(L"emf_worldfile_test");
    emf::WorldFile file("mymod");
    file.set_dir(dir);
    REQUIRE(file.attach());
    REQUIRE(g_load_cb);
    DeleteFileW((dir + L"\\0003.txt").c_str());
    int loads = 0;
    std::string got = "?";
    auto on_loaded = [&](const std::string& text) {
        ++loads;
        got = text;
    };
    CHECK_FALSE(file.poll(on_loaded));  // no save loaded yet
    CHECK_FALSE(file.save("too early"));
    g_load_cb(3, g_load_user);  // the host loaded world slot 3 (session 1)
    CHECK(file.poll(on_loaded));
    CHECK(loads == 1);
    CHECK(got.empty());  // no file yet
    CHECK(file.ready());
    CHECK(file.slot() == 3);
    CHECK_FALSE(file.poll(on_loaded));  // once per session
    REQUIRE(file.save("hello\n"));
    CHECK(file.save("hello\n"));  // unchanged: nothing to write
    g_session = 2;                // a reload: until its save loads, the old file must not be written
    CHECK_FALSE(file.poll(on_loaded));
    CHECK_FALSE(file.ready());
    CHECK_FALSE(file.save("from the old session"));
    g_load_cb(3, g_load_user);
    CHECK(file.poll(on_loaded));
    CHECK(loads == 2);
    CHECK(got == "hello\n");  // read back from disk
    CHECK(GetFileAttributesW((dir + L"\\0003.txt.tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
    DeleteFileW((dir + L"\\0003.txt").c_str());
    RemoveDirectoryW(dir.c_str());
}

TEST_CASE("storage: a second world load in the same session re-reads; nothing is written into the other slot's file") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    const std::wstring dir = temp_dir(L"emf_worldfile_test2");
    DeleteFileW((dir + L"\\0003.txt").c_str());
    DeleteFileW((dir + L"\\0005.txt").c_str());
    emf::WorldFile file("mymod");
    file.set_dir(dir);
    REQUIRE(file.attach());
    std::string got;
    auto on_loaded = [&](const std::string& text) { got = text; };
    g_load_cb(3, g_load_user);
    REQUIRE(file.poll(on_loaded));
    REQUIRE(file.save("world three"));
    g_load_cb(5, g_load_user);  // another load, same session number
    CHECK_FALSE(file.ready());
    CHECK_FALSE(file.save("still world three's data"));
    REQUIRE(file.poll(on_loaded));  // the new world's file is read
    CHECK(got.empty());
    REQUIRE(file.save("world five"));
    FILE* f = nullptr;
    REQUIRE(_wfopen_s(&f, (dir + L"\\0003.txt").c_str(), L"rb") == 0);
    char buf[64] = {};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    CHECK(std::string(buf, n) == "world three");  // untouched
    DeleteFileW((dir + L"\\0003.txt").c_str());
    DeleteFileW((dir + L"\\0005.txt").c_str());
    RemoveDirectoryW(dir.c_str());
}

TEST_CASE("storage: a folder that cannot be written is logged once, not on every save") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    const std::wstring blocker = temp_dir(L"emf_worldfile_blocker");  // a FILE where the folder should be
    FILE* b = nullptr;
    REQUIRE(_wfopen_s(&b, blocker.c_str(), L"wb") == 0);
    fclose(b);
    emf::WorldFile file("mymod");
    file.set_dir(blocker + L"\\mymod");
    REQUIRE(file.attach());
    g_load_cb(3, g_load_user);
    REQUIRE(file.poll([](const std::string&) {}));
    g_logged.clear();
    CHECK_FALSE(file.save("a"));
    CHECK_FALSE(file.save("b"));
    CHECK_FALSE(file.save("c"));
    CHECK(g_logged.size() == 1);
    DeleteFileW(blocker.c_str());
}

namespace {
std::vector<std::string> g_global_logged;  // what the API emf::init holds logs (it must not be used below)
void f_global_log(const char* mod, const char*, ...) { g_global_logged.push_back(mod); }
}  // namespace

TEST_CASE("storage: a WorldFile attached to an explicit API uses it, not emf::api(); text() follows reads and saves") {
    reset();
    g_global_logged.clear();
    static EmfApi global = fake_api();
    global.on_load = nullptr;  // attaching through emf::api() would fail
    global.log = f_global_log;
    REQUIRE(emf::init(&global));
    static EmfApi mine = fake_api();
    const std::wstring dir = temp_dir(L"emf_worldfile_explicit");
    DeleteFileW((dir + L"\\0003.txt").c_str());
    emf::WorldFile file("mymod");
    file.set_dir(dir);
    REQUIRE(file.attach(&mine));
    REQUIRE(g_load_cb);
    g_load_cb(3, g_load_user);
    std::string got = "?";
    REQUIRE(file.poll([&](const std::string& t) { got = t; }));
    CHECK(got.empty());
    CHECK(file.text().empty());
    REQUIRE(file.save("a"));
    CHECK(file.text() == "a");
    // a write failure is logged through the explicit API
    const std::wstring blocker = temp_dir(L"emf_worldfile_explicit_blocker");
    FILE* b = nullptr;
    REQUIRE(_wfopen_s(&b, blocker.c_str(), L"wb") == 0);
    fclose(b);
    emf::WorldFile blocked("mymod");
    blocked.set_dir(blocker + L"\\mymod");
    REQUIRE(blocked.attach(&mine));
    g_load_cb(3, g_load_user);
    REQUIRE(blocked.poll([](const std::string&) {}));
    g_logged.clear();
    CHECK_FALSE(blocked.save("x"));
    CHECK(g_logged.size() == 1);
    CHECK(g_global_logged.empty());
    DeleteFileW(blocker.c_str());
    DeleteFileW((dir + L"\\0003.txt").c_str());
    RemoveDirectoryW(dir.c_str());
}

namespace {
uint64_t g_fake_now = 0;
uint64_t fake_clock() { return g_fake_now; }
}  // namespace

TEST_CASE("storage: a WorldFile saves only while its server world runs (poll called within 2 s), not after joining another game") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    const std::wstring dir = temp_dir(L"emf_worldfile_live");
    DeleteFileW((dir + L"\\0003.txt").c_str());
    emf::WorldFile file("mymod");
    file.set_dir(dir);
    file.set_clock(fake_clock);
    REQUIRE(file.attach());
    g_fake_now = 1000;
    g_load_cb(3, g_load_user);
    REQUIRE(file.poll([](const std::string&) {}));
    g_fake_now = 1500;
    CHECK(file.save("a"));
    g_fake_now = 3600;  // the host's world stopped (quit to the menu, joined someone else's game)
    CHECK_FALSE(file.ready());
    CHECK_FALSE(file.save("b"));
    CHECK_FALSE(file.poll([](const std::string&) {}));  // the server world ticks again: nothing new to read
    CHECK(file.ready());
    CHECK(file.save("c"));
    DeleteFileW((dir + L"\\0003.txt").c_str());
    RemoveDirectoryW(dir.c_str());
}

// ---- helpers for gameplay mods: time of day, kill, players and their accounts ----
namespace {
void health(uint32_t entity, std::initializer_list<int32_t> storage, uint32_t current) {
    auto& h = plant<gen::Health>(entity);
    h.storageCount = 8;
    int i = 0;
    for (int32_t v : storage) h.dataStorage[i++] = v;
    auto& n = plant<gen::NetworkHealth>(entity);
    n.health = current;
    n.healthMax = 1498;
}
}  // namespace

TEST_CASE("emf::time_of_day: hours from the world's Daytime (+0x48, ns since midnight), nothing when absent or nonsense") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    emf::World w(kW);
    CHECK_FALSE(emf::time_of_day(w));
    std::vector<uint8_t> d(0x70, 0);
    const int64_t tod = 6ll * 3600 * 1000000000ll + 45ll * 60 * 1000000000ll;  // 06:45
    std::memcpy(d.data() + 0x48, &tod, 8);
    g_singletons["g38_daytime::Daytime"] = d;
    REQUIRE(emf::time_of_day(w));
    CHECK(*emf::time_of_day(w) == doctest::Approx(6.75));
    const int64_t bad = -5;
    std::memcpy(g_singletons["g38_daytime::Daytime"].data() + 0x48, &bad, 8);
    CHECK_FALSE(emf::time_of_day(w));
    CHECK_FALSE(emf::time_of_day(emf::World()));
}

TEST_CASE("emf::kill: the Health slot holding the current health becomes 0; players, the dead and client worlds are refused") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    emf::World w(kW);
    health(500, {1498, 0, 1498, 0, 0, 0, 0, 1498}, 1498);
    health(501, {0, 0, 1498, 0, 0, 0, 0, 1498}, 0);       // already dead
    health(502, {640, 0, 1498, 0, 0, 0, 0, 1498}, 700);  // the replicated health lags: no slot is sure
    health(1, {100, 0, 100}, 100);
    plant<gen::PlayerState>(1);
    CHECK(emf::detail::current_health_slot(g_store[{gen::Health::kHash, 500}].data() + 8, 8, 1498) == 0);
    CHECK(emf::detail::current_health_slot(g_store[{gen::Health::kHash, 502}].data() + 8, 8, 700) == -1);
    std::string why;
    CHECK(emf::detail::kill_with(&api, kW, 500, &why));
    CHECK(w.get<gen::Health>(500)->dataStorage[0] == 0);
    CHECK(w.get<gen::Health>(500)->dataStorage[2] == 1498);
    CHECK_FALSE(emf::kill(w, 501));
    CHECK_FALSE(emf::detail::kill_with(&api, kW, 502, &why));
    CHECK(why.find("slot 0") != std::string::npos);
    CHECK(w.get<gen::Health>(502)->dataStorage[0] == 640);  // nothing written
    CHECK_FALSE(emf::detail::kill_with(&api, kW, 1, &why));
    CHECK(why.find("player") != std::string::npos);
    CHECK(w.get<gen::Health>(1)->dataStorage[0] == 100);
    CHECK_FALSE(emf::kill(w, 777));  // no Health at all
    health(503, {50, 0, 1498}, 50);
    g_kind = EMF_WORLD_CLIENT;
    CHECK_FALSE(emf::kill(w, 503));  // clients only see NetworkHealth
    g_kind = EMF_WORLD_SERVER;
    CHECK(emf::kill(w, 503));
    // the hashes and sizes emf.hpp uses without the generated header are the generated ones
    CHECK(emf::detail::kHealthHash == gen::Health::kHash);
    CHECK(emf::detail::kHealthSize == sizeof(gen::Health));
    CHECK(emf::detail::kNetworkHealthHash == gen::NetworkHealth::kHash);
    CHECK(emf::detail::kNetworkHealthSize == sizeof(gen::NetworkHealth));
    CHECK(emf::detail::kPlayerStateHash == gen::PlayerState::kHash);
    CHECK(offsetof(gen::Health, dataStorage) == 8);
}

TEST_CASE("query: players are the ids 1..1023 with PlayerState and a position; player_account reads BasicPlayerInfos") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    emf::World w(kW);
    for (uint32_t id : {1u, 3u, 1023u}) {
        plant<gen::PlayerState>(id);
        place(id, id, 0, 0);
    }
    plant<gen::PlayerState>(5);  // no position (yet): not counted
    place(2000, 0, 0, 0);
    plant<gen::PlayerState>(2000);  // past the player range
    CHECK(emf::players(w) == std::vector<uint32_t>{1, 3, 1023});
    std::vector<uint8_t> infos(0xA0 * 4, 0);
    const uint32_t one = 1, three = 3, wrong = 9;
    const uint64_t a1 = 0xAA, a3 = 0xCC;
    std::memcpy(infos.data(), &one, 4);
    std::memcpy(infos.data() + 8, &a1, 8);
    std::memcpy(infos.data() + 2 * 0xA0, &three, 4);
    std::memcpy(infos.data() + 2 * 0xA0 + 8, &a3, 8);
    std::memcpy(infos.data() + 1 * 0xA0, &wrong, 4);  // entry 1 is not entity 2's
    g_singletons["BasicPlayerInfos"] = infos;
    CHECK(emf::player_account(w, 1) == 0xAAu);
    CHECK(emf::player_account(w, 3) == 0xCCu);
    CHECK(emf::player_account(w, 2) == 0u);
    CHECK(emf::player_account(w, 0) == 0u);
    CHECK(emf::player_account(w, 5000) == 0u);
}

TEST_CASE("emf::kill writes only slot 0, the current-health slot confirmed in game; a match elsewhere (the maximum) is refused") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    emf::World w(kW);
    health(600, {640, 0, 1498, 0, 0, 0, 0, 1498}, 1498);  // the replicated health equals the maximum's slot, not slot 0
    std::string why;
    CHECK_FALSE(emf::detail::kill_with(&api, kW, 600, &why));
    CHECK(w.get<gen::Health>(600)->dataStorage[0] == 640);
    CHECK(w.get<gen::Health>(600)->dataStorage[2] == 1498);  // the maximum is never touched
    CHECK(w.get<gen::Health>(600)->dataStorage[7] == 1498);
}

// ---- v18: emf::health, emf::attribute, attribute_add, damage, and emf::kill through the attributes ----
namespace {
// attributes as a v18 core reports them: entity -> {current, max}; slots are the core's business
std::map<uint32_t, std::pair<int32_t, int32_t>> g_attrs;
std::vector<std::pair<uint32_t, int32_t>> g_attr_adds;  // (entity, delta)
std::vector<std::vector<uint32_t>> g_damages;            // attacker, target, amount
bool g_sigs_found = true;
bool f_attr_get(EmfWorld*, uint32_t e, uint32_t id, int32_t* out) {
    auto it = g_attrs.find(e);
    if (it == g_attrs.end()) return false;
    if (id == EMF_ATTR_HEALTH) *out = it->second.first;
    else if (id == EMF_ATTR_MAX_HEALTH) *out = it->second.second;
    else return false;
    return true;
}
bool f_attr_add(EmfWorld*, uint32_t e, uint32_t id, int32_t delta) {
    if (id != EMF_ATTR_HEALTH || !g_attrs.count(e)) return false;
    g_attr_adds.push_back({e, delta});
    const int64_t v = static_cast<int64_t>(g_attrs[e].first) + delta;  // the core clamps to 0..max
    g_attrs[e].first = static_cast<int32_t>(v < 0 ? 0 : v > g_attrs[e].second ? g_attrs[e].second : v);
    return true;
}
bool f_damage(EmfWorld*, uint32_t attacker, uint32_t target, int32_t amount) {
    g_damages.push_back({attacker, target, static_cast<uint32_t>(amount)});
    return true;
}
void* f_resolve(const char* name) {
    static int fn = 0;
    const std::string n = name;
    return g_sigs_found && (n == "AttrFind" || n == "AttrAdd") ? &fn : nullptr;
}
EmfApi v18_api() {
    EmfApi a = fake_api();
    a.attribute_get = f_attr_get;
    a.attribute_add = f_attr_add;
    a.damage = f_damage;
    a.resolve = f_resolve;
    return a;
}
void reset_v18() {
    reset();
    g_attrs.clear();
    g_attr_adds.clear();
    g_damages.clear();
    g_sigs_found = true;
}
}  // namespace

TEST_CASE("emf::health: current and max from the attributes; NetworkHealth where they are not (a client, an older core)") {
    reset_v18();
    static EmfApi api = v18_api();
    REQUIRE(emf::init(&api));
    emf::World w(kW);
    g_attrs[500] = {700, 1498};
    int32_t cur = 0, max = 0;
    REQUIRE(emf::health(w, 500, &cur, &max));
    CHECK(cur == 700);
    CHECK(max == 1498);
    health(501, {0, 0, 0}, 320);  // only NetworkHealth (healthMax 1498)
    REQUIRE(emf::health(w, 501, &cur, &max));
    CHECK(cur == 320);
    CHECK(max == 1498);
    CHECK_FALSE(emf::health(w, 777, &cur, &max));
    REQUIRE(emf::attribute(w, 500, EMF_ATTR_MAX_HEALTH));
    CHECK(*emf::attribute(w, 500, EMF_ATTR_MAX_HEALTH) == 1498);
    CHECK_FALSE(emf::attribute(w, 500, EMF_ATTR_MANA));
    g_kind = EMF_WORLD_CLIENT;  // a client world: NetworkHealth, the attributes are not asked
    const size_t asked = g_attr_adds.size();
    REQUIRE(emf::health(w, 501, &cur, &max));
    CHECK(cur == 320);
    CHECK_FALSE(emf::health(w, 500, &cur, &max));  // 500 has attributes only (a fake), no NetworkHealth
    CHECK(g_attr_adds.size() == asked);
    g_kind = EMF_WORLD_SERVER;
    static EmfApi old = fake_api();  // a v17-style table: no attribute functions
    REQUIRE(emf::init(&old));
    CHECK_FALSE(emf::attribute(w, 500, EMF_ATTR_HEALTH));
    REQUIRE(emf::health(w, 501, &cur, &max));  // NetworkHealth still works
    CHECK(cur == 320);
}

TEST_CASE("emf::attribute_add and emf::damage pass through to the core (false on an older core)") {
    reset_v18();
    static EmfApi api = v18_api();
    REQUIRE(emf::init(&api));
    emf::World w(kW);
    g_attrs[500] = {700, 1498};
    CHECK(emf::attribute_add(w, 500, EMF_ATTR_HEALTH, -50));
    CHECK(g_attrs[500].first == 650);
    CHECK(emf::damage(w, 1, 500, 99));
    REQUIRE(g_damages.size() == 1);
    CHECK(g_damages[0] == std::vector<uint32_t>{1, 500, 99});
    static EmfApi old = fake_api();
    old.size = static_cast<uint32_t>(offsetof(EmfApi, attribute_get));  // a v17 core's table
    old.version = 17;
    emf::api_ref() = &old;  // (init would refuse it: this is the SDK seen from a mod built against v17 calling helpers)
    CHECK_FALSE(emf::has_v18());
    CHECK_FALSE(emf::attribute_add(w, 500, EMF_ATTR_HEALTH, -50));
    CHECK_FALSE(emf::damage(w, 1, 500, 99));
    emf::api_ref() = &api;
}

TEST_CASE("emf::kill on a v18 core: the current-health attribute goes to 0 through attribute_add; never a player") {
    reset_v18();
    static EmfApi api = v18_api();
    REQUIRE(emf::init(&api));
    emf::World w(kW);
    g_attrs[500] = {1498, 1498};
    g_attrs[501] = {0, 1498};
    g_attrs[1] = {100, 100};
    plant<gen::PlayerState>(1);
    // its Health slot 0 does not hold the replicated health: the old path would refuse; the attribute path does not care
    health(500, {640, 0, 1498, 0, 0, 0, 0, 1498}, 1498);
    std::string why;
    CHECK(emf::detail::kill_with(&api, kW, 500, &why));
    REQUIRE(g_attr_adds.size() == 1);
    CHECK(g_attr_adds[0] == std::make_pair(500u, INT32_MIN));  // the core's clamp floors it at 0, whatever the health
    CHECK(g_attrs[500].first == 0);
    CHECK(w.get<gen::Health>(500)->dataStorage[0] == 640);  // no raw slot write
    CHECK_FALSE(emf::detail::kill_with(&api, kW, 501, &why));
    CHECK(why == "already dead");
    CHECK_FALSE(emf::detail::kill_with(&api, kW, 1, &why));
    CHECK(why.find("player") != std::string::npos);
    CHECK_FALSE(emf::detail::kill_with(&api, kW, 777, &why));  // no health attribute
    CHECK(g_attr_adds.size() == 1);
}

TEST_CASE("emf::kill falls back to slot 0 (confirmed in game) only when the attribute signatures are missing") {
    reset_v18();
    g_sigs_found = false;  // a v18 core on a game build where AttrFind / AttrAdd were not found
    static EmfApi api = v18_api();
    REQUIRE(emf::init(&api));
    emf::World w(kW);
    g_attrs[500] = {1498, 1498};
    health(500, {1498, 0, 1498, 0, 0, 0, 0, 1498}, 1498);
    CHECK(emf::kill(w, 500));
    CHECK(g_attr_adds.empty());
    CHECK(w.get<gen::Health>(500)->dataStorage[0] == 0);  // the old path wrote slot 0
}
