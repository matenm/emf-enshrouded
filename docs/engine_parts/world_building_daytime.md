# World: day/night, weather, building, voxels, bases (Tier 2.5 / 2.6)

Build 1269579, image base 0x140000000. Static analysis only (Ghidra via rea + local scripts).
Signatures come from `sig.py`/`sig2.py` (rel32 and RIP displacements wildcarded, trailing wildcards trimmed). They start at the function entry and were checked to be unique in `.text`.
Struct offsets come from `types.json` unless the text says "seen in code". `keen::Time` is a signed 64-bit count of **nanoseconds**. `WorldPosition` is 3 x i64 fixed point 32.32 (`float = v * 2^-32`).

Summary:
- **Day/night** is one small object (0x70 B), the `g38_daytime::Daytime` singleton. It stores an *anchor* (`ecsTime`, `gameTime`, `timeScale`) and derives everything else. The time of day is `Daytime+0x48` (ns, 0..24h), updated every tick by `Daytime_update` `0x140ea7c00` (called by `server_update_daytime` / `client_update_daytime`). **Set the time:** `Daytime_setTimeOfDay(daytime, todNs, ecsNow)` = `0x140e98630` (server). **Read:** `+0x48` or `Daytime_isDaytime` `0x140e89080`. Day and night length come from `ecs::GameSettings.dayTimeDuration/nightTimeDuration` (+0x78/+0x80). Clients get the anchor through `ServerInfoMessage.daytimeSync` → `Daytime_applySync` `0x140e86280`.
- **Weather** is *local*, not global. `WeatherSystem` (0x150 B) has N layers. Each layer runs one "phenomenon" instance (a moving rain/snow/fog cloud with position, radius, speed, spawn/despawn time). The server picks the next phenomenon per layer by weighted random when `layer.nextPhenomenonTime` is reached (`WeatherSystem_updateLayers` `0x140a9d730` → `spawnPhenomenon` `0x140a9c700`). The weather clock is the Daytime *game time*, not wall time. Weather is not an ECS program; the GameServer tick calls it directly (`0x140a9dd90`). Clients get instances through `WeatherSyncMessage` (network channel 0x12). Rain at a point is read with `WeatherSystem_sample` `0x140a9b9e0`.
- **Building** runs on the server inside 4 ECS programs (`player_building_blueprints`, `..._place_prop`, `..._dismantle`, `..._undo`). They are triggered by **action-sequence events** of the player (`actor::PlaceVoxelObjectEvent`, `actor::ChangeVoxel`, `actor::Dismantle`), not by raw input. Each builds a `BuildingOp` context on the stack and calls the g38_building core: **place block `0x140486ef0`**, **remove block `0x140487770`**, **place prop `0x1404866b0`**, then **finish `0x14048baa0`** (commits the inventory transaction, emits `UiBuildingEvent`). The core writes voxels through VoxelWorld edit functions (`0x140a837b0` place blueprint, `0x140a8c220` remove blueprint, plus terrain add/remove/flatten/decay/overgrowth).
- **Terraforming** (pickaxe/shovel/terrain tools) runs through the impact system: impact function `terraforming` (`0x1403e5b70`, hash = nameHash of `impact_nodes::Terraforming` `0x9aabd181`), called from `impact_simulation`. It switches on `TerraformingType` and calls the same g38_building core.
- **Bases:** `PlayerBasesSystemServer` (0x2070 B) holds up to 16 base records (stride 0x130, AABB + sub-box mask). Lookups: `findBaseAtPosition` `0x140468ce0`, `isInsideBase` `0x140469f70`. Per-frame counts (beds, comfort, crafting props) are cleared and rebuilt each tick by small programs. **BuildZoneSystem** is also rebuilt each tick (`clear_buildzones` then `add_buildzones_server` from every entity with `BaseIdComponent`, plus `NoBuildZone` entities).
- All world singletons above are created and registered in `ServerGame_create` `0x1404949b0` with `registerSingleton(world, name, ptr, opts)` `0x1409d50d0`. **Hooking `0x1409d50d0` and matching the name is the easiest way for a mod to get `Daytime*`, `WeatherSystem*`, `VoxelWorld*`, `BuildZoneSystem*`, `PlayerBasesSystemServer*`.** The same function also registers the server **impact function table** (see appendix A).

---

## 1. Day / night (`g38_daytime::Daytime`)

### 1.1 Object layout (seen in code, `Daytime_create` and the functions below). Size 0x70.
| off | type | meaning |
|---|---|---|
| +0x00 | ptr | resource system |
| +0x08 | u32 | handle of the "Ingame time config" resource |
| +0x10 | Time | **anchor ecsTime** (ECS world time when the anchor was set) = `DaytimeSync.ecsTime` |
| +0x18 | Time | **anchor gameTime** (cycle time at the anchor) = `DaytimeSync.gameTime` |
| +0x20 | f32 | **timeScale** (1.0; night skip raises it) = `DaytimeSync.gameTimeScale` |
| +0x24 | u32 | change counter, +1 on every re-anchor (setTimeOfDay, setTimeScale, setDurations) |
| +0x28 | Time | day start, time of day (default 6h = 21600e9). Copied from the config resource |
| +0x30 | Time | night start, time of day (default 18h) |
| +0x38 | Time | real duration of the day phase (default 30 min) = `GameSettings.dayTimeDuration` |
| +0x40 | Time | real duration of the night phase (default 12 min) = `GameSettings.nightTimeDuration` |
| +0x48 | Time | **current time of day**, 0..86400e9 ns (written by update) |
| +0x50 | Time | ecsTime of the last day/night flip |
| +0x58 | u8 | **isDay** (dayStart <= tod < nightStart) |
| +0x59 | u8 | authoritative flag (1 on the server world, 0 on client worlds). Only then do duration changes re-anchor |
| +0x5c, +0x64 | 2 x 8 B | two curve values evaluated from the config resource (light/sky blend; not traced) |

Math (seen in code):
- `gameTime(now) = anchorGameTime + (now - anchorEcsTime) * timeScale` (`Daytime_getGameTime` `0x140e76ce0`).
- `tod = map(gameTime mod (dayDur + nightDur))`. Cycle time 0 = midnight. The night phase runs from nightStart to dayStart (wraps past 24h) at speed `nightTod/nightDur`, the day phase at `dayTod/dayDur` (`Daytime_gameTimeToTimeOfDay` `0x140e71640`). The inverse is `Daytime_timeOfDayToGameTime` `0x140e667f0`. It returns a value inside the first cycle, so setting the time resets the game time to day 0.
- `Daytime_getDayProgress` `0x140e7de50`: 0..0.5 over the day phase, 0.5..1 over the night phase.

