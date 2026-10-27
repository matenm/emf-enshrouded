// put_items: the internal emf_inventory program running the game's inventory transaction (fake engine functions).
#include <doctest.h>

#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "common/offsets.h"
#include "core/inventory.h"
#include "core/system_programs.h"
#include "emf/generated/components.hpp"
#include "fake_world.h"

using namespace emf;
using namespace emf::core;
using emf::test::EventStore;
using emf::test::FakeProgramCtx;
using emf::test::FakeWorld;
using emf::test::SingletonStore;

namespace {
namespace gen = emf::gen::ecs;

// ---- fake engine transaction ----
struct TxLog {
    std::vector<std::string> calls;
    uint8_t setup[kSetupSize] = {};
    uint32_t owner = 0, item = 0, count = 0;
    uint64_t slot = 99;
    uint32_t remaining = 0;
    uint8_t error = 0;
    bool crash_in_add = false;
    OptU32 level{};
    OptU8 rarity{};
    uint64_t remove_slot = 0;
} g_tx;
void* f_ctor(uint8_t*) {
    g_tx.calls.push_back("ctor");
    return nullptr;
}
void f_init(uint8_t*, const uint8_t* setup) {
    g_tx.calls.push_back("init");
    std::memcpy(g_tx.setup, setup, kSetupSize);
}
TxResult* f_add(TxResult* out, uint8_t*, uint32_t owner, uint64_t slot, uint32_t item, uint32_t count, OptU32* level,
                OptU8* rarity, OptF32*) {
    g_tx.calls.push_back("add");
    g_tx.level = *level;
    g_tx.rarity = *rarity;
    if (g_tx.crash_in_add) *static_cast<volatile int*>(nullptr) = 1;
    g_tx.owner = owner;
    g_tx.slot = slot;
    g_tx.item = item;
    g_tx.count = count;
    std::memset(out, 0, sizeof(*out));
    out->error = g_tx.error;
    out->value = g_tx.remaining;
    return out;
}
TxResult* f_remove(TxResult* out, uint8_t*, uint32_t owner, uint32_t item, uint64_t slot, uint32_t count) {
    g_tx.calls.push_back("remove");
    g_tx.owner = owner;
    g_tx.item = item;
    g_tx.remove_slot = slot;
    g_tx.count = count;
    std::memset(out, 0, sizeof(*out));
    out->error = g_tx.error;
    out->value = g_tx.remaining;  // count still missing
    return out;
}
void f_commit(uint8_t*) { g_tx.calls.push_back("commit"); }
void f_rollback(uint8_t*) { g_tx.calls.push_back("rollback"); }
void f_free(uint8_t*) { g_tx.calls.push_back("free"); }
InventoryFns fake_fns() {
    InventoryFns f;
    f.ctor = f_ctor;
    f.init = f_init;
    f.add_items = f_add;
    f.remove_items = f_remove;
    f.commit = f_commit;
    f.rollback = f_rollback;
    f.free_arrays = f_free;
    return f;
}

// ---- a fake world: the player (1) with an InventorySetup linking inventory entity 500 ----
constexpr uint64_t kAccFuel = 0xA0, kAccInventory = 0xA1, kAccEmpty = 0xA2, kAccSetup = 0xA3, kAccItemState = 0xA4,
                   kAccOwner = 0xA5, kAccSlotSel = 0xA6;
alignas(8) uint8_t g_balancing[8], g_item_registry[8], g_attributes[8], g_knowledge[8];
gen::InventorySetup g_setup{};
alignas(8) uint8_t g_slot_selection[16], g_inventory[96], g_empty[4];
bool g_gc_disagrees = false;
std::map<std::pair<uint64_t, uint32_t>, void*> g_by_accessor;  // (accessor, entity) -> component
std::map<std::pair<uint32_t, uint32_t>, void*> g_by_hash;      // (hash, entity) -> component

void plant(uint64_t acc, uint32_t hash, uint32_t entity, void* p) {
    g_by_accessor[{acc, entity}] = p;
    g_by_hash[{hash, entity}] = p;
}
void* f_read_other(void** out, void*, uint64_t acc, uint32_t entity) {
    const auto it = g_by_accessor.find({acc, entity});
    *out = it == g_by_accessor.end() ? nullptr : it->second;
    return out;
}
void* f_get_component(void* out, void*, uint32_t hash, uint32_t entity) {
    auto o = static_cast<uint64_t*>(out);
    o[0] = o[1] = 0;
    const auto it = g_by_hash.find({hash, entity});
    if (it != g_by_hash.end()) {
        o[0] = reinterpret_cast<uint64_t>(g_gc_disagrees && hash == gen::InventorySetup::kHash ? g_empty : it->second);
        o[1] = 8;
    }
    return out;
}

// Iterator: [ctx] [sys-read: BalancingRegistry, g38_items::ItemRegistry, impact::AttributeRegistry]
// [sys-write: KnowledgeTriggerQueue] [other-write sorted: Fuel, Inventory, InventoryEmptyState, InventorySetup,
// ItemState, OwnerRelationship, SlotSelection] [events-out x2 = the job pointer]
bool g_bad_event_slots = false;
void f_iter_begin(void* ctx, void* iter, uint32_t) {
    auto* it = static_cast<uint64_t*>(iter);
    void* job = nullptr;
    std::memcpy(&job, ctx, 8);
    it[0] = reinterpret_cast<uint64_t>(ctx);
    it[1] = reinterpret_cast<uint64_t>(g_balancing);
    it[2] = reinterpret_cast<uint64_t>(g_item_registry);
    it[3] = reinterpret_cast<uint64_t>(g_attributes);
    it[4] = reinterpret_cast<uint64_t>(g_knowledge);
    const uint64_t acc[] = {kAccFuel, kAccInventory, kAccEmpty, kAccSetup, kAccItemState, kAccOwner, kAccSlotSel};
    for (size_t i = 0; i < 7; ++i) it[5 + i] = acc[i];
    it[12] = it[13] = g_bad_event_slots ? 0x1234 : reinterpret_cast<uint64_t>(job);
}
uint64_t f_register(void*, const void*, char) { return 1; }

uint64_t g_now = 1000;
std::vector<std::string> g_log;
bool logged(const std::string& needle) {
    for (const auto& l : g_log)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}
struct Delivered {
    EmfWorld* world;
    uint32_t entity, item, requested, added;
};
std::vector<Delivered> g_done;
void on_done(EmfWorld* w, uint32_t entity, uint32_t item, uint32_t requested, uint32_t added, void* user) {
    g_done.push_back({w, entity, item, requested, added});
    ++*static_cast<int*>(user);
}

struct Fixture {
    FakeWorld w;
    SingletonStore singles;
    EventStore events;
    SystemPrograms programs;
    std::vector<std::function<void(EmfWorld*)>> after;
    InventoryService service;
    FakeProgramCtx ctx;
    int user = 0;
    Fixture()
        : service(deps()), ctx(w.p()) {
        programs.reset();
        g_tx = {};
        g_log.clear();
        g_done.clear();
        g_by_accessor.clear();
        g_by_hash.clear();
        g_gc_disagrees = false;
        g_bad_event_slots = false;
        g_setup = {};
        g_now = 1000;
        g_setup.linksEntities[0].id = 500;
        g_setup.isInitialized = true;
        plant(kAccSetup, gen::InventorySetup::kHash, 1, &g_setup);
        plant(kAccSlotSel, gen::SlotSelection::kHash, 1, g_slot_selection);
        plant(kAccInventory, gen::Inventory::kHash, 500, g_inventory);
        plant(kAccEmpty, gen::InventoryEmptyState::kHash, 500, g_empty);
        singles.plant(w, "g38_items::ItemRegistry", g_item_registry);
        events.attach(w);
        events.plant(w, "InventoryFull", 4, 24);
        events.plant(w, "PickupEvent", 7, 48);
        w.put<uint8_t>(off::kKind, 0);
        void* wctx = &user;  // any non-null world context for readOther
        std::memcpy(ctx.job + off::kJobWorldCtx, &wctx, 8);
        REQUIRE(programs.add(service.spec()));
        REQUIRE(programs.register_into(w.p(), f_register) == 1);
        set_program_iter_begin(f_iter_begin);
    }
    ~Fixture() {
        set_program_iter_begin(nullptr);
        programs.reset();
    }
    InventoryService::Deps deps() {
        InventoryService::Deps d;
        d.fns = fake_fns();
        d.read_other = f_read_other;
        d.get_component = f_get_component;
        d.log = [](const std::string& s) { g_log.push_back(s); };
        d.after_step = [this](std::function<void(EmfWorld*)> fn) { after.push_back(std::move(fn)); };
        d.now_ms = [] { return g_now; };
        return d;
    }
    EmfWorld* world() { return reinterpret_cast<EmfWorld*>(w.p()); }
    bool put(uint32_t entity, uint32_t item, uint32_t count) {
        std::string why;
        return service.request({world(), entity, item, count, on_done, &user}, &why);
    }
    void tick() {
        system_program_desc(0).update(ctx.p());
        auto jobs = std::move(after);
        after.clear();
        for (auto& j : jobs) j(world());
    }
};
uint64_t q(const uint8_t* p, size_t off) {
    uint64_t v = 0;
    std::memcpy(&v, p + off, 8);
    return v;
}
uint32_t d(const uint8_t* p, size_t off) {
    uint32_t v = 0;
    std::memcpy(&v, p + off, 4);
    return v;
}
}  // namespace

