# AI: villager ActiveNpcState and enemy behavior (Tier 2.3)

Build 1269579, image base 0x140000000. Static analysis only (Ghidra via rea + local scripts).
Signatures come from `sig.py` (rel32 and RIP displacements wildcarded) and start at the function entry.

Summary:
- The villager AI is a classic enter/update/exit state machine. It runs per entity inside the ECS program `npc_active_behavior`.
- Everything a handler touches goes through one big per-entity context struct (`ctx`, about 0x8C0 bytes, on the stack of the update fn). The component pointers in it have fixed offsets.
- State changes always go through `SetState` (`0x140453cf0`). "Task finished" and "task failed" go through a small transition table in .data (`0x141fd2780`).
- The enemy AI (`enemy01`) has the same shape, but it is driven by conditions: a (state x condition) table at `0x141fd1e20`.

---

## 1. How the program iterates entities and gets component pointers

This is the key to reading villager state live.

### Iterator layout (generic, all programs)
`iter_begin(ProgramContext* pc, u64* iter, size)` = `0x1409bab60` fills a qword array:

| iter index | content |
|---|---|
| `[0]` | `ProgramContext*` |
| `[1 .. nTpl]` | template component pointers for the current entity (1 qword each) |
| next `nRead` | read component pointers (1 qword each, NULL if an optional component is missing) |
| next `nWrite*6` | write components, 6 qwords each: `{data*, chgMask1*, chgMask2*, x, u16 id + u8 flag, tracker*}` |
| then | singletons (sys-read/sys-write), then accessors for other entities' templates, then other-entity read/write accessors, then event readers |

**Order inside each group:** the required and optional lists from the program descriptor are merged and sorted alphabetically by base name ("Dynamic" prefix ignored).
- Example: the reads of `npc_active_behavior` are DynamicAnimal, CurrentTransform, InteractionOfferRuntime, IsTame, Movement, MovementCollider, MovementVector, DynamicShelter, Teleport, Velocity.
- This was verified from field offsets in 6+ places: Shelter `+0xd/+0xe/+0xf`, IsTame null check, `SteeringBehavior_Separation.enabled +0xc`, `NpcState.turnSetup +0x20`, `NpcState.minDistanceToPlayer +0x7c`, Teleport null check.
- Confidence: High.

`iter_next(pc, iter, size)` = `0x1409b5a50`. ProgramContext `+0x00` = batch/query descriptor `Q`, ProgramContext `+0x08` = u32 index of the current entity.
- **Entity 0:** it copies the column base pointers from `Q+0x1c8`. That is an array of 16-byte column descriptors `{u64 base; u16 stride @+8; u16 @+10; u8 trackA @+12; u8 trackB @+13}`, reads first, then writes.
- **Each following entity:** it adds `stride` to every pointer, and adds 4 to the change-mask pointers when tracking is on.
- **Templates:** per-entity pointer arrays at `Q+0x470+k*8`, indexed by entity index (entities in one batch can have different templates).
- **Counts:** `Q+0x4b0` = nTpl (u8), `Q+0x4b8` = nRead (u16), `Q+0x4ba` = nWrite (u16), `Q+0x830` = entity count, `Q+0x1c0` = change-tracking enabled. `Q+0x7f8` / `Q+0x808` = change-mask bases.
- **Result:** components are stored SoA. The data of entity i = `column.base + i * sizeof(component)`, and it is contiguous inside a batch (chunk).
- **Calls:** the program update fn is called once per batch and loops over the entities itself. So `0x140456790` is effectively per chunk, and `0x1404552c0` is **per entity** (not per chunk).
- Confidence: High for the per-entity pointer math. Medium that one ProgramContext equals one archetype chunk.

**Write rule:** before writing through a write pointer, the engine calls `0x1409c5df0(tracker, a,b,c,u16,u8)` (the change-tracking "mark dirty" call; it has 1090 callers) if `tracker != NULL`. A mod that writes components directly should do the same, or replicated/derived state may go stale.

### Villager per-entity context (`ctx`, built in `0x140456790`, passed to `0x1404552c0`)
Byte offsets (`ctx[n]` = qword n):

