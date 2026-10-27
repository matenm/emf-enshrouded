#include <doctest.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "emf/emf.hpp"

namespace {
struct Small {
    static constexpr uint32_t kHash = 0x11u;
    uint32_t v;
};
struct Big {
    static constexpr uint32_t kHash = 0x22u;
    uint8_t bytes[456];
};
struct BigDynamic {  // per-entity half of the split component Big, keyed by its own hash
    static constexpr uint32_t kHash = 0x33u;
    uint8_t bytes[136];
};

uint32_t g_small = 1234;
uint8_t g_dynamic[136];
EmfWorld* const kWorld = reinterpret_cast<EmfWorld*>(0x5000);
EmfWorldCallback g_tick_cb = nullptr;
void* g_tick_user = nullptr;

void* fake_get_component(EmfWorld*, uint32_t hash, uint32_t, uint64_t* size) {
    if (hash == Small::kHash) { if (size) *size = 4; return &g_small; }
    if (hash == BigDynamic::kHash) { if (size) *size = 136; return g_dynamic; }  // split component: dynamic half
    if (size) *size = 0;
    return nullptr;
}
void fake_for_each(EmfWorld*, EmfEntityCallback cb, void* user) {
    for (uint32_t id : {7u, 8u, 9u}) cb(id, user);
}
void fake_on_tick(EmfWorldCallback cb, void* user) {
    g_tick_cb = cb;
    g_tick_user = user;
}
uint8_t fake_kind(EmfWorld*) { return EMF_WORLD_SERVER; }

EmfApi make_fake_api() {
    EmfApi a{};
    a.version = EMF_API_VERSION;
    a.size = sizeof(EmfApi);
    a.get_component = fake_get_component;
    a.for_each_entity = fake_for_each;
    a.on_tick = fake_on_tick;
    a.world_kind = fake_kind;
    return a;
}
}  // namespace

TEST_CASE("emf::init rejects a missing or too-old API") {
    EmfApi old{};
    old.version = 0;
    old.size = sizeof(EmfApi);
    CHECK_FALSE(emf::init(nullptr));
    CHECK_FALSE(emf::init(&old));
    static EmfApi good = make_fake_api();
    CHECK(emf::init(&good));
}

TEST_CASE("World::get<T> returns the component only when the size matches T") {
    static EmfApi api = make_fake_api();
    REQUIRE(emf::init(&api));
    emf::World w(kWorld);
    REQUIRE(w.get<Small>(1) != nullptr);
    CHECK(w.get<Small>(1)->v == 1234);
    CHECK(w.get<Big>(1) == nullptr);  // the template half has no per-entity storage
    CHECK(w.get<BigDynamic>(1) == reinterpret_cast<BigDynamic*>(g_dynamic));
    CHECK(w.is_server());
}

TEST_CASE("World::for_each_entity and emf::on_tick accept capturing lambdas") {
    static EmfApi api = make_fake_api();
    REQUIRE(emf::init(&api));
    std::vector<uint32_t> seen;
    emf::World(kWorld).for_each_entity([&](uint32_t id) { seen.push_back(id); });
    CHECK(seen == std::vector<uint32_t>{7, 8, 9});

    int ticks = 0;
    emf::on_tick([&ticks](emf::World w) { ticks += (w.raw() == kWorld); });
    REQUIRE(g_tick_cb != nullptr);
    g_tick_cb(kWorld, g_tick_user);
    g_tick_cb(kWorld, g_tick_user);
    CHECK(ticks == 2);
}

