#include "attributes.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "common/signatures.h"
#include "emf/generated/components.hpp"

namespace emf::core {

namespace ecs = emf::gen::ecs;

namespace {

// AttributeComponent header (emf/generated/components.hpp AttributeComponent).
constexpr size_t kRootId = 0x0, kStorageOffset = 0x4, kFlags = 0x6, kStorageCount = 0x7, kHeaderSize = 8;
constexpr uint8_t kInitialized = 0x01;
// AttrInfo (the registry's 32-byte entry).
constexpr size_t kInfoRoot = 0x0, kInfoSlot = 0xC;

bool guarded_find(AttrFindFn find, AttrFindResult* out, uint32_t id, const void* reg) {
    __try {
        find(out, id, reg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool guarded_add(AttrAddFn add, void* comp, const void* reg, int32_t delta, uint32_t id) {
    __try {
        add(comp, reg, delta, id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool guarded_copy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* guarded_alloc(AllocPendingHitFn alloc, void* sys, uint8_t type) {
    __try {
        return alloc(sys, type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Finds `id` in the registry: its info's rootId and slot.
AttrStatus lookup(AttrFindFn find, const void* reg, uint32_t id, uint32_t* root, uint16_t* slot) {
    if (!find || !reg) return AttrStatus::Fault;
    AttrFindResult r{};
    if (!guarded_find(find, &r, id, reg)) return AttrStatus::Fault;
    if (r.err || !r.info) return AttrStatus::UnknownAttribute;
    uint8_t info[0x10];
    if (!guarded_copy(info, r.info, sizeof info)) return AttrStatus::Fault;
    std::memcpy(root, info + kInfoRoot, 4);
    std::memcpy(slot, info + kInfoSlot, 2);
    return AttrStatus::Ok;
}

// The checks of the game's recipe, plus the slot inside the component: where the value lives.
AttrStatus locate(AttrFindFn find, const uint8_t* comp, size_t size, const void* reg, uint32_t id, size_t* value_at) {
    if (!comp || size < kHeaderSize) return AttrStatus::NoComponent;
    uint8_t head[kHeaderSize];
    if (!guarded_copy(head, comp, sizeof head)) return AttrStatus::Fault;
    if (!(head[kFlags] & kInitialized)) return AttrStatus::NotInitialized;
    uint32_t root = 0;
    uint16_t slot = 0;
    const AttrStatus s = lookup(find, reg, id, &root, &slot);
    if (s != AttrStatus::Ok) return s;
    uint32_t comp_root = 0;
    std::memcpy(&comp_root, head + kRootId, 4);
    if (root != comp_root) return AttrStatus::OtherComponent;
    uint16_t offset = 0;
    std::memcpy(&offset, head + kStorageOffset, 2);
    const size_t at = static_cast<size_t>(offset) + static_cast<size_t>(slot) * 4;
    if (slot >= head[kStorageCount] || at + 4 > size) return AttrStatus::BadSlot;
    *value_at = at;
    return AttrStatus::Ok;
}

}  // namespace

std::optional<uint32_t> max_attribute_of(uint32_t id) {
    if (id == kAttrHealth) return kAttrMaxHealth;
    if (id == kAttrStamina) return kAttrMaxStamina;
    if (id == kAttrMana) return kAttrMaxMana;
    return std::nullopt;
}

bool attribute_layouts_checked(int build) {
    if (build < 0 || build >= static_cast<int>(kKnownBuildCount)) return false;
    for (const char* name : {"AttrFind", "AttrAdd", "AllocPendingHit"}) {
        const SigDef* d = find_signature(name);
        const uint32_t rva = d ? d->rva[static_cast<size_t>(build)] : 0;
        if (rva == 0 || rva == kRvaUnchecked) return false;
    }
    return true;
}

const std::vector<AttrComponent>& attribute_components() {
    static const std::vector<AttrComponent> k = {
        {ecs::Health::kHash, "Health"}, {ecs::Stamina::kHash, "Stamina"}, {ecs::Mana::kHash, "Mana"}};
    return k;
}

const char* attr_status_text(AttrStatus s) {
    switch (s) {
        case AttrStatus::Ok: return "ok";
        case AttrStatus::NoComponent: return "the entity has no Health, Stamina or Mana component that holds it";
        case AttrStatus::NotInitialized: return "the component is not initialised yet";
        case AttrStatus::UnknownAttribute: return "the game's attribute registry does not know that id";
        case AttrStatus::OtherComponent: return "the attribute belongs to another component";
        case AttrStatus::BadSlot: return "the attribute's slot is outside the component";
        case AttrStatus::Fault: return "reading it faulted";
        case AttrStatus::NoMaximum: return "its maximum could not be read, so a heal cannot be clamped";
    }
    return "?";
}

AttrStatus attr_root(AttrFindFn find, const void* registry, uint32_t id, uint32_t* root) {
    uint16_t slot = 0;
    return lookup(find, registry, id, root, &slot);
}

AttrStatus attr_read(AttrFindFn find, const uint8_t* comp, size_t comp_size, const void* registry, uint32_t id,
                     int32_t* out) {
    size_t at = 0;
    const AttrStatus s = locate(find, comp, comp_size, registry, id, &at);
    if (s != AttrStatus::Ok) return s;
    int32_t v = 0;
    if (!guarded_copy(&v, comp + at, 4)) return AttrStatus::Fault;
    if (out) *out = v;
    return AttrStatus::Ok;
}

AttrStatus attr_add(AttrFindFn find, AttrAddFn add, uint8_t* comp, size_t comp_size, const void* registry, uint32_t id,
                    int32_t delta, std::optional<int32_t> max, int32_t* applied, int32_t* now) {
    if (applied) *applied = 0;
    size_t at = 0;
    const AttrStatus s = locate(find, comp, comp_size, registry, id, &at);
    if (s != AttrStatus::Ok) return s;
    if (!add) return AttrStatus::Fault;
    if (delta > 0 && !max) return AttrStatus::NoMaximum;
    int32_t cur = 0;
    if (!guarded_copy(&cur, comp + at, 4)) return AttrStatus::Fault;
    int64_t want = static_cast<int64_t>(cur) + delta;
    // a heal stops at the max (and never lowers a value that is above it already); damage stops at 0 (and never raises
    // a value below it)
    if (delta > 0 && max) want = (std::min)(want, (std::max)(static_cast<int64_t>(*max), static_cast<int64_t>(cur)));
    if (delta < 0) want = (std::max)(want, (std::min)(int64_t{0}, static_cast<int64_t>(cur)));
    want = (std::min)(want, static_cast<int64_t>(INT32_MAX));
    want = (std::max)(want, static_cast<int64_t>(INT32_MIN));
    const int64_t change = want - cur;
    const int32_t d = static_cast<int32_t>(change);
    if (d != 0 && !guarded_add(add, comp, registry, d, id)) return AttrStatus::Fault;
    if (applied) *applied = d;
    if (now) {
        int32_t after = cur;
        if (!guarded_copy(&after, comp + at, 4)) after = static_cast<int32_t>(want);
        *now = after;
    }
    return AttrStatus::Ok;
}

void fill_hit_candidate(uint8_t* hc, const HitSpec& s, int64_t time_ns) {
    using HC = ecs::HitCandidate;
    static_assert(sizeof(HC) == kHitCandidateSize, "HitCandidate is 0x150 bytes");
    auto put = [hc](size_t off, const void* v, size_t n) { std::memcpy(hc + off, v, n); };
    put(offsetof(HC, timeStamp), &time_ns, 8);
    if (s.has_transform) put(offsetof(HC, targetTransform), s.target_transform, sizeof s.target_transform);
    put(offsetof(HC, position), s.position, 12);
    const float normal[3] = {0.f, 1.f, 0.f};
    put(offsetof(HC, normal), normal, 12);
    put(offsetof(HC, hitDirection), s.direction, 12);
    put(offsetof(HC, rootSourceId), &s.attacker, 4);
    put(offsetof(HC, targetId), &s.target, 4);
    const uint32_t flags = 0;  // the game adds WasKillingBlow (0x2000) itself
    put(offsetof(HC, flags), &flags, 4);
    put(offsetof(HC, rootTargetId), &s.target, 4);
    put(offsetof(HC, sourceId), &s.attacker, 4);
    put(offsetof(HC, permissionSourceId), &s.attacker, 4);
    const int32_t health = -s.amount, zero = 0;
    put(offsetof(HC, healthChange), &health, 4);
    put(offsetof(HC, staminaChange), &zero, 4);
    const float aggro = s.attacker ? 1.f : 0.f;  // no attacker: nobody to turn on
    put(offsetof(HC, aggro), &aggro, 4);
    put(offsetof(HC, impactId), &s.impact_id, 4);
    const uint16_t reaction = s.reaction;
    put(offsetof(HC, hitReaction), &reaction, 2);
    put(offsetof(HC, targetFaction), &s.faction, 1);
    put(offsetof(HC, sourceManaCost), &zero, 4);
    const uint8_t entity = 0, one = 1;
    put(offsetof(HC, candidateType), &entity, 1);  // allocPendingHit's overflow path does not set it
    put(offsetof(HC, sendHitEvent), &one, 1);
    put(offsetof(HC, ignoreConsecutiveHits), &entity, 1);
}

// ---- DamageQueue ----

bool DamageQueue::push(const HitSpec& s, std::string* why) {
    const std::lock_guard<std::mutex> l(m_);
    if (waiting_.size() >= kMaxWaiting) {
        if (why) *why = "64 hits are already waiting for the next step";
        return false;
    }
    waiting_.push_back(s);
    return true;
}

size_t DamageQueue::flush(void* world, uint32_t session, void* impact_system, AllocPendingHitFn alloc, int64_t time_ns,
                          uint32_t now_tick, const std::function<void(const std::string&)>& log) {
    std::vector<HitSpec> mine, dropped;
    {
        const std::lock_guard<std::mutex> l(m_);
        if (waiting_.empty()) return 0;
        for (auto it = waiting_.begin(); it != waiting_.end();) {
            if (it->world != world) {
                ++it;
                continue;
            }
            if (it->session != session || now_tick - it->queued_tick > kMaxAgeTicks)
                dropped.push_back(*it);
            else
                mine.push_back(*it);
            it = waiting_.erase(it);
        }
    }
    for (const auto& d : dropped) {
        char line[200];
        if (d.session != session)
            std::snprintf(line, sizeof line, "damage: the hit on %u (%d) was dropped: it was queued in an earlier session",
                          d.target, d.amount);
        else
            std::snprintf(line, sizeof line,
                          "damage: the hit on %u (%d) was dropped: it waited %u server steps (the game's pending-hit list "
                          "stayed full)",
                          d.target, d.amount, now_tick - d.queued_tick);
        if (log) log(line);
    }
    size_t written = 0;
    for (size_t i = 0; i < mine.size(); ++i) {
        uint8_t* hc = impact_system && alloc ? static_cast<uint8_t*>(guarded_alloc(alloc, impact_system, 0)) : nullptr;
        if (!hc) {  // the game's list is full (or the call failed): the rest waits for the next step
            const std::lock_guard<std::mutex> l(m_);
            waiting_.insert(waiting_.begin(), mine.begin() + static_cast<std::ptrdiff_t>(i), mine.end());
            break;
        }
        fill_hit_candidate(hc, mine[i], time_ns);
        ++written;
    }
    return written;
}

void DamageQueue::clear() {
    const std::lock_guard<std::mutex> l(m_);
    waiting_.clear();
}

uint32_t DamageQueue::peek_queued_tick(size_t i) const {
    const std::lock_guard<std::mutex> l(m_);
    return i < waiting_.size() ? waiting_[i].queued_tick : 0;
}

size_t DamageQueue::waiting() const {
    const std::lock_guard<std::mutex> l(m_);
    return waiting_.size();
}

uint32_t DamageQueue::next_impact_id() {
    // a range of its own, far from the small ids the game's impacts seem to use; 0 is never handed out
    const uint32_t n = (next_id_.fetch_add(1) & 0xFFFFu) + 1u;
    return 0x7E5F0000u | n;
}

// ---- HitOrderCheck ----

void HitOrderCheck::on_clear(uint32_t tick) {
    clear_tick_.store(tick);
    clear_seen_.store(true);
}

std::string HitOrderCheck::on_conclusion(uint32_t tick) {
    if (said_.load()) return {};
    const uint32_t n = conclusions_.fetch_add(1) + 1;
    char line[300];
    if (clear_seen_.load()) {
        const uint32_t c = clear_tick_.load();
        if (said_.exchange(true)) return {};
        if (c == tick)
            std::snprintf(line, sizeof line,
                          "damage: impact_hit_conclusion runs after clear_hit_candidates in the same step (tick %u): hits "
                          "queued in its pre-hook are applied in that step",
                          tick);
        else
            std::snprintf(line, sizeof line,
                          "damage: impact_hit_conclusion (tick %u) ran NOT in the same step as clear_hit_candidates (tick "
                          "%u); hits queued in its pre-hook are still read by it",
                          tick, c);
        return line;
    }
    if (n < 120) return {};
    if (said_.exchange(true)) return {};
    std::snprintf(line, sizeof line,
                  "damage: clear_hit_candidates' pre-hook did not run in %u steps of impact_hit_conclusion: the order was "
                  "not checked",
                  n);
    return line;
}

}  // namespace emf::core