| ctx offset | content |
|---|---|
| `+0x000` | `ProgramContext*` (pass to the ECS helpers below) |
| `+0x008..0x11f` | locomotion helper built by `0x14044a440`. Holds: spawnPos copy `+0x14` (float3), `ActiveNpcState.movementSetup*` `+0x20`, `NpcState.turnSetup*` `+0x28`, `pathNodeSetup*` `+0x30`, `roamingSetup*` `+0x38`, Actor* `+0x40`, ActorInput* `+0x48`, DynamicLocomotion* `+0x50`, DynamicPathWalker* `+0x58`, DynamicSteeringLocomotion* `+0x60`, NpcCommon* `+0x68`, SteeringAgent* `+0x70`, PathWalker tpl* `+0x78`, SelectedTargets* `+0xb0`, CurrentTransform* `+0xb8`, MovementVector* `+0xc0`, Movement* `+0xc8`, Velocity* `+0xd0`, Separation* `+0xd8`, Teleport* `+0xe0` |
| `+0x120..0x15f` | singletons: PlayerBasesSystemServer, VoxelWorld, ColliderSystem, PathGridSystem, VoxelGrid, **Daytime `+0x148`**, ItemRegistry, AttributeRegistry |
| `+0x160..0x1af` | reads: DynamicAnimal `+0x160`, **CurrentTransform `+0x168`**, InteractionOfferRuntime `+0x170`, IsTame `+0x178`, Movement `+0x180`, MovementCollider `+0x188`, MovementVector `+0x190`, DynamicShelter `+0x198`, Teleport `+0x1a0`, Velocity `+0x1a8` |
| `+0x1b0..0x44f` | 14 write entries, 0x30 bytes each (`+0` data*, `+0x28` tracker*): **DynamicActiveNpcState `+0x1b0`**, **ActiveNpcStateSaved `+0x1e0`**, Actor `+0x210`, ActorInput `+0x240`, InteractionAttachmentGuest `+0x270`, DynamicLocomotion `+0x2a0`, DynamicNetworkLocomotion `+0x2d0`, **NpcCommon `+0x300`**, DynamicNpcState `+0x330`, DynamicPathWalker `+0x360`, SelectedTargets `+0x390`, SteeringAgent `+0x3c0`, SteeringBehavior_Separation `+0x3f0`, DynamicSteeringLocomotion `+0x420` |
| `+0x450` | **`ActiveNpcState*` (template, 456 B)** |
| `+0x458` / `+0x460` / `+0x468` / `+0x470` | MovementColliderConfig* / NpcActionSequences* (opt) / NpcState* / PathWalker tpl* |
| `+0x478..0x4cf` | other-entity accessors: DynamicActiveNpcTarget `+0x478`, CurrentTransform `+0x480`, DespawnLink, InteractionAttachment `+0x490`, InteractionOfferRuntime `+0x498`, NpcBed, DynamicNpcSocializing, PlayerInput, ServerCursor; writable: DynamicActiveNpcTarget `+0x4c0`, NpcBed `+0x4c8` |
| `+0x4d0` | ActiveNpcTarget template accessor (other entities) |
| `+0x4d8` | NpcCommandEvent reader |
| `+0x4e0` | per-entity RNG (seeded each entity by `0x1409bb060`) |
| `+0x4e8..` | the iterator itself |

So inside any hook that receives `ctx`:
```
state    = *(u8*)(*(u64*)(ctx+0x1b0) + 0x00);   // DynamicActiveNpcState.state
decision = *(u8*)(*(u64*)(ctx+0x1b0) + 0x02);
tpl      =  *(u8**)(ctx+0x450);                 // ActiveNpcState (npcId +0, npcType +0x34, decisionSetup +0x38)
common   =  *(u8**)(ctx+0x300);                 // NpcCommon (currentTargetId +0x40, bedId +0x48, closestPlayerDistance +0x58)
```
Confidence: High.

### Generic ECS helpers seen here (useful for the framework, Medium)
- `0x1409b8030(pc, &out)`: current time (`Time`, ns).
- `0x1409b8010(pc, &out)`: frame delta time.
- `0x1409b2e40(pc, &out)`: EntityId of the current entity.
- `0x1409a58c0(pc, entityId)`: entity handle (NULL if gone).
- `0x1409b29a0(&out, entity, accessor)`: other entity's component pointer.
- `0x1409b3100(entity, tplAccessor)`: other entity's template pointer.
- `0x14099c410(pc, entityId)`: entity exists.
- `0x140228350(&out, rng, {float* w, count})`: weighted random pick. `out[0]` = failed, `out+8` = chosen index.

---

## 2. Villager state machine

### Top level
**`npc_active_behavior` update (per batch)**
- Address `0x140456790`, signature `48 89 5C 24 08 55 48 8D AC 24 30 F8`
- Builds `ctx` and the RNG. On first run it initializes the transition table at `0x141fd2780` (guard byte `0x141fd26c4`). Then for each entity it calls `0x14044a440` (locomotion helper) and `0x1404552c0`.
- Confidence: High.

**Per-entity worker**
- Address `0x1404552c0`, signature `40 53 48 83 EC 60 48 83 B9 60 01 00`
- Skips entities that have DynamicAnimal and no IsTame. Handles NpcCommandEvents (`0x14044f040`) and bed bookkeeping (`0x140450540`).
- If actor flag `0x32` is not set (`0x14021a900(actor,0x32)`, meaning unknown, maybe "active/simulated"), or `NpcCommon.closestPlayerDistance >= 150` m: it pauses. It clears the path, sets the socializing state to 0, and sets a random resume delay of 1-10 s in `NpcCommon.resumeUpdateTimeOffset`.
- Otherwise it calls PreUpdate `0x1404518d0`, CheckInterrupts `0x14044b3f0`, **RunStateMachine(ctx, 1)** `0x14045e650`, and PostUpdate `0x1404510b0`. Afterwards it updates the happiness bits in `ActiveNpcStateSaved+0xe`.
- **Best per-frame read hook.** Confidence: High.

