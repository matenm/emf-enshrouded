# Inventory, items, equipment and crafting (Tier 2.1 / 2.2)

Build 1269579, image base 0x140000000. Static analysis only (Ghidra via rea + local scripts).
Signatures come from `sig.py` (rel32 and RIP displacements wildcarded). They start at the function entry and were checked to be unique.
Struct offsets come from `types.json` (EML) unless the text says "seen in code".

Summary:
- An item stack is `{ItemId id; u32 count; EntityId pide}` (12 bytes). The `pide` is an optional "per-item data entity". Items with the flag `ItemFlag::HasPIDE` (equipment and other unique items) get their own ECS entity. That entity holds `ItemState`, `Durability`, `Level`, perks, gem slot and so on.
- An `Inventory` component is always **8 slots**. A player or chest is a tree: its `InventorySetup` links up to 16 child entities, each with a category. Each child has its own 8-slot `Inventory`.
- All inventory changes go through one **inventory transaction** object (`InventoryTx`, about 0x71B0 bytes, on the stack). The flow is init `0x140411df0`, then operations (add / remove / move / destroy / equip), then **commit `0x140408b50`** or **rollback `0x140410e60`**. 47 functions call the commit. They are the ECS programs that write `Inventory, InventoryEmptyState, ItemState, OwnerRelationship, SlotSelection`.
- Item definitions are `ItemInfo` blobs (2088 B). The lookup is `ItemRegistry_find(itemId, registry)` = **`0x140e7a870`**. It is a hash map at `ItemRegistry+0x38`.
- Recipes are `RecipeInfo` blobs (168 B). The lookup is `RecipeRegistry_find(recipeId, registry)` = **`0x140e7fbd0`**. It is a hash map at `RecipeRegistry+0x48`.
- A craft runs as: `player_crafting` turns `PlayerInput.playerCraftingAction` into a `CraftRecipeEvent`. Then `handle_crafting_events` calls the **craft core `0x140470f10`**. The core consumes the inputs, adds the outputs, commits, and emits `CraftRecipeSuccessEvent`.

---

## 1. Item identity and the item registry

### Types (types.json, matches memory)
| type | size | layout |
|---|---|---|
| `keen::ItemId` | 4 | u32. 0 = none |
| `keen::ItemData` | 8 | `+0 count u32`, `+4 pide EntityId` |
| `keen::ItemStack` | 12 | `+0 id ItemId`, `+4 data ItemData` (count at +4, pide at +8) |
| `keen::InventorySlotId` | 8 | `+0 entityId` (the 8-slot inventory entity), `+4 slotIndex u8` (0..7). Passed by value as one u64 |
| `keen::ItemInstanceInfo` | 20 | slotId, itemId, pide, stateMask |
| `keen::ecs::ItemState` (on the pide entity) | 20 | `+0 containedInSlotId`, `+8 itemId`, `+0xc baseDamageUi u16`, `+0xe itemState` (ItemStateFlag mask: 0 Broken, 1 DurabilityInitialized, 2 ContinuousDurabilityLoss, **3 IsEquipped**, 4 HasLevel, 5 HasDamage, 6 IsNew, 7 HasContinuousDurability, 8 HasWeaponGemSlot, 9 IsTinted), `+0x10 rarity`, `+0x11 level` |
| `keen::ecs::OwnerRelationship` (server only) | 4 | `ownerEntityId`. pide → container entity → ... → player |
| `keen::ItemInfo` (definition) | 2088 | `+0 itemId`, `+4 objectId Guid`, `+0x14 maxStackSize u16`, `+0x48 name` loca tag, `+0xcc category` (ItemCategory), `+0x104 equipment EquipmentSetup (1088 B)`, `+0x54c pidEntity TemplateReference`, `+0x5d8 flags` (ItemFlag bits: 0 HasKnowledgeUnlocks, **1 HasPIDE**, **2 IsContainer**, 3 IsBlueprint, 7 IsTerraformer, 8 IsNpc, 13 UsesFuel, 14 TwitchDrop, 15 CommerceItem, 20 IsFactoryEntity ...), `+0x608 perkReferences`, `+0x67c damageSetup`, `+0x6bc armorSetup`, `+0x6e8 fuelSetup`, `+0x820 debugName BlobString` |

Seen in code: `ItemInfo+0x14` is used as the max stack size in add-item, and `ItemInfo+0x5d8 & 2` (HasPIDE) / `& 4` (IsContainer) are tested in commit and destroy. Confidence: High.

### ItemRegistry singleton (`g38_items::ItemRegistry`)
- **Name:** ItemRegistry object (0x328 bytes). Each world (server and client) has its own.
- **Created by:** `createItemRegistry(alloc, resourceSystem, guidRef)` = `0x140e64980`. It allocates 0x328 bytes and loads the `ItemRegistryResource` (resource type hash `0x72ab39b6`). The callers are the world-setup functions `0x1404949b0` (server) and `0x14039fbb0` (client).
  Sig `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 70 48 8B 01 49 8B E8 ?? ?? ?? ?? 44 24 20 00 41 B1 08 BA 28 03`
