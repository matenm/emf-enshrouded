# ECS time outside programs, spawn transform, weather reset

Build 1269579, image base 0x140000000. Static analysis only (Ghidra via rea + `tools/re` scripts).
Signatures come from `tools/re/sig.py` (rel32/RIP displacements wildcarded by hand where sig.py left them) and were checked unique with `sig.verify`.
`keen::Time` = signed 64-bit nanoseconds. `WorldPosition` = 3 x i64 fixed point 32.32.

---

## Short answers

1. **ecsNow** = `tickLength * tickNumber` of the world:
   `ecsNow = *(i64*)(world+0xE16280) * *(u32*)(world+0xE1629C)` (0 when the tick is 0).
   From the sim-step hook argument `storage = world+0xE15DC0`: `*(i64*)(storage+0x4C0) * *(u32*)(storage+0x4DC)`.
   The game has a function for it: `ecsWorld_getTime(world)` = `0x1409b0e10`. Tick length is set to **16,666,667 ns (60 Hz)** at storage init. Save-load computes the value exactly this way before calling `setTimeOfDay`. Calling `setTimeOfDay` from the post-sim-step hook is safe: it is three plain stores into the Daytime object, there are no program or thread requirements, and clients get the new anchor by themselves.
2. **SpawnEntity native**: yes, the position goes in as a `CurrentTransform` spawn component (hash `0x027ac564`, 56 B `WorldTransform`, int64 32.32 position). The native does **not** call `createEntity`. It **queues** a create command in the running program's ECS command buffer (`0x140953770` → `0x140953800`), which the sim step flushes after all programs have run (`0x140a15960` → `0x140a144f0` → `createEntity`). The entity id is reserved at once and returned. The world is the one running `impact_simulation`, i.e. the **server world**. From native code outside programs, call the spawn wrapper `0x1409788d0(world, &guid, &slice)` (createEntity directly). The recipe is in section 2.4.
3. **Weather**: `WeatherSystem` layers: pointer `*(u8**)(ws+0xC8)`, count `*(u64*)(ws+0xD0)`, stride `0x18`. **`nextPhenomenonTime` is the i64 at layer+0x00.** The `WeatherSystem` singleton registered in the **server world** (`ServerGame_create` `0x1404949b0`) is the simulating one (`*(u32*)(ws+0x08) == 0`). The client world registers its own render-side `WeatherSystem` under the same name (mode 1). Reset: zero `nextPhenomenonTime`, and preferably also remove the current and previous instances (section 3.3).

---

## 1. ECS time ("ecsNow")

### 1.1 World clock block (seen in code: world tick `0x1409c86a0`, storage init `0x140984300`)
| world offset | storage offset | type | meaning |
|---|---|---|---|
| `+0xE16280` | `+0x4C0` | i64 | **ECS tick length** in ns. Initialized to `0xFE502B` = 16,666,667 (60 Hz) in storage init `0x140984300` (`mov qword [rdi+0x4C0], 0xFE502B`) |
| `+0xE16288` | `+0x4C8` | i64 | delta time of this frame = tickLen * min(ticksThisFrame, 3) |
| `+0xE16290` | `+0x4D0` | i64 | real elapsed ns since the last tick |
| `+0xE16298` | `+0x4D8` | u8 | "ticked this frame" (1), next 3 bytes cleared |
| `+0xE1629C` | `+0x4DC` | u32 | **tick number** (incremented by the clock before the sim step runs) |
| `+0xE162A0` | `+0x4E0` | u32 | previous tick number |
| `+0xE162A4` | `+0x4E4` | u32 | frame counter |
| `+0xE162A8` | `+0x4E8` | i64 | cached now = tickLen * tick (written by the clock right after the increment) |
| `+0x1CFB6B8` | | i64 | real-time accumulator anchor (wall ns) |
| `+0x1CFB6C0` | | i64 | real-time fixed step (also `0xFE502B`, set in world ctor `0x140976bc0`). The clock divides elapsed wall time by this one, but **ECS time uses `+0xE16280`** |

Both offsets are written in the same tick function, so they hold the same value by default. For ECS time, use `+0xE16280`, because that is the field the game's getters read.

