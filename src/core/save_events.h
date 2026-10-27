#pragma once
#include <windows.h>

#include <cstdint>
#include <vector>

#include "emf/emf_api.h"

namespace emf::core {

// Fan-out for OnSave / OnLoad. Callbacks are SEH-guarded; a faulting one is disabled.
class SaveEvents {
public:
    explicit SaveEvents(const char* what = "save callback") : what_(what) {}
    void subscribe(EmfSaveCallback cb, void* user);
    // Calls every active subscriber; returns how many faulted (and were disabled) this time.
    int fire(uint16_t world_slot_id);
    size_t active() const;

private:
    struct Sub {
        EmfSaveCallback cb;
        void* user;
        bool enabled;
    };
    const char* what_;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<Sub> subs_;
};

// Decides when loadSavegameStep has finished a load (result byte 0 after waiting, or a new instance).
// reset() when a new server world is set up, so a reused instance address still fires.
class LoadGuard {
public:
    bool on_step(const void* inst, uint8_t result);
    void reset();

private:
    const void* last_ = nullptr;
    bool fired_ = false;
};

// After save2WriteWorld: true and *slot = the running world's slot id when the save succeeded (result byte 0).
// The offsets come from save2WriteWorld's own code (common/code_offsets.h; they moved by 0x10 in build 1273043); an
// offset of 0 means "not found" and nothing is read. Reads are fault-guarded.
bool saved_world_slot(const uint8_t* save2, uint32_t result_offset, uint32_t slot_offset, uint16_t* slot);

// After loadSavegameStep finished a load: true and *slot = the u16 world slot id of the GameServer instance, which sits
// 2 bytes before its "local world slot set" flag (offset read from loadSavegameStep's code; 0x1756 on builds 1269579
// and 1273043, so the slot is at +0x1754). A flag offset below 2 means "not found" and nothing is read.
bool loaded_world_slot(const uint8_t* inst, uint32_t slot_flag_offset, uint16_t* slot);

}  // namespace emf::core
