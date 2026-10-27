# Custom ECS programs: can a mod register its own system?

Build 1269579, image base 0x140000000. Static analysis only (Ghidra via REA + byte scans with `tools/re`). Signatures come from `tools/re/sig.py`: the number before the pattern is its length in bytes, `??` is a wildcard. Each one was unique in `.text` when generated.

## Short answer

**Yes, very likely.** `registerProgram` (0x1409d5770) only stores the descriptor pointer, the name, the flags and the update function. Every name in the access lists is resolved **later, by name string**, when the world finalizes its programs on its first tick (0x140a179b0 → 0x1409f37d0 → 0x1409f3bb0). No other pre-built data is needed. A descriptor registered right after the game's own registration (best: right after `0x1404a5a30` returns) is treated exactly like a game program. It gets its masks, its slot in the dependency graph, archetype matching and job slices.

Hard rules for a mod descriptor:
1. Register **before the world's first tick** (finalize runs once; there is no re-finalize).
2. Set `+0x184 = 1` (tick interval; 0 means a division by zero in the stage scheduler).
3. Do not set desc flag `0x2` (it makes `registerProgram` skip the descriptor and return 1).
4. Every component and event name must exist, otherwise the rest of that program's masks stay empty (see risks).
5. The update function must iterate **all** entities of its slice or **none**. Stopping early is fatal (`Not all entities were updated in program '%s' (%u)`).
6. Before writing a component through a write slot, call `markWritten` (0x1409c5df0), the same way game programs do.

---

## 1. Lifecycle of a program

| Step | Where | What happens |
|---|---|---|
| World created | `createWorld` 0x140976bc0 | Allocates the 0x1CFB7C0-byte world. Builds the component type table (0x140974bf0, **sorted by qualified name**), the template and dynamic component maps, the singleton map and the program name map. `world+0xE15D58` = params byte `+0x43` ("may finalize"). The client passes 1 (checked in 0x140841c40). The server world gets it set later by 0x1409f2b00. |
| Singletons and programs registered | client setup 0x14039fbb0, server setup 0x1404949b0 | Both call `0x14122c1c0(world, x)` (59 programs) and then `0x1404a5a30(world)` (757 programs). In both setups `0x1404a5a30` is the **last** registration call. On the server it is the last call before the setup returns. |
| Server only: "may finalize" | server system update 0x140e47710, state 2 | After GameServer init (0x140e3a350, which runs the server setup) succeeds, it calls `0x1409f2b00(world)` = `mov byte [rcx+0xE15D58],1; ret`. |
| **Finalize** | `finalizePrograms` 0x140a179b0, called at the top of both world tick functions (server 0x1409c86a0, client 0x1409c80d0) | Runs only if `world[0]==0 && world[0xE15D58]!=0`. At the end it sets `world[0]=1`. The tick then moves state 1→2 when templates finish (0x1409bebd0). |
| Per tick | sim step 0x140a1e2e0 | Runs init programs (0x14099ff30), then the graph scheduler 0x1409a0e00 (or the stage scheduler 0x1409a2700 when `storage+0x518==0`; the sim step sets it to 1 first). |

**"Programs finalized" flag:** `world+0x000` (u8). 0 = registration open, 1 = finalized (templates loading), 2 = running. `registerProgram` does **not** check it.

### 1.1 What finalize (0x140a179b0) does, in order
1. `0x1409d4b80(world, 0x9931c5f3 /*FNV("keen::ecs::CollisionEvent")*/, 0x12)`. Internal event setup, not relevant.
2. **`resolvePrograms` 0x1409f37d0(registry=world+0x928, isServer)**. Logs `[ecs%c] Programs: %,u`. For every program entry it calls `resolveAccess` 0x1409f3bb0(entry+0x30, registry, desc), which turns the name lists into bitsets (section 3). It also:
   - builds `entry+0x680` = read | tpl | write | with. This is the "must have all" mask for archetype matching;
   - ORs entry flag **0x80** if the program uses the singleton `ecs::Tasks` (FNV 0x8D4074B4). This gives the program its own exclusive stage;
   - ORs entry flag **0x100** if the desc `+0x100` list is not empty.
   - The return value of 0x1409f3bb0 is **ignored**.
