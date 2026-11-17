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

// 0.6.10 (another framework running EMF mods reports a smaller table): a mod that checks emf::has_vNN() before newer
// calls may start on an older core or host by giving its own minimum; its newer functions are then null.
TEST_CASE("emf::init with a lower minimum takes a smaller table, zero-padded: newer functions null, has_vNN false") {
    reset();
    static EmfApi host = fake_api();
    host.version = 27;
    host.size = static_cast<uint32_t>(offsetof(EmfApi, net_peer) + sizeof(host.net_peer));  // a v27 table
    host.enemy_move = reinterpret_cast<decltype(host.enemy_move)>(uintptr_t{1});  // memory past its table: not its own
    CHECK_FALSE(emf::init(&host));  // the default: this SDK's own version
    REQUIRE(emf::init(&host, 27));
    CHECK(emf::api()->version == 27u);
    CHECK(emf::api()->size == host.size);
    CHECK(emf::api()->log == host.log);
    CHECK(emf::api()->enemy_move == nullptr);  // past the host's table: never read from it
    CHECK_FALSE(emf::has_v40());
    CHECK_FALSE(emf::has_v28());
    CHECK_FALSE(emf::init(&host, 28));  // the mod needs v28
    REQUIRE_FALSE(g_logged.empty());
    CHECK(g_logged.back().find("needs EMF API v28") != std::string::npos);
    static EmfApi full = fake_api();
    REQUIRE(emf::init(&full));  // a full table is used as it is
    CHECK(emf::api() == &full);
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
    CHECK_FALSE(file.save_player(0xAA, "x"));  // the players file: its own line, once
    CHECK_FALSE(file.save("d"));
    CHECK_FALSE(file.save_player(0xAA, "y"));
    CHECK(g_logged.size() == 2);
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

// ---- a text per player in the same world (<slot>.players.txt beside <slot>.txt) ----
namespace {
std::string slurp(const std::wstring& path) {
    std::string out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return out;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}
void spill(const std::wstring& path, const std::string& text) {
    FILE* f = nullptr;
    REQUIRE(_wfopen_s(&f, path.c_str(), L"wb") == 0);
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}
}  // namespace

TEST_CASE("storage: a text per player is read with the world's data and saved beside it") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    const std::wstring dir = temp_dir(L"emf_players_test");
    const std::wstring players = dir + L"\\0003.players.txt";
    DeleteFileW(players.c_str());
    emf::WorldFile file("mymod");
    file.set_dir(dir);
    REQUIRE(file.attach());
    CHECK_FALSE(file.player(0xAA).has_value());  // before the world's data
    CHECK_FALSE(file.save_player(0xAA, "x"));
    g_load_cb(3, g_load_user);
    REQUIRE(file.poll([](const std::string&) {}));
    REQUIRE(file.player(0xAA).has_value());
    CHECK(file.player(0xAA)->empty());  // none yet
    REQUIRE(file.save_player(0xBB, "kills=1"));
    REQUIRE(file.save_player(0xAA, "kills=3"));
    CHECK(*file.player(0xAA) == "kills=3");
    CHECK(file.players_path() == players);
    CHECK(slurp(players) == "emf players 1\n0x00000000000000aa 7\nkills=3\n0x00000000000000bb 7\nkills=1\n");
    CHECK(GetFileAttributesW((players + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
    g_session = 2;  // the next session reads it back
    g_load_cb(3, g_load_user);
    CHECK_FALSE(file.player(0xBB).has_value());  // not before its poll
    REQUIRE(file.poll([](const std::string&) {}));
    CHECK(*file.player(0xBB) == "kills=1");
    DeleteFileW(players.c_str());
    RemoveDirectoryW(dir.c_str());
}

TEST_CASE("storage: player texts round-trip any bytes; an empty text removes the player") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    const std::wstring dir = temp_dir(L"emf_players_bytes");
    const std::wstring players = dir + L"\\0003.players.txt";
    DeleteFileW(players.c_str());
    emf::WorldFile file("mymod");
    file.set_dir(dir);
    REQUIRE(file.attach());
    g_load_cb(3, g_load_user);
    REQUIRE(file.poll([](const std::string&) {}));
    static const char raw[] = "a\nemf players 1\n0x0000000000000001 3\n\0z\n";
    const std::string odd(raw, sizeof raw - 1);
    REQUIRE(file.save_player(0xAA, odd));
    g_session = 2;
    g_load_cb(3, g_load_user);
    REQUIRE(file.poll([](const std::string&) {}));
    CHECK(*file.player(0xAA) == odd);
    REQUIRE(file.save_player(0xAA, ""));
    CHECK(file.player(0xAA)->empty());
    CHECK(slurp(players) == "emf players 1\n");
    CHECK(file.save_player(0xCC, ""));  // removing one that is not there: nothing to do
    DeleteFileW(players.c_str());
    RemoveDirectoryW(dir.c_str());
}

TEST_CASE("storage: player saves are refused over 16 KiB or 2 MiB, for account 0, and into a file that does not parse") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    const std::wstring dir = temp_dir(L"emf_players_limits");
    const std::wstring players = dir + L"\\0003.players.txt";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::string big = "emf players 1\n";  // 127 players of 16 KiB: just under 2 MiB
    for (uint64_t a = 1; a <= 127; ++a) {
        char head[40];
        std::snprintf(head, sizeof head, "0x%016llx 16384\n", static_cast<unsigned long long>(a));
        big += head + std::string(16384, 'p') + "\n";
    }
    REQUIRE(big.size() <= emf::WorldFile::kMaxPlayersFile);
    spill(players, big);
    emf::WorldFile file("mymod");
    file.set_dir(dir);
    REQUIRE(file.attach());
    g_load_cb(3, g_load_user);
    REQUIRE(file.poll([](const std::string&) {}));
    CHECK(file.player(127)->size() == 16384);
    g_logged.clear();
    CHECK_FALSE(file.save_player(0xAA, std::string(16385, 'a')));  // one byte over 16 KiB
    CHECK(g_logged.size() == 1);
    CHECK_FALSE(file.save_player(0xAA, std::string(16384, 'a')));  // the file would pass 2 MiB
    CHECK(g_logged.size() == 2);
    CHECK(slurp(players) == big);  // untouched
    CHECK(file.save_player(1, std::string(16384, 'q')));  // the same size: fits
    CHECK(file.save_player(2, ""));
    CHECK(file.save_player(0xAA, std::string(16384, 'a')));  // room again
    CHECK_FALSE(file.save_player(0, "x"));
    // a file that does not parse is never overwritten
    spill(players, "emf players 1\n0xnothex 3\nabc\n");
    g_session = 2;
    g_load_cb(3, g_load_user);
    g_logged.clear();
    REQUIRE(file.poll([](const std::string&) {}));
    CHECK(g_logged.size() == 1);                 // said when it is read
    CHECK_FALSE(file.player(0xAA).has_value());  // nothing, not "": a mod must not take everyone for new
    CHECK_FALSE(file.save_player(0xAA, "x"));
    CHECK_FALSE(file.save_player(0xAA, "y"));
    CHECK(g_logged.size() == 1);
    CHECK(slurp(players) == "emf players 1\n0xnothex 3\nabc\n");
    DeleteFileW(players.c_str());
    RemoveDirectoryW(dir.c_str());
}

TEST_CASE("storage: a players file that is not EMF's format gives nothing and is never overwritten") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    const std::wstring dir = temp_dir(L"emf_players_bad");
    const std::wstring players = dir + L"\\0003.players.txt";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::string head = "emf players 1\n", aa = "0x00000000000000aa";
    const std::vector<std::string> bad = {
        head + aa + " 3\nab",                                     // the last entry cut short
        head + aa + " 9\nabc\n",                                  // a length past the end
        head + aa + " 3\nabc",                                    // no newline after the text
        "emf players 1\r\n" + aa + " 3\r\nabc\r\n",                // CRLF (a hand edit in Notepad)
        head + aa + " 16385\n" + std::string(16385, 'x') + "\n",  // over 16 KiB
        head + aa + " 1\na\n0x00000000000000AA 1\nb\n",            // the same account twice
        head + aa + "  3\nabc\n",                                 // two spaces
        "emf players 2\n",                                        // a newer format
    };
    emf::WorldFile file("mymod");
    file.set_dir(dir);
    REQUIRE(file.attach());
    uint32_t session = 1;
    for (const auto& text : bad) {
        CAPTURE(text.substr(0, 40));
        spill(players, text);
        g_session = ++session;
        g_load_cb(3, g_load_user);
        REQUIRE(file.poll([](const std::string&) {}));
        CHECK_FALSE(file.player(0xAA).has_value());
        CHECK_FALSE(file.save_player(0xAA, "new"));
        CHECK(slurp(players) == text);
    }
    spill(players, head + aa + " 3\nabc\n");  // fixed: the next load reads it
    g_session = ++session;
    g_load_cb(3, g_load_user);
    REQUIRE(file.poll([](const std::string&) {}));
    CHECK(*file.player(0xAA) == "abc");
    DeleteFileW(players.c_str());
    RemoveDirectoryW(dir.c_str());
}

