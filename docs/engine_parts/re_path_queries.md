# A creature's path query (build 1287369)

How the game fills the 0x110-byte path query for a walking creature, and where its values come from. EMF's
`path_find` (API v37) writes the same query; `EmfPathQuery.as_entity` (API v48) fills it with a creature's profile.

## The `pathfinding` program (0x14046b340)

After `PathQueryDefaults` (0x140982890: start search radius 2/2, search radius 4/2, everything else 0 or 1) it copies
from the creature's `DynamicPathFinder` (272 B) and its `PathFinder` template (20 B):

| query | from |
|---|---|
| +0x00 owner | the entity |
| +0x10 start | its `CurrentTransform` |
| +0x1C target | `DynamicPathFinder.targetPosition` |
| +0x28 priority, +0x29 keepLastResults | `queryPriority`, `keepLastResults` |
| +0x2C | `maxTargetNodeDistance`, squared |
| +0x30 | `maxVerticalDistance` |
| +0x36 targetType, +0x37 | `targetType`, `pathQueryCheck` |
| +0x38, +0x44 | `freePathCheckPosition`, `freePathCheckIgnoreEntityId` |
| +0x48 | `maxIterations` |
| +0x4C | `horizontalKeepDistanceDirection` |
| +0x54 moveTypes, +0x56/+0x57 jump heights | `moveTypes`, `maxJumpUpHeight`, `maxJumpDownHeight` |
| +0x58 | `keepDistance` |
| +0x5C path node rules | `pathNodeRequirements` |
| +0x64 target node rules | `targetNodeRequirements` with `useTargetNodeRequirements`, else `pathNodeRequirements` |
| +0x6C 20 costs (one per PathNodeType) | `pathNodeCostFactors` (17 floats), spread: none, none, tar, mud, road, roadCenter, fog, deadlyFog, lava, building, collider, walkablePlatform, noNpcMovement x3, npcAvoid, waterSurface, underWater, waterGround, closedDoor |
| +0xBC highest steps (terrain, building, collider: floats) | template `PathFinder.settings` |
| +0xC8, +0xC9, +0xCC | `checkFreePathToTarget`, `queryScoreType`, `queryOptions` |
| +0xD0, +0xE8 | `leashCenter`, `leashLength` |
| +0xF0/+0xF4 start search radius | template `PathFinder` |
| +0xF8/+0xFC search radius | `horizontalSearchRadius`, `verticalSearchRadius` |
| +0x100/+0x104 room across / up | `minHorizontalSpace`, `minVerticalSpace` |
| +0x108/+0x10C target room | `minTargetHorizontalSpace`, `minTargetVerticalSpace` |

The radii and rooms are `PathGridDistance` (half metres). The bit of a node rule mask and the index of a cost are the
`PathNodeType` (9 Building, 10 Collider, 11 WalkablePlatform, 16-18 water, 19 ClosedDoor; 2 tar, 7 deadly fog, 8 lava
by the cost spread).

## Where `DynamicPathFinder` comes from

`path_walking` (0x1404682ab) sets the room across to max(trunc(`PathWalker.horizontalRadius` x 2),
`DynamicPathWalker.minHorizontalSpace`), the target room to the same with `minTargetHorizontalSpace`, and the room up
from `PathWalker.height` alike. The rest comes from what the creature's behaviour asks for its current query, and it
changes with that, several times a second. Seen on a Vukah warrior:

| its state | moves | costs | other |
|---|---|---|---|
| Idle | walk | all 1 | node rules forbid 388 (tar, deadly fog, lava) |
| Roaming | walk | its roaming costs (road 0.3, roadCenter 0.25, tar, mud and lava 4) | target room 2, the target also forbids water |
| Guard (an order to walk somewhere) | walk + its extended moves (jump up, down, across) | its roaming costs | |
| Combat | walk, and every few steps walk + jumps (its jump move when the target seems out of reach) | all 1 | |

Jump heights stayed 0 throughout.

## The templates behind it

- `PathWalker` (92 B): `horizontalRadius`, `height`, `pathNodeRequirements`. The game paths a creature with this
  radius, not its body: a Vukah warrior 0 (a point), the Vukah brawler 0.5, both 2.0 high; a wolf 0 and 2.0.
- `Enemy01BehaviorState` (2520 B): `movementSetup.defaultMoveTypes` (walk for the Vukah and the wolf) and
  `extendedMoveTypes` (the warrior 0x1C, the brawler and the wolf 0x18), `openDoorSetup.canOpenDoors` (false for
  them), `pathNodeSetup` with `defaultCostFactors`, `roamingCostFactors` and `avoidWaterCostFactors`,
  `combatSetup.jumpMoveSettings` (`useJumpMoveIfTargetIsNotReachable`: the warrior yes, the brawler no).
- `Wildlife` (736 B): `movementSetup.defaultMoveTypes` and `pathNodeSetup`.
- `ActiveNpcState` (villagers, 456 B) and `Animal` (656 B): `pathNodeSetup` only, no move types. A villager's
  queries hold walk, the door move and jump down at most 1 m.
