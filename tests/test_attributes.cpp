// Attributes through the AttributeRegistry and damage through a pending hit (src/core/attributes.h), against a fake
// registry, a fake Health component and a fake allocPendingHit.
#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "common/signatures.h"
#include "core/attributes.h"
#include "emf/generated/components.hpp"

using namespace emf::core;
namespace ecs = emf::gen::ecs;

namespace {

// The registry's 32-byte entries: +0 rootId, +0xC u16 slot.
struct FakeInfo {
    uint32_t root;
    uint8_t pad[8];
    uint16_t slot;
    uint8_t rest[18];
};
static_assert(sizeof(FakeInfo) == 32);

constexpr uint32_t kHealthRoot = 0x11112222u, kStaminaRoot = 0x33334444u;
FakeInfo g_infos[] = {
    {kHealthRoot, {}, 0, {}},  // current health: slot 0
    {kHealthRoot, {}, 7, {}},  // max health: slot 7
    {kStaminaRoot, {}, 0, {}},  // stamina
    {kHealthRoot, {}, 9, {}},  // an id whose slot is past the storage
};
int g_find_calls = 0;
const void* g_registry_seen = nullptr;

AttrFindResult* fake_find(AttrFindResult* out, uint32_t id, const void* reg) {
    ++g_find_calls;
    g_registry_seen = reg;
    std::memset(out, 0, sizeof *out);
    const FakeInfo* info = nullptr;
    if (id == kAttrHealth) info = &g_infos[0];
    if (id == kAttrMaxHealth) info = &g_infos[1];
    if (id == kAttrStamina) info = &g_infos[2];
    if (id == 0xBADu) info = &g_infos[3];
    if (!info) {
        out->err = 0x1F;
        return out;
    }
    out->info = reinterpret_cast<const uint8_t*>(info);
    return out;
}

struct AddCall {
    void* comp;
    const void* reg;
    int32_t delta;
    uint32_t id;
};
std::vector<AddCall> g_adds;
// as the game's attrAdd: slot += delta (no checks); the tests look at what it was asked to do
void fake_add(void* comp, const void* reg, int32_t delta, uint32_t id) {
    g_adds.push_back({comp, reg, delta, id});
    auto* h = static_cast<ecs::Health*>(comp);
    const uint16_t slot = id == kAttrMaxHealth ? 7 : 0;
    h->dataStorage[slot] += delta;
}

// A mini-boss as seen in game: 1498, 0, 1498, 0, 0, 0, 0, 1498 with slot 0 = current, slot 7 = max.
ecs::Health health(int32_t cur, int32_t max = 1498, uint32_t root = kHealthRoot, uint8_t flags = 1) {
    ecs::Health h{};
    h.rootId.value = root;
    h.storageOffset = 8;
    h.flags = flags;
    h.storageCount = 8;
    h.dataStorage[0] = cur;
    h.dataStorage[2] = 1498;
    h.dataStorage[7] = max;
    return h;
}
const uint8_t* bytes(const ecs::Health& h) { return reinterpret_cast<const uint8_t*>(&h); }
uint8_t* bytes(ecs::Health& h) { return reinterpret_cast<uint8_t*>(&h); }
const int kReg = 0;  // the registry object (its address is all the fake needs)

}  // namespace

TEST_CASE("attributes: Health, Stamina and Mana are the components EMF looks in, by their types.json hash") {
    const auto& c = attribute_components();
    REQUIRE(c.size() == 3);
    CHECK(c[0].hash == ecs::Health::kHash);
    CHECK(c[1].hash == ecs::Stamina::kHash);
    CHECK(c[2].hash == ecs::Mana::kHash);
}

TEST_CASE("attr_read: the registry gives the slot, the component the value (current and max health)") {
    const ecs::Health h = health(700);
    int32_t v = 0;
    g_registry_seen = nullptr;
    CHECK(attr_read(fake_find, bytes(h), sizeof h, &kReg, kAttrHealth, &v) == AttrStatus::Ok);
    CHECK(v == 700);
    CHECK(g_registry_seen == &kReg);
    CHECK(attr_read(fake_find, bytes(h), sizeof h, &kReg, kAttrMaxHealth, &v) == AttrStatus::Ok);
    CHECK(v == 1498);
    uint32_t root = 0;
    CHECK(attr_root(fake_find, &kReg, kAttrStamina, &root) == AttrStatus::Ok);
    CHECK(root == kStaminaRoot);
}

