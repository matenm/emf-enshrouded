# Taking items out of an inventory, player level, item level/rarity/slot

Build **1273043** (image base 0x140000000), with the 1269579 RVA where known. Static analysis (Ghidra via rea, plus
the `tools/re` scripts with caches rebuilt for 1273043); one result is from the game (**[runtime]**). Every signature below was checked
with `sig.verify` on the installed 1273043 exe: each matches exactly once in `.text`, at the function entry.
It extends `docs/engine_parts/re_inventory_api.md` (setup block, tx layout, op records). Read that first.

Evidence tags: **[dec]** = decompiled code in 1273043, **[types]** = types.json, **[rt-doc]** = a runtime
observation in other repository docs, **[guess]** = inference that was not verified.

## Short answers
- **Remove N of an item:** `removeItems(out, tx, owner, itemId, preferredSlot, count)` = `0x14040a400`. It searches the
  owner's whole inventory tree: every InventorySetup link except category 5 (Virtual); it recurses into Virtual links.
  That covers the backpack, both action bars and the Equipment inventories (where equipped items sit). It queues
  **op 1** per slot. A fully emptied stack gets `op.pide = stack.pide` and the destroy flag (+0x30 = 1). At commit the
  pide entity is **despawned** and any `SlotSelection.equipment` reference to that slot is cleared (the item is
  unequipped). [dec]
- **Remove by explicit slot:** pass the slot as `preferredSlot`. It is tried first, and other slots are only searched
  while some count is still missing. With `preferredSlot = 0`, the **currently selected action-bar slot** is tried
  first. [dec]
- **Take and later give back the same gear (pide intact):** do NOT use removeItems or destroyItemStack, because they
  despawn the pide. Use **`moveStack(out, tx, dst, src, mode)` = `0x14040da50`**. It queues op 4 (take, which keeps the
  pide) and op 5 (put, which rewrites `ItemState.containedInSlotId` and `OwnerRelationship` of the pide). Move the item
  into a slot of some holding container and move it back later. The low-level pair `takeItems 0x1404122d0` (op 4)
  plus `putItems 0x140410200` (op 5) also keeps the pide, but between the two transactions the pide is orphaned (see
  section 4b). [dec]
- **Player level:** the `Level` attribute component on the player entity, attribute id **0x724bf568**. In practice this
  is **`dataStorage[0]`** (`Level+8`). The game itself reads `Level+8` for the save migration, and reads
  `*(Level + storageOffset)` for items. Clients get it replicated as `NetworkPlayerAttributes.level` (u16 at +0xB8).
  [dec]
- **Item level / rarity:** `ItemState+0x11 itemLevelUi` = `(u8) Level.dataStorage[0]` of the pide.
  `ItemState+0x10 itemRarityUi` = `PerkContainerNew.availablePerks - 1` when that is 0..4, otherwise
  `ItemInfo+0x17 rarity`. The game program `item_state_init` keeps both up to date. [dec]
- **Slot type / category:** `ItemInfo+0x104` = `EquipmentSetup.slot` (EquipmentSlotType, u8) and `ItemInfo+0xCC` =
  `ItemCategory`. The game maps slot type to EquipmentSlot with `0x140e78110`. [dec] + [types]

---

## 1. Function table (MS x64; out = result struct, tx = InventoryTx*)

`Result12 = {u8 error; u8 pad[3]; u32 zero; u32 value}`, the same struct addItems uses.
`Taken24 = {u8 error; pad[3]; u32 zero; u32 itemId @+8; u32 pide @+0xC; u32 count @+0x10; u8 slotEmptied @+0x14; pad}`.
`InventorySlotId` = u64 by value: low u32 = the 8-slot inventory entity, byte 4 = slot index 0..7.