// ---- v3 helpers ----
namespace {
struct PingEvent {
    static constexpr uint32_t kHash = 0x44u;
    static constexpr uint32_t kSize = 16;
    static constexpr const char* kName = "PingEvent";
    int64_t timeStamp;
    uint32_t a, b;
};
alignas(8) uint8_t g_ping[16];
std::string g_emit_name;
uint32_t g_emit_size = 0;
void* fake_emit_event(void*, const char* name, uint32_t size) {
    g_emit_name = name;
    g_emit_size = size;
    return g_ping;
}
uint32_t fake_read_events(void*, const char* name, uint32_t size, EmfEventCallback cb, void* user) {
    g_emit_name = name;
    g_emit_size = size;
    PingEvent e1{0, 1, 2}, e2{0, 3, 4};
    cb(&e1, user);
    cb(&e2, user);
    return 2;
}
std::string g_def_name;
std::vector<std::string> g_def_in, g_def_out;
EmfProgramCallback g_def_fn = nullptr;
void* g_def_user = nullptr;
std::vector<std::string> g_def_singletons, g_def_other;
bool fake_register_def(const EmfProgramDef* d) {
    g_def_name = d->name;
    g_def_in.clear();
    g_def_out.clear();
    g_def_singletons.clear();
    g_def_other.clear();
    for (auto p = d->events_in; p && *p; ++p) g_def_in.push_back(*p);
    for (auto p = d->events_out; p && *p; ++p) g_def_out.push_back(*p);
    for (auto p = d->singletons; p && *p; ++p) g_def_singletons.push_back(*p);
    for (auto p = d->other_read; p && *p; ++p) g_def_other.push_back(*p);
    g_def_fn = d->fn;
    g_def_user = d->user;
    return d->size == sizeof(EmfProgramDef) && d->world_kind == EMF_WORLD_SERVER;
}
}  // namespace

TEST_CASE("emf::emit<E> and emf::read<E> pass the event's name and size") {
    static EmfApi api = make_fake_api();
    api.emit_event = fake_emit_event;
    api.read_events = fake_read_events;
    REQUIRE(emf::init(&api));
    PingEvent* e = emf::emit<PingEvent>(reinterpret_cast<void*>(0x1));
    CHECK(e == reinterpret_cast<PingEvent*>(g_ping));
    CHECK(g_emit_name == "PingEvent");
    CHECK(g_emit_size == 16);
    std::vector<uint32_t> seen;
    CHECK(emf::read<PingEvent>(reinterpret_cast<void*>(0x1), [&](const PingEvent& p) { seen.push_back(p.a); }) == 2);
    CHECK(seen == std::vector<uint32_t>{1, 3});
}

TEST_CASE("emf::add_program builds the definition and calls the functor with the program context") {
    static EmfApi api = make_fake_api();
    api.register_program_def = fake_register_def;
    REQUIRE(emf::init(&api));
    void* seen_ctx = nullptr;
    CHECK(emf::add_program("guard", EMF_WORLD_SERVER, {"EntityDiedEvent"}, {"StartGuardingEvent", "StopGuardingEvent"},
                           [&seen_ctx](void* ctx) { seen_ctx = ctx; }));
    CHECK(g_def_name == "guard");
    CHECK(g_def_in == std::vector<std::string>{"EntityDiedEvent"});
    CHECK(g_def_out == std::vector<std::string>{"StartGuardingEvent", "StopGuardingEvent"});
    REQUIRE(g_def_fn != nullptr);
    g_def_fn(reinterpret_cast<void*>(0x77), g_def_user);
    CHECK(seen_ctx == reinterpret_cast<void*>(0x77));
}

TEST_CASE("emf::parse_guid reads text GUIDs in Windows (ms) or raw byte order") {
    uint8_t g[16];
    REQUIRE(emf::parse_guid("44604a7b-946b-4727-bb72-8c0eb3c08fa1", g));
    CHECK(g[0] == 0x7b);
    CHECK(g[3] == 0x44);
    CHECK(g[4] == 0x6b);
    CHECK(g[8] == 0xbb);
    CHECK(g[15] == 0xa1);
    REQUIRE(emf::parse_guid("44604a7b-946b-4727-bb72-8c0eb3c08fa1", g, false));
    CHECK(g[0] == 0x44);
    CHECK_FALSE(emf::parse_guid("not-a-guid", g));
}

// ---- v4: on_event helper ----
namespace {
std::string g_sub_name;
uint32_t g_sub_size = 0;
uint8_t g_sub_kind = 0xFF;
EmfWorldEventCallback g_sub_cb = nullptr;
void* g_sub_user = nullptr;
bool fake_on_event(uint8_t kind, const char* name, uint32_t size, EmfWorldEventCallback cb, void* user) {
    g_sub_kind = kind;
    g_sub_name = name;
    g_sub_size = size;
    g_sub_cb = cb;
    g_sub_user = user;
    return true;
}
}  // namespace

