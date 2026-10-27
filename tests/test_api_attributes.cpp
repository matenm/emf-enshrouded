// API v18: attribute_get, attribute_add, damage and the impact_hit_conclusion pre-hook, through the core state with fake
// game functions (attrFind, attrAdd, allocPendingHit, getComponent) and a fake world.
#include <windows.h>

#include <doctest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/api.h"
#include "core/api_sizes.h"
#include "core/attributes.h"
#include "core/program_registry.h"
#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "fake_world.h"

using namespace emf;
using namespace emf::core;
using emf::test::FakeProgramCtx;
using emf::test::FakeWorld;
using emf::test::SingletonStore;
namespace ecs = emf::gen::ecs;

namespace {

uint64_t g_now = 50000;
std::vector<std::string> g_lines;
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> g_comps;  // (hash, entity) -> data

void* fake_gc(void* out, void*, uint32_t hash, uint32_t entity) {
    auto o = static_cast<uint64_t*>(out);
    o[0] = o[1] = 0;
    auto it = g_comps.find({hash, entity});
    if (it != g_comps.end()) {
        o[0] = reinterpret_cast<uint64_t>(it->second.data());
        o[1] = it->second.size();
    }
    return out;
}

struct Info {
    uint32_t root;
    uint8_t pad[8];
    uint16_t slot;
    uint8_t rest[18];
};
constexpr uint32_t kHealthRoot = 0xAAAA0001u, kStaminaRoot = 0xAAAA0002u;
Info g_info_health{kHealthRoot, {}, 0, {}}, g_info_max{kHealthRoot, {}, 7, {}}, g_info_stamina{kStaminaRoot, {}, 0, {}},
    g_info_max_stamina{kStaminaRoot, {}, 7, {}};
bool g_max_stamina_known = true;
AttrFindResult* fake_find(AttrFindResult* out, uint32_t id, const void*) {
    std::memset(out, 0, sizeof *out);
    const Info* i = id == kAttrHealth ? &g_info_health : id == kAttrMaxHealth ? &g_info_max
                    : id == kAttrStamina ? &g_info_stamina
                    : id == kAttrMaxStamina && g_max_stamina_known ? &g_info_max_stamina : nullptr;
    if (!i) out->err = 0x1F;
    else out->info = reinterpret_cast<const uint8_t*>(i);
    return out;
}
int g_add_calls = 0;
void fake_add(void* comp, const void*, int32_t delta, uint32_t id) {
    ++g_add_calls;
    auto* h = static_cast<ecs::Health*>(comp);
    h->dataStorage[id == kAttrMaxHealth || id == kAttrMaxStamina ? 7 : 0] += delta;
}
std::vector<std::vector<uint8_t>> g_pending;
void* fake_alloc(void*, uint8_t) {
    g_pending.push_back(std::vector<uint8_t>(kHitCandidateSize, uint8_t{0}));
    return g_pending.back().data();
}

void put_health(uint32_t entity, uint32_t root, int32_t cur, int32_t max) {
    ecs::Health h{};
    h.rootId.value = root;
    h.storageOffset = 8;
    h.flags = 1;
    h.storageCount = 8;
    h.dataStorage[0] = cur;
    h.dataStorage[7] = max;
    g_comps[{ecs::Health::kHash, entity}] = std::vector<uint8_t>(reinterpret_cast<uint8_t*>(&h),
                                                                 reinterpret_cast<uint8_t*>(&h) + sizeof h);
}
int32_t health_of(uint32_t entity) {
    ecs::Health h{};
    std::memcpy(&h, g_comps[{ecs::Health::kHash, entity}].data(), sizeof h);
    return h.dataStorage[0];
}
void put_combat(uint32_t entity, bool handle_hits, bool immune = false) {
    ecs::CombatFlags f{};
    f.handleHits = handle_hits;
    f.immune = immune;
    g_comps[{ecs::CombatFlags::kHash, entity}] = std::vector<uint8_t>(reinterpret_cast<uint8_t*>(&f),
                                                                      reinterpret_cast<uint8_t*>(&f) + sizeof f);
}
void put_position(uint32_t entity, double x, double y, double z) {
    std::vector<uint8_t> t(56, 0);
    const int64_t p[3] = {static_cast<int64_t>(x * 4294967296.0), static_cast<int64_t>(y * 4294967296.0),
                          static_cast<int64_t>(z * 4294967296.0)};
    std::memcpy(t.data(), p, sizeof p);
    const float scale[3] = {1, 1, 1};
    std::memcpy(t.data() + 40, scale, sizeof scale);
    g_comps[{ecs::CurrentTransform::kHash, entity}] = t;
}

struct Fixture {
    CoreState state;
    FakeWorld server;
    SingletonStore ss;
    int registry = 0;
    Fixture(bool with_functions = true) {
        g_lines.clear();
        g_comps.clear();
        g_pending.clear();
        g_add_calls = 0;
        g_max_stamina_known = true;
        state.get_component = fake_gc;
        state.log_line = [](const std::string& s) { g_lines.push_back(s); };
        state.now_ms = [] { return g_now; };
        state.known_build = true;
        if (with_functions) {
            state.attr_find = fake_find;
            state.attr_add = fake_add;
            state.alloc_pending_hit = fake_alloc;
            state.damage_hooked = true;
        }
        ss.plant(server, "impact::AttributeRegistry", &registry);
        set_core_state(&state);
    }
    ~Fixture() { set_core_state(nullptr); }
    EmfWorld* w() { return reinterpret_cast<EmfWorld*>(server.p()); }
    // runs f(world) as a callback after the server world's step (where world calls are allowed)
    template <class F>
    void in_callback(F f) {
        auto* p = new F(std::move(f));
        api_table()->run_on_world(EMF_WORLD_SERVER, [](EmfWorld* w, void* u) {
            auto* fn = static_cast<F*>(u);
            (*fn)(w);
            delete fn;
        }, p);
        step();
    }
    void step() { state.dispatcher.run(w(), EMF_WORLD_SERVER, [](const std::string&) {}); }
};

bool logged(const std::string& needle) {
    for (auto& l : g_lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

TEST_CASE("api v18: the table has attribute_get, attribute_add and damage; EMF_ATTR_* are the game's ids") {
    const EmfApi* api = api_table();
    CHECK(api->version >= 18u);
    CHECK(EMF_API_VERSION >= 18);
    CHECK(api->attribute_get);
    CHECK(api->attribute_add);
    CHECK(api->damage);
    CHECK(EMF_ATTR_HEALTH == kAttrHealth);
    CHECK(EMF_ATTR_MAX_HEALTH == kAttrMaxHealth);
    CHECK(EMF_ATTR_STAMINA == kAttrStamina);
    CHECK(EMF_ATTR_MANA == kAttrMana);
    CHECK(kApiV17Size == offsetof(EmfApi, attribute_get));  // v18 appends after register_command_ex
    CHECK(kApiV18Size == offsetof(EmfApi, chat_tell));  // v19 appends after damage
}

TEST_CASE("attribute_get: current and max health of a creature and of the player, from the registry") {
    Fixture f;
    put_health(500, kHealthRoot, 700, 1498);
    put_health(1, kHealthRoot, 180, 250);
    const EmfApi* api = api_table();
    int32_t v = 0;
    CHECK(api->attribute_get(f.w(), 500, EMF_ATTR_HEALTH, &v));
    CHECK(v == 700);
    CHECK(api->attribute_get(f.w(), 500, EMF_ATTR_MAX_HEALTH, &v));
    CHECK(v == 1498);
    CHECK(api->attribute_get(f.w(), 1, EMF_ATTR_HEALTH, &v));
    CHECK(v == 180);
    v = -1;
    CHECK_FALSE(api->attribute_get(f.w(), 777, EMF_ATTR_HEALTH, &v));  // no Health
    CHECK_FALSE(api->attribute_get(f.w(), 500, EMF_ATTR_STAMINA, &v));  // no Stamina component
    CHECK_FALSE(api->attribute_get(f.w(), 500, 0x123u, &v));            // an id the registry does not know
    CHECK(v == -1);
    CHECK_FALSE(api->attribute_get(f.w(), 500, EMF_ATTR_HEALTH, nullptr));
}

TEST_CASE("attribute_get: Stamina is found by its root id among the attribute components") {
    Fixture f;
    put_health(500, kHealthRoot, 700, 1498);
    put_health(501, kStaminaRoot, 60, 0);
    g_comps[{ecs::Stamina::kHash, 500}] = g_comps[{ecs::Health::kHash, 501}];  // 500's Stamina: root kStaminaRoot
    int32_t v = 0;
    CHECK(api_table()->attribute_get(f.w(), 500, EMF_ATTR_STAMINA, &v));
    CHECK(v == 60);
}

TEST_CASE("attribute_get: server world only; off without the signature (logged once); no registry: false") {
    Fixture f;
    put_health(500, kHealthRoot, 700, 1498);
    FakeWorld client;
    client.put<uint8_t>(off::kKind, 2);
    int32_t v = 0;
    CHECK_FALSE(api_table()->attribute_get(reinterpret_cast<EmfWorld*>(client.p()), 500, EMF_ATTR_HEALTH, &v));
    CHECK_FALSE(logged("server world only"));  // a read on another world is just false, like get_component's NULL
    CHECK_FALSE(api_table()->attribute_add(reinterpret_cast<EmfWorld*>(client.p()), 500, EMF_ATTR_HEALTH, 1));
    CHECK(logged("server world only"));        // a change says why
    FakeWorld bare;  // a server world without the singleton
    CHECK_FALSE(api_table()->attribute_get(reinterpret_cast<EmfWorld*>(bare.p()), 500, EMF_ATTR_HEALTH, &v));
    Fixture off(false);
    put_health(500, kHealthRoot, 700, 1498);
    CHECK_FALSE(api_table()->attribute_get(off.w(), 500, EMF_ATTR_HEALTH, &v));
    CHECK_FALSE(api_table()->attribute_get(off.w(), 500, EMF_ATTR_HEALTH, &v));
    size_t n = 0;
    for (const auto& l : g_lines) n += l.find("AttrFind") != std::string::npos;
    CHECK(n == 1);
}

TEST_CASE("attribute_add: inside a world callback it runs at once, clamped to 0..max for health") {
    Fixture f;
    put_health(500, kHealthRoot, 700, 1498);
    const EmfApi* api = api_table();
    bool heal = false, hurt = false, overheal = false;
    f.in_callback([&](EmfWorld* w) {
        heal = api->attribute_add(w, 500, EMF_ATTR_HEALTH, 100);
        CHECK(health_of(500) == 800);
        overheal = api->attribute_add(w, 500, EMF_ATTR_HEALTH, 100000);
        CHECK(health_of(500) == 1498);
        hurt = api->attribute_add(w, 500, EMF_ATTR_HEALTH, -100000);
        CHECK(health_of(500) == 0);
    });
    CHECK(heal);
    CHECK(overheal);
    CHECK(hurt);
    CHECK(g_add_calls == 3);
}

TEST_CASE("attribute_add: outside a world callback it waits for the server world's next step") {
    Fixture f;
    put_health(500, kHealthRoot, 700, 1498);
    CHECK(api_table()->attribute_add(f.w(), 500, EMF_ATTR_HEALTH, -200));  // queued
    CHECK(health_of(500) == 700);
    f.step();
    CHECK(health_of(500) == 500);
    // a later refusal (here: the entity has no Health) is logged
    CHECK(api_table()->attribute_add(f.w(), 999, EMF_ATTR_HEALTH, -1));
    f.step();
    CHECK(logged("attribute_add"));
    CHECK(logged("999"));
}

TEST_CASE("attribute_add: refused on a client world, without the signatures, for another component's id") {
    Fixture f;
    put_health(500, kHealthRoot, 700, 1498);
    FakeWorld client;
    client.put<uint8_t>(off::kKind, 2);
    CHECK_FALSE(api_table()->attribute_add(reinterpret_cast<EmfWorld*>(client.p()), 500, EMF_ATTR_HEALTH, 5));
    bool other = true;
    f.in_callback([&](EmfWorld* w) { other = api_table()->attribute_add(w, 500, EMF_ATTR_STAMINA, 5); });
    CHECK_FALSE(other);
    CHECK(g_add_calls == 0);
    Fixture off(false);
    put_health(500, kHealthRoot, 700, 1498);
    CHECK_FALSE(api_table()->attribute_add(off.w(), 500, EMF_ATTR_HEALTH, 5));
}

TEST_CASE("damage: queues a pending hit with credit for the attacker; the target needs CombatFlags.handleHits") {
    Fixture f;
    put_health(6808, kHealthRoot, 900, 900);
    put_combat(6808, true);
    put_position(6808, 10, 0, 0);
    put_position(1, 0, 0, 0);
    put_health(42, kHealthRoot, 900, 900);
    put_combat(42, false);  // a prop that takes no hits
    const EmfApi* api = api_table();
    bool ok = false, prop = true, zero = true;
    f.in_callback([&](EmfWorld* w) {
        ok = api->damage(w, 1, 6808, 250);
        prop = api->damage(w, 1, 42, 250);
        zero = api->damage(w, 1, 6808, 0);
    });
    CHECK(ok);
    CHECK_FALSE(prop);
    CHECK(logged("handleHits"));
    CHECK_FALSE(zero);
    CHECK(f.state.damage_queue.waiting() == 1);
    CHECK(health_of(6808) == 900);  // the game applies it, not EMF
}

TEST_CASE("damage: the impact_hit_conclusion pre-hook hands the hit to the game in the server world") {
    Fixture f;
    put_health(6808, kHealthRoot, 900, 900);
    put_combat(6808, true);
    put_position(6808, 10, 2, 0);
    put_position(1, 0, 2, 0);
    g_comps[{ecs::Faction::kHash, 6808}] = std::vector<uint8_t>(1, uint8_t{4});
    f.in_callback([&](EmfWorld* w) { CHECK(api_table()->damage(w, 1, 6808, 250)); });
    REQUIRE(f.state.damage_queue.waiting() == 1);
    // the program runs in another world (the client's copy of the same function): nothing happens there
    FakeWorld client;
    client.put<uint8_t>(off::kKind, 2);
    FakeProgramCtx cctx(client.p());
    damage_pre_conclusion(cctx.p());
    CHECK(g_pending.empty());
    // the server world: the ImpactSystem singleton gets the hit
    SingletonStore impact;  // (one singleton per fake store: the registry is not needed any more)
    int impact_system = 0;
    impact.plant(f.server, "impact::ImpactSystem", &impact_system);
    FakeProgramCtx ctx(f.server.p());
    damage_pre_conclusion(ctx.p());
    REQUIRE(g_pending.size() == 1);
    ecs::HitCandidate c;
    std::memcpy(&c, g_pending[0].data(), sizeof c);
    CHECK(c.targetId.id == 6808u);
    CHECK(c.rootSourceId.id == 1u);
    CHECK(c.healthChange == -250);
    CHECK(c.hitDirection.x == doctest::Approx(1.0f));
    CHECK(c.position.x == doctest::Approx(10.0f));
    CHECK(c.position.y == doctest::Approx(2.0f));
    CHECK(static_cast<uint8_t>(c.targetFaction) == 4u);
    CHECK(c.impactId.id != 0u);
    CHECK(f.state.damage_queue.waiting() == 0);
}

TEST_CASE("damage: from outside a world callback it is checked and queued at the server world's next step") {
    Fixture f;
    put_health(6808, kHealthRoot, 900, 900);
    put_combat(6808, true);
    CHECK(api_table()->damage(f.w(), 0, 6808, 10));  // accepted, not checked yet
    CHECK(f.state.damage_queue.waiting() == 0);
    f.step();
    CHECK(f.state.damage_queue.waiting() == 1);
    CHECK(api_table()->damage(f.w(), 0, 31337, 10));  // no such target: refused at the step, logged
    f.step();
    CHECK(f.state.damage_queue.waiting() == 1);
    CHECK(logged("31337"));
}

TEST_CASE("damage: off without allocPendingHit or the pre-hook (false, logged); client world refused") {
    Fixture f;
    put_health(6808, kHealthRoot, 900, 900);
    put_combat(6808, true);
    f.state.damage_hooked = false;
    bool ok = true;
    f.in_callback([&](EmfWorld* w) { ok = api_table()->damage(w, 1, 6808, 10); });
    CHECK_FALSE(ok);
    CHECK(logged("damage"));
    f.state.damage_hooked = true;
    FakeWorld client;
    client.put<uint8_t>(off::kKind, 2);
    CHECK_FALSE(api_table()->damage(reinterpret_cast<EmfWorld*>(client.p()), 1, 6808, 10));
}

TEST_CASE("the clear_hit_candidates and impact_hit_conclusion pre-hooks log their order once") {
    Fixture f;
    f.server.put<uint32_t>(off::kTick, 777);
    FakeProgramCtx ctx(f.server.p());
    damage_pre_clear(ctx.p());
    damage_pre_conclusion(ctx.p());
    damage_pre_conclusion(ctx.p());
    size_t n = 0;
    for (const auto& l : g_lines) n += l.find("same step (tick 777)") != std::string::npos;
    CHECK(n == 1);
}

// ---- edge cases ----

TEST_CASE("attribute_add: a stamina heal stops at max stamina; without a readable max it is refused (logged)") {
    Fixture f;
    put_health(500, kHealthRoot, 700, 1498);
    put_health(501, kStaminaRoot, 60, 100);
    g_comps[{ecs::Stamina::kHash, 500}] = g_comps[{ecs::Health::kHash, 501}];
    auto stamina = [] {
        ecs::Health h{};
        std::memcpy(&h, g_comps[{ecs::Stamina::kHash, 500}].data(), sizeof h);
        return h.dataStorage[0];
    };
    bool up = false, down = false, blind = true;
    f.in_callback([&](EmfWorld* w) {
        up = api_table()->attribute_add(w, 500, EMF_ATTR_STAMINA, 500);
        down = api_table()->attribute_add(w, 500, EMF_ATTR_STAMINA, -30);
        g_max_stamina_known = false;
        blind = api_table()->attribute_add(w, 500, EMF_ATTR_STAMINA, 5);
    });
    CHECK(up);
    CHECK(down);
    CHECK(stamina() == 70);
    CHECK_FALSE(blind);
    CHECK(logged("maximum"));
}

TEST_CASE("damage: refuses a target with CombatFlags.immune") {
    Fixture f;
    put_health(42, kHealthRoot, 900, 900);
    put_combat(42, true, true);
    bool ok = true;
    f.in_callback([&](EmfWorld* w) { ok = api_table()->damage(w, 1, 42, 10); });
    CHECK_FALSE(ok);
    CHECK(logged("immune"));
    CHECK(f.state.damage_queue.waiting() == 0);
}

TEST_CASE("damage: a queued hit records the server step it was queued in") {
    Fixture f;
    f.server.put<uint32_t>(off::kTick, 4321);
    put_health(6808, kHealthRoot, 900, 900);
    put_combat(6808, true);
    f.in_callback([&](EmfWorld* w) { CHECK(api_table()->damage(w, 1, 6808, 10)); });
    REQUIRE(f.state.damage_queue.waiting() == 1);
    CHECK(f.state.damage_queue.peek_queued_tick(0) == 4321u);
}

TEST_CASE("defer_to_server: a call made outside callbacks runs only on the world and session it was meant for") {
    Fixture f;
    put_health(500, kHealthRoot, 700, 1498);
    CHECK(api_table()->attribute_add(f.w(), 500, EMF_ATTR_HEALTH, -100));  // queued for f's world
    FakeWorld other;  // another server world steps first (the session's world object changed)
    f.state.dispatcher.run(reinterpret_cast<EmfWorld*>(other.p()), EMF_WORLD_SERVER, [](const std::string&) {});
    CHECK(health_of(500) == 700);
    CHECK(logged("gone"));
    g_lines.clear();
    CHECK(api_table()->attribute_add(f.w(), 500, EMF_ATTR_HEALTH, -100));
    ++f.state.server_session;  // a reload that reused the world's address
    f.step();
    CHECK(health_of(500) == 700);
    CHECK(logged("session"));
}

TEST_CASE("hook_program: a program EMF's core holds is refused with the reason") {
    Fixture f;
    f.state.programs = std::make_unique<ProgramRegistry>([](uintptr_t, void*, void**) { return true; });
    f.state.reserved_programs = {"impact_hit_conclusion", "clear_hit_candidates"};
    CHECK_FALSE(api_table()->hook_program("impact_hit_conclusion", [](void*, void*) {}, nullptr, nullptr));
    CHECK(logged("impact_hit_conclusion"));
    CHECK(logged("damage"));
    CHECK(api_table()->hook_program("some_other_program", [](void*, void*) {}, nullptr, nullptr));  // pending: fine
}

TEST_CASE("damage: on only once impact_hit_conclusion's pre-hook is really installed") {
    Fixture f(false);
    f.state.alloc_pending_hit = fake_alloc;
    f.state.attr_find = fake_find;
    f.state.attr_add = fake_add;
    CHECK_FALSE(f.state.damage_hooked);
    note_program_hook("clear_hit_candidates", true);
    CHECK_FALSE(f.state.damage_hooked);
    note_program_hook("impact_hit_conclusion", false);
    CHECK_FALSE(f.state.damage_hooked);
    CHECK(logged("impact_hit_conclusion"));  // a failed install is logged
    note_program_hook("impact_hit_conclusion", true);
    CHECK(f.state.damage_hooked);
}