| name / signature | 1273043 | 1269579 | signature bytes (unique in 1273043) | conf. |
|---|---|---|---|---|
| `Result12* removeItems(Result12* out /*rcx*/, tx /*rdx*/, u32 owner /*r8d*/, u32 itemId /*r9d*/, u64 preferredSlot /*[rsp+28]*/, u32 count /*[rsp+30]*/)` | `0x14040a400` | `0x14040a080` | `48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 48 89 54 24 10 55 41 54 41 55 41 56 41 57 48 8D AC 24 20 FF FF` | **High** [dec] full body + inner + commit + vanilla caller (Tint handler `0x1403f8f80`) |
| `Result12* removeItemsAuto(out, tx, u32 owner, u32 itemId, u32 count /*[rsp+28]*/)` = removeItems with preferredSlot = `*(u64*)0x141510098` = 0 | `0x14040a3d0` | `0x14040a050` | `40 53 48 83 EC 30 8B 44 24 60 48 8B D9 89 44 24` | High [dec + asm] |
| `Result12* takeFromSlotById(out, tx, InvRef* inv, u32 invEntity, u8 slotIdx, u32 itemId, u32 count)`: removeItems' inner step; queues op 1 | `0x14040a6e0` | `0x14040a360` | `48 89 5C 24 08 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 40 0F B6 BC 24 A0 00 00` | High [dec] |
| `InvRefResult* getInventoryRef(out56, tx, u32 invEntity)`: `out[0]` error (0x16 entity 0, 0x1F no Inventory / no accessor); `out+8` = 6-qword write-tracked Inventory ref (`*(Inventory**)(out+8)`) | `0x14040f4a0` | `0x14040f120` (shift) | `48 89 5C 24 20 41 56 48 83 EC 60 41 8B D8 4C 8B` | High [dec] |
| `Taken24* takeItems(out, tx, u64 slot /*r8*/, u32 count /*r9d*/, u8 clearItemId /*[rsp+28]*/)`: queues **op 4**, keeps the pide | `0x1404122d0` | `0x140411f50` | `40 53 55 57 41 54 41 56 48 83 EC 60` | High [dec] |
| `Taken24* takeFromSlot(out, tx, u64 slot, int mode /*0 all,1 half,2 one*/, u8 clearItemId)` → takeItems | `0x140412490` | `0x140412110` | `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 50 48 8B F9 41` | High [dec] |
| `Result12* putItems(out, tx, u64 dstSlot /*r8*/, ItemInfo* info /*r9*/, u32 pide /*[rsp+28]*/, u32 count /*[rsp+30]*/)`: queues **op 5**; value = count not placed | `0x140410200` | `0x14040fe80` | `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 30 4D 8B F1 49` | High [dec] |
| `Result12* moveStack(out, tx, u64 dst /*r8*/, u64 src /*r9*/, int mode /*[rsp+28]*/)`: take + put + equipment remap; value = moved count | `0x14040da50` | `0x14040d6d0` | `48 89 5C 24 18 55 56 57 41 56 41 57 48 8B EC 48` | High [dec] |
| `Result8* destroyItemStack(out, tx, u64 slot)`: queues **op 6** (destroy flag set) and clears the stack; pide despawned at commit | `0x14040bca0` | `0x14040b920` | `48 89 5C 24 18 48 89 6C 24 20 57 48 83 EC 40 48 8B F9 49 8B` | High [dec] |
| `Res16* getItemStack(out, tx, u64 slot)`: `out+8 = ItemStack*`; 0x11 bad slot, 0x15 index ≥ 8, 0x1F no inventory | `0x14040f6c0` | `0x14040f340` | `48 89 5C 24 08 48 89 74 24 10 57 41` | High [dec] |
| `Result8* equipItem(out, tx, u64 slot, u8 EquipmentSlot /*>0x17 = auto*/)`: works on the **current iterated entity**, see 4c | `0x14040e370` | `0x14040dff0` | `44 88 4C 24 20 48 89 54 24 10 48 89 4C 24 08 55 56 41 54 41` | Medium [dec] (signature and preconditions read; the inner op-7 builder `0x14040bdf0` was not read) |
| `unequipItem` (not re-read) | `0x140412770` | `0x1404123f0` | `4C 89 44 24 18 53 55 56 41 55 41 56` | Medium |
| `bool touchInventory(tx, u64 slot)`: adds the slot's inventory and its owner chain to tx[0xE28] (InventoryEmptyState bookkeeping); false only on allocation failure (→ 0x27) | `0x14040fe00` | `0x14040fa80` | `48 89 5C 24 08 56 57 41 56 48 83 EC 40 4C 8B F1` | High [dec] |
| `InventoryTx_commit` / `rollback` (re-read for ops 1/4/5/6) | `0x140408ed0` / `0x1404111e0` | `0x140408b50` / `0x140410e60` | as in EMF `signatures.cpp` | High [dec] |
| `ItemInfo* ItemRegistry_find(u32 itemId, ItemRegistry*)` | `0x140e7d430` | `0x140e7a870` | `89 4C 24 08 48 83 EC 28 85 C9 74 1B` | High |
| `AttrRes* attrFind(out16, u32 attrId, AttributeRegistry*)`: `out[0]` 0 / 0x1F, `*(AttrInfo**)(out+8)`, AttrInfo `+0 rootId`, `+0xC u16 slot` | `0x14121e150` | `0x14121bea0` | `89 54 24 10 53 48 83 EC 20 48 8B D9 48 8D 54 24 38 49 8D 48` | High [dec] (same as combat.md 4.2) |
| `Res12* itemLevelOf(out, ItemInfo*, Level* pideLevel)`: value = `*(u32*)(Level + Level.storageOffset)`; 0x16 if not initialized; if no Level: default from `0x140e78400` | `0x140413d70` | `0x1404139f0`? (shift, n/c) | `40 53 48 83 EC 30 48 8B D9 33 C9 48 89 4B 04 C6` | High [dec] |
| `u8 itemRarityOf(ItemInfo*, PerkContainerNew* pc)`: `pc && pc->availablePerks-1 < 5 ? availablePerks-1 : ItemInfo+0x17` | `0x140413e00` | `0x140413a80`? (n/c) | `44 0F B6 41 17 48 85 D2 74 15 0F B6 0A 41 8B D0` | High [dec] |
| `u8 equipSlotForType(u8 EquipmentSlotType)` → EquipmentSlot (0x18 none, 0x19 MainHand) | `0x140e78110` | n/c | `80 F9 26 77 4C 48 8D 15 ?? ?? ?? ??` | High [dec] |
| `clearSlotReference(out, SlotSelection*, u64 slot)`: zeroes every `equipment[]` entry equal to slot (logs "[clearSlotReference] Duplicate slot reference found in equipment.") | `0x140e637f0` | n/c | – | High [dec] |
| program `player_level` (update) | `0x1402e2990` | `0x1402e27f0` | `48 89 4C 24 08 55 53 48 8D AC 24 E8 FE FF FF 48 81 EC 18 02 00 00 41 B8 C0 00 00 00` | High [dec] |
| program `item_state_init` (update, flags 0x0 = every tick) | `0x140400c20` | `0x1404008a0` | `40 55 56 41 56 48 8D 6C 24 B9 48 81 EC B0 00 00` | High [dec] |

