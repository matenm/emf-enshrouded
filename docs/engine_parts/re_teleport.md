# Teleporting players and actors (RE for C API G5)

Build 1273043 (client `enshrouded.exe` sha `d64966e2…`, dedicated server `enshrouded_server.exe` sha `d7baa768…`).
Static analysis: Ghidra through REA, plus `tools/re` (`program_access.py`, `disasm.py`). What was seen in the running
game is tagged.
Client addresses are VAs at image base `0x140000000`. Server addresses are given where EMF needs them.
`docs/ecs_programs.md` has 1269579 addresses; the 1273043 ones are below.

Tags: **Confirmed** = seen in code at the address given. **Likely** = strong evidence. **Guess** = not checked.
**Confirmed in game** = seen in the running game (a hosted LOCAL world and the dedicated server, both 1273043) with a
test mod (section 8).

## Short answer

**Confirmed in game: it works.** An EMF server program pushing the kind-3 `Teleport` record moved the player
(hosted game and dedicated server, joined client snapped once, no rubber-band), villagers and an enemy. No core change
was needed: the mod callback gets the raw `ProgramContext*`, so `*(*(ctx)+8)` is the command buffer. The things G5 must
add on top of section 5 are in section 8.3.


- `Teleport` is a dynamic, server-only component. Game code adds it to an entity by **pushing an "add component" record into the running program's command buffer**. The engine applies all such records at the end of the sim step. Nothing has to be declared in the program descriptor. **Confirmed** (`actor_teleport` and `admin_player` do it; neither lists `Teleport` anywhere).
- Next tick, program `teleport` (#701) moves the entity, bumps `TeleportCount`, and marks the component applied. The tick after, it queues the removal. **Confirmed**.
- `TeleportCount` is replicated. A client snaps (no interpolation) when its counter changes. **Confirmed** in code, and in game (section 3).
- Movement is server-authoritative: the client sends inputs, not positions. So a server-side teleport sticks. **Confirmed** from the input layout; the absence of rubber-banding is **Likely** (vanilla fast travel uses this exact path) and confirmed in game (section 3).
- **Recommended G5 path:** EMF's own internal server program pushes the same record the game pushes (kind 3, `Teleport`, 80 bytes) into `*(job+8)`. One new signature (`QueuePush`). Works for players and for NPCs that have `TeleportCount` and `LastTransform`.

---

## 1. How a teleport starts

### 1.1 The three game paths (all end in the same place)

| Path | Who starts it | What adds `Teleport` | Conf. |
|---|---|---|---|
| Fast travel from the map | `ClientPlayerInputData.teleportAction {target entity, teleporterType}` (PlayerInput +0x174, version +0x17C). Consumed by `player_control_action_sequence` (#524, `0x1402d7990`; compare at `0x1402d8780`, store into `ServerConsumedPlayerInput.consumedTeleportAction` +0x54 at `0x1402d88dd`). It starts the teleport action sequence. | `actor_teleport` | Confirmed (consume); Likely (sequence start) |
| Teleport zones (portals) | `InteractionAcceptedEvent` on an entity with template `TeleportZone`. `teleport_interaction` (#702, `0x140347a20`) finds the linked zone (`TeleportZoneConnection`) and writes the player's `ActorInput` and `SelectedTargets`. | `actor_teleport` | Likely (from its access list) |
| Respawn | `update_player_spawn` (`0x1402ec3e0`, server, not predicted) writes the player's `SelectedTargets` with the spawn point. The respawn sequence then fires the same event. | `actor_teleport` | Likely |
| Admin teleport | `PlayerInput.fromAdminClient.adminTeleport` (`AdminTeleportAction`, PlayerInput +0x434). `admin_player` (`0x1402cafd0`) checks `isAdmin` and compares the version (+0x458) with `consumedAdminTeleport` (+0xAC). | `admin_player` itself (`0x1402cc02e`) | Confirmed |
| NPCs and animals | Villager code (`0x1404488c0`, `0x140448b80`, NpcCommandEvent handler `0x14044f3c0`) and `animal_behavior` (`0x14022ad20`). | the NPC's own program | Confirmed |

`actor_teleport` (#39, update `0x1402707f0`, flags `0x10`): for each actor it scans the actor's action-sequence events for `keen::actor::TeleportToTargetEvent` (FNV `0x5c77582c`, check at `0x1402708c8`). The event names a `SelectedTargets` slot (0x88 bytes each, flag `+0x82 & 2`). It then:
- builds `Teleport { targetTransform = slot.transform (56 B), targetEntityId = slot +0x64, delayEndTime 0, applied 0, searchBestSpawnPosition }`;
- sets `searchBestSpawnPosition` only for players (id 1..1023), when the target has no `PlayerSpawnPoint` or its type is 1;
- for players, pushes a "replication checkpoint" record (kind 0x10, `0x140955620`);
- pushes the add record (kind 3, `0x140953670`, at `0x140270a42`).
**Confirmed** (decompiled).

`TeleportToTargetEvent` is an action-sequence event, not an ECS event. EMF's `emit_event` cannot send it. **Confirmed** (it is `keen::actor::`, read from the actor's sequence list by `0x140213b50`).

`AdminTeleportAction {Transform transform (float3 pos, f32 scale, quat); bool searchBestSpawnPosition; VersionedData}` comes from an admin client inside `PlayerInput.fromAdminClient` (+0x3C0; the action at +0x74). Only `admin_player` consumes it, and only for the sending player (the "current entity"). It converts the float position with `trunc(m × 2^32 + 0.5)`, sets scale (1,1,1), keeps the action's orientation and copies the flag. **Confirmed**. The client-side producer was not traced.

`TeleportPlayerEvent {timeStamp, playerEntityId, targetEntityId, teleporterType, float3 targetWorldPosition}` (hash `0xb10d83ef`): no program lists it. Its hash is kept in a global (`0x14150e4c8`) read by client-side code: `0x1403a90e0` (called from the client game update `0x14086fa20`) and UI code (`0x140825c50`, `0x140852760`). No server producer was found. **Guess:** a client notification for the fast-travel screen. Not useful for G5.

There is also an "Emergency teleport triggered for entity %k transform %k" path (`0x1402ca140`, from `player_control_not_predicted` and `0x1402ed730`). It starts action sequence `0x7bb17afe` toward the player's spawn point. **Confirmed in game:** a live player put 3 m (or 16-20 m) inside the terrain gets the `W` line about 0.3-0.6 s later and a game `Teleport` to the spawn point's landing pad about 1 s after that (`TeleportCount` +1). For a **dead** player inside the terrain it fires every tick (1,500 lines) and does nothing until the player respawns.

Respawn also goes through `Teleport`: each respawn bumped `TeleportCount` by 1. **Confirmed in game.**

### 1.2 The command buffer (the engine's "add component to entity")

Every program slice gets a command buffer: `cmdbuf = *(job + 8)` (`0x1409b1c50` is `return *(*(ctx)+8)`). The slice runner `0x1409a3400` sets it from its task entry and writes the program id into `cmdbuf+0x28`. **Confirmed**.

`QueuePush` = `0x140953670` (server `0x1405f3bb0`, 215 callers):
```c++
// returns a zeroed record, or NULL when the buffer is full
Record* queuePush(CmdBuf* cb, Buf* buf /* = cb+0x10 */, uint8_t kind, uint32_t size, uint32_t entity);
// buf: +0 data ptr, +8 capacity (u64), +0x10 used bytes (u32), +0x14 record count (u32)
// record (16-byte aligned): +0 u16 size, +2 u8 kind, +3 u8 0, +4 u16 cb[+0x28] (program id), +6 u16 cb[+0x2a],
//                          +8 u32 entity, +0xC payload (zeroed)
```
**Confirmed** (decompiled). `size` is the whole record, header included (QueuePush zeroes `size - 0xC` bytes from
`+0xC`); a kind-3 record has no count field: unused hash slots stay 0 (`addComponents` `0x140954270`, **Confirmed**).

**Confirmed in game** (EMF's own system program, flags `0x4|0x10`, hosted game and dedicated server):
`*(job+8)` is set for a system program too; capacity (`+0x18`) is **1 MiB** (1048576); the buffer is **shared**: before
the test mod pushed, `used`/`count` were already 160-1056 bytes / 2-20 records from other programs' slices, and `cb+0x28`
held the test mod's own program id (820), written by the slice runner. One push adds the record 16-byte aligned
(416 → 604 bytes). The largest `used` seen was about 1.2 KB: no pressure. `job+0x50` is **not** the tick in this job
(it read 16666667, the tick length in ns): use the world's tick.

Record kinds seen (processor `0x140a16020`, jump table at `0x140a17444`):

| Kind | Payload | Applied by | Conf. |
|---|---|---|---|
| 2 | a list of components with data ("Too big components" at 32 KB); its meaning was not traced | `0x140954400` builds it | Confirmed (builder only) |
| **3** | **add components**: `+0xC u32 hash[16]`, `+0x4C u16 size[16]`, data from `+0x6C`, packed | `addComponentNow` per slot; on failure logs `[ecs%c] Could not add component '%s' (%08x) for entity %k '%k'` | Confirmed |
| 4 | remove components: `+0xC u32 hash[16]` (record size 0x4C) | `0x1409dd9c0(storage, entity, typeIndex)` | Confirmed |
| 0xF | set the entity's streaming "hot point": `+0xC float2 (x, z)` | `0x140956740`; logs `Hot point already set` on failure | Confirmed |
| 0x10 | set a replication checkpoint for player `+0xC` | `0x1409f36e0` (logs `Replication Checkpoint %k Set for Player %k in Tick %,k`; deadline = tick + 30) | Confirmed (what the client does with it: Guess). **Confirmed in game:** `+0xC` = the entity id (1) logs "Player 0"; "Checkpoint 1 Set" in the push tick, "Checkpoint 2 Set" ~32-35 ticks later, "Reached" 39-47 ticks after the push, the same pattern as the game's respawn. Teleports without it arrived just as well. |
| 0x11 | fatal: "ChangeEntityComponent command not allowed in command queue" | | Confirmed |

The buffers live at `storage+0xEB9F08` (stride 0x40, count `storage+0xEB9F10`). `FlushCmdBuffers` `0x140a17490` (server `0x1406b8640`) runs inside SimStep (called from `0x140a1fe10`) on the world's own thread. It sorts all records by program id, then applies them. **Confirmed**. It is the function ENGINE_MAP calls "process queued creates".

Helpers built on `QueuePush`:
- `addComponents(cb, entity, Slice<{u32 hash; u16 size; void* data}>*)` = `0x140954270` (server `0x1405f47b0`): one kind-3 record with up to 16 components. **Confirmed**.
- `addComponent<Teleport>(cb, entity, const Teleport*)` = `0x140256260` and `removeComponent<Teleport>(cb, entity)` = `0x1402257b0`. Both have **no callers** in 1273043 (inlined everywhere). Good reference, too fragile to depend on. **Confirmed**.

The immediate function under the queue: `addComponentNow(storage, u32 entity, u32 hash, {void* data; u64 size}*)` = `0x1409559e0` (server `0x1405f5f20`). If the entity **already has** the component it returns true and **does not overwrite the data**. Otherwise it moves the entity to the new archetype and copies the data. **Confirmed**.

How to add a component safely:
- From inside a program update (any worker thread): push a kind-3 record into that slice's own `*(job+8)` buffer. This is what game code does. **Confirmed**.
- From outside programs: `addComponentNow` on the world's own thread while none of its jobs run (before SimStep, or after `waitForWorldJobs`). The flush calls it on that thread too. **Likely** safe; not used by game code outside the flush (callers: the flush, `0x140955c10`, `0x14098dae0`).
- Server world only for `Teleport` (`server_only`). The record's world is the job's storage.

### 1.3 The `Teleport` component and program #701

`Teleport` (80 B, hash `0x23384d40`; attributes `server_only, is_dynamic, do_not_save, hidden`): **Confirmed** (types.json + code).

| Off | Field | Notes |
|---|---|---|
| 0x00 | `WorldTransform targetTransform` | pos i64×3 (32.32 fixed) @0, quat @0x18, scale float3 @0x28. #701 copies **all 56 bytes** into `CurrentTransform`, scale included. |
| 0x38 | `EntityId targetEntityId` | if it has `PlayerSpawnPoint`, the target is moved to landing pad `(entity-1) % 5` (`0x1403b1cf0`). |
| 0x40 | `Time delayEndTime` | #701 never reads it. Other readers: not checked (Guess: unused). |
| 0x48 | `bool applied` | |
| 0x49 | `bool searchBestSpawnPosition` | |

Program `teleport` (#701, update `0x14027b4e0`, server `0x14008dc20`, flags `0x10` = server only, not in prediction). Required writes: `CurrentTransform, LastTransform, Teleport, TeleportCount`. So **it only runs for entities that have all four**. Per entity, **Confirmed** (decompiled):
1. If `applied`: push a kind-4 record that removes `Teleport`. Done.
2. Else: target = `targetTransform`; landing pad if `targetEntityId` has `PlayerSpawnPoint`.
3. Players (id 1..1023): push a hot point (kind 0xF) at the target's x/z, every tick until applied. **Likely:** this makes the server stream the destination.
4. If `searchBestSpawnPosition`: `0x1403cdc40` searches `ecs::PathGridSystem` (`0x1409ae170`) near the target. Status 0 = found (use it); 1-3 = **not ready: skip this entity, retry next tick**; anything else = use the raw target.
5. Players: game log, level info: `Player EntityId %k teleported From %k to %k`.
6. Write `CurrentTransform = target`, `LastTransform.transform = target`, `TeleportCount.counter += 1`, `TeleportCount.lastTeleportTime = now`, `applied = 1`. Zero `Velocity`, `LerpTransform`, `ServerSubmerge` when present. All through `markWritten`.

So one teleport takes three ticks: add (end of tick N), move (N+1), remove (end of N+2). More ticks if the ground search waits.
**Confirmed in game** (after each server step): push tick N: `Teleport` present, `applied` 0; N+1: `CurrentTransform`
= `LastTransform` = target, `TeleportCount` +1, `lastTeleportTime` = now, `applied` 1; N+2: `Teleport` gone. With
`searchBestSpawnPosition` the move came 4-32 ticks later (the "not ready" retries; 1 tick when the area was already
loaded, 26-32 ticks for a destination 1 km away). The game logged `Player EntityId 1 teleported From … to …` each time
(with the ground-search result as "to"). No `Could not add component` or `Hot point already set` line in any test.

Other programs react while `Teleport` exists (read or optional read): fall damage is reset (`actor_teleport_fall_damage`), glider, hook shot, ladders and climbing walls are cancelled, villager and animal AI reset, interaction queries pause. **Confirmed** from the access lists; effects **Likely**.

`actor_is_ready_for_gameplay` (`0x140266580`): a player with a `Teleport` component is **not ready for gameplay** (Actor +0xBF8 bit 50 cleared). **Confirmed**. **Confirmed in game:** bit 50 (`Actor.currentState`, `0xBF8` = 3064) clears the tick after the add and stays clear after the move: 20-56 ticks for short hops, 146-174 ticks after a 1 km move (streaming); the player does not fall during it. Villagers' and enemies' bit stays set. So a `Teleport` that never goes away freezes the player. It never goes away when the entity lacks `TeleportCount` or `LastTransform`, or when the ground search stays "not ready".

### 1.4 searchBestSpawnPosition

`0x1403cdc40(out, float3 pos, bool isPlayer, PathGridSystem*, u8 variant)` builds a query (values 0x18, 0x18, 4) and a cell-type exclusion mask: types {2,3,7,8} for any actor, plus {12,13,14,15} for players. `0x1409ae170` returns status 0x15 ("use raw target") when the path grid is off or missing. **Confirmed**. What the numbers mean (metres or cells, which cell types are water or shroud): **Guess**. It snaps to the nearest walkable path-grid cell; it is not a raycast to the terrain surface.

**Confirmed in game** (player on open ground, offsets from the surface): +4, +8, −3, −6 and −10 m snapped to the
surface (x/z moved by up to 1.5 m, 0.5 m steps); **+16, +30, −16 and −20 m found nothing and used the raw target** (a
16 m and a 30 m fall killed the level-1 character; −16/−20 m left it inside the terrain until the emergency teleport).
So it corrects only about ±10 m vertically, silently. It waits for the destination to stream (12-32 ticks at 1 km).
An NPC, which gets no hot point, also got its search answered (10 ticks, 150 m away).

---

## 2. Triggers EMF could use

| Option | What it needs | Risks | Verdict |
|---|---|---|---|
| **(a) An event or action EMF can already send** | `TeleportToTargetEvent` is not an ECS event (no). `InteractionAcceptedEvent` on a `TeleportZone` only goes to the linked zone. Writing `PlayerInput.teleportAction` / `adminTeleport` with a new version: the next network input from the client overwrites it, and the consumer fires on any version change (Guess: a second, stray teleport). | Not arbitrary positions; races with network input; admin-only for `adminTeleport`. | No |
| **(b) EMF's own server program pushes the add record** (as `actor_teleport` and `admin_player` do) | `QueuePush` signature. A request queue drained by an internal program; `cmdbuf = *(job+8)`. No descriptor declaration is needed for the add. | Must check the target has `CurrentTransform`, `LastTransform`, `TeleportCount` and no `Teleport` (else a stuck component freezes a player). Buffer full → retry next tick. | **Recommended** |
| (c) Call `addComponentNow` directly | Signature `0x1409559e0`. Server thread, before SimStep or after `waitForWorldJobs` (where EMF spawns and destroys). | Bypasses the queue's ordering. Same checks as (b). Likely safe; the flush calls it on the same thread. | Fallback only |
| (d) Write `CurrentTransform`/`LastTransform` and bump `TeleportCount` (v16 `program_write_other`, no new signature) | Mirror step 6 of #701 with `markWritten`. | No hot point (destination may not be streamed: fall through terrain), no ground search, no fall-damage reset (lethal fall after an upward move), glider/climb/NPC state not reset, no "not ready" window. Without the counter bump the client slides across the map. | Last resort, for entities without `TeleportCount` |

Rubber-banding: `ClientPlayerInputData` (952 B) holds only actions, sticks, camera, cursor and button bits. No position. **Confirmed**. The server owns the position; the client's prediction is corrected to it. **Likely** (vanilla fast travel and admin teleport depend on it).

---

## 3. Server and client

- Write path: `teleport` runs on the server world only (flag 0x10). On a dedicated server it runs in `enshrouded_server.exe` (same code: the program, `QueuePush`, `addComponentNow`, `FlushCmdBuffers`, the ground search and 14 `Teleport` sites all found). **Confirmed**.
- `Teleport` and `LastTransform` are `server_only`: never replicated. **Confirmed** (attributes).
- `CurrentTransform` and `TeleportCount` are replicated (written by a server program, read by a client program). **Confirmed** by the rule in `re_custom_programs.md` §3 plus the access lists, and in game (below).
- Client: `render_transform_with_teleport` (client program, `0x14030fdf0`): when `TeleportCount.counter != RenderTeleportCount.counter` it copies the counter, sets `wasTeleported`, resets the render history to the new `CurrentTransform` and resets `HermiteInterpolation`. So the model and camera snap. **Confirmed**. `player_camera_control` reads `RenderTeleportCount` too.
- **Confirmed in game**, client world: in the hosted game the local player's `CurrentTransform`,
  `TeleportCount` and `RenderTeleportCount` all changed in one client tick (a one-tick jump, no slide). On the
  **dedicated server** the joined client also jumped in one tick, 44 ms after the server push (6 m hop), then walked
  10 m with no snap-back. After a 1 km move the joined client got the new position **one tick before** the new
  `TeleportCount` (counter and `RenderTeleportCount` changed the next tick): at most one frame interpolated. The
  destination was drawn within about 2 s, with no loading screen and no fall.
- Fast travel also sets a replication checkpoint for the player (kind 0x10, 30 ticks). Admin teleport does not. **Confirmed**. **Guess:** it makes the client wait for a consistent snapshot of the destination (the loading screen). EMF can push it as an option.
- Saving: `Teleport` is `do_not_save`. A save during a pending teleport just drops it. **Confirmed** (attribute).

## 4. NPCs

- Villagers and animals use the same component: `0x1404488c0` (teleport to a transform when farther than 0.3 m, optional 3.5 m cap), `0x140448b80` (to a float3, keeps the current orientation, scale 1, `searchBestSpawnPosition` default true), the NpcCommandEvent handler `0x14044f3c0`, `animal_behavior` `0x14022ad20`. **Confirmed**.
- So villagers and animals have `TeleportCount` and `LastTransform`. **Likely** (the game's own adds would leave a stuck component otherwise); confirmed in game (below).
- Enemies: one regular enemy checked in game (below), not every kind. EMF checks each entity and refuses one without them.
- **Confirmed in game:** villagers (1029, 1030) and an enemy (`Enemy01BehaviorState`, 6905) have both; the
  flame altar (it has `PlayerSpawnPoint`), chests, bushes and a spawned barrel have neither. Around the server's
  starting vault (400 m): 22 entities with `TeleportCount`, all 22 also with `LastTransform`; 8 entities with an
  `Actor` but without them. A villager moved 5 m kept falling under physics (off a ledge) and a villager brought 250 m
  to the player walked about afterwards (AI kept running). **An enemy moved 150 m away from the player was removed by
  the game 0.6 s after the move** (the area around players only; #701 pushes no hot point for NPCs).
- `NpcCommandEvent` type 0 (Reset) reaches `0x1404488c0` through `0x14044f3c0` (teleports a villager home when far). **Confirmed** (call). It is no general "go to x,y,z".

---

## 5. Design for G5

### 5.1 C API (v20; v19 is the chat and save API)

```c
#define EMF_TELEPORT_FIND_GROUND  0x01u  /* searchBestSpawnPosition: nearest walkable path-grid cell, found only within
                                            about 10 m of y (confirmed in game); farther = the raw target, silently */
#define EMF_TELEPORT_FACE_YAW     0x02u  /* use `yaw_deg`; otherwise keep the entity's current orientation */
#define EMF_TELEPORT_CHECKPOINT   0x04u  /* players: also set a replication checkpoint, as fast travel does */

typedef void (*EmfTeleportCallback)(EmfWorld* world, uint32_t entity, bool moved,
                                    float x, float y, float z, void* user);

typedef struct EmfTeleport {
    uint32_t size;          /* sizeof(EmfTeleport) */
    uint32_t entity;
    float x, y, z;          /* metres, world space (y up) */
    float yaw_deg;          /* with EMF_TELEPORT_FACE_YAW */
    uint32_t spawn_point;   /* optional: an entity with PlayerSpawnPoint (flame altar, spire): the game picks the
                               landing pad (entity-1) % 5 around it; x/y/z = that entity's position */
    uint32_t flags;
    EmfTeleportCallback cb; /* after the move (moved = true, final position) or the give-up (moved = false) */
    void* user;
} EmfTeleport;

/* SERVER world only. True = queued; it happens within 1-3 server ticks (longer while FIND_GROUND waits for terrain).
 * False (logged) when refused: not the server world, bad entity or numbers, no CurrentTransform/LastTransform/
 * TeleportCount, a teleport already pending, 64 requests waiting, or the feature is off on this build.
 * Any thread. cb runs on the server thread after a step (spawn/destroy allowed there). */
bool (*teleport)(EmfWorld* world, const EmfTeleport* request);
```

Lua mirror (Lua 1.6): `emf.teleport(world, id, x, y, z [, {ground = true, yaw = deg, spawn_point = id, checkpoint = false}] [, fn(moved, x, y, z)])` returns true when queued. `world:position(id)` already exists for reading back.

### 5.2 Implementation

- New internal server program `emf_teleport` (flags `0x4 | 0x10`, server world). other-read: `CurrentTransform, LastTransform, Teleport, TeleportCount`. `kInternalPrograms` 3 → 4.
- `teleport()` validates the numbers on the caller's thread: finite floats; x and z in 0..10240 (the EntityGrid is 160 cells × 64 m and logged positions look like (3704, 905, 1171); the origin is a **Guess**); y in -1000..5000 (**Guess**); entity ≠ 0. It queues the request.
- In the program's update (server worker thread, inside SimStep; the job's command buffer is exclusive to this slice):
  1. Read the target through the accessors. Refuse if `CurrentTransform`, `LastTransform` or `TeleportCount` is missing. Wait (keep it queued, max 2 s) if `Teleport` is already there.
     **Added after the in-game test:** refuse a **dead** entity (a dead player moved into the terrain fell
     through the world and the emergency teleport could not help until respawn); refuse a second request for an entity
     already pushed and not yet moved, **including the same tick** (two kind-3 records in one tick: the first wins, the
     second is dropped silently by `addComponentNow`).
  2. Build `Teleport`: position = `round(m × 2^32)` (the game does `trunc(m × 2^32 + 0.5)`); orientation = current (or a yaw quaternion); **scale = the current scale** (#701 writes scale too); `targetEntityId` = `spawn_point` or 0; `delayEndTime` 0; `applied` 0; `searchBestSpawnPosition` = FIND_GROUND.
  3. `r = QueuePush(cb, cb + 0x10, 3, 0x6C + 0x50, entity)`; NULL → retry next tick. Else `r+0xC = 0x23384d40`, `r+0x4C = (u16)0x50`, copy 80 bytes to `r+0x6C`.
  4. CHECKPOINT and entity 1..1023: `r2 = QueuePush(cb, cb + 0x10, 0x10, 0x10, entity)`; `r2+0xC = entity`.
  5. Remember `TeleportCount.counter`. Each tick: counter changed → `cb(moved = true, CurrentTransform)`. Still pending after 10 s (600 ticks) → push a kind-4 record (`QueuePush(cb, cb+0x10, 4, 0x4C, entity)`, `+0xC = 0x23384d40`), `cb(moved = false)`, log it. This frees a player stuck "not ready".
- Fail-soft: without the `QueuePush` signature, or if the program is not running in this world, `teleport()` returns false and logs once. A joined client has no server world: false.
- Optional fallback (c): when the program is missing, call `addComponentNow(storage, entity, 0x23384d40, {&tp, 80})` from the server post-step (after `waitForWorldJobs`). Only behind its own signature.

### 5.3 Signatures (each checked unique in `.text` of both 1273043 exes with `tools/re/sig.py`)

| Name | Client VA (RVA) | Server VA | Pattern | Use |
|---|---|---|---|---|
| `QueuePush` | `0x140953670` (`0x953670`) | `0x1405f3bb0` | `48 89 74 24 10 57 48 83 EC 20 44 8B 52 10 48 8B FA 41 8B C2 83 E0 0F 74 07 44 2B D0 41 83 C2 10` | **required** for G5 |
| `AddComponents` | `0x140954270` (`0x954270`) | `0x1405f47b0` | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 30 49 8B 58 08 33 ED 41 BD` | optional (for a generic `add_component`) |
| `AddComponentNow` | `0x1409559e0` (`0x9559e0`) | `0x1405f5f20` | `48 89 5C 24 20 44 89 44 24 18 89 54 24 10 55 56 57 41 54 41 56 B8 90 28 00 00` | optional fallback (c) |
| `FlushCmdBuffers` | `0x140a17490` (`0xa17490`) | `0x1406b8640` | `40 53 41 54 48 83 EC 38 45 33 E4 48 8B D9 41 8B D4 44 38 A1 18 DC ED 00` | diagnostics only |
| `IsAdmin` | `0x1403b4390` (`0x3b4390`) | `0x140160330` | `8D 41 FF 4C 8B C2 3D FE 03 00 00 77 1A 48 8D 14 80 48 C1 E2 05 42 39 0C` | optional: `BasicPlayerInfos[entity-1] +0x9D bit 0` = game admin |

Program update functions (for hooks or logging; EMF finds them by name, as `programs.py` does): `teleport` `0x14027b4e0` / server `0x14008dc20`; `actor_teleport` `0x1402707f0` / `0x140083340`; `admin_player` `0x1402cafd0` / `0x1400b82c0`; `teleport_interaction` `0x140347a20` / `0x140108390`.

Code offsets EMF should record (and check on a known build): `cmdbuf = *(job+0x08)`; `buf = cmdbuf+0x10`; record `+0x0C` hashes, `+0x4C` sizes, `+0x6C` data; Actor ready flag `Actor+0xBF8` bit 50.

---

## 6. In-game test plan (about 30 min of game time)

**Results in section 8.** Note: in the LOCAL test world, 1025 is the flame altar (it has `PlayerSpawnPoint`) and 1026/1027 are chests; the villagers were 1029 and 1030 (ids change per session).

Tool: a C++ test mod (mod id `tps`, not packaged). It finds `QueuePush` with the v14 pattern API, registers a server program with other-read `CurrentTransform, LastTransform, Teleport, TeleportCount`, and takes console commands `tps.<cmd>`. Keep a "home" position (`tps.mark`) to return to.

Log every tick for 3 s after each request, to `emf_core.log`: tick; `Teleport` present / `applied`; `TeleportCount.counter` and `lastTeleportTime`; `CurrentTransform` and `LastTransform` positions; Actor +0xBF8 bit 50; `cmdbuf`, its capacity (`+0x18`), used (`+0x20`) and count (`+0x24`). After each run, copy `enshrouded.log` too (look for `Player EntityId … teleported From … to …`, `Could not add component`, `Hot point already set`, `Replication Checkpoint`).

**Local world (a LOCAL test character and world; ~15 min)**
1. `inspect 1`: has `CurrentTransform`, `LastTransform`, `TeleportCount`; no `Teleport`. `inspect` a villager (1025/1026) and a nearby enemy: note which have `TeleportCount`/`LastTransform`.
2. `tps.hop 10 0 0` (no ground search). Pass: `Teleport` appears at tick N+1, the position equals the target at N+1, counter +1, `Teleport` gone by N+3, the game log line, the camera snaps without a slide, no damage.
3. `tps.hop 0 30 0` then `tps.hop 0 30 0 ground`. Pass: the first falls and takes fall damage from the new height only; the second lands on walkable ground near x/z (log where).
4. Dig a 2 m hole at the altar, stand in it, `tps.hop 0 -3 0 ground` and `tps.hop 3 0 0 ground`. Pass: lands on walkable ground, not inside terrain.
5. Far: `tps.to <x> <y> <z> ground` about 1 km into unexplored land, then `tps.home`. Log how many ticks the search waits. Pass: arrives, does not fall through, controls work after (bit 50 set again). Repeat with `checkpoint`: look for "Replication Checkpoint … Set" and "… Reached".
6. `tps.npc 1025 5 0 0`: the villager moves, keeps its AI (state changes within 30 s), `Teleport` is gone.
7. Same tick twice on player 1: the second is refused or ignored; no crash, one move.
8. A spawned barrel (no `TeleportCount`): EMF refuses. Then force it once (`tps.force`): `Teleport` stays; after 10 s the watchdog's kind-4 record removes it.
9. Bounds: log `pos` at a few places to confirm x/z stay in 0..10240.

**Dedicated server (~15 min; the server lists itself publicly, so get an OK first)**
10. Server with EMF + the test mod; join with the test character. From `commands.txt`: `tps.hop #1 10 0 0`. Pass: the joined client snaps once; walk for 10 s: no snap back; the server log shows the "teleported From" line.
11. `tps.to #1 <far> ground checkpoint`: watch for pop-in, falling, or a loading screen. Pass: arrives and plays normally.
12. `tps.bring #1 <villager>`: the player lands next to the villager.

**Fail signs:** the client returns to the old spot within a second (rubber-band); bit 50 stays clear (stuck "not ready"); falling through terrain; `Could not add component 'Teleport'` in the game log; a crash in `0x140a16020` or in the test mod's program.

---

## 7. Not settled statically

- ~~Whether a joined client's prediction ever fights the snap~~ **Confirmed in game:** it does not (section 3).
- What `PathGridSystem` cell types 2/3/7/8/12-15 are, and the search radius unit. (In game: about ±10 m vertically, section 1.4.)
- Exactly what the replication checkpoint does on the client. (In game: no visible difference with or without it.)
- ~~Which enemies have `TeleportCount` and `LastTransform`~~ **Confirmed in game:** a regular enemy has both (section 4); not every kind was checked.
- `delayEndTime`: no reader found in #701; others not checked. (In game: 0 always worked.)
- ~~The command buffer's capacity~~ **Confirmed in game:** 1 MiB, shared with other programs (section 1.2).
- The world's x/z origin and size (EntityGrid suggests 10.24 km). Positions seen in game: 3619-4748 x, 1160-1434 z, y 677-905: inside 0..10240, origin unproven.

---

## 8. In-game test results (Confirmed in game)

Test mod: mod id `tps` (not packaged). It uses only
the public API: `find_pattern` (QueuePush), a server program with other-read `CurrentTransform, LastTransform, Teleport,
TeleportCount` (v8), commands, `on_tick`. It reads `job = *ctx`, `cmdbuf = *(job+8)` itself. **No core hook was added.**
Tested in a hosted game (a LOCAL test character and world) and on the dedicated server (joined with the same
character), with the `emf_core` and game logs and screenshots.

### 8.1 Local world (hosted game)

| Step | Result | Details (`#n` = request number in the log) |
|---|---|---|
| 1 inspect | pass | player 1: CT, LT, TC (counter 0), no `Teleport`, bit 50 set; villagers 1029/1030 and enemy 6905 have CT+LT+TC; altar 1025, chests, bushes, barrel have no LT/TC |
| 2 hop 10 0 0 | pass | #1: push tick +0 `Teleport` applied 0; +1 moved exactly, counter 0→1; +2 gone; bit 50 back at +26; hp 60→60; client: one-tick 10.00 m jump, `TeleportCount`/`RenderTeleportCount` 0→1 in the same tick; game log line |
| 3 hop 0 30 0 / ground | pass / **not as hoped** | #2: moved +1, fell 30 m from the new height, died on landing. #3 `ground`: search waited 9 ticks, then used the raw target (still 30 m up): fell and died |
| 4 inside terrain (no digging: targets 2-3 m under the surface) | pass with `ground` | #4 −3 m: lifted to the surface (+10 ticks); #5 3 m sideways, −2 m: surface, x +0.5 m; #6 −3 m **without** `ground`: left inside the terrain, the game's emergency teleport moved it to the altar pad ~1.6 s later. Bracket #17-#25: works at +4/+8/−6/−10 m, raw target at +16/−16/−20 m |
| 5 far (1 km east) | pass with a right y | #26 y 760 `ground`: waited 12 ticks, raw (ground is at 698): 62 m fall, died, **did not fall through**. #27 y 704 `ground checkpoint`: waited 32 ticks, landed standing at (4737.25, 698.47, 1426.25); checkpoint Set/Reached; bit 50 back after ~150-175 ticks; walked (off a cliff: the spot is a ridge). #29/#30 far and `tps.home` back: clean |
| 6 villager | pass | #7 1029 +5 m: moved +1, gone +2, then fell 6.7 m off its ledge; #8 1030 brought 250 m beside the player (`ground`): walked about over the next 30 s |
| 7 twice in one tick | pass | #9/#10: the test mod's guard refused the second. #11/#12 `raw`: both records pushed in the same tick; one move to the **first** target, counter +1, no error line, no crash |
| 8 barrel | pass | #14 refused (no LT/TC); #15 forced: `Teleport` stayed `applied` 0 (no move) until the watchdog's kind-4 record at +600; gone at +601. Barrel destroyed after |
| 9 bounds | pass (weak) | all positions inside 0..10240 (section 7) |
| extra: enemy | moved, then despawned | #13 6905 150 m away from the player: moved +10, then the game removed the entity at +49 |
| extra: dead player | **bad** | #22/#23 and #28 moved a dead player (not refused by the test mod): into the terrain it fell through the world, the emergency teleport fired every tick without effect until respawn |
| extra: yaw=180 | works | #31: the body turned 180°, the camera did not |

### 8.2 Dedicated server (enshrouded_server.exe, joined from this PC)

| Step | Result | Details |
|---|---|---|
| 10 hop #1 (6 m along the vault floor; from the server console through the debug bridge, same as `commands.txt`) | pass | server #1: moved +1, gone +2; joined client: one-tick 6.00 m jump 44 ms after the push, counters 0→1; walked 10 m, no further jump; server log "teleported From" line |
| 11 far + ground + checkpoint | pass | server #2: waited 14 ticks, landed at (4737.25, 698.47, 1426.25); checkpoint Set (push tick) / Reached (+43); client: the position arrived one tick before the counter; destination drawn within ~2 s, no loading screen, no fall |
| 12 bring | pass (adapted) | the server's world has no villagers: `tps.bring #1 2000` (the vault's flame altar) landed 0.7 m from the aim point. Extra: `spawn=3825468238` (a `PlayerSpawnPoint` 188 m away) landed on its landing pad, 2 m off (pad 0 for entity 1). Arriving there unlocked recipes for the character (exploration side effects happen) |
| client side | refused | `tps.*` typed into the joined client: "commands need a world you host" (no server world on a client) |

### 8.3 What G5 needs beyond section 5

1. **Refuse dead entities** (and players in the death/respawn state).
2. **FIND_GROUND is not "land on the ground"**: it corrects only about ±10 m and otherwise uses the raw target without
   telling anyone. `/unstuck` should aim at a known safe point (the player's spawn point with `spawn_point`, which the
   game lands on its pads) or a recorded position; arbitrary x/z needs a ground height from somewhere else. The
   callback should report the final position (and whether it differs from the target) so a mod can tell.
3. **Same-tick duplicates**: track pending entities inside the program, not only "has `Teleport`".
4. **NPCs far from players**: enemies moved out of the players' area are removed by the game within a second; keep NPC
   teleports near a player (or document it). Villagers survived.
5. Report "ready again" (bit 50) separately from "moved" if a mod wants to act after the player can play (20-175 ticks).
6. Use the world tick, not `job+0x50`. The command buffer is shared and big (1 MiB): QueuePush NULL is unlikely, the
   retry is still right.
7. No new core hook is needed for the buffer: the internal program reads `*(*(ctx)+8)` like the test mod did.

---

## 9. The built API, and two more answers for Waypoint Teleports

The API is `teleport` (C API v20, `src/core/teleport.h`; [reference.md](../modding/reference.md) v20), with the
section 8.3 needs: dead entities and pending duplicates refused, the final position and off-target / raw-target flags in
the callback, a READY notice, the 10 s watchdog with the kind-4 removal, `QueuePush` in the signature table. Two facts
the mod needed came from a static look at build 1273043 (client addresses; both are in the server exe too):

**The player's own spawn point = `SelectedTargets.targets[9].targetEntityId`** (component offset `+0x52C`).
**Confirmed** in code: the emergency teleport (`0x1402ca15c..0x1402ca222`) does exactly
`spawn = (t9 != 0 && entityExists(t9)) ? t9 : ServerPlayers[P-1] +0x14` (`entityExists` = `0x14099d7f0`), copies it into
`targets[1]` and starts sequence `0x7bb17afe`. Writers: `player_update_important_locations` (`0x1402e4b30`, every tick,
write at `0x1402e4dec`) sets slot 9 to each `PlayerSpawnPoint` + `TriggerShape` entity whose area the player stands in
(not `NoRespawn`, not in fog type 3); `update_player_spawn` (`0x1402ec3e0`) refills it only when it is 0 or names a
destroyed entity, from `ServerPlayers[P-1] +0x14` (the spawn entity resolved when the player joined, `0x140e3b630`;
`ServerPlayers` is not a world singleton EMF reads). `emf::player_spawn_point` reads slot 9 and checks the entity still
has `PlayerSpawnPoint`; the mod falls back to the nearest flame altar. **Confirmed in game** (the LOCAL test world): slot 9
= 1025, the flame altar (`PlayerSpawnPoint.target` 2, Base), also 1 km away; after a respawn slot 1 held it too.

**Inside the shroud = `EnvironmentSensor.fogType >= 2` and actor state flag 14 (`InSaveZone`) clear.** **Confirmed**:
vanilla's fast-travel ban `0x140e793d0(out, fogType, state)` (server `0x14092d570`), called by
`player_control_action_sequence` at `0x1402d8942` (server `0x1400c5c32`), blocks when InSaveZone is clear and the fog
type is not 0/1 (also while UnderWater, bit 44, or Falling, bit 32); a non-zero mask drops the `teleportAction`.
`environment_sensor` (`0x1403305c0`, server world) writes `fogType` (players every tick); `player_update_insave_zone`
(`0x1402e5020`) toggles bit 14 inside the trigger shape of the slot-9 spawn point. The state is `Actor.currentState`, or
while an action sequence plays (`Actor+0x1B1 & 1`) `(state | Actor+0xBD0) & ~Actor+0xBD8`. A second signal, Actor bit 13
`InFog` (written only by `fog_resistance_update` `0x1402d0ad0`), is the "shroud timer drains" flag (clear while dead or
under water). `FogResistance` current < max is also true while the timer refills outside: not used. **Confirmed in
game** outside the shroud only: `fogType` 0 at the altar and 1 km away, bit 14 set at the altar and clear 1 km
away; a place inside the shroud was not visited.

**QueuePush and faults.** QueuePush reserves the record (bumps `used` and `count`) before it zeroes
and fills it. A fault inside it after the reservation can therefore leave a half-written record in the buffer, which
the flush then applies as it finds it; EMF cannot undo that. It turns teleport off at the first fault (in the add or in
the checkpoint push), keeps watching what it pushed, and logs the entities whose `Teleport` it can no longer remove.

**Also confirmed in game:** a player in the server world has `Health` but no `NetworkHealth` (EMF's dead check reads
the health attribute); a player put 3 m inside the terrain gets the emergency teleport's `W` line 0.48 s later and its
`Teleport` to the spawn point 1.05 s later, even when another teleport moved the player out in between.
