# Night Raids (`raids`)

At nightfall, by chance, waves of enemies gather outside a base where players are and march on its flame altar. You get
a warning first, a line on screen counts the raiders left, and when every raider is dead the raid is repelled and the
players at the base get a reward. Survivors flee at dawn.

It answers the "optional base raids" request (2.5K votes on the official feature board); no other mod does this.

## Switch it on for a world

Night Raids is installed and on, but **every world starts without raids**. In the world you want raids in:

- type `raids.world on` into the console (the key under Esc), the game's chat (`/raids.world on`) or `emf\commands.txt`,
- or switch on **Raids in the world you are in** in the Mods tab while that world is loaded.

The switch is kept with the world (`emf\config\raids\<world slot>.txt`). `raids.world off` switches it off again.

## How a raid goes

1. Once per night, when the clock passes **Raids come at** (20:00), the chance is rolled. A raid needs a player inside a
   base: within **Base radius** of a flame altar.
2. A warning: "The Shroud stirs: raiders gather outside your base and attack in 60 s!", and a countdown on screen.
3. The waves: the raiders appear 40-60 m from the altar (always 5 m or more beyond the base radius, never inside a base),
   at the level of the strongest player at the base plus the difficulty, and march on the altar. Within 25 m of a living
   player, companion or summon they fight the nearest one, and keep at it while it lives and stays near (with
   **Raiders attack** on "Players only", players alone).
   The next wave comes after **Seconds between waves**, or 5 s after a whole wave is dead. At
   most 24 raiders are alive at once.
4. Every raider dead: "Raid repelled!", and every player near the base (within 20 m of the raiders' ring, about 80 m
   from the altar with the default radius) gets the reward: one rune and one fur per raider slain, or also a rare
   weapon at their level. Only raiders a player or an ally (the companion, a summon) hit count as slain: one that fell
   to its death, or that got stuck and was removed (it fell at its spawn, or found no way in for a minute), does not.
   A raider fighting a player near it (within 8 m, on its level) is not counted as stuck while it fights (1.3.0).
   A reward that does not fit is dropped at the player's feet. The raiders' corpses keep their loot.
5. At dawn (6:00), or when no player has been near the base for a minute, the survivors leave.

Where they appear: near spots players have walked over outside the bases this session (moved along their direction onto
the ring, at their height), else on a ring around the altar at its height + 2 m (tested: they land and walk in). No
raid comes when you sleep through nightfall.

## Settings (Mods tab, EMF.exe, or `set raids <key> <value>`)