### 1.2 Getters
| Name | Address | Signature | What it does | Conf. |
|---|---|---|---|---|
| **ecsWorld_getTime(world)** | `0x1409b0e10` | `8B 91 9C 62 E1 00 48 8B 81 80 62 E1` | `return tick ? tickLen*tick : 0` (tickLen = ±INT64_MAX sentinels pass through unchanged). Used by save-load, weather time (`0x140499740`), GameServer code. No .pdata entry (leaf). | High |
| world_getNowCached(world) | `0x1409b6e40` | `48 8B 81 A8 62 E1 00 C3 CC CC CC CC` | `return *(i64*)(world+0xE162A8)`. | High |
| **ProgramCtx_getTime(pc, Time* out)** | `0x1409b8030` | `48 8B 01 44 8B 40 6C 48 8B 40 50 4D` | Same formula on the program's state copy: `state=*pc; out = state+0x50 (tickLen) * state+0x6C (tick)`. 919 call sites. | High |
| ProgramCtx_getNowCached(pc, out) | `0x1409b6e30` | `48 8B 01 48 8B 48 78 48 8B C2 48 89` | `out = (*pc)+0x78`, the cached now. | High |

Why the program value is the same: the inline program runner `0x1409a2020` builds a program state whose `+0x50..+0xA8` is a copy of the job slice `+0x50..+0xA8`. The sim step copies the world clock block (`movups xmm0,[storage+0x4C0]` / `[storage+0x4D0]` at `0x140a1e425`) into the per-step context. So `state+0x50 = world+0xE16280`, `state+0x6C = world+0xE1629C`, `state+0x78 = world+0xE162A8`, and the getters use the same multiply-with-sentinel code. The ECS savegame writer `0x1409e23c0` also stores the world "time" as `world+0xE16280 * world+0xE1629C`.

### 1.3 How the three setTimeOfDay callers get ecsNow
- **Savegame load** `0x140e4a4f0`: `setTimeOfDay(ctx[4] /*Daytime*/, savedTod, ecsWorld_getTime(ctx[0] /*world*/))`. Weather load in the same function uses `Daytime_getGameTime(daytime, ecsWorld_getTime(world))`. High.
- **ServerGame_create** `0x1404949b0`: `setTimeOfDay(serverGame->+0x2D0, 7h = 25,200,000,000,000, 0)` (tick 0 means time 0). High.
- **admin_player** `0x1402cae30`: `setTimeOfDay(daytime, AdminSetDayTimeAction.syncDaytime, AdminSetDayTimeAction.syncDaytimeTick)`. Both values are `keen::Time` sent by the admin client (input `+0x478`/`+0x480`). The client sends its own ECS time, which tracks the server tick. High.

### 1.4 Using it from the post-sim-step hook
```cpp
// hook on 0x140a1e2e0(void* storage, bool ticked), after calling the original:
uint8_t* world = (uint8_t*)storage - 0xE15DC0;
if (world[0xE15DC8] != 0) return;                 // server world only
int64_t  tickLen = *(int64_t*)(world + 0xE16280);
uint32_t tick    = *(uint32_t*)(world + 0xE1629C);
int64_t  ecsNow  = tick ? tickLen * (int64_t)tick : 0;  // == ((int64_t(*)(void*))0x1409b0e10)(world)
void* daytime = GetSingleton(world, "g38_daytime::Daytime");   // or ServerGame+0x2D0
((void(*)(void*, int64_t, int64_t))0x140e98630)(daytime, hours * 3600000000000LL, ecsNow);
```
**Is it safe there?** Yes (High):
- `setTimeOfDay` = `daytime+0x10 = ecsNow; daytime+0x18 = timeOfDayToGameTime(daytime, tod) (0x140e667f0); daytime+0x24++`. No locks, no ECS calls, and it needs no program context. Save-load and ServerGame_create call it outside programs on the server thread.
- The server sim step runs synchronously on the server thread: GameServer update `0x140e46070` → `0x140a1e210(world)` → `0x140a1e2e0(storage, 1)`. After it returns, no program jobs are running. The Daytime readers (`server_update_daytime`, impact, AI, ...) only run inside the step, and the server Daytime object is not shared with the client world (the client worlds create their own).
- Client sync is automatic. The GameServer ServerInfo sender `0x140e4c620` calls `Daytime_getSyncIfChanged(out, &GameServer+0x17C4, daytime)` = `0x140eadec0`. When `daytime+0x24` (the change counter) differs from the last value sent, it copies `{+0x10 ecsTime, +0x18 gameTime, +0x20 scale}` into `ServerInfoMessage.daytimeSync` and sends it.
- Order inside one GameServer frame: clock `0x1409c86a0` (tick++) → sim step → weather tick `0x140a9dd90` (uses `Daytime_getGameTime(daytime, ecsWorld_getTime(world))`). A change made in the post-sim-step hook is therefore seen by the weather update in the same frame.