- **Registered as a world singleton** through `registerSingleton(world, StringView* name, void* instance, options*)` = `0x1409d50d0`. The name is hashed with FNV-1a and stored in the table at `world+0x7462d0` (entries of 0xE8 bytes: `+0xa0` name hash, `+0xa8` instance). The lookup map is at `world+0x7462f8`. In the server setup the instance comes from `[gameSystems+0x1d8]`; RecipeRegistry comes from `[gameSystems+0x1e0]`. The setup also registers `BalancingRegistry` `+0x1f0`, `workshop::WorkshopRegistry` `+0x1f8` and `impact::AttributeRegistry` `+0x240`.
  Sig `48 89 5C 24 20 55 56 57 41 54 41 55 48 83 EC 20 4C 8B 52 08`
  **Mod tip:** hook `0x1409d50d0`. When `name == "g38_items::ItemRegistry"` or `"g38_items::RecipeRegistry"`, save `r8`. This gives the registry pointer for each world. Confidence: High (byte-decoded call sites + decompiled body).
- **Filled by:** `ItemRegistry_update(reg)` = `0x140ea8d40`, called every frame from the world registry updates `0x14049b2c0` (server) and `0x1403a8140` (client).
  - Phase 0: when the resource is loaded, it reserves the maps, then fills item tags and weapon categories. It then calls `preloadItemInfos` = `0x140e87e10`, which starts loading every `ItemInfo` (resource type `0xb5ce8765`).
  - Phase 1: for each `ItemInfo` that finished loading, it inserts `itemId → ItemInfo*` into the map at `+0x38`.
  Sig (update) `48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 60 80`
  Sig (preload) `48 8B C4 57 41 54 41 55 41 56 48 81`
- **Layout (seen in code):** `+0x00` allocator, `+0x08` resource system, `+0x10` resource handle, `+0x20` `ItemRegistryResource*` (itemRefs, itemTags, weaponCategories, blueprints, ..., dbgNames).
  - `+0x38` HashMap<ItemId, ItemInfo*>. Bitset `+0x40`, capacity `+0x48`, keys `+0x60`, values `+0x78`, count `+0x8c`. Linear probing; slot = `key & (cap-1)`.
  - `+0x90` HashMap<ItemId, debug name>.
  - `+0xe8` HashMap<Guid, preload entry (0x30 B, `+0x10` = ItemInfo*)>.
  - `+0x320` u8 "still loading", `+0x321` u8 "started".

### Lookup functions
- **ItemRegistry_find(ItemId id, ItemRegistry* reg) → ItemInfo*** = `0x140e7a870` (341 callers). Returns NULL for id 0 or unknown ids. Confidence: High.
  Sig `89 4C 24 08 48 83 EC 28 85 C9 74 1B`
- **By GUID:** `preloadItemInfos` computes **FNV-1a-32 over the 16 GUID bytes** (`0x811c9dc5` / `0x1000193`, and 0 for a null GUID) as the key of the debug-name map. So `ItemId = FNV1a32(ItemInfo.objectId)` very likely holds, and a mod can turn a GUID into an id and call `0x140e7a870`. The other way is the GUID map at `+0xe8`. Confidence: Medium (the hash is seen; that `ItemInfo.itemId` uses the same hash is inferred).
- **By name:** there is no name→id map. The map at `+0x90` goes id → `dbgNames` string. A mod can build its own name table by iterating the `+0x38` map and reading `ItemInfo.debugName` (`+0x820`, BlobString `{u32 offset from the field, u32 length}`). **Confirmed at runtime: shipping data has the debug names** (4207 items; EMF `find_item`). Confidence: High.

---

## 2. Inventory components and layout

| component | size | notes |
|---|---|---|
| `ecs::Inventory` | 96 | `slots[8]` of ItemStack. Dynamic, saved, persistent |
| `ecs::InventorySetup` | 84 | `+0x00 linksEntities[16]` (child inventory entities), `+0x40 linksCategories[16]` (InventoryCategory: 1 Customization, 2 Equipment, 3 Currency, 4 Generic, 5 Virtual), `+0x50 inventoryFlags` (0 IsPlayer, 1 IsNpc, 2 ShowContentPreview, 3 IndestructibleWhileNotEmpty), `+0x51 genericSlotCount`, `+0x52 availableSlotCount`, `+0x53 isInitialized` (add-item fails if 0) |
| `ecs::InventoryEmptyState` | 2 | `toggleVFX`, `isEmpty` |
| `ecs::SlotSelection` | 216 | `+0x00 equipment` = `EquipmentSlotData` (20 × InventorySlotId: vanity ×5, armour ×5, ring0/1, rangedWeapon, shield, arrow, spell, fishingBait, hookShot, glider, backpack), `+0xa0 actionbarEntityIds[2]`, `+0xa8 actionItem` (ItemInstanceInfo), `+0xbc disabledEquipment`, `+0xc0 actionbarSlotSelection`, `+0xc8 materialSelection` |
| `ecs::Equipment` | 1380 | server only, not saved. Derived cache: `+0 visualMask`, `+4 impactMask`, `+8 equipmentImpacts[11]` (ItemImpact 72 B), `+0x320 equipmentVisuals[24]`, `+0x560 bodyMaterialId`. Rebuilt by `equipment_impacts` / `equipment_visuals` from SlotSelection + Inventory + ItemState |
| `ecs::RenderEquipment` | 788 | client only. Model handles / colors per visual slot. Written by `render_equipment_models_ag2` |
| `ecs::EquipmentCheck` | 1 | tag (s110 of `apply_charactersetup`) |
| `ecs::DefaultInventory` / `InventorySetupOverwrite` / `InventoryCraftingStock` / `DynamicInventoryCraftingStock` | | starting loot, setup override, "this container counts as base crafting stock" |

Seen in code: `addItem` collects (Inventory*, entityId) pairs from the setup links and loops slots 0..7 with stride 12. The error string "genericSlotCount should be multiple of %u" is in `inventory_setup`. Confidence: High for the 8-slot model.