**RunStateMachine(ctx, phase)**
- Address `0x14045e650`, signature `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 60 0F B6 EA 48`
- Phase values: 0 OnEnter, 1 OnUpdate, 2 OnExit, 3 interrupt/stop (handled by Sitting, Interacting, FindBed), 4 resume.
- In OnUpdate:
  - If `ActiveNpcStateSaved.overrideRoamingRange && roamingRange==0` ("stay" order), it stops the current task once (`wasPaused` +0x79).
  - It adds dt to `NpcCommon.currentStateDuration` (+0x18).
  - If the villager is farther than max(targetRadius, bedRadius)+1 from its spawn, it calls `0x1404543c0` (teleport home, Medium) and goes to Roam.
  - Then it calls the dispatcher.
- Confidence: High (leash detail: Medium).

**StateDispatch(ctx, phase)**
- Address `0x14044adf0`, signature `48 89 5C 24 10 57 48 83 EC 30 48 8B F9 0F B6 DA 48 8B 89 D8`
- A `switch` on `DynamicActiveNpcState.state` (byte +0). The table below lists the handlers.
- Confidence: High.

**SetState(ctx, newState)**
- Address `0x140453cf0`, signature `48 89 5C 24 10 48 89 6C 24 18 57 48 83 EC 30 48 8D B9 B0 01`
- If the state changes:
  - It runs `RunStateMachine(ctx,2)` (exit), then resets the path walker (`0x1403b6a00`, `0x1403b6990`).
  - It sets `previousState = state` (skipped when the old state is FinishTask), then `state = new`.
  - It resets the NpcCommon timers (`0x140446fc0`), then runs `RunStateMachine(ctx,0)` (enter).
- Confidence: High.

**FinishState(ctx, StateCondition)**
- Address `0x1404546a0`, signature `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B D9 0F B6 F2 48 8B 89`
- Looks up `next = *(u8*)(0x141fd277e + state*2 + cond)` (cond 0 = IsFinished, 1 = HasFailed).
- On HasFailed it does `NpcCommon.taskFailures++` (+0x68e). If `next != 0` it calls SetState.
- Confidence: High.

**SetDecision(ctx, Decision)**
- Address `0x1404532a0`, signature `48 89 5C 24 08 57 48 83 EC 30 48 8B D9 0F B6 FA`
- Writes `DynamicActiveNpcState.decision` (+2) and clears `NpcCommon.taskFailures`.
- Confidence: High.

### State handlers (from the switch in `0x14044adf0`)

| # | State | Handler | What it does (short) |
|---|---|---|---|
| 1 | Start | `0x14045e3c0` | Rolls NpcCommon.sleepTime/wakeUpTime from `sleepSetup` (tpl +0xa8..+0xb4), sets spawnPosition to the current position if it is zero, and finishes once the bed search is done |
| 2 | FindBed | `0x14045c3a0` | Targets `NpcCommon.bedId`, or searches for a bed near spawn (`0x14044c650`, radius `searchTargetSetup.bedRadius` tpl +0xbc). Goes to GoToTarget (6), or to Sleep (8) if there is no bed and the decision is WantsToSleep |
| 3 | Idle | inline | Enter: `stateTimer = rand(idleSetup.min,max)` (tpl +0x48/+0x50). Update: counts down, then Finish(0) |
| 4 | DecideTask | `0x14045bab0` | Picks the next task (see 2.2) |
| 5 | FindTarget | inline + `0x14044d980` | Async range-query target search (see 2.3). Gives up after 1 s |
| 6 | GoToTarget | `0x14045c850` | Paths to `NpcCommon.currentTargetId` (`0x14044e670`). On arrival it maps the decision to a state: SitDown→Sitting 9, Interact→Interacting 10, Socialize→NpcSocializing 11, Sleep→Sleep 8, Eat→Eating 12, Idle→Idle 3, anything else→0 |
| 7 | Roam | `0x14045d650` | Random point in `roamingSetup.maxRadiusFromSpawn` (tpl +0x5c) or in the saved roamingRange (`0x140447650`), then moves there (`0x140449440`) |
| 8 | Sleep | `0x14045de30` | Sets Actor flag `+0xbf8 |= 0x800`, plays sleepSequence (tpl +0x14) at the bed or on the ground. Wakes when Daytime leaves the sleep window (sets `StayAwakeUntilEvening` bit 0x10). Exit re-rolls the sleep times |
| 9 | Sitting | `0x14045da20` | Sets Actor `+0xbf8 |= 0x200`, timer from idleSetup, plays sitSequence (tpl +0x4) at the seat transform |
| 10 | Interacting | `0x14045cd00` | Turns toward the target and plays `NpcCommon.interactionTarget.interactionSequence` (+0xb8). Fails if there is none |
| 11 | NpcSocializing | `0x14045cf70` | Talk with another NPC (data in `socializingData` +8) |
| 12 | Eating | **none** | Not in the switch, and its table entry is (0,0). Nothing sets WantsToEat. **Unused slot** |
| 13 | WaitingCloseToPlayer | `0x14045ea10` | Timer from `waitCloseToPlayerSetup` (tpl +0x68..). Exit sets the cooldown `DynamicNpcState.nextWaitCloseToPlayerTime` |
| 14 | AvoidBuildingCursor | inline | Walks away from a player's build cursor (`0x140448980`) |
| 15 | AvoidWater | `0x14045b850` | Walks out of shallow water (`avoidWaterSetup`) |
| 16 | InsideWater | inline | Waits until `0x14021a3b0(actor)` (in water) is false |
| 17 | FinishTask | `0x14045c5d0` | Stops the sequence (`0x140445f70`) and teleports to `teleportPositionAfterFinishing` if set. Goes to Roam if the previous state was Sitting, else Finish(0) |

