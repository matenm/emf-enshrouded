# Bounty Board (`bounty`, Lua)

Every day each player gets three bounties: slay creatures of a faction ("Slay 6 Vukah"), gather a material ("Gather 60
Wood logs"), or slay a champion. Finish one and claim its reward. Daily and repeatable quests (164 votes) and a kill
tracker (65) are open requests on the official board; no Nexus mod has them.

It runs where the world runs, so on the host or a dedicated server it works for every player, also those who joined
without EMF. Each player's bounties and progress are kept with the world, by account
(`emf\config\bounty\<world slot>.txt`).

## Switch it on

Bounty Board is installed **switched off**: it hands out items to every player. Switch it on in the Mods tab (the key
under Esc) or in EMF.exe, then restart the game (or the server).

## Commands

| Command | Who | What |
|---|---|---|
| `/bounty.board` | every player | your bounties, progress, rewards, the time to new ones and your rerolls |
| `/bounty.claim` | every player | the rewards of your finished bounties |
| `/bounty.reroll <n>` | every player | swap bounty n for another one (1 per period by default) |
| `/bounty.top` | every player | who finished the most bounties in this world |
| `bounty.give <id>` | host, admins | put a bounty from the pool on your board (for tests) |
| `bounty.reset` | host, admins | new bounties for everyone now |

The commands for every player work from the chat on a dedicated server, and in a game you host with
`[chat] players=1` in `emf\emf_core.ini` (see `docs/modding/server.md`). The host also sees their bounties on screen
("Bounty: Vukah 2/6 | Wood logs 20/60 | Champion 0/1"). (Lua mods put their id in front of every command, so the
board is `/bounty.board` rather than a bare `/bounty`.)

## What counts

- **Slay:** a kill of a creature of the bounty's faction (read from the creature when first hit). With **Shared
  kills** (on) it counts for every player who hit it in its last 10 seconds; off, only for the last player who hit it.
  A kill by a companion or a summon counts only if the game names the player as the hit's source; a creature killed
  without a hit (a test command) counts for nobody.
- **Gather:** raw materials a player picks up in the world (from a tree, a plant, a rock, a loose drop, a creature's
  corpse) whose name holds the bounty's word. Never items taken out of a chest, a crafting station or a death bag, nor
  out of a tombstone, nor a bounty's own reward, nor items coming back from a chest a mod was filling (quick stack,
  sorting chests). Loose drops a mod made do not count either: a reward this mod dropped at a full player's feet
  (picked up within 30 minutes of play), and a loose drop of an item that a chest within 30 m got from a mod in the
  last 2 minutes (what did not fit there: quick stack and sorting chests drop it beside the chest or the player). A
  drop picked up later than that, or farther from such a chest, counts. Another mod that puts items straight into a
  player counts like gathering: a reward, a bonus, and a Grave Keeper claim of a fresh tombstone (its stacks are put
  into you). EMF cannot tell such a put from a pickup in the world (that would need the core to mark its own puts).
- **Champion:** a kill (as above) of a strong creature: at least 2 levels above the player and at least 1,000 health
  when first hit (the Wandering Champion and the bosses with a boss bar are). A heuristic: EMF cannot name a creature's
  template from Lua.

## Rewards

Rewards wait for `/bounty.claim` (or come at once with **Rewards come at once**). They go straight into your
inventory; what does not fit is dropped at your feet, never lost. A reward that could not even be dropped (you left
the game before it landed) is kept for you: `/bounty.board` says "1 reward waiting" and your next `/bounty.claim`
brings it. Gear comes at your level with **Gear rewards'
rarity** (uncommon by default). **Reward amounts** scales the counts.

## New bounties

**New bounties** comes every real day at **At (hour)** on the host's clock (6:00 by default), at every dawn in the
game, or every few hours of play. A new period draws new bounties for each player (the same player gets the same
draw for the same period, so asking again never changes them); progress and unclaimed rewards of the old period go.

## The pool

The bounties are the `POOL` table at the top of `emf\mods\bounty\main.lua`: kind, faction or item word, how many,
reward items, how often it is drawn. Change it while the game runs and save: the mod reloads. **How many to slay or
gather** scales the counts (easy x0.6, hard x1.5).

Installing EMF again replaces `main.lua`, and your changes to `POOL` with it. To keep your own bounties, put them in
`emf\config\bounty\pool.lua` instead: that folder stays when EMF is installed again or removed. The file returns a
table like `POOL`:

```lua
return {
  {id = "gather_flint", kind = "gather", match = "flint", name = "Flint", n = 30,
   reward = {{item = "Material_T1_Token_WeaponUpgrade_Rune01", count = 5}}},
  {id = "kill_vukah", kind = "kill", faction = 3, name = "Vukah", n = 10, weight = 5,
   reward = {{item = "Material_T1_Token_WeaponUpgrade_Rune01", count = 10}}},
}
```

When it is there, it is the whole pool: `main.lua`'s `POOL` is not used. It is read when the mod starts and at each
world load. An entry that does not fit (no id, an unknown kind, no `n`, a kill without `faction`, a gather without
`match`) is skipped and named in the log; a file that does not run, or holds no entry that fits, leaves `main.lua`'s
pool in use, and the log says so. The file can only build the table: it cannot reach the game, files or other
programs.

## Settings (Mods tab, EMF.exe, or `set bounty <key> <value>`)

| Setting | Default | What it does |
|---|---|---|
| Bounties per player (`bounties_per_period`) | 3 | 1-5 |
| How many to slay or gather (`difficulty`) | normal | easy, normal, hard |
| Shared kills (`share_kills`) | on | |
| New bounties (`reset`) | every day | `real_day`, `game_dawn`, `hours` |
| At (hour) (`reset_hour`) | 6 | 0-23, the host's clock |
| Every (hours of play) (`reset_hours`) | 4 | 1-24 |
| Rerolls per period (`rerolls`) | 1 | 0-3 |
| Rewards come at once (`auto_claim`) | off | |
| Reward amounts (x) (`reward_scale`) | 1 | 0.5-3 |
| Gear rewards' rarity (`reward_rarity`) | uncommon | |
| Show your bounties (`hud`), Corner, Colour | on, bottom left | the host's own line |

## Good to know

- Players who joined get no note when a bounty is done (EMF has no way to send one player a chat line outside a
  command): they see it with `/bounty.board`. The host gets a note on screen.
- The log (`emf\emf_core.log`, lines `[bounty]`) names every bounty finished and every reward.