### 1.2 Functions
- **Daytime_create(alloc, resSys, configRef*, bool authoritative)** = `0x140e64170`. Allocates 0x70 B and sets the defaults above. Logs `[g38_daytime] Failed to create Ingame Time` / `Invalid Ingame time config Resource`. The server calls it with 1 (`ServerGame_create` `0x1404949b0`), the client worlds with 0 (`0x140840bf0`, `0x140841370`). The server then calls `setTimeOfDay(daytime, 7h, 0)`, so a new world starts at 07:00. Confidence: High.
  Sig `48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 70 48 8B 01 41 0F B6`
- **Daytime_update(daytime, ecsNow)** = `0x140ea7c00`. Reloads day/night start from the config resource if it changed. Computes `+0x48` tod and `+0x58` isDay, updates `+0x50`, evaluates the two curves. Logs `[g38_daytime] Ingame time not created - time stands still` if null. Confidence: High.
  Sig `40 53 57 48 81 EC B8 00 00 00 48 8B`
- **Daytime_setDurations(daytime, dayDur, nightDur, ecsNow)** = `0x140ea7f60`. If they changed: keep the current tod, store the new durations, and (authoritative only) re-anchor and bump `+0x24`. Called every tick by the update programs with `GameSettings.dayTimeDuration/nightTimeDuration`. Confidence: High.
  Sig `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 49 8B F9 49 8B D8`
- **Daytime_setTimeOfDay(daytime, Time tod, Time ecsNow)** = `0x140e98630`. `+0x10 = ecsNow; +0x18 = timeOfDayToGameTime(tod); +0x24++`. Callers: `admin_player` (admin "set time"), `ServerGame_create` (07:00), savegame load `0x140e4a4f0` (restores the saved tod from "Miscellaneous Data"). Confidence: High.
  Sig `40 53 48 83 EC 20 48 8B D9 4C 89 41`
- **Daytime_setTimeScale(daytime, float scale, Time ecsNow)** = `0x140e99ab0`. Re-anchors and sets `+0x20`. Only caller: `server_nighttime_skip`. Confidence: High.
  Sig `48 89 5C 24 08 57 48 83 EC 30 0F 29 74 24 20 49`
- **Daytime_getGameTime(daytime, ecsNow) → Time** = `0x140e76ce0`. Confidence: High. Sig `4C 8B 51 10 49 B8 FF FF FF FF FF FF`
- **Daytime_gameTimeToTimeOfDay(daytime, gameTime) → Time** = `0x140e71640`. Sig `48 89 5C 24 18 55 48 83 EC 10 4C 8B`
- **Daytime_timeOfDayToGameTime(daytime, tod) → Time** = `0x140e667f0`. Sig `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 10 4C 8B 51 30 48`
- **Daytime_isDaytime(daytime) → bool** = `0x140e89080` (reads +0x48 against +0x28/+0x30). Sig `48 8B 41 48 48 3B 41 28 7C 09 48 3B 41 30 7D 03 B0 01 C3 32` (tiny function; the pattern runs into padding, so reading the offsets directly is safer).
- **Daytime_getTimeScale** = `0x140e85700` (`return +0x20`). **Daytime_getDayProgress** = `0x140e7de50` (sig `48 83 EC 08 4C 8B 51 28 4C 8B D9 48`).
- **Daytime_applySync(daytime, DaytimeSync*)** = `0x140e86280`. Copies `{ecsTime, gameTime, gameTimeScale}` to +0x10/+0x18/+0x20. The client calls it when a `ServerInfoMessage` arrives (network channel 1 in the GameClient receive loop `0x1408695a0`). `ServerInfoMessage` (0x108 B, network reflection at `0x141fb9980`) has `daytimeSync` at +0x08; the same message also carries the `GameSettings` block. Confidence: High (client side). Where the server builds this message was not traced.
  Sig `48 8B 02 48 89 41 10 48 8B 42 08 48`
- **GameSettings getters:** `0x140e75430` (`reg->+0x90->+0x78` dayTimeDuration) and `0x140e7dbd0` (`+0x80` nightTimeDuration), called on `BalancingRegistry`. So `BalancingRegistry+0x90` points to the live `ecs::GameSettings` (144 B; `+0x2e weatherFrequency`, `+0x78 dayTimeDuration`, `+0x80 nightTimeDuration`). Confidence: High.

### 1.3 ECS programs
| program | update fn | role |
|---|---|---|
| `server_update_daytime` | `0x1403457a0` | `setDurations(GameSettings)` then `update`. Flags 0x14 (server) |
| `client_update_daytime` | `0x140302610` | same body on the client world (flag 0x5) |
| `server_nighttime_skip` | `0x140343e10` | At night, counts players (16 slots, stride 0xA0) whose `PlayerState` bit 11 is set (very likely "sleeping"). Target scale = `60 * sleeping / total` (1.0 if nobody sleeps or it is day). Calls `setTimeScale` when the difference is large enough. Sig `48 8B C4 41 57 48 81 EC 80 00 00 00 0F 29 70 D8` |
| `admin_player` | `0x1402cae30` | When `PlayerInput.adminData.adminSetDaytime.versionData` (input +0x488) differs from `ServerConsumedPlayerInput.consumedAdminSetDaytime` (+0xb4), calls `setTimeOfDay(daytime, syncDaytime (+0x478), syncDaytimeTick (+0x480))`. `AdminClientPlayerInputData` sits at `PlayerInput+0x3c0` |
| readers | | `npc_active_behavior`, `animal_behavior`, `enemy01`, `enemy_combat_targets`, `daytime_temp`, `conditional_despawn`, `random_spawner`, `extinguish`, `fuel_setup`, `actor_wet_soaked_apply`, `interaction_attempt` (InteractionDaytimeLock), `impact_simulation`, `server_player_fishing`, `player_daytime_ui`, `player_daytime_animation_input_2`, `ambient_effects`, `footdown_effects` |

Note: the two update programs have identical bodies (only call targets differ), so byte signatures are not unique. Find them through their program descriptors (name string → descriptor `+0x10`), see `docs/ecs_programs.md`.

### 1.4 `IsDaytime` impact node
`impact_nodes::IsDaytime` (12 B, no fields) is an impact *function*, registered in `ServerGame_create` as `registerImpactFunction(impactSys, 0x52de54cc, 0x1403e0900, "isDaytime", 0)`. `0x52de54cc` is the type's nameHash. The callback is `result.bool = Daytime_isDaytime(ctx->+0x30->+0xb8)`, so the impact context holds `Daytime*` at `[ctx+0x30]+0xb8`. Confidence: High. Sig `40 53 48 83 EC 20 48 8B D9 49 8B 48`