3. `0x1409e6410(registry)` → `0x1409e6570` per program. Marks each component type as read/written/template-read by server or client programs (type info `+0xB0/+0xB1` bits). Then builds the registry bitsets `+0x190` (networked) and `+0x1E0`.
4. Two loops mark event types (`world+0xE15AB8`, stride 0x20, `+0xC` bits) as read/written by server/client programs, using the event bitsets of each program.
5. `buildPlans` **0x140980840(registry, storageA, storageB)**. Builds each program's iteration plan (section 4) from the bitsets. It resolves the singleton lists again **by name**, in descriptor order (0x1409a38c0), and the add/remove lists (0x1409a3700).
6. `buildStages` **0x140981090(storage)**, once for the main storage and once for the second (prediction) storage. Greedy stage packing for the stage scheduler (section 5).
7. Builds the **conflict matrix**: for every pair j < i, bit j of row i (at `storage+0x538 + i*0x70`, 896 bits) is set if `accessCompatible` 0x14099c690 says the two conflict, or if either program has flag **0x800**.
8. `world[0] = 1`.

---

## 2. Program descriptor (0x188 bytes)

A list is `{ const NameRef* items; uint64 count; }` with `NameRef = { const char* str; uint64 len; }` (no terminator needed). "dyn map" = dynamic-component map `registry+0x88`. "tpl map" = template-component map `registry+0x30`. Component hash = FNV-1a32(`"keen::ecs::" + name`) = types.json qualifiedHash (checked: CurrentTransform → 0x027ac564). Singleton hash = FNV-1a32(name) exactly as registered (e.g. `g38_daytime::Daytime`). Event hash = FNV-1a32(`"keen::ecs::" + name`), looked up in the event map `registry+0xE151B8`.

| Off | Field | Resolved by / map | Effect |
|---|---|---|---|
| +0x000 | name (ptr,len) | — | Unique per world (string compare). A duplicate logs `[ecs%c] entity program '%s' already registered` and returns 0. |
| +0x010 | update fn `void(ProgramContext*)` | — | Copied to `entry+0x28`. Called with no null check. |
| +0x018 | tpl (template, required) | tpl map | iterator group T, archetype filter |
| +0x028 | tpl optional | tpl map | iterator group T (null if absent) |
| +0x038 | read | dyn map | group R, filter, conflicts |
| +0x048 | read optional | dyn map | group R (null if absent) |
| +0x058 | write | dyn map | group W, filter, conflicts |
| +0x068 | write optional | dyn map | group W |
| +0x078 | **add-component** list ("change components") | dyn map (0x1409a3700) | u16 type indices appended to the iterator. Example: `transform_to_loot` adds loot components. |
| +0x088 | **remove-component** list | dyn map | same. Example: `transform_to_loot` removes AI components. |
| +0x098 | singletons read | singleton map | object pointers in the iterator. Read in descriptor order. |
| +0x0A8 | u64 optional mask for +0x98 (bit k = item k may be missing) | — | A missing singleton that is not optional sets entry flags 0x20/0x40 (program skipped). |
| +0x0B0 | singletons written | singleton map | as above |
| +0x0C0 | u64 optional mask for +0xB0 | — | |
| +0x0C8 | u64 "parallel write OK" mask for +0xB0 | — | Otherwise the singleton's own option `+0xB8` bit1 decides. |
| +0x0D0 | other-entity template access | tpl map | "other-tpl" accessors (max 9). Missing name → `Could not find component`. |
| +0x0E0 | other-entity read (random access) | dyn map | accessors (max 92 total with the next two) |
| +0x0F0 | other-entity write | dyn map | accessors. **Makes the program serial** (clears the parallel flag). |
| +0x100 | other-entity write, no serialization | dyn map | accessors. Sets entry flag 0x100 (gets a per-program sub-world, merged after the stage). |
| +0x110 | **with** filter (must have, no data access) | tpl map | joins the must-have mask |
| +0x120 | **without** filter | tpl map | archetype must have none of these |
| +0x130 | **any-of** filter | tpl map | must have at least one (only `dynamic_render_model` uses it) |
| +0x140 | whole-table write | dyn map | conflict mask only (unused by game) |
| +0x150 | whole-table read | dyn map | conflict mask only (`collider_system`) |
| +0x160 | events in | event map | event queue pointers in the iterator |
| +0x170 | events out | event map | one "sender" slot per event in the iterator |
| +0x180 | u32 flags | — | section 6 |
| +0x184 | u32 **tick interval** (game: always 1) | — | Copied to `entry+0x24`. The stage scheduler runs the program when `tick % interval == 0`. **Must be ≥ 1.** The graph scheduler ignores it. |

