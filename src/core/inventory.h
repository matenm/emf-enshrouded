#pragma once
// Putting items straight into an entity's inventory through the game's own inventory transaction
// (docs/engine_parts/re_inventory_api.md): an internal server program "emf_inventory" with the accessors and
// singletons the transaction needs runs queued requests; results go back to the callers after the server step.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/world_view.h"
#include "emf/emf_api.h"
#include "program_access.h"
#include "common/log_limiter.h"
#include "system_programs.h"

namespace emf::core {

// ---- the engine's transaction (re_inventory_api.md section 2) ----
constexpr size_t kTxSize = 0x71B0;
constexpr size_t kSetupSize = 0xF0;
struct TxResult {
    uint8_t error;  // 0 = ok
    uint8_t pad[3];
    uint32_t zero;
    uint32_t value;  // count that did not fit
};
struct OptU32 {
    uint32_t value;
    uint8_t has;
    uint8_t pad[3];
};
struct OptU8 {
    uint8_t value;
    uint8_t has;
};
struct OptF32 {
    float value;
    uint8_t has;
    uint8_t pad[3];
};
using TxCtorFn = void* (*)(uint8_t* tx);
using TxInitFn = void (*)(uint8_t* tx, const uint8_t* setup);
using AddItemsFn = TxResult* (*)(TxResult* out, uint8_t* tx, uint32_t owner, uint64_t slot, uint32_t item,
                                 uint32_t count, OptU32* level, OptU8* rarity, OptF32* fuel);
using TxFn = void (*)(uint8_t* tx);
// removeItems(out, tx, owner, item, preferredSlot, count): out.value = count still missing (re_take_items.md).
using RemoveItemsFn = TxResult* (*)(TxResult* out, uint8_t* tx, uint32_t owner, uint32_t item, uint64_t preferred_slot,
                                    uint32_t count);
struct InventoryFns {
    TxCtorFn ctor = nullptr;
    TxInitFn init = nullptr;
    AddItemsFn add_items = nullptr;
    RemoveItemsFn remove_items = nullptr;  // optional: take requests need it
    TxFn commit = nullptr;
    TxFn rollback = nullptr;
    TxFn free_arrays = nullptr;
    bool complete() const { return ctor && init && add_items && commit && rollback && free_arrays; }
};

// The handles from emf_inventory's iterator that go into the setup block.
struct InventoryHandles {
    void* program_context = nullptr;
    void* item_registry = nullptr;
    void* balancing_registry = nullptr;
    void* attribute_registry = nullptr;
    void* knowledge_queue = nullptr;
    uint64_t fuel = 0, inventory = 0, inventory_empty_state = 0, inventory_setup = 0, item_state = 0,
             owner_relationship = 0, slot_selection = 0;  // other-entity accessors
    uint64_t pickup_writer = 0, inventory_full_writer = 0;  // events-out slots
};
constexpr uint8_t kSetupFlags = 0x01 | 0x04;  // PickupEvent popup, InventoryFull when items did not fit
// The 0xF0-byte setup block for putting items into `entity`'s inventory (re_inventory_api.md section 1).
void fill_setup(uint8_t* setup, const InventoryHandles& h, uint32_t entity);

struct PutResult {
    uint32_t added = 0;    // count - remaining when committed, else 0 (take: count taken)
    uint32_t result_missing = 0;  // the engine's out.value (put: did not fit; take: not found)
    uint8_t error = 0;     // addItems' error code
    bool faulted = false;  // a game function faulted (the feature should stop)
};
// One transaction: ctor, init, addItems(owner = entity, slot 0 = automatic), commit when no error and something fit
// (else rollback), freeArrays. `tx` must hold kTxSize bytes, 16-aligned.
struct TxOptions {
    bool take = false;
    uint64_t slot = 0;
    uint32_t level = 0;
    uint8_t rarity = 0xFF;
};
PutResult run_put(const InventoryFns& fns, const InventoryHandles& h, uint8_t* tx, uint32_t entity, uint32_t item,
                  uint32_t count, const TxOptions& opt = {});

// ---- requests and the internal program ----
constexpr size_t kMaxPendingPuts = 64;
constexpr size_t kPutsPerTick = 8;
constexpr const char* kInventoryProgramName = "emf_inventory";
constexpr uint64_t kInventoryWatchdogMs = 2000;

struct PutRequest {
    EmfWorld* world = nullptr;
    uint32_t entity = 0, item = 0, count = 0;
    EmfPutItemsCallback cb = nullptr;
    void* user = nullptr;
    bool take = false;     // remove instead of put
    uint64_t slot = 0;     // take: the slot to take from first (0 = any)
    bool exact_slot = false;  // take: only from `slot`, refused when it does not hold the item
    uint32_t level = 0;    // put: item level (0 = default)
    uint8_t rarity = 0xFF; // put: rarity (0xFF = default)
};

class InventoryService {
public:
    struct Deps {
        InventoryFns fns;
        ReadOtherFn read_other = nullptr;
        GetComponentFn get_component = nullptr;
        std::function<void(const std::string&)> log;
        // Runs fn after the server world's next simulation step, on its thread, with that world.
        std::function<void(std::function<void(EmfWorld*)>)> after_step;
        std::function<uint64_t()> now_ms;  // wall clock (the watchdog)
    };
    explicit InventoryService(Deps deps);
    ~InventoryService();
    InventoryService(const InventoryService&) = delete;
    InventoryService& operator=(const InventoryService&) = delete;

