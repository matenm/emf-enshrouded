# Villages: farming, animals, trading, NPC-run bases

Build **1273043** (image base 0x140000000). Static only: types.json via `tools/re/show2.py`, the generated
`components.hpp`, `export/templates.csv` / `template_guids.csv` / `items.csv` / `export/samples` / `export/interesting`,
string anchors, the program catalog rebuilt for 1273043 (`programs.py` with registerProgram `0x1409d6f60`; the
repository's caches are for 1269579), and a few functions read as raw disassembly with capstone, not with REA/Ghidra.
In-game results are in "Runtime results" at the end. Addresses are **1273043** unless marked "(1269579)".

Source tags: **[types]** types.json / components.hpp, **[tpl]** template catalog or template dump, **[str]** string in
.rdata, **[asm]** disassembly of the 1273043 function, **[docs]** other repository docs, **[guess]** inference.

## Feasibility summary

| feature | how a mod could do it | confidence | what to verify in game |
|---|---|---|---|
| Villager harvests ripe crops into a chest | find roots with `Growth`; ripe = root `Level` attr == last `Upgrade.levels[].level`; walk there (`NpcCommandEvent` GoToPosition); emit **`LootInteractionEvent {guest = chest, host = stage entity (DynamicUpgrade.entityId), offerId 0, lootAll 1, bypassInteraction 1}`**: the game's `immediate_loot_action` rolls the stage's `DefaultInventory` loot table into the guest and destroys the stage | High for the mechanism (the game itself emits this exact shape with bypass=1); Medium that a chest guest is fine | `PickupEvent` with pickedUpBy = chest; items land; stage fades; root disappears ~4 s later; chest-full = nothing happens |
| Replant | after harvest the root destroys itself (vanilla behaviour). Either `take_items(chest, seed)` + `spawn` the `T*_Plant_Growth_*` root at the old position, or the "re-arm" trick: write the root's `Level` back to the first level before the old stage is gone | Medium (spawn), Low (re-arm) | spawned root grows and **survives a reload** (a spawned chest did not); soil must be an `allowedSoil` or growth destroys it |
| Feed animals | `put_items(feeder, "Material_T3_Turkey_Feeding", n)` (feeders are `Container`s with `InventorySetup`); or take from a chest first | Medium | feeder accepts it via `put_items` (its slots are ItemCollection slots); animal `needs` bit Food clears |
| Collect eggs/milk/wool | same `LootInteractionEvent` with host = the farm animal (it has its own `Inventory`, refilled by `interaction_loot_refill`), guest = chest | Medium-High | the animal's offer verb, items moved, refill resumes |
| Craft feed / goods for the village | existing `CraftRecipeEvent` (operator = chest, station = Farmer or other NPC) as in `villager_jobs` | High [docs] | Farmer workshop `0xd9a35a6f` recipes (dump at runtime) |
| Trade | no shop system. NPC "traders" are NPC **workshops** with barter recipes (Fisher trader `0xc4acc104`, Chinese-New-Year trader `0x8554479a`). Mod trade = `CraftRecipeEvent` against a trader station, or a mod-defined exchange with `take_items` + `put_items` | High (no shop), Medium (recipe route) | dump the trader recipes; whether a villager with its dynamic `Crafting.workshopId` set to a trader workshop works as the station |
| Second, NPC-run village | build (player) a second flame-altar base; move villagers there with `NpcCommandEvent` **Reset** (sets spawnPosition + teleports); assign beds by writing `NpcBed.associatedNpc`; mod keeps per-village job data in a sidecar | Medium | bed link (`npc_bed` sets `NpcCommon.bedId`), villager stays, happiness bits |
| Villagers work while the player is away | vanilla does **not** simulate there: villager worker pauses ≥150 m from a player; `growth`, `animal_food_consumption` are gated by terrain streaming / active cell. A mod would have to simulate remote villages abstractly (timers + `put_items`/`take_items` on chests) | High (the gates), Low-Medium (abstract sim: far entities may not be reachable) | `get_component` on a chest 1-2 km from the player; whether events on far entities are processed |
| Villager food need | the villager happiness bit **Food is hard-wired on** every tick (`or byte [ActiveNpcStateSaved+0xe], 4` at `0x140455935`); state 12 `Eating` and decision `WantsToEat` are wired but unused | High (stub found) | a real hunger loop would touch `requiredHappyNpcCount` gates (flame upgrades) |

---

## 1. Farming

### Facts
- **The plant entity ("root")** is a `Plant`-family template `T<tier>_Plant_Growth_<Crop>` (66 templates, e.g. `T1_Plant_Growth_Tomato`, `_Base_Plant_Growth`) with `Growth, GrowthHint, Level, Upgrade, NetworkGrowth, NetworkLevel, AttachToSurface, ClientAttachCursorOnHover, CurrentTransform, StaticTransform, ...` and **no** inventory or interaction. [tpl]
  - `Growth` (40 B, server_only, savegame_relevant, template): `+0 minDuration` Time, `+8 randomDurationScale {min,max}`, `+0x10 hasSubmergedRequirement`, `+0x14 submergedRequirement {min,max}`, `+0x1c allowedSoil BlobArray<VoxelRequirement {soil ref 16, +0x10 soilMaterialId u8, +0x14 factor f32}>`. [types]
  - `DynamicGrowth` (hash `0x48a1b98b`, 16 B): `+0 nextLevelTime` Time, `+8 customGrowthLimit` u8 (player-set limit; `CursorChangeGrowthLimitAction.growthLimitDelta` through the cursor). [types]
  - `NetworkGrowth` (2 B): `voxelMaterialId` (soil under the plant), `customGrowthLimit`. [types]
  - `Upgrade` (template, 8 B): `levels BlobArray<UpgradeData 0x68 B>`: `+0 upgradeEntity` TemplateReference, `+0x10 level` s32, `+0x14 growthFactor` f32, sfx/vfx/offset, `+0x60 hasUpgradeAnimation`, `+0x64 height`. [types]
  - `DynamicUpgrade` (hash `0x90c3a700`, 12 B): `+0 entityId` = **the current stage entity**, `+4 shouldPlayAnimation`, `+8 spawnedLevel`. [types]
  - `Level` (16 B): attribute block `{rootId, storageOffset u16, flags, storageCount, dataStorage[2]}`; the growth level is attribute id **`0x724bf568`** (the same id as the player level, re_take_items.md), in practice `dataStorage[0]` (`Level+8`). [asm, docs]
- **Stage entities** (spawned per level by program `upgrade`): `T1_Plant_Growth_Tomato_Seedling_0`, `_Seedling_1`, `_Seedling_2_harvest` (naming is not uniform: some final stages are `Scatter_Loot_*_Harvest` templates; 52 crops have stage templates). Every stage is a `Prop` with `DefaultInventory, DestroyOnLoot, Destructible, InteractionOfferRuntime, ClientInteractionOffer, UpgradeAnimation, ...` (no `InventorySetup`). [tpl]
  - `DefaultInventory` (20 B): `+0 defaultInventory` GUID of a `keen::ecs::DefaultInventoryResource` (420 of them: a loot table of groups/stacks with `countMin/countMax/spawnProbability/weight`), `+0x10 keepOnRandomLoot`, `+0x11 applied`. [types, export/samples]
- **Program `growth`** `0x140332d10` (1269579 `0x140332b70`; tpl Growth, Upgrade; write DynamicGrowth, Level, DynamicUpgrade, NetworkGrowth) [asm]:
  - Runs each root only every 256 ticks (`0x1409f3d00(ctx, 7)`: `((id & 0xff) + tick) & 0xff == 0`, ~4.3 s).
  - **If `DynamicUpgrade.entityId != 0` and that entity no longer exists (`0x14099d7f0`), it queues destroy of itself** (`0x1409554c0(cmds, self, self)` = command type 1 = destroy, the 1273043 twin of 1269579 `0x140954250`). So **harvesting a crop removes the whole plant**; there is no auto-regrow.
  - Area gate `0x1403ce7f0(ctx, VoxelWorld, pos)`: terrain streamed + active cell, else skip (plants do not grow away from players).
  - Soil check `0x140320370`: voxel query under the plant; result code 3 = no data (skip); material not in `allowedSoil` → code `0x1f` → **the plant destroys itself**; match → growth factor = `VoxelRequirement.factor` (empty list = 1.0). The soil id is mirrored into `NetworkGrowth.voxelMaterialId`.
  - Level-up: if level < min(last `UpgradeData.level`, `customGrowthLimit`) and `now >= nextLevelTime` (0 = first run): `level += 1` (first run: level 1), `nextLevelTime = now + minDuration × rand(randomDurationScale) × soilFactor × f(GameSettings.plantGrowthSpeedFactor) × UpgradeData[level].growthFactor` (`0x140e81b00` reads `BalancingRegistry+0x90 → GameSettings+0x34`), sets `DynamicUpgrade.shouldPlayAnimation = 1`, writes the Level attribute and recomputes it (`0x140231a70`). Only one level per elapsed timer, so no catch-up after the area was unloaded. [asm]
- **Program `upgrade`** `0x14027e6b0` (tpl Upgrade; read Level; write DynamicUpgrade; flag 0x10): when Level changes it destroys the old child (`0x1409554c0`) and spawns `UpgradeData.upgradeEntity` for the new level, storing it in `DynamicUpgrade.entityId`. Also used by flame altars (PlayerSpawnPoint). [asm, partly guess for the exact spawn path]
- **"Ripe"**: root `Level` == the last `Upgrade.levels[n-1].level` (or == `customGrowthLimit` if the player capped it; the HUD strings `readyForHarvest`, `fullyGrown`, `growthLimitReached`, `growing`, `growingNotPossible`, `waterLevel` come from `FbUiLocaHudPlantGrowth`). The harvestable entity is `DynamicUpgrade.entityId`. Earlier stages are lootable too (probably the seedling back). [asm, types, str]
- **How the player harvests**: E on the stage → `AcceptOfferedInteractionEvent` → `interaction_attempt` → `InteractionAcceptedEvent` → the player's harvest sequence fires `actor::LootInteractionActionEvent {lootAll}` → `actor_game_events` `0x140263050` emits **`LootInteractionEvent`** (hash `0x730ab56d`, 128 B: `+8 guestId` (player), `+0xc hostId` (stage), `+0x10 offerId`, `+0x14 lootAll`, `+0x18 knowledgeUnlockOperations[8]`, `+0x78 bypassInteraction` (left 0)). [asm, types]
- **Program `immediate_loot_action`** `0x1403fd090` (1269579 `0x1403fcd10`; flag 0x4 system; events-in LootInteractionEvent) [asm]:
  1. Host must exist and have `InteractionOfferRuntime`. **If `bypassInteraction == 0`**, it requires `InteractionOfferRuntime.lastAcceptData.guestId (+0x18) == guestId` and `.offerId (+0x24) == offerId`; with `bypassInteraction = 1` this check is skipped.
  2. The guest only needs an inventory: `PlayerInput` is used just for a "prefer backpack" flag (`0x14040ff30`, sets setup flag 0x20).
  3. **Path A (host has `DefaultInventory`, no inventory: crop stages):** reads the host's Level attribute, rolls the loot table (`0x1403f5760` with the resource system), and `addItems(owner = guestId, auto slot, item, count, level)` for every stack. **All or nothing:** any error or leftover count → rollback (a full chest gets nothing and the plant stays). On success: queues "add component" `DestroyOnLootActivated` (`0x52525bab`) on the host and sets `DestroyOnLoot.shouldDestroy (+0x41) = 1`, then commits. `destroy_loot_container` `0x1403fcb30` then fades and destroys the stage, and `growth` kills the root.
  4. **Path B (host has a real inventory: loot containers, farm animals):** walks the host's 8 slots and moves them to the guest (`0x14040e160`; `lootAll` = whole stacks of every slot, else one item of the first slot), one commit per slot.
  5. Knowledge triggers only for guests in a player group (`ServerPlayerGroupMembers`).
- **The game itself emits a bypass loot event:** `player_control_action_sequence` (`0x1402d7990`) helper `0x1402ec570` emits `LootInteractionEvent {guest = self, host = target, offerId 0, lootAll 1, bypassInteraction 1}` after its own range checks (a "loot in range" feature). This is the exact shape a mod would send. [asm]
- **Planting**: seed items `T1_Seedling_Vegetable_Tomato` etc. (`items.csv`) are placed with the placement cursor (the plant root has `ClientAttachCursorOnHover`; `server_cursor_no_prediction` reads `DynamicGrowth`/`Upgrade` of others and writes `DynamicGrowth`). The exact spawn path (building `place_prop` vs an item sequence `SpawnEntityBaseEvent`) was not traced. [tpl, docs, guess]
- Farmland is a **voxel soil material**, not an entity (`Growth.allowedSoil`, `NetworkGrowth.voxelMaterialId`, loca `soilGrowthSpeedFactorHeader`, `preferredSoil`). `ActiveNpcTarget.setup.isFarmland` exists but is never tested by the villager AI [docs]. Factory props `T1_Seedling_Factory` (the Seedbed, `FactoryStation` + `Fuel`) and `T1_Farmbed_Factory` (`FactoryStation`) carry `ActiveNpcTarget`. [tpl]
- `GameSettings.plantGrowthSpeedFactor` = `+0x34` (`BalancingRegistry+0x90` → GameSettings). [types, asm]

### Candidate hooks for a mod
- **Find**: iterate entities with `DynamicGrowth` (`0x48a1b98b`) inside the village radius; ripe test as above (`get_component<Level>` `dataStorage[0]` vs `get_template<Upgrade>` last level).
- **Harvest into a chest**: from the mod's server program (events-out `LootInteractionEvent`): `guestId = chest` (or the villager, which has an `InventorySetup` [docs], then move to the chest with `take_items`/`put_items`), `hostId = DynamicUpgrade.entityId`, `offerId = 0`, `lootAll = 1`, `bypassInteraction = 1`. Optionally walk the villager there first with `NpcCommandEvent` GoToPosition.
- **Replant**: (a) record the root's position and template before the harvest, then `take_items(chest, seed, 1)` and `spawn(root template, pos)`; (b) "re-arm": while the looted stage is still fading, write the root's Level attribute back to the first level (+ `DynamicGrowth.nextLevelTime = 0`) so `upgrade` spawns a seedling and `growth` finds a live child. (b) keeps the player-built root (which is saved) but depends on ordering and needs a component write with change tracking (`program_write_other`, API v16).
- **Speed / boost**: `GameSettings.plantGrowthSpeedFactor` (global), or `DynamicGrowth.nextLevelTime` per plant.

### Risks
- A spawned root may not be saved (a spawned chest was not [docs]; all villager templates carry `ForceSave` [tpl], plants and chests do not). Test with a root spawned inside a base.
- A chest as guest in `addItems` is proven for crafting [docs] but not for this program; a stage with a level-scaled loot table might need the guest's level.
- `destroy_loot_container` timing (fade) and the 256-tick slice: the root disappears up to ~4 s after the harvest.
- Item-collection or category-restricted chests may refuse some items (all-or-nothing → retries forever; the mod should detect "no PickupEvent").

---

## 2. Animals

### Facts
- **Farm animals** are tame animal templates: `PLACEMENT_HELPER_T3_Animal_Farm_Turkey` etc. (`Animal` family, faction `FarmAnimal`, `IsTame`), babies `Animal_Baby_*` (faction `PlayerAlly`, `BabyAnimal`), wild versions `Animal_Wildlife_*_AG2` (identical component set except `IsTame`; `wildlife_farm_animal_transformer` switches Faction to FarmAnimal when tamed). They are also items (`Animal_Farming_Turkey` etc., `ItemFlag::IsAnimal` bit 10): animals can be picked up and placed (`animalPickupNotPossible`, `[save2] Failed to load Animal inventory`). Types: `animal::Type` None, Fish, FarmAnimal, Bunny, Catdeer, Goat, SandDigger, Turkey, Yak, Capybara, Pet, Cat, Dog, Panda. [tpl, types, str]
- `Animal` (template, 656 B): sequences (eat, pet, ...), `+0xbc type`, `decisionSetup`, `+0x130 searchTargetSetup {targetRadius, bedRadius}`, **`+0x208 hungerSetup {timeUntilHungerMin/Max, foodConsumptionRate}`**, `+0x220 reproductionSetup {maxReproductionDistance, baseReproductionChance, reproductionRate}`, `+0x260 babyAnimalVersion`. Turkey: hunger after 30-90 s, **foodConsumptionRate 600 s**, reproduction every 600 s within 15 m. [types, interesting/Animal_Baby_T3_Turkey.txt]
- `AnimalSaved` (56 B, saved): `+0x10 lastFoodConsumptionTime`, `+0x18 lastValidReproduction`, `+0x20 willConsumeFood`, `+0x28 nextReproductionCheck`, **`+0x30 needs`** (`Bitmask8<animal::Needs>`: Shelter=bit0, Sleep=bit1, Food=bit2). `DynamicAnimal` (240 B): `state` (animal::State: Idle, Roam, Sitting, Sleep, Eating 8, DrinkWater, ...), `+0x50 timeUntilHungry`, `+0x58 eatingCooldown`, `+0x60 hungerState {foodTargets[8], queries}`, `reproductionState`. `NetworkAnimal.needs` for the UI. [types]
- **Needs → unhappy** (`0x1402294e0`, inside the animal behaviour code): sets **actor state flag 22 `Unhappy`** (`Actor+0xbf8` bit 22) = `needs != 0`; Shelter bit set when actor flag 8 `Sheltered` is missing; Sleep bit cleared when a bed is found. [asm]
- **Feeders**: `Animal_Feeding_Feeder_Farming_{Bunny,Capybara,Catdeer,Goat,Sanddigger,Turkey,Yak}` and pet feeders `_Cat`, `_Dog`, plus `Animal_Feeding_Station`; family **Container** with `AnimalTarget` (`setup {isBed, isFood, isInteresting}`, `types` animal-type mask), `Inventory`, `InventorySetup(+Init, +Overwrite)`, **`ItemCollectionComponent`** (slots accept items by `ItemCollectionId`), `InteractionLock`, `UiInteractable`; HUD screen `FeedingStation` (24); loca `feedingStationStackAdditionMaxPossible`. Placeable items `Animal_Farm_Feeder_*` (stack 50). Beds `Prop_Animal_Farming_Bed(_YAK)` (`AnimalTarget` + `ActiveNpcTarget`). [tpl, types, str]
- **Food items** (ItemCategory `AnimalFood` 13 / `PetFood` 14): `Material_T1_Goat_Feeding`, `Material_T1_Bunny_Feeding`, `Material_T2_Catdeer_Feeding`, `Material_T3_Turkey_Feeding`, `Material_T4_Sanddigger_Feeding`, `Material_T5_Yak_Feeding`, `Material_T6_Capybara_Feeding`, `Material_T2_Cat_Feeding`, `Material_T2_Dog_Feeding` (stack 200). [items.csv, types]
- **Program `animal_food_consumption`** `0x14022ba20` (tpl Animal; write DynamicAnimal, AnimalSaved; other-tpl AnimalTarget; other-write the feeder's `Inventory`...; only `IsTame`): time-sliced and area-gated (`0x1403ce7f0`), finds feeders, consumes from the feeder inventory (helper `0x1402259f0`), updates `lastFoodConsumptionTime` / `willConsumeFood`. "Rate of animals consuming food from the feeding stations" [str]. [asm partly, docs]
- **Animal products** = `InteractionLootRefill` on the farm animal (also on the wild one): `{+0 waitTime, verbLoca, defaultSequence, items BlobArray<LootRefill {lootableItemRef, refillCount, maxCount}>}` + `DynamicInteractionLootRefill.refillTimer`. **Program `interaction_loot_refill`** `0x1402a47f0` [asm]:
  - Timer helper `0x1402a7b20`: if `refillTimer == 0` → set it to `waitTime`; **the countdown pauses while the actor has state flag `Dead` (7) or `Unhappy` (22)**; else `refillTimer -= dt`, refill when ≤ 0.
  - Refill: for each LootRefill item, count what the animal already holds (`0x140e7c120`) and `addItems(owner = the animal, item, refillCount)` up to `maxCount`, into the **animal's own `Inventory`**; then it sets `InteractionOfferRuntime` override verb/sequence (the "collect" prompt).
  - Collecting is a normal loot interaction (path B of `immediate_loot_action`, section 1).
  - Candidate products by name (mapping not verified): `Food_T3_raw_egg_BirdEggs`, `Material_T3_Feather`, `Food_T1_raw_hydration_Milk`, `Food_T2_raw_bad_hydration_CatdeerMilk`, `Food_T5_raw_hydration_MountaingoatMilk`, `Material_T5_raw_Wool`, `Material_T1_Animal_Fur`. [items.csv, guess]
- Limits: `BalancingTable.maximumAnimalsInWorld` (`+0x1ac`); "reproduction chance reduced gradually to 0% when the world animal limit is reached" [str]; cursor flag `MaxTameAnimalCountReached`. `BabyAnimal.growTime` "only applied while happy" [str]. Base counters in `ClientBaseIdComponent`: `farmingAnimalCount +0x28`, `hungryFarmingAnimalCount +0x2c`, `animalsWithoutBedCount +0x2e`, `npcBedCount +0x30`, `farmingAnimalBedCount +0x32`, `occupiedFarmingAnimalBedCount +0x34`. [types]

### Candidate hooks for a mod
- **Feed**: `put_items(feeder, food item, n)` (or `take_items(chest)` then `put_items(feeder)`); the feeder has an `InventorySetup`. Watch `AnimalSaved.needs` bit 2 and `hungryFarmingAnimalCount`.
- **Make feed**: `CraftRecipeEvent` at the Farmer NPC (workshop `0xd9a35a6f`) with operator = chest, if the feed recipes live there (check at runtime).
- **Collect produce**: `LootInteractionEvent {guest = chest, host = animal, offerId 0, lootAll 1, bypassInteraction 1}`; or `take_items(animal, item, n)` + `put_items(chest)` (the animal has an inventory).
- **Status for a village UI**: `ClientBaseIdComponent` counters, `AnimalSaved.needs`, `DynamicInteractionLootRefill.refillTimer`.

### Risks
- Feeder slots are ItemCollection slots: `put_items` (auto slot) may place nothing or need the slot id; verify with the console `put` on a feeder.
- Animals only eat and produce near a player (area gate + Unhappy pause).
- Exact refill items/amounts need the template data (runtime `get_template<InteractionLootRefill>` or an EML export).

---

## 3. Trading

### Facts
- **No vendor/shop/price system in this build.** No `Trade/Vendor/Shop` types or HUD screen (`HudScreenType` has `NpcCrafting`, `NpcDialog`, `NpcSummonerMenu`, `FeedingStation`, ... but no shop). [types]
- **`Commerce*` is the platform store**, not in-game trade: `CommerceProduct {locaTag, shopIcon, platformIdSteam, platformIdPs5, platformIdNx2, platformIdXbs}`, `ItemCommerceLink`, `CommerceRedeemLink`, `CommerceInventory` (DLC chests), HUD `RedeemCommerceProducts`, `CommerceProductChest`. [types]
- **NPC traders are NPC workshops with barter recipes.** `WorkshopRegistryResource` lists 11 `isNpc` workshops; two are traders: `NPC_Townsfolk_Male_Villager_08_fisher_Trader` → workshop **`0xc4acc104`** (template `NPC_Workshop_Fisher_Trader`) and `NPC_Townsfolk_Female_Villager_05_name_chineseNewYear_Trader` → **`0x8554479a`** (template `NPC_Workshop_ChineseNewYear_Trader`, an event NPC). Both templates have `Crafting`, `CraftingInteraction`, `Inventory`, `NpcDialog`. Quest sources `Fisher`, `ChineseNewYearTrader`. [export/samples, tpl, types]
- Offers and prices are therefore `RecipeInfo` inputs/outputs of those workshops (`RecipeRegistry+0xF8` per workshopId) [docs]; the only "currency"-like items are tokens such as `Material_T1_Token_WeaponUpgrade_Rune01` (ItemCategory `Currency` exists; contents unverified).
- `Crafting` (20 B: workshop ref, `+0x10 workshopId`) is **is_dynamic / do_not_save**, so each entity has its own copy. [types]
- Recipe gates that can block NPC-station crafts: `comfortLevel` (`npc_recipe_comfort_requirements` writes `CraftingInteraction.nextRecipeComfortRequirement` from the station NPC's comfort), `requiredHappyNpcCount`, `requiredProps`. [docs, types]

### Candidate hooks for a mod
- **Villager buys from a trader**: `CraftRecipeEvent {operator = village chest, station = the trader NPC entity, recipeId = a trader recipe}`: the same mechanism as `villager_jobs`. To let any villager act as a market stall, write its dynamic `Crafting.workshopId` to a trader workshop (re-apply after load).
- **Village-to-village trade**: a mod-defined exchange (rates in the mod's own config): `take_items(chest A, x)` + `put_items(chest B, x)` and the reverse, both through the game's transaction (all-or-nothing per call; check results in the callbacks).

### Risks
- Trader recipes unknown until dumped; the Chinese-New-Year trader may be unavailable outside the event.
- Changing `Crafting.workshopId` needs a component write; replication to clients (not server_only) and the crafting UI may misbehave.

---

## 4. Villages and bases

### Facts
- **Base** = a flame altar `Home_Base_FlameAltar*` with `BaseIdComponent {baseId GUID, buildZoneSize, endOfTimeout, wasAbandoned}` (saved); `base_id_generator` (init) assigns the GUID; `add_buildzones_server` adds its build zone each tick; `PlayerBasesSystemServer` (0x2070 B) holds **16 base records** [docs]; `FlameAltarAction` Abandon/Reclaim/RemoveWater/SetAsPreferredSpawnAltar; `BalancingTable.altarsPerFlameLevel`, `buildzoneSizesPerAltarLevel`. [types, docs]
- **Placed NPCs**: `keen::npc::PlacedNpcInfo {baseIndex, entityId, happiness}` (12 B) is very likely the value type of the `PlayerBasesSystemServer+0x1560` map (u32 key → 12-byte value, read by `npc_bed`, `player_assign_npc`) [types; the match is a guess from the size and the readers]. NPCs are rescued, then placed with the summoning staff / `NpcSummonerMenu` (`hintNpcNotPlaced`, `placeNpcInWorld`, `mapNpcUnplacedHint`). [str]
- **Uniqueness and count**: one entity per villager template (`unique_npc_spawns` destroys duplicates, keyed by the template GUID) [docs]. `NpcCollection` has 35 entries (incl. 6 `test_Townsfolk_*` and one deprecated); 58 templates have `ActiveNpcState` (incl. `Base_NPC*` and `*_8k_Version2..5` variants). **Every villager template carries `ForceSave`** (why spawned villagers are saved). No per-base NPC cap was found in this pass (not searched exhaustively); the practical limits are the unique set, beds and flame level (`requiredHappyNpcCount` on upgrade recipes). [tpl, docs]
- **Happiness** = `ActiveNpcStateSaved.happiness (+0xe)`, `npc::HappinessRequirement` Sheltered=bit0, Bed=bit1, Food=bit2. In the worker `0x140455640`: Food bit **always set**; Bed bit = `NpcCommon.bedId != 0` (after `bedInitialized`); Sheltered bit = `DynamicShelter+0xd` when valid. [asm, types]
- **Beds**: `NpcBed.associatedNpc` (NpcId, saved). `player_assign_npc` handles the player's `actor::AssignNpc {npcId}` sequence event. Program **`npc_bed`** `0x140457700` (every 32 ticks): looks up the NPC entity by NpcId (`0x140469c30` on PlayerBasesSystemServer); if the bed is farther than `ActiveNpcState.searchTargetSetup.bedRadius` from the NPC's `spawnPosition` → clears the link (and the NPC's `bedId`); if the NPC has no bed → queues `NpcCommon.bedId = this bed`; if the NPC has another bed → clears this bed's link. [asm]
- **Home**: `ActiveNpcStateSaved.spawnPosition` (+0, saved) and `overrideRoamingRange/roamingRange` (+0xc/+0x10, the player's "roaming range" input `NpcInputAction {targetNpc, roamingRange}` via `npc_active_input`). Target search (beds, seats, stations) is centred on spawnPosition, not on a base. `NpcCommandEvent` **Reset** sets spawnPosition = target and teleports [docs].
- **Away from players**: worker pauses when `closestPlayerDistance ≥ 150 m` [docs]; `growth`, `animal_food_consumption` and `actor_is_ready_for_gameplay` use the terrain-streamed + active-cell gate (`0x1403ce7f0`) [asm, docs].

### Candidate hooks for a mod
- **NPC-run village** = a second player base (so built props are saved and the build zone exists) that the mod tags as a village (sidecar save, `on_save`/`on_load`): chests, beds, farm soil, feeders, animals, stations.
- **Move villagers**: `NpcCommandEvent` Reset to the village (new spawnPosition + teleport), then `NpcBed.associatedNpc = npcId` on a free bed within `bedRadius`; optionally set `overrideRoamingRange = 1`, `roamingRange` to keep them inside.
- **More villagers**: spawn unique templates that are not in the world (the `test_Townsfolk_*` set works [docs]); truly new ones need new template GUIDs (EML data patch).
- **Village status**: `PlacedNpcInfo.baseIndex/happiness`, `ClientBaseIdComponent` counters, `findBaseAtPosition` [docs].

### Risks
- A mod-spawned altar (village without a player base) is untested: altar limits, base ownership, base record registration and saving of spawned props.
- Remote villages are frozen in vanilla; an abstract simulation must not touch entities that may not be streamed.
- Writing `NpcBed`/`ActiveNpcStateSaved` needs change tracking (`markWritten`) to be safe; EMF's `program_write_other` (API v16) writes with it.

---

## 5. Other hooks for "villagers run a village"
- **Eating slot**: `WantsToEat` (6) → FindTarget filter `isFood` → GoToTarget → `Eating` (12, no handler) is fully wired [docs]. Combined with the **hard-wired Food happiness** (`or byte [rsi+0xe], 4` at `0x140455935`, bytes `80 4E 0E 04`), a mod can add a real meal loop: clear bit 2 when hungry (patch or post-worker write), set it when the villager "ate" (took a food item from a village chest with `take_items`). Risk: happy-NPC counts gate flame upgrades.
- **`ActiveNpcTarget.isFarmland`** (+0) is never read by the AI [docs]; templates with farmland targets are unknown (read at runtime with `get_template<ActiveNpcTarget>`).
- **Actor state flags** (`actor::StateFlag`, `Actor+0xbf8`): 22 `Unhappy`, 56 `IsWorking`, 57 `Petting`, 9 `Sitting`, 11 `Sleeping` [types]. No writer of `IsWorking` found by a byte scan.
- **Interaction animations**: `NpcActionSequences.interactionSequences` and `ActiveNpcTarget.interactionOffsets/groups`; the Interacting state (10) plays `NpcCommon.interactionTarget.interactionSequence` [docs, types].
- **Base storage stock**: crafting in stock mode consumes from all base storage chests (`0x1404682e0`) [docs]; `area_deposit_trigger` (`AreaDepositEvent`, "deposit to nearby storage") needs `PlayerState`.
- **EMF API for this**: a component write with change tracking (Level, NpcBed, Crafting, ActiveNpcStateSaved) is `program_write_other` (API v16). Spawn with an orientation is missing (`spawn_ex` forbids `CurrentTransform`).

## Not read in detail (1273043)
- `immediate_loot_action` `0x1403fd090` helpers: `0x1403f45e0` (host inventory), `0x14040e160` (loot move), `0x1403f5760` (DefaultInventory roll): confirm a chest guest.
- `upgrade` `0x14027e6b0` (spawn path, change filter of flag 0x10): for the re-arm trick.
- `interaction_loot_refill` `0x1402a47f0` + `0x140e7c120`; animal needs `0x1402294e0`; feeder consumption `0x1402259f0`.
- `inventory_setup` `0x1403fff90` and `addItems` on ItemCollection slots (feeders).
- `player_building_place_prop` `0x1402d4fd0` / `server_cursor_no_prediction` `0x140292d70` (planting path).
- `handle_crafting_events` `0x140274be0`: where the station workshopId is read (dynamic `Crafting` vs template).
- `npc_bed` `0x140457700` helper `0x140469c30`; `unique_npc_spawns` `0x14045b470`.
- The world-save entity filter (why a spawned chest is not saved): ECS snapshot (1269579 `0x140985470`).

## In-game checks
1. `LootInteractionEvent` bypass harvest of a ripe tomato into a spawned chest: works (PickupEvent, items, stage and root
   gone; see "Runtime results").
2. The same with a farm animal: eggs in its inventory were moved into a chest. Its collect prompt was not checked.
3. `put` feed into a feeder: works. The animal's `needs` bit 2 was not checked.
4. A spawned plant root after a reload: gone (spawned entities are not saved).
5. Trader recipes: `job.recipes` for the Fisher trader listed 0 recipes with tomatoes and eggs. Not done: the recipe
   dumps of workshops `0xc4acc104`, `0x8554479a`, `0xd9a35a6f`, and a trader recipe crafted with a chest operator.
6. Not checked: moving a villager with Reset to a second base and writing `NpcBed.associatedNpc` (`NpcCommon.bedId`,
   happiness).
7. Not checked: entity reachability (reading a chest 1-2 km away, a loot event on a far plant).

---

## Runtime results (build 1273043, a local test world)

Confirmed in game with a test mod. **[runtime]** below = seen in game.

- **Spawned roots and the soil rule [runtime]:** a spawned `T1_Plant_Growth_Tomato` (`33ae9de5-…`) on stone floor
  destroys itself at its first growth pass (soil not in `allowedSoil`). Emptying the template's soil list
  (`Growth+0x20` count = 0, template data, until restart) keeps it. Tomato `Growth.minDuration` = 1200 s per level,
  `randomDurationScale {1.0, 1.2}`, `hasSubmergedRequirement 1` with `{0, 0.5}`. Writing `DynamicGrowth.nextLevelTime
  = 1` makes the next growth pass (every ~4.3 s per root) raise the level by one: 3 levels, stage entities change each
  time (`DynamicUpgrade.entityId`).
- **Harvest into a chest [runtime]:** `LootInteractionEvent {guest = chest 6829, host = ripe stage 6851, offerId 0,
  lootAll 1, bypassInteraction 1}` from a mod program → `PickupEvent: item 3dc85885 x2 by 6829 from 6851` in the same
  step; the root (6824) was destroyed within a second (vanilla: harvest kills the plant).
- **Re-arm [runtime]:** a direct write of the root's `Level.dataStorage[0]` from 3 to 1 (no change tracking) makes the
  game replace the stage with a new seedling (6847 → 6867). Done in the step the PickupEvent arrives (here: the
  program records the pickup, the mod's on_tick right after the step writes Level), the harvested root **survives
  and grows again** (6854: harvested, re-armed, level 2 with a new stage 24 s later). Writing it ~1 s later (a
  separate console command) was too late: the root was already gone. The old `nextLevelTime` was in the past, so the
  plant skipped its first level at once: also write `nextLevelTime = 0` (the growth pass then restarts the timer).
- **Spawned entities are not saved [runtime]:** after a quit and reload the spawned roots (one spawned with an extra
  `ForceSave` component through `spawn_ex`), the spawned chest, feeder and turkey were all gone. ForceSave does not
  make a spawned entity part of the save; replanting by spawning roots is therefore not durable.
- **Feeder [runtime]:** a spawned `Animal_Feeding_Feeder_Farming_Turkey` vanished within a second where it had no
  surface (it has `AttachToSurface`); spawned next to a chest it stayed, and `put_items(feeder,
  Material_T3_Turkey_Feeding, 10)` added 10 (its ItemCollection slots accept the food through the normal transaction).
  Its components: AnimalTarget (template), AttachToSurface, Inventory, InventorySetup, InventorySetupOverwrite,
  ItemCollectionComponent, InteractionLock, Destructible, OnDestroy, ...
- **Farm animal [runtime]:** the spawned tame turkey (`PLACEMENT_HELPER_T3_Animal_Farm_Turkey`) has faction 12
  (FarmAnimal), `AnimalSaved.needs = 03` (no shelter, no bed) and its loot refill timer (600 s) stays frozen while it
  is unhappy. Eggs put into its inventory were moved into a chest by the bypass loot event (`PickupEvent: item
  7106e3cc x2 by 6829 from 6887`).
- **Trader [runtime]:** `NPC_Workshop_Fisher_Trader` (`e7666cd3-…`) spawns as a villager; `job.recipes` with a chest of
  tomatoes and eggs listed 0 recipes (its barter inputs are other items; they were not listed in full).
- Animals near the test spot: 6 wild ones at ~110 m (faction 5).
- **Full chest [runtime]:** a farming villager's harvest into a chest with all 16 slots taken did nothing:
  no PickupEvent, the job noted "could not put the harvest into chest 6828 (is it full?)" once, and the plant stayed
  alive (level 3/3, same stage entity) through the 60 s pause. After one slot was freed the next try harvested
  2 tomatoes and re-armed the plant.