Bitsets are 640 bits (max 0x280 component types; the world has 687 reflected, the per-world count is at `registry+0x10`). Limits per program: tpl ≤ 8, read+write ≤ 42, singletons ≤ 128 (bitset 128), other accessors ≤ 92, other-tpl ≤ 9, add+remove ≤ 64, events-in ≤ 160, total events in+out ≤ 24. Going over a limit is a fatal `SmallDynamicArray is out of elements`.

**Missing names:** components and events missing from the map log `Could not find component '%s'` / `Could not find Event '%s'` and **abort the rest of `resolveAccess`** for that program, but the program stays registered. Missing singletons are skipped quietly in the masks; in the plan they become a null pointer plus flag 0x20/0x40 (program never runs) unless they are optional. Validate every name yourself before registering (look up the FNV hash in the maps, see section 8).

---

## 3. Program table entry (stride 0x1F20, at `world+0x746350`, count u16 at `world+0xE15350`, max 0x380)

`registerProgram` zeroes the entry and fills `+0x00` desc*, `+0x08` u16 id (= index+1), `+0x10` name, `+0x20` flags (|2 if the `disabled` argument is true), `+0x24` interval, `+0x28` update fn, and sets `+0x2D8 = +0x588 = 1` (parallel flags). If flags & 8 (init program), it also appends the index to the init list `world+0xE15358` (count `world+0xE15A58`). Name map insert: `world+0xE15A60`, FNV-1a(name) → index.

Filled at finalize (offsets from the entry):

| Off | Content |
|---|---|
| +0x030 / +0x080 | read / read-optional bitsets |
| +0x0D0 / +0x120 | tpl / tpl-optional |
| +0x170 | singletons-read bitset (2 qwords) |
| +0x180 | other-read |
| +0x220 | other-tpl |
| +0x270 | whole-table read (+0x150) |
| +0x2C0 | events-in bitset (3 qwords) |
| +0x2D8 | u8 parallel-safe A |
| +0x2E0 / +0x330 | write / write-optional |
| +0x420 | singletons-written bitset |
| +0x430 / +0x480 | other-write / other-write-unserialized (+0x100) |
| +0x520 | whole-table write (+0x140) |
| +0x570 | events-out bitset |
| +0x588 | u8 parallel-safe B |
| +0x590 / +0x5E0 / +0x630 | without / with / any-of |
| +0x680 | must-have-all mask (read\|tpl\|write\|with) |
| +0x6D0 / +0x6D2 / +0x6D4 | u16 counts: tpl, read, write |
| +0x6D8 | read+write plan, 42 × 12 B `{u32 dynHash, u16 typeIdx, u8 replicated, u8 saveRelevant, u8 optional}` (reads first, then writes, each in ascending type index). For reads both flags are 0. `replicated` = bit in registry `+0x190` (written by a server program and read by a client program; server storage only). `saveRelevant` = bit in registry `+0x230` (`ecs.savegame_relevant` attribute). |
| +0x8D4 | tpl plan, 8 × 12 B; count u8 at +0x934 |
| +0x938 | other accessors (ptr = typeinfo+0xB2), count u8 at +0xC18 |
| +0xC20 | other-tpl accessors, count u8 at +0xC68 |
| +0xC70 / +0x1178 | events-in queue pointers for storage A / B (counts at +0x1170 / +0x1678) |
| +0x1680 | u32 events-out count |
| +0x1684 | add/remove type indices (u16), count u8 at +0x1704; +0x1706 add count, +0x1708 remove count |
| +0x1710 | singleton object pointers, descriptor order (count u8 at +0x1B10) |
| +0x1B18 | alternate singleton pointers (singleton `+0xB0`) used by the second storage (count +0x1F18) |