TEST_CASE("inventory: the emf_inventory program declares what the transaction needs, accessors in engine order") {
    Fixture f;
    const ProgramDesc& desc = system_program_desc(0);
    CHECK(std::string(desc.name.s, desc.name.n) == "emf_inventory");
    REQUIRE(desc.sysRead.n == 3);
    REQUIRE(desc.sysWrite.n == 1);
    CHECK(std::string(desc.sysWrite.p[0].s, desc.sysWrite.p[0].n) == "KnowledgeTriggerQueue");
    REQUIRE(desc.otherWrite.n == 7);
    CHECK(std::string(desc.otherWrite.p[0].s, desc.otherWrite.p[0].n) == "Fuel");
    CHECK(std::string(desc.otherWrite.p[6].s, desc.otherWrite.p[6].n) == "SlotSelection");
    REQUIRE(desc.eventsOut.n == 2);
    CHECK(desc.otherRead.n == 0);
    CHECK((desc.flags & kFlagClient) == 0);  // server world only
}

TEST_CASE("inventory: a request runs ctor, init, addItems, commit, freeArrays with the documented setup block") {
    Fixture f;
    REQUIRE(f.put(1, 0x11, 5));
    CHECK(f.service.pending() == 1);
    f.tick();
    CHECK(g_tx.calls == std::vector<std::string>{"ctor", "init", "add", "commit", "free"});
    CHECK(g_tx.owner == 1);
    CHECK(g_tx.slot == 0);  // automatic slot
    CHECK(g_tx.item == 0x11);
    CHECK(g_tx.count == 5);
    const uint8_t* s = g_tx.setup;
    CHECK(s[0x00] == kSetupFlags);
    CHECK(s[0x01] == 0);  // own inventory only
    CHECK(q(s, 0x08) == reinterpret_cast<uint64_t>(f.ctx.p()));
    CHECK(q(s, 0x10) == reinterpret_cast<uint64_t>(g_item_registry));
    CHECK(q(s, 0x18) == reinterpret_cast<uint64_t>(g_balancing));
    CHECK(q(s, 0x20) == reinterpret_cast<uint64_t>(g_attributes));
    CHECK(q(s, 0x28) == kAccSetup);
    CHECK(q(s, 0x30) == kAccInventory);
    CHECK(q(s, 0x38) == kAccSlotSel);
    CHECK(q(s, 0x40) == kAccItemState);
    CHECK(q(s, 0x48) == kAccOwner);
    CHECK(q(s, 0x50) == kAccSetup);  // the write accessor is the same handle
    CHECK(q(s, 0x58) == kAccEmpty);
    CHECK(q(s, 0x70) == kAccFuel);
    CHECK(q(s, 0xA0) == reinterpret_cast<uint64_t>(f.ctx.job));  // PickupEvent writer
    CHECK(q(s, 0xA8) == reinterpret_cast<uint64_t>(f.ctx.job));  // InventoryFull writer
    CHECK(q(s, 0xC0) == reinterpret_cast<uint64_t>(g_knowledge));
    CHECK(d(s, 0xC8) == 1);  // operator = the receiving entity
    CHECK(d(s, 0xCC) == 0);
    CHECK(q(s, 0x60) == 0);  // unused fields stay zero
    CHECK(q(s, 0xD8) == 0);
    REQUIRE(g_done.size() == 1);  // delivered after the step
    CHECK(g_done[0].world == f.world());
    CHECK(g_done[0].entity == 1);
    CHECK(g_done[0].item == 0x11);
    CHECK(g_done[0].requested == 5);
    CHECK(g_done[0].added == 5);
    CHECK(f.user == 1);
    CHECK(f.service.pending() == 0);
}

