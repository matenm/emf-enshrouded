#include "program_access.h"

#include <cstdio>

#include "common/offsets.h"
#include "common/seh.h"

namespace emf::core {
namespace {
bool guarded_read(ReadOtherFn fn, void** out, void* world_ctx, uint64_t accessor, uint32_t entity) {
    __try {
        fn(out, world_ctx, accessor, entity);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool guarded_write(WriteOtherFn fn, void** out, void* storage, void* change_set, uint64_t accessor, uint32_t entity,
                   uint8_t variant) {
    __try {
        fn(out, storage, change_set, accessor, entity, variant);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void say(std::string* problem, std::string text) {
    if (problem) *problem = std::move(text);
}
}  // namespace

OtherRead read_other(const ProgramCall& call, const OtherAccess& a, std::string_view component, uint32_t entity,
                     ReadOtherFn read, GetComponentFn get_component, std::string* problem) {
    OtherRead r;
    if (!call.iter || !read || a.iter_slot >= kProgramIterQwords) return r;
    AccessorState st = accessor_state(call, a.index);
    if (st.check == AccessorCheck::Bad) return r;
    const uint64_t accessor = call.iter[a.iter_slot];
    void* job = nullptr;
    void* world_ctx = nullptr;
    if (!accessor || !seh_read(call.ctx, &job, 8) || !job ||
        !seh_read(static_cast<uint8_t*>(job) + off::kJobWorldCtx, &world_ctx, 8) || !world_ctx)
        return r;
    void* out[2] = {};  // the game stores {component*, size}
    if (!guarded_read(read, out, world_ctx, accessor, entity)) {
        say(problem, "the game's readOther faulted");
        return r;
    }
    void* const p = out[0];
    if (st.check == AccessorCheck::Unchecked) {
        const ComponentRef c = emf::get_component(get_component, call.world, a.hash, entity);
        // Only one side has it (e.g. an entity created this tick): inconclusive, read nothing, check again later.
        if (!p || !c.ptr) return r;
        if (p != c.ptr) {
            set_accessor_state(call, a.index, {AccessorCheck::Bad, 0});
            say(problem, "the accessor for '" + std::string(component) +
                             "' does not match getComponent (iterator layout?); refused in this world");
            return r;
        }
        st = {AccessorCheck::Good, c.size};
        set_accessor_state(call, a.index, st);
    }
    r.ptr = p;
    r.size = st.size;
    return r;
}

OtherRead write_other(const ProgramCall& call, const OtherAccess& a, std::string_view component, uint32_t entity,
                      ReadOtherFn read, WriteOtherFn write, GetComponentFn get_component, std::string* problem) {
    OtherRead r;
    if (!call.iter || !write || !read || !a.write || a.iter_slot >= kProgramIterQwords) {
        say(problem, "no write accessor for it in this program");
        return r;
    }
    AccessorState st = accessor_state(call, a.index);
    if (st.check == AccessorCheck::Bad) {
        say(problem, "its accessor failed the check earlier in this world");
        return r;
    }
    const uint64_t accessor = call.iter[a.iter_slot];
    if (!accessor) {
        char slots[160];
        std::snprintf(slots, sizeof slots, "the iterator has no write accessor in slot %zu (slots 1-4: %llx %llx %llx %llx)",
                      a.iter_slot, static_cast<unsigned long long>(call.iter[1]), static_cast<unsigned long long>(call.iter[2]),
                      static_cast<unsigned long long>(call.iter[3]), static_cast<unsigned long long>(call.iter[4]));
        say(problem, slots);
        return r;
    }
    uint8_t* job = nullptr;
    void* storage = nullptr;
    void* change_set = nullptr;
    uint8_t variant = 0;
    if (!seh_read(call.ctx, &job, 8) || !job || !seh_read(job + off::kJobWorldCtx, &storage, 8) || !storage ||
        !seh_read(job + off::kJobChangeSet, &change_set, 8) || !change_set ||
        !seh_read(job + off::kJobVariant, &variant, 1)) {
        say(problem, "the program's job has no storage or change set");
        return r;
    }
    if (st.check == AccessorCheck::Unchecked) {
        // checked through readOther first: it reads the accessor the same way and marks nothing changed
        void* seen[2] = {};
        if (!guarded_read(read, seen, storage, accessor, entity)) {
            say(problem, "the game's readOther faulted while checking the write accessor");
            return r;
        }
        const ComponentRef c = emf::get_component(get_component, call.world, a.hash, entity);
        if (!seen[0] || !c.ptr) {  // inconclusive (one side only): nothing written, checked again next time
            char why[200];
            std::snprintf(why, sizeof why, "inconclusive check for entity %u: readOther gave %p, getComponent %p", entity,
                          seen[0], c.ptr);
            say(problem, why);
            return r;
        }
        if (seen[0] != c.ptr) {
            set_accessor_state(call, a.index, {AccessorCheck::Bad, 0});
            say(problem, "the write accessor for '" + std::string(component) +
                             "' does not match getComponent (iterator layout?); refused in this world");
            return r;
        }
        st = {AccessorCheck::Good, c.size};
        set_accessor_state(call, a.index, st);
    }
    void* out[2] = {};  // the game stores {component*, size}
    if (!guarded_write(write, out, storage, change_set, accessor, entity, variant)) {
        say(problem, "the game's writeOther faulted");
        return r;
    }
    r.ptr = out[0];
    r.size = st.size;
    return r;
}

}  // namespace emf::core
