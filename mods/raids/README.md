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
   at the level of the strongest player at the base plus the difficulty, and march on the altar. Near a living player
   they fight that player. The next wave comes after **Seconds between waves**, or 5 s after a whole wave is dead. At
   most 24 raiders are alive at once.
4. Every raider dead: "Raid repelled!", and every player near the base (within 20 m of the raiders' ring, about 80 m
   from the altar with the default radius) gets the reward: one rune and one fur per raider slain, or also a rare
   weapon at their level. Only raiders a player or an ally (the companion, a summon) hit count as slain: one that fell
   to its death, or that got stuck and was removed (it fell at its spawn, or found no way in for a minute), does not.
   A reward that does not fit is dropped at the player's feet. The raiders' corpses keep their loot.
5. At dawn (6:00), or when no player has been near the base for a minute, the survivors leave.

Where they appear: near spots players have walked over outside the bases this session (moved along their direction onto
the ring, at their height), else on a ring around the altar at its height + 2 m (tested: they land and walk in). No
raid comes when you sleep through nightfall.

## Settings (Mods tab, EMF.exe, or `set raids <key> <value>`)

| Setting | Default | What it does |
|---|---|---|
| Raids in the world you are in (`this_world`) | off | the world's switch (as `raids.world`) |
| Chance each night (`chance`) | 35 % | 0-100, rolled once per night |
| Raids come at (`start_hour`) | 20:00 | 18:00-23:30 |
| Warning before they attack (`warning_seconds`) | 60 s | 10-300 |
| Quiet nights after a raid (`cooldown_nights`) | 0 | 0-10 |
| Waves (`waves`) | 2 | 1-5 |
| Raiders per wave (`per_wave`) | 4 | 1-12 |
| Seconds between waves (`wave_gap_seconds`) | 60 | 20-300 |
| Difficulty (`difficulty`) | normal | easy (5 levels below the strongest player at the base), normal, hard (+3), deadly (+6) |
| Boss wave (`boss`) | off | the last wave brings a Vukah brawler, a mini-boss with a boss bar |
| Who raids (`templates`) | Vukah warriors | or your own templates (below) |
| My own templates (`custom_templates`) | | template GUIDs separated by commas |
| Which base (`bases`) | the busiest | or any base with players, at random |
| Base radius (`base_radius`) | 30 m | 10-100. The game's own build zone is not readable (its size reads 0), so this is a fixed radius: a flame altar of a higher level builds a bigger zone, so raise it for an upgraded altar, or raiders may appear inside the zone's edge |
| Reward when repelled (`reward`) | runes and furs | none / runes and furs / also a rare weapon |
| Raid line on screen (`corner`, `color`) | top right, orange | |

Every setting applies at once.

## Commands

| Command | Who | What |
|---|---|---|
| `raids.status` | **any player** | whether raids are on in this world, the chance, and the raid going on |
| `raids.world on\|off` | host / admins | the world's switch |
| `raids.start [seconds]` | host / admins | a raid now at the base with the most players, after a 10 s warning (for tests; by day too) |
| `raids.stop` | host / admins | ends the raid; its living raiders are removed |
| `raids.kill` | host / admins | every living raider dies (for tests: they count as slain, the raid ends as repelled) |

"Any player" works from the game's chat on a dedicated server, and in a game you host with `[chat] players=1` in
`emf\emf_core.ini` (EMF API v17 commands for players; see `docs/modding/server.md`).

## Who raids

Only templates seen to wake and fight when spawned away from their camp are used: the **Vukah warrior**
(`Enemy_Vukah_Melee`) and, for the boss wave, the **Vukah brawler** (`Enemy_Midboss_Vukah_Bigfoot_BossHealthBar`).
Some templates spawn asleep (the heavy wolf mini-boss did). To try others, choose **My own templates** and list their
GUIDs (`export/template_guids.csv` from the guid_dump mod has every template); untested ones may stand still.

## Good to know

- **Host or dedicated server only**: the raid runs where the world runs. Players who joined see the raiders without
  EMF of their own; the warning, the notes and the line on screen show only on the host's screen (a dedicated server
  has no screen: players use `/raids.status`). There is no chat announcement.
- **Not saved**: the game does not save spawned raiders. A reload or a world switch simply ends the raid.
- Raiders never appear inside a base, but they do walk in: that is the attack.
- They do not break walls.
- The log (`emf\emf_core.log`, lines `[raids]`) has every raider's id, position and level.