TEST_CASE("storage: a players file another program holds open is read again later, never saved over meanwhile") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    const std::wstring dir = temp_dir(L"emf_players_locked");
    const std::wstring players = dir + L"\\0003.players.txt";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::string original = "emf players 1\n0x00000000000000aa 3\nabc\n";
    spill(players, original);
    emf::WorldFile file("mymod");
    file.set_dir(dir);
    REQUIRE(file.attach());
    HANDLE h = CreateFileW(players.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);  // no sharing
    REQUIRE(h != INVALID_HANDLE_VALUE);
    g_load_cb(3, g_load_user);
    g_logged.clear();
    REQUIRE(file.poll([](const std::string&) {}));
    CHECK(g_logged.size() == 1);
    CHECK_FALSE(file.player(0xAA).has_value());
    CloseHandle(h);
    CHECK_FALSE(file.save_player(0xBB, "b"));  // not read yet: a save now would drop 0xAA's text
    CHECK(slurp(players) == original);
    CHECK_FALSE(file.poll([](const std::string&) {}));  // the next step reads it again (nothing new for the mod)
    CHECK(*file.player(0xAA) == "abc");
    REQUIRE(file.save_player(0xBB, "b"));
    CHECK(slurp(players) == original + "0x00000000000000bb 1\nb\n");
    DeleteFileW(players.c_str());
    RemoveDirectoryW(dir.c_str());
}

