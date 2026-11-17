# Loot Magnet (`loot_magnet`)

Walk around, and the loot comes to you without pressing E:
- the corpses of creatures killed near you are looted into you;
- items on the ground and collectibles come too (from 2.0);
- by choice also mushrooms, fireflies, wild plants, chests in ruins, and barrels, crates and urns to smash.

It runs where the world runs, so on the host or a dedicated server it works for **every player**, also those who
joined without EMF. Each player switches their own magnet.

It answers the auto-loot request (3.2K votes on the official feature board) for the case no other mod covers: Auto
Loot Plus and the others work for single player or the host only, and every player has to install them. 2.0 brings
Auto Loot Plus's kinds of loot to EMF, with EMF's own code.

## Switch it on

Loot Magnet is installed **switched off**. Switch it on in the Mods tab (the key under Esc) or in EMF.exe, then
restart the game. Once it runs, it is on for every player by default (**On for players who never switched it**), and
each player switches their own:

- `/loot.toggle` in the game's chat (or `loot.toggle` in the console): on / off; `/loot.toggle on`, `/loot.toggle off`;
- `/loot.status`: whether yours is on, and the rules.
- `loot.chests` (host, or the chat admins): how many chests the magnet emptied in this world. They are not opened
  again; `loot.chests forget` clears the list. The list belongs to the world's save slot: clear it when a world
  starts anew in the same slot (a wiped dedicated server), or that world's chests are skipped.

These two are commands for every player: they work from the chat on a dedicated server, and in a game you host with
`[chat] players=1` in `emf\emf_core.ini` (see `docs/modding/server.md`). Each player's choice is kept with the world,
by account (`emf\config\loot_magnet\<world slot>.txt`). The host can also bind a key (**Your key to switch it**).

## What comes, and what never does

Most creatures do not keep their loot: a moment after they die they fade away, and the game leaves their loot in a
**loot spot** where they died (the small glowing pile you open with E). The magnet takes that loot spot. A creature that
keeps its loot in its own body is looted from the body.

The loot of a kill comes to the nearest player whose magnet is on when all of these hold:

