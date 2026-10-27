#include "signatures.h"

#include <algorithm>
#include <cstring>

#include "pe_image.h"
#include "sigscan.h"

namespace emf {
namespace {
constexpr KnownBuild kKnownBuilds[] = {
    {1269579u, "82e5020a8736348cd0374f004b2f5e5ef456b5bd27aa5420df34c7dd241cfeeb"},  // 1.0 release (2026-10-15)
    {1273043u, "d64966e23b1a458ee6f147bcbceac0b577fa7e39a212f371287d8cc119817882"},  // update of 2026-10-21
    {1273043u, "d7baa768eb00aa895d9f5a1cd8747ccf66858c7b542f50983092b2e3261588ee", true},  // its dedicated server
};
static_assert(std::size(kKnownBuilds) == kKnownBuildCount, "one entry per known build");

// Patterns generated with tools/re/sig.py for build 1269579; each matches exactly once in .text on every known build.
// RVAs: {1269579, 1273043, 1273043 server}; 0 = the exe has no such function (client-only code on the server).
// `required`: the probe/core will not start hooking without these.
constexpr SigDef kSignatures[] = {
    {Sig::SimStep, "SimStep", "40 55 53 56 41 55 48 8D AC 24 F8 FF FD FF B8 08 01 02 00 E8 ?? ?? ?? ??", {0xa1e2e0u, 0xa1fe10u, 0x6c0fc0u}, true},
    {Sig::GetComponent, "GetComponent", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D BA C0 5D E1 00 44 89", {0x9a6190u, 0x9a7570u, 0x647df0u}, true},
    {Sig::RegisterSingleton, "RegisterSingleton", "48 89 5C 24 20 55 56 57 41 54 41 55 48 83 EC 20 4C 8B 52 08 33 F6 4C 8B", {0x9d50d0u, 0x9d68c0u, 0x677240u}, true},
    {Sig::RegisterProgram, "RegisterProgram", "48 89 5C 24 08 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC A0 00", {0x9d5770u, 0x9d6f60u, 0x6778e0u}, false},
    {Sig::CreateEntity, "CreateEntity", "4C 89 4C 24 20 4C 89 44 24 18 89 54 24 10 48 89 4C 24 08 55 56 41 54 41", {0x97b790u, 0x97cad0u, 0x61d350u}, false},
    {Sig::DestroyEntity, "DestroyEntity", "48 89 5C 24 18 48 89 74 24 20 89 54 24 10 55 57 41 55 41 56 41 57 48 8D", {0x995ad0u, 0x996eb0u, 0x637730u}, false},
    {Sig::NpcWorker, "NpcWorker", "40 53 48 83 EC 60 48 83 B9 60 01 00 00 00 48 8B D9 74 0E 48 83 B9 78 01", {0x4552c0u, 0x455640u, 0x1f1d90u}, false},
    {Sig::NpcSetState, "NpcSetState", "48 89 5C 24 10 48 89 6C 24 18 57 48 83 EC 30 48 8D B9 B0 01 00 00 48 8B", {0x453cf0u, 0x454070u, 0x1f12e0u}, false},
    {Sig::NpcSetDecision, "NpcSetDecision", "48 89 5C 24 08 57 48 83 EC 30 48 8B D9 0F B6 FA 48 8B 89 28 03 00 00 48", {0x4532a0u, 0x453620u, 0x1f0b50u}, false},
    {Sig::SetTimeOfDay, "SetTimeOfDay", "40 53 48 83 EC 20 48 8B D9 4C 89 41 10 E8 ?? ?? ?? ?? FF 43 24 48 89 43", {0xe98630u, 0xe9b1f0u, 0x94f5a0u}, false},
    {Sig::GameLog, "GameLog", "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 64 24 20 55 41 56 41 57 48 8D AC 24 40 E0 FF", {0x8a98f0u, 0x8aab60u, 0x56fe20u}, false},
    {Sig::AddLogListener, "AddLogListener", "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B FA 48 8B F1 33 DB", {0x87a300u, 0x87b570u, 0x53fdb0u}, false},
    {Sig::ClientWorldSetup, "ClientWorldSetup", "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 50 FD FF FF", {0x39fbb0u, 0x39fd50u, 0u}, false},
    {Sig::ServerWorldSetup, "ServerWorldSetup", "48 89 5C 24 20 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 20 E3 FF", {0x4949b0u, 0x494d30u, 0x22e680u}, false},
    {Sig::GetEntityTransform, "GetEntityTransform", "48 89 5C 24 08 57 48 83 EC 20 48 8D B9 C0 5D E1 00 89 54 24 38 48 8D 8F", {0x9a7160u, 0x9a8540u, 0x648dc0u}, false},
    {Sig::IterBegin, "IterBegin", "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 4C 8B 09 48", {0x9bab60u, 0x9bc1a0u, 0x65cb40u}, false},
    {Sig::IterNext, "IterNext", "40 57 48 83 EC 20 4C 8B 01 48 8B FA 8B 41 08 4C 8B C9 49 3B 80 30 08 00", {0x9b5a50u, 0x9b6e60u, 0x6576e0u}, false},
    {Sig::CurrentEntityId, "CurrentEntityId", "8B 41 08 4C 8B C2 85 C0 75 06 89 02 48 8B C2 C3 8D 50 FF 48 8B 01 48 03 D2 48 8B 88 28 08 00 00 8B 04 D1 41", {0x9b2e40u, 0x9b4230u, 0x654ab0u}, false},
    {Sig::RegisterAllPrograms, "RegisterAllPrograms", "40 53 48 83 EC 50 48 8B D9 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 40 41 B8 24", {0x4a5a30u, 0x4a5db0u, 0x23ffc0u}, false},
    {Sig::SpawnWrapper, "SpawnWrapper", "48 83 EC 58 0F 10 02 8B 15 ?? ?? ?? ?? 33 C0 41 0F 10 08 48 81 C1 C0 5D", {0x9788d0u, 0x979c10u, 0x61a490u}, false},
    {Sig::Save2WriteWorld, "Save2WriteWorld", "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 F0 F4 FF FF", {0xe030e0u, 0xe05670u, 0x724360u}, false},
    {Sig::LoadSavegameStep, "LoadSavegameStep", "48 89 5C 24 18 48 89 7C 24 20 55 48 8B EC 48 83 EC 70 48 8B", {0xe3c540u, 0xe3f030u, 0x7a8780u}, false},
    // markWritten(tracker, maskA, maskB, entityRec, u16 typeIdx, u8 variant): component change tracking
    {Sig::MarkWritten, "MarkWritten", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 30 48 8B B1 90 02 00", {0x9c5df0u, 0x9c75e0u, 0x667ec0u}, false},
    // emitEvent(buffer, u32 hash, u32 size, u16 programId) -> zeroed event or NULL (docs/engine_parts/re_events.md)
    {Sig::EmitEvent, "EmitEvent", "48 83 EC 28 4C 8B D9 41 81 F8 00 01", {0x955400u, 0x956670u, 0x5f6bb0u}, false},
    // getTemplateComponent(world, qualifiedHash, entityId) -> the entity's template data (shared by every entity of
    // the template); template map registry+0x30, per-entity pointer column archetype+0x584.
    {Sig::GetTemplateComponent, "GetTemplateComponent", "48 89 5C 24 08 57 48 83 EC 20 48 8D B9 C0 5D E1 00 89 54 24 38 48 8B 0F 48 8D 54 24 38 48 83 C1 30 41 8B D8 E8", {0x9a6f00u, 0x9a82e0u, 0x648b60u}, false},
    // queueChatMessage(uiContext, const StringView* text, u8 kind, u32 locaTag1, u32 locaTag2) -> bool: the chat box
    // sending a line the local player typed (kind 0), before any network code (docs/engine_parts/re_chat.md).
    {Sig::QueueChatMessage, "QueueChatMessage", "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 20 48 8B 81 ?? ?? ?? ?? 41 8B D9 45 0F B6 F8 4C 8B E2 48 8B A8 ?? ?? ?? ??", {0xd5ce70u, 0xd5e8f0u, 0u}, false},
    // The inventory transaction (docs/engine_parts/re_inventory_api.md section 2): tx = 0x71B0 bytes.
    // InventoryTx* ctor(tx): zeroes it and sets up its three inline arrays (a leaf function).
    {Sig::InventoryTxCtor, "InventoryTxCtor", "33 D2 33 C0 48 89 11 48 89 51 08 48 89 51 10 48 89 51 18 48 89 51 20 48 89 51 28 48 89 51 30 48 89 51 38 48 89 51 40 48 89 51 48 48 89 51 50 48 89 51 58 48 89 51 60 48 89 51 68 48 89 51 70 48 89 51 78 48", {0x21dd00u, 0x21dea0u, 0x3ba50u}, false},
    // void init(tx, const Setup* 0xF0 bytes): copies the setup block into the tx (a leaf function).
    {Sig::InventoryTxInit, "InventoryTxInit", "C6 81 A8 71 00 00 00 48 8B 42 08 48", {0x411df0u, 0x412170u, 0x1b3aa0u}, false},
    // Result* addItems(out, tx, u32 owner, u64 slot (0 = auto), u32 itemId, u32 count, OptU32* level, OptU8* rarity,
    // OptF32* fuel): queues "put" ops; out = {u8 error, .., u32 value = count that did not fit at +8}.
    {Sig::InventoryAddItems, "InventoryAddItems", "4C 89 4C 24 20 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 48 8D AC 24 60 FF FF", {0x40b170u, 0x40b4f0u, 0x1ac790u}, false},
    // void commit(tx) / void rollback(tx) / void freeArrays(tx)
    {Sig::InventoryCommit, "InventoryCommit", "40 55 53 41 55 48 8D AC 24 30 FC FF", {0x408b50u, 0x408ed0u, 0x1aa0a0u}, false},
    {Sig::InventoryRollback, "InventoryRollback", "4C 8B DC 55 56 41 55 41 56 41 57 49", {0x410e60u, 0x4111e0u, 0x1b2b10u}, false},
    {Sig::InventoryFreeArrays, "InventoryFreeArrays", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 45 33 C0 48 81 C1 40 71 00 00", {0x21de50u, 0x21dff0u, 0x3bba0u}, false},
    // void* readOther(void** out, void* worldCtx, Accessor acc, u32 entity): another entity's component through a
    // program's other-entity accessor; worldCtx = *(void**)(*(u8**)programContext + 0x30).
    {Sig::ReadOther, "ReadOther", "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 20 5B C3 CC CC CC CC CC CC CC CC CC 4C 8B D1 4D", {0x9a5e20u, 0x9a7200u, 0x647a80u}, false},
    // removeItems(out, tx, u32 owner, u32 itemId, u64 preferredSlot, u32 count): queues "take" ops (pide destroyed at commit);
    // docs/engine_parts/re_take_items.md.
    {Sig::InventoryRemoveItems, "InventoryRemoveItems", "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 48 89 54 24 10 55 41 54 41 55 41 56 41 57 48 8D AC 24 20 FF FF", {0x40a080u, 0x40a400u, 0x1ab5d0u}, false},
    // void receive(Server* s): the server's network receive loop (a dedicated server, or a game the client hosts); for
    // each chat line a player sends it calls chatLogAdd on the server's chat system *(s+0x60).
    {Sig::ChatReceive, "ChatReceive", "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? FF FF 48 81 EC ?? ?? 00 00 4C 8B ?? 48 8B 49 30 E8 ?? ?? ?? ?? 49 8B ?? 10 48 8D 85 B8 00 00 00 48 89 85 90 00 00 00", {0xe47c20u, 0xe4a710u, 0x7b45b0u}, false},
    // Result* chatLogAdd(out, ChatSystem* sys, u8 kind, const StringView* text, u32 tag1, u32 tag2, u32 playerHandle,
    // u64 accountIdHash, const OptTime* time): queues one chat line for every reader (docs/engine_parts/re_chat.md).
    {Sig::ChatLogAdd, "ChatLogAdd", "40 55 56 41 54 41 55 41 57 48 83 EC 30 49 8B 41 08 33 ED 4D 8B F9 45 0F B6 E0 48 8B F2 4C 8B E9 48 3D 00 04 00 00 76 ??", {0xe546e0u, 0xe572a0u, 0x90b330u}, false},
    // void* writeOther(void** out, Storage* s, ChangeSet* c, Accessor a, u32 entity, u8 variant): another entity's
    // component through a program's other-write accessor, marked changed (players, save) unless s is a prediction
    // copy; s = *(job+0x30), c = *(job+0x1C0), variant = *(u8*)(job+0xBB). 1269579's RVA is inferred (0x220 before
    // readOther, as on both exes of 1273043).
    {Sig::WriteOther, "WriteOther", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 49 8B E9 4D 8B F0 48 8B F2 48 8B D9 4D 85 C9 0F 84", {0x9a5c00u, 0x9a6fe0u, 0x647860u}, false},
    // void waitForWorldJobs(World* w): mov rcx, [w+0xE15D60] (the world's task group); jmp to the task group's wait,
    // which helps run the group's tasks until none is queued or running. GameServer update calls it once per frame
    // after the server world's tick (1273043 +0xe49b52, server +0x7b39c7): SimStep leaves the EntityGrid update and the
    // range queries running on the server world. EMF calls it before its own tick work (core/world_jobs.h). 1269579's
    // RVA is inferred (SimStep + 0x1160, as on 1273043; that exe is no longer available).
    {Sig::WaitWorldJobs, "WaitWorldJobs", "48 8B 89 60 5D E1 00 E9 ?? ?? ?? ??", {0xa1f440u, 0xa20f70u, 0x6c2120u}, false},
    // ---- attributes and damage, from docs/engine_parts/re_attributes_menu.md; checked on both exes of 1273043 (the 1269579 exe was
    // not at hand: kRvaUnchecked) ----
    // AttrFindResult* attrFind(out {u8 err; u32; const AttrInfo* info}, u32 attrId, const AttributeRegistry* reg):
    // AttrInfo +0 rootId, +0xC u16 slot. No allocation, no locks: safe on any thread while the registry exists.
    {Sig::AttrFind, "AttrFind", "89 54 24 10 53 48 83 EC 20 48 8B D9 48 8D 54 24 38 49 8D 48 68", {kRvaUnchecked, 0x121e150u, 0x986f90u}, false},
    // void attrAdd(AttributeComponent* comp, const AttributeRegistry* reg, s32 delta, u32 attrId): slot += delta, then the
    // derived values; no rootId check, no clamp, no markWritten. Its body has a byte-identical twin (0x21c230 on
    // 1273043), so the pattern is its one call in applyHitToEntity's Health block and the function is the call's target.
    {Sig::AttrAdd, "AttrAdd", "44 8B 0D ?? ?? ?? ?? 44 8B C6 48 8B 55 A0 49 8B CE E8 ?? ?? ?? ??", {kRvaUnchecked, 0x3e8720u, 0x18b120u}, false, 0x11,
     "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 60 41 8B F8 48 8B DA 4C 8B C2 48 8B F1 41 8B D1 48 8D 4C 24 20 E8 ?? ?? ?? ?? 0F B6 4C 24 20 33 ED 84 C9"},
    // HitCandidate* allocPendingHit(ImpactSystem* sys, u8 candidateType): a zeroed record on the pending-hit list that
    // impact_hit_conclusion applies; not atomic (call it where that program owns the ImpactSystem).
    {Sig::AllocPendingHit, "AllocPendingHit", "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 0F B6 FA 48 81 C1 10 01 00 00", {kRvaUnchecked, 0x1218bd0u, 0x9819c0u}, false},
    // Client only: the main menu (the bridge's load_world).
    // void uiFrame(void): called once per UI frame while the UI context global is set; its first instruction
    // `mov rcx, [rip+disp]` (at +0xA) names that global.
    {Sig::UiFrame, "UiFrame", "48 89 74 24 10 57 48 83 EC 40 48 8B 0D ?? ?? ?? ??", {kRvaUnchecked, 0xd8cd40u, 0u}, false},
    // pregameState(Pregame*, u8 state, u8 phase (0 enter, 1 a command, 2 exit), Cmd*): the pregame state machine's
    // handler for every state; state 2 is the main menu (its phase-1 commands go to the pregame dispatcher 0x140869090),
    // state 3 Start Play. (The stand-alone main-menu handler at 0x140868fb0 the RE named is an unreferenced copy: the
    // game never calls it; confirmed in game.)
    {Sig::PregameState, "PregameState", "40 53 48 83 EC 60 45 0F B6 D0 48 8B D9 0F B6 CA 84 D2 0F 84 ?? ?? ?? ?? 83 E9 01 0F 84 ?? ?? ?? ?? 83 E9 01 74 ?? 83 E9 01 74 ?? 83 F9 01 0F 85", {kRvaUnchecked, 0x868310u, 0u}, false},
    // bool post*(UiCtx*, ...): push one command onto the pregame command queue (UiData+0x33CC0), as the menu's buttons do.
    {Sig::PostSelectCharacter, "PostSelectCharacter", "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B 81 D0 00 00 00 41 0F B7 D8 8B FA 48 8B B0 C0 3C 03 00 4C 8D 46 18 8B 46 10 49 3B 00 74 78 8B 56 40 8B C2 83 E0 0F 74 05 2B D0 83 C2 10 4C 8B 76 30 33 C0 8B CA 4C 03 F1 49 89 06 41 89 46 08 8D 42 0C 41 C6 06 11", {kRvaUnchecked, 0xd5e310u, 0u}, false},
    {Sig::PostSelectWorld, "PostSelectWorld", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 81 D0 00 00 00 0F B7 DA 48 8B B8 C0 3C 03 00 4C 8D 47 18 8B 47 10 49 3B 00 74 7D 8B 57 40 8B C2 83 E0 0F 74 05 2B D0 83 C2 10 48 8B 77 30 8D 42 03 8B CA 48 03 F1 66 C7 06 16 00", {kRvaUnchecked, 0xd5e610u, 0u}, false},
    {Sig::PostSessionArea, "PostSessionArea", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 81 D0 00 00 00 0F B6 F2 48 8B 98 C0 3C 03 00 8B 43 10 48 3B 43 18 74 7F 44 8B 43 40 41 8B C0 83 E0 0F 74 07 44 2B C0 41 83 C0 10 48 8B 7B 30 41 8D 40 02 41 8B C8 48 03 F9 66 C7 07 27 00", {kRvaUnchecked, 0xd5e450u, 0u}, false},
    {Sig::PostStart, "PostStart", "48 89 5C 24 10 57 48 83 EC 20 48 8B 81 D0 00 00 00 48 8B 98 C0 3C 03 00 4C 8D 43 18 8B 43 10 49 3B 00 75 13 33 FF 48 85 FF 0F 95 C0 48 8B 5C 24 38 48 83 C4 20 5F C3 8B 53 40 8B C2 83 E0 0F 74 05 2B D0 83 C2 10 48 8B 7B 30 8D 42 01 8B CA 48 03 F9 C6 07 28", {kRvaUnchecked, 0xd5bc70u, 0u}, false},
    // Result* characterSelected(Result* out {u8 err; ..}, Save2*): Start Play's "is a character selected?" check
    // (err 0x1F: "No Character selected", back to the menu).
    {Sig::CharacterSelected, "CharacterSelected", "40 53 48 83 EC 20 8B 82 E8 27 00 00 4C 8B C2", {kRvaUnchecked, 0xdd45b0u, 0u}, false},
    // ---- saves ----
    // void requestServerSave(GameServerInstance* inst): if inst+0x18AA == 0, the save system *(inst+0x20) has state
    // +0x78 == 0 and its flags +0x70 have bit 2 (world running): flags |= 1, state = 1, inst+0x17B0 = now. The world save
    // the 30 s timer asks for; server thread only (docs/engine_parts/re_saves.md; the body checked on both exes of
    // 1273043; the 1269579 RVA is the RE's).
    {Sig::RequestServerSave, "RequestServerSave", "40 53 48 83 EC 20 80 B9 AA 18 00 00", {0xe438f0u, 0xe463e0u, 0x7afea0u}, false},
    // ---- teleport ----
    // Record* queuePush(CmdBuf* cb, Buf* buf = cb + 0x10, u8 kind, u32 size (the whole record), u32 entity): a zeroed,
    // 16-byte aligned record in a program job's command buffer (header: u16 size, u8 kind, the program id from cb+0x28,
    // the entity), or NULL when the buffer is full; FlushCmdBuffers applies the records at the end of the step. teleport
    // pushes the game's own add-Teleport record with it (docs/engine_parts/re_teleport.md 1.2; 215 callers; checked on
    // both exes of 1273043 and live; the 1269579 exe was not at hand).
    {Sig::QueuePush, "QueuePush", "48 89 74 24 10 57 48 83 EC 20 44 8B 52 10 48 8B FA 41 8B C2 83 E0 0F 74 07 44 2B D0 41 83 C2 10", {kRvaUnchecked, 0x953670u, 0x5f3bb0u}, false},
};
static_assert(std::size(kSignatures) == kSigCount, "one entry per Sig");
}  // namespace

std::span<const KnownBuild> known_builds() { return kKnownBuilds; }

int find_known_build(std::string_view sha256) {
    const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    for (size_t i = 0; i < kKnownBuildCount; ++i) {
        const std::string_view known = kKnownBuilds[i].sha256;
        if (sha256.size() != known.size()) continue;
        size_t k = 0;
        while (k < known.size() && lower(sha256[k]) == known[k]) ++k;
        if (k == known.size()) return static_cast<int>(i);
    }
    return -1;
}

int known_build_index(uint32_t number, bool server) {
    for (size_t i = 0; i < kKnownBuildCount; ++i)
        if (kKnownBuilds[i].number == number && kKnownBuilds[i].server == server) return static_cast<int>(i);
    return -1;
}

bool absent_on(const SigDef& def, int build) {
    return build >= 0 && build < static_cast<int>(kKnownBuildCount) && def.rva[static_cast<size_t>(build)] == 0;
}

SigReport report_signature(const SigDef& def, const Resolved& r, int build, bool server_exe) {
    SigReport out;
    const bool known = build >= 0;
    if (is_usable(r, known)) {
        out.usable = true;
        return out;
    }
    if (absent_on(def, build)) {
        out.absent = true;
        return out;
    }
    bool absent_on_known_server = false;
    for (size_t i = 0; i < kKnownBuildCount; ++i)
        absent_on_known_server |= kKnownBuilds[i].server && def.rva[i] == 0;
    if (!known && server_exe && r.count == 0 && absent_on_known_server) {
        out.absent = true;
        out.note = std::string("signature ") + def.name + " not found (client-only code is absent from the server)";
        return out;
    }
    out.note = std::string("signature ") + def.name + " unusable (matches=" + std::to_string(r.count) + ")";
    return out;
}

std::span<const SigDef> all_signatures() { return kSignatures; }

const SigDef* find_signature(std::string_view name) {
    for (const auto& d : kSignatures)
        if (name == d.name) return &d;
    return nullptr;
}

Resolved resolve_one(std::span<const uint8_t> text, uintptr_t text_base, uintptr_t module_base, const SigDef& def,
                     int build) {
    ScanResult s = scan(text, text_base, parse_pattern(def.pattern));
    Resolved r;
    r.count = s.count;
    r.addr = s.count ? s.first : 0;
    if (s.count && def.call_at >= 0) {  // the function is the target of the call at match + call_at
        const size_t at = static_cast<size_t>(s.first - text_base) + static_cast<size_t>(def.call_at);
        bool good = false;
        if (at + 5 <= text.size() && text[at] == 0xE8) {
            int32_t rel = 0;
            std::memcpy(&rel, text.data() + at + 1, sizeof rel);
            const auto target = static_cast<uintptr_t>(static_cast<intptr_t>(text_base + at + 5) + rel);
            // the target is inside the section and (when the definition names it) the function the RE named
            good = target >= text_base && target - text_base < text.size();
            if (good && def.call_target_pattern) {
                const size_t off = static_cast<size_t>(target - text_base);
                const Pattern body = parse_pattern(def.call_target_pattern);
                const size_t n = (std::min)(body.bytes.size(), text.size() - off);
                const ScanResult m = scan(text.subspan(off, n), target, body);
                good = n == body.bytes.size() && m.count == 1 && m.first == target;
            }
            if (good) r.addr = target;
        }
        if (!good) {  // not a call, or not to the function the RE named: unusable
            r.addr = 0;
            r.count = 0;
        }
    }
    const bool known = build >= 0 && build < static_cast<int>(kKnownBuildCount);
    const uint32_t want = known ? def.rva[static_cast<size_t>(build)] : 0;
    r.rva_matches = r.count >= 1 && known && (want == kRvaUnchecked || (r.addr - module_base) == want);
    return r;
}

bool is_usable(const Resolved& r, bool known_build) {
    if (r.count != 1) return false;
    return known_build ? r.rva_matches : true;
}

std::array<Resolved, kSigCount> resolve_all(const void* module_base, int build) {
    std::array<Resolved, kSigCount> out{};
    auto text = section(module_base, ".text");
    if (!text) return out;
    std::span<const uint8_t> region(reinterpret_cast<const uint8_t*>(text->start), text->size);
    for (size_t i = 0; i < kSigCount; ++i)
        out[i] = resolve_one(region, text->start, reinterpret_cast<uintptr_t>(module_base), kSignatures[i], build);
    return out;
}

bool required_signatures_ok(const std::array<Resolved, kSigCount>& res, bool known_build) {
    for (size_t i = 0; i < kSigCount; ++i)
        if (kSignatures[i].required && !is_usable(res[i], known_build)) return false;
    return true;
}

}  // namespace emf
