#include <doctest.h>

#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "core/fault_report.h"
#include "core/system_programs.h"
#include "emf/generated/components.hpp"
#include "fake_world.h"

using namespace emf::core;
using emf::test::FakeWorld;

namespace {
void dummy_update(void*) {}
std::vector<const ProgramDesc*> g_registered;
std::vector<void*> g_registered_worlds;
uint64_t fake_register(void* world, const void* desc, char disabled) {
    if (!disabled) {
        g_registered.push_back(static_cast<const ProgramDesc*>(desc));
        g_registered_worlds.push_back(world);
    }
    return 1;
}
int g_calls = 0;
void* g_last_ctx = nullptr;
void counting_cb(void* ctx, void* user) {
    g_calls += *static_cast<int*>(user);
    g_last_ctx = ctx;
}
void crash_cb(void*, void*) { *static_cast<volatile int*>(nullptr) = 1; }
}  // namespace

TEST_CASE("build_system_desc fills exactly the engine fields") {
    static_assert(offsetof(ProgramDesc, update) == 0x10);
    static_assert(offsetof(ProgramDesc, sysRead) == 0x98);
    static_assert(offsetof(ProgramDesc, eventsOut) == 0x170);
    static_assert(offsetof(ProgramDesc, flags) == 0x180);
    ProgramDesc d{};
    static const std::string name = "mymod_tick";
    build_system_desc(d, name, EMF_WORLD_SERVER, dummy_update);
    CHECK(d.name.s == name.c_str());
    CHECK(d.name.n == name.size());
    CHECK(d.update == &dummy_update);
    CHECK(d.flags == (kFlagSystem | kFlagNotPredicted));
    CHECK(d.tickInterval == 1);
    CHECK(d.read.n == 0);
    CHECK(d.eventsIn.n == 0);
    ProgramDesc c{};
    build_system_desc(c, name, EMF_WORLD_CLIENT, dummy_update);
    CHECK(c.flags == (kFlagSystem | kFlagClient));
}

TEST_CASE("queued system programs are registered into each new world") {
    SystemPrograms sp;
    sp.reset();
    g_registered.clear();
    static int one = 1;
    REQUIRE(sp.add("mod_server_tick", EMF_WORLD_SERVER, counting_cb, &one));
    REQUIRE(sp.add("mod_client_tick", EMF_WORLD_CLIENT, counting_cb, &one));
    CHECK(sp.count() == 2);
    FakeWorld a, b;
    CHECK(sp.register_into(a.p(), fake_register) == 2);
    CHECK(sp.register_into(b.p(), fake_register) == 2);
    REQUIRE(g_registered.size() == 4);
    CHECK(std::string(g_registered[0]->name.s, g_registered[0]->name.n) == "mod_server_tick");
}

TEST_CASE("register_into stops when the world's program table is full") {
    SystemPrograms sp;
    sp.reset();
    g_registered.clear();
    static int one = 1;
    REQUIRE(sp.add("a", EMF_WORLD_SERVER, counting_cb, &one));
    FakeWorld w;
    w.put<uint16_t>(kProgramCount, static_cast<uint16_t>(kProgramMax));
    CHECK(sp.register_into(w.p(), fake_register) == 0);
    CHECK(g_registered.empty());
}

TEST_CASE("add rejects bad input, duplicates and a full pool") {
    SystemPrograms sp;
    sp.reset();
    static int one = 1;
    CHECK_FALSE(sp.add("", EMF_WORLD_SERVER, counting_cb, &one));
    CHECK_FALSE(sp.add("x", EMF_WORLD_SERVER, nullptr, &one));
    CHECK_FALSE(sp.add("x", EMF_WORLD_PREDICTION, counting_cb, &one));
    REQUIRE(sp.add("x", EMF_WORLD_SERVER, counting_cb, &one));
    CHECK_FALSE(sp.add("x", EMF_WORLD_SERVER, counting_cb, &one));
    for (size_t i = 1; i < kMaxModPrograms; ++i) CHECK(sp.add("p" + std::to_string(i), EMF_WORLD_SERVER, counting_cb, &one));
    CHECK_FALSE(sp.add("overflow", EMF_WORLD_SERVER, counting_cb, &one));
}