### The inventory transaction (`InventoryTx`)
Every program builds a **setup block** of 0xF0 bytes on the stack. It holds flags, the ProgramContext and component storage handles taken from the program iterator. The program then calls init, does its operations, and calls commit or rollback.
This means the add/remove functions need storage handles that the program declared in its access lists. They are not free functions that can be called from anywhere.

| name | address | signature | what it does |
|---|---|---|---|
| InventoryTx_init(tx, setup) | `0x140411df0` | `C6 81 A8 71 00 00 00 48 8B 42 08 48` | Copies the setup into tx. tx[0] = ProgramContext*, tx[1] = ItemRegistry*, tx[3] = InventorySetup (read), tx[4] = Inventory storage, tx[5] = ItemState storage, tx[0x12] = OwnerRelationship, tx[0x13] = InventorySetup (write), tx[0x1d] = SlotSelection storage, tx+0xB0 flags, tx+0xB1 stock mode. Leaf function (no .pdata). High (offsets seen in use) |
| InventoryTx_commit(tx) | `0x140408b50` | `40 55 53 41 55 48 8D AC 24 30 FC FF` | Applies the queued op records (0x34 B each, list at tx[0x1e]/tx[0x1f], type at +0x26). Op types: 0 = create pide entity ("Could not find item stack in slot (%k,%d) to create %k in"), 1/6 = destroy (6 = destroyItemStack; for IsContainer items it also clears InventorySetup links to the pide), 2/3 = +/- amount change, 4 = unequip, 5 = move (updates ItemState.containedInSlotId and OwnerRelationship), 7 = equip. Validates stacks ("Invalid Item Stack: EntityId %k ItemId 0x%08x..."). If flag 0x40 (dry run) is set, it rolls back instead. High for role, Medium for op meanings |
| InventoryTx_rollback(tx) | `0x140410e60` | `4C 8B DC 55 56 41 55 41 56 41 57 49` | Undoes the staged changes. Medium |
| InventoryTx_freeArrays(tx) | `0x14021de50` | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 45 33 C0 48 81 C1 40 71 00 00` | Frees the dynamic arrays at +0x7140/+0x6918/+0xf0. High |

Setup flags (tx+0xB0, seen in code): `0x01` = set by every real (non-dry-run) caller seen, meaning unknown, `0x04` = set by crafting, meaning unknown, `0x08` = skip outputs and the success event (craft), `0x10` = skip input consumption (craft), `0x20` = prefer backpack (set from `PlayerSettingFlags.ItemPickupPrioritizeBackpack`, `PlayerInput+0x5b0` bit 11), `0x40` = dry run, `0x80` = do not merge into existing stacks.
tx+0xB1 = stock mode: 0 = own inventory only; 1/2 = also the base "crafting stock" containers (crafting uses 2). tx+0x71A8 bit `0x10` is set when items did not fit (inventory full).
Results are `{u8 error; ...; u32 value at +8}`. Error codes seen: 0x12 null slot id, 0x15 unknown item, 0x16 entity has no InventorySetup, 0x1f no slot / target inventory, 0x27 op buffer full / not allowed.

### Inventory operations (all take `(Result* out, InventoryTx* tx, ...)`)
| name | address | signature | notes |
|---|---|---|---|
| **addItems**(out, tx, EntityId owner, InventorySlotId target (0 = auto), ItemId id, u32 count, Optional<u32>* level, Optional<u8>* rarity, Optional* extra) | `0x14040b170` | `4C 89 4C 24 20 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 48 8D AC 24 60 FF FF` | Fills partial stacks of the same item first (up to `maxStackSize`), then empty slots (`0x14040f460`), and writes stacks via `putStack 0x14040b760`. `out+8` = count that did not fit. 23 callers (loot pickup, admin create, crafting output, redeem, default inventory ...). High |
| **removeItems**(out, tx, EntityId owner, ItemId id, InventorySlotId preferred, u32 count) | `0x14040a080` | `48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 48 89 54 24 10 55 41 54 41 55 41 56 41 57 48 8D AC 24 20 FF FF` | Takes from the preferred slot (or the selected actionbar item) first, then from every linked inventory. `out+8` = amount still missing. Wrapper without a preferred slot: `0x14040a050` (`40 53 48 83 EC 30 8B 44 24 60 48 8B D9 89 44 24`). High |
| **destroyItemStack**(out, tx, InventorySlotId) | `0x14040b920` | `48 89 5C 24 18 48 89 6C 24 20 57 48 83 EC 40 48 8B F9 49 8B` | Queues op 6 (destroys the pide), clears the stack. String "Broken stack encountered on destroyItemStack". High |
| **moveStack**(out, tx, InventorySlotId dst, InventorySlotId src, int mode) | `0x14040d6d0` | `48 89 5C 24 18 55 56 57 41 56 41 57 48 8B EC 48` | mode 0 = all, 1 = half, 2 = one (the reverse order of `InventoryActionType`). Merges the same item or swaps. `out+8` = moved count. Medium/High |
| takeFromSlot(out, tx, slot, mode, flag) | `0x140412110` | `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 50 48 8B F9 41` | mode all/half/one → `takeItems 0x140411f50(out, tx, slot, count, flag)` (`40 53 55 57 41 54 41 56 48 83 EC 60`). Medium |
| putItems(out, tx, slot, ItemInfo*, data, count) | `0x14040fe80` | `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 30 4D 8B F1 49` | Medium |
| getItemStack(out, tx, slot) | `0x14040f340` | `48 89 5C 24 08 48 89 74 24 10 57 41` | Returns ItemStack*. Medium |
| resolveRootOwner(out, EntityId, OwnerRelationship storage, ProgramContext*) | `0x140e80c10` | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B C2 49` | Walks the OwnerRelationship chain up to the top owner (the player). High |