---

## 4. Ordering and dependencies

- **There are no explicit run-before/run-after lists** in the descriptor. Order = **registration order + access conflicts**.
- `accessCompatible` 0x14099c690(a, b) compares the compact access sets (built by 0x140971230). Two programs conflict when one writes what the other reads or writes (components, other-entity access, whole-table access, singletons). An exception: if one program's **without** set excludes what the other needs, only the random-access and singleton parts are compared. Events do **not** create ordering.
- **Graph scheduler 0x1409a0e00** (the default path; the sim step sets `storage+0x518=1` before choosing). Each active program becomes a task that depends on every **earlier-registered** active program it conflicts with (the matrix from step 7), or on all earlier ones if either has flag 0x800. A program registered **last** therefore runs **after every conflicting game program of the same tick**. Programs it does not conflict with may run at the same time on other workers. Nothing in the same tick depends on it.
- **Stage scheduler 0x1409a2700/0x140985a30** (used when `storage+0x518==0`). The stages from 0x140981090 are greedy. A parallel-safe program goes into the **earliest** parallel stage after the last stage it conflicts with (a program with no conflicts lands in stage 0). A serial program (has other-write, or a read/written singleton that is not flagged parallel-safe) joins the last stage if that stage is serial, otherwise it opens a new last stage. Flag 0x80 → its own exclusive stage at the end.
- To run **before** a game program, register earlier. Component names resolve at finalize, so a hook at the **entry** of `0x14122c1c0`/`0x1404a5a30` that calls `registerProgram` first also works. Every conflicting game program then waits for the mod program.
- **"Run once, alone, after everything":** flags `0x4 | 0x80 | 0x800` registered last. 0x800 makes it depend on all earlier programs in the graph scheduler. 0x80 puts it in a final exclusive stage in the stage scheduler.

---

## 5. Which programs run where (flags)

| Bit | Meaning (seen in code) |
|---|---|
| 0x1 | client program: runs only in the client storage (kind 2) |
| — (0x1 clear) | server program: runs in the server storage (kind 0) **and** in the client's prediction storage (kind 1) |
| 0x2 | disabled. As a desc flag, `registerProgram` skips the descriptor entirely. Through the `disabled` argument, the entry is registered and the scheduler skips it. |
| 0x4 | system program: **no entity query**. One slice per tick with 0 entities. Not matched to archetypes. |
| 0x8 | init program: runs on newly created entities (0x14099ff30), matched per new entity batch |
| 0x10 | not in prediction (kind 1) |
| 0x20 / 0x40 | computed: skipped (missing required singleton) / skipped in prediction |
| 0x80 | exclusive stage (auto-set if it uses `ecs::Tasks`) |
| 0x100 | computed: has +0x100 list → sub-world |
| 0x200 | split per chunk into parallel tasks (otherwise one task runs all slices) |
| 0x400 | task affinity = 1 (pinned, probably the main thread). The scheduler also pins 0x4 and 0x200 programs when the byte `storage+0x7517F8` is set. Pinning does **not** stop other programs running at the same time. |
| 0x800 | barrier: conflicts with every program |
| 0x1000, 0x4000 | seen in game data, meaning unknown |