TEST_CASE("storage: another world never sees the last world's players") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    const std::wstring dir = temp_dir(L"emf_players_worlds");
    DeleteFileW((dir + L"\\0003.players.txt").c_str());
    DeleteFileW((dir + L"\\0005.players.txt").c_str());
    emf::WorldFile file("mymod");
    file.set_dir(dir);
    REQUIRE(file.attach());
    g_load_cb(3, g_load_user);
    REQUIRE(file.poll([](const std::string&) {}));
    REQUIRE(file.save_player(0xAA, "three"));
    g_load_cb(5, g_load_user);  // another world, same session
    CHECK_FALSE(file.player(0xAA).has_value());
    CHECK_FALSE(file.save_player(0xAA, "not into three"));
    REQUIRE(file.poll([](const std::string&) {}));
    CHECK(file.player(0xAA)->empty());
    REQUIRE(file.save_player(0xAA, "five"));
    CHECK(slurp(dir + L"\\0003.players.txt") == "emf players 1\n0x00000000000000aa 5\nthree\n");
    DeleteFileW((dir + L"\\0003.players.txt").c_str());
    DeleteFileW((dir + L"\\0005.players.txt").c_str());
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

TEST_CASE("emf::give_xp gives XP as pending XP on a v22 core (the game levels up itself); false on an older core") {
    reset_v18();
    static std::vector<std::vector<int64_t>> adds;
    adds.clear();
    static EmfApi api = v18_api();
    api.attribute_add = [](EmfWorld*, uint32_t e, uint32_t id, int32_t d) {
        adds.push_back({e, static_cast<int64_t>(id), d});
        return true;
    };
    api.version = 22;
    REQUIRE(emf::init(&api));
    emf::World w(kW);
    CHECK(emf::has_v22());
    CHECK(emf::give_xp(w, 1, 500));
    REQUIRE(adds.size() == 1);
    CHECK(adds[0] == std::vector<int64_t>{1, static_cast<int64_t>(EMF_ATTR_XP_PENDING), 500});
    CHECK_FALSE(emf::give_xp(w, 1, 0));  // nothing to give; XP is never taken
    CHECK_FALSE(emf::give_xp(w, 1, -5));
    api.version = 21;
    CHECK_FALSE(emf::has_v22());
    CHECK_FALSE(emf::give_xp(w, 1, 500));
    CHECK(adds.size() == 1);
}