TEST_CASE("the trampoline calls the mod callback with the program context and survives a crash") {
    SystemPrograms sp;
    sp.reset();
    static int three = 3;
    REQUIRE(sp.add("ok", EMF_WORLD_SERVER, counting_cb, &three));
    REQUIRE(sp.add("bad", EMF_WORLD_SERVER, crash_cb, nullptr));
    g_calls = 0;
    system_program_desc(0).update(reinterpret_cast<void*>(0x55));
    CHECK(g_calls == 3);
    CHECK(g_last_ctx == reinterpret_cast<void*>(0x55));
    system_program_desc(1).update(nullptr);
    system_program_desc(1).update(nullptr);
    CHECK(system_program_faulted(1));
}

TEST_CASE("add is refused once registration is closed (a world already registered its programs)") {
    SystemPrograms sp;
    sp.reset();
    static int one = 1;
    CHECK_FALSE(sp.registration_closed());
    sp.close_registration();
    CHECK(sp.registration_closed());
    CHECK_FALSE(sp.add("late", EMF_WORLD_SERVER, counting_cb, &one));
    CHECK(sp.count() == 0);
    sp.reset();
    CHECK(sp.add("early", EMF_WORLD_SERVER, counting_cb, &one));
    sp.reset();
}

TEST_CASE("a faulting mod program is reported once, naming the program and its module") {
    SystemPrograms sp;
    sp.reset();
    static std::vector<std::string> faults;
    faults.clear();
    set_fault_reporter([](const std::string& s) { faults.push_back(s); });
    REQUIRE(sp.add("bad_tick", EMF_WORLD_SERVER, crash_cb, nullptr));
    system_program_desc(0).update(nullptr);
    system_program_desc(0).update(nullptr);
    set_fault_reporter(nullptr);
    REQUIRE(faults.size() == 1);
    CHECK(faults[0].find("bad_tick") != std::string::npos);
    CHECK(faults[0].find("emf_tests.exe") != std::string::npos);
    sp.reset();
}

// ---- programs with events ----
namespace {
using emf::test::EventStore;

ProgramSpec spec(const char* name, std::vector<std::string> in, std::vector<std::string> out,
                 ProgramCallback fn = counting_cb, void* user = nullptr) {
    static int one = 1;
    return ProgramSpec{name, EMF_WORLD_SERVER, fn, user ? user : &one, std::move(in), std::move(out)};
}
std::string list_item(const NameList& l, size_t i) { return std::string(l.p[i].s, l.p[i].n); }

// A ProgramContext {job*, entity index} whose job points at the world's entity storage.
struct FakeCtx {
    alignas(8) uint8_t job[0x200] = {};
    struct {
        void* job;
        uint32_t index, pad;
    } ctx{};
    explicit FakeCtx(uint8_t* world) {
        uint8_t* storage = world + emf::off::kStorage;
        std::memcpy(job + emf::off::kJobStorage, &storage, 8);
        ctx.job = job;
    }
    void* p() { return &ctx; }
};

struct Seen {
    bool had_call = false;
    ProgramCall call{};
    uint64_t iter1 = 0, iter2 = 0;
    std::optional<EventType> a, b, out;
    size_t a_slot = 0, b_slot = 0;
} g_seen_call;
void probing_cb(void*, void*) {
    const ProgramCall* c = current_program_call();
    g_seen_call.had_call = c != nullptr;
    if (!c) return;
    g_seen_call.call = *c;
    if (c->iter) {
        g_seen_call.iter1 = c->iter[1];
        g_seen_call.iter2 = c->iter[2];
    }
    g_seen_call.a = program_event(*c, "A_Event", false, &g_seen_call.a_slot);
    g_seen_call.b = program_event(*c, "B_Event", false, &g_seen_call.b_slot);
    g_seen_call.out = program_event(*c, "C_Event", true, nullptr);
}
int g_iter_begin_calls = 0;
void fake_iter_begin(void* ctx, void* iter, uint32_t) {
    ++g_iter_begin_calls;
    auto* it = static_cast<uint64_t*>(iter);
    it[0] = reinterpret_cast<uint64_t>(ctx);
    it[1] = 0x111;
    it[2] = 0x222;
}
}  // namespace