Handler signatures (all High confidence for the mapping):
`0x14045e3c0` `40 53 48 83 EC 70 48 83 B9 A0 01 00`;
`0x14045c3a0` `48 89 5C 24 20 57 48 83 EC 30 48 8B F9 0F B6 CA`;
`0x14045bab0` `80 FA 02 0F 84 ?? ?? ?? ?? 53 48 83`;
`0x14045c850` `40 53 41 56 48 83 EC 48 4C 8B F1 0F`;
`0x14045d650` `48 89 5C 24 10 48 89 74 24 18 57 48 81 EC A0 00 00 00 0F B6`;
`0x14045de30` `40 55 53 57 41 55 41 56 41 57 48 8D 6C 24 D1 48`;
`0x14045da20` `40 55 53 41 56 48 8D 6C 24 B9 48 81 EC E0 00 00`;
`0x14045cd00` `40 56 48 83 EC 60 48 8B F1 0F B6 CA`;
`0x14045cf70` `40 55 53 57 41 56 48 8D 6C 24 C1 48 81 EC C8 00 00 00 48 8B`;
`0x14045ea10` `48 89 5C 24 18 56 48 83 EC 30 4C 8B 89 50 04 00`;
`0x14045b850` `48 89 5C 24 10 57 48 83 EC 70 48 8B 99 50 04 00`;
`0x14045c5d0` `40 53 48 83 EC 60 0F 29 74 24 50 48 8B D9 84 D2`.

### 2.1 Transition table (`0x141fd2780`, u8[18][2], .bss, filled at first run)
Index: `0x141fd277e + state*2 + cond`.

| state | IsFinished → | HasFailed → |
|---|---|---|
| Start | Idle | – |
| FindBed | Idle | Idle |
| Idle | DecideTask | – |
| DecideTask | FindTarget | Idle |
| FindTarget | GoToTarget | Roam |
| GoToTarget | – (handler picks) | DecideTask |
| Roam | Idle | Idle |
| Sleep / Sitting / Interacting | FinishTask | FinishTask |
| NpcSocializing | DecideTask | DecideTask |
| Eating | – | – |
| WaitingCloseToPlayer | Roam | Idle |
| AvoidBuildingCursor | Idle | Idle |
| AvoidWater | DecideTask | Idle |
| InsideWater | DecideTask | – |
| FinishTask | DecideTask | DecideTask |

It is writable at runtime (after the first `npc_active_behavior` tick). Confidence: High.

### 2.2 DecideTask (`0x14045bab0`): where `decision` is chosen
Checks run in this order:
1. Three compiled-out debug stubs, each `xor al,al; ret` and each called only from here:
   - `0x140447090`: true → WantsToSitDown + Finish → FindTarget
   - `0x140447070`: true → WantsToInteract + Finish
   - `0x140447080`: true → WantsToRoam + SetState(Roam)
2. `ShouldSleep` `0x14044bbe0`: only in Idle, DecideTask, Roam, or GoToTarget-not-for-sleep. Uses Daytime vs `NpcCommon.sleepTime/wakeUpTime` (hours × 3.6e12 ns). True → WantsToSleep + SetState(FindBed).
3. If `socializingData.socializingEntityId != 0`: target that NPC → WantsToSocialize + GoToTarget.
4. Otherwise a weighted random pick over `ActiveNpcState.decisionSetup` (tpl +0x38). Each weight is zeroed if the previous state already did that task:
   - `w0 = idleWeight * (prev != Idle)` → WantsToIdle + SetState(Idle)
   - `w1 = inspectWeight * (prev != Interacting)` → WantsToInteract + Finish → FindTarget
   - `w2 = sitWeight * (prev != Sitting)` → WantsToSitDown + Finish → FindTarget
   - `w3 = roamWeight * (prev != Roam)` → WantsToRoam + SetState(Roam)

Confidence: High.