### 1.5 What a mod can do
- **Read:** `tod = *(i64*)(daytime+0x48)`; hours = `tod / 3.6e12`. `isDay = *(u8*)(daytime+0x58)`.
- **Set time (server/host only):** call `0x140e98630(daytime, hours*3600e9, ecsNow)`. `ecsNow` = `ecsWorld_getTime(world)` = `0x1409b0e10` (reads `world+0xe16280 * world+0xe1629c`), or reuse what the update program passes. The change counter goes up; clients then receive the new anchor. Side effect: the game time restarts at day 0, and the weather clock uses the game time (see 2.4).
- **Freeze / speed up:** call `setTimeScale(daytime, 0.0 or N, ecsNow)`. Note that `server_nighttime_skip` overwrites the scale when its target differs. Hook it or set the scale back every tick.
- **Day length:** write `GameSettings.dayTimeDuration/nightTimeDuration`. The next `server_update_daytime` applies them and keeps the current tod.
- On a client-only install changes are cosmetic; the next `ServerInfoMessage` overwrites the anchor.

---

## 2. Weather (`WeatherSystem`)

### 2.1 Model
- Data: `WeatherSystemResource` (layers[] 200 B each, phenomenons[] 176 B each, masks, state configurations). Each layer owns a range of phenomena (`WeatherSystemLayerResource.phenomenonIndexRange` at +0x12 first, +0x13 count) plus a built-in "nothing" phenomenon (+0x18). A phenomenon has `probability` +0x28, `flags` +0x2c, `durationMin/Max` +0x30/+0x38, `radiusMin/Max` +0x48, `moveSpeedMin/Max` +0x50, `spawnFadeTime/despawnFadeTime` +0x58/+0x60, `rainThreshold` +0x78, `effect` +0xa8 (`WeatherPhenomenonEffect`: 0 None, 1 Rain, 2 Blizzard).
- State: per layer one current and one previous (fading out) **instance**. Saved form: `weather::save_v0::WeatherSavedLayer {nextPhenomenon Time; currentInstance; previousInstance}`, instance = `{phenomenonIndex, spawnPosition WorldPosition, rotation, radius, moveSpeed, spawnTime, despawnTime}`.
- `keen::WeatherState` (Clear/Rain/Snow/Blizzard) is only used by ambient sound/VFX/material tables, not by the simulation.
- `GameSettings.weatherFrequency` (Disabled/Rare/Normal/Often) maps to a probability multiplier 0 / 0.5 / 1 / 2 (`0x140e86090`). It only multiplies phenomena whose `effect != None`. "Disabled" therefore removes rain and blizzards but keeps clouds/fog.

### 2.2 Object layout (seen in code; `WeatherSystem_create` `0x140a99330`, 0x150 B)
| off | meaning |
|---|---|
| +0x00 | resource system |
| +0x08 | u32 mode: 0 = server (simulates), 1 = client (renders) |
| +0x0c | u32 resource handle (`WeatherSystemResource`) |
| +0x10 | u32 resource version (a reload clears all instances) |
| +0x18.. | instance pool (slot map; entry 0x50 B: `+0` id, `+8` 0x48-byte payload: `+0` layer idx u8, `+1` phenomenon idx u8, `+8/+0x10/+0x18` position x/y/z fixed 32.32, `+0x20` rotation, `+0x24` radius, `+0x28` moveSpeed, `+0x30` spawnTime, `+0x38` despawnTime) |
| +0x88 / +0x90 | array of live instance handles (ptr, count) |
| +0xb0 | u8 "apply new-world state configuration" pending |
| +0xb4 / +0xb8 | RNG state (two 16-bit multiply-with-carry generators, multipliers 18000 and 0x9069) |
| +0xc8 / +0xd0 | **layers** (ptr, count), stride 0x18: `+0x00 nextPhenomenonTime`, `+0x08 currentPhenomenonIndex u64`, `+0x10 currentInstanceHandle u32`, `+0x14 previousInstanceHandle u32` |
| +0xf0.. | client: map from server instance id to local instance |
| +0x148 | client: render weather system (render instances are pushed with `0x1400f1b50`/`0x1400f53f0`) |

