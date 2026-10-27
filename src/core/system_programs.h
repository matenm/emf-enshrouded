#pragma once
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "emf/emf_api.h"
#include "events.h"
#include "programs.h"

namespace emf::core {

// ProgramDesc (0x188 B) as the engine reads it. See docs/engine_parts/re_custom_programs.md section 7.
struct NameRef {
    const char* s;
    uint64_t n;
};
struct NameList {
    const NameRef* p;
    uint64_t n;
};
struct ProgramDesc {
    NameRef name;                                                    // 0x000
    void (*update)(void* ctx);                                       // 0x010
    NameList tpl, tplOpt, read, readOpt, write, writeOpt, addComp, removeComp;  // 0x018..0x097
    NameList sysRead;                                                // 0x098
    uint64_t sysReadOptMask;                                         // 0x0A8
    NameList sysWrite;                                               // 0x0B0
    uint64_t sysWriteOptMask, sysWriteParallelMask;                  // 0x0C0, 0x0C8
    NameList otherTpl, otherRead, otherWrite, otherWriteUnser, with, without, anyOf, allWrite, allRead;  // 0x0D0..0x15F
    NameList eventsIn, eventsOut;                                    // 0x160, 0x170
    uint32_t flags;                                                  // 0x180
    uint32_t tickInterval;                                           // 0x184
};
static_assert(sizeof(ProgramDesc) == 0x188, "engine layout");

constexpr uint32_t kFlagClient = 0x1;
constexpr uint32_t kFlagSystem = 0x4;        // no entity query: one empty slice per tick
constexpr uint32_t kFlagNotPredicted = 0x10;
constexpr size_t kMaxModPrograms = 16;    // programs mods may add (EmfApi documents 16)
constexpr size_t kInternalPrograms = 4;   // EMF's own: emf_inventory, emf_teleport and the two event relays
constexpr size_t kMaxSystemPrograms = kMaxModPrograms + kInternalPrograms;
constexpr size_t kMaxProgramEvents = 24;   // engine: events in + out per program
constexpr size_t kMaxEventNameLength = 64;
constexpr size_t kProgramIterQwords = 128;  // iterator buffer of a mod program call (the engine never checks it)

// A mod program: runs once per tick on one world kind and may declare engine events it reads/sends.
struct ProgramSpec {
    std::string name;
    uint8_t world_kind = EMF_WORLD_SERVER;
    ProgramCallback fn = nullptr;
    void* user = nullptr;
    std::vector<std::string> events_in;   // short names ("HitEvent")
    std::vector<std::string> events_out;
    // Singletons the program reads / writes ("g38_items::ItemRegistry", as the engine's descriptors name
    // them) and components of OTHER entities it reads / writes (short names, "Inventory", "DynamicActiveNpcState").
    std::vector<std::string> singletons_read, singletons_write;
    std::vector<std::string> other_read, other_write;
};
constexpr size_t kMaxProgramAccessList = 16;  // per list (engine limits: 128 singletons, 92 other accessors)

// Where iter_begin puts a mod program's handles (no entity query: no template/read/write slots).
// re_custom_programs.md 6.1: [ctx] [singletons: sys-read, then sys-write] [other accessors: read, then write]
// [events-in queues] [events-out].
struct IterLayout {
    size_t singleton_base;
    size_t other_base;
    size_t events_in_base;
};
IterLayout iter_layout(size_t singletons, size_t others);

// A declared other-entity component of the running program: its iterator slot and types.json hash.
struct OtherAccess {
    size_t iter_slot;
    uint32_t hash;
    bool write;
    size_t index;  // position among the program's other-entity accessors (read list, then write list)
};

// What the first read through an accessor in a world showed (compared with getComponent).
enum class AccessorCheck : uint8_t { Unchecked, Good, Bad };
struct AccessorState {
    AccessorCheck check = AccessorCheck::Unchecked;
    uint64_t size = 0;  // the game's size of the component (from getComponent), once Good
};

// Fills a descriptor for a "system program" (runs once per tick on one world kind, iterates no entities).
void build_system_desc(ProgramDesc& d, const std::string& name, uint8_t world_kind, void (*update)(void*));

using RegisterProgramFn = uint64_t (*)(void* world, const void* desc, char disabled);

// Mod programs queued by register_program(); registered into every new world during its setup.
class SystemPrograms {
public:
    // false if the pool is full, the name is empty/duplicate, the callback is null, the kind is not 0/2,
    // an event name is not 1..64 chars of [A-Za-z0-9_], a list repeats a name, or there are more than 24 events
    // internal: one of EMF's own programs (not counted against the mods' kMaxModPrograms).
    bool add(const ProgramSpec& spec, bool internal = false);
    bool add(const std::string& name, uint8_t world_kind, ProgramCallback fn, void* user);
    // Registers every queued program into `world` (call right after registerAllPrograms). A program whose
    // events do not all exist in this world (or that sends an event of 256+ bytes) is left out of it and a
    // line is appended to `problems`. Returns how many were registered.
    size_t register_into(void* world, RegisterProgramFn register_program, std::vector<std::string>* problems = nullptr);
    size_t count() const;
    // After the first world registered its programs, add() is refused (it would only reach later worlds).
    void close_registration();
    bool registration_closed() const;
    void reset();  // tests only

private:
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    size_t count_ = 0;
    size_t mod_count_ = 0;
    bool closed_ = false;
};

// The mod program call running on this thread (set around the mod callback), or NULL.
struct ProgramCall {
    size_t slot = 0;
    void* ctx = nullptr;       // the engine's ProgramContext {job*, entity index}
    uint8_t* world = nullptr;  // the world the program runs in
    uint64_t* iter = nullptr;  // iterator filled by iter_begin, or NULL (no events_in or no IterBegin)
};
const ProgramCall* current_program_call();
// The world a program's ProgramContext belongs to (job -> entity storage -> world), or NULL; any program, the game's too.
uint8_t* program_context_world(void* ctx);
// iter_begin(ctx, iter, size) (Sig::IterBegin); the trampoline calls it for programs with events_in.
void set_program_iter_begin(void (*fn)(void* ctx, void* iter, uint32_t size));
// The declared event `name` of the call's program in the call's world: out = events_out, else events_in.
// For events_in, *iter_slot = its iterator index (1 + rank by event type index). nullopt if not declared
// or the program was not added to that world.
std::optional<EventType> program_event(const ProgramCall& call, std::string_view name, bool out, size_t* iter_slot);

// Iterator slot of a singleton the call's program declared (sys-read or sys-write), or nullopt.
std::optional<size_t> program_singleton_slot(const ProgramCall& call, std::string_view name);
// A component of other entities the call's program declared, or nullopt. For reading: its read accessor, else its
// write accessor; want_write: only its write accessor (a component may be in both lists).
std::optional<OtherAccess> program_other(const ProgramCall& call, std::string_view component, bool want_write = false);
// The accessor's check state in the call's world (Unchecked if the program was not added to that world).
AccessorState accessor_state(const ProgramCall& call, size_t index);
void set_accessor_state(const ProgramCall& call, size_t index, AccessorState state);

// The update function installed in descriptor `slot` (calls the mod callback under an SEH guard).
void (*system_program_trampoline(size_t slot))(void*);
bool system_program_faulted(size_t slot);
const ProgramDesc& system_program_desc(size_t slot);

}  // namespace emf::core