TEST_CASE("emf::on_event<E> subscribes with E's name and size and hands the functor a World and the event") {
    static EmfApi api = make_fake_api();
    api.on_event = fake_on_event;
    REQUIRE(emf::init(&api));
    EmfWorld* seen_world = nullptr;
    uint32_t seen_a = 0;
    CHECK(emf::on_event<PingEvent>(EMF_WORLD_SERVER, [&](emf::World w, const PingEvent& e) {
        seen_world = w.raw();
        seen_a = e.a;
    }));
    CHECK(g_sub_kind == EMF_WORLD_SERVER);
    CHECK(g_sub_name == "PingEvent");
    CHECK(g_sub_size == sizeof(PingEvent));
    REQUIRE(g_sub_cb != nullptr);
    PingEvent e{0, 42, 0};
    g_sub_cb(kWorld, &e, sizeof(e), g_sub_user);
    CHECK(seen_world == kWorld);
    CHECK(seen_a == 42);
}

// ---- v5: item helpers ----
namespace {
std::string g_find_name;
uint32_t g_give_player = 0, g_give_item_id = 0, g_give_count = 0;
uint32_t fake_find_item(EmfWorld*, const char* name) {
    g_find_name = name;
    return std::string(name) == "Torch" ? 0x22u : 0u;
}
uint32_t fake_give_item(EmfWorld*, uint32_t player, uint32_t item, uint32_t count) {
    g_give_player = player;
    g_give_item_id = item;
    g_give_count = count;
    return 7001;
}
}  // namespace

TEST_CASE("emf::item_id is FNV-1a over the GUID bytes; emf::give_item looks the name up first") {
    uint8_t g[16];
    for (int i = 0; i < 16; ++i) g[i] = static_cast<uint8_t>(i + 1);
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 16; ++i) h = (h ^ g[i]) * 0x01000193u;
    CHECK(emf::item_id(g) == h);
    const uint8_t zero[16] = {};
    CHECK(emf::item_id(zero) == 0u);
    static EmfApi api = make_fake_api();
    api.find_item = fake_find_item;
    api.give_item = fake_give_item;
    REQUIRE(emf::init(&api));
    CHECK(emf::give_item(emf::World(kWorld), 1, "Torch", 5) == 7001u);
    CHECK(g_give_item_id == 0x22u);
    CHECK(g_give_count == 5);
    g_give_item_id = 0;
    CHECK(emf::give_item(emf::World(kWorld), 1, "Nothing", 5) == 0u);
    CHECK(g_give_item_id == 0u);  // unknown names never reach give_item
}

TEST_CASE("emf::guid_to_text writes what parse_guid reads (Windows layout by default)") {
    uint8_t g[16];
    REQUIRE(emf::parse_guid("44604a7b-946b-4727-bb72-8c0eb3c08fa1", g));
    CHECK(emf::guid_to_text(g) == "44604a7b-946b-4727-bb72-8c0eb3c08fa1");
    REQUIRE(emf::parse_guid("44604a7b-946b-4727-bb72-8c0eb3c08fa1", g, false));
    CHECK(emf::guid_to_text(g, false) == "44604a7b-946b-4727-bb72-8c0eb3c08fa1");
}

TEST_CASE("emf::item_id known answer from the game: Material_T1_Wood_Logs") {
    uint8_t g[16];
    REQUIRE(emf::parse_guid("3f4093ae-75ed-4e5d-bd0b-784ba83b88f5", g));
    CHECK(emf::item_id(g) == 0x2b0cf1d5u);
}

// ---- v6: command helper ----
namespace {
std::string g_cmd_name, g_cmd_help;
EmfCommandCallback g_cmd_cb = nullptr;
void* g_cmd_user = nullptr;
bool fake_register_command(const char* name, const char* help, EmfCommandCallback cb, void* user) {
    g_cmd_name = name;
    g_cmd_help = help;
    g_cmd_cb = cb;
    g_cmd_user = user;
    return true;
}
}  // namespace

