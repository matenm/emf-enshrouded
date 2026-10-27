# Giving items from a mod: the inventory transaction API

Build 1269579, image base 0x140000000. Static analysis only (Ghidra via rea + local scripts).
Signatures come from `tools/re/sig.py` (rel32 and RIP displacements wildcarded). Each one starts at the function entry and was checked to be unique with `sig.verify`.
This file extends `inventory_crafting.md` section 2 ("The inventory transaction") and section 5a. Read that first for the item/inventory types.

Short answer:
- The 0xF0-byte **setup block** is now fully mapped (section 1). For "add items" you need 11 fields, and `inventory_actions` has all of them in its iterator.
- Call sequence: `InventoryTx_ctor 0x14021dd00` → `InventoryTx_init 0x140411df0(tx, setup)` → `addItems 0x14040b170` → `commit 0x140408b50` (or `rollback 0x140410e60`) → `InventoryTx_freeArrays 0x14021de50`. The tx object is 0x71B0 bytes.
- `addItems` only queues **op type 0** ("stack put into slot"). Commit turns op 0 into a pide spawn for HasPIDE items, then emits knowledge triggers and a `PickupEvent`.
- **ItemId = FNV-1a-32 over the 16 raw GUID bytes**. This is confirmed: the function is `0x14021f6f0`, and `pickup_item_setup` uses it.
- **Simpler route:** spawn a loose pickup entity next to the player with `spawnPickupItem 0x1403b5230` (or `spawnEntity 0x140953770` with a `PickupItem` component). This needs only the ProgramContext, the ItemRegistry and a transform, and no transaction (section 4).

---

## 1. The setup block (0xF0 bytes) and what `InventoryTx_init` does with it

No helper function builds the setup block. Every caller fills it inline from its own program iterator:
- `inventory_actions` `0x1403fd610` fills it inline three times: redeem (+0x2a8 action), create-building-item, and upgrade-storage.
- The admin create handler `0x1403f5940` fills it from the iterator pointer it receives as its 5th argument.
- `player_crafting`'s automatic-transfer helper `0x1402c18e0` fills it from its own iterator (shown in the "player_crafting" column below).

`InventoryTx_init(tx, setup)` (`0x140411df0`, decompiled) only copies the fields into the tx. The table gives the destination as `tx[n]` (qword index into the tx).

