# Grave Keeper (`grave`, Lua)

Died far from home? `/grave.where` says where your tombstone is, and `/grave.claim` brings its items back to you
from a distance. The official board's "claim tombstone" request has 58 votes and 109 comments; no Nexus mod does it.

It runs where the world runs, so on the host or a dedicated server it works for every player, also those who joined
without EMF.

## Switch it on

Grave Keeper is installed **switched off**: it softens the death penalty for every player. Switch it on in the Mods
tab (the key under Esc) or in EMF.exe, then restart the game (or the server).

## Commands

| Command | Who | What |
|---|---|---|
| `/grave.where` | every player | "Your tombstone: 230 m NE, 12 m below, 3 stacks. /grave.claim brings them back (within 150 m)." |
| `/grave.claim` | every player | the stacks of your newest tombstone come to you |
| `grave.list` | host, admins | every known tombstone, its owner (account) and its distance |
| `grave.adopt` | host, admins | the nearest tombstone within 10 m becomes yours (for tombstones from before Grave Keeper ran) |

The commands for every player work from the chat on a dedicated server, and in a game you host with
`[chat] players=1` in `emf\emf_core.ini` (see `docs/modding/server.md`). The host also sees the way to their own
tombstone on screen ("Tombstone: 230 m NE (3 stacks)").

(Lua mods put their id in front of every command, so the commands are `/grave.where` and `/grave.claim` rather than
a bare `/grave`.)

## Only your own tombstone

The game's tombstone carries nothing a mod can read that names its owner (in game it has `CreateItemLoot`,
`DestroyOnLoot`, a `LifeTime`, an `InventorySetup` and a `CustomPlayerString` whose meaning is unknown). So Grave Keeper
decides by where and when:

1. When a player dies (the game's `EntityDiedEvent` for a player), it notes their account and where they died, and
   which tombstones already lie within 10 m.
2. A tombstone that **appears** within 10 m of that spot, was not there at the death and belongs to nobody yet, goes
   to the waiting death nearest to it (whoever died first). The game leaves the tombstone when the player respawns,
   so a death waits until its player has been back up for a minute (at most 30 minutes, for a player who left on the
   death screen). Its place is kept with the world (`emf\config\grave\<world slot>.txt`).
3. Only those can be claimed, and only by their owner. A tombstone that was there before (another player's, an
   older one), one far from your death, or one Grave Keeper never saw appear is never yours.

So deaths from before Grave Keeper ran (or while it was off) have no owner: the host or an admin can hand one over
with `grave.adopt` (stand within 10 m of it). When two players die close together, each tombstone goes to the death
nearest to it; one about as near to two players' deaths (less than 4 m nearer to one than to the other) is nobody's
until `grave.adopt`. A tombstone that was once yours and was trimmed off the list (below) never becomes anyone's
again.

After a reload entity ids change, so a tombstone is found again by its place (within 1.5 m). The newest **Tombstones
kept per player** (3) are remembered; a tombstone that is gone (looted, or the game removed it when its time ran out)
is forgotten once fresh looks miss it for 10 s while a player is within 50 m of its place, and never in the first 30 s
after a load. A claim that emptied it forgets it at once.

The world is searched for tombstones a slice at a time (1,000 entities a server step), never all at once, so a
command may answer from the last search ("as last searched; the world is searched again now"). After a world switch
the commands answer "this world's data is not loaded yet" until the new world's file is read.

## Claiming

`/grave.claim` takes your newest tombstone that the game has loaded and brings its stacks to you from where you
stand; the game removes the emptied tombstone:

- Stacks in the tombstone's own inventory (a tombstone loaded from a save keeps them there) come with the game's own
  loot event, as when you press "take all" at it (seen in game), gear with its gems and perks.
- A fresh tombstone keeps its stacks in linked inventories, which that event does not reach (seen in game): each plain
  stack is taken out of the tombstone and put into you; what does not fit is dropped at your feet. **Gear stays** in
  such a tombstone (taking it would lose its gems and perks): the answer says so; take it by hand.

The rules:

- **Claim within** (150 m by default; 0 = from anywhere). The game only has the places near players loaded, so a
  tombstone very far away answers "the game has not loaded that place: come closer".
- **Between claims** (10 minutes of play by default), per player; it starts when the claim is sent (a second claim
  in the meantime is refused), and the wait follows the setting as it is now (lowering it shortens a cooldown that
  already runs). When nothing left the tombstone 5 s later, there is no cooldown.
- **Cost** (none by default): an item and a count, checked first and taken all or nothing; given back only when the
  tombstone is unchanged 5 s after the claim (nothing came).
- **A full inventory loses nothing**: with the loot event what does not fit stays in the tombstone, which stays
  yours; with a fresh tombstone it is dropped at your feet. When you left before it landed, it is dropped at the
  tombstone, else beside the nearest player, and the log says where.

## Settings (Mods tab, EMF.exe, or `set grave <key> <value>`)

| Setting | Default | What it does |
|---|---|---|
| Players may claim their tombstone (`claim`) | on | off: `/grave.where` still finds it |
| Claim within (m) (`claim_radius`) | 150 | 0-2000; 0 = from anywhere |
| Between claims (minutes) (`claim_cooldown_minutes`) | 10 | 0-60, minutes of play |
| Cost: item (`claim_cost_item`) | none | an item's debug name (`items rune` in the console) |
| Cost: how many (`claim_cost_count`) | 0 | 0-20 |
| Tombstones kept per player (`keep`) | 3 | 1-5 |
| Show where your tombstone is (`hud`), Corner, Colour | on, top right | the host's own line |

## Good to know

- North on the screen is the same guess as the Wandering Champion's (the world's +z); if the direction is off, change
  the two lines `NORTH` / `EAST` at the top of `main.lua` and save (the mod reloads).
- The log (`emf\emf_core.log`, lines `[grave]`) names every death noted, every tombstone found and every claim.