TEST_CASE("emf::xp_required, level_cap and set_level pass through on a v22 core; nothing on a v21 core") {
    reset_v18();
    static std::vector<std::vector<uint32_t>> sets;
    sets.clear();
    static EmfApi api = v18_api();
    api.player_xp_required = [](EmfWorld*, uint32_t level) { return level >= 1 && level <= 25 ? 1000u * level : 0u; };
    api.player_level_cap = [](EmfWorld*) { return 25u; };
    api.player_level_set = [](EmfWorld*, uint32_t player, uint32_t level, uint32_t xp) {
        sets.push_back({player, level, xp});
        return level >= 1 && level <= 25;
    };
    REQUIRE(emf::init(&api));
    emf::World w(kW);
    CHECK(emf::has_v22());
    CHECK(emf::xp_required(w, 4) == 4000u);
    CHECK(emf::xp_required(w, 26) == 0u);
    CHECK(emf::level_cap(w) == 25u);
    CHECK(emf::set_level(w, 7, 3));
    CHECK(emf::set_level(w, 7, 5, 1200));
    CHECK_FALSE(emf::set_level(w, 7, 30));
    REQUIRE(sets.size() == 3);
    CHECK(sets[0] == std::vector<uint32_t>{7, 3, 0});
    CHECK(sets[1] == std::vector<uint32_t>{7, 5, 1200});
    static EmfApi old = api;
    old.size = static_cast<uint32_t>(offsetof(EmfApi, player_xp_required));  // a v21 core's table
    old.version = 21;
    emf::api_ref() = &old;
    CHECK_FALSE(emf::has_v22());
    CHECK(emf::xp_required(w, 4) == 0u);
    CHECK(emf::level_cap(w) == 0u);
    CHECK_FALSE(emf::set_level(w, 7, 3));
    CHECK(sets.size() == 3);
    emf::api_ref() = &api;
}