Archetype matching (`0x14099d3d0`, cached per component set by `0x1409b6a30` at `storage+0x74C360`): no "without" component, at least one "any-of" if that list is non-empty, and **all** of `entry+0x680`. **An empty access list without flag 0x4 matches every archetype**: the update runs over every entity in the world. Use flag 0x4 for a program that does not iterate entities.

---

## 6. What the update function receives

`run slice` 0x1409a2020 builds a ~0x860-byte **job struct J on its stack** and calls `entry->update(&ctx)` with

```c++
struct ProgramContext { ProgramJob* job; uint32_t entityIndex; uint32_t pad; }; // entityIndex starts at 0
```

The update is called **once per slice** (one archetype chunk range), several times per tick, on task-system worker threads (unless 0x400), at the same time as non-conflicting programs.

Useful J fields: `+0x000` storage (`storage+0x08` = kind 0/1/2; for kinds 0 and 2 `world = storage - 0xE15DC0`), `+0xA8` name (ptr,len), `+0xB8` u16 program id, `+0xBB` u8 storage variant, `+0xC0` → singleton pointer array (count u8 at `+0x400` of that array), `+0x4B0` u8 T, `+0x4B8` u16 R, `+0x4BA` u16 W, `+0x828` entity list of the slice (16 B per entity, EntityId at +0), `+0x830` entity count of the slice.

### 6.1 Iterator layout (filled by iter_begin, advanced by iter_next)

`iter_begin(ctx, iter, size)` **ignores `size`** (r8 is overwritten at once). The size only describes the caller's buffer. Layout in qwords:

```
[0]                          ctx
[1 .. 1+T)                   template component ptrs (per chunk; null if optional and absent)
next R                       read component ptrs (current entity; null if optional and absent)
next 6*W                     write records: {ptr, maskA, maskB, entityRecPtr, u16 typeIdx | u8 variant<<16, changeTracker}
next S                       singleton object ptrs, desc order: sys-read list then sys-write list
next OT                      other-tpl accessors
next O                       other-entity accessors (other-read, then other-write, then +0x100 list)
next EI                      events-in queue ptrs
next EO                      event sender slots (each = J)
then u16 x (A+D)             add/remove component type indices
```

Inside T, R and W, items are in **ascending component type index**, required and optional mixed. Type indices are given in **ascending order of the component's qualified name** (`keen::ecs::<Base>`, byte compare; quicksort 0x140946dd0 over the type table). A split component's `DynamicX` half sorts as `X`. The game's own lists are already in this order (e.g. read `CurrentTransform, …, DynamicShelter, Velocity`).

**Iterator size** = `align8( 8*(1 + T + R + 6W + S + OT + O + EI + EO) + 2*(A + D) )` with T = |tpl ∪ tpl?|, R = |read ∪ read?|, W = |write ∪ write?|, S = |sys-read| + |sys-write|, OT = |other-tpl|, O = |+0xE0| + |+0xF0| + |+0x100|, A/D = |+0x78|/|+0x88|. This matches the constant passed to iter_begin in **812 of 815** game update functions. The exceptions are two flag-4 debug programs and `hitstop_effect`. Example: `npc_active_behavior` = 8·(1+5+10+84+8+1+11+1+0) = 0x3C8.

`iter_next(ctx, iter, size)` returns false when `ctx.entityIndex == J+0x830`. On the first call it loads the base pointers. Each later call adds each component's stride, so after a true return the slots point at the **current entity**. `currentEntity(ctx, &out)` 0x1409b2e40 reads `J+0x828[(idx-1)]`.

### 6.2 Minimal safe update function