"n/c" = not checked. The 1269579 RVAs in the 0x408000..0x413000 region are 1273043 minus 0x380. This shift is
confirmed by EMF's table (addItems, commit, rollback, init) and by the doc addresses of removeItems (0x40a080),
takeItems (0x411f50), putItems (0x40fe80), moveStack (0x40d6d0), destroyItemStack (0x40b920) and the op-1 producer
(0x40a360). The values marked "shift" or "?" were derived, not verified.

---

## 2. What each primitive does (all [dec])

### removeItems `0x14040a400`
1. Reads `InventorySetup` of `owner` through `tx[3]`. If it is missing → `error 0x16` (nothing queued).
2. **Preferred slot.** If `preferredSlot == 0`: reads the owner's `SlotSelection` (tx[0x1D]) and takes the selected
   action-bar slot (`0x140e86420`): `sel = SlotSelection+0xC0`. If `sel ≤ 15`: entity = `actionbarEntityIds[sel>=8]`
   (+0xA0 / +0xA4), index = `sel & 7`. Otherwise there is no preferred slot.
   If a preferred slot exists: `getInventoryRef(entity)`, then `takeFromSlotById(entity, index, itemId, count)`.
   An error from either is returned as the error. If nothing is missing afterwards, it returns.
   **No bounds check on the index:** a `preferredSlot` with index ≥ 8 reads past the 96-byte Inventory. Pass only
   0..7.
3. **Full search.** It collects the inventory list with `0x140e712b0(list, InventorySetup*, acc, pc, exclude=0)`.
   That function walks `linksEntities[0..15]` until it meets entity 0. Category 5 (Virtual) links are expanded
   recursively, and all other categories (1 Customization, 2 Equipment, 3 Currency, 4 Generic) are added. For each
   inventory with a readable Inventory, it calls `takeFromSlotById` for slots 0..7 until the missing count is 0. Slots
   holding another item id are skipped.
4. `out.value` = count still missing, with `out.error = 0`. **It is not all-or-nothing:** the takes it found stay
   queued. The vanilla pattern (Tint handler `0x1403f8f80`) is `if (out.error == 0 && out.value == 0) commit;
   else rollback;`.
