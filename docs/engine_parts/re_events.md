# ECS events: sending and reading (build 1269579)

How a program sends and reads engine events, so mod programs can do the same. Decompiled with REA/Ghidra; signatures from `tools/re/sig.py` (each unique in `.text`). Confidence High unless noted.

**Confirmed in game:** a mod system program sent `StartGuardingEvent`; the game queued it for the next tick and `enemy01` applied it. The same program read its own event back and the game's `EntityDiedEvent`. EMF exposes this as API v3 (`register_program_def`, `emit_event`, `read_events`).

## Event types
- An event is a reflected struct that derives from `keen::ecs::GameEvent` (8 bytes: `+0 timeStamp`, `keen::Time` in ns). 122 types do; 7 more derive from the older `keen::ecs::Event` and have **no** timestamp: `CollisionEvent` (64 B), `LoginEvent` (4), `RevivedPlayerEvent` (8), `AllItemsRepairedEvent` (8), `DeclinedInteraction` (12), `TestEvent1/2` (4). The reflection descriptor's base pointer (`+0x38`) tells them apart (checked in the exe); EMF stamps only GameEvent types and does not read event types whose size is not a multiple of 8 (queue stride not confirmed). Field offsets in `types.json` include the 8-byte header (`StartGuardingEvent.guardingEntityId` is at +8).
- Event hash = FNV-1a-32(`"keen::ecs::" + name`) = types.json `qualifiedHash` (e.g. `StartGuardingEvent` `0xb096ec86`).
- Per world: **event map** at `world+0xE15AE0` (Keen map, hash → u16 event type index; same layout as the other maps, `registry+0xE151B8` with registry = `world+0x928`). **Event type table** at `world+0xE15AB8` (pointer to entries, stride 0x20): entry `+0x00` = reflection descriptor pointer (descriptor `+0x40` = u32 size, `+0x50` = qualifiedHash, `+0x80` attributes), entry `+0x0C` = u8 flags (read/written by server/client programs, set at finalize).
- `resolveEventList` `0x14097dc00` turns a descriptor's event name list into a bitset over event type indices (`Could not find Event '%s'` on a miss, which aborts the rest of `resolveAccess`). So a program's event slots are ordered by **event type index**, not by the order of the names.
- Attributes `ecs.server_only` / `ecs.client_only` exist on some events (`NpcCommandEvent` is server only). `0x1409d4b80(world, hash, flags)` checks them for internal registrations; `resolveEventList` does not.

## Sending (events out)
- Every program job `J` (built on the stack by `run slice` `0x1409a2020`, `ProgramContext = {J*, entityIndex}`) has:
  - `J+0x0B8` u16 = program id (entry `+0x08`), read by `0x1409b6a20`;
  - `J+0x1B8` = the job's event write buffer (`slice+0x18`), read by `0x1409b85d0`;
  - `J+0x050` i64 tick and `J+0x06C` u32 tick length: `programTime` `0x1409b8030` returns `tick * length` (ns).
- Each events-out slot in the iterator holds `J` itself. Game code sends like this (`0x1402bb250`, `0x140350a50`, `0x140444ab0`, …):
  ```c
  u16 id = *(u16*)(J + 0xB8);
  void* buf = *(void**)(J + 0x1B8);
  Event* e = emitEvent(buf, hash, sizeof(Event), id);   // zeroed, or NULL
  if (e) { e->timeStamp = now; e->field = ...; }
  ```
- **`emitEvent` `0x140955400`** (`48 83 EC 28 4C 8B D9 41 81 F8 00 01`): `void* (Buffer* b, u32 hash, u32 size, u16 programId)`. Buffer = `{u8* base; u64 capacity; u32 used}`. Refuses `size >= 0x100` (returns NULL). Appends a record `{u32 hash; u16 programId; u8 size; u8 0; data[align8(size)]}`, zeroes the data and returns a pointer to it (the event struct, timestamp first). Returns NULL when the buffer is full. The caller fills the timestamp.
- The `size` passed is the full struct size from types.json (`AcceptOfferedInteractionEvent` 0x18, `RestoreEnemyHealthEvent` 0x18, `ActorRotationLerpEvent` 0x20, `CraftRecipeEvent` 0x38).
- Declaring the event in the descriptor's events-out list (`+0x170`) is what game programs do; it sets the event's "written by" bits at finalize. Observed: an event sent in tick N is in the readers' queues in tick N+1.