| setup off | size | tx dest | content | needed for add? | inventory_actions iter | admin handler | player_crafting |
|---|---|---|---|---|---|---|---|
| +0x00 | u8 | +0xB0 | **flags** (below) | yes | – | 0x01 (\|0x80 fillEmptyStacks, \|0x20 backpack) | 0 |
| +0x01 | u8 | +0xB1 | stock mode (0 = own inventory, 1/2 = also base crafting-stock chests) | 0 | – | 0 | 0 |
| +0x08 | ptr | [0x00] | **ProgramContext*** | yes | pc | pc | pc |
| +0x10 | ptr | [0x01] | **g38_items::ItemRegistry*** | yes | +0xd8 | +0xd8 | iter[10] |
| +0x18 | ptr | [0x02] | BalancingRegistry* (gem-slot chances for new pides) | for PIDE items | +0xa8 | +0xa8 | iter[8] |
| +0x20 | ptr | [0x0F] | impact::AttributeRegistry* (Fuel attribute in op 2/3) | no | +0xe8 | 0 | iter[12] |
| +0x28 | acc | [0x03] | **InventorySetup** other-entity accessor (read). The write accessor also works | yes | +0x160 | +0x160 | iter[16] (other-read) |
| +0x30 | acc | [0x04] | **Inventory** other-write accessor | yes | +0x148 | +0x148 | iter[19] |
| +0x38 | acc | [0x1D] | **SlotSelection** other-write accessor | yes | +0x190 | +0x190 | iter[24] |
| +0x40 | acc | [0x05] | **ItemState** other-write accessor | yes | +0x168 | +0x168 | iter[21] |
| +0x48 | acc | [0x12] | **OwnerRelationship** other-write accessor (root-owner walk) | yes | +0x178 | +0x178 | iter[23] |
| +0x50 | acc | [0x13] | InventorySetup **write** accessor (clears links when a container pide dies) | optional | +0x160 | 0 | 0 |
| +0x58 | acc | [0x14] | **InventoryEmptyState** other-write accessor. **Required:** commit rolls back if this is NULL and any inventory was touched | yes | +0x158 | +0x158 | iter[20] |
| +0x60 | acc | [0x15] | `WeaponGemSlot` accessor (single-ItemStack holder, slot index must be 0). Fallback when an entity has no Inventory | no | 0 | 0 | 0 |
| +0x68 | acc | [0x06] | `NetworkFuel` accessor (`{u16 fuel, u16 fuelMax}`; consumeFuel op 2) | no | 0 | 0 | iter[22] |
| +0x70 | acc | [0x07] | `Fuel` accessor (attribute component, changed at commit for op 2/3) | no | 0 (+0x140 exists) | 0 | iter[18] |
| +0x78..+0x98 | 5 ptr | [0x08..0x0C] | unknown accessors. Not used by add/commit/post-commit/createPide | no | 0 | 0 | 0 |
| +0xa0 | ptr | [0x17] | **PickupEvent** writer (event writer slot) | for UI popup | +0x1a8 | +0x1a8 | iter[27] |
| +0xa8 | ptr | [0x18] | **InventoryFull** writer | optional | +0x1a0 | 0 | iter[26] |
| +0xb0 | ptr | [0x19] | `HandleEquipmentSlotChangedEvent` writer (op 7 equip) | no | 0 | 0 | 0 |
| +0xb8 | ptr | [0x1A] | unknown | no | 0 | 0 | 0 |
| +0xc0 | ptr | [0x1B] | **KnowledgeTriggerQueue*** (item "obtained" knowledge, blueprint unlocks) | recommended | +0xf0 | +0xf0 | iter[13] |
| +0xc8 | u32 | +0xE0 | **operator entity** = receiving player (`PickupEvent.pickedUpByEntityId`). PickupEvent fires only for slots inside this entity's inventory tree | yes | player | player | player |
| +0xcc | u32 | +0xE4 | source entity (`pickedUpFromEntityId`). In entity-init programs it is also the owner of spawned pides | 0 | 0 | 0 | 0 |
| +0xd0 | ptr | [0x0D] | unknown (stock-mode related, set by place_prop) | no | 0 | 0 | 0 |
| +0xd8 | ptr | [0x0E] | PlayerBasesSystemServer* (stock-mode chest search) | stock mode | +0xc8 | 0 | iter[9] |
| +0xe0/+0xe8 | float3 | [0x10]/[0x11] | stock-mode search position (CurrentTransform fixed 32.32 × 2^-32) | stock mode | – | 0 | player pos |

"iter" offsets are byte offsets into `inventory_actions`' iterator (next table). "acc" means a qword taken from the iterator (an accessor handle), copied unchanged.

**Event writer slots:** `iter_begin` (`0x1409bab60`, decompiled) fills every events-out slot with the batch descriptor `Q` (`*pc`). The event type is passed explicitly to `emitEvent 0x140955400`. So the three event slots of a program hold the same value. Any non-NULL slot of a program that declares the event works.

**Flags (setup+0x00 → tx+0xB0):**

| bit | meaning |
|---|---|
| 0x01 | emit a `PickupEvent` (hash `0x7c31fc55`, 0x30 B) per op 0/5 that lands in the operator's tree. This is the "+N item" popup |
| 0x02 | `PickupEvent.hideInUi` |
| 0x04 | emit `InventoryFull` (hash `0x689ec110`) when items did not fit (tx+0x71A8 ≠ 0). Crafting uses 0x05 |
| 0x08 / 0x10 | craft: skip outputs / skip inputs |
| 0x20 | skip the default slot finder ("prioritize backpack", from `prefersBackpack 0x14040fbb0`) |
| 0x40 | dry run: commit does a rollback |
| 0x80 | do not merge into existing stacks |

Confidence: High (init and commit decompiled; three independent builders agree). The unknown fields are not read by the add path.

### `inventory_actions` iterator (size 0x1B0), the full layout
It follows the generic rule in `ai.md` section 1 and was checked against about 20 uses in `0x1403fd610`:

| off | content | off | content |
|---|---|---|---|
| 0x000 | ProgramContext* | 0x100 | other-tpl InventoryCraftingStock |
| 0x008 | read InventorySetup* (own) | 0x108 / 0x110 / 0x118 / 0x120 | other-read Actor / **CurrentTransform** / **PlayerInput** / UsedItem |
| 0x010 | read PlayerInput* (own) | 0x128 / 0x130 / 0x138 / 0x140 | other-write ColorPaletteItem / CustomPlayerString / Durability / Fuel |
| 0x018 | write CharacterSetup (6 qw) | 0x148 | other-write **Inventory** |
| 0x048 | write ServerConsumedPlayerInput (6 qw) | 0x150 | other-write DynamicInventoryCraftingStock |
| 0x078 | write SlotSelection (own, 6 qw) | 0x158 / 0x160 / 0x168 | **InventoryEmptyState / InventorySetup / ItemState** |
| 0x0a8 / 0x0b0 / 0x0b8 / 0x0c0 | BalancingRegistry / BasicPlayerInfos / CharacterPresetRegistry / G38Knowledge | 0x170 / 0x178 / 0x180 / 0x188 | Level / **OwnerRelationship** / PerkContainerNew / RandomLootLock |
| 0x0c8 / 0x0d0 | PlayerBasesSystemServer / ServerProgressSystem | 0x190 | other-write **SlotSelection** |
| 0x0d8 / 0x0e0 / 0x0e8 | **ItemRegistry** / RecipeRegistry / AttributeRegistry | 0x198 / 0x1a0 / 0x1a8 | events-out AreaDepositEvent / InventoryFull / **PickupEvent** (all = Q) |
| 0x0f0 / 0x0f8 | sys-write KnowledgeTriggerQueue / ServerFactoryInfo | | |

---

## 2. Functions, exact signatures and data layouts

All are MS x64 calling convention. Struct pointers: `Result` = `{u8 error; u8 pad[3]; u32 zero; u32 value}` (12 bytes; `value` = count that did not fit or is still missing). Error codes: 0x11 bad slot id, 0x12 null slot, 0x15 unknown item / no initialized InventorySetup / slot index ≥ 8, 0x1f no target inventory, 0x27 op buffer full.