5. It does not look the item up in the ItemRegistry. An unknown id simply finds nothing (`value = count`).

### takeFromSlotById `0x14040a6e0` (the op-1 producer)
- If `stack.id != itemId` → `value = count`, no op is queued.
- `touchInventory`, then `n = min(count, available)`. `available` is 1 for a stack with a pide, otherwise
  `stack.count` (helper `0x140e7c3f0`).
- It queues op `{slot, itemId, amount=n (u16), type=1}`.
- If the whole stack is taken: `op.pide = stack.pide` (when non-zero), the stack is zeroed (write-tracked), and
  `op+0x30 = 1` (destroy). Otherwise `stack.count -= n`.
- Errors: `0x27` if touch or op allocation fails.

### takeItems `0x1404122d0` (op 4, "lift")
- Uses `getItemStack`. An empty slot → success with nothing taken. Unknown item → `0x15`.
- It then calls touch, clamps the count, and queues op `{slot, itemId, amount, type=4}`.
  - HasPIDE item (`ItemInfo+0x5D8 & 2`): unless it is a dry run (tx flag 0x40), the pide entity must exist
    (`0x14099d7f0`), otherwise the op is popped and the call returns `0x15`. `op.pide = pide`; the result has
    `{itemId, pide, count=1}`.
  - Otherwise `op.pide = 0`; the result has `{itemId, 0, n}`.
- Whole stack: with `clearItemId = 1` the stack becomes `{0,0,0}`. With 0 only count and pide are zeroed (the id stays).
  `slotEmptied = 1`. Partial: `stack.count -= n`.
- **There is no destroy flag. The pide survives the commit.**

### putItems `0x140410200` (op 5)
- `dstSlot == 0` or `info == NULL` → `value = count`, no error.
- IsContainer items (`ItemInfo+0x5D8 & 4`) are refused (value = count) when `dst` lies inside the container's own
  inventory tree (`0x14040fff0`).
- **Empty destination:** it queues op `{dst, itemId, type=5}` and writes the stack `{itemId, count, pide}`. HasPIDE
  items get `count = 1`, `op.pide = pide`, `op.amount = 1`. Otherwise `op.amount = count`. **No max-stack check here.**
- **Same item, no pide, room left** (`0x14040fd40`: `maxStackSize - count`): it merges `min(count, room)` and
  returns the rest in value.
- **Occupied by another item** (or a pide item onto any occupied slot) → `value = count`, `error 0`. Always check
  value.
- Errors: `0x12` bad or unreadable slot, `0x27` op buffer.

### moveStack `0x14040da50`
- `dst == src` → ok. Both stacks must be readable (`0x14040eab0`), otherwise `0x1F`.
- It pushes a **remap entry `{old = src, new = dst}`** into `tx+0x6918` (tx[0xD23]). Failure → `0x27`.
- **Same item and same pide (merge):** `n = min(src count, room in dst)`, halved or one for mode 1 / 2. Then
  `takeItems(src, n, 1)` and `putItems(dst, info(src item), pide, n)`.
- **Otherwise:** `takeFromSlot(src, mode, 1)`. If dst is not empty, also `takeFromSlot(dst, mode, 1)` and put that into
  src (a swap). Then put the src items into dst.
- `out.value` = moved count. Any failure inside → `0x15` (the caller then rolls back).
- It does **not** check whether the destination inventory category accepts the item (the UI handlers do that). It works
  between any two entities whose Inventory the tx accessors can read (other-entity accessors).

### destroyItemStack `0x14040bca0`
Queues op 6 with destroy flag, `amount = 0` and `pide` (HasPIDE only), and sets stack count and pide to 0. An empty
stack with junk is logged "Broken stack encountered on destroyItemStack" and zeroed. Unknown item → the stack is just
zeroed.

---

## 3. Commit / rollback per op (corrects the op table in re_inventory_api.md) [dec]

Before the switch, commit resolves the root owner of `op.slot` through OwnerRelationship (`0x140e837d0`) and reads that
root's `SlotSelection` (tx[0x1D]). Call it `sel` (NULL for chests).