TEST_CASE("inventory: a partial fit commits what fit; nothing fitting or an error rolls back") {
    Fixture f;
    g_tx.remaining = 2;
    REQUIRE(f.put(1, 0x11, 5));
    f.tick();
    CHECK(g_tx.calls == std::vector<std::string>{"ctor", "init", "add", "commit", "free"});
    REQUIRE(g_done.size() == 1);
    CHECK(g_done[0].added == 3);

    g_tx.calls.clear();
    g_tx.remaining = 5;  // nothing fit
    REQUIRE(f.put(1, 0x11, 5));
    f.tick();
    CHECK(g_tx.calls == std::vector<std::string>{"ctor", "init", "add", "rollback", "free"});
    CHECK(g_done.back().added == 0);

    g_tx.calls.clear();
    g_tx.remaining = 0;
    g_tx.error = 0x15;  // unknown item / no initialized InventorySetup
    REQUIRE(f.put(1, 0x11, 5));
    f.tick();
    CHECK(g_tx.calls == std::vector<std::string>{"ctor", "init", "add", "rollback", "free"});
    CHECK(g_done.back().added == 0);
}

TEST_CASE("inventory: an accessor that disagrees with getComponent stops before any transaction") {
    Fixture f;
    g_gc_disagrees = true;
    REQUIRE(f.put(1, 0x11, 5));
    f.tick();
    CHECK(g_tx.calls.empty());
    REQUIRE(g_done.size() == 1);
    CHECK(g_done[0].added == 0);
    CHECK(logged("does not match getComponent"));
}