### 2.3 Functions
- **WeatherSystem_create(alloc, params*)** = `0x140a99330`. Callers: session setup `0x140e3a350` (server, mode 0) and client setup `0x140841c40` (mode 1). Confidence: High. Sig `48 8B C4 48 89 48 08 56 41 54 41 57`
- **WeatherSystem_update(ws, {Time t, bool valid}*, spawnParams*)** = `0x140a9db50`. Gets the resource. On a version change it removes all instances. Server: applies the pending new-world state configuration (`0x140a9ce20`), then `updateLayers`. Client: pushes every instance to the renderer (`0x140a9ddb0`). Confidence: High. Sig `48 8B C4 4C 89 40 18 56 41 54 41 55`
- **Server tick wrapper** `0x140a9dd90(ws, Time gameTime, WeatherSpawnParameters*)`. Called once per server frame from the **GameServer tick `0x140e46070`** as `0x140a9dd90(gameServer->+0x1798 (WeatherSystem), ServerGame_getWeatherTime(serverGame), &{eventProbabilityModifier})`. `ServerGame_getWeatherTime` = `0x140499740` = `Daytime_getGameTime(serverGame->+0x2d0, ecsNow)`. Client wrapper: `0x140a9db30` (from `0x14086e7b0`). Sig `48 83 EC 38 48 89 54 24 20 48 8D 54 24 20 C6 44 24 28 01 E8 ?? ?? ?? ?? 48 83 C4 38 C3 CC CC CC 40 55 53 56`
- **WeatherSystem_updateLayers(ws, resource, Time now, float* probabilityModifier)** = `0x140a9d730`. **This is where weather changes are decided.** For each layer: if the previous instance's despawnTime has passed, remove it. If `now >= layer.nextPhenomenonTime`: current becomes previous, then pick a new phenomenon by weighted random over the layer's phenomena (excluding the current one; weight = probability, times the modifier if `effect != None`) plus the "nothing" entry, and call `spawnPhenomenon`. Confidence: High. Sig `48 8B C4 4C 89 48 20 48 89 48 08 53 55 57 41 57`
- **WeatherSystem_spawnPhenomenon(ws, resource, Time now, u64 layer, u8 phenomenonIdx, {Time duration; bool set}* forced)** = `0x140a9c700`. `phenomenonIdx == layer count` means "nothing". Sets `layer.nextPhenomenonTime = now + duration - despawnFade` (random duration in [min,max], or the forced one). Creates the instance (random position inside the layer area or world, random radius/speed/rotation, `spawnTime = now`, `despawnTime = next + despawnFade`). Confidence: High. Sig `4C 89 4C 24 20 53 55 56 57 41 54 41 55 41 56 48 81 EC D0 00`
- **WeatherSystem_applyStateConfiguration(ws, now, guid*)** = `0x140a9ce20`. Applies a `WeatherStateConfiguration` (per layer: type 1 = clear with duration, type 2 = force a phenomenon with optional duration). Used for the new-world state. Confidence: Medium/High. Sig `4C 8B DC 41 54 48 81 EC 90 00 00 00`
- **WeatherSystem_removeInstance(ws, handle)** = `0x140a9b560`. Sig `85 D2 0F 84 ?? ?? ?? ?? 89 54 24 10 53 57 48 83`
- **WeatherSystem_sample(out, ws, WorldPosition*, Time gameTime) → WeatherSample*** = `0x140a9b9e0`. Used by `extinguish`, `fuel_setup`, `actor_wet_soaked_apply`, `ambient_effects`, `footdown_effects`, `wildlife`/`enemy01` (via `0x140355660`). `out+0x04` is the rain/wetness factor (extinguish uses it with Shelter). Confidence: Medium (only +0x04 confirmed). Sig `48 8B C4 48 89 58 18 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 20 01 00`
- **Network:** server writers `0x140a9a290` (full state per layer) and `0x140a9e1e0` (add/remove), both from the GameServer send step `0x140e4d6e0`, message type `WeatherSyncMessage` (descriptor `0x141fc5508`, siblings `WeatherSyncAddInstanceMessage`, `WeatherSyncRemoveInstanceMessage`). Client reader `0x140a9a510` (channel 0x12 in `0x1408695a0`). Sigs `48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00 48 6B` / `48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D 6C 24 C9 48 81 EC B0 00 00 00 48 8B 41 08` / `48 85 C9 0F 84 ?? ?? ?? ?? 48 89 7C 24 20 55 48`
- **Save/load:** load `0x140a9a8a0(result, ws, blob, daytimeGameTime)` from the savegame loader `0x140e4a4f0` (logs `[weather] invalid savegame version`, `[weather] weather system resource changed, ignoring saved state`). The save side is probably `0x140a98a20`/`0x140a9bfc0` (callers in `0x140e4d9d0`); not verified. Sig (load) `48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D 6C 24 C9 48 81 EC 90 00 00 00`
- **WeatherFrequency → modifier** `0x140e86090(balancingRegistry)`. Sig `48 83 EC 28 48 8B 81 90 00 00 00 0F`

### 2.4 How a mod reads or forces weather
- **Read:** iterate `ws+0xc8` layers; for each `currentInstanceHandle` look up the instance payload (phenomenon index, position, radius, times). The phenomenon's `effect` tells rain/blizzard. For "is it raining here", call `WeatherSystem_sample` like `extinguish` does.
- **Skip to the next weather (all layers):** set `layer.nextPhenomenonTime = 0` for each layer on the server WeatherSystem. The next server tick re-rolls (this is what the debug message `WeatherSystemDebugSkipToNextPhenomenonMessage{layerIndex}` implies). Clients follow via the sync messages.
- **Force a phenomenon:** hook `spawnPhenomenon` `0x140a9c700` and replace `phenomenonIdx` (and/or the forced duration), then trigger a re-roll as above. Or hook `updateLayers` `0x140a9d730` and change `*probabilityModifier` (0 = no rain).
- Changing the day time also moves the weather clock. A big jump backwards (setTimeOfDay resets game time to the first day) can leave `nextPhenomenonTime` far in the future, so weather may freeze until then. Reset `nextPhenomenonTime` after setting the time.

### 2.5 ECS programs (all read-only users; nothing writes `WeatherSystem` through ECS)
`actor_wet_soaked_apply` `0x14047a2f0`, `extinguish` `0x14047bbe0`, `fuel_setup` `0x1403320c0`, `enemy01` `0x1403688f0`, `wildlife` `0x140480a70`, `footdown_effects` `0x140307180`, `ambient_effects` `0x1402fd800`. Related components: `Shelter`, `DynamicShelter`, `Extinguish`, `WetDurationReduction`, `FuelSetup`.

---

## 3. Building and voxels

### 3.1 Flow on the server
1. The player's action sequence emits an event at the right animation frame: `actor::PlaceVoxelObjectEvent` (hash `0xde627227`, `+0x60 subtract`, `+0x61 consume`), `actor::ChangeVoxel` (`0x92273809`, `+0x60 changeType`: 1 Add, 2 Flatten, 3 HorizontalFlatten, 4 Undo) or `actor::Dismantle` (`0xb641023a`). The building programs collect them with `0x1402139b0`.
2. The program builds a **BuildingOp** (stack struct, at least 0x158 B). Seen in code: `+0x00` ProgramContext, `+0x28` ItemRegistry, `+0x30` voxel data (`VoxelWorld[+0]`), `+0xa8` and `+0xe0` event writers, `+0xe8/+0xf0` UndoStorage + current undo entry, `+0x100` VoxelBlueprintItemRegistry (`0x140e70000(reg, itemId)` → `VoxelBlueprintItem`), `+0x108` another registry used for material checks, `+0x130` voxel editor (`VoxelWorld[+8]`), `+0x14c` player EntityId, `+0x150` isSecondaryAction, `+0x151` "replay" flag (undo/redo; skips undo recording). Confidence: Medium (offsets from several callers). It also starts an **InventoryTx** (`0x140411df0`, see inventory_crafting.md).
3. Core call (below), then `Building_finish`.

