#include "inventory.h"

#include <malloc.h>

#include <algorithm>
#include <cstring>

#include "common/seh.h"
#include "emf/generated/components.hpp"

namespace emf::core {
namespace {
namespace gen = emf::gen::ecs;

void put_u64(uint8_t* p, size_t off, uint64_t v) { std::memcpy(p + off, &v, 8); }
void put_ptr(uint8_t* p, size_t off, const void* v) { put_u64(p, off, reinterpret_cast<uint64_t>(v)); }
void put_u32(uint8_t* p, size_t off, uint32_t v) { std::memcpy(p + off, &v, 4); }

// The engine calls of one transaction, POD-only so the fault guard can wrap them.
struct TxCall {
    const InventoryFns* fns;
    uint8_t* tx;
    const uint8_t* setup;
    uint32_t entity, item, count;
    TxResult result;
    uint32_t added;  // put: added; take: taken
    TxOptions opt;
};
bool guarded_tx(TxCall& c) {
    __try {
        c.fns->ctor(c.tx);
        c.fns->init(c.tx, c.setup);
        std::memset(&c.result, 0, sizeof(c.result));
        if (c.opt.take) {  // all or nothing, as the game's own callers do
            c.fns->remove_items(&c.result, c.tx, c.entity, c.item, c.opt.slot, c.count);
            c.added = (c.result.error == 0 && c.result.value == 0) ? c.count : 0;
        } else {
            OptU32 level{c.opt.level, static_cast<uint8_t>(c.opt.level ? 1 : 0), {}};
            OptU8 rarity{c.opt.rarity, static_cast<uint8_t>(c.opt.rarity != 0xFF ? 1 : 0)};
            OptF32 fuel{};
            c.fns->add_items(&c.result, c.tx, c.entity, 0, c.item, c.count, &level, &rarity, &fuel);
            c.added = (c.result.error == 0 && c.result.value < c.count) ? c.count - c.result.value : 0;
        }
        if (c.added)
            c.fns->commit(c.tx);
        else
            c.fns->rollback(c.tx);
        c.fns->free_arrays(c.tx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool guarded_callback(EmfPutItemsCallback cb, EmfWorld* w, const PutRequest& r, uint32_t added) {
    __try {
        cb(w, r.entity, r.item, r.count, added, r.user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void inventory_program(void* ctx, void* user) { static_cast<InventoryService*>(user)->run(ctx); }

const char* const kSingletonsRead[] = {"BalancingRegistry", "g38_items::ItemRegistry", "impact::AttributeRegistry"};
const char* const kSingletonWrite = "KnowledgeTriggerQueue";
const char* const kOtherWrite[] = {"Fuel",     "Inventory",         "InventoryEmptyState", "InventorySetup",
                                   "ItemState", "OwnerRelationship", "SlotSelection"};
}  // namespace

void fill_setup(uint8_t* s, const InventoryHandles& h, uint32_t entity) {
    std::memset(s, 0, kSetupSize);
    s[0x00] = kSetupFlags;
    s[0x01] = 0;  // own inventory only (no base crafting-stock chests)
    put_ptr(s, 0x08, h.program_context);
    put_ptr(s, 0x10, h.item_registry);
    put_ptr(s, 0x18, h.balancing_registry);
    put_ptr(s, 0x20, h.attribute_registry);
    put_u64(s, 0x28, h.inventory_setup);
    put_u64(s, 0x30, h.inventory);
    put_u64(s, 0x38, h.slot_selection);
    put_u64(s, 0x40, h.item_state);
    put_u64(s, 0x48, h.owner_relationship);
    put_u64(s, 0x50, h.inventory_setup);  // InventorySetup write accessor: the same handle, as inventory_actions passes
    put_u64(s, 0x58, h.inventory_empty_state);
    put_u64(s, 0x70, h.fuel);
    put_u64(s, 0xA0, h.pickup_writer);
    put_u64(s, 0xA8, h.inventory_full_writer);
    put_ptr(s, 0xC0, h.knowledge_queue);
    put_u32(s, 0xC8, entity);  // operator: the receiving entity
    put_u32(s, 0xCC, 0);       // source entity
}

PutResult run_put(const InventoryFns& fns, const InventoryHandles& h, uint8_t* tx, uint32_t entity, uint32_t item,
                  uint32_t count, const TxOptions& opt) {
    PutResult r;
    if (!fns.complete() || !tx || (opt.take && !fns.remove_items)) {
        r.faulted = true;
        return r;
    }
    uint8_t setup[kSetupSize];
    fill_setup(setup, h, entity);
    TxCall c{&fns, tx, setup, entity, item, count, {}, 0, opt};
    if (!guarded_tx(c)) {
        r.faulted = true;
        return r;
    }
    r.added = c.added;
    r.error = c.result.error;
    r.result_missing = c.result.value;
    return r;
}

InventoryService::InventoryService(Deps deps) : deps_(std::move(deps)) {
    tx_ = static_cast<uint8_t*>(_aligned_malloc(kTxSize, 16));
}

InventoryService::~InventoryService() { _aligned_free(tx_); }

ProgramSpec InventoryService::spec() {
    ProgramSpec s;
    s.name = kInventoryProgramName;
    s.world_kind = EMF_WORLD_SERVER;
    s.fn = inventory_program;
    s.user = this;
    s.events_out = {"InventoryFull", "PickupEvent"};
    for (const char* n : kSingletonsRead) s.singletons_read.emplace_back(n);
    s.singletons_write = {kSingletonWrite};
    for (const char* n : kOtherWrite) s.other_write.emplace_back(n);
    return s;
}

bool InventoryService::request(const PutRequest& r, std::string* why) {
    auto refuse = [&](const char* text) {
        if (why) *why = text;
        return false;
    };
    if (!r.world) return refuse("no server world");
    if (!r.entity) return refuse("entity 0");
    if (!r.item) return refuse("item 0");
    if (r.count == 0 || r.count > 65535) return refuse("count must be 1-65535");
    if (r.take && !deps_.fns.remove_items)
        return refuse("taking items is not available on this game build (the removeItems signature is not usable)");
    // removeItems does not bounds-check the slot index (re_take_items.md section 2)
    if (r.take && r.slot &&
        ((r.slot >> 40) != 0 || ((r.slot >> 32) & 0xFF) > 7 || static_cast<uint32_t>(r.slot) == 0))
        return refuse("slot must be an InventorySlotId: an inventory entity in bits 0-31 and a slot index 0-7 in bits 32-39");
    if (r.take && r.exact_slot && !r.slot) return refuse("an exact-slot take needs a slot");
    AcquireSRWLockExclusive(&lock_);
    const bool on = enabled_;
    const bool room = pending_.size() < kMaxPendingPuts;
    if (on && room) pending_.push_back(r);
    ReleaseSRWLockExclusive(&lock_);
    if (!on) return refuse("the inventory feature is off (an earlier call faulted)");
    if (!room) return refuse("64 requests are already waiting");
    return true;
}

bool InventoryService::enabled() const {
    AcquireSRWLockShared(&lock_);
    const bool on = enabled_;
    ReleaseSRWLockShared(&lock_);
    return on;
}

size_t InventoryService::pending() const {
    AcquireSRWLockShared(&lock_);
    const size_t n = pending_.size();
    ReleaseSRWLockShared(&lock_);
    return n;
}

PutRequest InventoryService::pending_at(size_t i) const {
    AcquireSRWLockShared(&lock_);
    const PutRequest r = i < pending_.size() ? pending_[i] : PutRequest{};
    ReleaseSRWLockShared(&lock_);
    return r;
}

void InventoryService::say(const std::string& line) {
    const uint64_t now = deps_.now_ms ? deps_.now_ms() : 0;
    AcquireSRWLockExclusive(&say_lock_);
    const std::string out = said_.pass(line, now);
    ReleaseSRWLockExclusive(&say_lock_);
    if (!out.empty() && deps_.log) deps_.log(out);
}

namespace {
thread_local std::string t_delivering_reason;
}

std::string InventoryService::delivering_reason() { return t_delivering_reason; }

IterLayout InventoryService::layout() {
    const ProgramSpec s = spec();
    return iter_layout(s.singletons_read.size() + s.singletons_write.size(), s.other_read.size() + s.other_write.size());
}

// Reads the handles the setup block needs from the program's iterator and checks the iterator layout: the events-out
// slots must hold the job pointer and the item registry must be the world's (re_inventory_api.md section 1).
bool InventoryService::gather(const ProgramCall& call, InventoryHandles& h, std::string& why) {
    if (!call.iter) {
        why = "no iterator (IterBegin not usable)";
        return false;
    }
    auto singleton = [&](const char* name) -> void* {
        const auto slot = program_singleton_slot(call, name);
        return slot && *slot < kProgramIterQwords ? reinterpret_cast<void*>(call.iter[*slot]) : nullptr;
    };
    auto accessor = [&](const char* name) -> uint64_t {
        const auto a = program_other(call, name);
        return a && a->iter_slot < kProgramIterQwords ? call.iter[a->iter_slot] : 0;
    };
    h.program_context = call.ctx;
    h.balancing_registry = singleton("BalancingRegistry");
    h.item_registry = singleton("g38_items::ItemRegistry");
    h.attribute_registry = singleton("impact::AttributeRegistry");
    h.knowledge_queue = singleton(kSingletonWrite);
    h.fuel = accessor("Fuel");
    h.inventory = accessor("Inventory");
    h.inventory_empty_state = accessor("InventoryEmptyState");
    h.inventory_setup = accessor("InventorySetup");
    h.item_state = accessor("ItemState");
    h.owner_relationship = accessor("OwnerRelationship");
    h.slot_selection = accessor("SlotSelection");
    const IterLayout l = layout();
    const size_t events_out = l.events_in_base;  // no events-in
    void* job = nullptr;
    seh_read(call.ctx, &job, 8);
    h.pickup_writer = call.iter[events_out];
    h.inventory_full_writer = call.iter[events_out + 1];
    if (!job || h.pickup_writer != reinterpret_cast<uint64_t>(job) ||
        h.inventory_full_writer != reinterpret_cast<uint64_t>(job)) {
        why = "the events-out slots do not hold the job pointer (iterator layout?)";
        return false;
    }
    void* world_registry = call.world ? WorldView(call.world).singleton("g38_items::ItemRegistry") : nullptr;
    if (!h.item_registry || (world_registry && world_registry != h.item_registry)) {
        why = "the item registry slot does not match the world's singleton (iterator layout?)";
        return false;
    }
    if (!h.balancing_registry || !h.attribute_registry || !h.knowledge_queue || !h.inventory || !h.inventory_setup ||
        !h.inventory_empty_state || !h.slot_selection || !h.item_state || !h.owner_relationship) {
        why = "a singleton or accessor slot is empty";
        return false;
    }
    return true;
}

// Checks the accessors on the receiving entity before the first transaction: its InventorySetup and SlotSelection,
// and Inventory / InventoryEmptyState on its first linked inventory, must read the same as getComponent (a slot
// shift in the sorted accessor block would move all of them).
// One component of another entity through the program's accessor (bad = refused; why says why).
const void* InventoryService::read_comp(const ProgramCall& call, const char* name, uint32_t e, bool& bad,
                                        std::string& why) {
    const auto a = program_other(call, name);
    if (!a) {
        bad = true;
        why = std::string("'") + name + "' is not declared";
        return nullptr;
    }
    std::string problem;
    const OtherRead r = read_other(call, *a, name, e, deps_.read_other, deps_.get_component, &problem);
    if (!problem.empty() || accessor_state(call, a->index).check == AccessorCheck::Bad) {
        bad = true;
        why = problem.empty() ? std::string("the accessor for '") + name + "' was refused earlier" : problem;
    }
    return r.ptr;
}

// A take with a slot: the slot's inventory must be linked in the entity's InventorySetup (removeItems would otherwise
// start in someone else's inventory), and with exact_slot the slot must hold the item now (removeItems falls back to
// any slot of the entity, which may be another copy: re_take_items.md section 2).
bool InventoryService::check_slot(const ProgramCall& call, const PutRequest& r, std::string& why) {
    const uint32_t inv_id = static_cast<uint32_t>(r.slot);
    const uint32_t index = static_cast<uint32_t>((r.slot >> 32) & 0xFF);
    bool bad = false;
    const void* p = read_comp(call, "InventorySetup", r.entity, bad, why);
    if (bad) return false;
    gen::InventorySetup setup{};
    if (!p || !seh_read(p, &setup, sizeof(setup))) {
        why = "could not read entity " + std::to_string(r.entity) + "'s InventorySetup";
        return false;
    }
    bool owned = false;
    for (const auto& link : setup.linksEntities)
        if (link.id == inv_id) owned = true;
    if (!owned) {
        why = "inventory " + std::to_string(inv_id) + " is not one of entity " + std::to_string(r.entity) +
              "'s inventories";
        return false;
    }
    if (!r.exact_slot) return true;
    p = read_comp(call, "Inventory", inv_id, bad, why);
    if (bad) return false;
    gen::Inventory inv{};
    if (!p || !seh_read(p, &inv, sizeof(inv))) {
        why = "could not read inventory " + std::to_string(inv_id);
        return false;
    }
    if (inv.slots[index].id.value != r.item) {
        why = "slot " + std::to_string(index) + " of inventory " + std::to_string(inv_id) +
              " does not hold the item (it changed)";
        return false;
    }
    return true;
}

bool InventoryService::check_target(const ProgramCall& call, uint32_t entity, std::string& why) {
    auto read = [&](const char* name, uint32_t e, bool& b) -> void* {
        return const_cast<void*>(read_comp(call, name, e, b, why));
    };
    bool bad = false;
    const auto* setup = static_cast<const gen::InventorySetup*>(read("InventorySetup", entity, bad));
    if (bad) return false;
    if (!setup) {
        why = "entity " + std::to_string(entity) + " has no inventory (InventorySetup), or it is not readable yet";
        return false;
    }
    gen::InventorySetup copy{};
    if (!seh_read(setup, &copy, sizeof(copy))) {
        why = "could not read entity " + std::to_string(entity) + "'s InventorySetup";
        return false;
    }
    read("SlotSelection", entity, bad);
    if (bad) return false;
    uint32_t child = 0;
    for (const auto& link : copy.linksEntities)
        if (link.id) {
            child = link.id;
            break;
        }
    if (child) {
        read("Inventory", child, bad);
        if (!bad) read("InventoryEmptyState", child, bad);
        if (bad) return false;
    }
    // The transaction may only run once the accessor block is known to be in place.
    const auto setup_access = program_other(call, "InventorySetup");
    const auto inv_access = program_other(call, "Inventory");
    if (accessor_state(call, setup_access->index).check != AccessorCheck::Good ||
        accessor_state(call, inv_access->index).check != AccessorCheck::Good) {
        why = "entity " + std::to_string(entity) + "'s inventory could not be checked yet (no linked inventory)";
        return false;
    }
    if (checked_world_ != call.world) {  // once per world
        checked_world_ = call.world;
        if (deps_.log)
            deps_.log("put_items: inventory accessors checked on entity " + std::to_string(entity) +
                      " (InventorySetup, SlotSelection, Inventory, InventoryEmptyState read as getComponent)");
    }
    return true;
}

uint32_t InventoryService::put_one(const ProgramCall& call, const InventoryHandles& h, const PutRequest& r,
                                   std::string& why) {
    if (r.world != reinterpret_cast<EmfWorld*>(call.world)) {
        why = "the world changed";
        say("put_items: a request for another world was dropped (the world changed)");
        return 0;
    }
    if (!check_target(call, r.entity, why)) {
        say("put_items: " + why);
        return 0;
    }
    if (r.take && r.slot && !check_slot(call, r, why)) {
        say("take_items: " + why);
        return 0;
    }
    const PutResult res = run_put(deps_.fns, h, tx_, r.entity, r.item, r.count, {r.take, r.slot, r.level, r.rarity});
    if (res.faulted) {
        AcquireSRWLockExclusive(&lock_);
        enabled_ = false;
        ReleaseSRWLockExclusive(&lock_);
        why = "the game's inventory transaction faulted; put_items is off until the game restarts";
        say("put_items: " + why);
        return 0;
    }
    if (res.error) {
        char b[8];
        snprintf(b, sizeof(b), "%02x", res.error);
        why = std::string("the game refused it (error 0x") + b + ")";
        say("put_items: the game refused a request (error 0x" + std::string(b) + ")");
    } else if (r.take && res.added < r.count) {
        why = "entity " + std::to_string(r.entity) + " does not hold " + std::to_string(r.count) + " of the item (" +
              std::to_string(res.result_missing) + " missing)";
    } else if (res.added < r.count) {
        why = res.added ? "only " + std::to_string(res.added) + " of " + std::to_string(r.count) + " fit: no room for the rest"
                        : "none of " + std::to_string(r.count) + " fit: no room";
    }
    return res.added;
}

void InventoryService::run(void* program_context) {
    const ProgramCall* call = current_program_call();
    if (!call || call->ctx != program_context) return;
    std::vector<PutRequest> work;
    const uint64_t now = deps_.now_ms ? deps_.now_ms() : 0;
    AcquireSRWLockExclusive(&lock_);
    last_run_world_ = call->world;
    last_run_ms_ = now;
    const size_t n = (std::min)(pending_.size(), kPutsPerTick);
    work.assign(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
    const bool on = enabled_;
    ReleaseSRWLockExclusive(&lock_);
    if (work.empty()) return;
    std::vector<Done> done;
    InventoryHandles h;
    std::string why;
    const bool ready = on && gather(*call, h, why);
    if (on && !ready) say("put_items: " + why);
    for (const auto& r : work) {
        const bool still_on = enabled();
        if (!on || !still_on) {
            done.push_back({r, 0, "put_items is off (an earlier transaction faulted)"});
        } else if (!ready) {
            done.push_back({r, 0, why});
        } else {
            std::string one_why;
            const uint32_t added = put_one(*call, h, r, one_why);
            done.push_back({r, added, one_why});
        }
    }
    deliver(std::move(done));
}

void InventoryService::world_reset() {
    std::vector<Done> done;
    AcquireSRWLockExclusive(&lock_);
    for (const auto& r : pending_) done.push_back({r, 0, "a new world was set up"});
    pending_.clear();
    ReleaseSRWLockExclusive(&lock_);
    checked_world_ = nullptr;  // a reload often reuses the world's address: check (and say so) again
    deliver(std::move(done));
}

void InventoryService::on_server_tick(EmfWorld* world) {
    if (!world) return;
    const uint64_t now = deps_.now_ms ? deps_.now_ms() : 0;
    std::vector<Done> done;
    AcquireSRWLockExclusive(&lock_);
    if (world != seen_world_) {
        seen_world_ = world;
        seen_since_ms_ = now;
    }
    const bool running = last_run_world_ == static_cast<void*>(world) && now - last_run_ms_ < kInventoryWatchdogMs;
    if (!pending_.empty() && !running && now - seen_since_ms_ >= kInventoryWatchdogMs) {
        for (const auto& r : pending_) done.push_back({r, 0, "the emf_inventory program is not running in this world"});
        pending_.clear();
    }
    ReleaseSRWLockExclusive(&lock_);
    if (done.empty()) return;
    say("put_items: the emf_inventory program is not running in this world (left out at world setup or faulted, "
        "see the log); waiting requests end with 0 added");
    deliver(std::move(done));
}

void InventoryService::deliver(std::vector<Done> done) {
    if (done.empty() || !deps_.after_step) return;
    auto log = deps_.log;
    deps_.after_step([done = std::move(done), log](EmfWorld* w) {
        for (const auto& d : done) {
            t_delivering_reason = d.added < d.req.count ? d.why : std::string();
            if (d.req.cb && !guarded_callback(d.req.cb, w, d.req, d.added) && log) log("put_items: a callback faulted");
            t_delivering_reason.clear();
        }
    });
}

}  // namespace emf::core