TEST_CASE("emf::add_command hands the functor a World and the arguments as strings") {
    static EmfApi api = make_fake_api();
    api.register_command = fake_register_command;
    REQUIRE(emf::init(&api));
    std::vector<std::string> seen;
    EmfWorld* seen_world = nullptr;
    CHECK(emf::add_command("mymod.count", "<n>: count", [&](emf::World w, const std::vector<std::string>& args) {
        seen_world = w.raw();
        seen = args;
    }));
    CHECK(g_cmd_name == "mymod.count");
    CHECK(g_cmd_help == "<n>: count");
    REQUIRE(g_cmd_cb != nullptr);
    const char* argv[] = {"mymod.count", "7"};
    g_cmd_cb(kWorld, 2, argv, g_cmd_user);
    CHECK(seen_world == kWorld);
    CHECK(seen == std::vector<std::string>{"mymod.count", "7"});
}

// ---- a C++ exception in a command is a reply, not a crash ----
namespace {
std::vector<std::string> g_replies;
void fake_command_reply(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_replies.push_back(buf);
}
}  // namespace

TEST_CASE("emf::add_command turns a C++ exception from the functor into a reply") {
    static EmfApi api = make_fake_api();
    api.register_command = fake_register_command;
    api.command_reply = fake_command_reply;
    REQUIRE(emf::init(&api));
    REQUIRE(emf::add_command("mymod.heal", "<n>", [](emf::World, const std::vector<std::string>& args) {
        (void)std::stoi(args[1]);  // throws std::invalid_argument for "abc"
    }));
    REQUIRE(g_cmd_cb != nullptr);
    g_replies.clear();
    const char* bad[] = {"mymod.heal", "abc"};
    g_cmd_cb(kWorld, 2, bad, g_cmd_user);  // must return normally
    REQUIRE(g_replies.size() == 1);
    CHECK(g_replies[0].rfind("mymod.heal: ", 0) == 0);
    REQUIRE(emf::add_command("mymod.odd", "", [](emf::World, const std::vector<std::string>&) { throw 42; }));
    g_replies.clear();
    const char* odd[] = {"mymod.odd"};
    g_cmd_cb(kWorld, 1, odd, g_cmd_user);
    REQUIRE(g_replies.size() == 1);
    CHECK(g_replies[0] == "mymod.odd: unknown C++ exception");
}

// ---- v7: template components ----
namespace {
uint32_t g_tpl_hash = 0, g_tpl_entity = 0;
uint64_t g_tpl_size = 456;
uint8_t g_tpl_buf[456] = {};
void* fake_get_template_component(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size_out) {
    g_tpl_hash = hash;
    g_tpl_entity = entity;
    if (size_out) *size_out = g_tpl_size;
    return g_tpl_buf;
}
struct TplThing {
    static constexpr uint32_t kHash = 0x1a8b5909u;
    static constexpr uint32_t kSize = 456;
    uint8_t bytes[456];
};
}  // namespace

TEST_CASE("World::get_template<T> asks for T::kHash and the entity") {
    static EmfApi api = make_fake_api();
    api.get_template_component = fake_get_template_component;
    REQUIRE(emf::init(&api));
    const TplThing* t = emf::World(kWorld).get_template<TplThing>(1025);
    static_assert(std::is_same_v<decltype(emf::World(kWorld).get_template<TplThing>(1)), const TplThing*>,
                  "template data is read-only: write it with emf::write_value");
    CHECK(static_cast<const void*>(t) == static_cast<const void*>(g_tpl_buf));
    CHECK(g_tpl_hash == 0x1a8b5909u);
    CHECK(g_tpl_entity == 1025);
    g_tpl_size = 400;  // the game's layout changed after an update
    CHECK(emf::World(kWorld).get_template<TplThing>(1025) == nullptr);
    g_tpl_size = 0;  // size unknown: refused like get<T>
    CHECK(emf::World(kWorld).get_template<TplThing>(1025) == nullptr);
    g_tpl_size = 456;
    api.get_template_component = nullptr;  // an older core without the function
    CHECK(emf::World(kWorld).get_template<TplThing>(1025) == nullptr);
}

// ---- v7: write_memory helper ----
namespace {
void* g_wm_dst = nullptr;
uint64_t g_wm_n = 0;
bool fake_write_memory(void* dst, const void* src, uint64_t n) {
    g_wm_dst = dst;
    g_wm_n = n;
    std::memcpy(dst, src, static_cast<size_t>(n));
    return true;
}
}  // namespace