```c++
using IterBegin  = void (*)(ProgramContext*, void* iter, uint32_t size);   // 0x1409bab60
using IterNext   = bool (*)(ProgramContext*, void* iter, uint32_t size);   // 0x1409b5a50
using MarkWritten= void (*)(void* tracker, void* maskA, void* maskB, void* entityRec,
                            uint16_t typeIdx, uint8_t variant);             // 0x1409c5df0

// desc: read {"CurrentTransform"}, write {"Health"} -> T=0 R=1 W=1 -> 8*(1+1+6)=64 bytes
void ModUpdate(ProgramContext* ctx) {
    alignas(16) uint64_t it[8 + 64];                 // be generous: the engine does not check size
    g_iterBegin(ctx, it, sizeof(it));
    while (g_iterNext(ctx, it, sizeof(it))) {        // never 'break' out of this loop
        auto* xf = (const uint8_t*)it[1];            // CurrentTransform
        uint64_t* w = &it[2];                        // write record for Health
        if (w[5]) g_markWritten((void*)w[5], (void*)w[1], (void*)w[2], (void*)w[3],
                                (uint16_t)w[4], (uint8_t)(w[4] >> 16));
        auto* hp = (uint8_t*)w[0];                   // write the component here
    }
}
```

`markWritten` marks the entity's component dirty for replication (maskA; server storage only, components in the registry `+0x190` bitset) and for saving (maskB; registry `+0x230` bitset). Game code calls it before every write (`actor_can_jump` 0x140260c10, `actor_teleport_fall_damage` 0x140270970). A system program (flag 0x4) with only singletons can just call iter_begin and read `it[1..1+S)`. iter_next returns false at once.

Do only what the descriptor declares: read/write the listed components and singletons. Do not create or destroy entities, or touch undeclared data, from inside the update. Use events or the add/remove lists for that, or do it in the sim-step hook from ENGINE_MAP 1.1.

---

## 7. How to register from a hook (recommended recipe)

1. Hook `registerAllPrograms` **0x1404a5a30** (`void(World* world)`, rcx = world). It is the last registration call in both client setup 0x14039fbb0 and server setup 0x1404949b0, and it runs before `world+0xE15D58` lets the world finalize. Call the original, then `registerProgram(world, &modDesc, false)` for each mod program. Check `*(u16*)(world+0xE15350) < 0x380` first (the game uses ~817 of 896 slots).
2. Keep each `ProgramDesc`, its NameRef arrays and its strings in **static storage** (the entry keeps the pointer, and finalize reads the lists later).
3. Use one descriptor per world kind or set the flags well. Flags 0 = host server world **plus** client prediction. Add 0x10 for host-only logic. 0x1 = client world only.
4. Validate component and event names first. The component maps already exist at this point (built in createWorld): template map `registry+0x30` (= world+0x958), dynamic map `registry+0x88` (= world+0x9B0). Keen hash map layout as in ENGINE_MAP: bitset +0x08, cap +0x10, keys +0x28, u16 values +0x40, count +0x54. Event map: `world+0xE15AE0`. Singletons (map `world+0x7462F8`) are only looked up at finalize, so they just have to exist by the first tick.

```c++
struct NameRef { const char* s; uint64_t n; };
struct NameList { const NameRef* p; uint64_t n; };
struct ProgramDesc {               // 0x188
    NameRef name; void (*update)(ProgramContext*);
    NameList tpl, tplOpt, read, readOpt, write, writeOpt, addComp, removeComp;   // 0x18..0x97
    NameList sysRead; uint64_t sysReadOptMask;                                   // 0x98, 0xA8
    NameList sysWrite; uint64_t sysWriteOptMask, sysWriteParallelMask;           // 0xB0, 0xC0, 0xC8
    NameList otherTpl, otherRead, otherWrite, otherWriteUnser, with, without, anyOf, allWrite, allRead; // 0xD0..0x15F
    NameList eventsIn, eventsOut;                                                // 0x160, 0x170
    uint32_t flags; uint32_t tickInterval; // = 1                                // 0x180, 0x184
};
static_assert(sizeof(ProgramDesc) == 0x188);
```

---

## 8. Functions

