#pragma once
// Attributes (current and max health, stamina, mana) through the game's AttributeRegistry, and damage through a
// pending hit that the game's impact_hit_conclusion applies (docs/engine_parts/re_attributes_menu.md, Part 1).
// Everything here takes the game's functions as pointers, so the tests run it against fakes.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace emf::core {

// Attribute ids (immediates in the game's code, build 1273043).
constexpr uint32_t kAttrHealth = 0x8EB84995u;     // current health (Health component)
constexpr uint32_t kAttrMaxHealth = 0xDC8358BEu;  // maximum health, computed by the game from its formula
constexpr uint32_t kAttrStamina = 0x04B6AA8Bu;    // current stamina (Stamina component)
constexpr uint32_t kAttrMana = 0x6AA2574Eu;       // current mana (Mana component)
constexpr uint32_t kAttrMaxStamina = 0xF443C410u;  // max stamina: network_stamina_attribute copies it to staminaMax
constexpr uint32_t kAttrMaxMana = 0x17536777u;     // max mana: read as mana's pair by three game functions (Medium)
// The maximum of a current-value attribute (health, stamina, mana), or nothing.
std::optional<uint32_t> max_attribute_of(uint32_t id);
// Whether the layouts these functions rely on (AttrInfo's slot, attrAdd, the HitCandidate) were checked for this known
// build (its index in known_builds()): their signatures' RVAs are recorded for it. Unknown builds and builds whose exe
// was not at hand (kRvaUnchecked) are not: a shifted offset must never turn a damage call into something else.
bool attribute_layouts_checked(int build);

// Components that are an AttributeComponent header (u32 rootId, u16 storageOffset, u8 flags (bit 0 = initialised),
// u8 storageCount) followed by int32 storage: where the attributes above live. An attribute belongs to the component
// whose rootId equals its AttrInfo.rootId.
struct AttrComponent {
    uint32_t hash;  // types.json qualifiedHash
    const char* name;
};
const std::vector<AttrComponent>& attribute_components();  // Health, Stamina, Mana

// attrFind's result: err 0 = found (0x1F = no such id), info = the registry's 32-byte entry (+0 rootId, +0xC u16 slot).
struct AttrFindResult {
    uint8_t err;
    uint8_t pad[3];
    uint32_t zero;
    const uint8_t* info;
};
static_assert(sizeof(AttrFindResult) == 16, "attrFind writes 16 bytes");
using AttrFindFn = AttrFindResult* (*)(AttrFindResult* out, uint32_t attr_id, const void* registry);
// attrAdd(comp, registry, delta, id): slot += delta, then the derived values. No rootId check, no clamp.
using AttrAddFn = void (*)(void* component, const void* registry, int32_t delta, uint32_t attr_id);
// allocPendingHit(ImpactSystem*, candidateType): a zeroed HitCandidate (transform scales 1) on the pending-hit list.
using AllocPendingHitFn = void* (*)(void* impact_system, uint8_t candidate_type);

enum class AttrStatus { Ok, NoComponent, NotInitialized, UnknownAttribute, OtherComponent, BadSlot, Fault, NoMaximum };
const char* attr_status_text(AttrStatus s);

// The attribute's root id (AttrInfo.rootId): the rootId of the component that holds it.
AttrStatus attr_root(AttrFindFn find, const void* registry, uint32_t id, uint32_t* root);
// Reads attribute `id` from `comp` (an attribute component of comp_size bytes) the way the game's own
// network_health_attribute does: the initialised bit, attrFind, the rootId check, then the slot (inside the component).
AttrStatus attr_read(AttrFindFn find, const uint8_t* comp, size_t comp_size, const void* registry, uint32_t id,
                     int32_t* out);
// Adds `delta` to attribute `id` of `comp` through attrAdd (which also recomputes the derived values), clamped: damage
// stops at 0, a heal at `max` (neither moves a value that is already past that bound further back). A heal without a
// max is refused (NoMaximum).
// *applied = the change made (0: attrAdd is not called), *now = the value after it. Checks the rootId first (attrAdd
// does not).
AttrStatus attr_add(AttrFindFn find, AttrAddFn add, uint8_t* comp, size_t comp_size, const void* registry, uint32_t id,
                    int32_t delta, std::optional<int32_t> max, int32_t* applied, int32_t* now);

// ---- damage: a pending hit for impact_hit_conclusion ----

// A mod's hit, filled on the server world's thread when it is queued.
struct HitSpec {
    void* world = nullptr;  // the server world it is for
    uint32_t session = 0;   // server_session when it was queued
    uint32_t attacker = 0;  // the root source (kill credit and XP when it is a player); 0 = none
    uint32_t target = 0;
    int32_t amount = 0;     // > 0
    bool has_transform = false;
    uint8_t target_transform[56] = {};  // the target's CurrentTransform (WorldTransform)
    float position[3] = {};   // where it hit, metres
    float direction[3] = {0, 0, 1};  // attacker -> target
    uint8_t faction = 0;      // the target's Faction
    uint8_t reaction = 2;     // impact::HitReactionType (2 = Light)
    uint32_t impact_id = 0;   // the dedupe key (unique per hit)
    uint32_t queued_tick = 0;  // the server world's step when it was queued
};

constexpr size_t kHitCandidateSize = 0x150;
// Writes a mod hit into a HitCandidate the game allocated (zeroed, transform scales 1): true damage (no armour), a
// HitEvent with a Light reaction and aggro, credit to the attacker.
void fill_hit_candidate(uint8_t* hc, const HitSpec& s, int64_t time_ns);

// Hits waiting for the next impact_hit_conclusion of their world. Pushed after a server step, flushed in that program's
// pre-hook (where the program owns the ImpactSystem). Thread-safe.
class DamageQueue {
public:
    static constexpr size_t kMaxWaiting = 64;
    static constexpr uint32_t kMaxAgeTicks = 180;  // server steps a hit may wait (the game's pending list full): dropped

    bool push(const HitSpec& s, std::string* why);
    // Allocates and fills one pending hit per waiting hit for `world` (session `session`, now at server step
    // `now_tick`); hits for an older session or more than kMaxAgeTicks steps old are dropped (logged). A null allocation
    // keeps the rest for the next step. Returns how many were written.
    size_t flush(void* world, uint32_t session, void* impact_system, AllocPendingHitFn alloc, int64_t time_ns,
                 uint32_t now_tick, const std::function<void(const std::string&)>& log);
    uint32_t peek_queued_tick(size_t i) const;  // tests: the queued step of waiting hit i (0 if none)
    void clear();
    size_t waiting() const;
    uint32_t next_impact_id();  // nonzero, unique for a long while

private:
    mutable std::mutex m_;
    std::vector<HitSpec> waiting_;
    std::atomic<uint32_t> next_id_{0};
};

// The one-time check that impact_hit_conclusion's pre-hook runs after clear_hit_candidates in the same step (the RE
// rates that timing Medium; the pre-hook flush works either way, since the program reads the list after it).
class HitOrderCheck {
public:
    void on_clear(uint32_t tick);
    // The line to log, once: "" until there is something to say, and after it was said.
    std::string on_conclusion(uint32_t tick);

private:
    std::atomic<uint32_t> clear_tick_{0};
    std::atomic<bool> clear_seen_{false};
    std::atomic<bool> said_{false};
    std::atomic<uint32_t> conclusions_{0};
};

}  // namespace emf::core