| name / signature | address | signature bytes | notes |
|---|---|---|---|
| `InventoryTx* InventoryTx_ctor(InventoryTx*)` | `0x14021dd00` | `33 D2 33 C0 48 89 11 48 89 51 08 48 89 51 10 48 89 51 18 48 89 51 20 48 89 51 28 48 89 51 30 48 89 51 38 48 89 51 40 48 89 51 48 48 89 51 50 48 89 51 58 48 89 51 60 48 89 51 68 48 89 51 70 48 89 51 78 48` (short form: `48 8D 81 40 69 00 00 48 89 81 18 69 00 00 48 8D 05 ?? ?? ?? ?? 48 89 81 30 69 00 00` matches at func+0xB8) | Zeroes the tx and sets up the three inline arrays. Upgrade-storage calls it before init; other callers inline the same code. High |
| `void InventoryTx_init(InventoryTx*, const Setup*)` | `0x140411df0` | `C6 81 A8 71 00 00 00 48 8B 42 08 48` | Copy only (section 1). Also clears tx+0x71A8. High |
| `Result* addItems(Result* out, InventoryTx*, u32 ownerEntity, u64 targetSlotId, u32 itemId, u32 count, OptU32* level, OptU8* rarity, OptF32* fuelPct)` | `0x14040b170` | see inventory_crafting.md | Details below. High |
| `Result* putStack(Result*, tx, u64 slotId, ItemInfo*, u32 count, OptU32*, OptU8*, OptF32*)` | `0x14040b760` | `48 89 5C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 30 4D` | Writes `stack.id = itemId; stack.count += n` directly in the Inventory component (via `getItemStack 0x14040f340` → `0x140225b70(pc, invAcc, entity)`). Records the touched inventory (`0x14040fa80`) and queues **op 0**. HasPIDE items: at most 1 per slot. High |
| `void InventoryTx_commit(InventoryTx*)` | `0x140408b50` | `40 55 53 41 55 48 8D AC 24 30 FC FF` | Rolls back if `flags & 0x40`, or if `tx[0x14]==0` and inventories were touched. Applies the ops, updates `InventoryEmptyState.isEmpty` of every touched inventory, calls post-commit `0x140411370`, then resets the tx. High |
| `void InventoryTx_rollback(InventoryTx*)` | `0x140410e60` | `4C 8B DC 55 56 41 55 41 56 41 57 49` | Use when `out.error != 0` or nothing fit. Medium |
| `void InventoryTx_freeArrays(InventoryTx*)` | `0x14021de50` | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 45 33 C0 48 81 C1 40 71 00 00` | Frees the arrays only if they grew past their inline storage. Call it after commit or rollback. High |
| `void postCommitEvents(InventoryTx*)` | `0x140411370` | `40 55 53 56 57 41 56 48 8D AC 24 F0 FE FF FF 48 81 EC 10 02 00 00 48 83` | KnowledgeTriggerQueue (`0x141200550` / `0x141200600`), InventoryFull, HandleEquipmentSlotChangedEvent and PickupEvent (section 1 flags). High |
| `bool createPide(ItemStack* stack, u32 rng[2], ItemInfo*, u64 slotId, InventoryTx*, OpExtra* ex)` | `0x14040ab40` | `48 89 5C 24 20 55 57 41 54 41 55 41 56 48 8D AC` | Commit op 0 for HasPIDE items. It spawns `ItemInfo+0x54c` (pidEntity template) with these inits: `UsedItem{itemId}` (0x0b02361d), `ItemState{slot,itemId,itemState=IsNew}` (0x3a2fac75), `OwnerRelationship{inventory entity}` (0xd7361788), optional `Level` (0xe550da37: random between ItemInfo+0x674/+0x678 or the requested level), `FuelPercentageInit` (0x6d3a006d), `PerkContainerNew` (0xe288d50c, from rarity) and `WeaponGemSlot` (0x84ba9b52, by balancing chance). It writes `stack.count=1, stack.pide=newEntity`. High |
| `u32 collectTargetInventories(List*, tx, InventorySetup*, ItemInfo*, SlotSelection*)` | `0x14040e890` | `4C 8B DC 49 89 5B 08 49 89 6B 10 49 89 73 18 57 41 54 41 55 41 56 41 57 48 81 EC B0` | Walks the InventorySetup links (`0x140e6e6f0`). If the item's category maps to a special inventory (ItemInfo+0xcc → `0x140e757d0`) it uses that one. Otherwise it uses all Generic (cat 4) inventories, with the two actionbar inventories (`SlotSelection+0xa0`) placed last. High |
| `bool prefersBackpack(pc, PlayerInputOtherReadAcc, u32 entity)` | `0x14040fbb0` | `48 89 5C 24 08 57 48 83 EC 30 41 8B D8 48 8B FA` | `(PlayerInput+0x5b0 >> 11) & 1`. High |
| `Result* consumeFuel(Result*, tx, u32 pide, u32 amount)` | `0x140409ee0` | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 81 EC 90 00 00 00 48 8B 3A` | Decreases `NetworkFuel.fuel` and queues op 2 (water charges for recipes, `0x1403f19f0`). High |
| `void* readOther(void** out, void* worldCtx, Accessor acc, u32 entity)` | `0x1409a5e20` | `40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 20 5B C3 CC CC CC CC CC CC CC CC CC 4C 8B D1 4D` | Reads another entity's component through an accessor. `worldCtx = *(void**)(*(u8**)pc + 0x30)` (that is `0x1409b2fe0(*pc)`). `out[0]` = component* or NULL. High |
| `u32 GuidToItemId(const u8 guid[16])` | `0x14021f6f0` | `48 89 5C 24 08 8B 01 4C 8B D1 8B D0` | Returns 0 for an all-zero GUID, otherwise FNV-1a-32 (`0x811c9dc5`, `*0x01000193`) over the 16 bytes in memory order. High |

Value types (seen in the admin handler and createPide):
```
struct OptU32 { u32 value; u8 has; u8 pad[3]; };   // level (passed by pointer, 8 bytes)
struct OptU8  { u8 value; u8 has; };               // rarity (ItemRarity)
struct OptF32 { float value; u8 has; u8 pad[3]; }; // FuelPercentageInit.initialFuelPercentage
struct ItemStack { u32 itemId; u32 count; u32 pide; };  // ItemData = {count, pide}; 0 = empty
// InventorySlotId as u64: low u32 = 8-slot inventory entity id, byte 4 = slot index 0..7 (decode 0x140e83df0, encode 0x140e65d70)
```

