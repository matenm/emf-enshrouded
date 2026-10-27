# Quick Stack and Chest Machines (`quick_stack` 1.2)

**Tap L**: what you carry goes into the chests within 10 m that already hold the same item. **Hold L at a chest**:
make it a sorting chest, a recycler or an overflow chest. The official board asks for an auto-sorting chest (347
votes) and for salvaging crafted items (604); Nexus has backpack sorters that work only for the player who has them
installed, but no input chest that players without mods can drop into, and no recycler.

It runs where the world runs, so on the host or a dedicated server the chests work for every player, also those who
joined without EMF: they just drop things in through the game's own chest window.

## Quick stack

- **Tap the key** (L by default): each stack you carry goes into the nearest chest within **Chest radius** that
  already holds that item, through the game's own inventory transactions, as much as it has room for (free slots and
  its part-filled stacks of that item); the rest goes to the next nearest chest that holds it, or stays with you.
  The stack sizes are read again every 10 s and in each new world, so a Big Stacks change counts soon after.
  "Quick stack: 901 item(s) into 1 chest(s)". At most 24 stacks move a press (the game's item queue is small); the
  note then says how many are left: press again for the rest.
- Never moved: gear with its own data (level, perks, gems), your action bar (**Leave the action bar alone**), arrows
  (**Keep arrows**), food (**Keep food**) and items whose name holds a word of **Never put away** (`torch, resin`).
  Sorting chests and recyclers never receive quick-stacked items (they pass things on), and neither do tombstones
  or loot containers (in the game a tombstone looks like a storage too, but it vanishes when emptied or when its time
  runs out).
- **Only at a base**: quick stack works only within **Base radius** of a flame altar.
- `/qs` (or `/quick_stack`) does the same for the player who types it, also from the game's chat on a dedicated
  server or a game you host with `[chat] players=1` (`docs/modding/server.md`). (`/stack` is taken: the developer
  command that sets an item's stack size.)

## Chest Machines

**Hold the key** while looking at a chest (within 8 m): a wheel opens; release over your choice.

| Wheel entry | What it does |
|---|---|
| Quick stack now | as a tap |
| Make this a sorting chest | every **Sorting chests put away every** seconds (10), each plain stack in it goes to the nearest chest within **Into chests within** (30 m) that already holds that item, as much as it has room for. What nobody holds stays (or goes to an overflow chest). Gear never moves. At most 16 stacks move per round; the rest go in the next. |
| Make this a recycler | with **Recyclers** on (off by default): every **Recyclers work every** seconds (15), a crafted item in it goes back into **Materials back** % (50) of its recipe's materials. The materials never stay in the recycler (it would recycle them again, losing more each time): they go to the nearest sorting chest in reach that has room for them (**Materials go to a sorting chest**), else the nearest chest in reach that holds the first material, else the nearest chest in reach with room |
| Make this an overflow chest | with **Overflow chests** on: takes from a sorting chest what no chest holds |
| Make this a normal chest | takes the mark off |

Marks are kept with the world by the chest's place (`emf\config\quick_stack\<world slot>.txt`), so they survive
reloads. A marked chest that was taken down loses its mark once it was missed for 30 s while a player was within 40 m
of its place, and never in the first 30 s after a load (a place far from every player may just not be loaded, so its
mark stays). A mark belongs to a chest, not only to a place: when another chest turns up at a marked place in the same
session (the marked one was taken down and a new one built there), the mark is dropped and the host told, so a new
chest never starts sorting or recycling on its own. After a load the first chest found at the place takes the mark.

Nothing moved is lost: what does not fit goes back where it came from; when that chest or player is gone (taken
down, left the game), it is dropped at the other end of the move, else beside the nearest player, and the log says
where. A move answered after the world changed (a reload, another world) touches nothing.

### The recycler's rules

- batches = the stack's count / what the recipe makes at once; each material comes back as
  floor(batches × its count × refund / 100). **Never more than the item cost**: refund at most 100 %, rounded down.
  4 wooden walls of 4 logs and 1 resin each at 50 %: 8 logs and 2 resin.
- It is taken first, all or nothing, and only when the target has room for the materials (counted in full stacks);
  anything that still does not fit is dropped beside the target (or the recycler, or the nearest player, when the
  target is gone), never lost. A recycler starts its next stack only after every material has landed. One take or put carries at most 65,535, so a
  huge stack (Big Stacks) is recycled in parts.
- It **leaves alone** (once noted in the log): gear (salvage it for runes at a workbench, as vanilla does), items no
  recipe makes, upgrades, cooked food, recipes that take "any" of a kind, recipes that make several things, too few
  for one batch, too few to give anything back at this refund, and **items that several recipes with different
  materials make** (refunding one of them could turn one material into another).
- The recipes are the game's own (`g38_items::RecipeRegistry`), read once per world.

## Commands

| Command | Who | What |
|---|---|---|
| `/qs`, `/quick_stack` | every player | quick stack for yourself |
| `/qs.chests` | every player | the sorting chests, recyclers and overflow chests within 50 m |
| `qs.mark sort\|recycle\|overflow\|none [chest]` | host, admins | mark the chest you look at, or the chest entity given (`near` lists them; from the chat without one: the nearest within 3 m) |
| `qs.list` | host, admins | every marked chest of this world |

## Settings (Mods tab, EMF.exe, or `set quick_stack <key> <value>`)

| Setting | Default | What it does |
|---|---|---|
| Quick stack key (`key`) | L | tap: quick stack; hold at a chest: the chest wheel |
| Chest radius (m) (`radius`) | 10 | 1-50 |
| Leave the action bar alone (`keep_action_bar`) | on | |
| Keep arrows (`keep_ammo`), Keep food (`keep_food`) | off | |
| Never put away (`exclude`) | empty | parts of item names, separated by commas |
| Only at a base (`only_in_base`), Base radius (`base_radius`) | off, 40 m | |
| Sorting chests put away every (s) (`sort_seconds`) | 10 | 5-120 |
| Into chests within (m) (`sort_radius`) | 30 | 5-60 |
| Overflow chests (`overflow`) | off | |
| Recyclers (`recycler`) | **off** | |
| Materials back (%) (`refund`) | 50 | 10-100 |
| Recyclers work every (s) (`recycle_seconds`) | 15 | 5-120 |
| Materials go to a sorting chest (`recycle_to_sorter`) | on | |
| Notes on screen (`notes`) | my quick stacks and marks | none / mine / all (also what the chests do) |

**From 1.1:** `key`, `radius` and `keep_action_bar` keep their values (the same keys in `emf\config\quick_stack.ini`,
now a key binding, a slider and a switch).

## Good to know

- Any storage counts as a chest, crafting stations and feeders too (a kiln that holds logs as fuel also receives logs).
- The rules are in `quickstack.h` (unit-tested in `tests/test_quick_stack.cpp`); the game side in `qs_glue.h`.
- The log (`emf\emf_core.log`, lines `[quick_stack]`) has every mark, every sorting pass that moved something, every
  recycling and every item a recycler left alone.
