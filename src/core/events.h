#pragma once
// Engine ECS events: look up an event type, read an event queue, send an event from a program job.
// Layouts: docs/engine_parts/re_events.md.
#include <cstdint>
#include <optional>
#include <string_view>

namespace emf::core {

struct EventType {
    uint32_t hash = 0;           // fnv1a32("keen::ecs::" + name) = types.json qualifiedHash
    uint16_t index = 0;          // event type index in this world (programs' event slots follow it)
    uint32_t size = 0;           // struct size in bytes (timestamp included)
    bool has_timestamp = false;  // derives from keen::ecs::GameEvent (timeStamp at +0); the old ecs::Event types do not
};
constexpr uint32_t kGameEventHash = 0x8c815f07u;  // fnv1a32("keen::ecs::GameEvent")

uint32_t event_hash(std::string_view short_name);

// The event called `short_name` (without "keen::ecs::") in this world, or nullopt (unknown, or the
// world's type table does not agree). Fault-guarded.
std::optional<EventType> find_event(const uint8_t* world, std::string_view short_name);

using EventVisitor = void (*)(const void* event, void* user);

// Calls cb for each event in an engine event queue (events `stride` bytes apart). Returns how many
// events were visited; stops early at a missing or unreadable page. Faults inside cb are not caught here.
uint32_t walk_event_queue(const void* queue, uint32_t stride, EventVisitor cb, void* user);

// emitEvent(buffer, hash, size, programId) -> zeroed event or NULL (Sig::EmitEvent).
using EmitEventFn = void* (*)(void* buffer, uint32_t hash, uint32_t size, uint16_t program_id);

// Sends an event of `size` bytes from the program job `job` (= *(void**)ProgramContext) the way game
// programs do; with `stamp`, writes the program time into its timeStamp (+0). Returns the event to fill in, or
// NULL (size 0, >= 256, < 8 when stamped, no buffer, buffer full, or a fault).
void* emit_from_job(EmitEventFn fn, void* job, uint32_t hash, uint32_t size, bool stamp);

}  // namespace emf::core