TEST_CASE("a program spec puts its event names into the engine descriptor") {
    SystemPrograms sp;
    sp.reset();
    REQUIRE(sp.add(spec("guard", {"EntityDiedEvent"}, {"StartGuardingEvent", "StopGuardingEvent"})));
    const ProgramDesc& d = system_program_desc(0);
    REQUIRE(d.eventsIn.n == 1);
    CHECK(list_item(d.eventsIn, 0) == "EntityDiedEvent");
    REQUIRE(d.eventsOut.n == 2);
    CHECK(list_item(d.eventsOut, 0) == "StartGuardingEvent");
    CHECK(list_item(d.eventsOut, 1) == "StopGuardingEvent");
    CHECK(d.flags == (kFlagSystem | kFlagNotPredicted));
    sp.reset();
}

TEST_CASE("add refuses bad event names, duplicates in one list and more than 24 events") {
    SystemPrograms sp;
    sp.reset();
    CHECK_FALSE(sp.add(spec("a", {""}, {})));
    CHECK_FALSE(sp.add(spec("b", {"Bad Name"}, {})));
    CHECK_FALSE(sp.add(spec("c", {}, {std::string(65, 'X')})));
    CHECK_FALSE(sp.add(spec("d", {"HitEvent", "HitEvent"}, {})));
    CHECK(sp.add(spec("e", {"HitEvent"}, {"HitEvent"})));  // in and out may share a name
    std::vector<std::string> many;
    for (int i = 0; i < 25; ++i) many.push_back("E" + std::to_string(i));
    CHECK_FALSE(sp.add(spec("f", many, {})));
    many.pop_back();
    CHECK(sp.add(spec("g", many, {})));
    sp.reset();
}

TEST_CASE("register_into keeps a program with an unknown event out of that world and says why") {
    SystemPrograms sp;
    sp.reset();
    g_registered.clear();
    FakeWorld w;
    EventStore ev;
    ev.attach(w);
    ev.plant(w, "KnownEvent", 1, 16);
    REQUIRE(sp.add(spec("good", {"KnownEvent"}, {})));
    REQUIRE(sp.add(spec("bad", {"KnownEvent"}, {"MissingEvent"})));
    std::vector<std::string> problems;
    CHECK(sp.register_into(w.p(), fake_register, &problems) == 1);
    REQUIRE(g_registered.size() == 1);
    CHECK(std::string(g_registered[0]->name.s, g_registered[0]->name.n) == "good");
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].find("'bad'") != std::string::npos);
    CHECK(problems[0].find("unknown event 'MissingEvent'") != std::string::npos);
    sp.reset();
}

TEST_CASE("register_into refuses to send events of 256 bytes or more") {
    SystemPrograms sp;
    sp.reset();
    g_registered.clear();
    FakeWorld w;
    EventStore ev;
    ev.attach(w);
    ev.plant(w, "HugeEvent", 2, 336);
    REQUIRE(sp.add(spec("reader", {"HugeEvent"}, {})));   // reading a big event is fine
    REQUIRE(sp.add(spec("sender", {}, {"HugeEvent"})));
    std::vector<std::string> problems;
    CHECK(sp.register_into(w.p(), fake_register, &problems) == 1);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].find("'sender'") != std::string::npos);
    CHECK(problems[0].find("336 bytes") != std::string::npos);
    sp.reset();
}

TEST_CASE("inside the mod callback the current program call knows ctx, world and its events") {
    SystemPrograms sp;
    sp.reset();
    g_registered.clear();
    FakeWorld w;
    EventStore ev;
    ev.attach(w);
    ev.plant(w, "B_Event", 5, 16);
    ev.plant(w, "A_Event", 2, 24);
    ev.plant(w, "C_Event", 3, 32);
    REQUIRE(sp.add(spec("probe", {"B_Event", "A_Event"}, {"C_Event"}, probing_cb)));
    REQUIRE(sp.register_into(w.p(), fake_register) == 1);
    set_program_iter_begin(fake_iter_begin);
    g_iter_begin_calls = 0;
    g_seen_call = {};
    FakeCtx ctx(w.p());
    CHECK(current_program_call() == nullptr);
    system_program_desc(0).update(ctx.p());
    CHECK(current_program_call() == nullptr);
    REQUIRE(g_seen_call.had_call);
    CHECK(g_seen_call.call.ctx == ctx.p());
    CHECK(g_seen_call.call.world == w.p());
    CHECK(g_iter_begin_calls == 1);
    CHECK(g_seen_call.iter1 == 0x111);
    // events_in iterator slots follow the event type index (A=2 before B=5), not the list order
    REQUIRE(g_seen_call.a.has_value());
    REQUIRE(g_seen_call.b.has_value());
    CHECK(g_seen_call.a_slot == 1);
    CHECK(g_seen_call.b_slot == 2);
    CHECK(g_seen_call.a->size == 24);
    REQUIRE(g_seen_call.out.has_value());
    CHECK(g_seen_call.out->size == 32);
    set_program_iter_begin(nullptr);
    sp.reset();
}