| Setting | Default | What it does |
|---|---|---|
| Raids in the world you are in (`this_world`) | off | the world's switch (as `raids.world`) |
| Raid strength (`preset`) | my own settings | a preset (below), or the settings one by one |
| Chance each night (`chance`) | 35 % | 0-100, rolled once per night |
| Raids come at (`start_hour`) | 20:00 | 18:00-23:30 |
| Warning before they attack (`warning_seconds`) | 60 s | 10-300 |
| Quiet nights after a raid (`cooldown_nights`) | 0 | 0-10 |
| Waves (`waves`) | 2 | 1-5 |
| Raiders per wave (`per_wave`) | 4 | 1-12 |
| Seconds between waves (`wave_gap_seconds`) | 60 | 20-300 |
| Difficulty (`difficulty`) | normal | easy (5 levels below the strongest player at the base), normal, hard (+3), deadly (+6) |
| Boss wave (`boss`) | off | the last wave brings a Vukah brawler, a mini-boss with a boss bar (from level 5 with **Only creatures you are ready for**) |
| Raiders attack (`targets`) | players, companions and summons | or players only. Allies are the fighters on the players' side: the companion, Necromancer's minions, Beastmaster's beasts, the game's own summons (never farm animals or villagers) |
| Only creatures you are ready for (`progression`) | on | no boss or creature above the players' level (below) |
| Who raids (`templates`) | Vukah warriors | or your own templates (below) |
| My own templates (`custom_templates`) | | template GUIDs separated by commas; `GUID@12` comes from player level 12 |
| Which base (`bases`) | the busiest | or any base with players, at random |
| Base radius (`base_radius`) | 30 m | 10-100. The game's own build zone is not readable (its size reads 0), so this is a fixed radius: a flame altar of a higher level builds a bigger zone, so raise it for an upgraded altar, or raiders may appear inside the zone's edge |
| Reward when repelled (`reward`) | runes and furs | none / runes and furs / also a rare weapon |
| Raid line on screen (`corner`, `color`) | top right, orange | |
| Raiders break walls (`break_walls`) | off | a raider held up by a wall or a closed gate on its way to the flame altar breaks through it: cracks first, then a hole that stays (saved with the world, like mining; the game resets holes outside a flame altar's area). Straw and wood go quickly, stone takes a group about half a minute, metal mostly holds. Never the ground, and never the world's own ruins: a wall spot on a ruin is left alone, and a hole next to a ruin is smaller. A spot not broken after 2 minutes is given up. Needs EMF 0.6.14 |
| Hole size (`hole_size`) | small | small (about a door, 2 m), medium (3.5 m), large (5 m) |
| Wall strength (x hits) (`wall_strength`) | 1 | 0.5-10: how many hits walls and gates take, times the normal. At 1 a wood wall cracks at the first hit and breaks at the second or third, planks take 5, stone about 20, a gate about 6. Several raiders at one wall, and harder raids, hit harder |
| Broken blocks drop items (`wall_drops`) | off | what a pickaxe would give for the broken blocks, and a broken gate's own item, at the hole |
| Raiders also break walls of terrain blocks (`break_terrain`) | off | walls of Stone, Granite, Field Stones, dirt, sand or road blocks are terrain to the game; with this on, raiders break those too, as long as a player placed them (where the world as the game shipped it had no ground). The world's own ground, hills and ruins are never touched. Needs EMF 0.6.19 |

Every setting applies at once.

### Raid strength: the presets

A preset sets how often and how hard raids come in one choice. While one is chosen, it decides the settings it greys
out; those keep showing your own values (used again when you go back to **My own settings**), so each preset's name in
the list spells out its numbers. The other settings work as always.

| | Gentle | Standard | Hard |
|---|---|---|---|
| For | the early game | most of the game (the defaults) | a real fight |
| Chance each night | 25 % | 35 % | 50 % |
| Warning | 120 s | 60 s | 45 s |
| Quiet nights after a raid | 1 | 0 | 0 |
| Waves x raiders | 1 x 3 | 2 x 4 | 3 x 6 |
| Seconds between waves | 90 | 60 | 45 |
| Difficulty | easy (5 levels below) | normal | hard (+3) |
| Boss wave | no | no | yes |
| Reward | runes and furs | runes and furs | also a rare weapon |

## Commands

| Command | Who | What |
|---|---|---|
| `raids.status` | **any player** | whether raids are on in this world, the raid strength, the chance, and the raid going on |
| `raids.world on\|off` | host / admins | the world's switch |
| `raids.start [seconds]` | host / admins | a raid now at the base with the most players, after a 10 s warning (for tests; by day too) |
| `raids.stop` | host / admins | ends the raid; its living raiders are removed |
| `raids.kill` | host / admins | every living raider dies (for tests: they count as slain, the raid ends as repelled) |
| `raids.look <raider>` | host / admins | the raider looks for a wall or a closed gate toward the altar now, as one held up for 10 s does (for tests of Raiders break walls) |

"Any player" works from the game's chat on a dedicated server, and in a game you host with `[chat] players=1` in
`emf\emf_core.ini` (EMF API v17 commands for players; see `docs/modding/server.md`).

## Who raids

Only templates seen to wake and fight when spawned away from their camp are used: the **Vukah warrior**
(`Enemy_Vukah_Melee`) and, for the boss wave, the **Vukah brawler** (`Enemy_Midboss_Vukah_Bigfoot_BossHealthBar`).
Some templates spawn asleep (the heavy wolf mini-boss did). To try others, choose **My own templates** and list their
GUIDs (`export/template_guids.csv` from the guid_dump mod has every template); untested ones may stand still.

### Only creatures you are ready for

On by default: raids bring nothing the players at the base are not ready for. The game keeps which zones you have seen
and which bosses you have beaten where EMF cannot read them yet, so the rule goes by levels:

- The boss wave's **Vukah brawler**, a Springlands mini-boss, waits until the strongest player at the base is level 5.
  Below that the last wave comes without it (the log says so). The difficulty does not change this: the players' own
  level counts.
- Your own templates: write `GUID@level` and that creature waits for a player of that level (a dragon as `GUID@25`).
  A bare GUID comes at any level, but if it turns out to be a boss or mini-boss (it has a boss bar), it is removed at
  once (EMF cannot tell whether you have beaten it): a Vukah warrior takes its place, a note says so once, and Vukah
  warriors come instead of that template until the game restarts. When none of your templates is ready yet, Vukah
  warriors come. Spaces around the `@` are fine.

Switch the rule off to get the old behaviour.

## Good to know

- **Host or dedicated server only**: the raid runs where the world runs. Players who joined see the raiders without
  EMF of their own; the warning, the notes and the line on screen show only on the host's screen (a dedicated server
  has no screen: players use `/raids.status`). There is no chat announcement.
- **Not saved**: the game does not save spawned raiders. A reload or a world switch simply ends the raid.
- Raiders never appear inside a base, but they do walk in: that is the attack.
- They break walls only with **Raiders break walls** on, and never the world's own ruins (EMF 0.6.14 tells them from
  what players built by comparing each block with the world as the game shipped it).
- Walls of terrain blocks (Stone, Granite, dirt ...) only with **Raiders also break walls of terrain blocks** on (1.3.0,
  EMF 0.6.19), and only the cells a player placed: the natural ground is never dug.
- At a wall, a raider walks up close and swings at it (1.3.0): each swing breaks a piece. For this the
  mod puts an invisible door at the wall for it to swing at, and takes it away once the wall is open or given up (it is
  never saved). A creature that does not swing for 10 seconds breaks the wall anyway, by the clock as before. A raider
  hit at its wall turns on whoever hit it.
- The log (`emf\emf_core.log`, lines `[raids]`) has every raider's id, position and level.
