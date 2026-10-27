#pragma once
// Engine struct offsets read from an instruction inside a signature-located function, for structs the game's type
// registry does not describe. A game update that moves such a field is then followed (unknown build) or caught (known
// build: the value read from code must equal the recorded one), the same rule the signatures follow.
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>
#include <span>
#include <string>
#include <utility>

#include "signatures.h"

namespace emf {

enum class CodeOffset : size_t {
    Save2Result,     // save state: 8-byte result of the last world job (byte 0: 0 = success)
    Save2WorldSlot,  // save state: u16 slot id of the running world
    InstWorldSlotFlag,  // GameServer instance: u8 "local world slot set"; the u16 slot id sits 2 bytes before it
    StorageTickPeriod,  // entity storage: i64 ns per tick (world + kStorage + this = kTickPeriodNs)
    StorageTick,        // entity storage: u32 current tick (world + kStorage + this = kTick)
    _Count
};
constexpr size_t kCodeOffsetCount = static_cast<size_t>(CodeOffset::_Count);

struct CodeOffsetDef {
    CodeOffset id;
    const char* name;
    Sig function;         // the function whose code holds the instruction
    const char* pattern;  // the instruction (sigscan syntax); must match exactly once in the window
    size_t disp_at;       // byte index of the 32-bit displacement inside the pattern
    size_t window;        // bytes searched from the function's start
    std::array<uint32_t, kKnownBuildCount> value;  // expected value on each known build
};

struct ResolvedOffset {
    uint32_t value = 0;   // the offset to use: on a known build its recorded value, else the first match (0 if none)
    uint32_t scanned = 0; // displacement of the first match in the code (0 if none)
    size_t count = 0;     // matches in the window, capped at 2
    bool value_matches = false;  // the scan found the detected known build's recorded value
    bool from_record = false;    // known build, but the scan did not confirm the record (the record is used)
    bool pair_ok = true;         // unknown build: consistent with the offsets it is read together with
};
constexpr uint32_t kMaxSaveResultSlotGap = 0x20;  // the save result sits 5 bytes after the slot on both known builds

std::span<const CodeOffsetDef> all_code_offsets();

// Reads every code offset from `text` (mapped at text_base). A function whose signature is not usable (sigs, build:
// as for the signatures; -1 = unknown build) yields count 0.
std::array<ResolvedOffset, kCodeOffsetCount> resolve_code_offsets(std::span<const uint8_t> text, uintptr_t text_base,
                                                                  const std::array<Resolved, kSigCount>& sigs,
                                                                  int build);
// The same over the .text section of the module mapped at module_base.
std::array<ResolvedOffset, kCodeOffsetCount> resolve_code_offsets(const void* module_base,
                                                                  const std::array<Resolved, kSigCount>& sigs,
                                                                  int build);
// A known build always uses its recorded value (the exe's hash pins the code; a disagreeing scan sets from_record).
// An unknown build needs exactly one match and a consistent pair (Save2Result within kMaxSaveResultSlotGap of
// Save2WorldSlot).
bool is_usable(const ResolvedOffset& r, bool known_build);

// The world's tick fields (offsets.h kTickPeriodNs / kTick) are where the compiled code expects them: always on a
// known build; on an unknown build only if SimStep's code confirms both.
bool world_tick_offsets_ok(const std::array<ResolvedOffset, kCodeOffsetCount>& r, bool known_build);

// emf_sigcheck's verdict for offset i: on a known build the scan must confirm the recorded value; on an unknown build
// it must match once (and the tick offsets must be where EMF's compiled layout expects them).
bool offset_check_ok(const std::array<ResolvedOffset, kCodeOffsetCount>& r, size_t i, bool known_build);

// One log line per offset a known build uses from its record although the scan did not confirm it, naming it.
std::vector<std::string> unconfirmed_offset_notes(const std::array<ResolvedOffset, kCodeOffsetCount>& r);

// Whether a hook that needs a signature and some code offsets can be installed, and if not, a log line naming why.
struct HookPlan {
    bool hook = false;
    std::string reason;
};
HookPlan plan_offset_hook(const char* hook_name, bool function_usable,
                          std::initializer_list<std::pair<const char*, bool>> offsets);

}  // namespace emf