TEST_CASE("attr_read: another component's attribute (rootId mismatch) is refused, as the game's own recipe does") {
    const ecs::Health h = health(700);
    int32_t v = -1;
    CHECK(attr_read(fake_find, bytes(h), sizeof h, &kReg, kAttrStamina, &v) == AttrStatus::OtherComponent);
    CHECK(v == -1);  // untouched
    const ecs::Health other = health(700, 1498, 0xDEADu);
    CHECK(attr_read(fake_find, bytes(other), sizeof other, &kReg, kAttrHealth, &v) == AttrStatus::OtherComponent);
}

TEST_CASE("attr_read: an uninitialised component, an unknown id, a slot past the storage and no component are refused") {
    int32_t v = 0;
    const ecs::Health raw = health(700, 1498, kHealthRoot, 0);  // flags bit 0 clear
    CHECK(attr_read(fake_find, bytes(raw), sizeof raw, &kReg, kAttrHealth, &v) == AttrStatus::NotInitialized);
    const ecs::Health h = health(700);
    CHECK(attr_read(fake_find, bytes(h), sizeof h, &kReg, 0x12345678u, &v) == AttrStatus::UnknownAttribute);
    CHECK(attr_read(fake_find, bytes(h), sizeof h, &kReg, 0xBADu, &v) == AttrStatus::BadSlot);  // slot 9 of 8
    CHECK(attr_read(fake_find, bytes(h), 8 + 4 * 3, &kReg, kAttrMaxHealth, &v) == AttrStatus::BadSlot);  // a short copy
    CHECK(attr_read(fake_find, nullptr, 0, &kReg, kAttrHealth, &v) == AttrStatus::NoComponent);
    CHECK(attr_read(nullptr, bytes(h), sizeof h, &kReg, kAttrHealth, &v) == AttrStatus::Fault);
    CHECK(std::string(attr_status_text(AttrStatus::OtherComponent)).find("another component") != std::string::npos);
}

TEST_CASE("attr_add: heals up to max, damages down to 0, and hands attrAdd the clamped delta") {
    ecs::Health h = health(700);
    g_adds.clear();
    int32_t applied = 0, now = 0;
    CHECK(attr_add(fake_find, fake_add, bytes(h), sizeof h, &kReg, kAttrHealth, 300, 1498, &applied, &now) == AttrStatus::Ok);
    CHECK(applied == 300);
    CHECK(now == 1000);
    REQUIRE(g_adds.size() == 1);
    CHECK(g_adds[0].comp == &h);
    CHECK(g_adds[0].reg == &kReg);
    CHECK(g_adds[0].delta == 300);
    CHECK(g_adds[0].id == kAttrHealth);
    CHECK(h.dataStorage[0] == 1000);
    // a heal past the max stops at it
    CHECK(attr_add(fake_find, fake_add, bytes(h), sizeof h, &kReg, kAttrHealth, 5000, 1498, &applied, &now) == AttrStatus::Ok);
    CHECK(applied == 498);
    CHECK(now == 1498);
    // damage past 0 stops at 0
    CHECK(attr_add(fake_find, fake_add, bytes(h), sizeof h, &kReg, kAttrHealth, -99999, 1498, &applied, &now) == AttrStatus::Ok);
    CHECK(applied == -1498);
    CHECK(now == 0);
    CHECK(h.dataStorage[0] == 0);
    // nothing left to change: attrAdd is not called
    const size_t calls = g_adds.size();
    CHECK(attr_add(fake_find, fake_add, bytes(h), sizeof h, &kReg, kAttrHealth, -5, 1498, &applied, &now) == AttrStatus::Ok);
    CHECK(applied == 0);
    CHECK(g_adds.size() == calls);
    // a heal stops at the max; without a known max it is refused (stamina and mana as health)
    ecs::Health s = health(50, 0, kStaminaRoot);
    CHECK(attr_add(fake_find, fake_add, bytes(s), sizeof s, &kReg, kAttrStamina, 1000000, std::nullopt, &applied, &now) ==
          AttrStatus::NoMaximum);
    CHECK(s.dataStorage[0] == 50);
    CHECK(attr_add(fake_find, fake_add, bytes(s), sizeof s, &kReg, kAttrStamina, 1000000, 100, &applied, &now) ==
          AttrStatus::Ok);
    CHECK(now == 100);
    CHECK(attr_add(fake_find, fake_add, bytes(s), sizeof s, &kReg, kAttrStamina, -30, std::nullopt, &applied, &now) ==
          AttrStatus::Ok);  // damage needs no max
    CHECK(now == 70);
    // no overflow at the edges
    ecs::Health big = health(2147483000, 2147483647);
    CHECK(attr_add(fake_find, fake_add, bytes(big), sizeof big, &kReg, kAttrHealth, 2147483647, 2147483647, &applied,
                   &now) == AttrStatus::Ok);
    CHECK(now == 2147483647);
    ecs::Health low = health(100, 200);
    CHECK(attr_add(fake_find, fake_add, bytes(low), sizeof low, &kReg, kAttrHealth, INT32_MIN, 200, &applied, &now) ==
          AttrStatus::Ok);  // emf::kill's delta
    CHECK(now == 0);
    CHECK(applied == -100);
}