Option (Medium, derived from the Daytime math in world_building_daytime.md): to keep the game time monotonic, which avoids the weather stall and the day-0 reset, set the anchor yourself instead of calling setTimeOfDay: `g = getGameTime(daytime, ecsNow)` (`0x140e76ce0`), `cycle = *(i64*)(daytime+0x38) + *(i64*)(daytime+0x40)`, `target = (g - g % cycle) + timeOfDayToGameTime(daytime, tod)`, and add `cycle` if `target < g`. Then write `+0x10 = ecsNow`, `+0x18 = target`, `++*(u32*)(daytime+0x24)`.

Signatures for the new functions:
- `0x140eadec0` Daytime_getSyncIfChanged: `41 8B 40 24 39 02 75 03 32 C0 C3 89`
- `0x140e4c620` GameServer_sendServerInfo: `48 8B C4 55 53 41 54 41 55 41 57 48`
- `0x140a1e210` serverWorld_runSimStep(world): `40 53 57 48 83 EC 38 80 39 02 48 8B` (checks `world+0 == 2` and not paused `world+0x1CF39CC`, then sim step(storage, 1))
- `0x140499740` ServerGame_getWeatherTime(serverGame): `40 53 48 83 EC 20 48 8B D9 48 8B 49 08 E8 ?? ?? ?? ?? 48 8B 8B D0 02 00 00` (`serverGame+0x08` = world, `+0x2D0` = Daytime)

---

## 2. Spawning

### 2.1 SpawnEntity impact native `0x1403e3960`
Sig `4C 89 44 24 18 55 53 56 57 41 54 41 55 48 8D 6C`. `fn(out, Slice<u32>* ports, ImpactCtx* ctx)`. It reads 13 ports (each a u16 offset into the impact frame; 0 = not connected). Order: port 0 is the output `entityId`, then the fields of `keen::impact_nodes::SpawnEntity` in types.json order:

| port | field | goes to SpawnParams |
|---|---|---|
| 0 | entityId (out) | receives the returned id |
| 1 | breakImpactHierarchy (bool) | `+0x6E` |
| 2 | addDespawnLink (bool) | `+0x6F` (port read without a null check) |
| 3 | copyParentLevel (bool) | `+0x70` (default 1) |
| 4 | copyParentFaction (bool) | **also written to `+0x70`** (game bug: `+0x71` stays 1) |
| 5 | templateInput (Guid) | `+0x38`. Required; aborts if 0 |
| 6 | transform (WorldTransform 56 B) | `+0x00`, copied verbatim |
| 7 | position (float3) | used only if port 6 is unconnected: `pos_i64 = (i64)(f * 4294967296.0f + 0.5f)` (float math!), identity rotation, scale 1 |
| 8 | velocity (float3) | `+0x54`, sets `+0x60` hasVelocity |
| 9 | parentEntityId | `+0x4C` |
| 10 | targetEntityId | `+0x50` |
| 11 | levelOverride (u32) | `+0x64`, sets `+0x68` |
| 12 | factionOverride (u8) | `+0x6C`, sets `+0x6D` |

`+0x48` = the impact's own entity (`0x1409b2e40(ctx->+0x30->[0])`), used as the "source". Default transform when nothing is connected: position 0, quaternion (0,0,0,1) at +0x18, scale (1,1,1) at +0x28.

### 2.2 Impact_spawnEntity(ctx, SpawnParams*) `0x1403e3e40`
Sig `48 89 5C 24 08 55 56 57 41 56 41 57 48 8D AC 24 20 F3 FF FF`. Builds a `Slice<SpawnComponent>` (inline capacity 32). Components, in order:
1. **`CurrentTransform` `0x027ac564`, size 0x38**: the 56-byte transform from params+0x00. Always added. (High)
2. `ImpactHierarchy` `0xcd3947e0` (16): copied from the source entity, or built from (source, parent); reset by `0x141217a80` if breakImpactHierarchy is set.
3. `TargetEntity` `0x76ee19b9` (4) if targetEntityId != 0, else `SelectedTargets` `0x486b5d4c` (0x888) copied from the source.
4. Copies of the source's combat stats when present: `Dexterity 0x10180f99`, `Strength 0x23dc6392`, `Intelligence 0x6d35734c`, `CritChance 0x1ce7b177`, `CritDamageMod 0x465ddd7a`, `DamageMod 0x45fda47a`, `ToolDamageMod 0x80051de0`, `StaminaDamageMod 0xe9b38733`, `DamageModAgainst 0xd3199cbd`, `ImpactItemConfig 0x4006f1f8`, `WeaponReference 0xec39b114`, `ParriedProjectile 0xc7ffa5b9`.
5. Level: `OverrideLevel 0xa21e7ee7` (levelOverride), or `Level 0xe550da37` copied from the source if copyParentLevel is set. Faction: `Faction 0xa1f30f21` (override, or copied when copyParentFaction is set).
6. `Velocity 0xfb332100` (12) if set. `DespawnLink 0x7a10128f` (8) = {source, 1} if addDespawnLink. `SpawnedByLogicSystem 0x894961f5` (8) if the impact context has one (`ctx->+0x30->+0x30`).