TEST_CASE("inventory: a target without an inventory, or events-out slots that are not the job, are refused") {
    Fixture f;
    REQUIRE(f.put(77, 0x11, 5));  // entity 77 has no InventorySetup
    f.tick();
    CHECK(g_tx.calls.empty());
    REQUIRE(g_done.size() == 1);
    CHECK(g_done[0].added == 0);
    CHECK(logged("entity 77 has no inventory"));

    g_bad_event_slots = true;
    REQUIRE(f.put(1, 0x11, 5));
    f.tick();
    CHECK(g_tx.calls.empty());
    CHECK(g_done.back().added == 0);
    CHECK(logged("iterator layout"));
}

TEST_CASE("inventory: request validation and the pending limit") {
    Fixture f;
    std::string why;
    CHECK_FALSE(f.service.request({nullptr, 1, 0x11, 5, on_done, &f.user}, &why));
    CHECK_FALSE(f.service.request({f.world(), 0, 0x11, 5, on_done, &f.user}, &why));
    CHECK_FALSE(f.service.request({f.world(), 1, 0x11, 0, on_done, &f.user}, &why));
    CHECK_FALSE(f.service.request({f.world(), 1, 0x11, 65536, on_done, &f.user}, &why));
    CHECK_FALSE(f.service.request({f.world(), 1, 0, 5, on_done, &f.user}, &why));
    for (size_t i = 0; i < kMaxPendingPuts; ++i) REQUIRE(f.put(1, 0x11, 1));
    CHECK_FALSE(f.put(1, 0x11, 1));
    CHECK(f.service.pending() == kMaxPendingPuts);
    f.tick();  // at most kPutsPerTick per tick
    CHECK(f.service.pending() == kMaxPendingPuts - kPutsPerTick);
    CHECK(g_done.size() == kPutsPerTick);
}