TEST_CASE("emf::write_value writes one value through write_memory") {
    static EmfApi api = make_fake_api();
    api.write_memory = fake_write_memory;
    REQUIRE(emf::init(&api));
    float target = 3.0f;
    const float* read_only_view = &target;  // what get_template<T> hands out
    CHECK(emf::write_value(read_only_view, 6.0f));
    CHECK(target == 6.0f);
    CHECK(g_wm_dst == &target);
    CHECK(g_wm_n == sizeof(float));
    api.write_memory = nullptr;
    CHECK_FALSE(emf::write_value(&target, 1.0f));
    CHECK(target == 6.0f);
}

namespace {
struct {
    std::string component;
    uint32_t entity = 0, size = 0;
    std::string singleton;
} g_v8hpp;
uint32_t g_v8hpp_value = 77;
void* fake_read_other(void*, const char* component, uint32_t entity, uint32_t size) {
    g_v8hpp.component = component;
    g_v8hpp.entity = entity;
    g_v8hpp.size = size;
    return &g_v8hpp_value;
}
void* fake_program_singleton(void*, const char* name) {
    g_v8hpp.singleton = name;
    return &g_v8hpp_value;
}
}  // namespace

TEST_CASE("emf::read_other<T> passes the component name, entity and sizeof(T); emf::program_singleton forwards") {
    EmfApi a = make_fake_api();
    a.program_read_other = fake_read_other;
    a.program_singleton = fake_program_singleton;
    REQUIRE(emf::init(&a));
    int ctx = 0;
    const Small* s = emf::read_other<Small>(&ctx, "Small", 42);
    CHECK(s == reinterpret_cast<const Small*>(&g_v8hpp_value));
    CHECK(g_v8hpp.component == "Small");
    CHECK(g_v8hpp.entity == 42);
    CHECK(g_v8hpp.size == sizeof(Small));
    CHECK(emf::program_singleton(&ctx, "g38_items::ItemRegistry") == &g_v8hpp_value);
    CHECK(g_v8hpp.singleton == "g38_items::ItemRegistry");
}

namespace {
struct {
    uint32_t entity = 0, item = 0, count = 0;
    EmfPutItemsCallback cb = nullptr;
    void* user = nullptr;
    bool accept = true;
} g_put;
bool fake_put_items(EmfWorld*, uint32_t entity, uint32_t item, uint32_t count, EmfPutItemsCallback cb, void* user) {
    g_put.entity = entity;
    g_put.item = item;
    g_put.count = count;
    g_put.cb = cb;
    g_put.user = user;
    return g_put.accept;
}
}  // namespace

TEST_CASE("emf::put_items looks the item up, queues it, and hands the functor the world and how many were added") {
    static EmfApi api = make_fake_api();
    api.find_item = fake_find_item;
    api.put_items = fake_put_items;
    REQUIRE(emf::init(&api));
    uint32_t added = 99;
    std::vector<uint32_t> seen;
    CHECK(emf::put_items(emf::World(kWorld), 1, "Torch", 5, [&](emf::World w, uint32_t n) {
        CHECK(w.raw() == kWorld);
        added = n;
        seen.push_back(n);
    }));
    CHECK(g_put.entity == 1);
    CHECK(g_put.item == 0x22u);
    CHECK(g_put.count == 5);
    REQUIRE(g_put.cb);
    g_put.cb(kWorld, 1, 0x22u, 5, 4, g_put.user);  // the core reports 4 added
    CHECK(added == 4);
    CHECK(seen.size() == 1);
    CHECK_FALSE(emf::put_items(emf::World(kWorld), 1, "Nothing", 5, [](emf::World, uint32_t) {}));  // unknown name
    g_put.accept = false;
    CHECK_FALSE(emf::put_items(emf::World(kWorld), 1, "Torch", 5, [](emf::World, uint32_t) {}));  // refused: freed
    g_put.accept = true;
}

