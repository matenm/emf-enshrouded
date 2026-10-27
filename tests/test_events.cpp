#include <doctest.h>

#include <cstring>
#include <memory>
#include <vector>

#include "common/offsets.h"
#include "core/events.h"
#include "fake_world.h"

using namespace emf::core;
using emf::test::FakeWorld;
namespace off = emf::off;

namespace {
struct EventWorld {
    FakeWorld w;
    emf::test::EventStore ev;
    EventWorld() { ev.attach(w); }
    void plant(const char* name, uint16_t index, uint32_t size, uint32_t desc_hash = 0, bool timestamped = true) {
        ev.plant(w, name, index, size, desc_hash, timestamped);
    }
};

using emf::test::FakeQueue;

std::vector<uint32_t> g_seen;
void collect(const void* e, void*) {
    uint32_t v;
    std::memcpy(&v, static_cast<const uint8_t*>(e) + 8, 4);
    g_seen.push_back(v);
}

struct EmitCall {
    void* buffer = nullptr;
    uint32_t hash = 0, size = 0;
    uint16_t id = 0;
    int calls = 0;
} g_emit;
alignas(8) uint8_t g_event_storage[256];
void* fake_emit(void* buffer, uint32_t hash, uint32_t size, uint16_t id) {
    g_emit = {buffer, hash, size, id, g_emit.calls + 1};
    std::memset(g_event_storage, 0, sizeof(g_event_storage));
    return g_event_storage;
}
void* full_emit(void*, uint32_t, uint32_t, uint16_t) { return nullptr; }

struct FakeJob {
    alignas(8) uint8_t j[0x200] = {};
    alignas(8) uint8_t buffer_obj[0x18] = {};
    FakeJob(uint16_t program_id, int64_t tick, uint32_t tick_len) {
        void* b = buffer_obj;
        std::memcpy(j + off::kJobEventBuffer, &b, 8);
        std::memcpy(j + off::kJobProgramId, &program_id, 2);
        std::memcpy(j + off::kJobTick, &tick, 8);
        std::memcpy(j + off::kJobTickLength, &tick_len, 4);
    }
};
}  // namespace

TEST_CASE("event_hash is FNV-1a of keen::ecs:: plus the short name (types.json qualifiedHash)") {
    CHECK(event_hash("StartGuardingEvent") == 0xb096ec86u);
    CHECK(event_hash("EntityDiedEvent") == 0xd8ec982au);
}

TEST_CASE("find_event resolves an event through the world's event map and type table") {
    EventWorld ew;
    ew.plant("StartGuardingEvent", 3, 16);
    auto e = find_event(ew.w.p(), "StartGuardingEvent");
    REQUIRE(e.has_value());
    CHECK(e->hash == 0xb096ec86u);
    CHECK(e->index == 3);
    CHECK(e->size == 16);
    CHECK_FALSE(find_event(ew.w.p(), "NoSuchEvent").has_value());
    CHECK_FALSE(find_event(nullptr, "StartGuardingEvent").has_value());
    CHECK_FALSE(find_event(ew.w.p(), "").has_value());
}

TEST_CASE("find_event refuses an entry whose descriptor names a different type") {
    EventWorld ew;
    ew.plant("StopGuardingEvent", 2, 16, 0x12345678u);
    CHECK_FALSE(find_event(ew.w.p(), "StopGuardingEvent").has_value());
}

TEST_CASE("walk_event_queue visits every event across pages in engine order") {
    FakeQueue fq(2, 70);
    fq.fill(70, 0x20);  // 63 events fit in one page (63 * 0x20 <= 0x7F8)
    g_seen.clear();
    CHECK(walk_event_queue(fq.q, 0x20, collect, nullptr) == 70);
    REQUIRE(g_seen.size() == 70);
    for (uint32_t i = 0; i < 70; ++i) CHECK(g_seen[i] == i);
}

TEST_CASE("walk_event_queue handles an empty queue and stops at a missing page") {
    FakeQueue empty(1, 0);
    g_seen.clear();
    CHECK(walk_event_queue(empty.q, 0x10, collect, nullptr) == 0);
    CHECK(g_seen.empty());

    FakeQueue broken(2, 70);
    broken.fill(63, 0x20);
    broken.link(0, 0);  // the count says 70, but there is no second page
    g_seen.clear();
    CHECK(walk_event_queue(broken.q, 0x20, collect, nullptr) == 63);
    CHECK(walk_event_queue(nullptr, 0x20, collect, nullptr) == 0);
    CHECK(walk_event_queue(broken.q, 0, collect, nullptr) == 0);
}

TEST_CASE("emit_from_job sends through the job's buffer and program id and stamps the time") {
    FakeJob job(77, 1000, 16666667);
    g_emit = {};
    void* e = emit_from_job(fake_emit, job.j, 0xb096ec86u, 16, true);
    REQUIRE(e == g_event_storage);
    CHECK(g_emit.buffer == job.buffer_obj);
    CHECK(g_emit.hash == 0xb096ec86u);
    CHECK(g_emit.size == 16);
    CHECK(g_emit.id == 77);
    int64_t ts = 0;
    std::memcpy(&ts, e, 8);
    CHECK(ts == 1000ll * 16666667ll);
}

TEST_CASE("emit_from_job refuses sizes the engine refuses and reports a full buffer") {
    FakeJob job(1, 1, 1);
    g_emit = {};
    CHECK(emit_from_job(fake_emit, job.j, 1, 256, true) == nullptr);
    CHECK(emit_from_job(fake_emit, job.j, 1, 4, true) == nullptr);  // a timestamped event is at least 8 bytes
    CHECK(emit_from_job(fake_emit, job.j, 1, 0, false) == nullptr);
    CHECK(g_emit.calls == 0);
    CHECK(emit_from_job(full_emit, job.j, 1, 16, true) == nullptr);
    CHECK(emit_from_job(fake_emit, nullptr, 1, 16, true) == nullptr);
    CHECK(emit_from_job(nullptr, job.j, 1, 16, true) == nullptr);
    FakeJob no_buffer(1, 1, 1);
    std::memset(no_buffer.j + off::kJobEventBuffer, 0, 8);
    CHECK(emit_from_job(fake_emit, no_buffer.j, 1, 16, true) == nullptr);
    CHECK(g_emit.calls == 0);
}

TEST_CASE("find_event tells events with a timestamp (GameEvent) from the old ones without") {
    EventWorld ew;
    ew.plant("StartGuardingEvent", 3, 16);
    ew.plant("LoginEvent", 4, 4, 0, false);
    auto a = find_event(ew.w.p(), "StartGuardingEvent");
    auto b = find_event(ew.w.p(), "LoginEvent");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->has_timestamp);
    CHECK_FALSE(b->has_timestamp);
}

TEST_CASE("emit_from_job leaves events without a timestamp untouched and allows small ones") {
    FakeJob job(5, 1000, 16666667);
    g_emit = {};
    void* e = emit_from_job(fake_emit, job.j, 0x4b5ada75u, 8, false);  // RevivedPlayerEvent {target, source}
    REQUIRE(e == g_event_storage);
    int64_t first8 = -1;
    std::memcpy(&first8, e, 8);
    CHECK(first8 == 0);  // zeroed by the engine, not stamped
    CHECK(emit_from_job(fake_emit, job.j, 0x554be244u, 4, false) == g_event_storage);  // LoginEvent, 4 bytes
    CHECK(g_emit.size == 4);
}
