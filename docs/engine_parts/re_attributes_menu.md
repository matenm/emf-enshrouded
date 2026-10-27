# RE: attributes/damage, local-world auto start, world name (build 1273043)

All addresses are for **build 1273043** (client `enshrouded.exe`, sha256 `d64966e2…7882`, image base 0x140000000) unless marked. The addresses in `docs/engine_parts/combat.md` / `re_saves.md` are for 1269579. In the combat code they moved by about +0x380.
Method: static analysis with the `tools/re` scripts (with the string and call caches regenerated for 1273043: rerun `allstrs.py` after a game update), objdump disassembly, and Ghidra pseudocode through REA. Points marked "confirmed in game" were also checked in the running game.
Every signature below was checked with `sig.verify` and matches **exactly once** in `.text` of the 1273043 client exe. The Part 1 signatures were also checked against the 1273043 dedicated server (`enshrouded_server.exe`, sha `d7baa768…`); the server addresses are listed too.
Calling convention everywhere: Win64 (rcx, rdx, r8, r9, then the stack).

---

## Part 1: Health and attributes

### 1a. `attrFind`: how an id becomes a slot (High)

| | |
|---|---|
| Address | `0x14121e150` (RVA 0x121e150; server 0x140986f90) |
| Signature | `89 54 24 10 53 48 83 EC 20 48 8B D9 48 8D 54 24 38 49 8D 48 68` (21 B, unique client + server) |
| C signature | `AttrFindResult* attrFind(AttrFindResult* out, u32 attrId, const AttributeRegistry* reg)` |
| Result | `struct AttrFindResult { u8 err; u8 pad[3]; u32 zero; const AttrInfo* info; }` (16 B). `err` is 0 on success and 0x1F when the id is not found; `info` is null on error. |
| AttrInfo (32 B entries) | `+0x00 u32 rootId`, `+0x0C u16 slot` (other fields unknown) |

- **Body.** It calls the open-addressing hash lookup `0x14121e0a0(&reg->map /*reg+0x68*/, &attrId)`. The lookup reads the capacity at map+0x10, the occupancy bitset at +0x08, the keys at +0x28 and the values (stride 0x20) at +0x40, and it does nothing else. It does not allocate, takes no locks and writes no global state.
- **Thread safety.** It is safe to call from any thread for as long as the registry exists. The registry is the ECS singleton `impact::AttributeRegistry`; it is built at world setup and only read afterwards (sys-read by about 40 programs that run in parallel). Calling it from EMF's post-step command hook is safe, and so is calling it from inside a mod program.
- **The game's own recipe.** `network_health_attribute` `0x140275d70` (the Health → NetworkHealth program) does exactly this, so EMF can copy it:
  ```
  if (!comp || !(comp->flags & 1)) -> error 0x16
  r = attrFind(id, reg); if (r.err) -> error
  if (r.info->rootId != comp->rootId) -> error 0x2C        // wrong component for this id
  value = *(s32*)((u8*)comp + comp->storageOffset /*u16 @+4*/ + r.info->slot*4)
  ```
  The asm is at 0x140275dcf–0x140275e5f: `cmp [rcx],eax` compares info->rootId with comp->rootId, then `movzx edx,word [rcx+0xc]` loads the slot.

**EMF recipe for any entity, player or creature, in a world this game hosts (High).**
```cpp
// comp = get_component(serverWorld, Health::kHash /*0xDC101091*/, entity)   (server_only: host only)
// reg  = singleton(serverWorld, "impact::AttributeRegistry")                (EMF already resolves it in inventory.cpp)
bool attr_read(const uint8_t* comp, const void* reg, uint32_t id, int32_t* out) {
    if (!comp || !(comp[6] & 1)) return false;                 // AttributeComponent.flags bit0 = Initialized
    struct { uint8_t err, pad[3]; uint32_t z; const uint8_t* info; } r{};
    g_attrFind(&r, id, reg);
    if (r.err || !r.info || *(const uint32_t*)r.info != *(const uint32_t*)comp) return false;  // rootId check
    uint16_t off = *(const uint16_t*)(comp + 4), slot = *(const uint16_t*)(r.info + 0xC);
    if (slot >= comp[7] /*storageCount*/) return false;         // extra guard (not in game code)
    *out = *(const int32_t*)(comp + off + slot * 4); return true;
}
current health = attr_read(Health, reg, 0x8EB84995)   max health = attr_read(Health, reg, 0xDC8358BE)
```
- **Attribute ids** (all used as immediates or `.rdata` constants in this build):
  - current health `0x8EB84995` (`.rdata` 0x14150f98c), High;
  - max health, computed, `0xDC8358BE`, High;
  - base max `0x59185EF5` (0x14150f990), Medium;
  - stamina `0x04B6AA8B` (0x14150f988), High;
  - **mana `0x6AA2574E` (0x14150f984), High.** `impact_hit_conclusion` loads it at 0x1403ee1bd to charge `HitCandidate.sourceManaCost` against the source's Mana component.
  - `health_recharge` also looks up `0x95DA4F21`, `0x4C1DAA74`, `0xDEE8D7AC` and `0xA4F5866E`. These are probably regen inputs; the names are unknown.