Example (item_equipped_lifetime `0x140400340`, seen in code): for each pide with `ItemState.itemState & IsEquipped == 0` it does `init(tx, setup); destroyItemStack(r, tx, ItemState.containedInSlotId); r.ok ? commit(tx) : rollback(tx)`.

### Player inventory input handler: `inventory_actions` (`0x1403fd610`)
Sig `40 55 41 55 48 8D AC 24 28 14 FF FF`. It loops over players with `PlayerInput` and `ServerConsumedPlayerInput`. Each action carries a `VersionedData`. If `PlayerInput.<action>.versionData != Consumed.<x>`, the action runs and the consumed value is updated. PlayerInput offsets: `fromClient` at +0x8, `fromAdminClient` at +0x3c0.

| PlayerInput field (offset) | consumed | handler |
|---|---|---|
| inventoryTransferAction (+0x8) | +0x20 | `0x1403f62e0` (transfer / move; "Setting Target Slot Id & Target Entity id is not spupported") |
| sortInventoryAction (+0x70) | +0x24 | `0x1403f6de0` (sortInventory) |
| createBuildingItemAction (+0x1fc) | +0x30 | `0x1403f7a60` (only blueprint / terraformer / npc / screwdriver items) |
| itemAction (+0x1e4, version +0x1f4, type +0x1f8) | +0x80 | Inspect `0x1403f8340`, Trash `0x1403f8e70`, UnlockPerk `0x1403f8560`, Tint `0x1403f8c00`, TintRemove inline (via `0x14037f6b0`). Type 1 Consume is not handled here |
| redeemItemAction (+0x2a8) | +0x90 | inline, uses addItems (TwitchDrop / commerce items) |
| upgradeStorageAction | +0x4c | inline ("[UpgradeStorageAction] ...") |
| **adminInventoryCreateAction (+0x3c0)** | +0xa0 | **`0x1403f5940`** (see section 5) |
| adminApplyInventory (+0x45c) | +0xb0 | clear `0x1403f5fa0` + apply default inventory `0x1403f3290` (by GUID +0x460) |
| adminDestroyItemAction (+0x490) | +0xb8 | `0x1403f5170` |
| adminPerkAction (+0x4b0) | +0xc4 | `0x1403f7880` |
| adminRepairAllItems (+0x598) / adminRefillAllFuel (+0x59c) | +0xd4 / +0xd8 | `0x140413e70` / `0x1403f89d0` |

The admin block only runs if `isAdmin(playerEntity, BasicPlayerInfos)` = `0x1403b41f0` returns true. That function checks byte `+0x9d` bit 0 of the 0xA0-byte player info entry at index `entityId-1`.
Sig (isAdmin) `8D 41 FF 4C 8B C2 3D FE 03 00 00 77`. Confidence: High for the dispatch, Medium for the BasicPlayerInfo field.

---

## 3. Equipping

- **Data model:** "equipped" means that `SlotSelection.equipment[slot]` holds the item's InventorySlotId, and the pide's `ItemState.itemState` has the IsEquipped bit set (`item_equipped_state`). The `Equipment` component is only a derived cache.
- **SlotSelection_setEquipmentSlot(EquipmentSlot s, SlotSelection* sel, InventorySlotId v)** = `0x140e990a0`, sig `80 F9 17 0F 87 ?? ?? ?? ?? 4C 8D 0D`.
  **getEquipmentSlot(s, sel)** = `0x140e76040`, sig `80 F9 17 0F 87 ?? ?? ?? ?? 4C 8D 05`. It returns 0 if the stored value is invalid.
  Mapping: EquipmentSlot 0..16 → `equipment[0..16]`, 21 HookShot → [17], 22 Glider → [18], 23 Backpack → [19]. The material slots 17..20 are not stored here. High.
- **ChangeEquipmentAction handler** = `0x14037f7e0` (called from `equipment_actions` `0x140381e70`).
  Sig `48 89 5C 24 18 48 89 7C 24 20 55 48 8D AC 24 30 8E FF FF B8`.
  It reads PlayerInput `+0x1c8` slotId, `+0x1d0` version (consumed `+0x78`), `+0x1d4` EquipmentSlot, `+0x1d5` type. Equip calls **equipItem(out, tx, InventorySlotId, EquipmentSlot)** = `0x14040dff0` (`44 88 4C 24 20 48 89 54 24 10 48 89 4C 24 08 55 56 41 54 41`). Unequip calls **unequipItem** = `0x1404123f0` (`4C 89 44 24 18 53 55 56 41 55 41 56`). After either call it commits or rolls back. High for the dispatch, Medium for the internals of equipItem (not decompiled).
- Other equipment programs:
  - `equipment_actions` `0x140381e70` also runs the weapon gem / ShroudForge path `0x14037fa40`.
  - `swap_equipment_set_actions` `0x140383180` (vanity / active set swap).
  - `equipment_impacts` `0x140381ee0` (pushes item impacts into ImpactSystem).
  - `equipment_visuals` `0x140382220`.
  - `apply_armor` `0x140381840` (ArmorPoints from equipped items).
  - `setup_actionbar` `0x140382fe0`.
  - `item_equipped_state` `0x1404006f0`.
  - `item_equipped_lifetime` `0x140400340` (destroys items that only live while equipped).
  - `render_equipment_models_ag2` `0x140382e90` (client).

