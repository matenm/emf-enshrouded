#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace emf {

enum class Sig : size_t {
    SimStep, GetComponent, RegisterSingleton, RegisterProgram, CreateEntity, DestroyEntity,
    NpcWorker, NpcSetState, NpcSetDecision, SetTimeOfDay, GameLog, AddLogListener,
    ClientWorldSetup, ServerWorldSetup, GetEntityTransform, IterBegin, IterNext, CurrentEntityId,
    RegisterAllPrograms, SpawnWrapper, Save2WriteWorld, LoadSavegameStep, MarkWritten, EmitEvent,
    GetTemplateComponent, QueueChatMessage,
    InventoryTxCtor, InventoryTxInit, InventoryAddItems, InventoryCommit, InventoryRollback, InventoryFreeArrays,
    ReadOther,
    InventoryRemoveItems,
    ChatReceive, ChatLogAdd,
    WriteOther,
    WaitWorldJobs,
    // attributes and damage on every exe (docs/engine_parts/re_attributes_menu.md)
    AttrFind, AttrAdd, AllocPendingHit,
    // the main menu's UI frame and command queue, client only (UiFrame..CharacterSelected stay together, in this order)
    UiFrame, PregameState, PostSelectCharacter, PostSelectWorld, PostSessionArea, PostStart, CharacterSelected,
    // request_save (docs/engine_parts/re_saves.md)
    RequestServerSave,
    // teleport (docs/engine_parts/re_teleport.md)
    QueuePush,
    _Count
};
constexpr size_t kSigCount = static_cast<size_t>(Sig::_Count);

// Game builds whose exe EMF has checked: every signature's RVA is recorded for each (SigDef::rva, same order).
struct KnownBuild {
    uint32_t number;     // the menu's "Build version"
    const char* sha256;  // the exe, lower-case hex
    bool server = false; // enshrouded_server.exe (the dedicated server of that build)
};
constexpr size_t kKnownBuildCount = 3;
std::span<const KnownBuild> known_builds();
// Index into known_builds() of the exe with this sha256 (hex, any case), or -1 for an unknown build.
int find_known_build(std::string_view sha256);
// Index into known_builds() of this build number's game exe (or with server=true its dedicated server), or -1.
int known_build_index(uint32_t number, bool server = false);

// SigDef::rva for a known build whose exe was not at hand when the signature was added: on that build a single match
// is accepted wherever it is (as on an unknown build); emf_sigcheck prints "unchecked".
constexpr uint32_t kRvaUnchecked = 0xFFFFFFFFu;

struct SigDef {
    Sig id;
    const char* name;
    const char* pattern;
    std::array<uint32_t, kKnownBuildCount> rva;  // expected RVA on each known build (of the function, see call_at)
    bool required;                               // the probe/core refuses to start without it
    // >= 0: the pattern matches a call site and the function is the target of the E8 rel32 call at match + call_at
    // (Resolved::addr and the RVAs are the function's). For a function whose body has a byte-identical twin.
    int call_at = -1;
    // with call_at: the target must lie inside the section and start with these bytes (the function's own pattern,
    // which its twins match too), or the signature is not usable
    const char* call_target_pattern = nullptr;
};

struct Resolved {
    uintptr_t addr = 0;     // first match (0 if none)
    size_t count = 0;       // matches found, capped at 2
    bool rva_matches = false;
};

std::span<const SigDef> all_signatures();
const SigDef* find_signature(std::string_view name);
// The known build's exe has no such function (its recorded RVA is 0: client-only code on the dedicated server).
bool absent_on(const SigDef& def, int build);

// How start-up reports one signature. absent: client-only code the dedicated server does not have, on its known build
// (counted only, no note) or on an unknown server build when it was not found and a known server build lacks it too.
// note: the log line for a signature that is not usable ("" when there is nothing to say).
struct SigReport {
    bool usable = false;
    bool absent = false;
    std::string note;
};
SigReport report_signature(const SigDef& def, const Resolved& r, int build, bool server_exe);

// Scans one signature in `text` (mapped at text_base) of a module mapped at module_base. `build` is the index of the
// known build (rva_matches compares with that build's RVA) or -1 for an unknown build (rva_matches stays false).
Resolved resolve_one(std::span<const uint8_t> text, uintptr_t text_base, uintptr_t module_base, const SigDef& def,
                     int build);
// A signature may be used if it matched exactly once and, on the known build, at the expected RVA.
bool is_usable(const Resolved& r, bool known_build);
// Resolves every signature against the .text section of the module mapped at module_base (build: as resolve_one).
std::array<Resolved, kSigCount> resolve_all(const void* module_base, int build);
// True if every signature marked "required" is usable.
bool required_signatures_ok(const std::array<Resolved, kSigCount>& res, bool known_build);

}  // namespace emf