- **Mini-boss storage** `1498,0,1498,0,0,0,0,1498`: slot 0 is current health, which matches the in-game kill by writing 0. Which of slot 2 and slot 7 is the computed max (0xDC8358BE) and which is the base (0x59185EF5) comes from resource data. **Guess: Low.** Always use `attrFind`.
- **A joined client** has no Health component. Only `NetworkHealth {u32 health; u32 healthMax}` exists there.

### 1b. `attrAdd`: the delta write (High)

| | |
|---|---|
| Address | `0x1403e8720` (server body 0x14018b120) |
| C signature | `void attrAdd(AttributeComponent* comp, const AttributeRegistry* reg, s32 delta, u32 attrId)`. The return value in eax is garbage. |
| Signature problem | The body has a **byte-identical twin** at `0x14021c230`; only the rel32 values differ (diffs at +0x29/+0x74/+0x112). The best whole-body pattern matches both twins: `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 60 41 8B F8 48 8B DA 4C 8B C2 48 8B F1 41 8B D1 48 8D 4C 24 20 E8 ?? ?? ?? ?? 0F B6 4C 24 20 33 ED 84 C9`. Either twin is fine to call. |
| Unique way to resolve it | Use the call site in applyHitToEntity's Health block: `44 8B 0D ?? ?? ?? ?? 44 8B C6 48 8B 55 A0 49 8B CE E8 ?? ?? ?? ??`. It matches once at 0x1403e9431 (server 0x14018be31). attrAdd is the rel32 target of the E8 at match+0x11. |

What it does (asm 0x1403e8720–0x1403e8873):
1. `info = attrFind(attrId, reg)`; it returns if not found.
2. If `comp && (comp->flags & 1)`: `slot[info->slot] += delta`. **It does not check `rootId`**, so the caller must make sure `comp` is the Health component; run the 1a check first.
3. `root = attrFindRoot(comp->rootId, reg)` (`0x14121e1a0`, signature `48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 83 78 64 00`).
4. For i from last to first: if `AttributeStructure[i].calculation` has ops, it evaluates them with `0x14021c9a0` and stores the result in slot i. This is the derived-value (formula) recompute.
5. **There is no clamp**, unless a formula clamps. Fall damage clamps by hand (`max(0, …)`), which suggests the current-health slot has no clamping formula (Medium). EMF should clamp itself: `new = clamp(cur+delta, 0, max)`, then call `attrAdd(comp, reg, new-cur, 0x8EB84995)`. A delta of 0 is a plain "recalc".
6. **It does not mark the component written** (no `markWritten`).

**Does it trigger death, replication and NetworkHealth? Yes, through polling.**
- `death` `0x1402734a0` runs every tick over every entity with Health. It reads 0x8EB84995 through attrFind and handles value < 1 by setting the actor dead state and emitting `EntityDiedEvent`. This matches the kill seen in game. High.
- `network_health_attribute` `0x140275d70` iterates every entity with Health + NetworkHealth every tick (the `iter_next 0x1409b6e60` loop has no change filter). It copies cur and max into `NetworkHealth`, calling `markWritten` only when a value changed, so the change replicates to clients. High.
- Health itself is `server_only` + `do_not_save`, so skipping `markWritten` on the Health write has no replication or save effect. (Medium. Game programs do mark it, through their accessors. In-game evidence that skipping it works: the slot-0 kill.)
- **What attrAdd does NOT do:**
  - no `HitEvent`, so no hit reaction, no damage numbers, no aggro;
  - no change to `ExperienceSource`, so no kill credit or XP for anyone new;
  - no `WasKillingBlow`, so `destructible` props are **not** destroyed (that program needs a killing-blow HitEvent).