- the creature died (the game's `EntityDiedEvent`) at least **After the kill** ago, and less than **Looks for loot for**
  ago (15 s by default, at least 8 s: the loot appears 1-3 s after the death, so until then the magnet looks again
  every half second);
- the player is within **Reach** of the loot;
- the loot is in the body (dead, its health 0, still the creature that died: the same kind, within 5 m of where it
  died), or in a loot spot that appeared within 5 m of where it died, at or after the death by the game's clock (the
  loot spot's spawn time against the time of the death; a loot spot that was already there, from an earlier kill, is
  not this kill's);
- it holds something;
- with **What comes** = materials: every stack in it is a material or food, by its debug name: starting with
  `Material_`, `Food_` or `_Food_`, or a terrain block (`Build_TerrainMaterial_`); arrows (`Ammo_`) if **Arrows count
  as materials**. Loot with gear in it is left for you to open.

A kill without a hit counts too (the champion's `champion.kill`, or anything else that kills).

Never:

- a corpse twice;
- a player's body or grave, an ally's (the companion, summons), a farm animal, anything tame or with an owner, a
  villager, or anything they leave when they die;
- anything inside a base: within **Never within** metres of a flame altar (raids at your base are looted by hand);
- chests and props (they are not creatures that die, and a loot spot that was there before the kill is not its loot);
- a player's grave (the game's tombstone, which is kept with the world): a loot spot is never saved.

The loot moves with the game's own loot event, as when you press "take all": the stacks go into your inventory and
the game shows them. **A full inventory loses nothing**: the game moves the stacks one slot at a time, each in its own
inventory transaction (`docs/engine_parts/re_villages.md`, `immediate_loot_action`), so what fits comes and the rest
stays where it was; the host gets a note "the loot did not come (is your inventory full?)" (once per session).

Some loot sits in inventories the corpse or loot spot links, not in its own, and the game's loot event does not reach
those (Grave Keeper found that with tombstones). Plain stacks there are taken out and put into you; what does not fit
is dropped at your feet. **Gear there stays** (taking it out would lose its gems and perks): the log says so, the host
gets a note, and you open the loot by hand.

## Everything else near you (2.0)

Once a second, for every player whose magnet is on, Loot Magnet looks at what lies near them and takes it the game's
own way: the same loot event the game sends when you press E. Each group has its own switch and reach (0 = the
general **Reach**):

| Group (setting) | Default | What it takes |
|---|---|---|
| Items on the ground (`pick_ground`) | on | drops, sticks and stones after chopping or mining, what a smashed barrel drops, loose stones and twig piles, a bat's fur patch or a skeleton's bones, and the stones, ore, wood, meat and drinks that lie in ruins and camps; never keys, weapons or lore the game placed in the world |
| Collectibles (`pick_collectibles`) | on | the collection items lying in the world |
| Mushrooms (`pick_mushrooms`) | off | wild mushrooms |
| Fireflies and bait bugs (`pick_fireflies`) | off | fireflies and the other critters that are fishing bait |
| Wild plants (`pick_plants`) | off | herbs, flowers, bushes and wild crops you collect with E, and wild shiitake (it grows like a plant); a plant is gone once collected, as by hand |
| Chests in ruins (`open_chests`) | off | the chests the game placed in the world: it opens them as you would (the game rolls their loot), then empties them; a chest it emptied is remembered for that world (`emf\config\loot_magnet_chests`), so it is not opened again after a reload |
| Smash barrels, crates and urns (`smash`) | off | only those that drop loot: one hit with their own health; the drop lies on the ground, and Items on the ground picks it up |
| Smash bee hives (`smash_hives`) | off | experimental: one hit breaks a wild bee hive open; its honey and wax drop on the ground (Items on the ground picks them up) and **a wasp swarm may come out and attack** |

Never:
- **Inside a base:** within **Never within (m) of a flame altar**. Smashing keeps 10 m more away. So your chests and
  your buildings are never touched.
- **A crop:** the stage a planted crop shows is never harvested, wherever the farm is (taking it would end the plant).
- **Anything a player placed** is never opened, taken or smashed; **thrown animal bait** stays where you threw it.
- **A chest another player opened** stays theirs: Loot Magnet empties only a chest it opened itself, or one you opened.
  A chest that a player without the magnet stands nearer to is left to them, and so is a barrel.
- **A locked chest:** one that needs a lockpick or a key is never opened (that would use up the item); once a player
  opened it with the item, it is.
- **A quest item:** the game has no mark for them, so names that look like one (Quest, _NPC_, LoreText, MapPiece,
  Key_ or Prop_Component_ at the start) stay where they are. A chest that holds one is left to you.
- **Items on the ignore list:** **Never pick up**, item names as `items <words>` in the console shows them, comma
  separated. End one with * for every name that begins so (`Food_T1_raw_*`). A chest that holds an ignored item is
  left whole. The list cannot apply to wild plants without a pickup: their loot is rolled when they are taken.
- **Enemy spawners** (bone piles, spider eggs), nests, fog nest resources, the Flame Power capsules, trees and rocks;
  bee hives only with **Smash bee hives** on. Trees are never felled: a hit from a mod takes a tree away with only its
  resin, while the wood comes stroke by stroke from your own axe.

**What the game switched off:** a chest it emptied, or a swarm it switched off or put out, is left alone. Fireflies
come by day as well: the game offers them by day and by night (seen in game).

**A full bag:** a pickup comes only while you have an empty slot, so it always fits whole; plants and chests give what
fits and keep the rest. An item is tried 5 times, 2 s apart, then left until you come back.

## Settings (Mods tab, EMF.exe, or `set loot_magnet <key> <value>`)

| Setting | Default | What it does |
|---|---|---|
| Reach (`radius`) | 8 m | 2-20; also for every group whose own reach is 0 |
| Items on the ground, Collectibles (`pick_ground`, `pick_collectibles`) | on | with their own reach (`pick_ground_reach`, ...: 0-20, 0 = Reach) |
| Mushrooms, Fireflies and bait bugs, Wild plants (`pick_mushrooms`, `pick_fireflies`, `pick_plants`) | off | each with its own reach |
| Never pick up (`ignore`) | empty | item names, comma separated; `*` at the end for a beginning |
| Chests in ruins, Smash barrels, crates and urns, Smash bee hives (`open_chests`, `smash`, `smash_hives`) | off | each with its own reach |
| After the kill (`delay`) | 1 s | 0-10: the loot comes no sooner |
| Looks for loot for (`window`) | 15 s | 8-60 s after the kill; an older corpse is left to you |
| What comes (`categories`) | materials | materials (and food) only, or everything |
| Arrows count as materials (`include_ammo`) | on | |
| On for players who never switched it (`default_on`) | on | |
| Your key to switch it (`toggle_key`) | none | the host's own magnet |
| Never within (m) of a flame altar (`base_radius`) | 30 m | 10-100 (a flame altar of a higher level builds a bigger zone: raise it to match) |
| Log every decision (`debug_log`) | on | a log line per death noted and per corpse decided; a line for what each sweep took; once a minute, how long the sweep's look took |

## Why a corpse was not looted

With **Log every decision** (`debug_log`, on by default) `emf\emf_core.log` has a line for each death the magnet
noticed and for what became of each corpse:

- `[loot_magnet] death of 6938 noted at (3739.5, 841.0, 1473.5), 46 m from the nearest flame altar`
- `[loot_magnet] corpse 6938 looted into player 1 from the loot it left where it died (loot container 7012, 1.0 m)`
- `[loot_magnet] corpse 6938 looted into player 1 (1.0 m; interaction offer no)` (the loot was in the body)
- `[loot_magnet] corpse 6938 skipped: inside a base` (or: a player's or an ally's body, holds something that is not a
  material, already looted...; `skipped: the loot it left (loot container 7012) holds something that is not a
  material`)
- `[loot_magnet] corpse 6938 left: its time ran out while nobody with a magnet within reach (the nearest player with a
  magnet was 14.2 m away, reach 8 m)`
- `[loot_magnet] corpse 6938 left: no loot appeared within 15 s of its death (none on it, and no loot container within 5 m
  of where it died)`
- `[loot_magnet] corpse 6938: loot container 7012 (1.2 m) not taken: it was there before the death (an earlier kill's
  loot), 3.0 s before it` (or: too far from where it died; the game has not given it a spawn time (yet); taken for
  another kill): each loot spot near a kill that was not taken for it, once.
- `[loot_magnet] corpse 6938: nothing came from the loot event yet (try 1 of 5): sent again`: a corpse may become
  lootable a moment after it died, so a loot event nothing came from is sent again, every 2 s, up to 5 times (nothing
  moved, so nothing is taken twice).
- `[loot_magnet] corpse 6938: 1 gear stack(s) stay in it ...: open it by hand`: gear in a linked inventory.

Switch it off once you trust it: then only the looted lines are written.

## Good to know

- **Host or dedicated server only.** In someone else's game it does nothing (that game's host decides).
- **Loose drops** (separate pickups lying on the ground) are taken by **Items on the ground** (on by default, 2.0),
  not by the corpse part: a quest item or a key the game placed is never taken (see "Never" above).
- The log (`emf\emf_core.log`, lines `[loot_magnet]`) names every corpse looted and the player it went to.