TEST_CASE("emf::add_program with singletons and other-entity reads fills the v8 fields") {
    static EmfApi api = make_fake_api();
    api.register_program_def = fake_register_def;
    REQUIRE(emf::init(&api));
    CHECK(emf::add_program("watch", EMF_WORLD_SERVER, {}, {"NpcCommandEvent"}, {"g38_items::ItemRegistry"},
                           {"CurrentTransform", "Inventory"}, [](void*) {}));
    CHECK(g_def_name == "watch");
    CHECK(g_def_out == std::vector<std::string>{"NpcCommandEvent"});
    CHECK(g_def_singletons == std::vector<std::string>{"g38_items::ItemRegistry"});
    CHECK(g_def_other == std::vector<std::string>{"CurrentTransform", "Inventory"});
}

namespace {
std::string g_add_log;
void fake_log_text(const char*, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_add_log = buf;
}
}  // namespace

TEST_CASE("emf::add_program with other_write fails on a pre-v16 core (every write would give NULL) and says so") {
    static EmfApi api = make_fake_api();
    api.register_program_def = fake_register_def;
    api.log = fake_log_text;
    api.size = static_cast<uint32_t>(offsetof(EmfApi, program_write_other));  // a v15 core
    emf::api_ref() = &api;  // (emf::init refuses an older core; a mod may still set the API itself)
    g_def_name.clear();
    g_add_log.clear();
    CHECK_FALSE(emf::add_program("writer", EMF_WORLD_SERVER, {}, {}, {}, {}, {"CurrentTransform"}, [](void*) {}));
    CHECK(g_def_name.empty());  // not registered
    CHECK(g_add_log.find("writer") != std::string::npos);
    CHECK(g_add_log.find("needs EMF API v16") != std::string::npos);
    // nothing to write: a plain v8 program, fine on that core
    CHECK(emf::add_program("reader", EMF_WORLD_SERVER, {}, {}, {}, {"CurrentTransform"}, {}, [](void*) {}));
    CHECK(g_def_name == "reader");
    api.size = sizeof(EmfApi);  // a v16 core
    CHECK(emf::add_program("writer", EMF_WORLD_SERVER, {}, {}, {}, {}, {"CurrentTransform"}, [](void*) {}));
    CHECK(g_def_name == "writer");
    emf::api_ref() = nullptr;
}

namespace {
EmfWheelCallback g_wheel_cb = nullptr;
void* g_wheel_user = nullptr;
bool fake_wheel_open(const EmfWheel* w) {
    g_wheel_cb = w->cb;
    g_wheel_user = w->user;
    return true;
}
std::vector<std::string> g_hpp_logs;
void fake_log(const char*, const char* fmt, ...) { g_hpp_logs.emplace_back(fmt); }
}  // namespace

TEST_CASE("emf::wheel: a callback that throws is caught, logged, and its functor is still freed") {
    static EmfApi api = make_fake_api();
    api.wheel_open = fake_wheel_open;
    api.log = fake_log;
    REQUIRE(emf::init(&api));
    auto alive = std::make_shared<int>(1);
    CHECK(emf::wheel("t", {"a"}, 0, EMF_WORLD_SERVER, [alive](emf::World, int) { throw std::runtime_error("boom"); }));
    CHECK(alive.use_count() == 2);
    REQUIRE(g_wheel_cb);
    g_hpp_logs.clear();
    g_wheel_cb(nullptr, 0, g_wheel_user);  // must not throw out of the C callback
    CHECK(alive.use_count() == 1);         // freed
    CHECK_FALSE(g_hpp_logs.empty());
}

namespace {
EmfItemsRequest g_items_req{};
bool g_items_take = false;
bool fake_take(EmfWorld*, const EmfItemsRequest* r) {
    g_items_req = *r;
    g_items_take = true;
    return true;
}
bool fake_put_ex(EmfWorld*, const EmfItemsRequest* r) {
    g_items_req = *r;
    g_items_take = false;
    return true;
}
}  // namespace