TEST_CASE("attr_add: the rootId is checked first (attrAdd does not), so another component is never written") {
    ecs::Health h = health(700);
    g_adds.clear();
    int32_t applied = 9, now = 9;
    CHECK(attr_add(fake_find, fake_add, bytes(h), sizeof h, &kReg, kAttrStamina, -10, std::nullopt, &applied, &now) ==
          AttrStatus::OtherComponent);
    CHECK(g_adds.empty());
    CHECK(h.dataStorage[0] == 700);
    ecs::Health raw = health(700, 1498, kHealthRoot, 0);
    CHECK(attr_add(fake_find, fake_add, bytes(raw), sizeof raw, &kReg, kAttrHealth, -10, 1498, &applied, &now) ==
          AttrStatus::NotInitialized);
    CHECK(attr_add(fake_find, nullptr, bytes(h), sizeof h, &kReg, kAttrHealth, -10, 1498, &applied, &now) == AttrStatus::Fault);
    CHECK(g_adds.empty());
}

TEST_CASE("fill_hit_candidate: a true-damage hit from the attacker with credit, a reaction and a HitEvent") {
    alignas(8) uint8_t hc[kHitCandidateSize] = {};
    HitSpec s;
    s.attacker = 1;
    s.target = 6808;
    s.amount = 250;
    s.has_transform = true;
    for (int i = 0; i < 56; ++i) s.target_transform[i] = static_cast<uint8_t>(i + 1);
    s.position[0] = 10.f;
    s.position[1] = 20.f;
    s.position[2] = 30.f;
    s.direction[0] = 1.f;
    s.direction[1] = 0.f;
    s.direction[2] = 0.f;
    s.faction = 4;
    s.impact_id = 0x7E5F0001u;
    fill_hit_candidate(hc, s, 123456789);
    ecs::HitCandidate c;
    std::memcpy(&c, hc, sizeof c);
    CHECK(c.timeStamp.value == 123456789);
    CHECK(std::memcmp(&c.targetTransform, s.target_transform, 56) == 0);
    CHECK(c.position.x == 10.f);
    CHECK(c.position.z == 30.f);
    CHECK(c.normal.y == 1.f);
    CHECK(c.hitDirection.x == 1.f);
    CHECK(c.rootSourceId.id == 1u);         // kill credit and XP go to the root source
    CHECK(c.targetId.id == 6808u);
    CHECK(c.rootTargetId.id == 6808u);
    CHECK(c.sourceId.id == 1u);
    CHECK(c.permissionSourceId.id == 1u);
    CHECK(c.flags == 0u);                   // not healing; the game adds WasKillingBlow itself
    CHECK(c.healthChange == -250);
    CHECK(c.staminaChange == 0);
    CHECK(c.aggro == 1.f);
    CHECK(c.impactId.id == 0x7E5F0001u);
    CHECK(static_cast<uint16_t>(c.hitReaction) == 2u);  // Light
    CHECK(static_cast<uint8_t>(c.targetFaction) == 4u);
    CHECK(c.sourceManaCost == 0);
    CHECK(static_cast<uint8_t>(c.candidateType) == 0u);  // Entity
    CHECK(c.sendHitEvent);
    CHECK_FALSE(c.ignoreConsecutiveHits);
}