### addItems in detail (`0x14040b170`, decompiled)
1. `ItemInfo* info = ItemRegistry_find(itemId, tx[1])`. If NULL → error 0x15.
2. Reads `InventorySetup` of `ownerEntity` via `tx[3]`. It must exist with `isInitialized (+0x53) != 0`, otherwise error 0x15.
3. `SlotSelection* sel = 0x1404080e0(pc, tx[0x1d], owner)`, then `collectTargetInventories`. Empty result → error 0x1f.
4. If `targetSlotId == 0`: preferred slot `0x14040f460(tx, list, owner, info, 10)`. Then it puts into that slot (if any) or into the explicit target slot.
5. Unless HasPIDE or flag 0x80: it tops up existing stacks of the same id below `ItemInfo+0x14` (maxStackSize).
6. Unless flag 0x20: default slot finder `0x14040f460(..., 0)`. Then every empty slot of every listed inventory (`0x140e891f0` = slot empty).
7. If items remain: `tx+0x71A8 |= 0x10`, and `out.value = remaining`. `out.error` is still 0.

Count can be larger than one stack, because it spreads over several slots. The admin handler commits when `error==0 && remaining < count`. Use `remaining == 0` if you want all-or-nothing.

### Commit op records (0x34 bytes, array `tx[0x1e]`, count `tx[0x1f]`, inline capacity 512)
`+0x00 u64 slotId, +0x08 itemId, +0x0c OptU32 level, +0x14 OptU8 rarity, +0x18 OptF32 fuel, +0x20 pide EntityId, +0x24 u16 amount, +0x26 u8 type, +0x27 u8 equipmentSlot, +0x28 u64 second slotId (op 7), +0x30 u8 "destroy pide" flag`.

| type | producer | commit action |
|---|---|---|
| **0** put | putStack `0x14040b760` (**the only op addItems queues**) | HasPIDE: createPide. Post-commit: knowledge trigger + PickupEvent |
| 1 take | `0x14040a360` (removeItems family) | if +0x30 set: despawn the pide (`0x140954250`); IsContainer items also clear InventorySetup links via `tx[0x13]` |
| 2 / 3 | consumeFuel `0x140409ee0` / `0x140412220` | `Fuel` attribute −/+ amount (`0x140407ea0`, AttributeRegistry attr `0x05b0bf75`) |
| 4 | takeItems `0x1404122d0` (1273043) "take without destroy" (not "unequip": the unequip effect comes from clearing the SlotSelection reference; see `re_take_items.md`) | recompute equipment slot references |
| 5 move | putItems `0x14040fe80` | `ItemState.containedInSlotId` + `OwnerRelationship` of the pide, equipment remap. PickupEvent like op 0 |
| 6 destroy | destroyItemStack `0x14040b920` | same as 1 |
| 7 equip | equipItem | `SlotSelection_setEquipmentSlot`, `ItemState` IsEquipped bit, HandleEquipmentSlotChangedEvent |

Confidence: High for 0, 2/3, 5, 7. Medium for 1/4/6 (producers not fully read).

### InventoryTx layout (0x71B0 bytes)
`[0x00..0x1D]` = the copied setup (section 1); `+0xB0` flags, `+0xB1` stock mode, `+0xE0` operator, `+0xE4` source. `[0x1e]` ops ptr (inline storage at +0x118, cap 0x200, grow fn `[0x21]`). `[0xd23]` slot-remap array (16-B elements, inline at +0x6940, cap 0x80). `[0xe28]` touched-inventory entity ids (u32, inline at +0x7168, cap 0x10). `+0x71A8` u8 result flags (0x10 = did not fit). Allocate it on the heap (29 KB) and align it to 16.

---

## 3. Recipe: give N of item X to a player from a hook on `inventory_actions`

`inventory_actions` (update `0x1403fd610`, sig `40 55 41 55 48 8D AC 24 28 14 FF FF`) runs only in the **server world** (host or single player), once per batch of players. Clients that joined someone else's server cannot do this: the server owns the inventory.

**Important iterator detail (decompiled `iter_next 0x1409b5a50`):** iteration state is `*(u32*)(pc+8)`. `iter_next` returns false once it equals `Q+0x830`, and nothing resets it. So the original update leaves it at "end". Run your own loop **before** calling the original, and restore the index afterwards.