### 3.2 g38_building core
- **Building_placeBlueprint(op, Transform* t, ItemId blueprintItem)** = `0x140486ef0`. Looks up the `VoxelBlueprintItem` (`{itemId, size uint3, data}`) with `0x140e70000(op->+0x100, itemId)` and the material item (`t+0x1c`). Computes the rotated bounds, checks collisions (`0x1404869d0`), records undo data, then writes voxels with **`VoxelWorld_placeBlueprint` `0x140a837b0`(op->+0x130, &result, op->+0x30, ..., shape, material, flags)** and emits the place event (`0x140490080`). Caller: `player_building_blueprints` and undo/redo. Confidence: High. Sig `48 8B C4 55 53 56 57 41 54 41 56 41 57 48 8D A8 88 FD FF FF`
- **Building_removeBlueprint(op, Transform*, ItemId)** = `0x140487770`. Dismantle of one block: `VoxelWorld_removeBlueprint` `0x140a8c220`, then gives the loot back (`0x14048fd80`) and emits `BuildingTearDownEvent`/`BuildingPickUpEvent` (`0x1404901c0`). Confidence: High. Sig `48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 48 8D AC 24 E0 E5 FF FF`
- **Building_placeProp(op, PropDesc*, ItemId)** = `0x1404866b0`. Spawns the prop entity (`0x140485a80`), emits `LogicEntityPlacedEvent` (`0xbea1ac2b`) for logic items (ItemInfo flag 0x80000), records undo, emits the place event. Also used by `0x1402c8c50`. Confidence: High. Sig `48 89 74 24 18 48 89 7C 24 20 55 41 55 41 57 48`
- **Building_finish(op, InventoryTx* tx, ItemId material, bool success)** = `0x14048baa0`. Success: commit the inventory tx (`0x140408b50`) and the undo entry. Failure: roll back (`0x140410e60`). Always emits `UiBuildingEvent` (`0xbbaebaf7`: playerEntityId, materialItemId, isSecondaryAction, isUndoAction, isSuccessful). Confidence: High. Sig `48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 20 4C 8B F1 48 8B 89`
- **Building_consumeResources(invCtx, undoEntry, flag)** = `0x14048eb30` (8 callers). Sig `40 55 56 48 83 EC 78 48 8B F2 48 8B`
- **Building_prepareBlueprint** `0x14048a2e0` (cost/preview), **Building_undo** `0x140489cd0`, **Building_spawnProp** `0x140485a80`, place-event emit `0x140490080`, remove-event emit `0x1404901c0`. Sigs `48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 C0 FE FF FF 48` / `48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 56 41 57 48 8D 68 A8` / `48 89 5C 24 10 55 56 57 41 56 41 57 48 8D 6C 24 90 48 81 EC`
- Terraform helpers (called from the impact function, 3.4): remove/dig `0x140489850`, mining of built voxels `0x140488f70`, add/flatten `0x140486b60`, overgrowth `0x140488090`, decay `0x140487ec0`.

