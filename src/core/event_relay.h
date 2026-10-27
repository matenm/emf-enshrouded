#pragma once
// Gameplay event callbacks (API v4 on_event): mods subscribe to engine events during emf_mod_load; an internal
// mod program per world kind copies those events every tick (capture), and the core hands the copies to the
// subscribers after the simulation step, on the world's own thread (deliver).
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "emf/emf_api.h"

namespace emf::core {

using RelayCallback = void (*)(EmfWorld* world, const void* event, uint32_t size, void* user);
constexpr size_t kRelayMaxEventsPerKind = 24;            // one relay program per kind; engine limit per program
constexpr size_t kRelayBufferBytes = size_t(1) << 20;    // per world and tick
constexpr size_t kRelayEntryCost = 16;                   // bookkeeping counted per captured event

class EventRelay {
public:
    explicit EventRelay(size_t buffer_bytes = kRelayBufferBytes) : cap_(buffer_bytes) {}

    // False (and *why) when closed, kind is not server/client, cb is null, the name is not 1..64 chars of
    // [A-Za-z0-9_], or it would be the 25th distinct event of that kind.
    bool subscribe(uint8_t kind, const std::string& event, uint32_t expected_size, RelayCallback cb, void* user,
                   std::string* why);
    // Distinct subscribed events of a kind, in first-subscription order (the relay program's events_in).
    std::vector<std::string> events(uint8_t kind) const;
    // Copies one event of `world` (program thread). Unsubscribed names are ignored; past the buffer cap it drops.
    void capture(void* world, uint8_t kind, std::string_view event, const void* data, uint32_t size);
    // Calls the subscribers of `kind` for the events captured in `world`, in capture order, then forgets them.
    // Returns the number of callbacks made. Run on the world's thread after its step.
    size_t deliver(EmfWorld* world, uint8_t kind, const std::function<void(const std::string&)>& log);
    // A world is being set up: drop anything left under its address by an earlier world.
    void forget_world(void* world);
    // No more subscriptions: the relay programs are registered, or (reason given) on_event cannot work on this build.
    void close(const std::string& unavailable_reason = {});
    void new_session();  // a new server world: overflow is logged again
    bool closed() const;
    void reset();  // tests only

private:
    struct Sub {
        uint8_t kind;
        std::string event;
        uint32_t expected;
        RelayCallback cb;
        void* user;
        bool enabled = true;
        bool size_warned = false;
    };
    struct Entry {
        uint32_t name_idx;  // into names_[kind]
        uint32_t offset;
        uint32_t size;
    };
    struct Buffer {
        void* world = nullptr;
        uint8_t kind = 0;
        std::vector<uint8_t> bytes;
        std::vector<Entry> entries;
        size_t used = 0;
    };
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    size_t cap_;
    bool closed_ = false;
    std::string unavailable_;
    bool overflow_pending_ = false;
    bool overflow_logged_ = false;
    std::vector<Sub> subs_;
    std::vector<std::string> names_[3];
    std::vector<Buffer> buffers_;
};

class SystemPrograms;

// The internal mod program of one world kind: copies the subscribed events of every tick into the relay.
struct RelayProgram {
    EventRelay* relay = nullptr;
    uint8_t kind = 0;
    std::vector<std::string> events;  // the events_in it was registered with
};
void relay_program(void* program_context, void* user);  // user = RelayProgram*

// Call when the first world registers its programs: adds one relay program per world kind with subscriptions
// (events that do not exist in `world` are dropped and logged), then closes the relay. `storage` (2 entries,
// static) holds the programs. Returns how many programs were added; 0 once the relay is closed.
size_t add_relay_programs(EventRelay& relay, SystemPrograms& programs, const uint8_t* world, RelayProgram storage[2],
                          const std::function<void(const std::string&)>& log);

}  // namespace emf::core