## Reading (events in)
- The iterator holds one queue pointer per events-in entry (after the other-entity accessors, before the events-out slots), in event type index order. Run slice copies them from program entry `+0xC70` (`+0x1178` for the prediction storage), count u8 at `+0x1170`.
- **Queue** (one per event type and storage): `+0x18` u32 event count, `+0x30` page base, `+0x40` u16 first page (1-based, 0 = none). Pages are 0x800 bytes: `+0x02` u16 next page (1-based), events packed from `+0x08`, at a fixed stride equal to the struct size, 0x7F8 usable bytes per page.
  ```c
  page = first ? base + (first-1)*0x800 : 0; off = 0;
  for (i = 0; i < count; ++i) {
      if (off + stride > 0x7F8) { next = *(u16*)(page+2); page = next ? base + (next-1)*0x800 : 0; off = 0; }
      Event* e = page + 8 + off; off += stride;
  }
  ```
  Seen with strides 0x10 (`EntityDiedEvent` in `trigger_knowledge_on_kill` `0x140416760`, `StartGuardingEvent` in `0x140353fb0`), 0x20 (`SetEnemyVariableEvent` in the enemy update `0x140370170`) and 0xF8 (`HitEvent` in `healthbar` `0x140275070`).
- Readers filter by entity themselves: every reader compares an entity field with its current entity (`currentEntity` `0x1409b2e40`).

## Example: the companion guard handler (`0x140353fb0`, called from the enemy update)
Queues of `enemy01` in type order: CallForHelp(0), Hit(1), Hook(2), Noise(3), SetEnemyVariable(4), StartGuarding(5), StopGuarding(6), SyncedAttack(7).
- For each `StopGuardingEvent` with `guardingEntityId == me` and `guardedEntityId == DynamicEnemy01BehaviorState.guardEntityId` (+0x454): `guardEntityId = 0`.
- For each `StartGuardingEvent` with `guardingEntityId == me`: `guardEntityId = guardedEntityId`. **No other check** (no faction, distance or `canGuardPlayer` test; those are only in the interaction offer, see `re_companions.md`).
So a mod program that sends `StartGuardingEvent{guarding = companion, guarded = player}` makes any `enemy01` actor guard the player.

## Programs that use the events EMF works with (from `programs2.json`)
| Event | Sent by | Read by |
|---|---|---|
| StartGuardingEvent | player_control_action_sequence | enemy01 |
| StopGuardingEvent | player_control_action_sequence, call_factions_for_help | enemy01 |
| NpcCommandEvent (server only) | actor_spawn_entity | npc_active_behavior |
| EntityDiedEvent | death, actor_basics | trigger_knowledge_on_kill, combat_experience_source, random_spawner, … |
| HitEvent | impact_hit_conclusion | healthbar, hit_history, enemy01, durability_loss, … |
| PickupEvent | inventory_actions, loot_pickup_item, player_crafting, … | update_ui_data, random_spawner |
| CraftRecipeEvent | player_crafting | handle_crafting_events |

## Functions
| Name | Address | Signature | Notes |
|---|---|---|---|
| emitEvent | 0x140955400 | `48 83 EC 28 4C 8B D9 41 81 F8 00 01` | see above |
| jobProgramId | 0x1409b6a20 | — | `return *(u16*)(J+0xB8)` |
| jobEventBuffer | 0x1409b85d0 | — | `return *(void**)(J+0x1B8)` |
| programTime | 0x1409b8030 | `48 8B 01 44 8B 40 6C 48 8B 40 50 4D` | `*(i64*)(J+0x50) * *(u32*)(J+0x6C)` |
| resolveEventList | 0x14097dc00 | see re_custom_programs.md | names → bitset |
| checkEventAttributes | 0x1409d4b80 | — | `(world, hash, flags)`, logs server/client-only misuse |
| guardEventHandler | 0x140353fb0 | — | StopGuarding then StartGuarding queues of `enemy01` |
