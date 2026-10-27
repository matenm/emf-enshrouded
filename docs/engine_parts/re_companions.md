# Companions: the engine's "guard" system (static + runtime)

Goal: a companion that follows the player and fights. The engine already has this as **guarding**: an enemy-AI actor (program `enemy01`) can guard another entity, following it within set distances and fighting near it. Existing companions are enemy-AI actors on the player's side (faction PlayerAlly / AutomatedPlayer), not villagers.

## The chain (High unless noted)
1. **Offer** — program `guard_interactions` `0x1402d1630` (per player). For the entity under the player's cursor (`SelectedTargets`) it offers a special interaction when all of these hold:
   - the player's optional `CallFactionsForHelp.callForHelpFactions` (+4, u16 bitmask) has the bit of the target's faction (Medium: the mask is read through the player's optional read slot);
   - the factions are friends (`0x140e89820`);
   - the target's template `Enemy01BehaviorState.guardSetup.canGuardPlayer` is set (template +0x596 = `guardSetup` +0x588, field +0x0e);
   - the target is within 3 m (squared distance ≤ 9.0).
   The offer is `SpecialInteraction` 3 (StartGuarding), or 4 (StopGuarding) when `DynamicEnemy01BehaviorState.guardEntityId` (+0x454) already equals the player.
2. **Accept** — program `player_control_action_sequence` `0x1402d77f0` (interact key) sends `StartGuardingEvent` / `StopGuardingEvent` and may write `CallFactionsForHelp` / `DynamicEnemy01BehaviorState` of the other entity.
3. **Guard** — program `enemy01` `0x1403688f0` consumes `StartGuardingEvent` / `StopGuardingEvent` (events-in) and drives the guard behavior with `guardEntityId` (+0x454), `guardingTargetState` (+0x458), `guardingTargetMovementTarget` (+0x45c), `currentGuardTargetDistance` (+0x468).

## Data
- `keen::enemy01::GuardSetup` (20 B, inside the `Enemy01BehaviorState` template at +0x588): `startDistance`, `targetDistance`, `maxArriveDistance` (floats), `guardParent`, `guardGroupLeader`, `canGuardPlayer`, `useMovementTarget`, `useTargetOffsetsWhileTargetIsMoving`, `predictTargetPosition`, `disableRunning` (bools).
- `guardParent`: guard the entity's parent. createEntity's SpawnRequest starts with a parent id (`+0x00`), so spawning an actor with the player as parent may make it guard the player without any interaction (not verified; EMF's spawn passes parent 0).
- 79 templates carry `canGuardPlayer` (EML export flag). Player-side ones: `Player_Companion_FlyingSkull`, `Player_Companion_FlyingSkull_Throwable_T4` (PlayerAlly), **`Player_Vukah_Melee`** (AutomatedPlayer, a humanoid melee Vukah; GUID `44604a7b-946b-4727-bb72-8c0eb3c08fa1`, has `DoNotSave`). Many wild animals and Vukah enemies carry the flag too (they guard their own leaders).

## Confirmed in game: a companion that follows
- **A mod-made companion works without the interaction.** The `companion` mod spawns a template and, from its own ECS program, sends `StartGuardingEvent{guardingEntityId = companion, guardedEntityId = player}` (API v3 `emit_event`). The guard handler `0x140353fb0` applies it without any other check. (Two other routes are not tested: a data-only EML template change, and spawning with parent = player plus `guardParent`.)
- **Template matters:**

| Template | Result |
|---|---|
| `FakePlayer_Vukah_Melee` (`9543cfb0-b4d7-408e-905c-ccb79a15136d`, faction Player) | **Follows**: when the player walks away it switches to enemy state 20, catches up and stays 1-3 m away; while the player stands still it idles/roams up to ~8 m away. Its hits do 0 damage (see the faction results below). |
| `Player_Companion_Skeleton_Melee_GemOfNecromancy` (`10f892be-2374-4d31-ae27-85e0a36f3853`, PlayerAlly) | Follows, but dies after ~55 s (summon lifetime). |
| `Player_Vukah_Melee` (`44604a7b-...`, AutomatedPlayer) | Dormant: has `PlayerInput`, so the game only marks it ready for gameplay once `PlayerInput.fromClientIsReadyForGameplay` is set (no client sets it). With that set by the mod it gets the guard target but stands still. |

- **Why a spawned actor can stay dormant:** `enemy01` skips an actor that is not `IsReadyForGameplay` (actor state flag 50) while its enemy state is not Invalid; spawned actors start in state `Spawning` (4). See ENGINE_MAP's "Confirmed in game" notes on events and the companion.
- Enemy state 20 is the guard/follow state here (the enum calls it Guard). It is requested in the enemy update; `FUN_140371e80` sets the roaming movement target from the guarded entity's position.
- `Faction` is a dynamic (per-entity, saved) component: a mod can turn a hostile guard-capable template (e.g. `Enemy_Vukah_Melee`) into an ally by writing it (below).
- Other candidates from EML's template export: `Enemy_Companion_Catdeer`, `Enemy_Companion_Bunny` (faction Player), `Player_Companion_Skeleton_Elite_Melee_GemOfGreaterNecromancy`, `Player_Companion_FlyingSkull*` (PlayerAlly).

## Confirmed in game: faction decides the damage
- `Enemy_Vukah_Melee` spawned with `spawn` and its **`Faction` (1-byte dynamic component, 3 = Vukah) rewritten right after the spawn** (same server callback; the component exists at once) accepts `StartGuardingEvent` (its template has `canGuardPlayer 1`), follows the player in enemy state 20 and never attacks the player.
- **Faction 7 (`Player`)**: it attacks hostiles, but every hit is `HitEvent.healthChange +0`, the same as `FakePlayer_Vukah_Melee` (faction Player). Player-faction attackers seem to use the player damage model (weapon/attributes), which these bodies lack.
- **Faction 11 (`PlayerAlly`)**: full damage. It hit a hostile `Enemy_Vukah_Melee` three times for -48 and killed it (twice, in two sessions); the hostile hit it twice (-48 each) and it survived. The game showed "Enemy slain, XP +50" to the player.
- The host's **client world** copy of the companion already had faction 11 when the mod looked (the write happened in the spawn tick, before its first replication); later rewrites were not needed.
- **A reload reuses the server world's address** (`000003006B0BED80` in both sessions), so a mod cannot tell sessions apart by the world pointer (a companion that tried saw its `respawn_seconds` path, not a reload). API v9 `server_session()` (bumped when the core sees a server world set up) tells them apart.
- A hostile spawned 15 m away inside the test building (+x) vanished within a minute (`destroy` found no such entity).

## Open points
- Across reloads: these bodies are not saved (`DoNotSave`), so a mod re-spawns the companion on `on_load`, or an EML patch removes `DoNotSave` from the template.
- Gear and stats: `Health`, `Stamina` and friends are in the generated headers. `put_items` works for bodies with an `InventorySetup`; neither Vukah body here has one, so gear needs another body or the equipment components.
- Tasks ("do player tasks"): beyond guarding, `enemy01` behaviors are driven by template data. Attack, stay and come back are in `re_companion_orders.md`; other commands such as "go there" are not mapped.
