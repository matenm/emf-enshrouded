#include "events.h"

#include <windows.h>

#include <cstring>
#include <string>

#include "common/fnv.h"
#include "common/offsets.h"
#include "common/seh.h"
#include "common/world_view.h"

namespace emf::core {
namespace {

template <class T>
bool rd(const void* addr, T& out) {
    return seh_read(addr, &out, sizeof(T));
}

const uint8_t* page_ptr(const uint8_t* base, uint16_t index_1based) {
    return index_1based ? base + size_t(index_1based - 1) * off::kEventPageSize : nullptr;
}

void* call_emit(EmitEventFn fn, void* buffer, uint32_t hash, uint32_t size, uint16_t id, const int64_t* stamp) {
    __try {
        void* e = fn(buffer, hash, size, id);
        if (e && stamp) std::memcpy(e, stamp, sizeof(*stamp));
        return e;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

}  // namespace

uint32_t event_hash(std::string_view short_name) {
    std::string q = "keen::ecs::";
    q.append(short_name);
    return fnv1a32(q);
}

std::optional<EventType> find_event(const uint8_t* world, std::string_view short_name) {
    if (!world || short_name.empty()) return std::nullopt;
    const uint32_t hash = event_hash(short_name);
    const auto index = WorldView(const_cast<uint8_t*>(world)).map_u16(off::kEventMap, hash);
    if (!index) return std::nullopt;
    const uint8_t* table = nullptr;
    const uint8_t* desc = nullptr;
    uint32_t size = 0, desc_hash = 0;
    if (!rd(world + off::kEventTypes, table) || !table) return std::nullopt;
    if (!rd(table + size_t(*index) * off::kEventTypeStride, desc) || !desc) return std::nullopt;
    if (!rd(desc + off::kTypeDescSize, size) || !rd(desc + off::kTypeDescHash, desc_hash)) return std::nullopt;
    if (desc_hash != hash || size == 0) return std::nullopt;
    // Walk the base types: events deriving from GameEvent start with timeStamp, the old ecs::Event ones do not.
    bool stamped = false;
    const uint8_t* base = nullptr;
    for (int depth = 0; depth < 32 && rd(desc + off::kTypeDescBase, base) && base; ++depth, desc = base) {
        uint32_t base_hash = 0;
        if (!rd(base + off::kTypeDescHash, base_hash)) break;
        if (base_hash == kGameEventHash) {
            stamped = true;
            break;
        }
    }
    return EventType{hash, *index, size, stamped};
}

uint32_t walk_event_queue(const void* queue, uint32_t stride, EventVisitor cb, void* user) {
    if (!queue || !cb || stride == 0 || stride > off::kEventPageUsable) return 0;
    const uint8_t* q = static_cast<const uint8_t*>(queue);
    uint32_t count = 0;
    const uint8_t* base = nullptr;
    uint16_t first = 0;
    if (!rd(q + off::kQueueCount, count) || !rd(q + off::kQueuePages, base) || !rd(q + off::kQueueFirstPage, first))
        return 0;
    if (!count || !base) return 0;
    const uint8_t* page = page_ptr(base, first);
    uint32_t offset = 0, pages = 1, visited = 0;
    for (uint32_t i = 0; i < count && page; ++i) {
        if (offset + stride > off::kEventPageUsable) {
            uint16_t next = 0;
            if (!rd(page + off::kEventPageNext, next) || ++pages > off::kMaxEventPages) break;
            page = page_ptr(base, next);
            offset = 0;
            if (!page) break;
        }
        const uint8_t* event = page + off::kEventPageData + offset;
        uint8_t probe = 0;
        if (!rd(event + stride - 1, probe)) break;
        cb(event, user);  // mod code: a fault here reaches the mod program's guard, which disables it
        ++visited;
        offset += stride;
    }
    return visited;
}

void* emit_from_job(EmitEventFn fn, void* job, uint32_t hash, uint32_t size, bool stamp) {
    if (!fn || !job || size == 0 || size >= 0x100 || (stamp && size < 8)) return nullptr;
    const uint8_t* j = static_cast<const uint8_t*>(job);
    void* buffer = nullptr;
    uint16_t id = 0;
    int64_t tick = 0;
    uint32_t tick_length = 0;
    if (!rd(j + off::kJobEventBuffer, buffer) || !buffer || !rd(j + off::kJobProgramId, id) ||
        !rd(j + off::kJobTick, tick) || !rd(j + off::kJobTickLength, tick_length))
        return nullptr;
    const int64_t now = tick * static_cast<int64_t>(tick_length);
    return call_emit(fn, buffer, hash, size, id, stamp ? &now : nullptr);
}

}  // namespace emf::core
