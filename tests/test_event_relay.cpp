#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/event_relay.h"
#include "core/fault_report.h"

using namespace emf::core;

namespace {
struct Call {
    std::string who;
    EmfWorld* world;
    uint32_t value;
    uint32_t size;
};
std::vector<Call> g_calls;
std::vector<std::string> g_log;
auto logger = [](const std::string& s) { g_log.push_back(s); };

void record(EmfWorld* world, const void* event, uint32_t size, void* user) {
    uint32_t v = 0;
    std::memcpy(&v, static_cast<const uint8_t*>(event) + 8, 4);
    g_calls.push_back({static_cast<const char*>(user), world, v, size});
}
void crash(EmfWorld*, const void*, uint32_t, void*) { *static_cast<volatile int*>(nullptr) = 1; }

struct Ev {
    int64_t stamp = 0;
    uint32_t value = 0;
    uint32_t pad = 0;
};
Ev ev(uint32_t v) { return Ev{0, v, 0}; }

EmfWorld* const kServer = reinterpret_cast<EmfWorld*>(0x1000);
EmfWorld* const kClient = reinterpret_cast<EmfWorld*>(0x2000);

bool logged(const std::string& needle) {
    for (const auto& l : g_log)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

TEST_CASE("event relay: subscribers get the captured events of their kind in capture order") {
    EventRelay r;
    g_calls.clear();
    static char s1[] = "s1", s2[] = "s2", s3[] = "s3";
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "A_Event", 16, record, s1, nullptr));
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "B_Event", 16, record, s2, nullptr));
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "A_Event", 16, record, s3, nullptr));
    CHECK(r.events(EMF_WORLD_SERVER) == std::vector<std::string>{"A_Event", "B_Event"});
    const Ev a1 = ev(1), b1 = ev(2), a2 = ev(3);
    r.capture(kServer, EMF_WORLD_SERVER, "A_Event", &a1, sizeof(Ev));
    r.capture(kServer, EMF_WORLD_SERVER, "B_Event", &b1, sizeof(Ev));
    r.capture(kServer, EMF_WORLD_SERVER, "A_Event", &a2, sizeof(Ev));
    CHECK(r.deliver(kServer, EMF_WORLD_SERVER, logger) == 5);
    REQUIRE(g_calls.size() == 5);
    CHECK(g_calls[0].who == "s1");
    CHECK(g_calls[0].value == 1);
    CHECK(g_calls[1].who == "s3");
    CHECK(g_calls[2].who == "s2");
    CHECK(g_calls[2].value == 2);
    CHECK(g_calls[3].who == "s1");
    CHECK(g_calls[3].value == 3);
    CHECK(g_calls[4].who == "s3");
    CHECK(g_calls[0].world == kServer);
    CHECK(g_calls[0].size == 16);
    g_calls.clear();
    CHECK(r.deliver(kServer, EMF_WORLD_SERVER, logger) == 0);  // delivered events are gone
}

TEST_CASE("event relay: events of one world kind never reach subscribers of another") {
    EventRelay r;
    g_calls.clear();
    static char srv[] = "server", cli[] = "client";
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "A_Event", 16, record, srv, nullptr));
    REQUIRE(r.subscribe(EMF_WORLD_CLIENT, "A_Event", 16, record, cli, nullptr));
    const Ev a = ev(7);
    r.capture(kServer, EMF_WORLD_SERVER, "A_Event", &a, sizeof(Ev));
    CHECK(r.deliver(kClient, EMF_WORLD_CLIENT, logger) == 0);
    CHECK(r.deliver(kServer, EMF_WORLD_SERVER, logger) == 1);
    REQUIRE(g_calls.size() == 1);
    CHECK(g_calls[0].who == "server");
}

TEST_CASE("event relay: a subscriber expecting another size is skipped and logged once") {
    EventRelay r;
    g_calls.clear();
    g_log.clear();
    static char ok[] = "ok", bad[] = "bad";
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "A_Event", 16, record, ok, nullptr));
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "A_Event", 24, record, bad, nullptr));
    const Ev a = ev(1);
    r.capture(kServer, EMF_WORLD_SERVER, "A_Event", &a, sizeof(Ev));
    r.capture(kServer, EMF_WORLD_SERVER, "A_Event", &a, sizeof(Ev));
    CHECK(r.deliver(kServer, EMF_WORLD_SERVER, logger) == 2);
    for (const auto& c : g_calls) CHECK(c.who == "ok");
    size_t mentions = 0;
    for (const auto& l : g_log) mentions += l.find("on_event('A_Event')") != std::string::npos;
    CHECK(mentions == 1);
    CHECK(logged("16 bytes"));
    CHECK(logged("expects 24"));
}

