#include "event_relay.h"

#include <cstring>

#include "common/seh.h"
#include "events.h"
#include "fault_report.h"
#include "system_programs.h"

namespace emf::core {
namespace {

bool valid_name(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

bool call_guarded(RelayCallback cb, EmfWorld* world, const void* event, uint32_t size, void* user) {
    __try {
        cb(world, event, size, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void set_why(std::string* why, const std::string& text) {
    if (why) *why = text;
}

struct ExclusiveLock {  // released even if the body throws (bad_alloc)
    SRWLOCK* lock;
    explicit ExclusiveLock(SRWLOCK* l) : lock(l) { AcquireSRWLockExclusive(lock); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(lock); }
};

struct CaptureCtx {
    EventRelay* relay;
    void* world;
    uint8_t kind;
    const std::string* name;
    uint32_t size;
};
void capture_one(const void* event, void* user) {
    auto* c = static_cast<CaptureCtx*>(user);
    c->relay->capture(c->world, c->kind, *c->name, event, c->size);
}

}  // namespace

bool EventRelay::subscribe(uint8_t kind, const std::string& event, uint32_t expected_size, RelayCallback cb, void* user,
                           std::string* why) {
    if (kind != EMF_WORLD_SERVER && kind != EMF_WORLD_CLIENT) {
        set_why(why, "world kind must be server or client");
        return false;
    }
    if (!cb) {
        set_why(why, "null callback");
        return false;
    }
    if (!valid_name(event)) {
        set_why(why, "event names are 1-64 letters, digits or _ (the short type name, e.g. HitEvent)");
        return false;
    }
    AcquireSRWLockExclusive(&lock_);
    bool ok = true;
    if (closed_) {
        set_why(why, unavailable_.empty() ? "call it from emf_mod_load, before the first world is set up"
                                          : "on_event is unavailable: " + unavailable_);
        ok = false;
    } else {
        auto& names = names_[kind];
        bool known = false;
        for (const auto& n : names) known |= n == event;
        if (!known && names.size() >= kRelayMaxEventsPerKind) {
            set_why(why, "at most 24 different events per world kind");
            ok = false;
        } else {
            if (!known) names.push_back(event);
            subs_.push_back(Sub{kind, event, expected_size, cb, user});
        }
    }
    ReleaseSRWLockExclusive(&lock_);
    return ok;
}

std::vector<std::string> EventRelay::events(uint8_t kind) const {
    if (kind > 2) return {};
    AcquireSRWLockShared(&lock_);
    std::vector<std::string> out = names_[kind];
    ReleaseSRWLockShared(&lock_);
    return out;
}

void EventRelay::capture(void* world, uint8_t kind, std::string_view event, const void* data, uint32_t size) {
    if (!world || kind > 2 || !data || !size) return;
    // Copy the engine's bytes before taking the lock: a fault here must not leave the relay locked.
    std::vector<uint8_t> copy(size);
    if (!seh_read(data, copy.data(), size)) return;
    ExclusiveLock guard(&lock_);
    const auto& names = names_[kind];
    uint32_t idx = 0;
    while (idx < names.size() && names[idx] != event) ++idx;
    if (idx < names.size()) {
        Buffer* b = nullptr;
        for (auto& x : buffers_)
            if (x.world == world && x.kind == kind) b = &x;
        if (!b) {
            buffers_.push_back(Buffer{world, kind, {}, {}, 0});
            b = &buffers_.back();
        }
        if (b->used + size + kRelayEntryCost > cap_) {
            overflow_pending_ = true;
        } else {
            const uint32_t offset = static_cast<uint32_t>(b->bytes.size());
            b->bytes.resize(b->bytes.size() + size);
            std::memcpy(b->bytes.data() + offset, copy.data(), size);
            b->entries.push_back(Entry{idx, offset, size});
            b->used += size + kRelayEntryCost;
        }
    }
}

size_t EventRelay::deliver(EmfWorld* world, uint8_t kind, const std::function<void(const std::string&)>& log) {
    if (!world || kind > 2) return 0;
    Buffer mine;
    std::vector<std::string> names;
    std::vector<size_t> subs;
    bool log_overflow = false;
    AcquireSRWLockExclusive(&lock_);
    for (auto it = buffers_.begin(); it != buffers_.end(); ++it) {
        if (it->world == world && it->kind == kind) {
            mine = std::move(*it);
            buffers_.erase(it);
            break;
        }
    }
    names = names_[kind];
    for (size_t i = 0; i < subs_.size(); ++i)
        if (subs_[i].kind == kind && subs_[i].enabled) subs.push_back(i);
    if (overflow_pending_ && !overflow_logged_) {
        log_overflow = true;
        overflow_logged_ = true;
    }
    overflow_pending_ = false;
    ReleaseSRWLockExclusive(&lock_);

    if (log_overflow && log)
        log("on_event: more than " + std::to_string(cap_ / 1024) +
            " KiB of events in one tick; the rest were dropped (logged once per session)");
    size_t calls = 0;
    for (const auto& e : mine.entries) {
        const std::string& name = names[e.name_idx];
        const void* data = mine.bytes.data() + e.offset;
        for (size_t i : subs) {
            AcquireSRWLockShared(&lock_);
            const Sub s = subs_[i];
            ReleaseSRWLockShared(&lock_);
            if (!s.enabled || s.event != name) continue;
            if (s.expected && s.expected != e.size) {
                if (!s.size_warned) {
                    AcquireSRWLockExclusive(&lock_);
                    subs_[i].size_warned = true;
                    ReleaseSRWLockExclusive(&lock_);
                    if (log)
                        log("on_event('" + name + "'): the game's event is " + std::to_string(e.size) +
                            " bytes, the mod expects " + std::to_string(s.expected) +
                            "; not delivered to that callback (a game update? the mod or EMF needs an update)");
                }
                continue;
            }
            ++calls;
            if (!call_guarded(s.cb, world, data, e.size, s.user)) {
                AcquireSRWLockExclusive(&lock_);
                subs_[i].enabled = false;
                ReleaseSRWLockExclusive(&lock_);
                report_fault("on_event callback for '" + name + "'", reinterpret_cast<const void*>(s.cb));
            }
        }
    }
    return calls;
}

void relay_program(void* program_context, void* user) {
    auto* rp = static_cast<RelayProgram*>(user);
    const ProgramCall* call = current_program_call();
    if (!rp || !rp->relay || !call || call->ctx != program_context || !call->iter) return;
    for (const auto& name : rp->events) {
        size_t slot = 0;
        const auto e = program_event(*call, name, false, &slot);
        if (!e || e->size % 8 != 0 || slot >= kProgramIterQwords) continue;
        CaptureCtx c{rp->relay, call->world, rp->kind, &name, e->size};
        walk_event_queue(reinterpret_cast<const void*>(call->iter[slot]), e->size, capture_one, &c);
    }
}

size_t add_relay_programs(EventRelay& relay, SystemPrograms& programs, const uint8_t* world, RelayProgram storage[2],
                          const std::function<void(const std::string&)>& log) {
    if (relay.closed()) return 0;
    size_t added = 0;
    const uint8_t kinds[2] = {EMF_WORLD_SERVER, EMF_WORLD_CLIENT};
    for (int k = 0; k < 2; ++k) {
        std::vector<std::string> keep;
        for (const auto& name : relay.events(kinds[k])) {
            const auto e = find_event(world, name);
            if (!e) {
                if (log) log("on_event('" + name + "'): the game has no event with that name; nothing will be delivered");
                continue;
            }
            if (e->size % 8 != 0) {
                if (log)
                    log("on_event('" + name + "'): the queue layout of " + std::to_string(e->size) +
                        "-byte events is not known yet; nothing will be delivered");
                continue;
            }
            keep.push_back(name);
        }
        if (keep.empty()) continue;
        storage[k].relay = &relay;
        storage[k].kind = kinds[k];
        storage[k].events = keep;
        ProgramSpec spec{kinds[k] == EMF_WORLD_SERVER ? "emf_event_relay_server" : "emf_event_relay_client", kinds[k],
                         relay_program, &storage[k], keep, {}};
        if (programs.add(spec, true)) {
            ++added;
        } else if (log) {
            log(std::string("on_event: could not add the relay program ") + spec.name + " (program pool full?)");
        }
    }
    relay.close();
    return added;
}

void EventRelay::forget_world(void* world) {
    AcquireSRWLockExclusive(&lock_);
    for (auto it = buffers_.begin(); it != buffers_.end();) {
        if (it->world == world) it = buffers_.erase(it);
        else ++it;
    }
    ReleaseSRWLockExclusive(&lock_);
}

void EventRelay::close(const std::string& unavailable_reason) {
    AcquireSRWLockExclusive(&lock_);
    closed_ = true;
    if (!unavailable_reason.empty()) unavailable_ = unavailable_reason;
    ReleaseSRWLockExclusive(&lock_);
}

void EventRelay::new_session() {
    AcquireSRWLockExclusive(&lock_);
    overflow_logged_ = false;
    ReleaseSRWLockExclusive(&lock_);
}

bool EventRelay::closed() const {
    AcquireSRWLockShared(&lock_);
    const bool c = closed_;
    ReleaseSRWLockShared(&lock_);
    return c;
}

void EventRelay::reset() {
    AcquireSRWLockExclusive(&lock_);
    closed_ = overflow_pending_ = overflow_logged_ = false;
    unavailable_.clear();
    subs_.clear();
    for (auto& n : names_) n.clear();
    buffers_.clear();
    ReleaseSRWLockExclusive(&lock_);
}

}  // namespace emf::core
