# Companion orders and strength (in-game tests)

Build 1273043, local test world. Test console commands (not part of the shipped companion mod) sent events from the
companion mod's server program and read/wrote the companion's components. Body: `Enemy_Vukah_Melee` with `Faction` 11
(PlayerAlly), guarding the player (see `re_companions.md`).

## The companion body (`inspect`, 110 components, 27 template)
Relevant ones: `Level` (16 B, attribute: `dataStorage[0]` = level, `[1]` = 105), `NetworkLevel` (u16, what clients
see), `EnemyScaling` (template) / `DynamicEnemyScaling` (`lastLevel`, `lastDamageMultiplier`, `initialBase*`,
`nextUpdateTime`), `Health` (attribute, `dataStorage` 119 0 119 0 0 0 0 119 at level 1), `Stamina`, `Strength` /
`Dexterity` / `Intelligence` (all 0 for this body), `DamageMod` (0.75 0.75 1.0 0), `DamageSusceptibility`,
`BuffContainer`, `DynamicAggro` (2848 B, the threat table), `SelectedTargets` (2184 B), `ControlCombatTarget`
(target 0), `CombatTargetActor`, `ExperienceSource`, `RandomLoot`, `DoNotSave`. No `Inventory`, no `Equipment`.

## Orders
- **Attack that**: `AddThreatTargetEvent {attackerId = companion, targetId = X, threatValue 1000, threatPoolId 0}`
  sent from a server program. The companion switched to enemy state 6 and went for X: a spawned wild bunny
  (`Animal_Wildlife_T2_Bunny_AG2`, normally ignored) was hit for -48 and died ~23 s later ("1 xp gains"); the companion
  then went back to guarding the player. A fleeing target can escape: the guard leash keeps pulling the companion back
  (a second bunny ran for 90 s without a hit).
- **Stay here**: `StartGuardingEvent {guarding = companion, guarded = a static entity}`. With a spawned decoration
  barrel as the guarded entity (`0ce28db1-...`), the companion walked to it (state 20) and stayed there: it was 17.5-19.8
  m from the player after the player walked away. Guarding villager 1025 also worked (it followed the villager).
  The companion mod must not re-send "guard the player" meanwhile (it does every 3 s when the guard target is not the
  player).
- **Come back / follow**: `StartGuardingEvent` with the player again: from 17 m to 2.6 m within 10 s.
- `RegisterControlledCombatTargetEvent` and `RestoreEnemyHealthEvent` (heal over a duration) were wired but not tested.

## Strength: the level drives health and damage
- Writing `Level.dataStorage[0] = 10` and `DynamicEnemyScaling.nextUpdateTime = 0`: the next scaling update set
  `lastLevel` 10, `NetworkLevel` 10 and **Health 119 -> 417** (current and max); it stayed after later updates
  (`lastDamageMultiplier` stayed 1.0).
- The level-10 companion hit a hostile `Enemy_Vukah_Melee` for **-156** (-48 at level 1) and killed it in one hit.
- So a mod can make the companion stronger (gear, progression) by setting its level. Unknown: whether the game ever
  writes the level back (it did not within ~2 minutes), and how levels above the world's range behave.

## Other notes
- A hostile spawned with `spawn` appears ~2 m from the player and killed the level-1 test character in two hits
  (-48 each); the backpack bag was retrieved after the respawn.