- **Thread rule.** Write only while no program runs: EMF's post-step command hook on the server thread, or inside a mod program that declares `other-write: Health`, so the scheduler serialises it against `health_recharge`, `impact_hit_conclusion`, `fall_damage_*` and the rest.

### 1c. A cleaner damage path with kill credit and hit reactions: queue a HitCandidate (High for the mechanism, Medium for the exact timing)

There is **no DamageEvent type**. `HitEvent` (0xCC0D57E0, 248 B) is only the after-the-fact notification. Emitting it yourself changes no health, and it would hand out XP sources and feedback without any damage, so it is not a way to deal damage.

The game path is:
1. `applyDamage` (impact native) calls `allocPendingHit` to queue a candidate.
2. Program `impact_hit_conclusion` dedupes the candidates and calls `applyHitToEntity`.
3. `applyHitToEntity` does attrAdd(Health), sets the killing-blow flag and emits `HitEvent`.
4. The consumers react:
   - `combat_experience_source` sets the victim's ExperienceSource, so the root source gets the XP on `EntityDiedEvent`;
   - `enemy01` handles hit reactions and aggro;
   - `server_player_control_hitevents` and the client copies of the HitEvent handle player stagger;
   - `destructible`, `hit_history`, the healthbar, and others react too.

A mod can enter at step 1 without the impact VM:

| Function | Address (client / server) | Signature | C signature |
|---|---|---|---|
| allocPendingHit | 0x141218bd0 / 0x1409819c0 | `48 89 5C 24 08 57 48 83 EC 20 48 8B D9 0F B6 FA 48 81 C1 10 01 00 00` | `HitCandidate* allocPendingHit(ImpactSystem* sys, u8 candidateType)`. It pushes onto the growable array at sys+0x110 {data, count, cap, growFn}; capacity starts at 256. It falls back to the array at sys+0x138 and **does not set the type on that path**, so always write +0x144 yourself. The record is zeroed and the transform scales are set to 1.0. **Not atomic.** |
| impact_hit_conclusion (program fn) | 0x1403edc70 / 0x140190670 | `48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 68 FC FF FF` | Program update `(ProgramCtx*)`. It reads the pending array (sys+0x110/+0x118) once at its start (0x1403edec0, via `0x14121f7d0`). |
| applyHitToEntity | 0x1403e8ec0 / 0x14018b8c0 | `4C 89 44 24 18 48 89 54 24 10 55 57 41 54 41 55 41 56 41 57 48 8D AC 24 78 FF FF FF` | `void (bool* outPropagate, EntityHandle target, HitConclusionCtx*, HitCandidate*)`. It needs the program's accessor context, so **do not call it directly**. |
| clear pending (inner fn of program `clear_hit_candidates`) | 0x14121a240 / 0x140983080 | `48 89 5C 24 08 57 48 83 EC 40 48 8B 91 20 01 00 00` | It sets sys+0x118 = 0 and moves the overflow array (+0x138) into the primary. The program fn 0x1403edaf0 has 5 byte-identical twins. |