---

## 4. Crafting

### Types
| type | size | layout |
|---|---|---|
| `RecipeInfo` (runtime blob) | 168 | `+0x00 recipeGuid`, `+0x10 recipeId`, `+0x14 workshopGuid`, `+0x24 workshopId`, `+0x28 input BlobArray<RecipeInputResource 48 B>`, `+0x30 output BlobArray<RecipeItemStackResource 24 B: itemRef16, +0x10 itemId, +0x14 count>`, `+0x38 craftingDuration Time (ns)`, `+0x40 requiredProps`, `+0x48 requiredPropsMask u64`, `+0x50 knowledgeRequirement`, `+0x5c completionRequirementQuery`, `+0x80 level`, `+0x84 comfortLevel`, `+0x88 serverProgressLevel`, `+0x8c requiredHappyNpcCount`, `+0x90 requiredEnergy`, `+0x94 waterAmount`, `+0x98 waterCharges`, `+0x9f isCookingRecipe`, `+0xa0 isUpgrade`, `+0xa1 requiresSheltered` |
| BlobArray | 8 | `{i32 selfRelativeOffset; u32 count}`. Data is at `&field + offset` (seen in code: `rec+0x30+*(int*)(rec+0x30)`) |
| `ecs::Crafting` | 20 | `workshop ObjectReference`, `+0x10 workshopId`. On stations and NPC workshops |
| `ecs::CraftingInteraction` | 32 | server only: `craftingPropsInRange u64`, `nextRecipeComfortRequirement`, workshop, `+0x1c workshopId`. On the player / NPC |
| `ecs::FactoryStation` | 104 | `+0 state` (0 NoRecipe, 1 MissingMaterials, 2 Paused, 3 Stopped, 4 Running, 5 MissingOutputSpace), `+4 runningRecipe`, `+0x28 nextRecipe`, `+0x2c nextRecipeInputCategorySelections`, `+0x50 recipeStart`, `+0x58 recipePauseDuration`, `+0x60 useAllAvailableCategoryIngredients`, `+0x61 isStoppedByPlayer`, `+0x62 missingRequirements` |
| `ecs::PlayerCraftingAction` (in PlayerInput at +0xbc) | 68 | `craftingStationId`, `+4 recipeId`, `+8 itemId`, `+0xc amount u16`, `+0x10 waterAmount`, `+0x14 waterSourceId`, `+0x18 inputCategorySelections[4]`, `+0x38 useAll`, `+0x3c versionData`, `+0x40 type` (0 CraftRecipe, 1 AutomaticTransfer, 2 SelectCraftingStationRecipe, 3 SetUseAll, 4 StopProduction, 5 TransferWater) |
| `ecs::CraftRecipeEvent` (type hash `0xa4766fb7`) | 56 | `+8 craftingOperatorId`, `+0xc craftingStationId`, `+0x10 recipeId`, `+0x14 recipeAmount u16`, `+0x18 inputCategorySelections` |
| `ecs::CraftRecipeSuccessEvent` (type hash `0x7fe02e20`) | 16 | `+8 workshopId`, `+0xc recipeId` |

The event type hash equals the `qualifiedHash` of the event type in types.json.

### RecipeRegistry singleton (`g38_items::RecipeRegistry`, 0x150 bytes)
- **create** = `0x140e659b0` (`48 89 5C 24 08 48 89 74 24 10 55 57 41 56 48 8B EC 48 83 EC 70 48 8B F2`). It loads `RecipeRegistryResource` (type `0x5f5382cf`; `/build/recipe_registry.kbf`).
- **RecipeRegistry_finishLoading(reg)** = `0x140eac060` (`40 57 48 83 EC 70 48 83 79 20 00 48`). It is called every frame from the same world registry updates (`0x14049b2c0` server, `0x1403a8140` client). While `+0x20` is NULL it polls the resource. Once the resource is loaded, it stores the pointer at `+0x20` and builds:
  - `+0x48` HashMap<RecipeId, RecipeInfo*>. Bitset `+0x50`, cap `+0x58`, keys `+0x70`, values `+0x88`, count `+0x9c`.
  - `+0xA0` HashMap<RecipeInputCategoryId, RecipeInputCategory*>. Count at `+0xf4`.
  - `+0xF8` HashMap<WorkshopId, SmallDynamicArray<RecipeInfo*, 512>>. Elements are 0x1008 B, with the u16 count at +0x1000; values at `+0x138`, count `+0x14c`. It aborts if a workshop has more than 512 recipes.
  - Capacity is reserved as 2 × next_pow2(max(16, n)), so there is room for a few extra entries without a rehash.
  Confidence: High.
- **RecipeRegistry_find(RecipeId, reg) → RecipeInfo*** = `0x140e7fbd0` (`48 89 5C 24 08 48 89 7C 24 10 8B C1 4C 8B D2 85 C9 74 60 83`). 32 callers. High.

### Craft pipeline (server world)
1. **`player_crafting`** `0x1402e0e50` (`40 55 41 57 48 8D 6C 24 D8 48 81 EC 28 01 00 00 41 B8 E0 00`). It reads `PlayerInput.playerCraftingAction` (consumed `+0x44`):
   - type 0: emits `CraftRecipeEvent` through `emitEvent(queue, 0xa4766fb7, 0x38, ...)` = `0x140955400` (`48 83 EC 28 4C 8B D9 41 81 F8 00 01`). Operator = player, station = action.craftingStationId.
   - type 1: `0x1402c18e0` (automatic transfer of ingredients into a station).
   - types 2/3/4: write the station's `FactoryStation` (nextRecipe / useAll / isStoppedByPlayer).
   - type 5: `0x1402c6d00` (water transfer).
   High.