Then: `queueCreateEntity(cmdBuf = *(*pc + 8), &outId, sourceId, parentId, &guid, &slice, kind=0)`. Here `pc = ctx->+0x30->[0]`, and `0x1409b0860(pc)` returns the program state's `+0x08` (the per-program command buffer). The native adds no `SpawnTime` (the admin spawn does, see 2.5). Confidence: High.

### 2.3 The command-buffer path (deferred create)
| Name | Address | Signature | What it does | Conf. |
|---|---|---|---|---|
| queueCreateEntity | `0x140953770` | `40 53 48 83 EC 60 33 C0 44 89 4C 24` | `(cmdBuf, u32* outId, u32 sourceEntity, u32 parentId, Guid*, Slice<SpawnComponent>*, u8 kind)`. Builds a SpawnRequest {parent, guid, slice, mask 0, kind} on the stack and calls the core with entityId 0. 40 callers (programs). | High |
| queueCreateEntity core | `0x140953800` | `48 89 5C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 50 4C` | `(cmdBuf, u32* outId, u32 sortKey, u32 entityId, SpawnRequest*)`. Logs `[ecs] Can not create entity with template guid 0`. Allocates a type-0 command record (`0x140952400`): `+0xC` id, `+0x10` parent, `+0x14` guid, `+0x25` kind, `+0x26` mask, `+0x28` u8 count, `+0x2C` packed components `{u32 hash; u16 size; bytes}`. If entityId is 0, it takes `++*(u32*)*(cmdBuf+0x38)` atomically, so **the id is known immediately**. Also called directly by `admin_player` (admin spawn). | High |
| command executor | `0x140a144f0` | `4C 8B DC 55 41 54 41 55 49 8D AB 58` | `(storage, record)`. Switch on `record+2`: 0 = create → `createEntity(storage, rec+0xC, &{parent, guid, slice, mask, kind}, 0)` (skipped on the prediction world); logs `Could not create entity with id %k`. 1 = destroy, 2/3 = add component, 5-13 path/range queries, ... | High |
| command flush | `0x140a15960` | `40 53 41 54 48 83 EC 38 45 33 E4 48` | Called once by the sim step (`0x140a1e2e0` at `0x140a1e4fa`) after the programs; runs the executor over the queued records. | High |

So the entity only exists after the current sim step's flush. Init-only programs (flag 0x8) and normal programs see it from then on.

### 2.4 createEntity and the SpawnRequest (corrections to ENGINE_MAP 1.4)
`u32 createEntity(void* storage, u32 entityId /*0 = allocate*/, SpawnRequest* req, void* componentMask /*nullable 0x50 B bitset; pass 0*/)` = `0x14097b790`. Returns the id (0 = failed).
- The 2nd argument is the **entity id to use** (0 = allocate from the atomic counter at `storage+0x498`), not an "owner". Ids 1..0x3FF are treated specially: they map to player mask bit `1<<(id-1)`.
- **SpawnRequest (0x30)**: `+0x00 u32 parentEntityId` (stored in the entity record next to the id; 0 = none), `+0x04 Guid template`, `+0x18 SpawnComponent* comps`, `+0x20 u64 count` (max 128), `+0x2A u16 player mask` (used when `kind & 2`), `+0x2C u8 kind` (0 for normal spawns; bits 4/8 set extra entity flags).
- `SpawnComponent (0x18) = {u32 qualifiedHash; u32 pad; void* data; u64 size}`. The size must equal the component size (`Component %u size of %,u does not match to spawn size of %,u`). Spawn components are added to the archetype even when the template does not list them (that is how `DespawnLink`/`TargetEntity` get attached). An unknown hash logs `Could not find component %08x`.
- Not thread safe: it writes chunk storage directly. Inside programs the game always uses the command buffer. Outside programs the game calls it directly from the server thread: save-load (`0x140e3b4d0` etc.) and the server system update `0x140e47710` go through the spawn wrapper.

