# Combat: factions, damage, health/attributes, buffs

Build 1269579, image base 0x140000000. Static analysis only. Signatures come from `sig.py` (first number = byte length, `??` = wildcard).

## Overview: how a hit becomes damage (server world)

1. `impact_simulation` (0x1403ef7a0) runs the data-driven **impact programs** (node graphs from game resources). Graph nodes call C++ **impact natives**. The `ApplyDamage` node calls `applyDamage` (0x1403d4d50).
2. `applyDamage` works out the final health/stamina change (damage types, armor, susceptibility, crit, block, faction modifiers). It does **not** touch Health. It fills a 0x150-byte **pending hit** record (a `HitEvent` plus extra fields) and queues it in `impact::ImpactSystem` (`allocPendingHit` 0x141216920).
3. `impact_hit_conclusion` (0x1403ed8f0, flag 0x4) reads all pending hits. It keeps only the strongest hit per (target, impactId), charges the attacker's mana cost, then calls `applyHitToEntity` (0x1403e8b40) for the target and walks up the `ImpactHierarchy` parents (at most 5 levels).
4. `applyHitToEntity` changes the Health (and Stamina, if blocked) attributes and sends the real `ecs::HitEvent` (0xF8 B). If health goes from >0 to <=0, it sets `WasKillingBlow` (0x2000).
5. HitEvent consumers include `destructible`, `healthbar`, `hit_history`, `call_factions_for_help`, `combat_experience_source`, `toggle_on_hit`, `server_player_control_hitevents`, and on the client `client_player_control_hitevents` and `set_low_health_color_gradient_factor`.
6. `death` (0x140273300) sees Health < 1, sets the actor's dead state and sends `EntityDiedEvent`.
7. `network_health_attribute` (0x140275bd0) copies Health into `NetworkHealth` (replicated) for clients.

Fall damage does **not** use this pipeline. `fall_damage_infliction` writes Health directly (see 3.8).

---

## 1. Factions

### 1.1 Types
- `keen::FactionType` (u8 enum): 0 Environment, 1 HitAll, 2 Fogger, 3 Vukah, 4 Scavenger, 5 Wildlife, 6 WildBeast, 7 Player, 8 AutomatedPlayer, 9 HitOthers, 10 Skeleton, 11 PlayerAlly, 12 FarmAnimal, 13 Cyclops, 14 Drak, 15 Ancients.
- `keen::FactionMask` = Bitmask16<FactionType>.
- `ecs::Faction` (1 B, dynamic, savegame_relevant): `+0 faction`.
- Related: `ecs::PacifiedState` (16 B: +0 canBePacified, +1 isPacified, +4 pacifiedBy, +8 pacifyEndTime), `ecs::Pacify` (server_only, +4 pacifyFactions mask), `ecs::CallFactionsForHelp` (+4 callForHelpFactions mask), `ecs::KillFaction` (server_only, +8 factionMask, +0xC playerMask), `ecs::AdminKillFactionCommand` (+4 factionMask), `BuffType.factionExcludeMask` (+0x5C), `HitEvent.targetFaction` (+0xF0), `ParriedProjectile.originalFaction`.

### 1.2 The relation logic is hard-coded (there is no data table)
No reflected type holds a faction-relation table. The rules live in small leaf functions near 0x140e89230. These leaf functions have no `.pdata` entry, so `funcs.callers` misses them. Use a raw E8 scan.

**isEnemyFaction(a, b)** (by value, `cl`=a, `dl`=b)
- Address 0x140e892f0. Signature `84 C9 74 56 8D 41 F9 A8 FA 75 11 80` (verified unique).
- Pointer variant **isEnemyFaction(const Faction* a, const Faction* b)**, where null means Environment: 0x140e89230, `48 85 C9 74 06 44 0F B6 01 EB 03 45 32 C0 48 85 D2 74 05 0F B6 0A EB 02 32 C9 45 84`.
- The rules are checked in this order (confirmed from the assembly):
  1. a = Environment: not an enemy.
  2. a and b both "player side" {Player 7, AutomatedPlayer 8, PlayerAlly 11}: not enemies. FarmAnimal is excluded from this set. The test is `(x-7)&0xFA==0 && x!=12`.
  3. a = HitAll: enemy of everything, including HitAll.
  4. a = b: not enemies.
  5. Fixed friendly pairs (symmetric): Vukah–WildBeast, WildBeast–Skeleton, WildBeast–Cyclops, WildBeast–Drak, Skeleton–Fogger, Cyclops–Wildlife.
  6. Anything else: enemy. This covers player side vs all monsters, FarmAnimal, HitOthers and Environment-faction targets, and all monster factions against each other except the pairs in step 5.
- Confidence: High.

**isFriendFaction(a, b)**: `a == b` or both player side.
- By value: 0x140e89880, `3A CA 74 1B 8D 41 F9 A8 FA 75 11 80` (verified).
- By pointer: 0x140e897e0, `48 85 C9 74 06 44 0F B6 01 EB 03 45 32 C0 48 85 D2 74 05 0F B6 0A EB 02 32 C9 44 3A`.
- Confidence: High.

