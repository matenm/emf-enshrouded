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
    // update of 2026-11-02 (game git 2c08d7bbd8f4; the number is the .kfc's version string, as for 1273043)
    {1283531u, "0403981fd2e8ad18bd8c5698dff03c8d505b5a46677832c496db9c645513f2a6"},
    {1283531u, "4429653ac0cfa154bc08e138853caa5d8557f82129b7488031f40345816aa6dc", true},  // its dedicated server
    // update of 2026-11-03 (a hotfix: the code EMF uses, its offsets and every reflected type are 1283531's)
    {1285601u, "ce6fdba63e27744943b695921383a29133285d359e07671f6ca33a47e2147618"},
    {1285601u, "c5c9fe50c6398d2adedd98923c2d04bf9d9b4af64c1447ac167cdf8f874ff84f", true},  // its dedicated server
    // update of 2026-11-04 (the code EMF uses, its offsets and every reflected type are 1285601's)
    {1287369u, "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77"},
    {1287369u, "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2", true},  // its dedicated server
};
static_assert(std::size(kKnownBuilds) == kKnownBuildCount, "one entry per known build");

// Patterns generated with tools/re/sig.py for build 1269579; each matches exactly once in .text on every known build.
// Build 1283531 moved world and UI offsets that some patterns held (docs/ENGINE_MAP.md "Game update: build 1283531"):
// those bytes are wildcards now, so each such pattern matches the code before and after that update.
// RVAs: {1269579, 1273043, 1273043 server, 1283531, 1283531 server, 1285601, 1285601 server,
// 1287369, 1287369 server}; 0 = the exe has no
// such function (client-only
// code on the server).
// `required`: the probe/core will not start hooking without these.
constexpr SigDef kSignatures[] = {
    {Sig::SimStep, "SimStep", "40 55 53 56 41 55 48 8D AC 24 F8 FF FD FF B8 08 01 02 00 E8 ?? ?? ?? ??", {0xa1e2e0u, 0xa1fe10u, 0x6c0fc0u, 0xa2c5a0u, 0x6c8f10u, 0xa2c580u, 0x6c8f10u, 0xa2c660u, 0x6c8f10u}, true},
    {Sig::GetComponent, "GetComponent", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D BA ?? ?? ?? 00 44 89", {0x9a6190u, 0x9a7570u, 0x647df0u, 0x9b2e00u, 0x64ed50u, 0x9b2de0u, 0x64ed50u, 0x9b2ec0u, 0x64ed50u}, true},
    {Sig::RegisterSingleton, "RegisterSingleton", "48 89 5C 24 20 55 56 57 41 54 41 55 48 83 EC 20 4C 8B 52 08 33 F6 4C 8B", {0x9d50d0u, 0x9d68c0u, 0x677240u, 0x9e26c0u, 0x67e720u, 0x9e26a0u, 0x67e720u, 0x9e2780u, 0x67e720u}, true},
    {Sig::RegisterProgram, "RegisterProgram", "48 89 5C 24 08 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC A0 00", {0x9d5770u, 0x9d6f60u, 0x6778e0u, 0x9e2d80u, 0x67ede0u, 0x9e2d60u, 0x67ede0u, 0x9e2e40u, 0x67ede0u}, false},
    {Sig::CreateEntity, "CreateEntity", "4C 89 4C 24 20 4C 89 44 24 18 89 54 24 10 48 89 4C 24 08 55 56 41 54 41", {0x97b790u, 0x97cad0u, 0x61d350u, 0x987d90u, 0x623ce0u, 0x987d70u, 0x623ce0u, 0x987e50u, 0x623ce0u}, false},
    {Sig::DestroyEntity, "DestroyEntity", "48 89 5C 24 18 48 89 74 24 20 89 54 24 10 55 57 41 55 41 56 41 57 48 8D", {0x995ad0u, 0x996eb0u, 0x637730u, 0x9a2950u, 0x63e8a0u, 0x9a2930u, 0x63e8a0u, 0x9a2a10u, 0x63e8a0u}, false},
    {Sig::NpcWorker, "NpcWorker", "40 53 48 83 EC 60 48 83 B9 60 01 00 00 00 48 8B D9 74 0E 48 83 B9 78 01", {0x4552c0u, 0x455640u, 0x1f1d90u, 0x45c810u, 0x1f7850u, 0x45c810u, 0x1f7850u, 0x45c8f0u, 0x1f7850u}, false},
    {Sig::NpcSetState, "NpcSetState", "48 89 5C 24 10 48 89 6C 24 18 57 48 83 EC 30 48 8D B9 B0 01 00 00 48 8B", {0x453cf0u, 0x454070u, 0x1f12e0u, 0x45b240u, 0x1f6da0u, 0x45b240u, 0x1f6da0u, 0x45b320u, 0x1f6da0u}, false},
    {Sig::NpcSetDecision, "NpcSetDecision", "48 89 5C 24 08 57 48 83 EC 30 48 8B D9 0F B6 FA 48 8B 89 28 03 00 00 48", {0x4532a0u, 0x453620u, 0x1f0b50u, 0x45a7c0u, 0x1f65b0u, 0x45a7c0u, 0x1f65b0u, 0x45a8a0u, 0x1f65b0u}, false},
    {Sig::SetTimeOfDay, "SetTimeOfDay", "40 53 48 83 EC 20 48 8B D9 4C 89 41 10 E8 ?? ?? ?? ?? FF 43 24 48 89 43", {0xe98630u, 0xe9b1f0u, 0x94f5a0u, 0xeb2400u, 0x959790u, 0xeb23e0u, 0x959820u, 0xeb24c0u, 0x959810u}, false},
    {Sig::GameLog, "GameLog", "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 64 24 20 55 41 56 41 57 48 8D AC 24 40 E0 FF", {0x8a98f0u, 0x8aab60u, 0x56fe20u, 0x8b5410u, 0x575890u, 0x8b53f0u, 0x575890u, 0x8b54d0u, 0x575890u}, false},
    {Sig::AddLogListener, "AddLogListener", "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B FA 48 8B F1 33 DB", {0x87a300u, 0x87b570u, 0x53fdb0u, 0x885dd0u, 0x545890u, 0x885db0u, 0x545890u, 0x885e90u, 0x545890u}, false},
    {Sig::ClientWorldSetup, "ClientWorldSetup", "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 50 FD FF FF", {0x39fbb0u, 0x39fd50u, 0u, 0x3a5fd0u, 0u, 0x3a5fd0u, 0u, 0x3a60b0u, 0u}, false},
    {Sig::ServerWorldSetup, "ServerWorldSetup", "48 89 5C 24 20 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 20 E3 FF", {0x4949b0u, 0x494d30u, 0x22e680u, 0x49bd30u, 0x233ee0u, 0x49bd30u, 0x233ee0u, 0x49be10u, 0x233ee0u}, false},
    {Sig::GetEntityTransform, "GetEntityTransform", "48 89 5C 24 08 57 48 83 EC 20 48 8D B9 ?? ?? ?? 00 89 54 24 38 48 8D 8F", {0x9a7160u, 0x9a8540u, 0x648dc0u, 0x9b3df0u, 0x64fd40u, 0x9b3dd0u, 0x64fd40u, 0x9b3eb0u, 0x64fd40u}, false},
    {Sig::IterBegin, "IterBegin", "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 4C 8B 09 48", {0x9bab60u, 0x9bc1a0u, 0x65cb40u, 0x9c7ac0u, 0x663b30u, 0x9c7aa0u, 0x663b30u, 0x9c7b80u, 0x663b30u}, false},
    {Sig::IterNext, "IterNext", "40 57 48 83 EC 20 4C 8B 01 48 8B FA 8B 41 08 4C 8B C9 49 3B 80 ?? 08 00", {0x9b5a50u, 0x9b6e60u, 0x6576e0u, 0x9c26c0u, 0x65e610u, 0x9c26a0u, 0x65e610u, 0x9c2780u, 0x65e610u}, false},
    {Sig::CurrentEntityId, "CurrentEntityId", "8B 41 08 4C 8B C2 85 C0 75 06 89 02 48 8B C2 C3 8D 50 FF 48 8B 01 48 03 D2 48 8B 88 ?? 08 00 00 8B 04 D1 41", {0x9b2e40u, 0x9b4230u, 0x654ab0u, 0x9bf920u, 0x65b870u, 0x9bf900u, 0x65b870u, 0x9bf9e0u, 0x65b870u}, false},
    {Sig::RegisterAllPrograms, "RegisterAllPrograms", "40 53 48 83 EC 50 48 8B D9 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 40 41 B8 24", {0x4a5a30u, 0x4a5db0u, 0x23ffc0u, 0x4acea0u, 0x245960u, 0x4acea0u, 0x245960u, 0x4acf80u, 0x245960u}, false},
    {Sig::SpawnWrapper, "SpawnWrapper", "48 83 EC 58 0F 10 02 8B 15 ?? ?? ?? ?? 33 C0 41 0F 10 08 48 81 C1 ?? ??", {0x9788d0u, 0x979c10u, 0x61a490u, 0x984db0u, 0x620d00u, 0x984d90u, 0x620d00u, 0x984e70u, 0x620d00u}, false},
    {Sig::Save2WriteWorld, "Save2WriteWorld", "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 F0 F4 FF FF", {0xe030e0u, 0xe05670u, 0x724360u, 0xe1b490u, 0x72cb30u, 0xe1b470u, 0x72cb30u, 0xe1b550u, 0x72cb30u}, false},
    {Sig::LoadSavegameStep, "LoadSavegameStep", "48 89 5C 24 18 48 89 7C 24 20 55 48 8B EC 48 83 EC 70 48 8B", {0xe3c540u, 0xe3f030u, 0x7a8780u, 0xe55ba0u, 0x7b1a10u, 0xe55b80u, 0x7b1a10u, 0xe55c60u, 0x7b1a10u}, false},
    // markWritten(tracker, maskA, maskB, entityRec, u16 typeIdx, u8 variant): component change tracking
    {Sig::MarkWritten, "MarkWritten", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 30 48 8B B1 90 02 00", {0x9c5df0u, 0x9c75e0u, 0x667ec0u, 0x9d3090u, 0x66f040u, 0x9d3070u, 0x66f040u, 0x9d3150u, 0x66f040u}, false},
    // emitEvent(buffer, u32 hash, u32 size, u16 programId) -> zeroed event or NULL (docs/engine_parts/re_events.md)
    {Sig::EmitEvent, "EmitEvent", "48 83 EC 28 4C 8B D9 41 81 F8 00 01", {0x955400u, 0x956670u, 0x5f6bb0u, 0x9613b0u, 0x5fcfc0u, 0x961390u, 0x5fcfc0u, 0x961470u, 0x5fcfc0u}, false},
    // getTemplateComponent(world, qualifiedHash, entityId) -> the entity's template data (shared by every entity of
    // the template); template map registry+0x30, per-entity pointer column archetype+0x584.
    {Sig::GetTemplateComponent, "GetTemplateComponent", "48 89 5C 24 08 57 48 83 EC 20 48 8D B9 ?? ?? ?? 00 89 54 24 38 48 8B 0F 48 8D 54 24 38 48 83 C1 30 41 8B D8 E8", {0x9a6f00u, 0x9a82e0u, 0x648b60u, 0x9b3b90u, 0x64fae0u, 0x9b3b70u, 0x64fae0u, 0x9b3c50u, 0x64fae0u}, false},
    // queueChatMessage(uiContext, const StringView* text, u8 kind, u32 locaTag1, u32 locaTag2) -> bool: the chat box
    // sending a line the local player typed (kind 0), before any network code (docs/engine_parts/re_chat.md).
    {Sig::QueueChatMessage, "QueueChatMessage", "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 20 48 8B 81 ?? ?? ?? ?? 41 8B D9 45 0F B6 F8 4C 8B E2 48 8B A8 ?? ?? ?? ??", {0xd5ce70u, 0xd5e8f0u, 0u, 0xd72cd0u, 0u, 0xd72cb0u, 0u, 0xd72d90u, 0u}, false},
    // The inventory transaction (docs/engine_parts/re_inventory_api.md section 2): tx = 0x71B0 bytes.
    // InventoryTx* ctor(tx): zeroes it and sets up its three inline arrays (a leaf function).
    {Sig::InventoryTxCtor, "InventoryTxCtor", "33 D2 33 C0 48 89 11 48 89 51 08 48 89 51 10 48 89 51 18 48 89 51 20 48 89 51 28 48 89 51 30 48 89 51 38 48 89 51 40 48 89 51 48 48 89 51 50 48 89 51 58 48 89 51 60 48 89 51 68 48 89 51 70 48 89 51 78 48", {0x21dd00u, 0x21dea0u, 0x3ba50u, 0x221910u, 0x3ed10u, 0x221910u, 0x3ed10u, 0x2219f0u, 0x3ed10u}, false},
    // void init(tx, const Setup* 0xF0 bytes): copies the setup block into the tx (a leaf function).
    {Sig::InventoryTxInit, "InventoryTxInit", "C6 81 A8 71 00 00 00 48 8B 42 08 48", {0x411df0u, 0x412170u, 0x1b3aa0u, 0x419060u, 0x1b9300u, 0x419060u, 0x1b9300u, 0x419140u, 0x1b9300u}, false},
    // Result* addItems(out, tx, u32 owner, u64 slot (0 = auto), u32 itemId, u32 count, OptU32* level, OptU8* rarity,
    // OptF32* fuel): queues "put" ops; out = {u8 error, .., u32 value = count that did not fit at +8}.
    {Sig::InventoryAddItems, "InventoryAddItems", "4C 89 4C 24 20 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 48 8D AC 24 60 FF FF", {0x40b170u, 0x40b4f0u, 0x1ac790u, 0x412380u, 0x1b1f60u, 0x412380u, 0x1b1f60u, 0x412460u, 0x1b1f60u}, false},
    // void commit(tx) / void rollback(tx) / void freeArrays(tx)
    {Sig::InventoryCommit, "InventoryCommit", "40 55 53 41 55 48 8D AC 24 30 FC FF", {0x408b50u, 0x408ed0u, 0x1aa0a0u, 0x40fd50u, 0x1af860u, 0x40fd50u, 0x1af860u, 0x40fe30u, 0x1af860u}, false},
    {Sig::InventoryRollback, "InventoryRollback", "4C 8B DC 55 56 41 55 41 56 41 57 49", {0x410e60u, 0x4111e0u, 0x1b2b10u, 0x418070u, 0x1b8310u, 0x418070u, 0x1b8310u, 0x418150u, 0x1b8310u}, false},
    {Sig::InventoryFreeArrays, "InventoryFreeArrays", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 45 33 C0 48 81 C1 40 71 00 00", {0x21de50u, 0x21dff0u, 0x3bba0u, 0x221a60u, 0x3ee60u, 0x221a60u, 0x3ee60u, 0x221b40u, 0x3ee60u}, false},
    // void* readOther(void** out, void* worldCtx, Accessor acc, u32 entity): another entity's component through a
    // program's other-entity accessor; worldCtx = *(void**)(*(u8**)programContext + 0x30).
    {Sig::ReadOther, "ReadOther", "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 20 5B C3 CC CC CC CC CC CC CC CC CC 4C 8B D1 4D", {0x9a5e20u, 0x9a7200u, 0x647a80u, 0x9b2a90u, 0x64e9e0u, 0x9b2a70u, 0x64e9e0u, 0x9b2b50u, 0x64e9e0u}, false},
    // removeItems(out, tx, u32 owner, u32 itemId, u64 preferredSlot, u32 count): queues "take" ops (pide destroyed at commit);
    // docs/engine_parts/re_take_items.md.
    {Sig::InventoryRemoveItems, "InventoryRemoveItems", "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 48 89 54 24 10 55 41 54 41 55 41 56 41 57 48 8D AC 24 20 FF FF", {0x40a080u, 0x40a400u, 0x1ab5d0u, 0x411280u, 0x1b0d90u, 0x411280u, 0x1b0d90u, 0x411360u, 0x1b0d90u}, false},
    // void receive(Server* s): the server's network receive loop (a dedicated server, or a game the client hosts); for
    // each chat line a player sends it calls chatLogAdd on the server's chat system *(s+0x60).
    {Sig::ChatReceive, "ChatReceive", "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? FF FF 48 81 EC ?? ?? 00 00 4C 8B ?? 48 8B 49 30 E8 ?? ?? ?? ?? 49 8B ?? 10 48 8D 85 B8 00 00 00 48 89 85 90 00 00 00", {0xe47c20u, 0xe4a710u, 0x7b45b0u, 0xe61560u, 0x7bdb20u, 0xe61540u, 0x7bdb20u, 0xe61620u, 0x7bdb20u}, false},
    // Result* chatLogAdd(out, ChatSystem* sys, u8 kind, const StringView* text, u32 tag1, u32 tag2, u32 playerHandle,
    // u64 accountIdHash, const OptTime* time): queues one chat line for every reader (docs/engine_parts/re_chat.md).
    {Sig::ChatLogAdd, "ChatLogAdd", "40 55 56 41 54 41 55 41 57 48 83 EC 30 49 8B 41 08 33 ED 4D 8B F9 45 0F B6 E0 48 8B F2 4C 8B E9 48 3D 00 04 00 00 76 ??", {0xe546e0u, 0xe572a0u, 0x90b330u, 0xe6e100u, 0x915170u, 0xe6e0e0u, 0x915200u, 0xe6e1c0u, 0x9151f0u}, false},
    // void* writeOther(void** out, Storage* s, ChangeSet* c, Accessor a, u32 entity, u8 variant): another entity's
    // component through a program's other-write accessor, marked changed (players, save) unless s is a prediction
    // copy; s = *(job+0x30), c = *(job+0x1C0), variant = *(u8*)(job+0xBB). 1269579's RVA is inferred (0x220 before
    // readOther, as on both exes of 1273043).
    {Sig::WriteOther, "WriteOther", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 49 8B E9 4D 8B F0 48 8B F2 48 8B D9 4D 85 C9 0F 84", {0x9a5c00u, 0x9a6fe0u, 0x647860u, 0x9b2870u, 0x64e7c0u, 0x9b2850u, 0x64e7c0u, 0x9b2930u, 0x64e7c0u}, false},
    // void waitForWorldJobs(World* w): mov rcx, [w+0xE15D60] (the world's task group; +0xE17960 on 1283531, the pattern
    // keeps the disp32's upper half 0x00E1 so it stays unique); jmp to the task group's wait,
    // which helps run the group's tasks until none is queued or running. GameServer update calls it once per frame
    // after the server world's tick (1273043 +0xe49b52, server +0x7b39c7): SimStep leaves the EntityGrid update and the
    // range queries running on the server world. EMF calls it before its own tick work (core/world_jobs.h). 1269579's
    // RVA is inferred (SimStep + 0x1160, as on 1273043; that exe is no longer available).
    {Sig::WaitWorldJobs, "WaitWorldJobs", "48 8B 89 ?? ?? E1 00 E9 ?? ?? ?? ??", {0xa1f440u, 0xa20f70u, 0x6c2120u, 0xa2d710u, 0x6ca080u, 0xa2d6f0u, 0x6ca080u, 0xa2d7d0u, 0x6ca080u}, false},
    // ---- attributes and damage, from docs/engine_parts/re_attributes_menu.md; checked on both exes of 1273043 (the 1269579 exe was
    // not at hand: kRvaUnchecked) ----
    // AttrFindResult* attrFind(out {u8 err; u32; const AttrInfo* info}, u32 attrId, const AttributeRegistry* reg):
    // AttrInfo +0 rootId, +0xC u16 slot. No allocation, no locks: safe on any thread while the registry exists.
    {Sig::AttrFind, "AttrFind", "89 54 24 10 53 48 83 EC 20 48 8B D9 48 8D 54 24 38 49 8D 48 68", {kRvaUnchecked, 0x121e150u, 0x986f90u, 0x123b600u, 0x991790u, 0x123a860u, 0x991820u, 0x123a930u, 0x991810u}, false},
    // void attrAdd(AttributeComponent* comp, const AttributeRegistry* reg, s32 delta, u32 attrId): slot += delta, then the
    // derived values; no rootId check, no clamp, no markWritten. Its body has a byte-identical twin (0x21c230 on
    // 1273043), so the pattern is its one call in applyHitToEntity's Health block and the function is the call's target.
    {Sig::AttrAdd, "AttrAdd", "44 8B 0D ?? ?? ?? ?? 44 8B C6 48 8B 55 A0 49 8B CE E8 ?? ?? ?? ??", {kRvaUnchecked, 0x3e8720u, 0x18b120u, 0x3ef490u, 0x190810u, 0x3ef490u, 0x190810u, 0x3ef570u, 0x190810u}, false, 0x11,
     "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 60 41 8B F8 48 8B DA 4C 8B C2 48 8B F1 41 8B D1 48 8D 4C 24 20 E8 ?? ?? ?? ?? 0F B6 4C 24 20 33 ED 84 C9"},
    // HitCandidate* allocPendingHit(ImpactSystem* sys, u8 candidateType): a zeroed record on the pending-hit list that
    // impact_hit_conclusion applies; not atomic (call it where that program owns the ImpactSystem). On 1283531 a full
    // list logs "[impact] Could not add HitCandidate Event" and returns NULL (a bigger frame: sub rsp is a wildcard).
    {Sig::AllocPendingHit, "AllocPendingHit", "48 89 5C 24 08 57 48 83 EC ?? 48 8B D9 0F B6 FA 48 81 C1 10 01 00 00", {kRvaUnchecked, 0x1218bd0u, 0x9819c0u, 0x1235e70u, 0x98bfb0u, 0x12350d0u, 0x98c040u, 0x12351a0u, 0x98c030u}, false},
    // Client only: the main menu (the bridge's load_world).
    // void uiFrame(void): called once per UI frame while the UI context global is set; its first instruction
    // `mov rcx, [rip+disp]` (at +0xA) names that global.
    {Sig::UiFrame, "UiFrame", "48 89 74 24 10 57 48 83 EC 40 48 8B 0D ?? ?? ?? ??", {kRvaUnchecked, 0xd8cd40u, 0u, 0xda2040u, 0u, 0xda2020u, 0u, 0xda2100u, 0u}, false},
    // pregameState(Pregame*, u8 state, u8 phase (0 enter, 1 a command, 2 exit), Cmd*): the pregame state machine's
    // handler for every state; state 2 is the main menu (its phase-1 commands go to the pregame dispatcher 0x140869090),
    // state 3 Start Play. (The stand-alone main-menu handler at 0x140868fb0 the RE named is an unreferenced copy: the
    // game never calls it; confirmed in game.)
    {Sig::PregameState, "PregameState", "40 53 48 83 EC 60 45 0F B6 D0 48 8B D9 0F B6 CA 84 D2 0F 84 ?? ?? ?? ?? 83 E9 01 0F 84 ?? ?? ?? ?? 83 E9 01 74 ?? 83 E9 01 74 ?? 83 F9 01 0F 85", {kRvaUnchecked, 0x868310u, 0u, 0x872a30u, 0u, 0x872a10u, 0u, 0x872af0u, 0u}, false},
    // bool post*(UiCtx*, ...): push one command onto the pregame command queue (UiData+0x33CC0; +0x33CE0 on 1283531),
    // as the menu's buttons do.
    {Sig::PostSelectCharacter, "PostSelectCharacter", "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B 81 D0 00 00 00 41 0F B7 D8 8B FA 48 8B B0 ?? ?? 03 00 4C 8D 46 18 8B 46 10 49 3B 00 74 78 8B 56 40 8B C2 83 E0 0F 74 05 2B D0 83 C2 10 4C 8B 76 30 33 C0 8B CA 4C 03 F1 49 89 06 41 89 46 08 8D 42 0C 41 C6 06 11", {kRvaUnchecked, 0xd5e310u, 0u, 0xd726a0u, 0u, 0xd72680u, 0u, 0xd72760u, 0u}, false},
    {Sig::PostSelectWorld, "PostSelectWorld", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 81 D0 00 00 00 0F B7 DA 48 8B B8 ?? ?? 03 00 4C 8D 47 18 8B 47 10 49 3B 00 74 7D 8B 57 40 8B C2 83 E0 0F 74 05 2B D0 83 C2 10 48 8B 77 30 8D 42 03 8B CA 48 03 F1 66 C7 06 16 00", {kRvaUnchecked, 0xd5e610u, 0u, 0xd729d0u, 0u, 0xd729b0u, 0u, 0xd72a90u, 0u}, false},
    {Sig::PostSessionArea, "PostSessionArea", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 81 D0 00 00 00 0F B6 F2 48 8B 98 ?? ?? 03 00 8B 43 10 48 3B 43 18 74 7F 44 8B 43 40 41 8B C0 83 E0 0F 74 07 44 2B C0 41 83 C0 10 48 8B 7B 30 41 8D 40 02 41 8B C8 48 03 F9 66 C7 07 27 00", {kRvaUnchecked, 0xd5e450u, 0u, 0xd727f0u, 0u, 0xd727d0u, 0u, 0xd728b0u, 0u}, false},
    {Sig::PostStart, "PostStart", "48 89 5C 24 10 57 48 83 EC 20 48 8B 81 D0 00 00 00 48 8B 98 ?? ?? 03 00 4C 8D 43 18 8B 43 10 49 3B 00 75 13 33 FF 48 85 FF 0F 95 C0 48 8B 5C 24 38 48 83 C4 20 5F C3 8B 53 40 8B C2 83 E0 0F 74 05 2B D0 83 C2 10 48 8B 7B 30 8D 42 01 8B CA 48 03 F9 C6 07 28", {kRvaUnchecked, 0xd5bc70u, 0u, 0xd6fe60u, 0u, 0xd6fe40u, 0u, 0xd6ff20u, 0u}, false},
    // Result* characterSelected(Result* out {u8 err; ..}, Save2*): Start Play's "is a character selected?" check
    // (err 0x1F: "No Character selected", back to the menu).
    {Sig::CharacterSelected, "CharacterSelected", "40 53 48 83 EC 20 8B 82 E8 27 00 00 4C 8B C2", {kRvaUnchecked, 0xdd45b0u, 0u, 0xde9f00u, 0u, 0xde9ee0u, 0u, 0xde9fc0u, 0u}, false},
    // ---- saves ----
    // void requestServerSave(GameServerInstance* inst): if inst+0x18AA == 0, the save system *(inst+0x20) has state
    // +0x78 == 0 and its flags +0x70 have bit 2 (world running): flags |= 1, state = 1, inst+0x17B0 = now. The world save
    // the 30 s timer asks for; server thread only (docs/engine_parts/re_saves.md; the body checked on both exes of
    // 1273043; the 1269579 RVA is the RE's).
    {Sig::RequestServerSave, "RequestServerSave", "40 53 48 83 EC 20 80 B9 AA 18 00 00", {0xe438f0u, 0xe463e0u, 0x7afea0u, 0xe5d1b0u, 0x7b9390u, 0xe5d190u, 0x7b9390u, 0xe5d270u, 0x7b9390u}, false},
    // ---- teleport ----
    // Record* queuePush(CmdBuf* cb, Buf* buf = cb + 0x10, u8 kind, u32 size (the whole record), u32 entity): a zeroed,
    // 16-byte aligned record in a program job's command buffer (header: u16 size, u8 kind, the program id from cb+0x28,
    // the entity), or NULL when the buffer is full; FlushCmdBuffers applies the records at the end of the step. teleport
    // pushes the game's own add-Teleport record with it (docs/engine_parts/re_teleport.md 1.2; 215 callers; checked on
    // both exes of 1273043 and live; the 1269579 exe was not at hand).
    {Sig::QueuePush, "QueuePush", "48 89 74 24 10 57 48 83 EC 20 44 8B 52 10 48 8B FA 41 8B C2 83 E0 0F 74 07 44 2B D0 41 83 C2 10", {kRvaUnchecked, 0x953670u, 0x5f3bb0u, 0x95e350u, 0x5f9f60u, 0x95e330u, 0x5f9f60u, 0x95e410u, 0x5f9f60u}, false},
    // GameServer_sendServerInfo (docs/modding/world_settings.md): each frame; with the instance's send-settings flag set
    // it copies the session's ecs::GameSettings into the ServerInfo every client gets, then clears the flag. Not checked
    // on 1269579: world settings use the resync only where this RVA was checked (rva_checked).
    {Sig::SendServerInfo, "SendServerInfo", "48 8B C4 55 53 41 54 41 55 41 57 48", {kRvaUnchecked, 0xe4f110u, 0x7b9060u, 0xe65fa0u, 0x7c2610u, 0xe65f80u, 0x7c2610u, 0xe66060u, 0x7c2610u}, false},
    // u32 xpRequired(u32 level, const BalancingRegistry* reg): the XP level `level` needs before the next (pure: the
    // balancing table at reg+0x20, experienceNeedPerLevel at +0x1D8); what player_level stores as the requirement
    // (docs/research xp_levels, build 1283531; 1273043 not checked).
    {Sig::XpRequired, "XpRequired", "4C 8B 4A 20 44 8B D1 4D 85 C9 75 06 B8 01 00 00 00 C3 41 83 FA 05 77 0D 41 8D 4A FF 41 8B 84 89 D8 01 00 00 C3", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0xe97620u, 0x93e880u, 0xe97600u, 0x93e910u, 0xe976e0u, 0x93e900u}, false},
    // u32 maxPlayerLevel(const BalancingRegistry* reg): the balancing table's playerLevelCap (+0x10); pending XP at the
    // cap is thrown away by player_level.
    {Sig::MaxPlayerLevel, "MaxPlayerLevel", "48 8B 41 20 48 85 C0 75 06 B8 01 00 00 00 C3 8B 40 10 C3", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0xe99270u, 0x9404f0u, 0xe99250u, 0x940580u, 0xe99330u, 0x940570u}, false},
    // The game's UI scopes (docs/engine_parts/re_ui_scopes.md): UiScopeOpen(scope, nameInfo) and UiScopeClose(scope) around every
    // named part of the UI, and the UI layer's transform stack push and pop. Client only; checked on 1287369 alone.
    {Sig::UiScopeOpen, "UiScopeOpen", "48 89 5C 24 08 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B F9 48 8B DA 45 33 C0 48 8B 52 10 48 8B 48 18 48 89 0F 48 C7 47 08 00 00 00 00 48 C7 47 10 00 00 00 00 E8 ?? ?? ?? ?? 48 8B CF 48 89 47 08 C6 47 14 01 E8 ?? ?? ?? ?? 48 8B 4F 08 48 8B D3 48 C7 47 18 00 00 00 00 C6 47 20 00 C6 47 22 00 E8", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0xa3cb10u, 0u}, false},
    {Sig::UiScopeClose, "UiScopeClose", "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 60 80 79 20 00 48 8B D9 74 6A 48 8B 49 08 0F 29 74 24 50 66 0F 6F 35 ?? ?? ?? ?? C7 44 24 70 FF 00 FF FF C7 44 24 74 00 00 80 3F", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0xa40080u, 0u}, false},
    {Sig::UiPushTransform, "UiPushTransform", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 83 79 08 04 48 8B EA F2 0F 10 02 48 8B F9 0F B7 35 ?? ?? ?? ?? 8B 42 08 F2 0F 11 44 24 20 66 89 74 24 2C 89 44 24 28 0F 85 ?? ?? ?? ?? 48 8B CA E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 48 8B B7 E0 6C 00 00 48 8D 9E E8 04 03 00", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0x11c4aa0u, 0u}, false},
    {Sig::UiPopTransform, "UiPopTransform", "48 83 EC 28 48 8B 81 10 42 00 00 4C 8B C1 48 FF C8 48 89 81 10 42 00 00 83 79 08 04 75 41 48 85 C0 74 3C 48 8B D0 48 8B 81 08 42 00 00 0F B7 4C 50 FE 49 8B 80 E0 21 00 00 48 03 C9 0F B7 44 C8 0C 49 8B 88 E0 6C 00 00 66 39 81 2C 52 03 00 74 0E 66 89 81 2C 52 03 00 C6 81 D2 52 03 00 00 49 8B C8 E8 ?? ?? ?? ?? 49 FF 88 E8 21 00 00 48 83 C4 28 C3", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0x11c2850u, 0u}, false},
    // chatClientSend(out, ChatSystem* sys, u8 kind, const StringView* text, u32 tag1, u32 tag2): queues a line in the
    // client's chat outbox, skipping the UI's text-chat gate (docs/engine_parts/re_chat.md "B"; the dedicated server has
    // an unused copy). Checked on 1273043 (both exes), 1285601 (client) and 1287369 (both exes) with tools/re/sig.py.
    {Sig::ChatClientSend, "ChatClientSend", "48 89 6C 24 20 56 41 54 41 57 48 83 EC 20 49 8B 41 08 4D 8B F9 45 0F B6 E0 48 8B EA 48 8B F1 48 3D 00 04 00 00 76 ?? 33 D2 C6 01 12", {kRvaUnchecked, 0xe99f70u, 0x94e320u, kRvaUnchecked, kRvaUnchecked, 0xeb1140u, kRvaUnchecked, 0xeb1220u, 0x958570u}, false},
    // void clientOutboxFlush(GameClient*): sends the client's queued chat lines as ChatMessageClientMessages; called from
    // the GameClient update every frame. Client only. 12 bytes, one match on 1273043, 1285601 and 1287369.
    {Sig::ClientOutboxFlush, "ClientOutboxFlush", "40 55 53 56 48 8D AC 24 10 FC FF FF", {kRvaUnchecked, 0x867710u, 0u, kRvaUnchecked, 0u, 0x871e10u, 0u, 0x871ef0u, 0u}, false},
    // bool postJoinPending(UiCtx* ui, const StringView* password): command 0x2a, "join the pending session" (the one
    // enshrouded.exe +connect asked for), as the main menu's "About to join" prompt posts it ("Join Session", with an
    // empty password when +password gave one). The pregame dispatcher sets Client_Online and enters Start Play. Client
    // only; checked on 1287369 alone (one match).
    {Sig::PostJoinPending, "PostJoinPending", "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 20 48 8B 81 D0 00 00 00 4C 8B F2 48 8B B0 ?? ?? 03 00 8B 46 10 48 3B 46 18 0F 84 ?? ?? ?? ?? 8B 5E 40 8B C3 83 E0 0F 74 05 2B D8 83 C3 10 48 8B 7E 30 33 D2 8B CB 41 B8 01 04 00 00 48 03 F9 48 8D 4F 01 E8 ?? ?? ?? ?? 33 C0 C6 07 2A", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0xd6fa30u, 0u}, false},
    // ---- refusing a join ([net] mods_check=refuse; private research kick_player_2026-11-09.md) ----
    // void handleAccountAction(GameServer* gs, const AccountActionEvent* ev): the server's handler of an admin's
    // account action (after the world tick, on the server thread). For Kick (ev+0x14 == 0) it finds the connection of
    // the account at ev+8, skips players with CanKickBan (record +0x10 bit 0) and asks the session to kick that machine
    // (the target's game shows "kicked" and leaves; nothing is stored, they can rejoin). EMF passes Kick only, never Ban
    // or UnBan (both write the ban lists). The start through `cmp byte [rdx+14h], 2` (UnBan first); 1287369 alone.
    {Sig::HandleAccountAction, "HandleAccountAction", "48 89 5C 24 18 48 89 74 24 20 55 57 41 54 41 56 41 57 48 8D AC 24 ?? ?? FF FF 48 81 EC ?? ?? 00 00 80 7A 14 02", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0xe4b3b0u, 0x7a6e50u}, false},
    // sessionRequestKick(session, u32 machine, bool ban) through the handler's Kick call site: the CanKickBan test of
    // the record (+0x1F0), its machine handle (+0x1E4), ban = false, GameServer+0x10, the call. EMF never calls it: it
    // is the check that the handler's layout is still the one above (refuse needs both).
    {Sig::SessionRequestKick, "SessionRequestKick", "41 F6 86 F0 01 00 00 01 75 ?? 41 8B 96 E4 01 00 00 45 33 C0 49 8B 4F 10 E8", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0xa6edc0u, 0xa02c90u}, false, 0x18},
    // The player widget (doPlayerHealthWidget) calls its bar function three times inside vitalBars: health (data at
    // widget+0x18, style +0xdbf8), mana (+0xf0, +0xdc58) and stamina (+0x1c8, +0xdc28), each storing the returned rect
    // in a list of at most 4 (docs/engine_parts/re_ui_scopes.md). The pattern is the three calls: a game update that
    // moves an offset breaks it and the HUD editor's bars turn off. The function is the first call's target.
    {Sig::PlayerVitalBar, "PlayerVitalBar", "4C 8D 86 F8 DB 00 00 48 8D 57 18 48 8D 4C 24 40 E8 ?? ?? ?? ?? 0F 10 00 0F B6 45 F8 3C 04 0F 84 ?? ?? ?? ?? 48 03 C0 0F 11 44 C5 B8 FE 45 F8 45 38 27 75 ?? 4C 8D 86 58 DC 00 00 48 8D 97 F0 00 00 00 48 8D 4C 24 40 E8 ?? ?? ?? ?? 0F 10 00 0F B6 45 F8 3C 04 0F 84 ?? ?? ?? ?? 48 03 C0 0F 11 44 C5 B8 FE 45 F8 45 38 26 75 ?? 4C 8D 86 28 DC 00 00 48 8D 97 C8 01 00 00 48 8D 4C 24 40 E8", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0xc7fc40u, 0u}, false, 0x10},
    // The game's text (private research item_names_2026-11-10 §5, L1 and L5): LocaPool_lookup(out, pool, tag), the
    // lookup with the game's own fallbacks (it pins error 0x12, the null-tag id at +0x18, the slots +0xE0/+0x40/+0x90,
    // the language at +0xD8 and a slot's table at +0x20), and LocaSlot_find(out, slot, tag), one slot's binary search
    // (English while another language is active). Client only: the dedicated server has no text.
    {Sig::LocaPoolLookup, "LocaPoolLookup", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 33 FF 48 8B F2 48 8B D9 45 85 C0 75 ?? 48 89 79 08 48 8B C1 48 89 79 10 89 79 04 C6 01 12 48 8B 5C 24 48 48 8B 74 24 50 48 83 C4 30 5F C3 44 3B 42 18 75 ?? 0F 10 05 ?? ?? ?? ?? 89 79 04 48 8B C3 40 88 39 0F 11 41 08 48 8B 5C 24 48 48 8B 74 24 50 48 83 C4 30 5F C3 48 81 C2 E0 00 00 00 48 89 6C 24 40 48 89 7C 24 20 48 89 7C 24 28 48 39 7A 20 74 ?? 48 8D 4C 24 20 E8 ?? ?? ?? ?? 84 C0 75 ?? 48 8D 6E 40 39 BE D8 00 00 00 75 ?? 48 8B D5 48 8D 4C 24 20 E8 ?? ?? ?? ?? 84 C0 75 ?? 48 8D 96 90 00 00 00", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0x12169b0u, 0u}, false},
    {Sig::LocaSlotFind, "LocaSlotFind", "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 48 8B 42 20 48 8B F1 48 85 C0 0F 84 ?? ?? ?? ?? 44 8B 48 04 45 85 C9 0F 84 ?? ?? ?? ?? 48 63 10", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0x1216280u, 0u}, false},
    // hits before they land (API v31): applyHitToEntity(outPropagate, entity, ctx, hit), the function that applies one
    // landing hit to one entity, at impact_hit_conclusion's entity call (its stack copy at rbp+0x240, ctx at rbp+0x170,
    // the propagate byte at rbp+0x3E8); the target must start with the function's own prologue through the "[impact]
    // Droped Hit" string's lea. Both exes of 1287369 alone (private research hit_hooks_2026-11-11 section 7).
    {Sig::ApplyHitToEntity, "ApplyHitToEntity", "4C 8D 8D 40 02 00 00 48 8B D0 4C 8D 85 70 01 00 00 4C 8B F0 48 8D 8D E8 03 00 00 E8", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x3efd10u, 0x190fb0u}, false, 0x1B, "4C 89 44 24 18 48 89 54 24 10 55 57 41 54 41 55 41 56 41 57 48 8D AC 24 78 FF FF FF 48 81 EC 88 01 00 00 49 8B 38 4C 8B E1 49 8B 48 38 4D 8B E9 48 89 7C 24 60 4D 8B F8 4C 8B F2 E8 ?? ?? ?? ?? 84 C0 75 ?? 0F 10 05 ?? ?? ?? ?? 45 33 C9 4C 8D 44 24 70 48 8D 15"},
    // The vitals panel (doPlayerHealthWidget, after vitalBars closes): the widget loads healthColors.frameColor
    // (bundle+0xdbf8) into r9 and opens an off-screen layer with it as the tint: uiLayerOpen(state, scope, 0, UiColor tint
    // by value, enable = 1, margins*, rect = null). The function is the call's target; the HUD editor swaps the tint of
    // this one call (armed at the vitalBars close). Client only; checked on 1287369 alone (private research
    // panel_tint_2026-11-11 section 5).
    {Sig::PanelLayerOpen, "PanelLayerOpen", "4C 8B 88 C8 00 00 00 48 8D 44 24 40 48 89 44 24 28 C6 44 24 20 01 4D 8B 89 F8 DB 00 00 E8", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0xa3c8b0u, 0u}, false, 0x1d, "4C 8B DC 53 48 81 EC A0 00 00 00 33 C0 48 89 54 24 20"},
    {Sig::ApplyBuffCore, "ApplyBuffCore", "48 8B C4 48 89 58 10 4C 89 48 20 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 B8 FA FF FF 48 81 EC 10 06 00 00 4C 8B A5 70 05 00 00 49 8B F1 0F 29 70 B8 41 8B D8 0F 29", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x3b3540u, 0x15e130u}, false},
    // Markers (API v33, private research markers_2026-11-11 6.2), client only, checked on 1287369 alone. Guards, not
    // functions: each match is inside a function, and EMF reads the displacements it needs from the matched bytes.
    // The in-world waypoint marker's call of worldToUi: `add r8, 0x1d20 (view, masked) ... lea r9, [r8+0x40]
    // (projection) ... call worldToUi`.
    {Sig::UiCameraBlock, "UiCameraBlock", "49 81 C0 ?? ?? ?? ?? 89 45 80 48 8D 46 10 49 8B CA 0F 11 4C 24 70 48 89 44 24 20 4D 8D 48 40 E8", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0xcf239fu, 0u}, false},
    // clear_map_markers_for_ui's end: the three map marker lists' counts set to 0 (the main list's count at +0x36ea08 is
    // the first displacement, masked; its data pointer is 8 bytes before it and its capacity 8 bytes after).
    {Sig::MapMarkerSliceClear, "MapMarkerSliceClear", "E8 ?? ?? ?? ?? 48 8B 44 24 28 33 C9 48 89 88 ?? ?? ?? ?? 48 89 88 ?? ?? ?? ?? 48 89 88 ?? ?? ?? ?? 48", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0x30447fu, 0u}, false},
    // replicate_map_markers_for_ui filling an entry (0xA0 bytes): label tag at +0x08, position at +0x10, visibility 2 at
    // +0x34; then the label's pointer and length at +0x50 and its hash at +0x60.
    {Sig::MapMarkerEntryLayout, "MapMarkerEntryLayout", "89 46 08 0F 10 07 0F 11 46 10 F2 0F 10 4F 10 F2 0F 11 4E 20 C6 46 34 02 4C 8B 54 24", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0x316534u, 0u}, false},
    {Sig::MapMarkerEntryLabel, "MapMarkerEntryLabel", "0F 11 4E 50 F2 0F 11 46 60 48 8B 44 24", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0x31684du, 0u}, false},
    // The ground under a map spot (API v34, private research map_click_2026-11-11 3 and 6), checked on 1287369 alone.
    // The game's voxel ray: (grid, hit out, from, to, options); the realm table's stride 0x3870 is inside it.
    {Sig::VoxelRay, "VoxelRay", "48 8B C4 48 89 50 10 55 53 57 41 54 48 8D A8 58 FF FF FF 48 81 EC 88 01 00 00 41 0F 28 08 48 8B F9 F2 0F 10 05 ?? ?? ?? ?? 49 8B D9 0F 29 70 A8 4D 8B E0 41 0F 28 31 0F 29 78 98 0F 28 FE 44 0F 29 40 88", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0xa0f770u, 0x6ac7e0u}, false},
    // The scene's reset-bounds setter: the boxes outside which the game sends a player to their altar (manager+0xb0).
    {Sig::ResetBoundsSet, "ResetBoundsSet", "48 8B C4 48 89 58 18 48 89 68 20 57 48 81 EC A0 00 00 00 48 8D 99 B0 00 00 00 C6 40 E8 00 48 C7 43 08 00 00 00 00 48 8B FA 0F 10 4A 08 48 8B CB C7 40 EC 00 00 00 00 0F 10 42 20 F2 0F 10 52 18", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0xeb37f0u, 0x95b460u}, false},
    // The game's fog sampler: FogSample(out 0x50 B, FogQuery*, WorldPosition*) -> not_loaded at +0, float w[18] at +8
    // (private docs/research/fog_query_2026-11-12.md).
    {Sig::FogSample, "FogSample", "40 55 56 41 56 48 8D 6C 24 A0 48 81 EC 60 01 00 00 4D 8B 08 4C 8B F2 4D 8B 50 08 48 8B F1 49 8B 40 10 49 81 C1 00 00 00 80", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x1222180u, 0xa484b0u}, false},
    // The game's world-map canvas: MapCanvasDraw(UiScope*, const MapView*) draws the whole map over the scope's node
    // rect (+0x114) at view+0x10 UI units per metre (base zoom view+0x58); its pattern covers the frame context
    // (+0x1e: mov r8, [rip]), +0xd0 and +0x38 (private docs/research/minimap_renderer_2026-11-12.md).
    {Sig::MapCanvasDraw, "MapCanvasDraw", "40 55 53 56 57 41 55 41 56 48 8D AC 24 28 E9 FF FF B8 D8 17 00 00 E8 ?? ?? ?? ?? 48 2B E0 4C 8B 05 ?? ?? ?? ?? 45 33 ED 48 8B D9 48 8B FA 45 8B F5 49 8B 80 D0 00 00 00 48 8B 08 48 85 C9 74 ?? 4C 8B 71 38", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0xd7c690u, 0u}, false},
    // UiPushClip(layer, const float rect[4], char intersect) and UiPopClip(layer): the UI's clip stack (strictly paired).
    {Sig::UiPushClip, "UiPushClip", "48 89 5C 24 10 57 48 83 EC 60 0F 29 74 24 50 48 8B F9 48 8B 89 10 42 00 00", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0x11c3a30u, 0u}, false},
    {Sig::UiPopClip, "UiPopClip", "83 7F 14 02 75 14 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B C8 E8", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0x11c2610u, 0u}, false, 0x15, "48 FF 89 B0 01 00 00 83 79 08 04"},
    // Path queries (API v37, private research path_query_impl_2026-11-12), server world (game and dedicated server),
    // 1287369 alone. u32* createPathQuery(CmdBuf* cb, u32* outId, const PathQuery* q /* 0x110 B */): a kind-5 record
    // (0x128 B) through QueuePush; id = ++*(u32*)(*(cb+0x38)+4), 0 when the buffer is full (the bytes through the id
    // store: kind 5, size 0x128, the counter at cb+0x38).
    {Sig::CreatePathQuery, "CreatePathQuery", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 41 8B 00 49 8B D8 48 8B FA 89 44 24 20 48 8D 51 10 41 B0 05 41 B9 28 01 00 00 48 8B F1 E8 ?? ?? ?? ?? 48 8B D0 48 85 C0 75 ?? 89 07 48 8B C7 48 8B 5C 24 40 48 8B 74 24 48 48 83 C4 30 5F C3 48 8B 4E 38 B8 01 00 00 00 F0 0F C1 41 04 FF C0 48 8D 4A 10 89 42 0C", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x95fa00u, 0x5fb550u}, false},
    // bool destroyPathQuery(CmdBuf* cb, u32 entity, u32 queryId): a kind-8 record (0x10 B); the flush removes the
    // query (the entity is only logged). NOT the kind-7 twin (ResetPathQueryResults, 0x960470): only the kind byte differs.
    {Sig::DestroyPathQuery, "DestroyPathQuery", "40 53 48 83 EC 30 8B C2 41 8B D8 48 8D 51 10 89 44 24 20 41 B0 08 41 B9 10 00 00 00 E8 ?? ?? ?? ?? 48 85 C0 75 ?? 48 83 C4 30 5B C3 89 58 0C B0 01 48 83 C4 30 5B C3", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x960310u, 0x5fbe60u}, false},
    // bool getPathQueryResult(ProgramContext* ctx, Result* out /* 0x28 B */, u32 queryId, bool acceptDirty): the path
    // grid at *(job+0x28), records of 0x1A0 B, state +0x120 (3, or 4 with points when acceptDirty); the pattern covers
    // the copy into out (points +0x128/+0x130, second array +0x150/+0x158, counter +0x178, result +0x191).
    {Sig::GetPathQueryResult, "GetPathQueryResult", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 01 41 0F B6 F1 48 8B FA 44 89 44 24 40 48 8B 58 28 48 85 DB 0F 84 ?? ?? ?? ?? 48 8D 54 24 40 48 8D 8B 38 01 00 00 E8 ?? ?? ?? ?? 48 3B 83 48 01 00 00 0F 83 ?? ?? ?? ?? 48 8B 93 78 01 00 00 48 69 C8 A0 01 00 00 48 03 D1 74 ?? 0F B6 82 20 01 00 00 3C 03 74 ?? 40 84 F6 74 ?? 3C 04 75 ?? 48 83 BA 30 01 00 00 00 74 ?? 48 8B 82 28 01 00 00 48 89 07 48 8B 82 30 01 00 00 48 89 47 08 48 8B 82 50 01 00 00 48 89 47 10 48 8B 82 58 01 00 00 48 89 47 18 8B 82 78 01 00 00 89 47 20 0F B6 82 91 01 00 00 88 47 24 B0 01", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x9b7530u, 0x6533c0u}, false},
    // PathQuery* pathQueryDefaults(PathQuery* q): the game's defaults for all 0x110 bytes but the owner (+0); the
    // pattern holds the radii (+0xF0..+0x10C) and the 20 node costs (+0x6C..+0xBB) it writes.
    {Sig::PathQueryDefaults, "PathQueryDefaults", "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 66 0F 6F 05 ?? ?? ?? ?? 33 C0 C7 83 F0 00 00 00 02 00 00 00 C7 83 F4 00 00 00 02 00 00 00 C7 83 F8 00 00 00 04 00 00 00 48 C7 83 FC 00 00 00 02 00 00 00 48 89 83 04 01 00 00 89 83 0C 01 00 00 48 8B C3 0F 11 43 6C 0F 11 43 7C 0F 11 83 8C 00 00 00 0F 11 83 9C 00 00 00 0F 11 83 AC 00 00 00 48 83 C4 20 5B C3", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x982890u, 0x61e720u}, false},
    // WaterSurfacePlane(out, WaterSimulationServer*, WorldPosition*, float maxDown): the plane {0, 1, 0, d} at +0, valid
    // at +0x10; the surface height is -d (private research water_2026-11-12)
    {Sig::WaterSurfacePlane, "WaterSurfacePlane", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 50 48 8B D9 0F 29 74 24 40 48 8D 4C 24 30 41 B1 01 0F 28 F3 49 8B F8 48 8B F2 E8 ?? ?? ?? ?? 80 7C 24 3C 00 74 ?? 0F 57 C0 F3 48 0F 2A 47 08 F3 0F 59 05 ?? ?? ?? ?? F3 0F 58 44 24 34 0F 57 05 ?? ?? ?? ?? 33 C0 89 03 48 C7 43 04 00 00 80 3F", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x1121680u, 0x8a70f0u}, false},
    // path.door (0.6.11, private research path_doors_2026-11-14.md 1.1 and 2), 1287369 alone, both exes. Guards only:
    // EMF reads the PathGridSystem's memory and calls none of these. PG_RegisterInteraction(grid, {entity, kind, box}):
    // the door count u16 at grid+0xabe1c0 (0x1000 at most), entries of 0x38 B at grid+0xa861c0 ({u32 entity, u8 kind,
    // six (u32 cell, u8 node) pairs: min x y z, max x y z}), the cells object at grid+0x130, a door's box flattened to
    // its lowest node layer (kinds 1 and 2: max.y = min.y). Its start only (review M6: about 190 bytes, not the whole
    // function with its alignment fillers), so a change further down does not turn path.door off.
    {Sig::PathGridRegister, "PathGridRegister", "4C 8B DC 48 81 EC B8 00 00 00 0F B7 81 C0 E1 AB 00 41 B8 00 10 00 00 66 41 3B C0 0F 84 ?? ?? ?? ?? 49 89 5B 10 48 6B D8 38 33 C0 49 89 7B E8 4D 89 73 D0 48 81 C3 C0 61 A8 00 48 03 D9 89 03 88 43 04 66 FF 81 C0 E1 AB 00 4C 8B B1 30 01 00 00 48 8D 4C 24 20 8B 02 4D 8B C6 89 03 0F B6 42 04 88 43 04 0F B6 7A 04 48 83 C2 10 E8 ?? ?? ?? ?? 0F 10 08 0F 10 40 20 0F 10 50 10 0F 11 4C 24 50 0F 11 44 24 70 40 80 FF 02 74 ?? 40 80 FF 01 75 ?? 48 8B 44 24 58 48 89 44 24 70 0F 10 44 24 70 0F 11 4B 08 0F 11 53 18 0F 11 43 28", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x9e2b80u, 0x67eb20u}, false},
    // PG_WorldBoxToCells(out, box, cells): the world's corner (+0x120), nodes per cell across and up (+0xd1, +0xd2),
    // nodes per metre (+0xf0, +0xf4, +0xf8), the cell shifts (+0x108, +0x10c) and cell counts (+0x110, +0x114).
    {Sig::PathGridBoxToCells, "PathGridBoxToCells", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 30 F2 0F 10 1A 4D 8B F8 F3 0F 10 62 04 48 8B F2 F3 0F 10 52 08 0F 57 ED F3 41 0F 5C 90 28 01 00 00 F2 41 0F 10 88 20 01 00 00 4C 8B E1 41 8B AF 08 01 00 00 F3 0F 5C D9 41 0F B6 BF D1 00 00 00 0F 28 C1 F2 41 0F 10 8F F0 00 00 00 8B CD 45 8B B7 10 01 00 00 F3 0F 5F D5 45 8B 97 0C 01 00 00 41 FF CE 41 8B 9F 14 01 00 00 F3 0F 5F DD 45 0F B6 9F D2 00 00 00 41 8B C6 0F C6 C0 55 F3 0F 5C E0 0F 28 C1 F3 4C 0F 2C CA F3 0F 59 C3 F3 0F 5F E5 F3 48 0F 2C D3 F2 0F 10 5E 10 D3 EA 41 8B CA 41 3B D6 F3 4C 0F 2C C4 0F 46 C2 FF CB 89 44 24 10 8B C3 41 D3 E8 8B CD 44 3B C3 41 0F 46 C0 41 D3 E9 89 44 24 18 45 3B CE 41 8B C6 41 0F 46 C1 33 D2 89 44 24 20 F3 48 0F 2C C0 0F 28 C1 F7 F7 0F C6 C0 55 F3 0F 59 C4 88 54 24 14 33 D2 F3 0F 10 66 14 F3 48 0F 2C C0 F3 41 0F 10 87 F8 00 00 00 F3 0F 59 C2", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x975160u, 0x610ff0u}, false},
    // a node's world position (node, cells): a cell record's node block (+0x40; nodes at +0x28, the cells object's
    // empty block at +0x2c8 when there is none) and the node strides (+0x12c, +0x130, +0x134: u16 x, y, z).
    {Sig::PathGridNodePos, "PathGridNodePos", "48 8B C4 53 55 56 57 48 83 EC 68 44 0F B7 12 45 33 C9 4C 89 60 10 48 8B E9 44 0F B6 62 06 49 8B F0 4C 89 70 18 B9 FF FF 00 00 44 0F B6 72 04 4C 89 78 D8 44 0F B6 7A 05 44 88 78 B4 44 88 70 AC 44 88 60 BC 66 45 85 D2 74 ?? 66 44 3B D1 74 ?? 49 83 C0 78 45 8D 5A FF 41 8B D2 41 8B 40 28 4C 3B D8 72 ?? 41 8B C9 EB ?? 41 8B 48 2C 49 0F AF CB 49 03 08 8B 41 20 4D 8B D8 89 44 24 30 8B 41 24 89 44 24 38 8B 41 28 0F 10 44 24 30 89 44 24 40 F2 0F 10 4C 24 40 0F 11 44 24 48 F2 0F 11 4C 24 58 EB ?? 8B 04 25 20 00 00 00 49 83 C0 78 89 44 24 30 41 8B D2 8B 04 25 24 00 00 00 41 8B DA 89 44 24 38 4D 8B D8 8B 04 25 28 00 00 00 89 44 24 40 F2 0F 10 4C 24 40 F2 0F 11 4C 24 58 0F 10 44 24 30 0F 11 44 24 48 66 45 85 D2 74 ?? 66 44 3B D1 74 ?? 41 8B 43 28 8D 5A FF 49 8D 7B 28 8B D3 48 3B D0 73 ?? 41 8B 4B 2C 48 0F AF CA 49 03 0B EB ?? FF CB 49 8D 78 28 49 8B C9 48 8B 49 40 48 8D 86 C8 02 00 00 0F B7 96 2C 01 00 00 48 85 C9 4C 8D 59 28 4C 0F 44 D8 0F B7 86 30 01 00 00 41 0F AF C7 66 41 FF C2 4C 8B 7C 24 60 41 0F AF D6 4C 8B B4 24 A0 00 00 00 66 03 D0 0F B7 86 34 01 00 00", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x968ff0u, 0x604c00u}, false},
    // the cell list walk inside PG_RegisterInteraction (match at +0x212): the list at cells+0xb8, a cell's coords
    // (+0x20, +0x28), its state (+0x2c, dirtied to 2), the next cell (+8); the alignment filler after the null check is
    // a wildcard (review M6).
    {Sig::PathGridCellList, "PathGridCellList", "49 8B 96 B8 00 00 00 48 85 D2 74 ?? ?? ?? F2 0F 10 42 20 8B 4A 28 F2 0F 11 44 24 20 44 8B 44 24 20 45 3B C5 72 ?? 48 8B 44 24 20 48 C1 E8 20 41 3B C2 72 ?? 41 3B CF 72 ?? 44 3B C5 77 ?? 41 3B C4 77 ?? 41 3B C9 77 ?? 0F B6 4A 2C 83 E9 01 74 ?? 83 E9 02 74 ?? 83 E9 01 74 ?? 83 F9 02 75 ?? C6 42 2C 02 48 8B 52 08 48 85 D2 75 ??", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x9e2d92u, 0x67ed32u}, false},
    {Sig::BlockKernel, "BlockKernel", "48 89 5C 24 10 4C 89 4C 24 20 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 B0 C6", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x4966e0u, 0x22dc40u}, false},
    {Sig::VoxelRegionRead, "VoxelRegionRead", "48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 60 48 8B 71 08 48", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x10b20e0u, 0x836910u}, false},
    {Sig::BuildingConfigFind, "BuildingConfigFind", "48 8B 41 38 48 85 C0 74 3A 44 8B 50 0C 33 C9 4D 85 D2 74 2F 4C 63 48 08 4C 8D 58 08 45 33 C0 90 45 85 C9 75 04 33 C0 EB 04 4B 8D 04 0B 49 03 C0 38 10 74 11", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0xe8c9b0u, 0x933af0u}, false},
    {Sig::VoxelDamageGet, "VoxelDamageGet", "48 89 5C 24 08 57 48 83 EC 30 F2 0F 10 02 48 8B FA 44 8B 4A", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x4a9cf0u, 0x2426c0u}, false},
    // the shipped world's column reader (ruin protection): ColumnAcquire(handle, editor, level, bz << 32 | bx),
    // BrickSeek(handle + 0x30, by), BrickDecode(result, out slice, brick bytes), ColumnRelease(loader, resource id)
    {Sig::BlockColumnAcquire, "BlockColumnAcquire", "48 89 5C 24 18 4C 89 4C 24 20 56 57 41 54 41 56 41 57 48 83 EC 30 0F 10", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x10ab270u, 0x82f840u}, false},
    {Sig::BlockBrickSeek, "BlockBrickSeek", "48 83 EC 28 8B 41 4C 48 89 5C 24 30 48 8B D9 48 89 6C 24 38 8D 6A 01 48", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x10c6170u, 0x84adc0u}, false},
    {Sig::BlockBrickDecode, "BlockBrickDecode", "48 89 6C 24 18 48 89 74 24 20 48 89 4C 24 08 57 41 54 41 55 41 56 41 57 49 8B 30 48 8B FA 49 8B", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x10cc7d0u, 0x8514b0u}, false},
    {Sig::BlockColumnRelease, "BlockColumnRelease", "4C 8B C9 85 D2 75 05 45 33 C9 EB 5F 48 8B 89 B8 01 00 00 8B C2 25 FF FF", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0x10cc750u, 0x851430u}, false},
    {Sig::HudScreenRead, "HudScreenRead", "44 0F B6 BF ?? ?? ?? ?? 41 80 FF ?? 44 88 BD ?? ?? ?? ?? 48 C7 44 24 78 0E 00 00 00 41 0F 94 C5 0F 94 C0", {kRvaUnchecked, kRvaUnchecked, 0u, kRvaUnchecked, 0u, kRvaUnchecked, 0u, 0xcdc3dcu, 0u}, false},
    // breaking terrain a player placed (v47): TerrainConfigFind(registry, terrain material) -> its config (stride 0xB8)
    // or null; VoxelBoxFillAir(editor, 0, voxel data, &{Vec4 lo, Vec4 hi}, &u64 0, 1)
    {Sig::TerrainConfigFind, "TerrainConfigFind", "4C 8B 51 38 4D 85 D2 74 3A 45 8B 5A 04 33 C9 4D 85 DB 74 2F 4D 63 0A 45 33 C0 66 0F 1F 44 00 00 45 85 C9 75 04 33 C0 EB 04 4B 8D 04 0A 49 03 C0 38 10 74 11 48 FF C1 49 81 C0 B8 00 00 00", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0xe9f270u, 0x9464b0u}, false},
    {Sig::VoxelBoxFillAir, "VoxelBoxFillAir", "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 48 89 48 08 57 41 54 41 55 41 56 41 57 48 81 EC 00", {kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, kRvaUnchecked, 0xa9aac0u, 0xa453c0u}, false},
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

bool rva_checked(const SigDef& def, int build) {
    if (build < 0 || build >= static_cast<int>(kKnownBuildCount)) return false;
    const uint32_t rva = def.rva[static_cast<size_t>(build)];
    return rva != 0 && rva != kRvaUnchecked;
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

std::array<Resolved, kSigCount> rebased(std::array<Resolved, kSigCount> res, uintptr_t from, uintptr_t to) {
    for (auto& r : res)
        if (r.addr) r.addr = r.addr - from + to;
    return res;
}

bool required_signatures_ok(const std::array<Resolved, kSigCount>& res, bool known_build) {
    for (size_t i = 0; i < kSigCount; ++i)
        if (kSignatures[i].required && !is_usable(res[i], known_build)) return false;
    return true;
}

}  // namespace emf