2. **`handle_crafting_events`** `0x140274a40` (`48 89 4C 24 08 55 41 54 48 8D AC 24 78 FD FF FF`). It iterates entities with `Crafting`. For each `CraftRecipeEvent` whose stationId is the current entity:
   - It looks up the recipe ("[crafting] Invalid recipe id '%k'").
   - It checks base permission for two special workshop ids (`0xb326e042`, `0x6f7d5d2f`) via `0x1403b1b20`.
   - It builds a **CraftContext**: `+0 operatorId`, `+4 stationId`, `+8 station workshopId`, `+0x10 InventoryTx setup` (flags 0x05, plus 0x20 if the operator prefers the backpack; stock mode 2), `+0x110 event queue`, more fields up to about +0x168.
   - It calls the craft core `recipeAmount` times. On success with progress tracking it triggers knowledge (`0x141200600`).
   High.
3. **Craft core: `craftRecipe(CraftContext*, RecipeInfo*) → int`** = `0x140470f10` (`48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D AC 24 A0 8B FF FF`). Steps:
   - InventoryTx_init from ctx+0x10.
   - Resolve inputs, including category selections: `0x140e6c770`.
   - `applyRecipe` = `0x1403f1cb0`.
   - For `isUpgrade` recipes, an upgrade check (`0x1404714a0`).
   - **commit**, then emit `CraftRecipeSuccessEvent`. On failure, rollback.
   Return codes from applyRecipe: 0 ok, 2 output did not fit, 4 missing materials / requirements, 5 missing crafting props. High.
4. **`applyRecipe(tx, recipe, inputs, propsMask, levelInfo, ?, EntityId operator, u32 amount)`** = `0x1403f1cb0` (`48 89 5C 24 20 55 56 41 56 48 81 EC 90 00 00 00 4C 8B F2 49`). Steps:
   - Checks `(recipe.requiredPropsMask & props) == mask` and the level / comfort requirements (`0x140e5d360`).
   - **Consumes inputs** with `removeRecipeInputs` = `0x1403f1760` (`48 89 5C 24 10 44 89 4C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 81 EC B0 00 00 00 45 33 ED 45`). It uses removeItems on the operator and, in stock mode, on the base storage chests found by `0x1404682e0`. The dry-run variant is `0x1403f2480`.
   - Consumes water charges (`0x1403f19f0`).
   - **Produces outputs:** `addItems(tx, operator, 0, output.itemId, amount*output.count)` for each output entry.
   High.
   Note: `0x140e99af0` (`xor al,al; ret`) is a stub that returns false. It is the "ignore missing materials" check, but the same code is probably shared by unrelated functions (COMDAT-folded), so **do not patch it**. Hook `0x1403f1760` instead.
5. **Automated stations: `factory_station`** `0x140330660` (`40 55 41 56 48 8D 6C 24 D8 48 81 EC 28 01 00 00 41 B8 E0 00`). This is the state machine on `FactoryStation.state`:
   - `0x1403286e0` picks up `nextRecipe`.
   - `0x140327aa0` tries to start (pulls materials; `48 89 74 24 10 48 89 7C 24 18 55 41 56 41 57 48 8D AC 24 60 8A FF FF B8`).
   - When Running and `recipeStart + craftingDuration*speed <= now`, it calls **`0x1403282a0`** = complete (`48 89 5C 24 08 48 89 74 24 10 57 48 81 EC 00 02 00 00 48 8D`).
   - `0x14031ff90` / `0x140328130` also call the craft core.
   - `factory_station_info` `0x1403309f0` fills `ServerFactoryInfo` for the UI.
   High for the states, Medium for the helper names.
6. **Support programs:**
   - `crafting_props_in_range` `0x140455a60` (sets CraftingInteraction.craftingPropsInRange from PlayerBasesSystemServer).
   - `npc_recipe_comfort_requirements` `0x140457f40`.
   - `crafting_data` `0x1402a2230` (copies to ClientCraftingInteraction).
   - `base_crafting_prop_register` `0x140455950`.
   - `player_cooking` `0x1402df030`.
   - `gem_crafting_sequence` `0x140274690`.
   - Client-side UI: `unlocked_recipes_ui` `0x140316080`, `available_crafting_props_ui` `0x1402fed00`, `cache_crafting_stock` `0x1402af210`, `ui_npc_crafting_requirements` `0x14045a7f0`.

### Confirmed in game: crafting for a player (`CraftRecipeEvent` with a player as the operator)
Build 1273043, a LOCAL test character (level 1) in the LOCAL test world. A test mod sent the event from its own server
program and logged the operator's inventory every tick, `CraftRecipeSuccessEvent` and `PickupEvent`.
The test world has no workbench, so one was spawned (`spawn ebf450c3-...` = `Craftingstation_Workbench`, workshop
`4da88f73`, 166 recipes); a charcoal kiln (`T1_Charcoal_Kiln_Factory`, workshop `2546ae49`) for a timed recipe.
- **(a) It crafts N with the player's own materials.** Operator = player 1, station = player 1 (hand crafting, workshop 0),
  wooden arrows `278240d5` (10x <- 2 twigs) x5 with 10 twigs: the twigs went, 50 arrows came into the player, in the
  step after the send (31-47 ms). The game sent 5 `PickupEvent` (pickedUpBy 1, from 0, x10) and 5
  `CraftRecipeSuccessEvent {workshop 0, recipe}`. At the spawned workbench, stone blocks `852c5432` (100x <- 2 stone)
  x3: -6 stone, +300 blocks, 3 success events with workshop `4da88f73`.