**isFriendlyOrPacified(const Faction* a, const Faction* b, const PacifiedState* aState)**
- Address 0x140e89820, `48 85 C9 74 06 44 0F B6 09 EB 03 45` (verified).
- Returns true if isFriend(a, b) is true. Also returns true when a is not player side, b is Player or AutomatedPlayer, and `aState->isPacified` (+1) is set. So a pacified NPC counts as a friend of players.
- Callers: `call_factions_for_help`, `enemy_sensor`, `friend_sensor`, `guard_interactions`, `enemy01`, `server_player_ranged_weapon_sticky_aim`.
- Confidence: High for the logic, Medium that the third argument is PacifiedState (it matches the field layout and `call_factions_for_help` reads PacifiedState).

**isPlayerSideFaction(f)**
- Address 0x140e8b200, `8D 41 F9 A8 FA 75 08 80 F9 0C 74 03` (verified).
- Used by `flee_sensor` and `enemy_combat_targets`. Confidence: High.

**Who calls the pointer-based isEnemyFaction (0x140e89230)**: `flee_sensor`, `enemy01` (twice), `enemy_combat_targets` (twice), `player_target_selector`. So AI target choice uses the same rules.

**Who calls the value-based versions**: the impact natives `isEnemy`/`isFriend`/`filterFriends`, plus `client_cursor`/`server_cursor_no_prediction` (0x14048f060).

**Modding**: to change who fights whom, patch or hook 0x140e892f0 and 0x140e89230 together. Both start with a short `jz` in the first 5 bytes; MinHook relocates that correctly. For friend logic, also patch 0x140e89880, 0x140e897e0 and 0x140e89820. No inlined copies were found: the `(x-7)&0xFA` byte pattern only occurs inside these helpers.

### 1.3 Faction access in impact graphs
- **getEntityFaction(ImpactExecCtx*, EntityId)**: 0x1403dea90, `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B F1 8B DA`. Reads the entity's Faction component (component id `ctx[6][0x2C]`). If the entity has none, it follows another component (id `ctx[6][0x2D]`, EntityId at +4, probably parent/owner) and returns that entity's faction. Confidence: High for the logic, Medium for what the fallback component is.
- Impact natives (see section 2): `isEnemy` 0x1403e0930, `isFriend` 0x1403e0a00, `hasFaction` 0x1403e05d0, `filterFaction` 0x1403dac50, `filterFriends` 0x1403dae10, `entityInRangeWithFaction` 0x1403d9a70, `forEachEntityWithFactionInRange` 0x1403dc4b0, `destroyFactionQuery` 0x1403d9990 (logs "[impact] Could not Shutdown Faction Query"), `pacifyFaction` 0x1403e20e0, `activateCallFactionForHelp` 0x1403d8b70.
- `isEnemy` and `isFriend` have byte-identical bodies (only the call target differs), so a byte signature cannot tell them apart. Find them through the native registration pattern instead (section 2).
- Friendly fire is decided in the impact graph data: graphs call `isEnemy`/`filterFriends` before `ApplyDamage`. `applyDamage` itself only checks factions for base protection (3.1).

---

## 2. Impact natives (C++ functions called from impact graphs)

**registerImpactNatives**
- Address 0x1404949b0, `48 89 5C 24 20 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 20 E3 FF`.
- Registers 108 natives. Each one is `registerNative(registry, u32 FNV1a(NodeTypeName), fn, &nameSlice, u8 argCount)` = **0x141220f30** (`48 89 5C 24 18 48 89 74 24 20 89 54 24 10 55 57 41 56 48 8B`).
- Example: `edx = 0xB04D0E7A` = FNV1a("ApplyDamage"), argCount 0x19.
- Hash = 32-bit FNV-1a. It is also used for reflection type hashes, e.g. FNV1a("keen::ecs::Health") = 0xDC101091.
- **A robust way to find any native**: scan for `4C 8D 05 ?? ?? ?? ?? BA <FNV1a(NodeName)>`; the `lea r8` gives the function. Checked for IsEnemy (0x79B5A97B), IsFriend (0xAC31B17D), ApplyDamage, ApplyBuff (0x91F30B26), HasFaction, SetAttribute, ChangeAttribute.
- Confidence: High.

**Native calling convention**: `NativeResult* fn(NativeResult* out, u32** args, ImpactExecCtx* ctx)`.
- `*args` is an array of u32 slot handles; a handle of 0 means the input is not connected.
- Value address = `*(u16*)(ctx[0] + (h & 0xFFFF)*0x18 + 0xE) + ctx[2]`.
- `ctx[6]` = impact world context: component/singleton ids and registries. Known entries: `+0xD0` AttributeRegistry, `+0x80` BuffRegistry, `+0x110` (index 0x22) ImpactSystem, `+0x38` default source entity.
- `out`: byte +0 error code, +8 bool result.
- Confidence: High for the slot decoding, Medium for the context field names.