TEST_CASE("inventory: a new server world ends pending requests with 0; a request for another world too") {
    Fixture f;
    REQUIRE(f.put(1, 0x11, 5));
    f.service.world_reset();
    CHECK(f.service.pending() == 0);
    for (auto& j : f.after) j(f.world());
    f.after.clear();
    REQUIRE(g_done.size() == 1);
    CHECK(g_done[0].added == 0);

    FakeWorld other;
    std::string why;
    REQUIRE(f.service.request({reinterpret_cast<EmfWorld*>(other.p()), 1, 0x11, 5, on_done, &f.user}, &why));
    f.tick();
    CHECK(g_tx.calls.empty());
    CHECK(g_done.back().added == 0);
}

TEST_CASE("inventory: a faulting game function turns the feature off for the session") {
    Fixture f;
    g_tx.crash_in_add = true;
    REQUIRE(f.put(1, 0x11, 5));
    f.tick();
    CHECK_FALSE(f.service.enabled());
    REQUIRE(g_done.size() == 1);
    CHECK(g_done[0].added == 0);
    CHECK(logged("faulted"));
    std::string why;
    CHECK_FALSE(f.service.request({f.world(), 1, 0x11, 5, on_done, &f.user}, &why));
    CHECK(why.find("off") != std::string::npos);
}

TEST_CASE("inventory: requests end with 0 when emf_inventory has not run in the server world for 2 s") {
    Fixture f;
    REQUIRE(f.put(1, 0x11, 5));
    g_now = 1000;
    f.service.on_server_tick(f.world());  // the first tick seen for this world
    g_now = 2500;
    f.service.on_server_tick(f.world());
    CHECK(f.service.pending() == 1);  // not 2 s yet
    g_now = 3100;
    f.service.on_server_tick(f.world());
    CHECK(f.service.pending() == 0);
    for (auto& j : f.after) j(f.world());
    REQUIRE(g_done.size() == 1);
    CHECK(g_done[0].added == 0);
    CHECK(logged("emf_inventory program is not running"));
}

TEST_CASE("inventory: a program that runs keeps its waiting requests") {
    Fixture f;
    for (int i = 0; i < 20; ++i) REQUIRE(f.put(1, 0x11, 1));
    g_now = 1000;
    f.service.on_server_tick(f.world());
    g_now = 4000;
    f.tick();  // runs: 8 done, 12 waiting
    f.service.on_server_tick(f.world());
    CHECK(f.service.pending() == 12);
    CHECK_FALSE(logged("emf_inventory program is not running"));
}

namespace {
struct SeenReason {
    InventoryService* service;
    std::string why;
    uint32_t added = 99;
};
void on_done_reason(EmfWorld*, uint32_t, uint32_t, uint32_t, uint32_t added, void* user) {
    auto* s = static_cast<SeenReason*>(user);
    s->why = s->service->delivering_reason();
    s->added = added;
}
size_t log_count(const std::string& needle) {
    size_t n = 0;
    for (const auto& l : g_log) n += l.find(needle) != std::string::npos;
    return n;
}
}  // namespace

TEST_CASE("inventory: the reason for a short result can be read while it is delivered") {
    Fixture f;
    SeenReason seen{&f.service};
    std::string why;
    REQUIRE(f.service.request({f.world(), 77, 0x11, 5, on_done_reason, &seen}, &why));  // 77 has no inventory
    f.tick();
    CHECK(seen.added == 0);
    CHECK(seen.why.find("entity 77 has no inventory") != std::string::npos);
    CHECK(f.service.delivering_reason().empty());  // only while the callback runs
    SeenReason ok{&f.service};
    REQUIRE(f.service.request({f.world(), 1, 0x11, 5, on_done_reason, &ok}, &why));
    f.tick();
    CHECK(ok.added == 5);
    CHECK(ok.why.empty());
}

