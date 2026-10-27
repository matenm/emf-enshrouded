#include "save_events.h"

#include "common/seh.h"
#include "fault_report.h"

namespace emf::core {
namespace {
bool guarded(EmfSaveCallback cb, uint16_t slot, void* user) {
    __try {
        cb(slot, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

bool saved_world_slot(const uint8_t* save2, uint32_t result_offset, uint32_t slot_offset, uint16_t* slot) {
    if (!save2 || !slot || !result_offset || !slot_offset) return false;
    uint8_t result = 0xFF;
    uint16_t id = 0;
    if (!emf::seh_read(save2 + result_offset, &result, 1) || result != 0) return false;
    if (!emf::seh_read(save2 + slot_offset, &id, 2)) return false;
    *slot = id;
    return true;
}

bool loaded_world_slot(const uint8_t* inst, uint32_t slot_flag_offset, uint16_t* slot) {
    if (!inst || !slot || slot_flag_offset < 2) return false;
    uint16_t id = 0;
    if (!emf::seh_read(inst + slot_flag_offset - 2, &id, 2)) return false;
    *slot = id;
    return true;
}

bool LoadGuard::on_step(const void* inst, uint8_t result) {
    if (!inst) return false;
    if (inst != last_) {
        last_ = inst;
        fired_ = false;
    }
    if (result != 0) {  // still loading (or failed): the next result 0 is a new load
        fired_ = false;
        return false;
    }
    if (fired_) return false;
    fired_ = true;
    return true;
}

void LoadGuard::reset() {
    last_ = nullptr;
    fired_ = false;
}

void SaveEvents::subscribe(EmfSaveCallback cb, void* user) {
    if (!cb) return;
    AcquireSRWLockExclusive(&lock_);
    subs_.push_back({cb, user, true});
    ReleaseSRWLockExclusive(&lock_);
}

int SaveEvents::fire(uint16_t world_slot_id) {
    AcquireSRWLockShared(&lock_);
    std::vector<std::pair<size_t, Sub>> live;
    for (size_t i = 0; i < subs_.size(); ++i)
        if (subs_[i].enabled) live.push_back({i, subs_[i]});
    ReleaseSRWLockShared(&lock_);
    int faults = 0;
    for (const auto& [i, s] : live) {
        if (guarded(s.cb, world_slot_id, s.user)) continue;
        ++faults;
        report_fault(what_, reinterpret_cast<const void*>(s.cb));
        AcquireSRWLockExclusive(&lock_);
        subs_[i].enabled = false;
        ReleaseSRWLockExclusive(&lock_);
    }
    return faults;
}

size_t SaveEvents::active() const {
    AcquireSRWLockShared(&lock_);
    size_t n = 0;
    for (const auto& s : subs_) n += s.enabled;
    ReleaseSRWLockShared(&lock_);
    return n;
}

}  // namespace emf::core