Combat-related natives (name, then address): getBuffTarget 0x1403de410, getHitTarget 0x1403ded00, isEnemy 0x1403e0930, isFriend 0x1403e0a00, getWeaponBaseDamage 0x1403dfe30, createDamageSetup 0x1403d9710, **applyDamage 0x1403d4d50**, setAttribute 0x1403e2bc0, applyAttributeValue 0x1403d45a0, changeAttributeValue 0x1403d8c80, revertApplyAttributeValue 0x1403e2580, fetchAttribute 0x1403d9b70, fetchOptionalAttribute 0x1403da4f0, hasFaction 0x1403e05d0, hasAttackCategory 0x1403e01f0, hasWeaponCategory 0x1403e0710, hasDamageType 0x1403e0410, **applyBuff 0x1403d4920**, hasBuff 0x1403e02e0, removeBuff 0x1403e23d0, forEachHitEvent 0x1403dc6b0, forEachHitEventInStream 0x1403dd020, SendBaseHitEvent 0x1403e2900, SendRevivedPlayerEvent 0x1403e2ad0, isConsecutiveHit 0x1403e0890, filterConsecutiveHits 0x1403da8d0, destroyEntity 0x1403d98f0, spawnEntity 0x1403e3960.

---

## 3. Damage

### 3.1 applyDamage (impact native for the `ApplyDamage` node)
- **Address** 0x1403d4d50. **Signature** `48 8B C4 4C 89 40 18 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 08 F9`.
- **Argument slots** (follow `impact_nodes::ApplyDamage`, 176 B):
  - [0] wasBlocked (output, bool)
  - [1] sourceId, [2] targetId, [3] hitPosition, [4] hitNormal, [5] hitDirection
  - [6] attackMod (float multiplier), [7] attackStaminaMod, [8] noiseRange
  - [9] manaCost, [10] specialAbilityGain, [11] damageScaleAttribute
  - [12] pushBack, [13] hitReaction
  - [14] damageSetup (13 floats: Cut, Pierce, Blunt, Fire, Ice, Fog, Lightning, Healing, Poison, Wood, Stone, Metal, Explosion)
  - [15] attackCategory, [16] weaponCategory, [17] materialId, [18] targetMaterialId
  - [19] canBeBlocked, [20] canCrit, [21] forceInterrupt, [22] visceralAttackType
  - [23] ignoreConsecutiveHits, [24] pushbackDirectionOverride
- **What it does**:
  - Reads about 30 attacker and target components: DamageMod, CritChance, CritDamageMod, DamageModAgainst, DamageModBackstab, DamageModSneak, DamageModSkillShot, MeleeDamageMod, RangedDamageMod, MagicDamageMod, ToolDamageMod, WeaponCategoryDamageMod, Armor, ArmorPoints, DamageSusceptibility, Block, Faction and others.
  - Scales the DamageDistribution and rolls crit.
  - Applies the per-faction "foe" modifier. The target faction picks an attribute of DamageModAgainst: Fogger 0x4E71115F, Vukah 0x90475703, Scavenger 0xC1A7DBD3, Wildlife/WildBeast 0x4F8EDE68, Skeleton 0x843E96B1, Drak 0x2822B2A8, Ancients 0x25B0743E.
  - Applies susceptibility, then armor.
  - Handles block: health damage becomes stamina damage and the WasBlocked flag is set.
  - Handles merciless/visceral kills.
  - Fills the pending hit and queues it.
- **Base protection**: if the target faction is Environment or FarmAnimal and the source lacks permission, it emits `ecs::ProhibitedHit` (0x746D2A02, 0x38 B) instead of a hit.
- **Confidence**: High for the role and queueing, Medium for the exact meaning of each modifier.

Damage helpers:
- **damageTotal(DamageDistribution*)**: 0x1403dec70, `F3 0F 10 09 0F 57 D2 F3 0F 10 59 1C`. Returns `round(|healing| - sum(all damage types))`. This becomes the signed healthChange (negative = damage). Confidence: High.
- **applyArmor(dist, armorPointsComp, armorComp, attrReg, ...)**: 0x1403d3f90, `4D 85 C0 0F 84 ?? ?? ?? ?? 48 8B C4 48 89 58 10`. Subtracts flat armor per physical and magical type (attribute ids 0xA4F76E0A, 0x9CE25B8B, 0x3B0CAAC9, 0xE2078DA9, 0xFD79BE67, 0x6BE8B435, ...) after a penetration fraction. Confidence: Medium.
- **applySusceptibility**: 0x1403d84b0, `48 8B C4 48 89 58 10 48 89 70 18 57 41 56 41 57 48 81 EC A0`. Uses DamageSusceptibility (19 floats). Reports weakspot/resistance flags. Confidence: Medium.