TEST_CASE("program_event refuses undeclared events and worlds the program was not added to") {
    SystemPrograms sp;
    sp.reset();
    FakeWorld w, other;
    EventStore ev;
    ev.attach(w);
    ev.plant(w, "A_Event", 2, 24);
    ev.plant(w, "C_Event", 3, 32);
    REQUIRE(sp.add(spec("p", {"A_Event"}, {"C_Event"})));
    REQUIRE(sp.register_into(w.p(), fake_register) == 1);
    ProgramCall call{0, nullptr, w.p(), nullptr};
    CHECK(program_event(call, "A_Event", false, nullptr).has_value());
    CHECK_FALSE(program_event(call, "A_Event", true, nullptr).has_value());   // declared as in, not out
    CHECK_FALSE(program_event(call, "C_Event", false, nullptr).has_value());  // declared as out, not in
    CHECK_FALSE(program_event(call, "Z_Event", false, nullptr).has_value());
    ProgramCall elsewhere{0, nullptr, other.p(), nullptr};
    CHECK_FALSE(program_event(elsewhere, "A_Event", false, nullptr).has_value());
    sp.reset();
}

// ---- singletons and other-entity accessors ----
namespace {
ProgramSpec access_spec(const char* name, std::vector<std::string> sread, std::vector<std::string> swrite,
                        std::vector<std::string> oread, std::vector<std::string> owrite,
                        std::vector<std::string> in = {}, ProgramCallback fn = counting_cb) {
    ProgramSpec s = spec(name, std::move(in), {}, fn);
    s.singletons_read = std::move(sread);
    s.singletons_write = std::move(swrite);
    s.other_read = std::move(oread);
    s.other_write = std::move(owrite);
    return s;
}
struct AccessSeen {
    bool iter = false;
    std::optional<size_t> item_registry, knowledge, missing_singleton;
    std::optional<OtherAccess> inventory, slot_selection, player_input, undeclared;
    size_t event_slot = 0;
} g_access;
void access_cb(void*, void*) {
    const ProgramCall* c = current_program_call();
    if (!c) return;
    g_access.iter = c->iter != nullptr;
    g_access.item_registry = program_singleton_slot(*c, "g38_items::ItemRegistry");
    g_access.knowledge = program_singleton_slot(*c, "KnowledgeTriggerQueue");
    g_access.missing_singleton = program_singleton_slot(*c, "NoSuchSingleton");
    g_access.inventory = program_other(*c, "Inventory");
    g_access.slot_selection = program_other(*c, "SlotSelection");
    g_access.player_input = program_other(*c, "PlayerInput");
    g_access.undeclared = program_other(*c, "Health");
    program_event(*c, "A_Event", false, &g_access.event_slot);
}
}  // namespace

