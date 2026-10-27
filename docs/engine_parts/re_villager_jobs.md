# Villager jobs: crafting from a chest, moving villagers, what the player looks at

Build 1273043, a local test world. Confirmed in game with a test mod that sent events from its own server program (API v3) and read data in console commands.

## Real crafting with a chest as the operator: works
- `CraftRecipeEvent` (56 B: `craftingOperatorId +8`, `craftingStationId +0xc`, `recipeId +0x10`, `recipeAmount +0x14`, input category selections) sent with **operator = a storage chest** (`Prop_Decoration_T0_Storage_16`, spawned with `spawn`, `InventorySetup` initialized) and **station = a villager** (entity 1025) crafted for real: the game's `handle_crafting_events` took the inputs from the chest, put the output into it (`PickupEvent` pickedUpBy = the chest) and emitted `CraftRecipeSuccessEvent {workshopId, recipeId}`.
- Sent again with the chest empty: no success event, no item (the game checks the materials itself).
- Recipe used: `0xf94576e0` = 1x `Weapon_T0_1H_Mace_Club_Craft` <- 6x `Material_T1_Wood_Logs`, 1x `Build_TerrainMaterial_T1_Stone` (props 0, knowledge 0).

## Recipes of a station
- `Crafting` (20 B) on stations and villagers: `workshop` GUID, `+0x10 workshopId`.
- `g38_items::RecipeRegistry` + `0xF8`: Keen map (plain keys, linear probing) workshopId -> `SmallDynamicArray<RecipeInfo*, 512>` (0x1008-byte values, u16 count at `+0x1000`).
- `RecipeInfo` (inventory_crafting.md table): `+0x10 recipeId`, `+0x28` inputs `BlobArray<RecipeInputResource 48 B>` and `+0x30` outputs `BlobArray<RecipeItemStackResource 24 B>` (both: self-relative i32 offset, u32 count; item id at `+0x10`, count at `+0x14` of each element; a category input has item id 0), `+0x38 craftingDuration` (ns), `+0x48 requiredPropsMask`, `+0x50 knowledgeRequirement`, `+0x84 comfortLevel`.
- Both villagers of the test world (early survivors, `npcType` 1) have **workshop 0**: the 57 hand-crafting recipes the player has too (the player entity also has `Crafting` workshop 0). Villagers with a trade in a base have their own workshop (not in this world).

## Moving a villager: `NpcCommandEvent`
- `NpcCommandEvent` (72 B: `type +8` (0 Reset, 1 GoToPosition), `npcId +0xc` = the villager's `ActiveNpcState.npcId` (template data, `+0`), `targetTransform +0x10` = a `WorldTransform`), sent from a mod program: villager 1025 walked from 7 m to 3.6 m of the chest (state 7), then roamed again (state 3) a few seconds later, as with the sample mod's F7 key. A job has to re-send it to keep the villager near.

## What the player is looking at
- `PlayerFocus.position` is the player's own position (not the crosshair).
- `PlayerInput.fromClient.cursorInput.primaryTransform` is all zero outside building mode.
- `ClientInteractionQuery.nearestOfferedInteractionOfferId` (client world) changed from 0 to 1 when the chest's "E Open" prompt showed, but no entity's `InteractionOfferRuntime.offerId` was 1: it is not that id (probably an index; not mapped).
- **`PlayerInput.fromClient.camera`** (`ClientCamera` 64 B: `position` WorldPosition, `orientation` quaternion x,y,z,w, `distance`, `fovY`, ...) is live on the server world and orbits the player (1.6-3.1 m) as the view turns. **Its forward vector is the orientation applied to +z**: q ≈ (0.092, -0.701, 0.092, 0.701) looked toward -x at the player from x+2.6 m; q ≈ (0.131, 0.004, -0.001, 0.991) looked toward +z and slightly down. A ray from the camera along that vector picks the entity under the crosshair.

## The `villager_jobs` mod
Built on these findings and confirmed in game (see ENGINE_MAP "Confirmed in game: villager jobs"): crafting every 10 s from a chest, the "waiting" note when materials ran out and back to work after a refill, a villager walking 18 m to its chest, the stop wheel, and saved jobs loaded again after a reload (then dropped, because a spawned chest is not saved).

## Other notes
- A chest spawned with `spawn` is not saved with the world (gone after a reload); chests the player builds are.