**Timing (Medium from registration order; confirmed in game: pre-hooks on both programs saw `clear_hit_candidates` earlier in the same step, tick 660).**
- All of the following programs sys-write `impact::ImpactSystem`, so they are serialised in registration order. Their order in `RegisterAllPrograms` (call-site order in 0x1404a5db0) is:
  1. `clear_hit_candidates` (#248);
  2. `impact_system_prepare` (#507);
  3. `impact_initialize` (#508);
  4. `impact_prepare` (#509);
  5. `impact_simulation` (#510);
  6. `impact_hit_conclusion` (#511);
  7. `impact_cleanup` (#514);
  8. `server_update_impact_system` (#516).
- So the list is **cleared at the start of each tick**. A candidate pushed after the step, which is where EMF's command hook runs, is wiped before `impact_hit_conclusion` sees it.
- EMF's own programs are registered after all game programs (`hk_register_all_programs`), so they run too late as well.
- **Use EMF's existing `hook_program("impact_hit_conclusion", pre, …)`.** In `pre`, drain a mod damage queue into `allocPendingHit(ImpactSystem singleton, 0)`. This runs on the right job thread at the right moment, with the ImpactSystem owned by that program.
- Runtime check: pre-hooks on `clear_hit_candidates` and `impact_hit_conclusion` that log their order once (in game, the clear runs first).
- Fallback (Low/Medium): push into the overflow array at sys+0x138 after the step. The clear moves it into the primary on the next tick. This is hacky.

**HitCandidate (= `keen::ecs::HitCandidate`, 0x150 B; its first 0xF8 B are the `HitEvent`).** Fields a mod should set; everything else stays zero:

| Off | Field | Value for a mod hit |
|---|---|---|
| +0x08 | targetTransform (WorldTransform 56 B) | the target's CurrentTransform (copied into the HitEvent for client feedback) |
| +0x6C / +0x78 / +0x84 | position / normal / hitDirection (float3) | the hit point, the surface normal, and the attacker→target direction (used for push-back and direction reactions) |
| +0x90 | rootSourceId | **attacker root: the player entity for kill credit/XP** (0 = no credit) |
| +0x94 | targetId | victim |
| +0x98 | flags | 0, or 0x800000 ForceInterrupt. Healing: set **0x4000 WasHealing** (otherwise a Destructible minimum-damage check can drop it). The game adds 0x2000 WasKillingBlow itself. |
| +0xA0 | rootTargetId | victim (or its ImpactHierarchy root); 0 means targetId is used |
| +0xA4 | sourceId | attacker (normally the weapon or projectile; using the player is fine) |
| +0xA8 | permissionSourceId | attacker |
| +0xBC | healthChange (s32) | **−damage** (+heal) |
| +0xC0 | staminaChange | 0 (only used when the hit was blocked or parried) |
| +0xC8 | aggro (float) | e.g. 1.0 (threat for enemy AI) |
| +0xD4 | impactId | a unique nonzero id per request (the dedupe key: per (root target, impactId) only the hit with the largest |healthChange| survives each tick). An unknown id is harmless: `0x141225490` only records hits for existing impact instances. |
| +0xE0 | hitReaction (u16 `impact::HitReactionType`) | 0 None, 1 VeryLight, 2 Light, 3 Medium, 4 Heavy, 5 Boss, 6 LongStun, 7 TrapStun, 9 ThrowIntoAir |
| +0xE2 | pushBackForce (u8) | 0.. |
| +0xF0 | targetFaction | the victim's `Faction` |
| +0x140 | sourceManaCost | 0 (when nonzero, the source must have that much mana or the hit is dropped) |
| +0x144 | candidateType | **0 = Entity** (1 Terrain, 2 Building) |
| +0x14C | sendHitEvent | **1** |
| +0x14D | ignoreConsecutiveHits | **0** (when nonzero, the conclusion skips targets the impact already hit, through `0x1412212a0`) |

What then happens, from the asm of 0x1403edef0–0x1403ee4a3 and of applyHitToEntity:
- The hit is dropped if neither targetId nor rootTargetId exists (`0x14099d7f0`).
- The target's `CombatFlags.handleHits` must be set (it is for creatures and players). This path **skips armour, susceptibility, crit, block and the base-protection (`ProhibitedHit`) checks**: it is "true damage".
- Health goes through attrAdd, and `WasKillingBlow` is set on >0 → ≤0.
- `HitEvent` is emitted; if the event buffer is full, it goes into the ImpactSystem ring at +0x190 and is re-emitted next tick by `impact_system_prepare`.
- **Kill credit:** `combat_experience_source` `0x14032c640` reads each HitEvent's rootSourceId and rootTargetId. When rootSourceId is a player id (1..0x3FE), or is resolved to its owner through a component lookup (companions/summons; Medium), it writes it into the victim's `ExperienceSource`. On `EntityDiedEvent` that player gets XP, and `trigger_knowledge_on_kill` reads the same field.
- Bonus event: `RestoreEnemyHealthEvent` (0xD5785045, 24 B: +8 enemyId, +0x10 duration) is consumed by `enemy01_pre_update`, which heals an enemy over time. Medium.

**Summary for the API.**
- `emf_attr_get(entity, id)` and `emf_attr_add(entity, id, delta)` (clamped, through attrFind + attrAdd): post-step or in a mod program. Use them for heal, god-mode style edits and silent damage.
- `emf_damage(target, source, amount, reaction)`: queue a HitCandidate and flush it in the `impact_hit_conclusion` pre-hook. It gives real hits with credit and reactions.
- `CombatFlags.immune` / `handleHits` remain the cheap god-mode switch.

---

## Part 2: starting LOCAL slot 1 offline without menu clicks

### What the menu actually does (High)

The UI never calls save or session code directly. Each frame the UI pushes small command records into a **pregame command queue**, and the pregame state machine dispatches them.

- **UI frame callback** `0x140bc1e40` (signature `40 55 53 57 41 54 48 8D 6C 24 88 48 81 EC 78 01 00 00`).
  - It is registered as the UI system's frame callback in app setup (0x1408493bd, stored at uiSystem+0xF60; uiSystem = gameApp+0x5B0) and called by the UI system update (0x140a43302, `call rax`).
  - It sets the global **`DAT_14281e498` = &uiCtx (a stack object)** for the frame and sets it back to 0 at the end. So **the global is valid only inside the UI frame.**
  - `uiCtx+0xD0` → UiData, a persistent object. **`UiData+0x33CC0` → the command queue**: `{+0x08 Cmd** ptrs; +0x10 u64 count; +0x18 cap; +0x20 growFn; +0x30 u8* buffer; +0x40 u32 writeOffset (16-aligned)}`. It is a plain, non-atomic slice.
- **Command posters**, all `bool fn(UiCtx* ui, …)`, which read `ui+0xD0`→`+0x33CC0`. Use the long signatures below; they are unique, but these template clones differ only in a few bytes.

| Cmd | Poster | Address | Layout | Pregame dispatcher action (`0x140869090`) |
|---|---|---|---|---|
| 0x11 select character | postSelectCharacter(ui, u32 unused, u16 slotId) | 0x140d5e310 | {u8 0x11; u32 @+4; u16 slot @+8}, 12 B | `selectCharacter 0x140e0d430(out, save2, slot)`: needs the record state to be 2/3/4, then sets save2+0x1192BE (slot) and +0x1192C0 (valid) |
| 0x16 select/play world | postSelectWorld(ui, u16 slotId) | 0x140d5e610 | {u8 0x16; u16 slot @+1}, 3 B | `if (selectWorld 0x140e0d670(save2, slot)) playWorld 0x140835520(pregame, slot)`. selectWorld needs the world list loaded (save2+0x984 == 3) and the record state to be 2/4, then sets save2+0x1192BB/+0x1192BD. playWorld sets gameApp+0x35058 = slot, +0x3505A = 1, copies the world name to +0x34FE8 and clears +0x35270 (adventure). |
| 0x27 session area | postSessionArea(ui, u8 a) | 0x140d5e450 | {u8 0x27; u8 a @+1}, 2 B | a==0: gameApp+0x34FC0 = **1 (Offline)**, mode +0x34FA8 = 0. a==2: +0x34FC0 = 3 (Online). Other values are ignored. |
| 0x28 start | postStart(ui) | 0x140d5bc70 | {u8 0x28}, 1 B | `stateMachine(pregame+0x300).request(0)` enters `PreGame::Start_Play` (`0x1408719b0`) |

- **The UI path for "Start…" then "Offline: Start" on a local world.**
  - Clicking a world entry in the list UI (`0x140ce3ca0`) posts 0x16 (at 0x140ce3fb7 and 0x140ce4b00).
  - "Start" calls `0x140d79ea0(spec)`, which calls `0x140d79c90`. That function posts `0x27 {online ? 2 : 0}` and then `0x28`. The online flag is UiData's host-settings byte at `UiData+0x336A0+0x2F1`; byte +0x2F0 decides whether the Offline/Online popup appears first (0x140ce49d4–0x140ce4a3b).
  - So "Offline: Start" is exactly: **0x16 {slot} → 0x27 {0} → 0x28**.
  - The character comes from the character-select screen (`0x140b623b0` posts 0x11). `selectCharacter` is called **only** from the dispatcher (cmds 0x0F and 0x11); there is no other code path that picks a character.
- **What the game then does.**
  - Start Play checks the selected character with `0x140dd45b0(save2)`. If none is selected it logs "No Character selected" and goes back to the menu.
  - `0x1408664a0` pulls the world's game-settings preset (gameApp+0x35060.., +0x350F8 = 1).
  - The play state's creation step 3 (`0x14084a2c0`) hosts the session. **area == 1 means offline**: max players 1, and the session name is gameApp+0x34FE8.
  - For area 3 the visibility comes from user settings `+0x211` (1/2/3 map to visibility 2/1/0, with password blocks at +0x218/+0x250/+0x288), and max players is clamped to 1..16.
  - Step 5 loads gameApp+0x35058 and logs `[play] Load local save Slot: '%k'`.
- **Session fields in gameApp** (= `*(pregame+0x28)`):
  - `+0x34FA8` mode: 0 Lobby, 1 Host_Offline, 2 Host_Online, 3 Client_Online;
  - `+0x34FC0` area: 1 Offline, 2 LAN (unsupported), 3 Online. Evidence: the strings "[session] Can't set an offline/lan game favorite", "Unsupported area %u", and the dispatcher's 0→1 / 2→3 mapping.

### Can EMF do it? Yes. Risk: Medium.

**Hooks and functions EMF needs** (client only; all signatures unique):

| Purpose | Address | Signature |
|---|---|---|
| Per-UI-frame hook point. It is called once per UI frame, right after `DAT_14281e498` is set (its only caller is 0x140bc1e40). Its first instruction gives the global: `mov rcx,[rip+disp]` at +0xA, so global = fn+0x11+disp. | `0x140d8cd40` `void(void)` | `48 89 74 24 10 57 48 83 EC 40 48 8B 0D ?? ?? ?? ??` |
| Pregame main-menu state handler: `(Pregame* rcx, u8 phase dl, Cmd* r8)`. Phase 0 = enter (gives pregame and save2), 1 = command or update, 2 = exit. Use it to capture `pregame`, `gameApp = *(pregame+0x28)`, `save2 = *(pregame+0x140)`, and a **"main menu active" flag** between phase 0 and phase 2. **Correction (confirmed in game):** this stand-alone copy is never called (no reference but `.pdata`). The game runs the combined pregame state handler `0x140868310` `(Pregame* rcx, u8 state dl, u8 phase r8b, Cmd* r9)` (reached through the thunk 0x140827a10, which normalises a 6th byte on the stack): state 2 is this main menu (the same body, inlined), state 3 Start Play (0x1408719b0). Signature `40 53 48 83 EC 60 45 0F B6 D0 48 8B D9 0F B6 CA 84 D2 0F 84 ?? ?? ?? ?? 83 E9 01 0F 84 ?? ?? ?? ?? 83 E9 01 74 ?? 83 E9 01 74 ?? 83 F9 01 0F 85` (unique; absent from the server). States seen at start-up: 0, then 1 about 6 s later, then 2 (the main menu). | `0x140868fb0` | `40 53 48 83 EC 60 48 8B D9 0F B6 CA` |
| postSelectCharacter | `0x140d5e310` | `48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B 81 D0 00 00 00 41 0F B7 D8 8B FA 48 8B B0 C0 3C 03 00 4C 8D 46 18 8B 46 10 49 3B 00 74 78 8B 56 40 8B C2 83 E0 0F 74 05 2B D0 83 C2 10 4C 8B 76 30 33 C0 8B CA 4C 03 F1 49 89 06 41 89 46 08 8D 42 0C 41 C6 06 11` |
| postSelectWorld | `0x140d5e610` | `48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 81 D0 00 00 00 0F B7 DA 48 8B B8 C0 3C 03 00 4C 8D 47 18 8B 47 10 49 3B 00 74 7D 8B 57 40 8B C2 83 E0 0F 74 05 2B D0 83 C2 10 48 8B 77 30 8D 42 03 8B CA 48 03 F1 66 C7 06 16 00` |
| postSessionArea | `0x140d5e450` | `48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 81 D0 00 00 00 0F B6 F2 48 8B 98 C0 3C 03 00 8B 43 10 48 3B 43 18 74 7F 44 8B 43 40 41 8B C0 83 E0 0F 74 07 44 2B C0 41 83 C0 10 48 8B 7B 30 41 8D 40 02 41 8B C8 48 03 F9 66 C7 07 27 00` |
| postStart | `0x140d5bc70` | `48 89 5C 24 10 57 48 83 EC 20 48 8B 81 D0 00 00 00 48 8B 98 C0 3C 03 00 4C 8D 43 18 8B 43 10 49 3B 00 75 13 33 FF 48 85 FF 0F 95 C0 48 8B 5C 24 38 48 83 C4 20 5F C3 8B 53 40 8B C2 83 E0 0F 74 05 2B D0 83 C2 10 48 8B 7B 30 8D 42 01 8B CA 48 03 F9 C6 07 28` |
| Safety net: "character selected?", called only from Start Play | `0x140dd45b0` `Result* (Result* out, Save2* s)` | `40 53 48 83 EC 20 8B 82 E8 27 00 00 4C 8B C2` |
| (reference) pregame dispatcher | `0x140869090` | `4C 8B DC 55 56 57 49 8D AB 58 FC FF FF` |
| (reference) play-state dispatcher | `0x140852760` | `4C 8B DC 55 56 57 49 8D AB 18 FF FF FF` |
| (reference) playWorld / selectWorld / selectCharacter / getSelectedWorldName | 0x140835520 / 0x140e0d670 / 0x140e0d430 / 0x140dd1b40 | `48 89 5C 24 08 57 48 83 EC 50 0F B7 DA 48 8B F9 48 8B 91 40 01 00 00` / `48 89 5C 24 08 57 48 83 EC 20 8B 81 80 09 00 00` / `48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 8B 82 E8 27 00 00` / `48 89 5C 24 08 57 48 83 EC 30 8B 82 80 09 00 00` |

**save2 offsets for 1273043.** These are +0x10 from 1269579 and were read from the 1273043 functions above.
- `+0x980`: world-list job byte (1 = busy). `+0x984`: list state (3 = loaded).
- World record = `save2 + 0x990 + storage*0xE60 + index*0x170`: `+0` state (1 empty, 2 exists, 4 changed), `+0x58` name `{char*; u64 len}`.
- `+0x27E8`: character-list busy byte (1 = busy).
- Character record = `save2 + 0x27F8 + storage*0x57980 + index*0x5798`: `+0` state, `+0x10` name `{char*; u64 len}` (confirmed by renameCharacter `0x140e06f90`: `lea rcx,[rsi+0x10]` then string assign).
- `+0x1192BB` u16 selected world, `+0x1192BD` valid; `+0x1192BE` u16 selected character, `+0x1192C0` valid.
- Slot id u16: low byte = index+1, high byte = storage (**0 Local, 1 Steam**) (helpers 0x140e7b830 / 0x140e7eca0).

**Recipe** (the bridge thread only sets a request; all game calls happen in the UI-frame hook):
1. The bridge command `start_local world=0x0001 char=local:N` stores the request atomically.
2. In the 0x140d8cd40 detour (call the original first or last, either is fine), check the preconditions. Abort with a message if any fails:
   - the main-menu state is active (from the 0x140868fb0 hook);
   - `gameApp+0x34FA8 == 0`, and no server or client world exists;
   - `save2+0x984 == 3`, `(u8)save2[0x980] != 1`, `(u8)save2[0x27E8] != 1`;
   - world record (st 0, idx 0) state is 2 or 4;
   - character record (**st 0**, idx N) state is 2, 3 or 4. Log its name (the LOCAL test character's).
   - Refuse any slot id whose high byte != 0.
3. Frame A: `postSelectCharacter(ui, 0, 0x00NN)`, then `postSelectWorld(ui, 0x0001)`.
4. Frame B, or later: verify:
   - `save2+0x1192BE == 0x00NN` and `+0x1192C0 != 0`;
   - `save2+0x1192BB == 0x0001`;
   - `gameApp+0x35058 == 0x0001`, `+0x3505A == 1`, `+0x35270 == 0`.

   Then post `postSessionArea(ui, 0)` and `postStart(ui)` in the same frame, so nothing can slip in between.
5. **Safety net that enforces the LOCAL-only rule.** Post-hook `0x140dd45b0`. If `*(u8*)(save2+0x1192BF) != 0` (the selected character is Steam storage) **or** `*(u8*)(gameApp+0x35059) != 0` (the world slot to load is Steam storage), overwrite `out[0] = 0x1F`. The game then logs "No Character selected" and returns to the menu **before** any world or session starts. Arm it for EMF-initiated starts, or always in test builds.
6. Expected log lines: `[pregame] Start Play`, `[play] start creation step …`, `[play] Load local save Slot: '0 ( Local )'`.
7. (Confirmed in game.) The server world starts stepping about 1 s after the start, long before the save has loaded and the local player exists (12 s on the test world): wait for the local player before calling the world "running".

**What can go wrong.**
- **Wrong thread.** The queue is a plain slice: no lock, count++, offset bump. Posting from the bridge thread can corrupt it or race the consumer. Post only from inside the UI frame (the hook above), as the game's own buttons do.
  - The UI frame runs from the UI system update (`0x140a41570`/`0x140a44000`). That update is called from frame functions that reference "render_graph_frame" (0x1408557d0/0x140855970). Exactly which OS thread that is was not settled. Medium.
- **A session is running.** The play-state dispatcher `0x140852760` maps 0x11, 0x15, 0x16, 0x17, 0x26, 0x27, 0x28 and 0x2A to its default case, which is a plain `ret` (jump-table decode at 0x140854630/0x1408544c8; the default 0x140854482 restores registers and returns). Posting in game is therefore **ignored, not harmful** (High). Still gate on "menu active".
- **Too early.** If the slot lists are not loaded, `selectWorld` fails silently, and 0x28 then enters Start Play with no world (+0x3505A == 0, play data +0x12DE8 stays 0). That is a stuck or empty start; step 2's checks prevent it.
- **The UI changes the selection.** The character screen posts its own 0x11 when the user picks a character. EMF's verify step plus the safety net cover this.
- **Direct-To-Play exists but must not be used.** App creation step `0x140848130` case 0x1D has modes in gameApp+0x35478 (1 "Play%k" test character, 2 "Benchmark" offline, 3 "Stresstest" online). The value comes from app config +0x188 (copied in 0x1408397d0), and no command-line option sets it: the retail parser `0x1408fe880` only knows `+connect`, `+connect_lobby`, `+password`, `-username`, `--run-*`, `--vulkan-device-index`, `--large-pages`, `--wait-for-debugger`, `--agent-address` and `-log`. Mode 1 **creates or uses a character named "Play…" through createCharacter, which picks Steam storage when Steam is present**. Rejected.
- **Picking a LOCAL character by name.**
  - The game picks the character only through `selectCharacter`, i.e. cmd 0x11 with a slot id.
  - A call can name the Local slot explicitly: slot id `0x00(idx+1)`.
  - The name lives at character record +0x10, so EMF can enumerate the Local table (st = 0, idx 0..15, state 2/4), match the test character's name, and log it.
  - Recommendation: a config value `autostart.character = local:<index>`, together with the name check.

---

## Part 3: world display name (bonus, High)

- **Session world name:** `gameApp+0x34FE8 = {char* data; u64 len}`, not NUL-terminated.
  - playWorld (0x140835520) copies the selected world's save name there (`getSelectedWorldName 0x140dd1b40` returns world record `+0x58/+0x60`).
  - The play step uses it as the session name (0x14084a2c0, case 3: `local_628 = *(gameApp+0x34FE8)` when the length is nonzero).
  - For an adventure it holds the adventure name, and for Direct-To-Play the text "Direct To Play".
  - gameApp = `*(pregame+0x28)`, which EMF can capture with the 0x140868fb0 hook. The app object lives for the whole process.
- **Without gameApp, using the save hooks EMF already has:**
  - the bound slot is `u16 at save2+0xB7297` (EMF already reads this offset from code);
  - record = `save2 + 0x990 + (slot>>8)*0xE60 + ((slot&0xFF)-1)*0x170`;
  - name = `{*(char**)(rec+0x58), *(u64*)(rec+0x60)}`;
  - save2 = `*(inst+0x1C8)` in the load hook.
- The bridge `state` could also report `gameApp+0x34FA8` (mode) and `+0x34FC0` (area: offline/online).

---

## Confidence summary

| Claim | Conf. |
|---|---|
| attrFind signature, layout and thread safety; recipe copied from network_health_attribute | High |
| attrAdd behaviour (no rootId check, no clamp, no markWritten, formula recompute); has a twin; unique call-site signature | High |
| death and NetworkHealth pick up any Health write within a tick | High (death confirmed in game; NetworkHealth from asm) |
| HitCandidate layout and the fields the conclusion and applyHitToEntity need | High |
| Pending list cleared at tick start, so the hit must be queued from the `impact_hit_conclusion` pre-hook | Medium (registration-order reasoning); confirmed in game with two program pre-hooks |
| Kill credit through rootSourceId → ExperienceSource | Medium-High |
| Pregame command queue, command layouts, "Offline: Start" = 0x16 → 0x27{0} → 0x28 | High |
| UI ctx global valid only inside the UI frame; 0x140d8cd40 as the hook point | High |
| Exact OS thread of the UI frame | Medium |
| In-game posts are ignored by the play dispatcher | High |
| save2 and gameApp offsets listed above (1273043) | High (read from 1273043 code) |
| Which Health slot holds max (2 or 7) | Low (data-driven) |