    // The emf_inventory program (server world); its callback is run() through `this`.
    ProgramSpec spec();
    // Queues a request (world must be the server world the caller got). False and *why when refused.
    bool request(const PutRequest& r, std::string* why);
    // The program callback (inside the program call).
    void run(void* program_context);
    // A new server world is being set up: pending requests end with added 0.
    void world_reset();
    // Call after every server world step: when requests wait but emf_inventory has not run in this world for
    // kInventoryWatchdogMs (left out of the world, never scheduled, or faulted), they end with added 0 (logged).
    void on_server_tick(EmfWorld* world);
    bool enabled() const;
    size_t pending() const;
    PutRequest pending_at(size_t i) const;  // a copy of a waiting request (tests, diagnostics)
    // While a put_items callback runs: why fewer items than requested were added ("" if all were). Its thread only.
    static std::string delivering_reason();
    // Where iter_begin puts the program's handles, from its own singleton and accessor lists.
    IterLayout layout();

private:
    struct Done {
        PutRequest req;
        uint32_t added;
        std::string why;  // why fewer than requested were added
    };
    bool gather(const ProgramCall& call, InventoryHandles& h, std::string& why);
    bool check_target(const ProgramCall& call, uint32_t entity, std::string& why);
    bool check_slot(const ProgramCall& call, const PutRequest& r, std::string& why);
    const void* read_comp(const ProgramCall& call, const char* name, uint32_t entity, bool& bad, std::string& why);
    uint32_t put_one(const ProgramCall& call, const InventoryHandles& h, const PutRequest& r, std::string& why);
    void deliver(std::vector<Done> done);
    void say(const std::string& line);

    Deps deps_;
    uint8_t* tx_ = nullptr;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<PutRequest> pending_;
    bool enabled_ = true;
    SRWLOCK say_lock_ = SRWLOCK_INIT;
    LogLimiter said_;                // the same line at most every 10 s
    void* checked_world_ = nullptr;  // the world whose accessor self-check was logged
    // Watchdog (guarded by lock_): where and when the program last ran, and since when the server world is seen.
    void* last_run_world_ = nullptr;
    uint64_t last_run_ms_ = 0;
    EmfWorld* seen_world_ = nullptr;
    uint64_t seen_since_ms_ = 0;
};

}  // namespace emf::core