### 3.2 Pending hit record (0x150 bytes)
The first 0xF8 bytes have the same layout as `ecs::HitEvent`. HitEvent derives from BaseHitEvent:

| Offset | Field |
|---|---|
| +0x08 | targetTransform |
| +0x6C | position |
| +0x78 | normal |
| +0x84 | hitDirection |
| +0x90 | rootSourceId |
| +0x94 | targetId |
| +0x98 | flags (HitEventFlags) |
| +0xA0 | rootTargetId |
| +0xA4 | sourceId |
| +0xBC | **healthChange (s32, negative = damage)** |
| +0xC0 | staminaChange |
| +0xC4 | specialAbilityGain |
| +0xC8 | aggro |
| +0xCC | noiseRange |
| +0xD0 | hitCounter |
| +0xD4 | impactId |
| +0xD8 | weaponCategory |
| +0xDC | damageTypes (mask) |
| +0xDE | hitDirectionConfig |
| +0xDF | attackCategory |
| +0xE0 | hitReaction |
| +0xE2 | pushBackForce |
| +0xE8 | attackerActorStates |
| +0xF0 | targetFaction |

Extra fields after the HitEvent part:
- +0x140 manaCost (taken from the source's Mana attribute 0x6AA2574E; the hit is dropped if the source cannot pay)
- +0x144 kind byte (0 = damage hit from applyDamage; 1/2 = other kinds)
- +0x14C "emit HitEvent" flag (applyDamage sets it to 1)
- +0x14D "check already hit" flag

**HitEventFlags** bits:

| Bit | Value | Flag |
|---|---|---|
| 0 | 0x1 | WasBlocked |
| 1 | 0x2 | WasParried |
| 2 | 0x4 | WasBlockBroken |
| 3 | 0x8 | WasCrit |
| 4 | 0x10 | WasVisceral |
| 5 | 0x20 | WasBackstab |
| 6 | 0x40 | WasWeakspot |
| 7 | 0x80 | WasSkillShot |
| 8 | 0x100 | WasResistance |
| 9 | 0x200 | WasImmune |
| 10 | 0x400 | WasIneffective |
| 11 | 0x800 | HideIneffective |
| 12 | 0x1000 | HideEffective |
| 13 | 0x2000 | WasKillingBlow |
| 14 | 0x4000 | WasHealing |
| 15 | 0x8000 | WasTerrain |
| 16 | 0x10000 | WasBuilding |
| 17–21 | 0x20000–0x200000 | PileUp, Flattening, Overgrowth, Decay, Additive |
| 22 | 0x400000 | UseHitDirectionForPushBack |
| 23 | 0x800000 | ForceInterrupt |

**allocPendingHit(ImpactSystem*, u8 kind)**
- Address 0x141216920, `48 89 5C 24 08 57 48 83 EC 20 48 8B D9 0F B6 FA 48 81 C1 10`.
- Takes a record from the pool at `sys+0x110` (fallback `sys+0x138`) and sets +0x144 = kind. Returns the record (Ghidra shows `void` because of the tail call).
- Callers: applyDamage, actor_interaction 0x140264fb0, and the natives/helpers 0x1403d33f0, 0x1403d3550, 0x1403db830, 0x1403e1500, 0x1403e1900.
- Confidence: High.

**alreadyHit(ImpactSystem*, impactId, targetId)**: 0x14121eff0. Confidence: Medium.

### 3.3 impact_hit_conclusion (ECS program, server)
- **Address** 0x1403ed8f0. **Signature** `48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 68 FC FF`.
- Walks the pending-hit list (stride 0x150).
- Removes duplicates per impactId/target, keeping the largest |healthChange|.
- Checks the mana cost (+0x140) against the source's Mana and subtracts it.
- Calls applyHitToEntity for the target and then for each ImpactHierarchy parent while `CombatFlags.propagateHits` allows it (at most 5 levels).
- Confidence: High.

### 3.4 applyHitToEntity: **the best hook for changing or logging damage**
- **Address** 0x1403e8b40. **Signature** `4C 89 44 24 18 48 89 54 24 10 55 57 41 54 41 55 41 56 41 57 48 8D AC 24`.
- **C signature**: `void applyHitToEntity(bool* outPropagate, EntityHandle target, HitConclusionCtx* ctx, PendingHit* hit)`.
- **What it does**:
  1. Logs "[impact] Droped Hit" if the hit is invalid.
  2. Reads CombatFlags (+1 propagateHits, +2 handleHits). If `handleHits` is 0 it returns.
  3. If blocked or parried (flags & 3): applies `hit+0xC0` to the Stamina attribute 0x04B6AA8B. Reaching <= 0 sets WasBlockBroken (4).
  4. Applies `hit+0xBC` to Health attribute 0x8EB84995 through `attrAdd`. Going from >0 to <=0 sets WasKillingBlow (0x2000).
  5. If the entity has no Health but has `Destructible`: adds a Health component (type hash 0xDC101091). Its base max (0x59185EF5) is `Destructible.maxHealth` and current = clamp(max + healthChange).
  6. Emits `NoiseEvent` (0xD079D101) when noiseRange > 0.
  7. Emits `HitEvent` (0xCC0D57E0, 0xF8 B, copied from the record with the final values).
- **Hooking**: change `hit->healthChange` (+0xBC) and `hit->staminaChange` (+0xC0) before calling the original. Skipping the original cancels the hit. Server world only.
- **Confidence**: High.

### 3.5 HitEvent (ECS event, 248 B, hash 0xCC0D57E0)
Emitted only in applyHitToEntity (0x1403e8b40). `impact_system_prepare` (0x1403efa30) also references the type. It is the right thing to read for logging damage after the fact (healthChange, sourceId, targetId, flags).

### 3.6 destructible (program)
- **Address** 0x14032e5e0. **Signature** `40 55 56 41 54 41 55 41 56 41 57 48 8D AC 24 68`. Flags 0x4.
- For every HitEvent with WasKillingBlow whose target has `Destructible` and no `DisableDestructible`/`DismantleOverride`:
  - adds a `LifeTime` component (fade-out), or destroys the entity directly if it cannot fade (`destroyEntity` 0x140954250);
  - then runs loot/destroy feedback (0x1403b2120, 0x1403b5230).
- `ecs::Destructible` (server_only, 2 B): +0 maxHealth u16 ("Destroy entity if Health reaches 0").
- Confidence: High.

### 3.7 death (program) and EntityDiedEvent
- **Address** 0x140273300. **Signature** `40 55 56 48 8D 6C 24 E8 ?? ?? ?? ?? 01 00 00 41 B8 88 00 00` (contains a wildcard; re-check before use).
- For each entity with Health, reads attribute 0x8EB84995. If < 1 and the actor is not already in state 0xC or 7:
  - sets the actor dead bit (`Actor+0xBF8 |= 0x80`) and a pending flag (`Actor+0xEAA`);
  - emits `EntityDiedEvent` (0xD8EC982A, 16 B, +8 deadEntityId).
- Also handles some player respawn state (Actor +0xEA8/+0xEAB/+0xEAC).
- `actor_basics` (0x14025ef10) also emits EntityDiedEvent.
- Consumers: `trigger_knowledge_on_kill`, `combat_experience_source`.
- Confidence: High for the event, Medium for the actor state bits.

### 3.8 Fall damage (bypasses the hit pipeline)
- Components: `FallDamage` (server_only: +0 fallDamageStartDistance, +4 fallDamageLethalDistance, +8 fallDamageSequence) and `DynamicFallDamage` (+0 wasFalling, +4 fallStartAltitude, +8 detectedFallDistance, +0xC detectedFallDamagePercentage).
- Programs: `fall_damage_detection` 0x140273b00, `fall_damage_calculation` 0x140273960, `fall_damage_infliction` **0x140274070** (`40 55 41 57 48 8D AC 24 08 FF FF FF`), `fall_damage_init` 0x1402745c0, `actor_teleport_fall_damage` 0x140270970.
- Infliction: `hp = max(0, floor(hp - maxHp(0xDC8358BE) * pct))`. It writes the Health slot directly, then calls `attrRecalc` (0x1402318d0). Then it flips the sign of the percentage so it is applied only once.
- Confidence: High.

---

## 4. Health and the attribute system

### 4.1 Layout
Health, Mana, Stamina, DamageSusceptibility, Revive, Armor and about 50 other components are **AttributeComponents**. All are `server_only` and `do_not_save`.

| Offset | Field |
|---|---|
| +0 | u32 rootId (attribute root type, HashKey32) |
| +4 | u16 storageOffset (offset of the value array from the component start, normally 8) |
| +6 | u8 flags (bit0 Initialized, bit1 Break_OnWrite) |
| +7 | u8 storageCount |
| +8 | `dataStorage[]` |

- `ecs::Health` = 40 B: header + `StaticArray<sint32,8>`.
- `DamageSusceptibility` = 84 B: header + 19 floats. `Revive` = header + 6 floats.

Which slot holds what is decided by the **AttributeRegistry** (built from resources). Never hard-code slot numbers; look the slot up by attribute id.

Known attribute ids (HashKey32; the names are not recovered):

| Id | Meaning | Evidence | Confidence |
|---|---|---|---|
| 0x8EB84995 | Health current | death, NetworkHealth.health, applyHit | High |
| 0xDC8358BE | Health max (computed) | NetworkHealth.healthMax, fall damage | High |
| 0x59185EF5 | Health max input/base | set from Destructible.maxHealth, health_max_scaling | Medium |
| 0x04B6AA8B | Stamina current | block path in applyHit | High |
| 0x6AA2574E | Mana current | manaCost path, attribute_save | Medium |

### 4.2 Functions
**attrFind(out, u32 attrId, AttributeRegistry* reg)**
- Address 0x14121bea0, `89 54 24 10 53 48 83 EC 20 48 8B D9 48 8D 54 24 38 49 8D 48`.
- Looks `attrId` up in the hash map at `reg+0x68`.
- Result: `out[0]` = error (0 ok, 0x1F not found), `*(AttrInfo**)(out+8)`. AttrInfo has `+0` rootId and `+0xC` u16 slot index.
- 177 callers. Confidence: High.

**attrFindRoot(out, u32 rootId, reg)**
- Address 0x14121bef0, `48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 83 78 64 00`.
- Returns the root's AttributeStructure list (used for recalculating formulas). Confidence: High.

**attrAdd(AttributeComponent* c, reg, s32 delta, u32 attrId)**
- Address 0x1403e83a0, `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 60 41 8B F8 48 8B DA 4C 8B C2 48 8B F1 41 8B D1 48 8D 4C 24 20 E8 D3 3A E3` (the trailing rel32 bytes are not wildcarded; trim them).
- `slot += delta`, then re-runs every formula of the root (bytecode `AttributeOps`, evaluator 0x14021c800). There is no clamping here; clamping comes from the formulas.
- Only caller is applyHitToEntity. Confidence: High.

**attrRecalc(AttributeComponent* c, reg)**
- Address 0x1402318d0. Not unique by bytes (74 callers; probably several identical copies).
- Same recalculation loop as attrAdd. Called after direct writes. Confidence: Medium.

### 4.3 How a mod reads or writes current/max health (server world)
1. Get the entity's `Health` component (type hash 0xDC101091) and the `impact::AttributeRegistry` singleton. The registry is also at impact world context +0xD0.
2. `info = attrFind(reg, 0x8EB84995)`. Check `info->rootId == comp->rootId` and `comp->flags & 1`.
3. Read: `cur = *(int32*)((u8*)comp + comp->storageOffset + info->slot*4)`. Do the same with 0xDC8358BE for max.
4. Write: either call `attrAdd(comp, reg, delta, 0x8EB84995)`, or set the slot and call `attrRecalc`.
- Health <= 0 is picked up by `death` on the next tick. `network_health_attribute` replicates the new value.

### 4.4 Client-side health and related components
- **NetworkHealth** (8 B, dynamic, not saved): +0 health u32, +4 healthMax u32. Written by `network_health_attribute` (0x140275bd0, `4C 89 74 24 20 55 48 8D 6C 24 A9 48 81 EC A0 00 00 00 41 B8 48 00 00 00`). This is the client-readable health. Confidence: High.
- `NetworkPlayerAttributes`: written by `network_player_attributes` 0x140275f20 (all player stats).
- **StoreAttributes** (server_only, savegame, player): +0 health, +4 stamina, +8 mana, +0xC bodyHeat, +0x10 oxygen (floats), +0x14 restoreAttributes. Program `attribute_save` 0x1402ce750 writes both StoreAttributes and the live attributes. Probably it stores on save and restores on load when `restoreAttributes` is set. Confidence: Medium.
- **PlayerLowHealth** (16 B, client UI): +0 blendOutDuration, +8 lastTriggerTime. Written by `set_low_health_color_gradient_factor` 0x140312a60 (client, 0x201) from NetworkPlayerAttributes on HitEvent. UI only.
- `HealthRecharge` (server_only): used by `health_recharge` 0x1402361c0. `health_max_scaling` 0x140235fe0 recomputes max from Constitution. `health_init` 0x14122e120 is the init program.
- **CombatFlags** (server_only, 12 B): +0 immune, +1 propagateHits, +2 handleHits, +3 canBlock, +4 ignoreMagic, +5 ignoreMelee, +6 ignoreRanged, +7 ignoreSkillShot, +8 ignoreTool, +9 ignoreOther, +0xA ignoreAoE, +0xB canBeBackstabbed. Setting `immune` or clearing `handleHits` is a cheap god-mode switch.

---

## 5. Buffs

### 5.1 Types
- **BuffContainer** (472 B, dynamic, savegame):
  - +0x000 foodBuffs[4], +0x030 genericBuffs[16], +0x0F0 genericDebuffs[16], +0x1B0 restedBuff[1], +0x1BC terrainDebuff[1] (all `BuffInstance`)
  - +0x1C8 hungerStartTime, +0x1D0 extendedFoodBuffSlots
- **BuffInstance** (12 B): +0 buffEntityId, +4 typeId (BuffTypeId), +8 cleansed. Every active buff is its own **entity**.
- **BuffComponent** (60 B, server_only, savegame; lives on the buff entity): +0 buffTypeId, +4 damageSetup (DamageSetup, 52 B), +0x38 isDamageSetupSet.
- **BuffNetworkData** (24 B): +0 lifeTime, +8 endOfLife, +0x10 buffTypeId, +0x14 level. Written by `buff_network`; used for client display.
- **BuffType** (184 B, resource):
  - +0 buffTypeId, +4 slot (BuffSlotType: Food, Buff, Debuff, Rested, TerrainDebuff), +5 applyType (Invalid, ResetTime, ReplaceAtEnd, Replace), +8 defaultLifeTime
  - +0x49 despawnOnDeath, +0x4A isSaved, +0x4C impact (ImpactProgramReference), +0x5C factionExcludeMask, +0x60 impactValues
  - +0x78 onCreate, +0x80 whileApplied, +0x88 onTimeout
- **AppliedBuffEvent** (24 B, hash 0x690CBB0B): +8 targetEntityId, +0xC slotType, +0x10 slotTypeCount.
- `actor::ApplyBuffEvent` (192 B, actor sequence event): +0x60 buffTypeOverride, +0x70 consume, +0x84 damageDistribution. `actor::CleanseBuffEvent`.
- `impact_nodes::ApplyBuff` (96 B): buffInput, targetEntityId, damageSetup, usedItem, durationInput.

### 5.2 Functions
**applyBuff (impact native)**
- Address 0x1403d4920, `4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 80 48 81 EC`.
- Argument slots: [0] buff ref (16 B guid), [1] targetEntityId, [2] damageSetup, [3] source override, [4] duration.
- Resolves the BuffType through **getBuffType(id, BuffRegistry)** (0x140e729d0, `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 85 C9 74 73`; BuffRegistry at impact ctx[6]+0x80). If that fails it logs "[apply buff] Unknown Buff '%k'".
- Then calls the core function below. Confidence: High.

**applyBuffCore: the central "apply a buff" function**
- Address **0x1403acd70**, `48 8B C4 48 89 58 10 4C 89 48 20 48`.
- C signature: `u8* applyBuffCore(u8* outErr, u32 targetId, u32 sourceId, const BuffType* type, ApplyBuffCtx* ctx)`.
- Steps:
  1. Returns 0 without doing anything if the target is dead and `despawnOnDeath` is set.
  2. Gets the target's BuffContainer. Skips if the target's Faction bit is in `type->factionExcludeMask`.
  3. Finds the slot (0x1403b0020).
  4. Handles the reapply type: ResetTime updates the lifetime of the existing buff; Replace/ReplaceAtEnd destroy the old buff entity.
  5. Spawns the buff entity (0x140953770) with BuffComponent, LifeTime and so on, and stores it in the BuffInstance.
  6. Sends AppliedBuffEvent (**0x1403b7430**, `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B B9 E0`) and the buff init (0x1403ac580). Emits `NotifyEvent`s for UI/VFX.
- Error codes: 0 ok, 0x11 not allowed, 0x12 no target/type, 0x14 no slot, 0x15 missing entity.
- Callers: programs `actor_apply_buff`, `update_buffcontainer`, `thirst_and_hunger`, `daytime_temp`, `actor_wet_soaked_apply`, `movement_collider`, and the impact native.
- Confidence: High for the role, Medium for the error-code meanings.

`CreateBuffRegistry` is only a job-name string (getter at 0x14083e830, next to "CreateAttributeSystem" and "WaitAttributeSystemResource"). The registry itself is the ECS singleton `BuffRegistry`. No code refs to the getter were found.

---

## 6. Server vs client

- All core combat state is **`server_only`**: Health and every other AttributeComponent, DamageSusceptibility, CombatFlags, Destructible, FallDamage, BuffComponent, KillFaction, StoreAttributes, Pacify. A client world does not have them. So damage, death and buffs are **server-authoritative**.
- Clients see replicated mirrors: `NetworkHealth`, `NetworkPlayerAttributes`, `BuffNetworkData`, plus HitEvents for feedback (`client_player_control_hitevents` flag 0x1, `set_low_health_color_gradient_factor`, `fill_ui_healthbar`, `hitstop_effect`, all 0x201).
- Server-side programs (flag 0x4 or touching server_only components): `impact_hit_conclusion` 0x4, `impact_cleanup` 0x4, `impact_system_prepare` 0x4, `clear_hit_candidates` 0x4, `server_update_impact_system` 0x1004, `destructible` 0x4, `healthbar` 0x4, `toggle_on_hit` 0x4, `combat_experience_source` 0x4, `trigger_knowledge_on_kill` 0x4, `impact_simulation` 0x1200, `death` 0x0, `fall_damage_*` 0x0, `update_buffcontainer`/`actor_apply_buff` 0x210.
- A mod that hooks the damage functions should check `world+0xE15DC8 == 0` (server world). In a joined online game the local GameServer is not the authority, so hooks there have no effect on the real game state. The host's machine runs the real pipeline.

---

## 7. Relevant ECS programs

| Program | Update fn | Flags | Role |
|---|---|---|---|
| impact_initialize | 0x1403eed40 | 0x200 | sets up Impact (reads Faction, DamageMod, BuffComponent) |
| impact_prepare | 0x1403ef6e0 | 0x0 | reads all attribute components |
| impact_simulation | 0x1403ef7a0 | 0x1200 | runs impact graphs, so the natives (applyDamage, applyBuff) run here |
| impact_hit_conclusion | 0x1403ed8f0 | 0x4 | applies pending hits |
| impact_system_prepare / clear_hit_candidates / impact_cleanup | 0x1403efa30 / 0x1403ed770 / 0x1403ed7d0 | 0x4 | ImpactSystem housekeeping |
| server_update_impact_system | 0x1403f0340 | 0x1004 | ImpactSystem tick |
| resource_conclusion | 0x1403efac0 | 0x4 | emits ResourceChangeEvent |
| death | 0x140273300 | 0x0 | Health < 1 → EntityDiedEvent |
| destructible | 0x14032e5e0 | 0x4 | destroys Destructible entities on killing blow |
| fall_damage_infliction | 0x140274070 | 0x0 | writes Health directly |
| health_recharge / health_max_scaling / health_init | 0x1402361c0 / 0x140235fe0 / 0x14122e120 | 0 / 0 / 0x8 | regen, max from Constitution, init |
| network_health_attribute | 0x140275bd0 | 0x0 | Health → NetworkHealth |
| attribute_save | 0x1402ce750 | 0x0 | StoreAttributes ↔ live attributes |
| hit_history / healthbar / toggle_on_hit | 0x140333370 / 0x140275070 / 0x140348440 | 0 / 4 / 4 | HitEvent consumers |
| call_factions_for_help | 0x14032b340 | 0x0 | HitEvent → CallForHelpEvent (uses 0x140e89820) |
| combat_experience_source / trigger_knowledge_on_kill | 0x14032c4a0 / 0x140416760 | 0x4 | XP and knowledge on hit/kill |
| server_player_control_hitevents / client_player_control_hitevents | 0x1402e5cb0 / 0x1402afd40 | 0x200 / 0x1 | hit reactions |
| actor_apply_buff / update_buffcontainer / rested_buff / load_buff / buff_network | 0x14025e4a0 / 0x14023d410 / 0x140250a90 / 0x14023cbd0 / 0x14023c950 | 0x210 / 0x210 / 0x210 / 0 / 0 | buffs |
| copy_parent_damage_susceptibility | 0x14032d510 | 0x200 | |
| enemy_combat_targets / player_target_selector / enemy_sensor / friend_sensor / flee_sensor / enemy01 / guard_interactions | 0x140379210 / 0x140477850 / 0x14032ed20 / 0x140331d30 / 0x140331060 / 0x1403688f0 / 0x1402d1630 | 0x0 | use the faction helpers |

There are also about 30 `*_init` programs for attribute components (e.g. `damagesusceptibility_init` 0x14122d5e0, `damagemod_init` 0x14122d040), all with flag 0x8.

---

## 8. What a modding framework should hook or call

| Goal | Hook or call |
|---|---|
| Change or log all combat damage | Hook **applyHitToEntity 0x1403e8b40**; edit `hit+0xBC`/`+0xC0` |
| Change how damage is computed (before dedup) | Post-hook **applyDamage 0x1403d4d50** |
| Observe damage afterwards | Read HitEvents (or hook just after the HitEvent emit) |
| Fall damage | Hook `fall_damage_infliction` 0x140274070 (not covered by the hooks above) |
| Death | Hook `death` 0x140273300, or consume EntityDiedEvent |
| Faction relations | Patch 0x140e892f0 + 0x140e89230 (enemy), 0x140e89880 + 0x140e897e0 + 0x140e89820 (friend) |
| Read/write health | `attrFind` 0x14121bea0 + `attrAdd` 0x1403e83a0, attribute ids from 4.1 |
| God mode | `CombatFlags.immune` / `handleHits` |
| Apply a buff | Call **applyBuffCore 0x1403acd70** with a BuffType from `getBuffType` 0x140e729d0. The ApplyBuffCtx is about 0x90 bytes of component ids (see the impact native for how it is filled), so wrapping the impact native is easier |
| Enumerate impact natives | Parse 0x1404949b0, or use the `BA <FNV1a(name)>` pattern |

---

## 9. Open questions

- Attribute id names: they are HashKey32 values, and FNV-1a of obvious names did not match. They probably come from the attribute resource's `debugNames`.
- Exact meaning of the impact world context fields (`ctx[6]` indices 0x2C Faction, 0x2D parent link, 0x22 ImpactSystem, 0x1A AttributeRegistry, 0x10 BuffRegistry; the rest are unmapped). The table is filled in 0x1404949b0 through `FUN_1409d50d0` name lookups and could be parsed.
- Whether a client world ever runs `impact_hit_conclusion` (prediction world, kind 1).
- Full ApplyBuffCtx layout, and the meaning of applyBuffCore return codes 0x11/0x15.
- Which component is the `[0x2D]` fallback in getEntityFaction (owner or parent).
- Flag 0x4 is assumed to mean server-only from the program contents; it was not confirmed in the scheduler. `registerProgram` only interprets 0x2 (disabled) and 0x8 (init).
