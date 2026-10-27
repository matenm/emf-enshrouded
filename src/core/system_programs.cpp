#include "system_programs.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "common/offsets.h"
#include "common/seh.h"
#include "common/world_view.h"
#include "emf/generated/components.hpp"
#include "fault_report.h"

namespace emf::core {
namespace {

// The events of one program as resolved in one world.
struct WorldEvents {
    void* world = nullptr;
    std::vector<EventType> in;         // same order as the spec's events_in
    std::vector<size_t> in_iter_slot;  // iterator index of each events_in entry
    std::vector<EventType> out;        // same order as the spec's events_out
    std::vector<AccessorState> accessors;  // one per other-entity accessor
};
constexpr size_t kMaxWorldsPerProgram = 8;  // old worlds (earlier sessions) are dropped first

// A component of other entities, in the order the engine puts its accessor into the iterator.
struct OtherName {
    std::string name;      // short name as declared
    uint32_t hash;         // types.json qualifiedHash
    std::string sort_key;  // qualified name with a "Dynamic" prefix removed (the engine's type order)
};

struct Slot {
    std::string name;  // stable storage: the engine keeps pointers to it
    std::vector<std::string> events_in, events_out;
    std::vector<NameRef> in_refs, out_refs;
    std::vector<std::string> singletons_read, singletons_write;
    std::vector<NameRef> sread_refs, swrite_refs;
    std::vector<OtherName> other_read, other_write;                // sorted
    std::vector<std::string> other_read_names, other_write_names;  // the descriptor's strings (same order)
    std::vector<NameRef> oread_refs, owrite_refs;
    ProgramDesc desc{};
    ProgramCallback fn = nullptr;
    void* user = nullptr;
    bool faulted = false;
    std::vector<WorldEvents> worlds;  // guarded by g_worlds_lock
};
std::array<Slot, kMaxSystemPrograms> g_slots;
SRWLOCK g_worlds_lock = SRWLOCK_INIT;
void (*g_iter_begin)(void*, void*, uint32_t) = nullptr;
thread_local const ProgramCall* t_call = nullptr;

bool guarded(ProgramCallback fn, void* ctx, void* user) {
    __try {
        fn(ctx, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool iter_begin_guarded(void (*fn)(void*, void*, uint32_t), void* ctx, uint64_t* iter) {
    __try {
        fn(ctx, iter, static_cast<uint32_t>(kProgramIterQwords * 8));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ProgramContext.job -> job+0 entity storage -> world (server and client storages).
uint8_t* world_of(void* ctx) {
    void* job = nullptr;
    uint8_t* storage = nullptr;
    if (!ctx || !seh_read(ctx, &job, 8) || !job || !seh_read(static_cast<uint8_t*>(job) + off::kJobStorage, &storage, 8) ||
        !storage)
        return nullptr;
    return storage - off::kStorage;
}

void run_slot(size_t i, void* ctx) {
    Slot& s = g_slots[i];
    if (!s.fn || s.faulted) return;
    uint64_t iter[kProgramIterQwords] = {};  // slots the engine does not fill read as null
    ProgramCall call{i, ctx, world_of(ctx), nullptr};
    const bool wants_iter = !s.events_in.empty() || !s.singletons_read.empty() || !s.singletons_write.empty() ||
                            !s.other_read.empty() || !s.other_write.empty();
    if (wants_iter && g_iter_begin && iter_begin_guarded(g_iter_begin, ctx, iter)) call.iter = iter;
    const ProgramCall* outer = t_call;
    t_call = &call;
    const bool ok = guarded(s.fn, ctx, s.user);
    t_call = outer;
    if (!ok) {
        s.faulted = true;
        report_fault("mod program '" + s.name + "'", reinterpret_cast<const void*>(s.fn));
    }
}

template <size_t I>
void trampoline(void* ctx) {
    run_slot(I, ctx);
}

template <size_t... I>
constexpr std::array<void (*)(void*), sizeof...(I)> make(std::index_sequence<I...>) {
    return {&trampoline<I>...};
}
constexpr auto kTrampolines = make(std::make_index_sequence<kMaxSystemPrograms>{});

bool valid_event_list(const std::vector<std::string>& names) {
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string& n = names[i];
        if (n.empty() || n.size() > kMaxEventNameLength) return false;
        for (char c : n)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
        for (size_t j = 0; j < i; ++j)
            if (names[j] == n) return false;
    }
    return true;
}

bool valid_singleton_list(const std::vector<std::string>& names) {
    if (names.size() > kMaxProgramAccessList) return false;
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string& n = names[i];
        if (n.empty() || n.size() > kMaxEventNameLength) return false;
        for (char c : n)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ':'))
                return false;
        for (size_t j = 0; j < i; ++j)
            if (names[j] == n) return false;
    }
    return true;
}

// "keen::ecs::DynamicInventoryCraftingStock" -> "keen::ecs::InventoryCraftingStock"
std::string strip_dynamic(const std::string& qualified) {
    const size_t c = qualified.rfind(':');
    const size_t start = c == std::string::npos ? 0 : c + 1;
    if (qualified.compare(start, 7, "Dynamic") == 0 && qualified.size() > start + 7)
        return qualified.substr(0, start) + qualified.substr(start + 7);
    return qualified;
}

// Resolves short component names against the generated tables and sorts them in the engine's component order
// (ascending qualified name, a Dynamic half as its base; stable for ties). False on an unknown name, a repeat or a
// list longer than kMaxProgramAccessList.
bool other_list(const std::vector<std::string>& names, std::vector<OtherName>& out) {
    out.clear();
    if (names.size() > kMaxProgramAccessList) return false;
    for (size_t i = 0; i < names.size(); ++i) {
        for (size_t j = 0; j < i; ++j)
            if (names[j] == names[i]) return false;
        const emf::gen::ComponentInfo* found = nullptr;
        auto look = [&](const auto& table) {
            for (const auto& ci : table) {
                const char* c = std::strrchr(ci.name, ':');
                if (names[i] == (c ? c + 1 : ci.name)) found = &ci;
            }
        };
        look(emf::gen::kComponents);
        if (!found) look(emf::gen::kDynamicComponents);
        if (!found) return false;
        out.push_back({names[i], found->hash, strip_dynamic(found->name)});
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const OtherName& a, const OtherName& b) { return a.sort_key < b.sort_key; });
    return true;
}

std::vector<std::string> names_of(const std::vector<OtherName>& list) {
    std::vector<std::string> out;
    for (const auto& o : list) out.push_back(o.name);
    return out;
}

NameList as_list(std::vector<NameRef>& refs, const std::vector<std::string>& names) {
    refs.clear();
    for (const auto& n : names) refs.push_back({n.c_str(), n.size()});
    return {refs.empty() ? nullptr : refs.data(), refs.size()};
}

const char* kind_name(uint8_t kind) {
    switch (kind) {
        case EMF_WORLD_SERVER: return "server";
        case EMF_WORLD_PREDICTION: return "prediction";
        case EMF_WORLD_CLIENT: return "client";
        default: return "unknown";
    }
}

// Every other-entity component must be a per-entity component of `world`: the engine resolves these names in its
// dynamic map and stops resolving the program's access lists at the first miss (re_custom_programs.md), which would
// leave the iterator slots EMF computed empty. A world whose registry cannot be read is not checked.
bool check_other_components(const Slot& s, uint8_t* world, std::string& why) {
    const WorldView view(world);
    for (const auto* list : {&s.other_read, &s.other_write})
        for (const auto& o : *list)
            if (view.has_dynamic_component(o.hash) == std::optional<bool>(false)) {
                why = "'" + o.name + "' is not a per-entity component in this world (a template-only one? use its "
                      "Dynamic half, e.g. DynamicActiveNpcState)";
                return false;
            }
    return true;
}

// Resolves a slot's events in `world`. On failure fills `why` and returns false.
bool resolve_events(const Slot& s, const uint8_t* world, WorldEvents& out, std::string& why) {
    for (const auto& n : s.events_in) {
        const auto e = find_event(world, n);
        if (!e) {
            why = "unknown event '" + n + "'";
            return false;
        }
        out.in.push_back(*e);
    }
    for (const auto& n : s.events_out) {
        const auto e = find_event(world, n);
        if (!e) {
            why = "unknown event '" + n + "'";
            return false;
        }
        if (e->size >= 0x100) {
            why = "event '" + n + "' is " + std::to_string(e->size) + " bytes; events of 256 bytes or more cannot be sent";
            return false;
        }
        out.out.push_back(*e);
    }
    // events_in queue pointers sit in the iterator after the singletons and other-entity accessors, ordered by
    // event type index
    const IterLayout layout = iter_layout(s.singletons_read.size() + s.singletons_write.size(),
                                          s.other_read.size() + s.other_write.size());
    for (const auto& e : out.in) {
        size_t rank = 0;
        for (const auto& other : out.in) rank += other.index < e.index;
        out.in_iter_slot.push_back(layout.events_in_base + rank);
    }
    return true;
}

void remember_world(Slot& s, WorldEvents&& we) {
    AcquireSRWLockExclusive(&g_worlds_lock);
    auto it = std::find_if(s.worlds.begin(), s.worlds.end(), [&](const WorldEvents& w) { return w.world == we.world; });
    if (it != s.worlds.end()) {
        *it = std::move(we);
    } else {
        if (s.worlds.size() >= kMaxWorldsPerProgram) s.worlds.erase(s.worlds.begin());
        s.worlds.push_back(std::move(we));
    }
    ReleaseSRWLockExclusive(&g_worlds_lock);
}

}  // namespace

IterLayout iter_layout(size_t singletons, size_t others) { return {1, 1 + singletons, 1 + singletons + others}; }

void build_system_desc(ProgramDesc& d, const std::string& name, uint8_t world_kind, void (*update)(void*)) {
    std::memset(&d, 0, sizeof(d));
    d.name = {name.c_str(), name.size()};
    d.update = update;
    d.flags = kFlagSystem | (world_kind == EMF_WORLD_CLIENT ? kFlagClient : kFlagNotPredicted);
    d.tickInterval = 1;
}

bool SystemPrograms::add(const ProgramSpec& spec, bool internal) {
    if (spec.name.empty() || spec.name.size() > 100 || !spec.fn) return false;
    if (spec.world_kind != EMF_WORLD_SERVER && spec.world_kind != EMF_WORLD_CLIENT) return false;
    if (!valid_event_list(spec.events_in) || !valid_event_list(spec.events_out)) return false;
    if (spec.events_in.size() + spec.events_out.size() > kMaxProgramEvents) return false;
    if (!valid_singleton_list(spec.singletons_read) || !valid_singleton_list(spec.singletons_write)) return false;
    std::vector<OtherName> oread, owrite;
    if (!other_list(spec.other_read, oread) || !other_list(spec.other_write, owrite)) return false;
    AcquireSRWLockExclusive(&lock_);
    bool ok = !closed_ && count_ < kMaxSystemPrograms && (internal || mod_count_ < kMaxModPrograms);
    for (size_t i = 0; ok && i < count_; ++i) ok = g_slots[i].name != spec.name;
    if (ok) {
        Slot& s = g_slots[count_];
        s.name = spec.name;
        s.events_in = spec.events_in;
        s.events_out = spec.events_out;
        s.fn = spec.fn;
        s.user = spec.user;
        s.faulted = false;
        build_system_desc(s.desc, s.name, spec.world_kind, kTrampolines[count_]);
        s.desc.eventsIn = as_list(s.in_refs, s.events_in);
        s.desc.eventsOut = as_list(s.out_refs, s.events_out);
        s.singletons_read = spec.singletons_read;
        s.singletons_write = spec.singletons_write;
        s.other_read = std::move(oread);
        s.other_write = std::move(owrite);
        s.other_read_names = names_of(s.other_read);
        s.other_write_names = names_of(s.other_write);
        s.desc.sysRead = as_list(s.sread_refs, s.singletons_read);
        s.desc.sysWrite = as_list(s.swrite_refs, s.singletons_write);
        s.desc.otherRead = as_list(s.oread_refs, s.other_read_names);
        s.desc.otherWrite = as_list(s.owrite_refs, s.other_write_names);
        ++count_;
        if (!internal) ++mod_count_;
    }
    ReleaseSRWLockExclusive(&lock_);
    return ok;
}

bool SystemPrograms::add(const std::string& name, uint8_t world_kind, ProgramCallback fn, void* user) {
    return add(ProgramSpec{name, world_kind, fn, user, {}, {}});
}

size_t SystemPrograms::register_into(void* world, RegisterProgramFn register_program, std::vector<std::string>* problems) {
    if (!world || !register_program) return 0;
    AcquireSRWLockShared(&lock_);
    const size_t n = count_;
    ReleaseSRWLockShared(&lock_);
    auto* w = static_cast<uint8_t*>(world);
    size_t done = 0;
    for (size_t i = 0; i < n; ++i) {
        uint16_t programs = 0;
        if (!seh_read(w + kProgramCount, &programs, 2) || programs >= kProgramMax) break;
        Slot& s = g_slots[i];
        WorldEvents we;
        we.world = world;
        std::string why;
        if (!check_other_components(s, w, why) || !resolve_events(s, w, we, why)) {
            if (problems)
                problems->push_back("mod program '" + s.name + "' not added to the " + kind_name(WorldView(w).kind()) +
                                    " world: " + why);
            continue;
        }
        we.accessors.assign(s.other_read.size() + s.other_write.size(), AccessorState{});
        remember_world(s, std::move(we));  // before the world can run the program
        if (register_program(world, &s.desc, 0) & 0xFF) ++done;
    }
    return done;
}

size_t SystemPrograms::count() const {
    AcquireSRWLockShared(&lock_);
    size_t n = count_;
    ReleaseSRWLockShared(&lock_);
    return n;
}

void SystemPrograms::close_registration() {
    AcquireSRWLockExclusive(&lock_);
    closed_ = true;
    ReleaseSRWLockExclusive(&lock_);
}

bool SystemPrograms::registration_closed() const {
    AcquireSRWLockShared(&lock_);
    const bool closed = closed_;
    ReleaseSRWLockShared(&lock_);
    return closed;
}

void SystemPrograms::reset() {
    AcquireSRWLockExclusive(&lock_);
    AcquireSRWLockExclusive(&g_worlds_lock);
    for (auto& s : g_slots) s = Slot{};
    ReleaseSRWLockExclusive(&g_worlds_lock);
    count_ = 0;
    mod_count_ = 0;
    closed_ = false;
    ReleaseSRWLockExclusive(&lock_);
}

const ProgramCall* current_program_call() { return t_call; }
uint8_t* program_context_world(void* ctx) { return world_of(ctx); }

void set_program_iter_begin(void (*fn)(void* ctx, void* iter, uint32_t size)) { g_iter_begin = fn; }

std::optional<EventType> program_event(const ProgramCall& call, std::string_view name, bool out, size_t* iter_slot) {
    if (call.slot >= kMaxSystemPrograms || !call.world) return std::nullopt;
    const Slot& s = g_slots[call.slot];
    const auto& names = out ? s.events_out : s.events_in;
    size_t idx = names.size();
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) idx = i;
    if (idx == names.size()) return std::nullopt;
    std::optional<EventType> result;
    AcquireSRWLockShared(&g_worlds_lock);
    for (const auto& we : s.worlds) {
        if (we.world != call.world) continue;
        const auto& list = out ? we.out : we.in;
        if (idx < list.size()) {
            result = list[idx];
            if (!out && iter_slot) *iter_slot = we.in_iter_slot[idx];
        }
        break;
    }
    ReleaseSRWLockShared(&g_worlds_lock);
    return result;
}