### 2.3 Target search (FindTarget, `0x14044d980(result, ctx, decision)`)
Signature `48 8B C4 44 88 40 18 48 89 48 08 55 56 41 54 41`.
- **Query:**
  - Radius = `searchTargetSetup.targetRadius` (tpl +0xb8), or `ActiveNpcStateSaved.roamingRange` if overridden.
  - Center = **spawnPosition** (`ActiveNpcStateSaved+0`).
  - Starts an async range query (`0x140954100`, max 32 hits, filter `0x141510900`, hash `0x4b2f085d`). The query id goes in `NpcCommon.queryId` (+0x38). Result 3 = pending; results are read next frame (`0x1409546d0` / `0x1409ab7d0`).
- **Filter per hit:** read `ActiveNpcTarget` (tpl of the other entity) `setup` flags:
  - WantsToSitDown: `isSeating` (+3) plus a seat-free check `0x14021a5f0`
  - WantsToInteract: `isInteresting` (+5) or `isCraftingStation` (+2), plus a matching NpcActionSequences entry `0x14044e230`
  - WantsToSleep: `isBed` (+1)
  - WantsToEat: `isFood` (+4)
  - **`isFarmland` (+0) is never tested here.**
- **Availability check** `0x140450f40`: `DynamicActiveNpcTarget.isAvailable`, `usedByEntityId` is 0 or self, `validOffsetsMask != 0`, interaction offer not disabled.
- **Weight** = clamp(1 - dist/(2r), 0.005, 1)², ×0.01 if the hit equals `NpcCommon.previousTargetId`. Then a weighted pick.
- **Reservation:** queues a write through `0x1409528c0` / `0x140953340`. On success it sets `NpcCommon.currentTargetId` (+0x40), `previousTargetId` (+0x4c), `squaredDistanceToTarget` (+0x5c).
- `0x14044c110` releases the range query.

Confidence: High (filter and weights), Medium (the reservation mechanics).

The reservation is cleared by program `npc_active_target` (`0x140457000`, signature `40 55 57 48 8D 6C 24 E8 ?? ?? ?? ?? 01 00 00 41 B8 58 00 00 00 48 8D 54 24 40 48 8B`). Every 7th tick it sets `isAvailable = !underwater` and clears `usedByEntityId` when that user no longer targets it and is Idle, DecideTask or Roam. Confidence: Medium-High.

### 2.4 Interrupts (`CheckInterrupts` `0x14044b3f0`, runs every update before the state machine)
Signature `40 53 48 83 EC 30 48 8B D9 48 8B 89 D8 01 00 00`. Priority order:
1. Actor in water → InsideWater.
2. `0x14044b930` (`48 89 5C 24 18 57 48 83 EC 40 48 8B F9 48 8B 89`): player close (`interactionStates & IsCloseToPlayer`) and cooldown over → WaitingCloseToPlayer.
3. Shallow water ahead (`0x1404480a0`, minWaterDepth tpl +0x9c) → AvoidWater.
4. Build cursor near (`0x140445f30`) → AvoidBuildingCursor.
5. ShouldSleep → FindBed.
6. `0x14044b7b0` (`48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8D B9 B0 01`): socialize request while Idle or Roam → WantsToSocialize + GoToTarget.

Confidence: High.

### 2.5 Other villager functions
- **`0x14044f040` (HandleNpcCommandEvents)**, signature `48 89 5C 24 20 57 48 83 EC 70 4C 8B`
  - Reads `NpcCommandEvent` (72 B: type +8, npcId +0xc, targetTransform +0x10). The event pages are 0x800 bytes and have an 8-byte header.
  - Matches on `ActiveNpcState.npcId`.
  - `Reset` (0): SetState(Start), spawnPosition = target, teleport.
  - `GoToPosition` (1): SetState(Roam), decision WantsToRoam, `NpcCommon.currentTargetPosition` = target.
  - **This is the engine's own "command a villager" API.** High.
- **`0x140450540`** (`48 89 5C 24 18 57 48 83 EC 40 48 8B D9 48 8B 89`): validates `NpcCommon.bedId` and runs the initial bed search. Sets `bedInitialized` (+0x78). High.
- **`0x1404518d0`** PreUpdate (`48 89 5C 24 10 48 89 74 24 18 57 48 81 EC 90 00 00 00 48 8D`): spawn-point fixups (re-home if more than 150 m away and outside a base), bed validity, NPC-in-range list (`0x1404541f0`), socializing tick. Medium.
- **`0x1404510b0`** PostUpdate (`40 56 48 83 EC 60 48 8B F1 48 8B 89`): per-state movement flags. Separation steering, path walker `isWalkingActive`/`keepLastResults`, sitting flag `Actor+0xc00 |= 0x8000`. High.
- **`0x140454050`** SetTarget(ctx, &EntityId) (`48 89 5C 24 18 57 48 83 EC 30 48 8B D9 48 8B FA`): sets `NpcCommon.currentTargetId` and clears the target position and interactionTarget. High.
- **`0x1404532a0`** SetDecision (see above). **`0x14044aa00`**: returns `DynamicActiveNpcState*` after marking it dirty (no unique signature; reach it via a caller).

---