| Name | Address | Signature | Notes | Conf. |
|---|---|---|---|---|
| registerProgram | 0x1409d5770 | `48 89 5C 24 08 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC A0 00` | `bool(World*, ProgramDesc*, bool disabled)`. Skips desc flag 2; rejects duplicates and a full table. | High |
| registerAllPrograms | 0x1404a5a30 | `40 53 48 83 EC 50 48 8B D9 48 8D 15` | `void(World*)`. 757 programs. Last registration call. **Hook here.** | High |
| registerMorePrograms | 0x14122c1c0 | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B DA 48 8B F9 E8 36 36` | `void(World*, ?)`. 59 programs. Runs before 0x1404a5a30. | High |
| finalizePrograms | 0x140a179b0 | `48 8B C4 48 89 48 08 41 56 48 81 EC` | `void(World*)`. Once per world (guard `world[0]==0 && world[0xE15D58]`). | High |
| resolvePrograms | 0x1409f37d0 | `40 53 57 41 54 41 55 41 57 48 81 EC A0 00 00 00` | `(registry, isServer)`. Logs `[ecs%c] Programs: %,u`. | High |
| resolveAccess | 0x1409f3bb0 | `48 8B C4 41 54 41 56 41 57 48 81 EC` | `bool(entry+0x30, registry, desc)`. Names → bitsets. | High |
| resolveComponentList | 0x140975fb0 | `48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 50 0F 57` | `(bits, registry, list, bool dynamic)`. | High |
| resolveSingletonList | 0x14097df50 | `40 53 55 57 41 56 41 57 48 83 EC 30` | Clears the parallel flag unless the singleton is parallel-safe. | High |
| resolveOtherList | 0x140978b70 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 50 0F 57 C0 33` | other-entity lists. | High |
| resolveEventList | 0x14097dc00 | `48 89 5C 24 10 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC F0 00 00` | | High |
| componentHashFromName | 0x1409b0bc0 | `48 89 5C 24 18 55 56 57 48 8D 6C 24 C0 48 81 EC` | FNV-1a32("keen::ecs::"+name). | High |
| buildPlans | 0x140980840 | `4C 89 44 24 18 55 57 41 54 41 55 41 57 48 8D 6C` | Per-program iterator plans. | High |
| planSingletons | 0x1409a38c0 | `40 55 41 54 41 55 41 56 48 83 EC 28` | Singleton pointers in desc order, flags 0x20/0x40. | High |
| planChangeComponents | 0x1409a3700 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 50 49 8B 38 45` | +0x78/+0x88 lists. | High |
| planOtherAccessors | 0x1409a3af0 | `40 53 48 83 EC 20 33 C0 4D 8B D8 48` | | High |
| buildStages | 0x140981090 | `48 85 C9 0F 84 ?? ?? ?? ?? 48 8B C4 48 89 48 08 55 48 81 EC` | Stage packing per storage. | High |
| compactAccessSet | 0x140971230 | `48 83 EC 58 F3 0F 6F 02 F3 0F 6F 6A` | | High |
| accessCompatible | 0x14099c690 | `48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC B8 00 00 00 0F 10 59` | true = no conflict. | High |
| markComponentUsage | 0x1409e6410 | `48 89 5C 24 18 57 48 83 EC 20 33 DB` | | Medium |
| archetypeMatches | 0x14099d3d0 | `48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 48 81 C2 60` | without / any-of / all-of. | High |
| archetypeProgramMask | 0x1409b6a30 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B AA 70 C3 74` | Cached per component set. Skips flags 4 and 8. | High |
| setMayFinalize (server) | 0x1409f2b00 | `C6 81 58 5D E1 00 01 C3 CC CC CC CC` | `world+0xE15D58 = 1`. | High |
| createWorld | 0x140976bc0 | `40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 B8 FE FF` | | High |
| buildComponentTypes | 0x140974bf0 | `48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 28 FE FF` | Then sorted by 0x140946dd0. | High |
| graph scheduler | 0x1409a0e00 | `48 89 4C 24 08 55 53 56 48 8D AC 24 70 E2 FF FF` | | High |
| stage scheduler | 0x1409a2700 / 0x140985a30 | `40 55 53 56 48 8D AC 24 C0 1F FF FF` / `48 89 54 24 10 48 89 4C 24 08 53 56 57 41 54 41 55 41 56 48` | Honors the tick interval. | High |
| run init programs | 0x14099ff30 | `48 8B C4 41 56 48 81 EC 90 00 00 00` | flag-8 programs on new entities. | High |
| run slice | 0x1409a2020 | `40 55 53 57 41 54 41 55 48 8D AC 24 70 F8 FF FF 48 81 EC 90 08 00 00 48 8B 5A 10 45 33 D2 44 8B` | Builds J and calls the update. Fatal on partial iteration. | High |
| slice task | 0x1409e1390 | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B F9 33 D2` | | High |
| iter_begin | 0x1409bab60 | `48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60` | size argument unused | High |
| iter_next | 0x1409b5a50 | `40 57 48 83 EC 20 4C 8B 01 48 8B FA` | | High |
| currentEntity | 0x1409b2e40 | `8B 41 08 4C 8B C2 85 C0 75 06 89 02 48 8B C2 C3 8D 50 FF 48 8B 01 48 03 D2 48 8B 88 28 08 00 00 8B 04 D1 41` | | High |
| **markWritten** | 0x1409c5df0 | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 30 48 8B B1 90 02 00` | `(tracker, maskA, maskB, entityRec, u16 typeIdx, u8 variant)` = write record [5],[1],[2],[3],[4]. | High |