| op | producer(s) | commit | rollback (reverse order) |
|---|---|---|---|
| 1 take | takeFromSlotById `0x14040a6e0` (removeItems) | only if destroy flag: `clearSlotReference(sel, slot)` (unequips); if `op.pide`: IsContainer items also clear matching `InventorySetup.linksEntities` in the slot's (or its owner's) setup via tx[0x13], then **destroy the pide** (`0x1409554c0(*(pc_job+8), currentEntity, pide)`, deferred) | restores the stack: pide → `{id,1,pide}`; else empty → `{id,amount,0}`, non-empty → `count += amount` |
| 4 lift | takeItems `0x1404122d0` (moveStack, takeFromSlot, equip, …) | if `sel`: if the item's equipment slot (`equipSlotForType(ItemInfo+0x104)`) in `sel` holds `op.slot`, remember `{EquipmentSlot, slot}` for re-equipping; then `clearSlotReference(sel, slot)`. **The pide is not touched** | same as op 1 |
| 5 put | putItems `0x140410200` | if `op.pide`: `ItemState.containedInSlotId = slot` (tx[5]) and `OwnerRelationship.owner = slot entity` (tx[0x12]). Then, if `sel`: for remap entries matching a remembered op-4 slot, `setEquipmentSlot(type, sel, newSlot)`, so an equipped item moved within the same root stays equipped. Post-commit PickupEvent like op 0 (doc, flag 0x01) | count −= amount, or clears the stack (pide items / full amount) |
| 6 destroy | destroyItemStack | same as 1 | same as 1 |
| 8 (new) | InventorySetup link change (not traced) | – | toggles `linksEntities[amount]` / `linksCategories` between 0 and `{op.pide, 5}` |

The `re_inventory_api.md` table should say: **op 4 = take without destroy (moves and lifts), not "unequip"**. Its
"unequip" effect is the `clearSlotReference`.

Errors seen: 0x11 bad slot id, 0x12 null/unreadable slot, 0x15 unknown item / index ≥ 8 / move failure,
0x16 owner has no InventorySetup / entity 0, 0x1F no inventory / accessor, 0x27 op or remap buffer / touch failure.

---

## 4. How to call it (server world, inside EMF's `emf_inventory` program; the setup block exactly as `fill_setup` builds it)

All three recipes use the existing ctor → init → op → commit/rollback → freeArrays sequence. The accessors the remove
and move paths read are tx[3] InventorySetup, tx[4] Inventory, tx[5] ItemState, tx[0x12] OwnerRelationship, tx[0x13]
InventorySetup (write), tx[0x14] InventoryEmptyState (required, or commit rolls back) and tx[0x1D] SlotSelection.
EMF already fills all of them. Setup flags: use **0** for removal. For a move back into the player, 0 means no popup;
0x01 shows the "+N" PickupEvent popup, and adding 0x02 hides it.

### 4a. Remove N of item X (consumables, materials)
```cpp
Result12 r{};
removeItems(&r, tx, player, itemId, /*preferredSlot*/ slotOrZero, count);   // 0x14040a400
if (r.error == 0 && r.value == 0) commit(tx); else rollback(tx);           // all-or-nothing
freeArrays(tx);
```
- `slotOrZero = 0` prefers the held action-bar slot. Pass an explicit `InventorySlotId` (index 0..7) to take from that
  slot first.
- Equipped or gear items: the pide is despawned and the item is unequipped. Fine for "consume", wrong for "borrow".
- Remove one specific stack completely, whatever its id: `destroyItemStack(&r, tx, slot)`. This also despawns the pide.

### 4b. Take a gear item away and later give the same item back
**Recommended: move it into a holding container slot, then move it back (op 4 + op 5, pide preserved).**
```cpp
// take: player slot -> free slot of a holding container (any entity with InventorySetup + Inventory)
Result12 r{}; moveStack(&r, tx, holdSlot, playerSlot, /*mode all*/ 0);    // 0x14040da50
(r.error == 0 && r.value > 0) ? commit(tx) : rollback(tx);
// give back later (another tx): container slot -> a free player slot (find one by reading the Inventory components)
moveStack(&r, tx, freePlayerSlot, holdSlot, 0);
```
- The pide entity keeps its Level, perks, durability and gem slot. Commit rewrites `ItemState.containedInSlotId` and
  `OwnerRelationship`. [dec]