```cpp
// types from section 2; Setup = u8[0xF0]; InventoryTx = u8[0x71B0]
void hk_inventory_actions(void* pc) {
    if (!g_pending.empty() && !(*(u8*)(*(u8**)pc + 0xA1) == 1)) {   // same early-out as the original (0x1409c40b0)
        u32 savedIdx = *(u32*)((u8*)pc + 8);
        alignas(16) u64 it[0x1B0 / 8];
        iter_begin(pc, it, 0x1B0);                                 // 0x1409bab60
        while (iter_next(pc, it, 0x1B0)) {                         // 0x1409b5a50
            u32 player; getCurrentEntity(pc, &player);             // 0x1409b2e40
            auto req = g_pending.take(player); if (!req) continue; // {itemId, count}
            alignas(16) u8 s[0xF0] = {};
            s[0] = 0x01 | 0x04;                                    // PickupEvent popup + InventoryFull
            *(void**)(s+0x08) = pc;
            *(u64*)(s+0x10) = it[0xD8/8];   // ItemRegistry
            *(u64*)(s+0x18) = it[0xA8/8];   // BalancingRegistry
            *(u64*)(s+0x20) = it[0xE8/8];   // AttributeRegistry
            *(u64*)(s+0x28) = it[0x160/8];  // InventorySetup (read)
            *(u64*)(s+0x30) = it[0x148/8];  // Inventory
            *(u64*)(s+0x38) = it[0x190/8];  // SlotSelection
            *(u64*)(s+0x40) = it[0x168/8];  // ItemState
            *(u64*)(s+0x48) = it[0x178/8];  // OwnerRelationship
            *(u64*)(s+0x50) = it[0x160/8];  // InventorySetup (write)
            *(u64*)(s+0x58) = it[0x158/8];  // InventoryEmptyState (required)
            *(u64*)(s+0x70) = it[0x140/8];  // Fuel
            *(u64*)(s+0xA0) = it[0x1A8/8];  // PickupEvent writer
            *(u64*)(s+0xA8) = it[0x1A0/8];  // InventoryFull writer
            *(u64*)(s+0xC0) = it[0xF0/8];   // KnowledgeTriggerQueue
            *(u32*)(s+0xC8) = player;       // operator / receiver
            u8* tx = g_txBuf;               // 0x71B0 bytes, heap, 16-aligned, one per thread
            InventoryTx_ctor(tx);           // 0x14021dd00
            InventoryTx_init(tx, s);        // 0x140411df0
            Result r{}; OptU32 lvl{}; OptU8 rar{}; OptF32 fuel{};
            addItems(&r, tx, player, 0, req->itemId, req->count, &lvl, &rar, &fuel);  // 0x14040b170
            if (r.error == 0 && r.value < req->count) InventoryTx_commit(tx);        // 0x140408b50
            else                                      InventoryTx_rollback(tx);      // 0x140410e60
            InventoryTx_freeArrays(tx);                                              // 0x14021de50
            // r.value = items that did not fit -> re-queue or spawn a pickup (section 4)
        }
        *(u32*)((u8*)pc + 8) = savedIdx;   // normally 0, so the original iterates normally
    }
    orig_inventory_actions(pc);
}
```
Notes:
- **Which entity:** the batch entities are the players: they have PlayerInput, ServerConsumedPlayerInput, InventorySetup, CharacterSetup and SlotSelection. Player entity ids are 1..1023 (the root-owner check is `id-1 < 0x3ff`; `BasicPlayerInfos` at iter+0xb0 is indexed by `id-1`, 0xA0 B per entry). You can skip players whose own `InventorySetup` (iter+0x08 → +0x53) is not initialized.
- **Owner can be any entity with an InventorySetup** (for example a chest), because the accessors are other-entity accessors. `s+0xC8` only decides who gets the PickupEvent and the knowledge triggers.
- **Optional values:** pass `lvl = {L, 1}` / `rar = {R, 1}` to force the level / rarity of new equipment pides. `fuel = {pct, 1}` sets the initial fuel. All zero means default rolls.
- Setting `s[0] |= 0x02` hides the popup.
- The same recipe works from any other server program whose iterator holds the same accessors. Map its iterator with the rule in `ai.md` section 1 and the catalog lists. For example, `player_crafting` (`0x1402e0e50`, qword indices in the section-1 table) or `item_state`.

Confidence: High for the field mapping and the call order (it mirrors the admin handler `0x1403f5940` exactly). Medium for the restore-index trick (derived from `iter_begin` / `iter_next` code, not run).