- **(b) No timer.** Every craft that worked finished in the step after the send; `craftingDuration` is not applied by
  this path (all 166 workbench recipes and 56 of the 57 hand recipes have duration 0). The timed recipes found (kiln:
  charcoal 120 s, tar and wood acid 300 s; hand: fishing bait 210 s) all have category inputs; charcoal x1 at the kiln
  with 26 logs did nothing (no craft, `FactoryStation` stayed state 0). So a mod that wants timed recipes paced must
  pace them itself.
- **(c) Too few materials: as many as they pay for.** Arrows x5 with 5 twigs: 2 crafted (-4 twigs, +20 arrows, 2
  success events), 1 twig left, the other 3 skipped silently. Nothing crafted without materials (with a chest as
  the operator, see `re_villager_jobs.md`), and a craft whose output has no room does nothing (both confirmed in game).
- **(d) Checks:**
  - **No knowledge check.** A level-1 character crafted `Ammo_T7_Spell_Fire_Bolt_lvl47` (`57a21e8b`, knowledge
    `e75245d1`) at once from 4 resin + 4 diamonds. Statically: neither `player_crafting` nor `handle_crafting_events`
    reads the `G38Knowledge` singleton (`tools/re/program_access.py`); only client UI programs (`unlocked_recipes_ui`,
    `player_ui`) do. The server trusts the client's UI to offer only unlocked recipes.
  - **The station must be an entity with `Crafting` whose workshop has the recipe.** A hand recipe at the workbench, a
    workbench recipe at the player, a hand recipe at the flame altar and a recipe at a chest (no `Crafting`) all did
    nothing.
  - **No distance check.** Arrows with station = a villager 115 m away (workshop 0) crafted into the player.
  - Level, comfort and props: every hand and workbench recipe of this build has requiredPropsMask 0, level 0 and comfort
    0, so these were not tested in game; `applyRecipe` checks them (above) and `handle_crafting_events` reads `Comfort`,
    `Level` and `ecs::ServerProgressSystem`. The altar's upgrade recipes (`isUpgrade`, levels 0-3) were left alone.
  - Materials came only from the player: with the stone only in a chest 2 m from the player and the altar, stone
    blocks x2 did nothing (the "stock" mode of the transaction did not reach that chest).
  - Category inputs with `inputCategorySelections` all zero: the game picks for itself. String `7e4e353a` x2 with 10
    fibre made 2 string from 6 fibre.
- **(e) The station need not be in use.** No station was opened in any of these sends; the player had no
  `CraftingInteraction` component at all on the server.
- **The pinned recipe is client-only.** The player had no `PlayerPinnedRecipes` in the server world; the only writer is
  `player_pin_recipe_input` (flags 0x1, `UiPinRecipeEvent`), the only reader `player_ui` (client). A server cannot read
  a guest's pin.
- **Mod-sent events are seen by `on_event<CraftRecipeEvent>`** in the step they are processed, with the operator,
  station, recipe and amount as sent, so a mod can tell the game's own crafts from its own by counting what it sent.
- Consequence for Bulk Craft (`mods/bulk_craft`): the event is safe for items (the game takes the materials and gives
  the output in one transaction, all or nothing per craft), but the mod must check knowledge, range and pacing itself.

---

## 5. Giving items, adding recipes, NPC crafting (modding hooks)

### (a) Give items to a player (server / host only; clients only receive replication)
1. **Easiest: use the admin path.** On the server, write a `keen::ecs::AdminInventoryCreateAction` into the player's `PlayerInput.fromAdminClient` (`PlayerInput+0x3c0`) and change its `versionData` (`+0x3c4`) to a value different from `ServerConsumedPlayerInput+0xa0`. The next tick of `inventory_actions` runs **`0x1403f5940`** (`48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 F0 8D FF FF`). Fields: `itemId +0`, `count +8`, `hasItemRarity +0xa`, `rarity +0xb`, `level +0xc`, `createForAllPlayers +0xd`, `createFullStack +0xe` (count = maxStackSize, otherwise 1), `fillEmptyStacks +0xf` (repeats until full).
   Problem: the block is gated by `isAdmin` `0x1403b41f0`. Either hook it to return true for mod requests, or set the admin bit. Note that the handler ignores `count`; it uses 1 or a full stack.
2. **Cleaner: hook the `inventory_actions` update (`0x1403fd610`) or its admin handler.** Call `0x1403f5940(ProgramContext*, ItemInfo*, playerEntity, AdminInventoryCreateAction*, iter)` with the iterator the program already has. Or build an InventoryTx from the same iterator and call `addItems 0x14040b170` with any count, then commit `0x140408b50`. This needs the iterator slot layout of `inventory_actions` (not fully mapped yet; the handler reads iter `+0xa8 +0xd8 +0xf0 +0x118 +0x148 +0x158 +0x160 +0x168 +0x178 +0x190 +0x1a8`).
3. Item ids come from `ItemRegistry_find` / FNV-1a(GUID) (section 1).