---

## 9. Verdict and risks

**"Call registerProgram with our own descriptor right after the original world-setup function returns" will very likely work.** Doing it right after `0x1404a5a30` returns is the safest point and works for both worlds. Static evidence:
- registration stores pointers only;
- all name resolution happens later, by FNV hash of the name;
- archetype matching, conflicts, stages and plans are all built from the full table at finalize;
- the client finalizes on its first tick and the server after `0x1409f2b00`, and both come after the setup returns.

Risks:
1. **Late registration** (after the first tick) silently breaks things. The entry gets no masks or plan and is missing from stages and the conflict matrix. Archetypes already cached never match it, and new ones match it with an empty must-have mask, so it would run over every entity. Guard with `world[0]==0`.
2. **Bad names** abort `resolveAccess` part-way. The return value is ignored, so later masks stay zero, the iterator layout changes and the update reads the wrong slots. Pre-validate all names.
3. **Iterator order and size**: slots follow the type index (sorted qualified names), not the order of the mod's list. Sort your lists the same way, or read the plan back from `entry+0x6D8/+0x8D4` after finalize. Use an oversized iter buffer, because the engine never checks it.
4. **Partial iteration is fatal.** Always run `iter_next` until it returns false.
5. **Threading:** updates run on workers, in parallel with non-conflicting programs. Under-declaring access is a data race. Over-declaring only costs parallelism.
6. **Replication and saving:** skipping `markWritten` means writes are not sent to clients and not marked for saving.
7. **Prediction:** a flag-0 program also runs in the client's prediction storage (on a joined client, too). Use 0x10 for host-only logic.
8. **Missing singletons** (e.g. a server singleton listed in the client world) disable the program in that world (flags 0x20/0x40) unless they are marked optional.
9. **Interval = 0** → divide-by-zero in the stage scheduler. **Desc flag 2** → silently not registered.
10. Capacity: about 79 free slots (896 max). Per-program limits are in section 2.
11. Which scheduler is active (graph vs stage) depends on `storage+0x518`, which may be cleared in 0x140a177b0/0x140a1d080 (not checked). Programs registered this way run in game: EMF's own and the mods' programs in the server and client worlds (`ENGINE_MAP.md`).

## Open questions
- Meaning of flags 0x1000/0x4000.
- Whether 0x140a177b0 ever clears `storage+0x518` (which would select the stage scheduler).
- The event-out sender API (what an update does with the EO slots); `re_events.md` has how programs send events.