TEST_CASE("inventory: a refusal repeated within 10 s is logged once, then again with a count") {
    Fixture f;
    REQUIRE(f.put(77, 0x11, 5));
    f.tick();
    REQUIRE(f.put(77, 0x11, 5));
    f.tick();
    CHECK(log_count("entity 77 has no inventory") == 1);
    g_now = 12000;
    REQUIRE(f.put(77, 0x11, 5));
    f.tick();
    CHECK(log_count("entity 77 has no inventory") == 2);
    CHECK(logged("(1 more since the last one)"));
}

TEST_CASE("inventory: the accessor self-check passing is logged once per world") {
    Fixture f;
    REQUIRE(f.put(1, 0x11, 5));
    f.tick();
    REQUIRE(f.put(1, 0x11, 5));
    f.tick();
    CHECK(log_count("put_items: inventory accessors checked") == 1);
}

TEST_CASE("inventory: the program's iterator layout comes from its own singleton and accessor lists") {
    Fixture f;
    const auto spec = f.service.spec();
    const auto l = f.service.layout();
    CHECK(l.other_base == 1 + spec.singletons_read.size() + spec.singletons_write.size());
    CHECK(l.events_in_base == l.other_base + spec.other_read.size() + spec.other_write.size());
}

TEST_CASE("inventory: a partial fit's reason reads as one plain sentence") {
    Fixture f;
    g_tx.remaining = 2;  // 3 of 5 fit
    SeenReason seen{&f.service};
    std::string why;
    REQUIRE(f.service.request({f.world(), 1, 0x11, 5, on_done_reason, &seen}, &why));
    f.tick();
    CHECK(seen.added == 3);
    CHECK(seen.why == "only 3 of 5 fit: no room for the rest");
    g_tx.remaining = 5;  // nothing fits
    SeenReason none{&f.service};
    REQUIRE(f.service.request({f.world(), 1, 0x11, 5, on_done_reason, &none}, &why));
    f.tick();
    CHECK(none.added == 0);
    CHECK(none.why == "none of 5 fit: no room");
}

TEST_CASE("inventory: put passes an item level and rarity to addItems when asked (gear copies)") {
    Fixture f;
    std::string why;
    PutRequest r{f.world(), 1, 0x11, 1, on_done, &f.user};
    r.level = 12;
    r.rarity = 3;
    REQUIRE(f.service.request(r, &why));
    f.tick();
    CHECK(g_tx.level.has == 1);
    CHECK(g_tx.level.value == 12);
    CHECK(g_tx.rarity.has == 1);
    CHECK(g_tx.rarity.value == 3);
    REQUIRE(f.put(1, 0x11, 1));  // a plain put leaves both to the game
    f.tick();
    CHECK(g_tx.level.has == 0);
    CHECK(g_tx.rarity.has == 0);
}

TEST_CASE("inventory: a take removes from the preferred slot and commits only when all of it was there") {
    Fixture f;
    std::string why;
    PutRequest r{f.world(), 1, 0x11, 2, on_done, &f.user};
    r.take = true;
    r.slot = (uint64_t(3) << 32) | 500;  // inventory entity 500, slot 3
    REQUIRE(f.service.request(r, &why));
    f.tick();
    CHECK(g_tx.calls == std::vector<std::string>{"ctor", "init", "remove", "commit", "free"});
    CHECK(g_tx.owner == 1);
    CHECK(g_tx.item == 0x11);
    CHECK(g_tx.remove_slot == r.slot);
    CHECK(g_tx.count == 2);
    REQUIRE(g_done.size() == 1);
    CHECK(g_done[0].added == 2);  // taken

    g_tx.calls.clear();
    g_tx.remaining = 1;  // only one of the two was there: nothing is taken
    SeenReason seen{&f.service};
    PutRequest r2 = r;
    r2.cb = on_done_reason;
    r2.user = &seen;
    REQUIRE(f.service.request(r2, &why));
    f.tick();
    CHECK(g_tx.calls == std::vector<std::string>{"ctor", "init", "remove", "rollback", "free"});
    CHECK(seen.added == 0);
    CHECK(seen.why.find("does not hold") != std::string::npos);

    g_tx.calls.clear();
    g_tx.remaining = 0;
    g_tx.error = 0x16;  // no InventorySetup
    REQUIRE(f.service.request(r, &why));
    f.tick();
    CHECK(g_tx.calls == std::vector<std::string>{"ctor", "init", "remove", "rollback", "free"});
    CHECK(g_done.back().added == 0);
}