### 3.3 VoxelWorld edit functions (module `0x140a76e00..0x140a8f300`)
| function | address | caller(s) | sig |
|---|---|---|---|
| VoxelWorld_placeBlueprint | `0x140a837b0` | placeBlueprint, prepare | `4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 18 FF FF FF` |
| VoxelWorld_removeBlueprint | `0x140a8c220` | removeBlueprint, dismantle variants, undo table | `4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 A8 FE FF FF` |
| terrain add (a) | `0x140a843c0` | `0x140486b60` | `48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 48 89 48 08 55 41 54 41 55 41 56 41 57 48 8D A8 28` |
| terrain add/flatten (b) | `0x140a84d10` | `0x140486b60`, `0x140485e40` | `48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 56 41 57 48 8D A8 C8` |
| terrain remove (dig) | `0x140a846c0` | `0x140489850` | `48 8B C4 48 89 58 10 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 68 FE FF` |
| decay paint | `0x140a83a70` | `0x140487ec0` | `48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 60 20 55 41 56 41 57 48 8D 6C 24` |
| overgrowth paint | `0x140a83e30` | `0x140488090` | `48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D A8 F8` |
| VoxelWorld_create | `0x140a85b10` | `0x140e3a350` (server), `0x140841c40` (client); 0x78 B object. Not confirmed that this is the same pointer as the `VoxelWorld` singleton | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B 01 48 8B FA 41 B1 08 C6 44 24 20 00 BA 78 00` |

The place/remove functions build an edit region (7-level mip pyramid, `0x140a85ef0`) and run the kernels `0x140a77680` (place) / `0x140a7b200` (remove). Confidence: High for the call graph, Medium for the exact meaning of the terrain variants (a)/(b). Undo uses a revert table with entries `0x14048fb90`, `0x14048fc20`, `0x14048fe40`, `0x14048fee0`, `0x14048ffa0` (no direct callers).

Replication: voxel damage goes to clients through `ServerVoxelDamageSystem` (`gameServer+0x1a0`), writer `0x1404a18a0` (`[S][voxel_damage] Failed to write replication message`), client reader `0x1403c7cd0`. Terraformed chunks are stored compressed (`[voxeldata]` module around `0x1410942f0`). Sig `44 89 44 24 18 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D`

### 3.4 Terraforming through impacts
- `terraforming` impact function = `0x1403e5b70` (9 args, hash `0x9aabd181` = nameHash of `impact_nodes::Terraforming {playerId, transform, terraformingType, damageTerraformingSetup, materialId, rangeDefault, rangeAdd, rangeFlatten, specialAbilityGain}`). It runs inside `impact_simulation` (`0x1403ef7a0`, which writes `VoxelWorld`, `ServerVoxelDamageSystem`, `g38_building::UndoStorage`). It switches on `TerraformingType`: 0 Remove → `0x140489850` + `0x140488f70` + loot; 1 Add → `0x1403d3550`; 2/3 Flatten → `0x1403db830`; 4/5 Overgrowth → `0x1403e1900`; 6/7 Decay → `0x1403e1500`; then `Building_finish`. Confidence: High. Sig `4C 89 44 24 18 48 89 4C 24 08 55 53 56 41 55 41`
- Efficiency per tool/material: `TerraformingEfficiencyRegistry` (`TerraformingEfficiencyRegistryResource {terrainConfigs[], buildingConfigs[]}`), refreshed by `update_terraforming_queries_server` `0x1404178b0`. Log `[TerraformingEfficiencyRegistry] Finished loading.` in `0x140ead100`.

### 3.5 Build zones
- **BuildZoneSystem_addZone(bzs, u8 list, u8 flag, u8 ownerKind, WorldAABB*, EntityId, bool abandoned, BaseId*)** = `0x140e557e0`. list 0 → `bzs+0x10` (base build zones; `add_buildzones_*` pass 0,0,0), 1 → `+0x38` (no-build zones; `add_no_buildzones` passes a variable list value), 2 → `+0x60` (dynamic no-build zones around creatures: `add_no_buildzones_enemy` ownerKind 2, `_animal` 3, `_npc` 5, flag 1). Call-site arguments read from bytes. Confidence: High for the lists, Medium for the names of the flag/ownerKind arguments. Sig `48 83 EC 78 8B 84 24 A8 00 00 00 4C`
- The lists are cleared every tick by `clear_buildzones` `0x14032bbd0` (`0x140e5f700`) and refilled: `add_buildzones_server` `0x140328df0` adds one box per entity with `BaseIdComponent{baseId, buildZoneSize float3, endOfTimeout, wasAbandoned}` + `CurrentTransform` + `TriggerShape`. `NoBuildZone {isStrict}` entities add no-build boxes. Static scene zones come from `SceneBuildZonesMessage {allowedZones, forbiddenZones, activeZones, playableZones}`.
- `quest_entity_check` reads the zone list through `0x140e720a0`.
- **Open:** the function that answers "may I build at this point" was not isolated (it is inside the cursor code `server_cursor_no_prediction` `0x140292bd0` / `client_cursor` `0x14028fa70`). To build anywhere, a mod can add a huge zone through `addZone` every tick after `clear_buildzones`, or hook the cursor check once found.

### 3.6 ECS programs (building)
| program | update fn | trigger / role |
|---|---|---|
| `player_building_blueprints` | `0x1402d30d0` | `PlaceVoxelObjectEvent` (subtract=0) → placeBlueprint. Writes VoxelWorld, UndoStorage, PlayerBasesSystemClient; out `BuildingPlaceEvent`, `UiBuildingEvent` |
| `player_building_place_prop` | `0x1402d4e30` | `ChangeVoxel` (Add) → placeProp; checks base limits (beds, crafting props, `0x140469a00`, `0x140468e70`, `0x1404699a0` vs balancing caps) and registers the prop in PlayerBasesSystemServer (`0x14046ad70/ad40/ada0`) |
| `player_building_dismantle` | `0x1402d3ae0` | `actor::Dismantle` → removeBlueprint / prop pickup |
| `player_building_undo` | `0x1402d5980` | `ChangeVoxel` (Undo) → `Building_undo` |
| `server_cursor_no_prediction` | `0x140292bd0` | placement preview/validation (reads BuildZoneSystem, PlayerBasesSystemServer, VoxelWorld, UndoStorage, blueprints) |
| `impact_simulation` | `0x1403ef7a0` | terraforming impacts |
| `add_buildzones_server` / `_client` | `0x140328df0` / `0x1403288e0` | rebuild zones |
| `add_no_buildzones*`, `client_add_no_buildzones` | `0x140329300`, `0x140329c80`, `0x1403299b0`, `0x140329f20`, `0x1402fee80` | no-build zones |
| `clear_buildzones` / `_client` | `0x14032bbd0` / `0x14032bc00` | clear zones each tick |
| `update_terraforming_queries_server` | `0x1404178b0` | TerraformingEfficiencyRegistry |
| `player_control_building_fly_mode` | `0x1402dbc80` | build fly mode |
| `render_transform_player_building_spark`, `player_building_spark_sfx` | `0x14030fa30`, `0x14030db40` | client visuals for `PlayerBuildingSpark` |

Components/events: `Terraforming {snappingQueryId, temporaryMiningQueryId}`, `TerraformingLootMemory`, `NoBuildZone`, `ClientNoBuildZone`, `BaseIdComponent`, `ClientBaseIdComponent`, `PlayerBuildingSpark`, `SnappingBoxReference`; events `BuildingPlaceEvent`/`BuildingTearDownEvent`/`BuildingPickUpEvent` (72 B: position, orientation, volumeMin/Max, material, ownerId ...), `UiBuildingEvent`, `TerraformingHitEvent`, `LogicEntityPlacedEvent`.

### 3.7 What a mod would hook or call
- Observe all building: hook `Building_finish` `0x14048baa0` (one call per operation, has player id, material and success) or listen to `BuildingPlaceEvent`/`BuildingTearDownEvent`.
- Block or change a placement: hook `Building_placeBlueprint` / `Building_placeProp` / `Building_removeBlueprint` and return 0.
- Place blocks from code: build a `BuildingOp` like `player_building_blueprints` does and call `0x140486ef0` + `0x14048baa0`. The op layout is only partly known, so the safer route is to call `VoxelWorld_placeBlueprint` `0x140a837b0` directly. Its parameters (shape descriptor from `0x14048b760`, material from `0x14048ee40`) still need work.

---

## 4. Player bases (`PlayerBasesSystemServer`) and flame altars

- **PlayerBasesSystemServer_create(alloc, ctx)** = `0x140467740` (0x2070 B; client variant `0x140467590`). Log `[player_bases] Failed to create PlayerBasesSystem`. Created in `ServerGame_create` and stored at `serverGame+0x2b0`. Sig `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 50 48 8B 01 48 8B EA`
- Layout (seen in code, Medium): `+0x08` u8 isServer, `+0x24` u32 baseCount, base records at `+0xc0 + i*0x130` (i < 16): `+0x48` base id/entity (8 B, returned by `getBaseIdAtPosition`), `+0x60` AABB min float3, `+0x6c` AABB max, `+0x7c` u32 mask of sub-boxes (16 sub-boxes of 0x20 B per base at `[+0x1330]`). `+0x1560` hash map u32 → 12-byte value (used by `npc_bed`, `npc_bed_init`, `npc_active_input`, `player_assign_npc`, read through `0x1404698b0`; likely NPC/bed → base assignment). `+0x15b8` a second map. 16 x 0x58 per-player sets at `+0x1610`.
- **findBaseAtPosition(out, pbs, float3*)** = `0x140468ce0` (24 callers). `out[0] = 0` found / `0x1f` not found, `out[8]` = base index. Sig `48 89 5C 24 08 57 48 83 EC 10 8B 7A 24 45 33 DB`
- **isInsideBase(pbs, float3*) → bool** = `0x140469f70` (used by `environmental_hazards`, `random_pickup_loot_spawner`, `update_player_query_triggers` ...).
- **getBaseIdAtPosition(pbs, WorldPosition*)** = `0x140469600` (`home_base`, `crafting_props_in_range`). **getBaseRecordAtPosition** = `0x1404694b0` (`comfort_energy_gathering`). Sig `48 89 5C 24 08 57 48 83 EC 40 48 8B FA 48 8B D9 48 85 D2 0F`
- Per-frame rebuild: `base_bed_counts_cleanup_server` → `0x140466ba0` (clears counts), `base_register_npc_bed_server` → `0x14046a490(pbs, pos)` (one per `NpcBed`), `base_comfort`, `base_crafting_prop_register`, `base_crafting_props_cleanup`, `register_animal_in_base_server`, `base_register_animal_bed_server`. So villager-related counts (beds) are recomputed each tick from entities.
- Flame altar: `ecs::FlameAltarAction {baseEntityId, versionData, type}` (Abandon, Reclaim, RemoveWater, SetAsPreferredSpawnAltar) is a player input handled by `base_player_action` `0x1402cf610` (writes `WaterSimulationServer`, `ServerProgressSystem`, `BaseIdComponent`); it emits `FlameAltarEvent`. `AdminSetFlameLevel {versionData, flameLevel}` is an admin input. The world flame level lives in `ecs::ServerProgressSystem` (not analysed here).
- Programs that read PlayerBasesSystemServer and matter for villagers: `npc_active_behavior` `0x140456790`, `npc_active_input` `0x140456e50`, `npc_bed` `0x140457380`, `npc_bed_init` `0x1404576f0`, `player_assign_npc` `0x1402d2e30`, `unique_npc_init` `0x14045ade0` / `unique_npc_spawns` `0x14045b0f0` (write it), `animal_behavior`, `animal_reproduction`, `home_base` `0x140333820` (writes `ClientBaseIdComponent`), `comfort_energy_gathering`, `crafting_props_in_range`.

---

## 5. Where the objects live (seen in code)

`ServerGame_create(alloc, params*)` = `0x1404949b0` (0xEA0-byte ServerGame). It creates the systems and registers them with `registerSingleton(world, StringView* name, void* ptr, opts*)` = `0x1409d50d0`:

| singleton name | source |
|---|---|
| `VoxelWorld` | `params[2]` (created by the session setup `0x140e3a350` with `0x140a85b10`) |
| `BuildZoneSystem` | `params[0xf]` |
| `WeatherSystem` | `params[0x10]` (created by `0x140e3a350` with `0x140a99330`) |
| `ServerVoxelDamageSystem` | `params[0x15]` |
| `g38_daytime::Daytime` | `serverGame+0x2d0` |
| `PlayerBasesSystemServer` | `serverGame+0x2b0` |
| `g38_building::UndoStorage` | `params[0xd]` (copied to `serverGame+0x38`) |
| `TerraformingEfficiencyRegistry`, `VoxelBlueprintRegistry`, `VoxelBlueprintMaterialPoolRegistry`, `VoxelBlueprintItemRegistry` | ServerGame registries |

Other offsets: `serverGame+0x08` ECS world, `serverGame+0x1f0` BalancingRegistry (`+0x90` → GameSettings), `serverGame+0x3e8` impact function registry. GameServer (arg 3 of the server tick `0x140e46070`): `+0x1d0` ECS world, `+0x1d8` ServerGame, `+0x1a0` ServerVoxelDamageSystem, `+0x1798` WeatherSystem. GameClient: `+0x4671d8` client WeatherSystem, `+0x4671e0` client Daytime. The client world registers its own singletons in `0x14039fbb0`.
Sigs: ServerGame_create `48 89 5C 24 20 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 20 E3 FF`, registerSingleton `48 89 5C 24 20 55 56 57 41 54 41 55 48 83 EC 20 4C 8B 52 08`, GameServer tick `48 89 4C 24 08 55 53 57 41 54 41 55 41 56 41 57 48 8D AC 24`, savegame load `40 55 56 41 56 41 57 48 8D AC 24 48`.

---

## 6. Hook list for the framework (short)
| purpose | address |
|---|---|
| capture singletons by name | `0x1409d50d0` registerSingleton |
| add custom impact functions | `0x141220f30` registerImpactFunction(registry, nameHash, fn, StringView* name, u8 argCount) |
| read/set time of day | `Daytime+0x48`, `0x140e98630` setTimeOfDay, `0x140e99ab0` setTimeScale |
| day phase change | hook `0x140ea7c00` and compare `+0x58` before/after |
| weather decisions | `0x140a9d730` updateLayers, `0x140a9c700` spawnPhenomenon |
| block/observe building | `0x140486ef0`, `0x140487770`, `0x1404866b0`, `0x14048baa0` |
| terraforming | `0x1403e5b70` impact `terraforming` |
| base lookups | `0x140468ce0`, `0x140469f70` |
| build zones | `0x140e557e0` addZone, after `clear_buildzones` |

## 7. Open questions
- The build-permission test (point inside a base zone, not inside a no-build zone) is not isolated.
- Exact parameter meaning of `VoxelWorld_placeBlueprint` (shape descriptor, material, flags) for calling it without a `BuildingOp`.
- Where the server sends `ServerInfoMessage` (and so when clients get a new day anchor).
- Weather save function and the full `WeatherSample` output struct.
- The two Daytime curve values at +0x5c/+0x64.
- Meaning of the `PlayerBasesSystemServer+0x1560` map value and where the flame altar level per base is stored.

## Appendix A: server impact functions (registered in `0x1404949b0` via `0x141220f30`)
Format: name, callback, hash, argument count. The hash equals the nameHash of the matching `impact_nodes::` type (checked for `Terraforming` and `IsDaytime`), and the count matches its field count. Callbacks have the shape `Result* fn(Result* out, ?, ImpactContext* ctx)`.

`startQuery` 0x1403e58d0 (0x59edd145, 6 args); `stopQuery` 0x1403e5ae0 (0xe27c52c3, 1 args); `getTransform` 0x1403dfc40 (0xa18f5959, 3 args); `getBuffTarget` 0x1403de410 (0xc24cb9bb, 1 args); `getSelf` 0x1403df5f0 (0x1d09759, 1 args); `getParent` 0x1403df450 (0xb8329a37, 1 args); `getEntityParent` 0x1403de9c0 (0xaf655170, 2 args); `getRoot` 0x1403df580 (0xbf5ad13b, 1 args); `getHitTarget` 0x1403ded00 (0xd3c48b1, 2 args); `getLockedTarget` 0x1403df280 (0x1f5adf94, 3 args); `setHookTarget` 0x1403e3630 (0x265d26d5, 2 args); `getQueryResultCount` 0x1403df4c0 (0xf39c6af3, 2 args); `fetchQueryResult` 0x1403da5e0 (0x1b467fe0, 8 args); `getOnCollisionEventCount` 0x1403df3e0 (0x8469973d, 1 args); `fetchOnCollisionEvent` 0x1403da010 (0xda5e432a, 11 args); `isValidEntity` 0x1403e0e30 (0xcf3810a6, 1 args); `isSameEntity` 0x1403e0bc0 (0x530b5054, 2 args); `isEnemy` 0x1403e0930 (0x79b5a97b, 2 args); `isFriend` 0x1403e0a00 (0xac31b17d, 2 args); `isDaytime` 0x1403e0900 (0x52de54cc, 0 args); `isSprinting` 0x1403e0d20 (0xdb41ca5b, 1 args); `overrideTemplateBuildingFlyMode` 0x1403e1f70 (0x1c9ae6cf, 4 args); `getWeaponBaseDamage` 0x1403dfe30 (0x194ae077, 1 args); `createDamageSetup` 0x1403d9710 (0x947a6bd5, 14 args); `applyDamage` 0x1403d4d50 (0xb04d0e7a, 25 args); `setAttribute` 0x1403e2bc0 (0x93853d29, 3 args); `applyAttributeValue` 0x1403d45a0 (0x798388df, 7 args); `(name not decoded)` 0x1403d8c80 (0x44e4187, 6 args); `revertApplyAttributeValue` 0x1403e2580 (0xe41652a7, 7 args); `(name not decoded)` 0x1403d8c80 (0x44e4187, 6 args); `fetchAttribute` 0x1403d9b70 (0xdb35d7c9, 3 args); `fetchOptionalAttribute` 0x1403da4f0 (0xa385dcb9, 3 args); `add` 0x1403e8190 (0x53f7fdba, 4 args); `sub` 0x1403e8190 (0xd367cdeb, 4 args); `mul` 0x1403e8190 (0xc19163a7, 4 args); `div` 0x1403e8190 (0x67eed8fe, 4 args); `clamp` 0x1403d8f30 (0x7f11f072, 4 args); `equal` 0x1403d9240 (0x294c6df, 3 args); `less` 0x1403d9240 (0x69190348, 3 args); `isInState` 0x1403e0ad0 (0x44396293, 2 args); `hasFaction` 0x1403e05d0 (0x695e7565, 3 args); `hasAttackCategory` 0x1403e01f0 (0x2d3e3e5f, 3 args); `hasWeaponCategory` 0x1403e0710 (0x42c15437, 2 args); `hasWeaponCategoryType` 0x1403e07d0 (0xb67b054f, 2 args); `hasDamageType` 0x1403e0410 (0x84891ee2, 3 args); `consumeItem` 0x1403d9530 (0xa0209dda, 1 args); `applyOffset` 0x1403d7f00 (0x90cf9818, 5 args); `spawnEntity` 0x1403e3960 (0x61b11b03, 13 args); `applyBuff` 0x1403d4920 (0x91f30b26, 5 args); `hasBuff` 0x1403e02e0 (0x9866d55a, 2 args); `removeBuff` 0x1403e23d0 (0xca687904, 3 args); `randomBranchNode` 0x1403e21f0 (0x9dcb0a66, 1 args); `randomValueNode` 0x1403e2340 (0x92bf9f5b, 1 args); `randomIndex` 0x1403e22a0 (0xf976c136, 2 args); `triggerNoise` 0x1403e7480 (0xd471249f, 2 args); `pacifyFaction` 0x1403e20e0 (0x39ee6f5d, 1 args); `activateCallFactionForHelp` 0x1403d8b70 (0x283aba15, 1 args); `forEachHitEvent` 0x1403dc6b0 (0x9f34f4ea, 25 args); `SendBaseHitEvent` 0x1403e2900 (0x6b4da871, 5 args); `SendRevivedPlayerEvent` 0x1403e2ad0 (0xbe0e869d, 2 args); `itemBasedMaterialReaction` 0x1403e0e80 (0xad415283, 8 args); `forEachParryEvent` 0x1403dd930 (0xc5453cfb, 5 args); `terraforming` 0x1403e5b70 (0x9aabd181, 9 args); `addWater` 0x1403d3b10 (0xbb6e8623, 5 args); `getCursorTransform` 0x1403de710 (0x79fb429b, 2 args); `clearConsecutiveHits` 0x1403d91d0 (0x5fdbd8ee, 1 args); `isConsecutiveHit` 0x1403e0890 (0x7fca1b56, 1 args); `GetConsecutiveHitCount` 0x1403de460 (0xe56d6397, 1 args); `GetEnemyCountInRange` 0x1403de830 (0xe8912af0, 2 args); `IsSkillUnlocked` 0x1403e0c30 (0x4b131213, 2 args); `SetTransform` 0x1403e3860 (0x236b6555, 1 args); `Normalize` 0x1403e1e30 (0x4adde30c, 3 args); `SetLength` 0x1403e3720 (0xa5ff238b, 3 args); `GetLength` 0x1403dee70 (0xd8e52c57, 2 args); `GetSlotTransform` 0x1403df650 (0x3e1c0b5d, 5 args); `SetEnemyVariable` 0x1403e3300 (0x31fb9991, 3 args); `onEvent` 0x1403e1ee0 (0x45dbf538, 2 args); `entityInRangeWithFaction` 0x1403d9a70 (0x61946152, 4 args); `forEachEntityInStream` 0x1403dc2f0 (0xbe4ece89, 4 args); `forEachHitEventInStream` 0x1403dd020 (0x72fdd0f5, 25 args); `resetEntityStream` 0x1403e24d0 (0x640f8053, 1 args); `isStreamEmpty` 0x1403e0dd0 (0xec2558de, 1 args); `forEachCollisionInStream` 0x1403dbd20 (0xd0a532b0, 11 args); `filterRandom` 0x1403db250 (0xa5e145f8, 4 args); `filterFriends` 0x1403dae10 (0xd6bb2b78, 4 args); `filterSameRoot` 0x1403db630 (0xf52a5619, 4 args); `filterRootEntity` 0x1403db440 (0xdb4bf324, 4 args); `filterFaction` 0x1403dac50 (0xf56c44f1, 5 args); `filterIsInState` 0x1403dafd0 (0x55d4c71d, 5 args); `filterConsecutiveHits` 0x1403da8d0 (0x6e453d91, 3 args); `filterEntityTag` 0x1403daa30 (0xbd6aa1d0, 5 args); `getStreamEventCount` 0x1403df960 (0x9870a3b4, 3 args); `forEachEntityWithFactionInRange` 0x1403dc4b0 (0x19143416, 6 args); `destroyFactionQuery` 0x1403d9990 (0x93e40aa9, 6 args); `forEachPlayerInRange` 0x1403ddbb0 (0xa8787e70, 5 args); `destroyPlayerQuery` 0x1403d9a00 (0x22a62ea4, 5 args); `getWeaponVisualEntityId` 0x1403dff70 (0x78244a69, 1 args); `hasEntityTag` 0x1403e0500 (0xafc5954, 2 args); `triggerKnowledge` 0x1403e7370 (0xf150d49, 2 args); `getCurseModifier` 0x1403de6b0 (0xd2d5826e, 1 args); `destroyEntity` 0x1403d98f0 (0xc691c4da, 1 args); `getWaterChargeSize` 0x1403dfdd0 (0x63b0d4c7, 1 args); `fromTransform` 0x1403ddd90 (0x866dfbeb, 6 args); `addSkillLevel` 0x1403d3320 (0xee3b6ad1, 2 args); `revertaddSkillLevel` 0x1403e2830 (0x1bed77df, 2 args); `debugDraw` 0x0 (0x9127757a, 0 args); `debugDrawCross` 0x0 (0xe7397efa, 1 args); `debugDrawArrow` 0x0 (0xe8573fe7, 2 args);