TEST_CASE("event relay: a crashing subscriber is disabled and reported once; the others keep running") {
    EventRelay r;
    g_calls.clear();
    static std::vector<std::string> faults;
    faults.clear();
    set_fault_reporter([](const std::string& s) { faults.push_back(s); });
    static char ok[] = "ok";
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "A_Event", 16, crash, nullptr, nullptr));
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "A_Event", 16, record, ok, nullptr));
    const Ev a = ev(1);
    r.capture(kServer, EMF_WORLD_SERVER, "A_Event", &a, sizeof(Ev));
    r.deliver(kServer, EMF_WORLD_SERVER, logger);
    r.capture(kServer, EMF_WORLD_SERVER, "A_Event", &a, sizeof(Ev));
    r.deliver(kServer, EMF_WORLD_SERVER, logger);
    set_fault_reporter(nullptr);
    CHECK(g_calls.size() == 2);
    REQUIRE(faults.size() == 1);
    CHECK(faults[0].find("A_Event") != std::string::npos);
}

TEST_CASE("event relay: a tick with more events than the buffer holds drops the rest and says so once") {
    EventRelay r(64);  // room for two 16-byte events with their bookkeeping
    g_calls.clear();
    g_log.clear();
    static char s[] = "s";
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "A_Event", 16, record, s, nullptr));
    for (uint32_t i = 0; i < 10; ++i) {
        const Ev a = ev(i);
        r.capture(kServer, EMF_WORLD_SERVER, "A_Event", &a, sizeof(Ev));
    }
    const size_t delivered = r.deliver(kServer, EMF_WORLD_SERVER, logger);
    CHECK(delivered >= 1);
    CHECK(delivered < 10);
    for (size_t i = 0; i < g_calls.size(); ++i) CHECK(g_calls[i].value == i);  // the kept ones are intact, in order
    CHECK(logged("dropped"));
    g_log.clear();
    for (uint32_t i = 0; i < 10; ++i) {
        const Ev a = ev(i);
        r.capture(kServer, EMF_WORLD_SERVER, "A_Event", &a, sizeof(Ev));
    }
    r.deliver(kServer, EMF_WORLD_SERVER, logger);
    CHECK_FALSE(logged("dropped"));  // once per session
}

TEST_CASE("event relay: subscription rules") {
    EventRelay r;
    static char s[] = "s";
    std::string why;
    CHECK_FALSE(r.subscribe(EMF_WORLD_PREDICTION, "A_Event", 16, record, s, &why));
    CHECK_FALSE(r.subscribe(EMF_WORLD_SERVER, "A_Event", 16, nullptr, s, &why));
    CHECK_FALSE(r.subscribe(EMF_WORLD_SERVER, "bad name", 16, record, s, &why));
    CHECK_FALSE(r.subscribe(EMF_WORLD_SERVER, "", 16, record, s, &why));
    for (int i = 0; i < 24; ++i) REQUIRE(r.subscribe(EMF_WORLD_SERVER, "E" + std::to_string(i), 16, record, s, nullptr));
    CHECK_FALSE(r.subscribe(EMF_WORLD_SERVER, "E24", 16, record, s, &why));
    CHECK(why.find("24") != std::string::npos);
    CHECK(r.subscribe(EMF_WORLD_SERVER, "E3", 16, record, s, nullptr));      // an event already relayed is fine
    CHECK(r.subscribe(EMF_WORLD_CLIENT, "E24", 16, record, s, nullptr));     // the limit is per world kind
    r.close();
    CHECK(r.closed());
    CHECK_FALSE(r.subscribe(EMF_WORLD_SERVER, "E1", 16, record, s, &why));
    CHECK(why.find("emf_mod_load") != std::string::npos);
}

TEST_CASE("event relay: unknown worlds deliver nothing; a new world drops what an old one left") {
    EventRelay r;
    g_calls.clear();
    static char s[] = "s";
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "A_Event", 16, record, s, nullptr));
    CHECK(r.deliver(kClient, EMF_WORLD_SERVER, logger) == 0);
    const Ev a = ev(1);
    r.capture(kServer, EMF_WORLD_SERVER, "A_Event", &a, sizeof(Ev));
    r.capture(kServer, EMF_WORLD_SERVER, "Unsubscribed_Event", &a, sizeof(Ev));  // ignored
    r.forget_world(kServer);  // the world was rebuilt (same address, new session)
    CHECK(r.deliver(kServer, EMF_WORLD_SERVER, logger) == 0);
    CHECK(g_calls.empty());
}

// ---- relay programs (task 2) ----
#include "core/system_programs.h"
#include "fake_world.h"

namespace {
using emf::test::EventStore;
using emf::test::FakeProgramCtx;
using emf::test::FakeQueue;
using emf::test::FakeWorld;

uint64_t relay_register(void*, const void*, char) { return 1; }
FakeQueue* g_relay_queue = nullptr;
void relay_iter_begin(void* ctx, void* iter, uint32_t) {
    auto* it = static_cast<uint64_t*>(iter);
    it[0] = reinterpret_cast<uint64_t>(ctx);
    it[1] = reinterpret_cast<uint64_t>(g_relay_queue ? g_relay_queue->q : nullptr);
}
std::string desc_name(const ProgramDesc& d) { return std::string(d.name.s, d.name.n); }
}  // namespace