TEST_CASE("singletons keep their order; other-entity components go into the descriptor in the engine's order") {
    SystemPrograms sp;
    sp.reset();
    REQUIRE(sp.add(access_spec("inv", {"g38_items::ItemRegistry", "BalancingRegistry"}, {"KnowledgeTriggerQueue"},
                               {"PlayerInput", "CurrentTransform"},
                               {"SlotSelection", "Inventory", "InventoryEmptyState", "DynamicInventoryCraftingStock"})));
    const ProgramDesc& d = system_program_desc(0);
    REQUIRE(d.sysRead.n == 2);
    CHECK(list_item(d.sysRead, 0) == "g38_items::ItemRegistry");
    CHECK(list_item(d.sysRead, 1) == "BalancingRegistry");
    REQUIRE(d.sysWrite.n == 1);
    CHECK(list_item(d.sysWrite, 0) == "KnowledgeTriggerQueue");
    REQUIRE(d.otherRead.n == 2);
    CHECK(list_item(d.otherRead, 0) == "CurrentTransform");
    CHECK(list_item(d.otherRead, 1) == "PlayerInput");
    REQUIRE(d.otherWrite.n == 4);  // a Dynamic half sorts as its base name (re_custom_programs.md 6.1)
    CHECK(list_item(d.otherWrite, 0) == "Inventory");
    CHECK(list_item(d.otherWrite, 1) == "DynamicInventoryCraftingStock");
    CHECK(list_item(d.otherWrite, 2) == "InventoryEmptyState");
    CHECK(list_item(d.otherWrite, 3) == "SlotSelection");
    CHECK(d.otherTpl.n == 0);
    CHECK(d.otherWriteUnser.n == 0);
    sp.reset();
}

TEST_CASE("iterator layout: ctx, singletons, other-entity accessors, then events-in queues") {
    const IterLayout l = iter_layout(3, 6);
    CHECK(l.singleton_base == 1);
    CHECK(l.other_base == 4);
    CHECK(l.events_in_base == 10);
    CHECK(iter_layout(0, 0).events_in_base == 1);
}

TEST_CASE("inside the callback: singleton and accessor slots, events-in shifted past them") {
    SystemPrograms sp;
    sp.reset();
    FakeWorld w;
    EventStore ev;
    ev.attach(w);
    ev.plant(w, "A_Event", 2, 24);
    REQUIRE(sp.add(access_spec("inv", {"g38_items::ItemRegistry", "BalancingRegistry"}, {"KnowledgeTriggerQueue"},
                               {"PlayerInput"}, {"SlotSelection", "Inventory"}, {"A_Event"}, access_cb)));
    REQUIRE(sp.register_into(w.p(), fake_register) == 1);
    set_program_iter_begin(fake_iter_begin);
    g_access = {};
    FakeCtx ctx(w.p());
    system_program_desc(0).update(ctx.p());
    CHECK(g_access.iter);
    CHECK(g_access.item_registry == std::optional<size_t>(1));
    CHECK(g_access.knowledge == std::optional<size_t>(3));  // sys-read list first, then sys-write
    CHECK_FALSE(g_access.missing_singleton.has_value());
    REQUIRE(g_access.player_input.has_value());
    CHECK(g_access.player_input->iter_slot == 4);  // other-read first
    CHECK_FALSE(g_access.player_input->write);
    REQUIRE(g_access.inventory.has_value());
    CHECK(g_access.inventory->iter_slot == 5);  // then other-write, sorted: Inventory, SlotSelection
    CHECK(g_access.inventory->write);
    CHECK(g_access.inventory->hash == emf::gen::ecs::Inventory::kHash);
    REQUIRE(g_access.slot_selection.has_value());
    CHECK(g_access.slot_selection->iter_slot == 6);
    CHECK_FALSE(g_access.undeclared.has_value());
    CHECK(g_access.event_slot == 7);  // 1 + 3 singletons + 3 accessors + rank 0
    set_program_iter_begin(nullptr);
    sp.reset();
}

TEST_CASE("a program with only singletons or accessors still gets an iterator") {
    SystemPrograms sp;
    sp.reset();
    FakeWorld w;
    REQUIRE(sp.add(access_spec("only_singletons", {"g38_items::ItemRegistry"}, {}, {}, {}, {}, access_cb)));
    REQUIRE(sp.register_into(w.p(), fake_register) == 1);
    set_program_iter_begin(fake_iter_begin);
    g_access = {};
    FakeCtx ctx(w.p());
    system_program_desc(0).update(ctx.p());
    CHECK(g_access.iter);
    set_program_iter_begin(nullptr);
    sp.reset();
}