TEST_CASE("fill_hit_candidate: without a transform the game's own one (scale 1) stays; other fields are left alone") {
    alignas(8) uint8_t hc[kHitCandidateSize];
    std::memset(hc, 0xAB, sizeof hc);
    HitSpec s;
    s.target = 9;
    s.amount = 1;
    fill_hit_candidate(hc, s, 0);
    CHECK(hc[0x08] == 0xAB);   // targetTransform untouched
    CHECK(hc[0x40] == 0xAB);   // volumeMin untouched
    CHECK(hc[0x108] == 0xAB);  // sourceTransform untouched
    ecs::HitCandidate c;
    std::memcpy(&c, hc, sizeof c);
    CHECK(c.rootSourceId.id == 0u);  // no attacker: no credit
    CHECK(c.healthChange == -1);
}

namespace {
struct FakeImpact {
    std::vector<std::vector<uint8_t>> pending;
    size_t room = 100;
    uint8_t type_seen = 0xFF;
};
void* fake_alloc(void* sys, uint8_t type) {
    auto* f = static_cast<FakeImpact*>(sys);
    f->type_seen = type;
    if (f->pending.size() >= f->room) return nullptr;
    f->pending.push_back(std::vector<uint8_t>(kHitCandidateSize, uint8_t{0}));
    return f->pending.back().data();
}
HitSpec spec(void* world, uint32_t target, int32_t amount, uint64_t at_ms, uint32_t session = 1) {
    HitSpec s;
    s.world = world;
    s.session = session;
    s.attacker = 1;
    s.target = target;
    s.amount = amount;
    s.queued_tick = static_cast<uint32_t>(at_ms);
    s.impact_id = target;
    return s;
}
}  // namespace

TEST_CASE("DamageQueue: the pre-hook writes each waiting hit of its world into the game's pending list, once") {
    DamageQueue q;
    int w1 = 0, w2 = 0;
    std::string why;
    REQUIRE(q.push(spec(&w1, 100, 50, 1000), &why));
    REQUIRE(q.push(spec(&w2, 200, 60, 1000), &why));
    REQUIRE(q.push(spec(&w1, 300, 70, 1000), &why));
    CHECK(q.waiting() == 3);
    FakeImpact sys;
    std::vector<std::string> log;
    auto sink = [&](const std::string& s) { log.push_back(s); };
    CHECK(q.flush(&w1, 1, &sys, fake_alloc, 5, 1010, sink) == 2);
    CHECK(sys.type_seen == 0);  // Entity
    REQUIRE(sys.pending.size() == 2);
    ecs::HitCandidate c;
    std::memcpy(&c, sys.pending[0].data(), sizeof c);
    CHECK(c.targetId.id == 100u);
    CHECK(c.healthChange == -50);
    CHECK(c.timeStamp.value == 5);
    std::memcpy(&c, sys.pending[1].data(), sizeof c);
    CHECK(c.targetId.id == 300u);
    CHECK(q.waiting() == 1);  // the other world's hit waits for its own world
    CHECK(q.flush(&w1, 1, &sys, fake_alloc, 5, 1020, sink) == 0);
    CHECK(sys.pending.size() == 2);  // written once only
    CHECK(log.empty());
}

TEST_CASE("DamageQueue: a full pending list keeps the rest for the next step; old hits and old sessions are dropped") {
    DamageQueue q;
    int w = 0;
    std::string why;
    for (uint32_t i = 1; i <= 3; ++i) REQUIRE(q.push(spec(&w, i, 10, 1000), &why));
    FakeImpact sys;
    sys.room = 1;
    std::vector<std::string> log;
    auto sink = [&](const std::string& s) { log.push_back(s); };
    CHECK(q.flush(&w, 1, &sys, fake_alloc, 0, 1000, sink) == 1);
    CHECK(q.waiting() == 2);
    sys.room = 100;
    CHECK(q.flush(&w, 1, &sys, fake_alloc, 0, 1016, sink) == 2);
    CHECK(q.waiting() == 0);
    // too old: impact_hit_conclusion did not run in time (its hook is missing): dropped with a line
    REQUIRE(q.push(spec(&w, 7, 10, 1000), &why));
    CHECK(q.flush(&w, 1, &sys, fake_alloc, 0, 1000 + DamageQueue::kMaxAgeTicks, sink) == 1);  // in time
    REQUIRE(q.push(spec(&w, 7, 10, 1000), &why));
    sys.room = sys.pending.size();  // the game's list stays full
    CHECK(q.flush(&w, 1, &sys, fake_alloc, 0, 1000 + DamageQueue::kMaxAgeTicks + 1, sink) == 0);
    CHECK(q.waiting() == 0);
    REQUIRE(log.size() == 1);
    CHECK(log[0].find("dropped") != std::string::npos);
    CHECK(log[0].find("server steps") != std::string::npos);  // counted in steps, not wall time
    CHECK(log[0].find("hook") == std::string::npos);          // the flush runs in the hook: never blame it
    sys.room = 100;
    // a hit queued in an earlier session (the same world object reused by a reload) never lands in the new one
    REQUIRE(q.push(spec(&w, 8, 10, 1000, 1), &why));
    CHECK(q.flush(&w, 2, &sys, fake_alloc, 0, 1001, sink) == 0);
    CHECK(q.waiting() == 0);
    CHECK(log.size() == 2);
}