TEST_CASE("relay programs: one per subscribed kind, with the events that exist in the world; the relay closes") {
    EventRelay r;
    SystemPrograms sp;
    sp.reset();
    g_log.clear();
    static char s[] = "s";
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "A_Event", 16, record, s, nullptr));
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "Missing_Event", 16, record, s, nullptr));
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "B_Event", 16, record, s, nullptr));
    FakeWorld w;
    EventStore ev;
    ev.attach(w);
    ev.plant(w, "A_Event", 2, 16);
    ev.plant(w, "B_Event", 3, 16);
    static RelayProgram storage[2];
    CHECK(add_relay_programs(r, sp, w.p(), storage, logger) == 1);
    CHECK(r.closed());
    REQUIRE(sp.count() == 1);
    const ProgramDesc& d = system_program_desc(0);
    CHECK(desc_name(d) == "emf_event_relay_server");
    REQUIRE(d.eventsIn.n == 2);
    CHECK(std::string(d.eventsIn.p[0].s, d.eventsIn.p[0].n) == "A_Event");
    CHECK(std::string(d.eventsIn.p[1].s, d.eventsIn.p[1].n) == "B_Event");
    CHECK(logged("Missing_Event"));
    CHECK(add_relay_programs(r, sp, w.p(), storage, logger) == 0);  // only once
    sp.reset();
}

TEST_CASE("relay programs: no subscriptions add nothing but still close the relay") {
    EventRelay r;
    SystemPrograms sp;
    sp.reset();
    FakeWorld w;
    static RelayProgram storage[2];
    CHECK(add_relay_programs(r, sp, w.p(), storage, logger) == 0);
    CHECK(sp.count() == 0);
    CHECK(r.closed());
}

TEST_CASE("relay programs: running the program captures this tick's events; deliver hands them over") {
    EventRelay r;
    SystemPrograms sp;
    sp.reset();
    g_calls.clear();
    static char s[] = "s";
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "A_Event", 16, record, s, nullptr));
    FakeWorld w;
    EventStore ev;
    ev.attach(w);
    ev.plant(w, "A_Event", 2, 16);
    static RelayProgram storage[2];
    REQUIRE(add_relay_programs(r, sp, w.p(), storage, logger) == 1);
    REQUIRE(sp.register_into(w.p(), relay_register) == 1);
    FakeQueue queue(1, 3);
    queue.fill(3, 16);
    g_relay_queue = &queue;
    set_program_iter_begin(relay_iter_begin);
    FakeProgramCtx ctx(w.p());
    system_program_desc(0).update(ctx.p());
    auto* world = reinterpret_cast<EmfWorld*>(w.p());
    CHECK(r.deliver(world, EMF_WORLD_SERVER, logger) == 3);
    REQUIRE(g_calls.size() == 3);
    CHECK(g_calls[0].value == 0);
    CHECK(g_calls[2].value == 2);
    CHECK(g_calls[0].world == world);
    set_program_iter_begin(nullptr);
    g_relay_queue = nullptr;
    sp.reset();
}

// ---- edge cases ----
TEST_CASE("event relay: closed as unavailable, subscribe says why (not 'too late')") {
    EventRelay r;
    static char s[] = "s";
    r.close("the IterBegin signature is not usable on this game build");
    std::string why;
    CHECK_FALSE(r.subscribe(EMF_WORLD_SERVER, "A_Event", 16, record, s, &why));
    CHECK(why.find("unavailable") != std::string::npos);
    CHECK(why.find("IterBegin") != std::string::npos);
    CHECK(why.find("emf_mod_load") == std::string::npos);
}

TEST_CASE("event relay: an overflow is logged again in a new session") {
    EventRelay r(64);
    static char s[] = "s";
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "A_Event", 16, record, s, nullptr));
    auto flood = [&r] {
        for (uint32_t i = 0; i < 10; ++i) {
            const Ev a = ev(i);
            r.capture(kServer, EMF_WORLD_SERVER, "A_Event", &a, sizeof(Ev));
        }
    };
    g_calls.clear();
    g_log.clear();
    flood();
    r.deliver(kServer, EMF_WORLD_SERVER, logger);
    CHECK(logged("dropped"));
    g_log.clear();
    r.new_session();
    flood();
    r.deliver(kServer, EMF_WORLD_SERVER, logger);
    CHECK(logged("dropped"));
}

TEST_CASE("event relay: an unreadable event is dropped and the relay keeps working") {
    EventRelay r;
    g_calls.clear();
    static char s[] = "s";
    REQUIRE(r.subscribe(EMF_WORLD_SERVER, "A_Event", 16, record, s, nullptr));
    r.capture(kServer, EMF_WORLD_SERVER, "A_Event", reinterpret_cast<const void*>(0x10), 16);
    const Ev a = ev(5);
    r.capture(kServer, EMF_WORLD_SERVER, "A_Event", &a, sizeof(Ev));
    CHECK(r.deliver(kServer, EMF_WORLD_SERVER, logger) == 1);
    REQUIRE(g_calls.size() == 1);
    CHECK(g_calls[0].value == 5);
}
