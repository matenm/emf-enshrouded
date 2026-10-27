# Hand-over (`handover`, a Lua mod)

Give items to the player next to you. Type `/handover wood 20` into the game's chat: 20 of the wood you carry go
straight into the inventory of the nearest other player within 5 m. `/handover.who` lists who is near you, and
`/handover.to Alice stone 10` (or `/handover.to #2 stone 10`) picks one. Both players get a chat line when the items
arrived.

It answers "Drop items / give to friends" (1.1K votes on the official feature board, still open), which nothing on
Nexus covers. It runs where the world runs (the host, or a dedicated server), so **players who joined need nothing**:
not EMF, not this mod.

It needs the `lua` mod 1.6 or newer and EMF API v19 (both in the EMF zip). It is installed **switched off**, because
it lets any player within reach put items into another player's bag: switch it on in the Mods tab or EMF.exe (or
`enabled=1` in `emf\config\handover.ini`). A player who does not want hand-overs types `/handover.block`.

## Commands

| Command | Who | What |
|---|---|---|
| `/handover <item> [count\|all]` | **any player** | to the nearest other player within reach |
| `/handover.to <name\|#k> <item> [count\|all]` | **any player** | to that player (within reach). `#k` is the number `/handover.who` gives; a name is the player's name as the game knows it (the whole name, or a start only one player has) |
| `/handover.who` | **any player** | the players within reach, nearest first: `#1 Alice 3 m, #2 Bob 4 m` |
| `/handover.block [on\|off]` | **any player** | refuse hand-overs from others (or accept them again); kept per world and account. A giver is told "Bob does not accept hand-overs" |
| `handover.log` | host / admins | the last 20 hand-overs |
| `handover.debug_target <entity\|off>` | host / admins | for tests without a second player: this villager or chest receives every hand-over until `off` |

- `<item>` is part of the item's name as the game knows it (`items wood` in the console lists them): `logs` finds
  `Material_T1_Wood_Logs`. A name that matches exactly wins; when several items match, the first five are listed so you
  can say which. When two players have the same name, `/handover.to` asks for their `#k`.
- Without a count you hand over your biggest stack of that item (one piece of gear-like items); `all` hands over every
  one you carry; a number hands over that many.
- "Any player" works from the game's chat on a dedicated server, and in a game you host with `[chat] players=1` in
  `emf\emf_core.ini` ([docs/modding/server.md](../../docs/modding/server.md)). The host can type them into the chat or
  the console as well.

**The host's wheel:** hold **H** (the **Your wheel** setting) in a world you host: pick a player near you, then one of
your stacks, then how many (1, 10, half, all).

## Items are never lost

Each stack moves with the Lua host's `emf.move`:

1. It is taken from the giver first, exactly from the stack the mod looked at, all or nothing (if it is no longer
   there, nothing happens). Worn equipment is never taken.
2. It goes into the receiver's inventory through the game's own inventory code (stacks are topped up, the receiver
   sees the game's "+20" note).
3. What did not fit goes back into the giver's inventory.
4. What does not fit back either drops beside the giver as a loose pickup; if the giver has left, beside the receiver,
   else beside the nearest player.

Only if nobody at all can take a drop is it lost, and then the log says `LOST` with what and how many. The move goes
on even if the mod errors or reloads meanwhile, and an answer that arrives after the world changed (a reload) touches
nothing. The giver gets a chat line for each outcome ("Bob had room for 25 of 40 x ...; the rest came back to you").

## Gear

A gear item (a weapon, armour, a ring: anything with its own item data for its level, perks and gem) is **refused**:
the game would make a fresh copy without its gems and perks. Put it in a chest instead. With **Allow gear** on, gear is
handed over one piece at a time at the same level and rarity, with a warning; its gems and perks are lost. Worn
equipment is never offered.

## Settings

| Setting | Default | What |
|---|---|---|
| Reach (m) | 5 | how far the receiver may be |
| Wait between hand-overs (s) | 2 | per player |
| Allow gear | off | see above |
| Tell both players | on | the chat lines when items arrive (the host of a game you host gets a note on screen) |
| Your wheel (host) | H | empty = no wheel |

## Testing it alone

Without a second player: `handover.debug_target <entity>` (an entity id of a villager or a chest, from `near 10` in
the console), then `/handover wood 20` and `inv <entity>`. `handover.debug_target off` ends it.