std::optional<size_t> program_singleton_slot(const ProgramCall& call, std::string_view name) {
    if (call.slot >= kMaxSystemPrograms) return std::nullopt;
    const Slot& s = g_slots[call.slot];
    const IterLayout l = iter_layout(s.singletons_read.size() + s.singletons_write.size(), 0);
    for (size_t i = 0; i < s.singletons_read.size(); ++i)
        if (s.singletons_read[i] == name) return l.singleton_base + i;
    for (size_t i = 0; i < s.singletons_write.size(); ++i)
        if (s.singletons_write[i] == name) return l.singleton_base + s.singletons_read.size() + i;
    return std::nullopt;
}

std::optional<OtherAccess> program_other(const ProgramCall& call, std::string_view component, bool want_write) {
    if (call.slot >= kMaxSystemPrograms) return std::nullopt;
    const Slot& s = g_slots[call.slot];
    const IterLayout l = iter_layout(s.singletons_read.size() + s.singletons_write.size(),
                                     s.other_read.size() + s.other_write.size());
    for (size_t i = 0; i < s.other_read.size() && !want_write; ++i)
        if (s.other_read[i].name == component) return OtherAccess{l.other_base + i, s.other_read[i].hash, false, i};
    for (size_t i = 0; i < s.other_write.size(); ++i)
        if (s.other_write[i].name == component) {
            const size_t index = s.other_read.size() + i;
            return OtherAccess{l.other_base + index, s.other_write[i].hash, true, index};
        }
    return std::nullopt;
}

AccessorState accessor_state(const ProgramCall& call, size_t index) {
    AccessorState st;
    if (call.slot >= kMaxSystemPrograms) return st;
    AcquireSRWLockShared(&g_worlds_lock);
    for (const auto& we : g_slots[call.slot].worlds)
        if (we.world == call.world && index < we.accessors.size()) st = we.accessors[index];
    ReleaseSRWLockShared(&g_worlds_lock);
    return st;
}

void set_accessor_state(const ProgramCall& call, size_t index, AccessorState state) {
    if (call.slot >= kMaxSystemPrograms) return;
    AcquireSRWLockExclusive(&g_worlds_lock);
    for (auto& we : g_slots[call.slot].worlds)
        if (we.world == call.world && index < we.accessors.size()) we.accessors[index] = state;
    ReleaseSRWLockExclusive(&g_worlds_lock);
}

void (*system_program_trampoline(size_t slot))(void*) { return kTrampolines[slot]; }
bool system_program_faulted(size_t slot) { return g_slots[slot].faulted; }
const ProgramDesc& system_program_desc(size_t slot) { return g_slots[slot].desc; }

}  // namespace emf::core