**Spawn wrapper** `u32 spawnEntity(void* world, const Guid* tmpl, const Slice<SpawnComponent>* comps)` = `0x1409788d0`, sig `48 83 EC 58 0F 10 02 8B 15 ?? ?? ?? ?? 33 C0 41 0F 10 08 48 81 C1 C0 5D`. It calls `createEntity(world+0xE15DC0, 0, &{parent 0, guid, comps, mask 0, kind 0}, 0)`. Example from the game, save2 loader `0x140e3b4d0`: components `CurrentTransform(0x38)`, `OverrideLevel(4)`, `0x0e699564(8)`, `0x4927f4e2(0x30)`, then `0x1409788d0(world, &guid, &{ptr, 4})`.

**Minimal recipe (native, server thread, right after the sim step returns)**, High confidence from code; the spawn wrapper with a `CurrentTransform` spawn component is confirmed in game (`ENGINE_MAP.md`, "Spawn / destroy"):
```cpp
#pragma pack(push, 8)
struct Guid { uint8_t b[16]; };
struct WorldTransform { int64_t pos[3]; float rot[4]; float scale[3]; uint32_t pad; }; // 56 B
struct SpawnComponent { uint32_t hash; uint32_t pad; const void* data; uint64_t size; };
struct SpawnSlice { const SpawnComponent* data; uint64_t count; };
#pragma pack(pop)
static_assert(sizeof(WorldTransform) == 56 && sizeof(SpawnComponent) == 0x18);
using SpawnFn = uint32_t(*)(void* world, const Guid*, const SpawnSlice*);   // 0x1409788d0

WorldTransform t{};
t.pos[0] = llround(x * 4294967296.0);  t.pos[1] = llround(y * 4294967296.0);  t.pos[2] = llround(z * 4294967296.0);
t.rot[3] = 1.0f;  t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;
int64_t spawnTime = ecsNow;                                  // optional, see 2.5
SpawnComponent comps[] = {
  { 0x027ac564, 0, &t, 56 },                                 // CurrentTransform (required for a position)
  { 0x583963a3, 0, &spawnTime, 8 },                          // SpawnTime (optional)
};
SpawnSlice slice{ comps, 2 };
uint32_t id = ((SpawnFn)0x1409788d0)(serverWorld, &templateGuid, &slice);   // 0 = failed (see log)
```
Optional extras with the same pattern: `OverrideLevel 0xa21e7ee7` (u32), `Faction 0xa1f30f21` (u8), `Velocity 0xfb332100` (float3), `DespawnLink 0x7a10128f` (8 B). To set a parent, call `createEntity` directly with `req+0x00 = parentId`.
Do not call it from inside a program job or from another thread. Never call it on the client world (kind byte at `world+0xE15DC8` must be 0).

### 2.5 Admin spawn (reference)
`admin_player` handles `AdminSpawnCommand` (`PlayerInput+0x3C0+0x18`: position WorldPosition, level, templateGuid, count, ...) by calling `queueCreateEntity core` directly. Components: `SpawnTime 0x583963a3` = `ProgramCtx_getTime`, `CurrentTransform`, `OverrideLevel`, optional `RandomLoot 0x7f2a8502` (36), `UnitGroupMember 0x8292d6fe` (12), and more (`LifeTime 0xf22c253c`, `EnemySettings 0x67fe5508`, player-related ones for fake players). The transform first goes through `0x14095a4f0`/`0x14095a760` (not traced; probably snapping or normalization). Medium.

---

## 3. Weather reset

### 3.1 Getting the WeatherSystem
- The server world registers it in `ServerGame_create` `0x1404949b0`: `registerSingleton(serverWorld, "WeatherSystem", createParams[0x10], opts)`. The same pointer is `GameServer+0x1798`, which the GameServer tick passes to `0x140a9dd90`. → `ws = GetSingleton(serverWorld, "WeatherSystem")`. High.
- The client world setup `0x14039fbb0` also registers a `"WeatherSystem"` (the render-side one, mode 1). Use the world with kind 0 and check `*(u32*)(ws+0x08) == 0` (server/simulating). High.