TEST_CASE("emf::take_items / emf::put_items_ex fill the v11 request and refuse on an older core") {
    static EmfApi api = make_fake_api();
    api.take_items = fake_take;
    api.put_items_ex = fake_put_ex;
    REQUIRE(emf::init(&api));
    uint32_t got = 99;
    CHECK(emf::take_items(emf::World(nullptr), 1, 0x11, 2, (uint64_t(3) << 32) | 500,
                          [&got](emf::World, uint32_t taken) { got = taken; }));
    CHECK(g_items_take);
    CHECK(g_items_req.size == sizeof(EmfItemsRequest));
    CHECK(g_items_req.slot == ((uint64_t(3) << 32) | 500));
    REQUIRE(g_items_req.cb);
    g_items_req.cb(nullptr, 1, 0x11, 2, 2, g_items_req.user);
    CHECK(got == 2);
    CHECK(emf::put_items_ex(emf::World(nullptr), 1, 0x11, 1, 12, 3, [](emf::World, uint32_t) {}));
    CHECK_FALSE(g_items_take);
    CHECK(g_items_req.level == 12);
    CHECK(g_items_req.rarity == 3);
    CHECK(g_items_req.flags == EMF_ITEMS_RARITY);
    g_items_req.cb(nullptr, 1, 0x11, 1, 1, g_items_req.user);  // frees the functor
    api.size = static_cast<uint32_t>(offsetof(EmfApi, put_items_ex));  // the core reports a v10 table
    CHECK_FALSE(emf::take_items(emf::World(nullptr), 1, 0x11, 1, 0, [](emf::World, uint32_t) {}));
    CHECK_FALSE(emf::put_items_ex(emf::World(nullptr), 1, 0x11, 1, 0, 0xFF, [](emf::World, uint32_t) {}));
    api.size = sizeof(EmfApi);
}

TEST_CASE("emf::take_items can ask for the exact slot; put_items_ex leaves the rarity to the game with 0xFF") {
    static EmfApi api = make_fake_api();
    api.take_items = fake_take;
    api.put_items_ex = fake_put_ex;
    REQUIRE(emf::init(&api));
    CHECK(emf::take_items(emf::World(nullptr), 1, 0x11, 1, (uint64_t(3) << 32) | 500, [](emf::World, uint32_t) {}, true));
    CHECK(g_items_req.flags == EMF_ITEMS_EXACT_SLOT);
    g_items_req.cb(nullptr, 1, 0x11, 1, 1, g_items_req.user);
    CHECK(emf::put_items_ex(emf::World(nullptr), 1, 0x11, 1, 5, 0xFF, [](emf::World, uint32_t) {}));
    CHECK(g_items_req.flags == 0);
    g_items_req.cb(nullptr, 1, 0x11, 1, 1, g_items_req.user);
}

namespace {
std::string g_hud_id, g_hud_text;
uint32_t g_hud_place = 99;
bool fake_hud(const char* id, const char* text, uint32_t place, uint32_t) {
    g_hud_id = id ? id : "";
    g_hud_text = text ? text : "";
    g_hud_place = place;
    return true;
}
}  // namespace

TEST_CASE("emf::hud sets a line (top right by default) and refuses on an older core") {
    static EmfApi api = make_fake_api();
    api.hud_set = fake_hud;
    REQUIRE(emf::init(&api));
    CHECK(emf::hud("clock.time", "14:05"));
    CHECK(g_hud_id == "clock.time");
    CHECK(g_hud_text == "14:05");
    CHECK(g_hud_place == EMF_HUD_TOP_RIGHT);
    CHECK(emf::hud("clock.time", "", EMF_HUD_BOTTOM_LEFT));
    CHECK(g_hud_place == EMF_HUD_BOTTOM_LEFT);
    api.size = static_cast<uint32_t>(offsetof(EmfApi, hud_set));  // a v11 core
    CHECK_FALSE(emf::hud("clock.time", "14:05"));
}

namespace {
std::string g_noted;
void fake_notify(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_noted = buf;
}
}  // namespace

TEST_CASE("emf.hpp: EMF_NOTIFY formats a note through the API, and does nothing before init") {
    emf::api_ref() = nullptr;
    EMF_NOTIFY("ignored %d", 1);  // no API yet: no crash
    EmfApi a = make_fake_api();
    a.notify = fake_notify;
    REQUIRE(emf::init(&a));
    EMF_NOTIFY("%d logs into %s", 5, "the chest");
    CHECK(g_noted == "5 logs into the chest");
    emf::api_ref() = nullptr;
}