TEST_CASE("emf::add_component and remove_component take the hash and size from the generated struct (v23)") {
    reset_v18();
    static std::vector<EmfComponentChange> changes;
    static std::vector<float> scales;
    changes.clear();
    scales.clear();
    static EmfApi api = v18_api();
    api.change_component = [](EmfWorld*, const EmfComponentChange* c) {
        changes.push_back(*c);
        if (c->data && c->data_size == 4) {
            float f = 0;
            std::memcpy(&f, c->data, 4);
            scales.push_back(f);
        }
        return true;
    };
    REQUIRE(emf::init(&api));
    emf::World w(kW);
    CHECK(emf::has_v23());
    emf::gen::ecs::ScaleComponent s{};
    s.uniformScale = 1.25f;
    CHECK(emf::add_component(w, 2000, s));
    CHECK(emf::remove_component<emf::gen::ecs::ScaleComponent>(w, 2000));
    CHECK(emf::add_component(w, 2001, emf::gen::ecs::IsTame{}, EMF_COMPONENT_PERSISTENT));
    REQUIRE(changes.size() == 3);
    CHECK(changes[0].size == sizeof(EmfComponentChange));
    CHECK(changes[0].op == EMF_COMPONENT_ADD);
    CHECK(changes[0].hash == emf::gen::ecs::ScaleComponent::kHash);
    CHECK(changes[0].data_size == 4u);
    CHECK(scales == std::vector<float>{1.25f});  // the data was there while the call ran
    CHECK(changes[1].op == EMF_COMPONENT_REMOVE);
    CHECK(changes[1].data == nullptr);
    CHECK(changes[2].flags == EMF_COMPONENT_PERSISTENT);
    // the callback form: an exception from f does not escape into EMF
    CHECK(emf::add_component(w, 2002, s, 0u, [](emf::World, uint32_t, const char*) { throw 1; }));
    REQUIRE(changes.size() == 4);
    CHECK_NOTHROW(changes[3].cb(kW, 2002, emf::gen::ecs::ScaleComponent::kHash, EMF_COMPONENT_DONE, "", changes[3].user));
    static EmfApi old = api;
    old.size = static_cast<uint32_t>(offsetof(EmfApi, change_component));  // a v22 core
    old.version = 22;
    emf::api_ref() = &old;
    CHECK_FALSE(emf::has_v23());
    CHECK_FALSE(emf::add_component(w, 2000, s));
    CHECK(changes.size() == 4);
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

TEST_CASE("emf::ui::Window builds the widgets in order; open keeps f until CLOSED; false on a v23 core (v24)") {
    reset_v18();
    struct Seen {
        std::string id, title;
        std::vector<EmfUiWidget> widgets;
        std::vector<std::string> keys, texts, tips;
        uint32_t widget_size, close_key, place;
        float width;
        uint8_t kind;
        EmfUiCallback cb;
        void* user;
    };
    static std::vector<Seen> opened;
    static std::vector<std::string> updates, closes;
    opened.clear();
    updates.clear();
    closes.clear();
    static EmfApi api = v18_api();
    api.window_open = [](const EmfWindowDef* d) {
        Seen s{d->id, d->title ? d->title : "", {}, {}, {}, {}, d->widget_size, d->close_key, d->place, d->width,
               d->world_kind, d->cb, d->user};
        for (uint32_t i = 0; i < d->count; ++i) {
            const EmfUiWidget& w = d->widgets[i];
            s.widgets.push_back(w);
            s.keys.emplace_back(w.key ? w.key : "");
            s.texts.emplace_back(w.text ? w.text : "");
            s.tips.emplace_back(w.tooltip ? w.tooltip : "");
        }
        opened.push_back(s);
        return true;
    };
    api.window_update = [](const EmfWindowDef* d) {
        updates.emplace_back(d->count ? d->widgets[d->count - 1].text : "");
        return true;
    };
    api.window_close = [](const char* id) {
        closes.emplace_back(id);
        return true;
    };
    REQUIRE(emf::init(&api));
    CHECK(emf::has_v24());
    emf::ui::Window w("test.stable", "Beasts");
    w.heading("Active 1/3")
        .slot("active.1", "Wolf 2", 3)
        .tooltip("Level 12, ready")
        .same_line()
        .slot("active.2", "Slot 2", 0, EMF_UI_EMPTY)
        .separator()
        .row("beast.Boar", "Boar (out)", EMF_UI_SELECTED)
        .button("call", "Call")
        .same_line()
        .button("release", "Release", EMF_UI_DISABLED)
        .bar("Wolf 2", 0.8f)
        .text("hello", 0x112233ffu)
        .place(EMF_UI_PLACE_RIGHT)
        .width(30)
        .close_key('B');
    static int calls = 0;
    static std::string last_key;
    static uint32_t last_kind = 0;
    calls = 0;
    CHECK(w.open(EMF_WORLD_CLIENT, [](emf::World, const emf::ui::Event& e) {
        ++calls;
        last_key = e.key;
        last_kind = e.kind;
    }));
    REQUIRE(opened.size() == 1u);
    const Seen& s = opened[0];
    CHECK(s.id == "test.stable");
    CHECK(s.title == "Beasts");
    CHECK(s.widget_size == sizeof(EmfUiWidget));
    CHECK(s.kind == EMF_WORLD_CLIENT);
    CHECK(s.close_key == 'B');
    CHECK(s.place == EMF_UI_PLACE_RIGHT);
    CHECK(s.width == 30.0f);
    REQUIRE(s.widgets.size() == 9u);
    const uint32_t types[] = {EMF_UI_HEADING, EMF_UI_SLOT, EMF_UI_SLOT, EMF_UI_SEPARATOR, EMF_UI_ROW,
                              EMF_UI_BUTTON,  EMF_UI_BUTTON, EMF_UI_BAR, EMF_UI_TEXT};
    for (size_t i = 0; i < 9; ++i) CHECK(s.widgets[i].type == types[i]);
    CHECK(s.widgets[1].count == 3u);
    CHECK(s.tips[1] == "Level 12, ready");
    CHECK(s.widgets[2].flags == (EMF_UI_SAME_LINE | EMF_UI_EMPTY));
    CHECK(s.widgets[4].flags == EMF_UI_SELECTED);
    CHECK(s.widgets[6].flags == (EMF_UI_SAME_LINE | EMF_UI_DISABLED));
    CHECK(s.widgets[7].value == 800u);
    CHECK(s.widgets[8].rgba == 0x112233ffu);
    CHECK(s.keys[0].empty());
    CHECK(s.keys[5] == "call");
    CHECK(s.texts[4] == "Boar (out)");
    // a click, then CLOSED (the last event: f is deleted after it)
    EmfUiEvent ev{};
    ev.size = sizeof ev;
    ev.kind = EMF_UI_CLICK;
    ev.window = "test.stable";
    ev.key = "call";
    ev.from_key = "";
    s.cb(kW, &ev, s.user);
    CHECK(calls == 1);
    CHECK(last_key == "call");
    ev.kind = EMF_UI_CLOSED;
    ev.key = "";
    s.cb(nullptr, &ev, s.user);
    CHECK(calls == 2);
    CHECK(last_kind == EMF_UI_CLOSED);
    // new contents, and closing
    w.text("more");
    CHECK(w.update());
    REQUIRE(updates.size() == 1u);
    CHECK(updates[0] == "more");
    CHECK(emf::ui::close("test.stable"));
    REQUIRE(closes.size() == 1u);
    CHECK(closes[0] == "test.stable");
    // an exception from f does not escape into EMF
    CHECK(w.open(EMF_WORLD_SERVER, [](emf::World, const emf::ui::Event&) { throw 1; }));
    CHECK_NOTHROW(opened.back().cb(nullptr, &ev, opened.back().user));
    // a v23 core
    static EmfApi old = api;
    old.size = static_cast<uint32_t>(offsetof(EmfApi, window_open));
    old.version = 23;
    emf::api_ref() = &old;
    CHECK_FALSE(emf::has_v24());
    CHECK_FALSE(w.open(EMF_WORLD_SERVER, [](emf::World, const emf::ui::Event&) {}));
    CHECK_FALSE(w.update());
    CHECK_FALSE(emf::ui::close("test.stable"));
    CHECK(opened.size() == 2u);
    emf::api_ref() = &api;
}

TEST_CASE("sdk: coordinate_words reads coordinates written many ways, other words stay") {
    using V = std::vector<std::string>;
    CHECK(emf::coordinate_words(V{"3232.3", "3232.3", "2332.4"}) == V{"3232.3", "3232.3", "2332.4"});
    CHECK(emf::coordinate_words(V{"3232.3,", "3232.3,2332.4"}) == V{"3232.3", "3232.3", "2332.4"});
    CHECK(emf::coordinate_words(V{"(3232.3,", "3232.3,", "2332.4)"}) == V{"3232.3", "3232.3", "2332.4"});
    CHECK(emf::coordinate_words(V{"[1", "2", "3]"}) == V{"1", "2", "3"});
    CHECK(emf::coordinate_words(V{"<1>", "<2>", "<3>"}) == V{"1", "2", "3"});
    CHECK(emf::coordinate_words(V{"player", "1", "at", "(3734.5,", "843.7,", "1418.0)", "m"}) ==
          V{"3734.5", "843.7", "1418.0"});  // a pasted pos line
    CHECK(emf::coordinate_words(V{"#2", "1,2,3", "ground"}) == V{"#2", "1", "2", "3", "ground"});
    CHECK(emf::coordinate_words(V{"1;2;3", "closest"}) == V{"1", "2", "3", "closest"});
    CHECK(emf::coordinate_words(V{}).empty());
}

TEST_CASE("sdk: coordinate_words (review): a pasted pos line followed by ground") {
    using V = std::vector<std::string>;
    CHECK(emf::coordinate_words(V{"player", "1", "at", "(1.0,", "2.0,", "3.0)", "m", "ground"}) == V{"1.0", "2.0", "3.0", "ground"});
}

TEST_CASE("emf::init (review): a lower minimum below 17 counts as 17 (the SDK's older wrappers need it); has_v41 by version") {
    reset();
    static EmfApi host = fake_api();
    host.version = 12;
    host.size = 300;
    CHECK_FALSE(emf::init(&host, 5));
    static EmfApi v40 = fake_api();
    v40.version = 40;  // a 0.6.8 or 0.6.9 core: the same table size as v41
    REQUIRE(emf::init(&v40, 40));
    CHECK(emf::has_v40());
    CHECK_FALSE(emf::has_v41());
    static EmfApi v41 = fake_api();
    v41.version = 41;
    REQUIRE(emf::init(&v41));
    CHECK(emf::has_v41());
    CHECK(emf::has_v25() == true);
    CHECK(emf::has_v26() == true);
    CHECK(emf::has_v27() == true);
}

TEST_CASE("emf::has_v42 (0.6.11): by the version number, the table did not grow") {
    reset();
    static EmfApi v41 = fake_api();
    v41.version = 41;
    REQUIRE(emf::init(&v41, 41));
    CHECK(emf::has_v41());
    CHECK_FALSE(emf::has_v42());
    static EmfApi v42 = fake_api();
    v42.version = 42;
    REQUIRE(emf::init(&v42));
    CHECK(emf::has_v42());
}

TEST_CASE("emf::has_v45 by the version (the table did not grow); has_v46 by the table's size (villager moves appended)") {
    reset();
    static EmfApi v44 = fake_api();
    v44.version = 44;
    v44.size = static_cast<uint32_t>(offsetof(EmfApi, villager_move));  // a v44/v45 core's table
    REQUIRE(emf::init(&v44, 40));
    CHECK(emf::has_v44());
    CHECK_FALSE(emf::has_v45());
    CHECK_FALSE(emf::has_v46());
    EmfMoveOrder m{};
    CHECK(emf::villager_move(emf::World{nullptr}, m) == 0u);
    CHECK(std::string(m.why).find("API v46") != std::string::npos);
    static EmfApi v46 = fake_api();
    v46.version = 46;
    REQUIRE(emf::init(&v46));
    CHECK(emf::has_v45());
    CHECK(emf::has_v46());
}

TEST_CASE("storage: a world file another program holds open is not handed over as empty, nor saved over; a later poll reads it") {
    reset();
    static EmfApi api = fake_api();
    REQUIRE(emf::init(&api));
    const std::wstring dir = temp_dir(L"emf_world_locked");
    const std::wstring main = dir + L"\\0003.txt";
    CreateDirectoryW(dir.c_str(), nullptr);
    spill(main, "keep me");
    emf::WorldFile file("mymod");
    file.set_dir(dir);
    REQUIRE(file.attach());
    HANDLE h = CreateFileW(main.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);  // no sharing
    REQUIRE(h != INVALID_HANDLE_VALUE);
    g_load_cb(3, g_load_user);
    g_logged.clear();
    std::string got = "none";
    CHECK_FALSE(file.poll([&](const std::string& t) { got = t; }));
    CHECK(got == "none");  // not "" as if there were no file
    CHECK_FALSE(file.ready());
    CHECK_FALSE(file.save("new"));
    CHECK(g_logged.size() == 1);
    CHECK_FALSE(file.poll([&](const std::string& t) { got = t; }));
    CHECK(g_logged.size() == 1);  // said once
    CloseHandle(h);
    CHECK(slurp(main) == "keep me");
    CHECK(file.poll([&](const std::string& t) { got = t; }));
    CHECK(got == "keep me");
    REQUIRE(file.save("new"));
    CHECK(slurp(main) == "new");
    DeleteFileW(main.c_str());
    RemoveDirectoryW(dir.c_str());
}