### 3.2 Layer layout (seen in `WeatherSystem_updateLayers` `0x140a9d730`, sig `48 8B C4 4C 89 48 20 48 89 48 08 53 55 57 41 57`)
| offset | type | meaning |
|---|---|---|
| `ws+0xC8` | ptr | layer array |
| `ws+0xD0` | u64 | **layer count** |
| layer `+0x00` | i64 Time | **nextPhenomenonTime** (weather/game time; the roll happens when `<= now`) |
| layer `+0x08` | u64 | current phenomenon index (excluded from the next pick) |
| layer `+0x10` | u32 | current instance handle (0 = none) |
| layer `+0x14` | u32 | previous instance handle (fading out; removed when its despawnTime `<= now`) |
Stride `0x18`. Instance pool entry 0x50 B at `ws+0x28` blocks (handle = `generation<<16 | index`), payload `+0x08`; `payload+0x30` spawnTime, `+0x38` despawnTime (game time). High.

Roll (when `layer.next <= now`): `removeInstance(ws, layer.prev)`, `prev = cur`, `cur = 0`, weighted pick, then `spawnPhenomenon(ws, res, now, layerIdx, idx, forced)` (`0x140a9c700`), which sets `next = now + duration - despawnFade`.

### 3.3 Recipe after setTimeOfDay (same post-sim-step hook; the weather tick runs later in the same frame)
```cpp
uint8_t* ws = (uint8_t*)GetSingleton(serverWorld, "WeatherSystem");
if (ws && *(uint32_t*)(ws + 0x08) == 0) {
  auto removeInstance = (void(*)(void*, uint32_t))0x140a9b560;   // handle 0 = no-op
  uint8_t* layers = *(uint8_t**)(ws + 0xC8);
  uint64_t n      = *(uint64_t*)(ws + 0xD0);
  for (uint64_t i = 0; i < n; ++i) {
    uint8_t* L = layers + i * 0x18;
    removeInstance(ws, *(uint32_t*)(L + 0x14)); *(uint32_t*)(L + 0x14) = 0;  // previous
    removeInstance(ws, *(uint32_t*)(L + 0x10)); *(uint32_t*)(L + 0x10) = 0;  // current
    *(int64_t*)(L + 0x00) = 0;                                               // nextPhenomenonTime
  }
}
```
- Zeroing only `nextPhenomenonTime` already ends the stall: the next weather tick re-rolls. But the old current instance then becomes "previous", and its despawnTime is in the old, later timeline, so it stays visible until the following roll. Removing both instances avoids that.
- `WeatherSystem_removeInstance(ws, u32 handle)` = `0x140a9b560` (sig `85 D2 0F 84 ?? ?? ?? ?? 89 54 24 10 53 57 48 83`). On the server it frees the pool slot, drops the handle from the live list (`ws+0x88/+0x90`) and queues a remove for every client tracker (`ws+0xE0`, stride 0x58, via `0x140a9b090`). So clients get `WeatherSyncRemoveInstanceMessage`, and the new instance arrives through the normal add path. It does **not** clear the layer handles; the caller must zero them. High.

---

## Hook / call summary for the framework
| Purpose | Address | Notes |
|---|---|---|
| ECS time | `0x1409b0e10(world)` or `tickLen(+0xE16280) * tick(+0xE1629C)` | 60 Hz, ns |
| Set time | `0x140e98630(daytime, todNs, ecsNow)` | server thread, outside programs; bumps `+0x24`, auto-synced |
| Spawn now | `0x1409788d0(world, Guid*, Slice*)` → `0x14097b790` | server thread, outside programs |
| Spawn from inside a program | `0x140953770(*(*pc+8), &id, self, parent, Guid*, Slice*, 0)` | deferred to the end of the step |
| Weather layers | `ws+0xC8` / `+0xD0`, stride 0x18, next @ +0 | server WeatherSystem (mode 0) |
| Remove weather instance | `0x140a9b560(ws, handle)` | network-safe |

## Open questions
- Not runtime-tested: direct `createEntity` after the sim step (strong static precedent: save2 loader and server system update), and the weather reset.
- Spawning an entity outside the step: replication is queued through the entity update queue (`Entity Update Queue is full for create`). This should behave like the save-load spawns, but it has not been checked at runtime.
- `0x14095a4f0`/`0x14095a760` (admin spawn transform post-processing) not traced. Also not traced: whether `SpawnTime` matters for templates with `LifeTime`/despawn logic (the admin spawn sets it; the SpawnEntity native does not).
- `copyParentFaction` port bug (port 4 overwrites `+0x70`): not checked in game.