### Item id from a GUID
`ItemId = GuidToItemId(guid)` = FNV-1a-32 over the 16 raw bytes (`0x14021f6f0`). Then validate with `ItemRegistry_find(id, reg)` (`0x140e7a870`). The result is non-NULL for real items, and `ItemInfo+0x14` (u16) is maxStackSize.
Evidence: program `pickup_item_setup` (`0x1404020c0`) sets `PickupItem.itemId = 0x14021f6f0(&PickupItem.item)` when the id is 0. The destructible drop code does the same. Hash the bytes exactly as they sit in an `ItemReference` / `ItemInfo+4 objectId`. How Keen's text GUIDs map to those bytes is not checked.

---

## 4. Simpler alternative: spawn a loose pickup next to the player (no transaction)

The destructible / mining drop code spawns a world pickup entity. The vanilla loot programs (`loot_pickup_in_range` `0x140400b50`, `loot_pickup_item` `0x140400d00`) then add it to the inventory through the normal transaction, with the popup.

**`void spawnPickupItem(SpawnCtx* ctx, const ItemDrop* drop, u32 level)`** = `0x1403b5230`, sig `40 55 56 41 54 41 56 41 57 48 8D AC 24 60 FF FF` (decompiled + assembly).
```
struct SpawnCtx {            // 0x58 bytes, built like this by 0x140326980 (destructible)
  void* spawnQueue;          // +0x00 = *(void**)(*(u8**)pc + 8)   (0x1409b0860(pc))
  u32*  rng;                 // +0x08 -> u32[2] state (non-zero; game seeds with 0x140878290 + 0x1409bb060)
  void* itemRegistry;        // +0x10
  u8    transform[56];       // +0x18 CurrentTransform (WorldPosition s64[3] fixed 32.32, quat, scale)
  u32   spawnerEntity;       // +0x50 (current entity; use the player id)
};
struct ItemDrop { u32 itemId; u8 count; };   // only the BYTE at +4 is read (max 255 per call)
```
It spawns template GUID bytes at `0x14150ccb0` (`77 27 6d a5 f1 da 64 41 90 d4 b7 b1 27 49 c7 23`, the generic pickup) through
**`EntityId* spawnEntity(void* queue, EntityId* out, u32 spawner, u32 zero, const Guid* tpl, const Slice* inits, u8 flags, u8 zero)`** = `0x140953770` (sig `40 53 48 83 EC 60 33 C0 44 89 4C 24`). It is deferred and returns the pre-allocated id in `out[0]`.
`Slice = {ComponentInit* data; u64 count}`, `ComponentInit = {u32 typeHash; u32 pad; void* data; u64 size}` (0x18 B). The four inits are:
- `CurrentTransform` 0x027ac564 (56 B, copy of ctx+0x18)
- `PickupItem` 0x08775a90 (28 B: `itemId, ItemReference(0), level, rarity=0, pickupAmount u16`)
- `Velocity` 0xfb332100 (random toss, y up 5..7)
- `ModelDynamic` 0x29a43465 (`newModel = ItemInfo+0x57c`)

For amounts above 255 or a rarity, call `spawnEntity` yourself with the same four inits. `PickupItem.pickupAmount` is u16.

From the `inventory_actions` hook you get the player's transform with `readOther(&p, *(void**)(*(u8**)pc+0x30), it[0x110/8] /*CurrentTransform other-read*/, player)`. Add an upward offset if you like.
Pros: no 29 KB tx, no accessor list, works from any server program that has a ProgramContext (spawning goes through the world command queue). Overflow is never lost.
Cons: the player must pick it up. Auto-collect depends on the `AutoCollectLooseLoot` player setting (bit 12) and range. It also creates network entities.
Confidence: High for the spawn code and layout. Medium that a mod-spawned pickup behaves exactly like a drop (template and pickup programs were not traced end to end).

The admin path (write `AdminInventoryCreateAction` into `PlayerInput+0x3c0`) is still an option. Its limits are listed in `inventory_crafting.md` 5a: isAdmin-gated, and count 1 or a full stack only.

---

## Open questions
- Setup +0x78..+0x98, +0xb8 and +0xd0 (tx[8..0xc], [0x1a], [0xd]) are still unidentified. They are not read by add, commit, post-commit or createPide. Candidates from access lists: Durability, Level, PerkContainerNew, ColorPaletteItem, CustomPlayerString.
- The internals of rollback (`0x140410e60`), and how it undoes stack writes made directly by putStack.
- Whether `loot_pickup_*` honours `PickupItem.pickupAmount` > 255 and an arbitrary spawner id.
- The byte order of a textual Keen GUID compared with the in-memory ItemReference used by the hash.