## 3. Components and singletons (villager)
- **`ActiveNpcState`** (template, 456 B, do_not_save): `npcId +0`, sequences `+4/+0x14/+0x24`, `npcType +0x34`, `decisionSetup +0x38` (idle/inspect/sit/roam weights), `idleSetup +0x48`, `roamingSetup +0x58`, `waitCloseToPlayerSetup +0x68`, `avoidWaterSetup +0x88`, `movementSetup +0xa0`, `sleepSetup +0xa8`, `searchTargetSetup +0xb8` (targetRadius, bedRadius), `pathNodeSetup +0xc0`, `socializingSetup +0x190`.
- **`DynamicActiveNpcState`** (136 B): `state +0`, `previousState +1`, `decision +2`, `socializingData +8`, `npcsInRange +0x38`, `bedInitialized +0x78`, `wasPaused +0x79`, `debugStateHistory +0x7a` (no writer seen; probably compiled out).
- **`ActiveNpcStateSaved`** (48 B, saved): `spawnPosition +0`, `overrideRoamingRange +0xc`, `wasSleeping +0xd`, `happiness +0xe`, `roamingRange +0x10`, `teleportPositionAfterFinishing +0x18`.
- **`NpcCommon`** (1688 B, server only): timers `+0x8..+0x30`, `queryId +0x38`, `closestPlayerId +0x3c`, `currentTargetId +0x40`, `bedId +0x48`, `previousTargetId +0x4c`, `sleepTime/wakeUpTime +0x50/+0x54` (hours), `closestPlayerDistance +0x58`, `currentTargetPosition +0x64`, `interactionTarget +0x88`, `taskFailures +0x68e`, `goToTargetTries +0x690`.
- **`ActiveNpcTarget`** (template on beds, seats, stations, food, farmland): `setup` (isFarmland, isBed, isCraftingStation, isSeating, isFood, isInteresting), `npcType +6`.
- **`DynamicActiveNpcTarget`**: `isAvailable +0`, `validOffsetsMask +0x10`, `usedByEntityId +0x18`.
- **`ClientActiveNpcState`**: the client copy (spawnPosition, roamingRange, bedId, happiness), written by `npc_active_behavior_to_client`. **No AI state is replicated to clients.**
- Singletons used: `g38_daytime::Daytime` (sleep window), `PlayerBasesSystemServer`, path, voxel and collider systems.

### Relevant ECS programs (villager)

| program | update fn | role |
|---|---|---|
| `npc_active_behavior` | `0x140456790` | the state machine (this doc) |
| `npc_active_target` | `0x140457000` | target availability and reservation cleanup |
| `npc_active_target_to_client` | `0x1404572d0` | NpcBed → ClientNpcBed |
| `npc_active_behavior_to_client` | `0x140456bf0` | → ClientActiveNpcState |
| `npc_active_input` | `0x140456e50` | player input → other villager's ActiveNpcStateSaved (relocate/roam range? Low) |
| `npc_socializing` | `0x1404580c0` | conversation slots, writes DynamicActiveNpcState of others |
| `find_npc_to_npc_interactions` | `0x1404561a0` | finds socializing partners |
| `npc_bed`, `npc_bed_init` | `0x140457380`, `0x1404576f0` | bed ↔ NPC assignment |
| `npc_behavior` | `0x1404578c0` | dialog/greeting layer (NpcState), not the task AI |
| `npc_update_important_locations` | `0x140458510` | SelectedTargets (spawn points) |
| `unique_npc_init` / `unique_npc_spawns` | `0x14045ade0` / `0x14045b0f0` | spawning |
| `player_assign_npc` | `0x1402d2e30` | assign NPC to bed |
| `npc_shelter` | `0x1402507f0` | shelter/comfort |

---

## 4. Recommended hook points (villager)

All villager AI runs in the **server world only** (NpcCommon and ActiveNpcTarget are server_only). On a client joined to someone else's game, read `ClientActiveNpcState` instead. Whether the client's local GameServer runs villager AI for remote worlds is unknown.

**(a) Read state each frame**
- Hook `0x1404552c0(ctx)` (the per-entity worker). On return read `*(u64*)(ctx+0x1b0)` → DynamicActiveNpcState, `*(u64*)(ctx+0x450)` → ActiveNpcState (npcId, npcType), `*(u64*)(ctx+0x300)` → NpcCommon, `*(u64*)(ctx+0x168)` → CurrentTransform. It is called once per villager per server tick.
- For low-rate events, hook **SetState `0x140453cf0`** (read `state` before and after the original): this gives every transition with old and new state.

**(b) Force a state or decision**
- Inside a worker hook (valid `ctx`), call `SetDecision 0x1404532a0(ctx, d)` and then `SetState 0x140453cf0(ctx, s)`. SetState runs the correct exit/enter logic. **Do not just poke the state byte**, because exit/enter would be skipped.
- To steer task choice: hook DecideTask `0x14045bab0`. On phase 1, set your own decision and call SetState/FinishState, then return without calling the original.
- Cheaper: make the stubs `0x140447090` / `0x140447070` / `0x140447080` return 1 (force sit / interact / roam). The stubs are 3 bytes followed by CC padding up to 16 bytes, so there is room for a jump patch.
- Engine-supported: post an `NpcCommandEvent` (Reset / GoToPosition) with the villager's npcId.
- Global retuning: edit the transition table `0x141fd2780`, or the `decisionSetup` weights in the template (shared by all NPCs of that type).