TEST_CASE("add refuses bad singleton names, unknown components, repeats and more than 16 per list") {
    SystemPrograms sp;
    sp.reset();
    CHECK_FALSE(sp.add(access_spec("a", {"Bad Name"}, {}, {}, {})));
    CHECK_FALSE(sp.add(access_spec("b", {""}, {}, {}, {})));
    CHECK_FALSE(sp.add(access_spec("c", {}, {}, {"NoSuchComponent"}, {})));
    CHECK_FALSE(sp.add(access_spec("d", {}, {}, {}, {"Inventory", "Inventory"})));
    CHECK_FALSE(sp.add(access_spec("e", {"X", "X"}, {}, {}, {})));
    std::vector<std::string> many;
    for (int i = 0; i < 17; ++i) many.push_back("S" + std::to_string(i));
    CHECK_FALSE(sp.add(access_spec("f", many, {}, {}, {})));
    many.pop_back();
    CHECK(sp.add(access_spec("g", many, {}, {}, {})));
    CHECK(sp.add(access_spec("h", {"impact::AttributeRegistry"}, {}, {"CurrentTransform"}, {"Inventory"})));
    sp.reset();
}

TEST_CASE("register_into leaves out a program whose other-entity component is not per-entity in that world") {
    SystemPrograms sp;
    sp.reset();
    g_registered.clear();
    FakeWorld w;
    emf::test::FakeRegistry reg;
    reg.attach(w, {emf::gen::ecs::CurrentTransform::kHash});
    REQUIRE(sp.add(access_spec("good", {}, {}, {"CurrentTransform"}, {})));
    REQUIRE(sp.add(access_spec("tpl_only", {}, {}, {"ActiveNpcState"}, {})));  // template-only: the engine would abort
    std::vector<std::string> problems;
    CHECK(sp.register_into(w.p(), fake_register, &problems) == 1);
    REQUIRE(g_registered.size() == 1);
    CHECK(std::string(g_registered[0]->name.s, g_registered[0]->name.n) == "good");
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].find("'tpl_only'") != std::string::npos);
    CHECK(problems[0].find("'ActiveNpcState' is not a per-entity component") != std::string::npos);
    sp.reset();
}

namespace {
void ctx_only_iter_begin(void* ctx, void* iter, uint32_t) { static_cast<uint64_t*>(iter)[0] = reinterpret_cast<uint64_t>(ctx); }
}  // namespace

TEST_CASE("the iterator is zeroed first: a slot the engine did not fill reads as null, not as an old value") {
    SystemPrograms sp;
    sp.reset();
    FakeWorld w;
    EventStore ev;
    ev.attach(w);
    ev.plant(w, "A_Event", 2, 24);
    ev.plant(w, "B_Event", 5, 16);
    ev.plant(w, "C_Event", 3, 32);
    REQUIRE(sp.add(spec("probe", {"B_Event", "A_Event"}, {"C_Event"}, probing_cb)));
    REQUIRE(sp.register_into(w.p(), fake_register) == 1);
    FakeCtx ctx(w.p());
    set_program_iter_begin(fake_iter_begin);  // fills slots 1 and 2 with 0x111 / 0x222
    g_seen_call = {};
    system_program_desc(0).update(ctx.p());
    CHECK(g_seen_call.iter1 == 0x111);
    set_program_iter_begin(ctx_only_iter_begin);  // an engine that left the slots alone
    g_seen_call = {};
    system_program_desc(0).update(ctx.p());
    CHECK(g_seen_call.iter1 == 0);
    CHECK(g_seen_call.iter2 == 0);
    set_program_iter_begin(nullptr);
    sp.reset();
}

TEST_CASE("mods get their 16 programs besides EMF's internal ones (put_items and the event relays)") {
    SystemPrograms sp;
    sp.reset();
    static int one = 1;
    for (int i = 0; i < 3; ++i) {
        ProgramSpec internal{"emf_internal" + std::to_string(i), EMF_WORLD_SERVER, counting_cb, &one, {}, {}};
        REQUIRE(sp.add(internal, true));
    }
    for (size_t i = 0; i < kMaxModPrograms; ++i) CHECK(sp.add("mod" + std::to_string(i), EMF_WORLD_SERVER, counting_cb, &one));
    CHECK_FALSE(sp.add("mod_extra", EMF_WORLD_SERVER, counting_cb, &one));
    CHECK(sp.count() == kMaxModPrograms + 3);
    sp.reset();
}
