# Waypoint Teleports (`teleports`)

`/unstuck` frees a player who is stuck (246 votes on the official board ask for it: players get stuck in the shroud, in
holes, under ledges), and `/home` takes a player to their spawn point. Admins get `tp`, `bring`, `tp.pos`, `tp.back` and
`tp.list`. Players are moved the way the game's own fast travel moves them (EMF's `teleport`, API v20): the game loads
the place around them, resets fall damage, and every player's game shows the jump at once, with no rubber-banding.

It runs where the world runs, so on the host or a dedicated server it works for every player, also those who joined
without EMF.

## Switch it on

Waypoint Teleports is installed **switched off**: it lets players move around the map. Switch it on in the Mods tab
(the key under Esc) or in EMF.exe, then restart the game (or the server). Players' chat commands are always on on a
dedicated server; in a game you host, guests need `[chat] players=1` in `emf\emf_core.ini` ([server.md](../../docs/modding/server.md)).

## For every player (type it in the game's chat)

- **`/unstuck`**: stand still for the countdown (3 s; moving 2 m cancels it). Then you hop a few metres up and the game's
  ground search looks for free ground about 10 m around that point. If it finds none, or it puts you back where you
  stood, the way out is your spawn point, and that counts as a **`/home`**: only when /home is on, not inside the shroud
  (unless `home_in_shroud`), not on /home's cooldown or combat window, and at /home's cost. Then /home's cooldown starts
  too. Otherwise "/unstuck could not help here" says why.
  - Not within **10 s of a hit** on you (no escape from a fight; checked again when the countdown ends), not while dead,
    once a minute.
  - The hop works inside the shroud.
- **`/home`**: stand still for the countdown, then you land on your spawn point's pads, as when you respawn there.
  - Your spawn point is the flame altar (or spire, or start location) the game would respawn you at; when the game
    has none for you, the nearest flame altar.
  - Every 10 minutes, not from inside the shroud (as with the game's fast travel), not while dead, not within 10 s of a
    hit (`home_combat_seconds`; checked again when the countdown ends).
  - An optional cost: an item and a count (`home_cost_item`, `home_cost_count`), taken when the teleport happens and
    given back if it fails: exactly what was taken, into your bag, at your feet if it is full, or (if you left) at the
    nearest other player. Nothing is given back when the world was reloaded or left meanwhile (the save may not hold
    the take: a refund could double it).

The countdown, cancels, refusals and results reach each player as chat lines only they see (EMF's `chat_tell`; the host
of a game you host sees them on screen). Where players' chat is off (a game you host without `[chat] players=1`), a guest
sees only the command's answer.

## For admins

The console, `emf\commands.txt`, the host's own chat, and the accounts in `[chat] admins` (with `[chat] everyone=1`
**every player** can use them). `#k` is the k-th player, lowest entity id first: `tp.list` shows them. A player's name
works too (as the chat shows it); when two players share it, use `#k`. No countdown.

| Command | What it does |
|---|---|
| `tp <#k\|name>` | you to player k (beside them) |
| `tp <#k\|name> <#m\|name>` | player k to player m (works from a dedicated server's console) |
| `bring <#k\|name>` | player k to you |
| `tp.pos [#k] <x> <y> <z> [ground]` | you (or player k) to a position; `ground`: the game's ground search there |
| `tp.back [#k]` | you (or player k) back to where you stood before your last teleport by this mod (this session) |
| `tp.list` | the players, their `#k` and where they are |

## Settings (Mods tab, EMF.exe, or `set teleports <key> <value>`)

| Setting | Default | What it does |
|---|---|---|
| /unstuck (`unstuck`) | on | |
| Search for ground from (m above) (`unstuck_height`) | 4 | 2-8: where the ground search starts, above the player (see below) |
| Cooldown (s) (`unstuck_cooldown_seconds`) | 60 | 10-600 |
| Not within (s) of a hit (`combat_seconds`) | 10 | 0-30 |
| /home (`home`) | on | |
| Not within (s) of a hit (`home_combat_seconds`) | 10 | 0-30 |
| Cooldown (min) (`home_cooldown_minutes`) | 10 | 0-60, shared with /unstuck's way home |
| Cost: item (`home_cost_item`) | (free) | an item name as the console's `items` command lists it |
| Cost: how many (`home_cost_count`) | 1 | 1-50 |
| Also inside the shroud (`home_in_shroud`) | off | |
| Countdown (s) (`countdown_seconds`) | 3 | 0-10, for /unstuck and /home |
| Commands (`enabled_commands`) | all | the commands that work here, separated by spaces |

Cooldowns, the combat window and `tp.back` positions last for the session (a reload forgets them).

**Why `unstuck_height` and not a hop distance.** The plan had `unstuck_distance` (3-30 m, default 10: how far a hop
may go). The live tests showed the game's ground search only finds ground within about 10 m of the point it is given,
up or down, and otherwise leaves you exactly there. A 30 m hop would mostly end in the air or inside a hill. So the hop
goes straight up from where you stand (2-8 m, default 4: enough to get over a lip or out of a hole, close enough for
the search), and anything farther is the spawn point, with /home's rules.

## How it is done (for the curious)

EMF's `teleport` hands the game a `Teleport` component for the player, the record the game's own fast travel adds, and
the game's teleport program moves the player on the next step (docs/engine_parts/re_teleport.md). The ground search is
the game's: it finds walkable ground only within about 10 m of the point it is given, and otherwise leaves the player
exactly there, which EMF reports; that is when `/unstuck` falls back to the spawn point. The player's spawn point is the
one the game keeps for its own "emergency teleport" (the player's `SelectedTargets` slot 9). "Inside the shroud" is the
game's own fast-travel test: the player's `EnvironmentSensor` reads dangerous or deadly fog, and the player is not in a
spawn point's safe zone. The rules are in `tp_logic.h` (unit-tested in `tests/test_teleports.cpp`), the game side in
`tp_glue.h` (`tests/test_teleports_glue.cpp`).

## Good to know

- In someone else's game it does nothing (that game's host decides).
- A player stuck *inside* the terrain is freed by the game itself about 1.5 s later (to the spawn point); `/unstuck` is
  for the places the game does not notice: holes, under ledges, behind walls, the shroud.
- On open ground `/unstuck` is a `/home` (with its cost and cooldown): the ground search finds the spot you stand on,
  which counts as "did not help".
- No commands move NPCs: the game removes an enemy moved away from every player within a second.