**(c) Add a new state or task (for example harvest farmland)**
- **Option 1, reuse the unused Eating slot.** The game already wires WantsToEat(6) → FindTarget filter → GoToTarget → Eating(12), and Eating has no handler. Steps:
  1. Hook DecideTask to sometimes pick decision 6.
  2. Make the WantsToEat filter test `isFarmland` (+0) instead of `isFood` (+4). Either hook `0x14044d980`, or do a one-byte patch of the `[target+4]` read in the decision-6 case.
  3. Hook `StateDispatch 0x14044adf0`: when the state is 12, run your enter/update/exit.
  4. Set table bytes `0x141fd2796/97` to `0x11` (FinishTask) so FinishState works.
- **Option 2, new state ids ≥18.** Hook `0x14044adf0` (dispatch) and `0x1404546a0` (FinishState). The latter is required because the table has only 18 rows and would be read out of bounds. Also handle `0x14045c850` (GoToTarget maps unknown decisions to state 0) and check the switches in `0x1404510b0` and `0x14044b3f0` (unknown states fall to the default branch, which is harmless).
- DynamicActiveNpcState is not saved, so custom states cannot corrupt savegames.

---

## 5. Enemy behavior (brief)

**`enemy01` update**
- Address `0x1403688f0`, signature `40 53 48 81 EC 90 06 00 00 41 B8 48`
- Iterator 0x648. Per entity: `0x140367bd0` (`48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 56 48 8D 6C 24 D1 48 81 EC`), which initializes the static tables (guard `0x141fd19b9`), builds the context, and calls the behavior update **`0x140370170`** (`40 53 56 57 41 56 48 81 EC 88 00 00`).
- In that context, `ctx[9]` = DynamicEnemy01BehaviorState* and `ctx[10]` = Enemy01BehaviorState* (template, 2504 B).
- Runs only when `DynamicEnemy01BehaviorState.enabled` (+0x498) is set. High.

**Condition-driven FSM**
- **`HandleCondition(ctx, enemy01::StateCondition)`**: address `0x140367260`, signature `48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 4C 8B 49 48 48`
  - `next = *(u8*)(0x141fd1e20 + state*0x21 + cond)`. The table has 30 states × 33 conditions (Death, TargetSelected, TargetLost, IsFinished, NoiseHeard, WantsToFlee, HasBeenHit, ...).
  - Feature gates: dynamic `enabledFeatures +0x4bc` / `disabledFeatures +0x4c0` override template `enabledFeatures +0x98c`.
  - Then calls SetState `0x140365cb0` (`48 8B C4 48 89 58 20 55 56 57 41 55 41 56 48 81`). High.
- **`StateDispatch(ctx, state, StateUpdateType)`**: address `0x140375490`, signature `48 89 5C 24 18 55 56 57 41 56 41 57 48 8D 6C 24 C9 48 81 EC D0 00 00 00`
  - StateUpdateType 0 OnEnter, 1 OnExecute, 2 OnExit. Jump table at `0x1403761ac` (states 1..29).
  - Main handlers: Idle/WaitAfterPatrol `0x1403723b0`, Combat `0x14036dd30`, Investigate `0x140372890`, ReturnHome/FleeHome `0x140374470`, ReactToNoise `0x140373f60`, Patrol `0x140373580`, Roaming `0x140374880`, Flee `0x1403716f0`, Guard `0x140371e80`, Interact `0x1403725f0`, Circling `0x14036d8c0`, AvoidWater `0x14036d430`, MoveToMarker `0x140373050`, Dead `0x14036fc60`, Spawning `0x1403750c0`. High for the table, Medium for the names (mapped by enum order).
- `DynamicEnemy01BehaviorState` (1256 B): `state +0`, `homeZonePosition +0x10`, `movementTarget +0x20`, `combatData +0x40` (combat state at `+0xc4`), `awarenessData +0x238`, `enabledFeatures +0x4bc`, `disabledFeatures +0x4c0`.

**Aggro and target selection**
- `enemy_combat_targets` `0x140379210` (`48 89 4C 24 08 55 41 55 48 8D AC 24 18 FD FF FF`), with the per-entity loop inline:
  1. Gathers candidates (`0x14037aa20`) and processes HitEvent, EnemyAttackStarted and EnemyIsHelping.
  2. Decays `AggroTarget.aggro` (+0x68) over time.
  3. Updates the 6 slots of `DynamicAggro.targets` (stride 0x1b0: `state +0x30` Unknown/Aborted/Noticed/Alarmed, `entityId +0x34`, `distanceToTarget +0x5c`, `aggro +0x68`).
  4. Sorts the Alarmed slots (`0x140376af0`).
  5. Writes the winner to `SelectedTargets.targets[0].targetEntityId` (+0x64) with `changeCounter++` (+0x80). Target switches are rate-limited by `DynamicAggro.nextAllowedTargetChangeTime` (+0xb10, now + 2 s + rand 0..0.5 s).
