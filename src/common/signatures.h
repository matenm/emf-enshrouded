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
    // world settings: the GameServer's ServerInfo, which carries the settings when the instance's flag asks (API v21)
    SendServerInfo,
    // a player's XP curve and level cap (API v22: player_xp_required, player_level_cap, player_level_set)
    XpRequired, MaxPlayerLevel,
    // the game's UI scopes and the UI layer's transform stack, client only (the HUD editor; docs/engine_parts/re_ui_scopes.md)
    UiScopeOpen, UiScopeClose, UiPushTransform, UiPopTransform,
    // mod messages (API v27): the client's chat send and the outbox flush that sends the lines on
    ChatClientSend, ClientOutboxFlush,
    // the main menu's "About to join" prompt: "Join Session" posts this command (the debug bridge's join)
    PostJoinPending,
    // the game's admin kick, server side (refusing a join, [net] mods_check=refuse): the handler and its Kick call
    HandleAccountAction, SessionRequestKick,
    // the player widget's three bar calls (health, mana, stamina), client only (the HUD editor's bars; re_ui_scopes.md)
    PlayerVitalBar,
    // the game's text (item names in the player's language), client only: the pool's lookup and its one-slot search
    LocaPoolLookup, LocaSlotFind,
    // hits before they land (API v31): the game's applyHitToEntity, server world (game and dedicated server)
    ApplyHitToEntity,
    // the vitals panel's uiLayerOpen call, client only (the HUD editor's panel colour: the tint argument swapped)
    PanelLayerOpen,
    // buffs (API v32): the game's applyBuffCore, called inside EMF's emf_buffs program (game and dedicated server)
    ApplyBuffCore,
    // markers (API v33), client only: guards that prove the UI camera block (UiPlayData+0x1d20, the projection at +0x40)
    // and the game's map marker list and entry layout; the camera and list displacements are read from the match
    UiCameraBlock, MapMarkerSliceClear, MapMarkerEntryLayout, MapMarkerEntryLabel,
    // the ground under a map spot (API v34), both exes: the game's voxel ray and the scene's reset-bounds setter
    VoxelRay, ResetBoundsSet,
    // the shroud at a spot on the server (the map teleport's landing): the game's fog sampler (FogSample)
    FogSample,
    // a minimap (client): the game's map canvas MapCanvasDraw(scope, view) and the UI clip push and pop around it
    MapCanvasDraw, UiPushClip, UiPopClip,
    // path queries (API v37), both exes: create, destroy, read a result, the game's query defaults
    CreatePathQuery, DestroyPathQuery, GetPathQueryResult, PathQueryDefaults,
    // the water surface at a point on the server (a map teleport lands on the water, not the lake bed), both exes
    WaterSurfacePlane,
    // path.door (0.6.11), both exes: guards that pin the path grid layout EMF reads (it calls none of them): the door
    // table (PG_RegisterInteraction's start), metres to cells and nodes, a cell's node block and strides, and the cell
    // list (a loop inside PG_RegisterInteraction)
    PathGridRegister, PathGridBoxToCells, PathGridNodePos, PathGridCellList,
    // breaking blocks (API v44), both exes: the building-mining kernel, the voxel region read, the building config
    // lookup and the damage store's get (private research wall_damage_2026-11-15.md section 13)
    BlockKernel, VoxelRegionRead, BuildingConfigFind, VoxelDamageGet,
    // ruin protection, both exes: the game's column reader of the shipped world's voxels (the base layer under the
    // saved changes): acquire a column, seek a brick, decode it, release the column (private research
    // ruin_blocks_2026-11-16.md 4.1)
    BlockColumnAcquire, BlockBrickSeek, BlockBrickDecode, BlockColumnRelease,
    // the chat key gate: the chat window's read of the HUD screen byte in UiPlayData (movzx r15d, byte [rdi+off];
    // cmp r15b, TextChat): the offset is the u32 at +4, TextChat (0x2b) the byte at +11. Client only.
    HudScreenRead,
    _Count
};
constexpr size_t kSigCount = static_cast<size_t>(Sig::_Count);

// Game builds whose exe EMF has checked: every signature's RVA is recorded for each (SigDef::rva, same order).
struct KnownBuild {
    uint32_t number;     // the menu's "Build version"
    const char* sha256;  // the exe, lower-case hex
    bool server = false; // enshrouded_server.exe (the dedicated server of that build)
};
constexpr size_t kKnownBuildCount = 9;
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
// True when the table holds this function's RVA on that known build as checked (not kRvaUnchecked, not absent).
bool rva_checked(const SigDef& def, int build);

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
// The same results at the same RVAs of the module mapped at `to`, from the image mapped at `from` (an address of 0 stays
// 0). EMF scans the exe's file mapped as an image (pe_image.h map_image_file): another DLL's hook on a function's first
// bytes in the live module does not hide it, and calling it at the live address goes through that hook as the game's
// own calls do.
std::array<Resolved, kSigCount> rebased(std::array<Resolved, kSigCount> res, uintptr_t from, uintptr_t to);
// True if every signature marked "required" is usable.
bool required_signatures_ok(const std::array<Resolved, kSigCount>& res, bool known_build);

}  // namespace emf