TEST_CASE("DamageQueue: at most 64 hits wait; clear empties it; impact ids are nonzero and differ") {
    DamageQueue q;
    int w = 0;
    std::string why;
    for (size_t i = 0; i < DamageQueue::kMaxWaiting; ++i) REQUIRE(q.push(spec(&w, 1, 1, 0), &why));
    CHECK_FALSE(q.push(spec(&w, 1, 1, 0), &why));
    CHECK(why.find("64") != std::string::npos);
    q.clear();
    CHECK(q.waiting() == 0);
    const uint32_t a = q.next_impact_id(), b = q.next_impact_id();
    CHECK(a != 0u);
    CHECK(b != 0u);
    CHECK(a != b);
}

TEST_CASE("HitOrderCheck: says once whether impact_hit_conclusion ran after clear_hit_candidates in the same step") {
    HitOrderCheck same;
    same.on_clear(500);
    const std::string line = same.on_conclusion(500);
    CHECK(line.find("after clear_hit_candidates in the same step") != std::string::npos);
    CHECK(same.on_conclusion(501).empty());  // once
    HitOrderCheck other;
    other.on_clear(499);
    CHECK(other.on_conclusion(500).find("NOT in the same step") != std::string::npos);
    HitOrderCheck never;  // the clear hook never ran: said after a while, not at once
    std::string said;
    for (uint32_t t = 1; t <= 200 && said.empty(); ++t) said = never.on_conclusion(t);
    CHECK(said.find("did not run") != std::string::npos);
}

TEST_CASE("fill_hit_candidate: a hit without an attacker brings no aggro; with one, aggro 1") {
    alignas(8) uint8_t hc[kHitCandidateSize] = {};
    HitSpec s;
    s.target = 9;
    s.amount = 5;
    fill_hit_candidate(hc, s, 0);
    ecs::HitCandidate c;
    std::memcpy(&c, hc, sizeof c);
    CHECK(c.aggro == 0.f);
    s.attacker = 1;
    fill_hit_candidate(hc, s, 0);
    std::memcpy(&c, hc, sizeof c);
    CHECK(c.aggro == 1.f);
}

TEST_CASE("attr_read: the max of current health, stamina and mana by id; other attributes have none") {
    CHECK(max_attribute_of(kAttrHealth) == std::optional<uint32_t>(kAttrMaxHealth));
    CHECK(max_attribute_of(kAttrStamina) == std::optional<uint32_t>(kAttrMaxStamina));
    CHECK(max_attribute_of(kAttrMana) == std::optional<uint32_t>(kAttrMaxMana));
    CHECK(kAttrMaxStamina == 0xF443C410u);  // network_stamina_attribute copies it into NetworkStamina.staminaMax
    CHECK(kAttrMaxMana == 0x17536777u);     // read as mana's pair by three game functions
    CHECK_FALSE(max_attribute_of(kAttrMaxHealth));
    CHECK_FALSE(max_attribute_of(0x12345678u));
}

TEST_CASE("attributes: attributes, attribute_add and damage only on a build whose layouts were checked (1273043 and its server)") {
    CHECK(attribute_layouts_checked(emf::known_build_index(1273043u)));
    CHECK(attribute_layouts_checked(emf::known_build_index(1273043u, true)));
    CHECK_FALSE(attribute_layouts_checked(emf::known_build_index(1269579u)));  // the exe was not at hand: unchecked
    CHECK_FALSE(attribute_layouts_checked(-1));                                // an unknown build (a game update)
    CHECK_FALSE(attribute_layouts_checked(99));
}
