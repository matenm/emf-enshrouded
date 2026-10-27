# Wandering Champion (`champion`, a Lua mod)

Every so often, while you are outdoors at night, a champion appears 80-150 m away and roams: a mini-boss with a boss
bar, at your level and a little above. A line on screen tells you how far it is and in which direction. Slay it for a
bonus; it leaves after a while, at dawn, or when everyone is far away.

It answers the "roaming boss" request (1.9K votes on the official feature board), and it is EMF's showcase of a real
gameplay mod in Lua: about 400 lines in `main.lua`, which you can change while the game runs (save the file: the mod
reloads within a second, and a champion that is out stays out).

It needs the `lua` mod 1.4 or newer (in the EMF zip). It is installed **switched off**: switch it on in the Mods tab
(the key under Esc) or in EMF.exe, then restart the game.

## How it goes

1. Time spent outdoors counts (farther than 40 m from every flame altar; at night only, with **Only at night**).
   Every **A chance every** minutes of it, the **Chance** is rolled.
2. The champion appears 80-150 m from a player who is outdoors: on a spot some player walked over outdoors this session
   (so it stands on ground), else in a random direction at the player's height + 3 m. Its level is that player's level
   plus **Levels above the player**. A note: "A champion roams: Vukah Brawler (level 7), 120 m NE of you".
3. It patrols up to four walked spots near where it appeared, a minute at each (it guards a small barrel the mod puts
   there). A player within 30 m is hunted: the champion goes for the nearest one.
4. Slain: the player whose hit killed it (or the nearest player within 60 m) gets the bonus: 10 weapon upgrade runes
   and the **Bonus item** at their level and **Its rarity**, on top of the champion's own loot. Each player's tally is
   kept with the world (`champion.stats`).
5. It leaves after **It leaves after** minutes, at dawn (with **Only at night**, once it has seen the night: one
   summoned by day for a test stays until its time is up), or when every player is more than 300 m away.

## Commands

| Command | Who | What |
|---|---|---|
| `champion.where` | **any player** | how far the champion is, and in which direction, from you |
| `champion.stats` | **any player** | champions slain in this world, by you and in all |
| `champion.summon [metres]` | host / admins | a champion now, near you (for tests: by day too; `champion.summon 30` puts it about 30 m away) |
| `champion.dismiss` | host / admins | it leaves now |
| `champion.kill` | host / admins | it dies now (for tests: no hit, the nearest player gets the bonus) |

"Any player" works from the game's chat on a dedicated server, and in a game you host with `[chat] players=1` in
`emf\emf_core.ini`. Lua: `emf.command(name, help, fn, {players = true})`.

## Settings (Mods tab, EMF.exe, or `set champion <key> <value>`)

| Setting | Default | What it does |
|---|---|---|
| A chance every (`interval_minutes`) | 20 | minutes outdoors between chances, 5-120 |
| Chance (`chance`) | 50 % | |
| Only at night (`night_only`) | on | 18:00-6:00; it fades at dawn |
| It leaves after (`lifetime_minutes`) | 10 | 2-30 |
| Levels above the player (`level_bonus`) | 2 | -5..10 |
| Bonus item (`reward_item`) | `Weapon_T1_1H_SwordEpic_01` | a debug name (`items sword` in the console); empty = none |
| How many (`reward_count`) | 1 | |
| Its rarity (`reward_rarity`) | rare | the game's choice, common .. legendary |
| Show where it is (`hud`), Corner, Colour | on, top right, gold | |

## Change it in `main.lua`

The top of the file is meant for you:

- `CHAMPIONS`: the templates, with the player levels each comes for. Only templates seen to wake when spawned away from
  their camp belong here; the **Vukah brawler** (`Enemy_Midboss_Vukah_Bigfoot_BossHealthBar`) did. Some spawn asleep
  (the heavy wolf mini-boss did): try new ones with `champion.summon`.
- `REWARDS`: the items the slayer gets.
- `NORTH` / `EAST`: which way is north in the world. This is a guess until checked in game: if the line on screen says
  N where the map says E, swap them and save.

## What it shows about Lua mods

Engine events (`HitEvent`, `EntityDiedEvent`), sending the game's own events (`StartGuardingEvent`,
`AddThreatTargetEvent`), spawning and component writes (`Level`), the time of day (`emf.time_of_day`, Lua 1.4), items
with a level and rarity (`emf.put` with `{level, rarity}`, Lua 1.4), killing without a hit (`emf.kill`, Lua 1.4),
per-world data that also lets a reload of the file take the champion over, commands for every player, and settings of
the newer kinds (sliders, selects, a colour, groups).

## Good to know

- It runs where the world runs (the host or a dedicated server); players who joined see the champion without EMF. The
  notes and the line on screen are the host's (a dedicated server has none: players use `/champion.where`).
- The champion is not saved: a reload of the world ends it (the tally stays).