TEST_CASE("inventory: a take is refused on a game build without the removeItems signature") {
    InventoryService::Deps d;
    d.fns = fake_fns();
    d.fns.remove_items = nullptr;
    InventoryService s(std::move(d));
    std::string why;
    PutRequest r{reinterpret_cast<EmfWorld*>(0x10), 1, 0x11, 1, nullptr, nullptr};
    r.take = true;
    CHECK_FALSE(s.request(r, &why));
    CHECK(why.find("not available") != std::string::npos);
    r.take = false;
    CHECK(s.request(r, &why));  // putting still works
}

TEST_CASE("inventory: a take's slot must be an InventorySlotId (index 0-7) of one of the entity's own inventories") {
    Fixture f;
    std::string why;
    PutRequest r{f.world(), 1, 0x11, 1, on_done, &f.user};
    r.take = true;
    r.slot = (uint64_t(8) << 32) | 500;  // index 8: past the 8 slots
    CHECK_FALSE(f.service.request(r, &why));
    CHECK(why.find("slot") != std::string::npos);
    r.slot = (uint64_t(1) << 40) | 500;  // bits above the index
    CHECK_FALSE(f.service.request(r, &why));
    r.slot = uint64_t(3) << 32;  // no inventory entity
    CHECK_FALSE(f.service.request(r, &why));
    r.slot = 0;
    r.exact_slot = true;  // exact needs a slot
    CHECK_FALSE(f.service.request(r, &why));

    r.exact_slot = false;
    r.slot = (uint64_t(3) << 32) | 777;  // a well-formed slot of an inventory entity 1 does not own
    SeenReason seen{&f.service};
    r.cb = on_done_reason;
    r.user = &seen;
    REQUIRE(f.service.request(r, &why));
    f.tick();
    CHECK(g_tx.calls.empty());  // no transaction
    CHECK(seen.added == 0);
    CHECK(seen.why.find("not one of entity 1's inventories") != std::string::npos);
}

TEST_CASE("inventory: an exact-slot take runs only while that slot holds the item") {
    Fixture f;
    std::string why;
    auto* inv = reinterpret_cast<emf::gen::ecs::Inventory*>(g_inventory);
    inv->slots[3].id.value = 0x22;  // something else is there now
    PutRequest r{f.world(), 1, 0x11, 1, on_done_reason, nullptr};
    SeenReason seen{&f.service};
    r.user = &seen;
    r.take = true;
    r.exact_slot = true;
    r.slot = (uint64_t(3) << 32) | 500;
    REQUIRE(f.service.request(r, &why));
    f.tick();
    CHECK(g_tx.calls.empty());
    CHECK(seen.added == 0);
    CHECK(seen.why.find("slot 3 of inventory 500 does not hold the item") != std::string::npos);

    inv->slots[3].id.value = 0x11;
    REQUIRE(f.service.request(r, &why));
    f.tick();
    CHECK(g_tx.calls == std::vector<std::string>{"ctor", "init", "remove", "commit", "free"});
    CHECK(g_tx.remove_slot == r.slot);
    inv->slots[3].id.value = 0;
}

TEST_CASE("inventory: the accessor check is logged again for a new session's world, even at the same address") {
    Fixture f;
    auto count = [] {
        size_t n = 0;
        for (const auto& l : g_log) n += l.find("inventory accessors checked") != std::string::npos;
        return n;
    };
    REQUIRE(f.put(1, 0x11, 1));
    f.tick();
    CHECK(count() == 1);
    REQUIRE(f.put(1, 0x11, 1));
    f.tick();
    CHECK(count() == 1);  // once per world
    f.service.world_reset();  // a reload: often the same world address
    REQUIRE(f.put(1, 0x11, 1));
    f.tick();
    CHECK(count() == 2);
}