- `post_enemy_combat_targets` `0x140379e80` (threat bookkeeping), `enemy_sensor` `0x14032ed20` (perception → EnemySensor), `enemy_threat_events` `0x14036bdb0` (AddThreatTargetEvent → Threat), `enemy01_pre_update` `0x1403689a0` (health restore events).
- Confidence: Medium-High.

Other enemy programs: `enemy_signal_behavior` `0x14036b830`, `enemy_marker_check` `0x140369c50`, `enemy_launch_projectiles` `0x140369af0`, `enemy_attack_attachment` `0x1403695a0`, `enemy_scaling` `0x14036a780`, `network_enemy` `0x14036c3a0`, `blocking_state_enemy` `0x140272b40`, `wildlife_enemy_transformer` `0x140480c70`, `update_spawned_enemy_entities` `0x14036cd50`.

**Enemy hook points**
- Per-entity read: `0x140370170` (`ctx[9]` = dynamic state).
- Transitions: `0x140365cb0` (SetState).
- Behavior overrides: `0x140367260` (HandleCondition, can veto or redirect), or patch the table `0x141fd1e20`.
- Aggro/targeting: hook `enemy_combat_targets`, or overwrite `SelectedTargets.targets[0]` after it runs.

---

## 6. Open questions
- Exact meaning of actor flag `0x32` (gate in the worker) and of the `Actor+0xbf8` / `+0xc00` bit flags (0x200 sit, 0x800 sleep, 0x1000000000 interact, 0x8000 sitting-anim).
- Is one `ProgramContext` exactly one archetype chunk? Who calls program update fns, and on which threads (job system)?
- Whether a client world simulates villagers at all (it probably does not; only the host does).
- `0x1404543c0`: confirm it is teleport-home (used by the leash check and by FinishTask).
- The reservation write path in target search (`0x1409528c0` / `0x140953340`), which looks like a deferred command buffer.
- Handlers not decompiled: NpcSocializing `0x14045cf70`, AvoidWater `0x14045b850`, and the enemy per-state functions.
- `isFarmland` might be used by some other system (not seen in the villager path).

## Unique villagers (static + runtime)
- `unique_npc_init` `0x14045ade0` (init program, flag `0x8`): for each new NPC, hashes its 16-byte GUID with FNV-1a, registers the hash with `PlayerBasesSystemServer`, copies two fields into `ActiveNpcStateSaved` (`+0x0c` = 1, `+0x10`) when the registry entry matches, and queues command type 3 (add component) `NewCharacterSetup` (`0xaf17b8ab`).
- `unique_npc_spawns` `0x14045b0f0`: looks the GUID hash up in a registry (`0x1404698b0` returns the entity that owns it). If another entity already owns it, it queues command type 1 through `0x140954250(cmds, id, id)` for the newcomer; otherwise it registers the newcomer (`0x14046a510`).
- Runtime: a second copy of an existing villager stays idle without `DynamicActiveNpcState`; a villager whose template is not in the world yet becomes active at once (see ENGINE_MAP, "Confirmed in game").
- For mods: spawn each townsfolk template at most once; a spawned villager is saved with the world.

## GoToPosition, exactly (decompile of `0x14044f040`; confirmed in game with the sample mod's F7)
The handler matches the event's npcId (`event+0x14`) against `ActiveNpcState.npcId` (`*(u32*)ctx[0x8a]`, i.e. `ctx+0x450 -> +0`). If `DynamicActiveNpcState.state == 1` (Start) and the type is not Reset, it ignores the event. For type 1 (GoToPosition):
1. `SetState(ctx, 7)` (Roam).
2. markWritten for the DynamicActiveNpcState write entry (`ctx+0x1b0`), then `decision = 1` (WantsToRoam), written directly (not through SetDecision).
3. markWritten for the NpcCommon write entry (`ctx+0x300`), then `NpcCommon+0x64/+0x68/+0x6c = target` as **float meters** (fixed-point position × 2^-32).
Write entry layout (0x30 bytes): `+0 data*`, `+8/+0x10/+0x18` three qwords passed to markWritten, `+0x20` u16 type index, `+0x22` u8 variant, `+0x28` tracker* (NULL → skip markWritten). markWritten is `0x1409c5df0(tracker, a, b, c, u16, u8)`.
Type 0 (Reset) instead does `SetState(ctx, 1)`; through the DynamicNpcState write entry (`ctx+0x330`, 96 B) it sets `isSpawning` (+0x49) = 1 and copies `event+0x30..0x3f` to `revertToRotation` (+0x4c, quaternion); through the ActiveNpcStateSaved entry (`ctx+0x1e0`, 48 B) it writes `spawnPosition` (+0/+4/+8) = target as floats; then it teleports (a Teleport command via `0x140952400` when the entity has a Teleport component, otherwise `0x140448540`). Each write is preceded by markWritten for its entry.