### (b) Add a recipe at runtime
- Hook the end of `RecipeRegistry_finishLoading` `0x140eac060` (server and client both have a registry; patch both so the UI and the server agree).
- Build a `RecipeInfo` (168 B) in your own memory. **The input/output BlobArrays use self-relative int32 offsets**, so put the arrays right after the struct and set the offsets relative to each field.
- Insert `recipeId → RecipeInfo*` into the map at `reg+0x48`, and push the pointer into the workshop list in the map at `reg+0xF8` (key `workshopId`). The maps use linear probing on `key & (cap-1)`, with a bitset of used slots.
- The recipe will also need knowledge / unlock state: `knowledgeRequirement` at `+0x50`. Zero probably means always available; not verified.
- New items (outputs) need an `ItemInfo` in the item map at `+0x38`. The same idea applies, but ItemInfo is much bigger and references models and loca.

### (c) Let an NPC craft
- In the vanilla game an NPC "crafts" only as a station: the player crafts at an NPC entity that has `Crafting` (workshop with `isNpc`). `handle_crafting_events` treats `craftingStationId` = NPC entity and takes and puts items in the **operator's** inventory.
- For autonomous NPC crafting, a mod can do one of two things:
  - Emit a `CraftRecipeEvent` (`emitEvent 0x140955400`, hash `0xa4766fb7`, size 0x38) with operator = the NPC (or a chest entity that has an InventorySetup) and station = an entity with `Crafting`.
  - Call the craft core `0x140470f10` from inside a hooked program with a filled CraftContext.
  Either way, the operator needs an initialized `InventorySetup` (`+0x53`), or addItems fails with 0x15/0x1f.
- Another option is to reuse `FactoryStation` automation (state machine above), which already crafts on a timer without a player.
- Confidence: Medium (the paths exist; nobody has tested an NPC operator).

### Other useful hook points
- `InventoryTx_commit` `0x140408b50`: one place that sees every inventory change (op list at tx[0x1e], count tx[0x1f]). Good for "on item added/removed" mod events.
- `addItems` `0x14040b170` / `removeItems` `0x14040a080`: change loot or stack sizes, block items.
- `craftRecipe` `0x140470f10` / `applyRecipe` `0x1403f1cb0`: change costs and outputs, "free crafting".
- `CraftRecipeSuccessEvent` consumers (for example `event_tracking`): an "on crafted" callback.
- `equipItem` `0x14040dff0` / `unequipItem` `0x1404123f0`: on-equip events, slot restrictions.

---

## ECS programs for this topic (name, update fn, flags)
| program | update fn | flags | role |
|---|---|---|---|
| inventory_actions | `0x1403fd610` | 0x200 | player inventory input, admin item commands |
| inventory_setup / inventory_setup_init | `0x1403ffc10` / `0x140400220` | 0x210 / 0x8 | builds InventorySetup links |
| default_inventory | `0x1403fc380` | 0x200 | fills starting / loot inventory (`0x1403f3290`) |
| loot_pickup_item / loot_pickup_in_range | `0x140400d00` / `0x140400b50` | 0x200 | pickups → addItems |
| immediate_loot_action / player_items_loot / area_deposit_trigger | `0x1403fcd10` / `0x140402290` / `0x140272540` | | loot and deposit |
| item_state / item_state_init / item_equipped_state / item_equipped_lifetime | `0x140236e10` / `0x1404008a0` / `0x1404006f0` / `0x140400340` | | pide item state |
| equipment_actions / swap_equipment_set_actions | `0x140381e70` / `0x140383180` | 0x210 / 0x200 | equip / unequip, set swap, weapon gems |
| equipment_impacts / equipment_visuals / apply_armor / setup_actionbar | `0x140381ee0` / `0x140382220` / `0x140381840` / `0x140382fe0` | | derived equipment state |
| render_equipment_models_ag2 | `0x140382e90` | 0x601 | client render models |
| player_crafting | `0x1402e0e50` | 0x210 | craft input → CraftRecipeEvent |
| handle_crafting_events | `0x140274a40` | 0x200 | executes crafts |
| factory_station (+_info, _init, _toggle, _sfx, _vfx) | `0x140330660` (`0x1403309f0`, `0x140330ec0`, `0x140330f30`, `0x140305bc0`, `0x140305eb0`) | 0x200 | automated stations |
| crafting_props_in_range / npc_recipe_comfort_requirements / crafting_data | `0x140455a60` / `0x140457f40` / `0x1402a2230` | | crafting context |
| player_cooking / gem_crafting_sequence | `0x1402df030` / `0x140274690` | | cooking, gems |
| predict_player_inventory | `0x1402e5310` | 0x14 | client prediction |

Singletons: `g38_items::ItemRegistry`, `g38_items::RecipeRegistry`, `g38_items::ItemKnowledgeDatabase` (client), `g38_items::ItemOrders` (client), `workshop::WorkshopRegistry`, `BalancingRegistry`, `KnowledgeTriggerQueue`, `ServerFactoryInfo`, `PlayerBasesSystemServer` (base storage for crafting stock).

## Open questions
- The full layout of the 0xF0-byte InventoryTx setup (only about 10 of 30 handles are identified). It is needed to call addItems from a mod's own context.
- The exact meaning of commit op types 2/3/4. The internals of equipItem / unequipItem were not decompiled.
- Whether `ItemInfo.itemId == FNV1a(objectId)` for every item. Whether shipping data has `dbgNames`.
- How the client crafting UI enumerates recipes (the workshop map or knowledge DB) and what an injected recipe needs to show up.
- The BasicPlayerInfo layout (admin bit at `+0x9d` is inferred from one check).