- If the item was equipped, op 4 unequips it. It is re-equipped only when src and dst share the same root (the same
  transaction's remap). Moving it back later leaves it unequipped. To re-equip, call `equipItem(&r, tx, slot,
  EquipmentSlot)`, but see 4c. [dec]
- The holding container must be loaded on the server and be readable through the program's other-entity accessors.
  Which entity to use (a hidden or mod-owned chest) is the open design question (section 7).

**Alternative without a container (Medium-Low):** `takeItems(&t, tx, playerSlot, 1, 1)` and commit. The pide entity is
**not** destroyed: there is no destroy flag, and no despawn was found on the paths read. Later:
`putItems(&r, tx, freeSlot, ItemRegistry_find(t.itemId), t.pide, 1)` and commit. Op 5 re-homes it. Risks:
- Between the two transactions the pide is orphaned: `ItemState.containedInSlotId` and `OwnerRelationship` still point
  at the old slot, which is now empty.
- `item_state` (`0x140236fb0`) destroys a *broken* pide (durability 0) whose slot does not hold it.
- What save/load does with an orphaned pide is unknown.
- The client UI will not show the item anywhere.

### 4c. Pitfall: functions that use the *current iterated entity*
- `equipItem` reads the operator from `CurrentEntityId(pc)` (`0x1409b4230`), not from the tx. Commit op 7 and the op-1
  despawn requester do the same.
- In a program that does not iterate (EMF's own program), CurrentEntityId returns 0 (pc+8 == 0): equipItem fails with
  0x1F, and the despawn is queued with requester 0. **[runtime, build 1273043]** It works: EMF's
  `take_items` (removeItems from the non-iterating `emf_inventory` program) took a level-10 Epic sword from the
  player's action bar and its pide (entity 6751) was gone 3 s later (`ItemState` no longer found). `put_items_ex`
  with the same item id, level 10 and rarity 3 gave it back with that level and rarity. A T0 helmet put with level 8
  showed level 4: the game caps an item's level (by tier, not checked further).
- removeItems, takeItems, putItems, moveStack and destroyItemStack do not use the current entity.

---

## 5. The player's level [dec unless marked]
- Component: `keen::ecs::Level` (16 B, attribute component; `server_only`, `savegame_relevant`) on the player entity.
  Header `{u32 rootId; u16 storageOffset; u8 flags (bit0 = initialized); u8 storageCount}`, then
  `u32 dataStorage[2]` at +8. [types]
- `player_level` (`0x1402e2990`, writes Experience, Level and PlayerTracking, emits LevelUpEvent `0x4183785e`):
  - It reads and writes the level through **attribute id 0x724bf568**: `attrFind`, check `AttrInfo.rootId ==
    Level.rootId`, then `value = *(u32*)((u8*)level + level->storageOffset + AttrInfo.slot*4)`.
  - On level-up: `level += 1`, `xp -= xpNeeded`, and `xpNeeded` is recomputed.
  - Experience ids: **0x8dcddaf4** current XP, **0x1d7dd102** pending XP (added to current XP, then zeroed),
    **0x4723f62** XP needed for the next level.
- **Index: `dataStorage[0]`.** Evidence:
  - The save migration `0x140ddc4d0` copies `*(u32*)(Level+8)` and `*(u32*)(Experience+8)` of the player into the
    character save record (+0x40 / +0x44).
  - Items read their level as `*(u32*)(Level + storageOffset)` (`itemLevelOf 0x140413d70`).
  - [rt-doc] On a companion, `dataStorage[0]` = level, and writing it changed the level (`re_companion_orders.md`).
  - Robust read: `attrFind(0x724bf568)` → slot; fall back to `[0]`.
- `dataStorage[1]`: unknown (105 on the enemy in rt-doc). [guess] a second attribute of the Level root, such as a
  cap.
- Client side / UI: `NetworkPlayerAttributes` (256 B, is_dynamic): `+0xB8 level u16`, `+0xA8 experience u16`,
  `+0xAA experienceRequired u16`, filled by `network_player_attributes` (`0x1402760c0`) from the three ids above.
  Other entities: `NetworkLevel.level` (u16). [dec] + [types]
- The main-menu "Level 1" comes from the character save record (+0x40), which is the same value. [dec, migration path]
- Admin path to set it: `PlayerInput` `adminSetCharacterLevel` (`AdminSetCharacterLevel {versionData, targetEntityId,
  u8 level}`), handled by `admin_player` (`0x1402cafd0`, writes Experience and Level). [types] Not traced.

## 6. Item level, rarity and slot type of a stack [dec + types]
- Stack → pide: `ItemStack {u32 id; u32 count; u32 pide}`; gear has `pide != 0`.
- `ItemState` (20 B, is_dynamic, so also present on clients) on the pide:
  - `+0x10 itemRarityUi` (ItemRarity: 0 Common, 1 Uncommon, 2 Rare, 3 Epic, 4 Legendary).
  - `+0x11 itemLevelUi` (u8).
  - `+0x0E itemState`: bit 3 IsEquipped, bit 4 HasLevel, …
- `item_state_init` (`0x140400c20`, program flags 0x0, compare-then-write each tick) sets:
  - `itemLevelUi = (u8) itemLevelOf(info, pide.Level)`, i.e. `Level.dataStorage[0]`.
  - `itemRarityUi = itemRarityOf(info, pide.PerkContainerNew)`. `PerkContainerNew` is 2 B `{u8 availablePerks; u8
    unlockedPerks}`; rarity = `availablePerks - 1` when that is 0..4, else `ItemInfo+0x17` (the default rarity).
  - `HasLevel` (0x10), `HasWeaponGemSlot` (0x100), `HasDamage` (0x20) and `baseDamageUi` (+0x0C).
- So reading `ItemState+0x10/+0x11` is correct. The authoritative values are pide `Level.dataStorage[0]` (server only)
  and `PerkContainerNew.availablePerks`. Stacks without a pide have no level (UI default) and the rarity is
  `ItemInfo+0x17`.
- Slot type (types.json offsets inside `ItemInfo`, 2088 B):
  - `+0xCC category` (ItemCategory: 0 Customization, **1 Equipment (armour)**, **2 Weapons**, 3 Tools, 4 Instrument,
    5 FishingRod, 6 BuildTools, 7 Consumables, 8 Ammunition, 9 Materials, … 15 WeaponGem, 16 ColorPalette).
  - **`+0x104 equipment.slot` (EquipmentSlotType u8):** 1 MeleeWeapon, 2 Tool, 3 BuildTool, 4 MidRangeWeapon,
    5 RangeWeapon, 6 Arrow, 7 Spell, 8 FishingBait, **13 Armour_Head, 14 Armour_UpperBody (chest), 15 Armour_Arms
    (hands), 16 Armour_LowerBody (legs), 17 Armour_Feet**, 18 Shield, 34 HookShot, 35 Glider, 36 Consumable,
    37 Armour_Ring, 38 Backpack.
  - `+0xD0 weaponCategoryReference`, `+0x674 itemLevelRange {min,max}`, `+0x17 rarity`, `+0x5D8 flags` (bit 1 HasPIDE,
    bit 18 IsVanityItem).
- The game's own mapping, `equipSlotForType 0x140e78110` (EquipmentSlotType → EquipmentSlot / SlotSelection index):
  - 1-4 → 0x19 MainHand (weapons and tools live in the action bar, not in `equipment[]`).
  - 5 → 12 RangedWeapon, 6 → 14 Arrow, 7 → 15 Spell, 8 → 16 FishingBait, 9-12 → 17-20 materials.
  - 13-17 → 5-9 (armour head … feet), 18 → 13 Shield.
  - 34 → 21 HookShot, 35 → 22 Glider, 38 → 23 Backpack.
  - 37 Armour_Ring → 0x18 (Invalid here: rings have two slots, 10/11, which are chosen elsewhere).
  - Commit uses this mapping for op 4. equipItem uses the reverse check `0x140e78ab0(slot) == ItemInfo+0x104`.

---

## 7. Open questions
- What save/load and the replication layer do with a pide orphaned by `takeItems` without a matching put (4b
  alternative). Test at runtime before relying on it.
- Which holding container to use for "borrow and return": a placed chest, a mod-spawned entity with InventorySetup +
  Inventory + InventoryEmptyState, or another inventory link of the player itself (e.g. a Virtual or Customization
  inventory). The primitives accept any entity; streaming and ownership were not checked.
- Whether category 1 / 3 (Customization, Currency) links exist on the player and hold anything removeItems could hit
  unintentionally (the code includes them).
- `Level.dataStorage[1]` meaning. The AttributeRegistry slot of 0x724bf568 is not provable statically (it is built
  from resources); `[0]` is supported by three independent readers.
- Post-commit (`0x1404116f0`) behaviour for op 4/5 was taken from `re_inventory_api.md`, not re-read in 1273043.
- The op-7 builder `0x14040bdf0` and the equip-slot finder `0x14040ef50` inside equipItem were not read.
- The 1269579 RVAs marked "?" / "n/c" were not checked against the old exe.
