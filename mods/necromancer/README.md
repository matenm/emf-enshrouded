# Necromancer (`necromancer`, a Lua mod)

A Necromancer class for Enshrouded. Raise skeleton warriors that stay with you, follow you and fight, raise a lich and a
bone golem with the **undead pack**, and cast raging spirits: flaming skulls that rush your foe for a few seconds.
Their kills are yours: XP and loot.

It needs the `lua` mod 1.9 or newer (in the EMF zip). It is installed **switched off**: switch it on in the Mods tab
(the key under Esc) or in EMF.exe, then restart the game. It works next to **Beastmaster**: with both installed, one
summoning wheel holds your undead and your beasts.

## How it goes

1. **Raise Skeleton** (wheel, or `/necromancer raise`): a skeleton warrior rises beside you for 25 % of your maximum
   mana, 1 s apart. At most 3 at a time. It is your ally at your level, guards you and follows you.
   **Raise Lich** (wheel, or `/necromancer raise lich`; needs the undead pack, below): a robed sorcerer that casts
   bolts, for 40 % of your maximum mana. At most 1. It has 1.5x a skeleton mage's health and 1.3x its damage.
   **Raise Bone Golem** (wheel, or `/necromancer raise golem`; the undead pack): a 3 m brute of fused bones that
   fights with the moves of the game's Vukah elite (hunched, long arms, fists), with twice its health, for 60 % of
   your maximum mana.
   At most 1.
2. **Your undead stay** until they fall. A fallen one is gone (raise a new one); its body goes a second later, so our
   own minions never drop loot. When you reload the world, the undead you had come back beside you, free of mana.
3. **Raging Spirits** (wheel, or `/necromancer spirits`): two flaming skulls for 8 % of your maximum mana. They rush
   the creature you look at (within 30 m), else the nearest foe within 12 m; with no foe they guard you. Foes are
   monsters and wild animals only: never players, allies, farm animals, or your base's dummies and mines. Each lasts
   6 s, at most 10 at a time (the oldest go first). Spirits are never saved.
4. **Attack** sends your undead at the creature you look at (within 40 m), or at what hit you in the last 10 s.
   **Gather** calls them to you. **Dismiss all** returns them to the ground.
5. **Kills:** when a minion's or a spirit's hit leaves a foe at 25 % health or less, you land the killing blow (XP and
   loot are yours). Never on bosses, mini-bosses or players.
6. **They stay close:** a minion more than 40 m from you comes back after 3 s; more than 80 m at once.

**The wheel (hold V):** Raise Skeleton (n/3), Raise Lich (n/1), Raise Bone Golem (n/1), Raging Spirits, Gather,
Attack, Dismiss all. With Beastmaster installed and **One wheel** on in both mods, V and B open the same "Summons"
wheel: "Undead: ..." and "Beasts: ..." items. Turn **One wheel** off in a mod to give it its own wheel again. A line
at the bottom left: "Undead: Skeletons 2/3, Liches 1/1, Spirits 2".

## Commands

All of these work from the game's chat for every player (`/necromancer <word>`) and in the console
(`necromancer.<word>`). Without a word, `/necromancer` lists them.

| Command | What |
|---|---|
| `/necromancer raise` | raise a skeleton warrior beside you |
| `/necromancer raise lich` | raise a lich beside you (the undead pack) |
| `/necromancer raise golem` | raise a bone golem beside you (the undead pack) |
| `/necromancer spirits` | raging spirits at what you look at (else the nearest foe) |
| `/necromancer attack` | your undead attack what you look at (else what hit you) |
| `/necromancer gather` | your undead come to you |
| `/necromancer dismiss` | your undead return to the ground |
| `/necromancer list` | how many undead of each kind you have |

## Settings (Mods tab, EMF.exe, or `set necromancer <key> <value>`)

| Setting | Default | What it does |
|---|---|---|
| Summoning wheel (`wheel_key`) | V | hold it for the wheel |
| One wheel with Beastmaster's beasts (`shared_wheel`) | on | off: this mod's own wheel |
| Mana to raise a skeleton (`raise_mana_pct`) | 25 % | of your maximum mana; 0 = free |
| Skeletons at once (`skeleton_cap`) | 3 | 1-8 |
| Seconds between raises (`raise_cooldown_s`) | 1 | |
| Liches at once (`lich_cap`) | 1 | 0-3; 0 = none |
| Mana to raise a lich (`lich_mana_pct`) | 40 % | |
| Bone golems at once (`golem_cap`) | 1 | 0-2; 0 = none |
| Mana to raise a bone golem (`golem_mana_pct`) | 60 % | |
| Mana for raging spirits (`spirit_mana_pct`) | 8 % | |
| Spirits per cast (`spirits_per_cast`), A spirit lasts (`spirit_life_s`) | 2, 6 s | |
| Spirits at once (`spirit_cap`) | 10 | the oldest go first |
| Minions come back from (`leash_m`), At once from (`leash_far_m`) | 40 m, 80 m | |
| You get your minions' kills (`xp_credit`) | on | |

## The undead pack (`emf_necromancer_undead`)

An EML data patch that adds new undead, made with Meshy and fitted to game skeletons, so they move and fight with
those creatures' animations and AI:
- the Lich, `Necromancer_Lich` (template `c4e1a7d2-6b3f-4d8e-9a10-5f2b7c3d9e02`), on the skeleton mage;
- the Bone Golem, `Necromancer_BoneGolem` (template `e7b3c9d1-2a4f-4e6b-8c15-9d2f4a6b8e02`), on the Vukah elite
  (`Enemy_Vukah_Melee_Elite_Armored`), as tall as it (3 m).

It is in the **EMF creature packs** download on EMF's Nexus page: install it with EMF.exe ("Install a downloaded file")
or like any EML mod (the `mods` folder next to the game, run through EML's `emm.exe`). The lich's template sets
the damage (DamageMod multiplier x1.3); its health (x1.5) comes from Necromancer when it raises a lich, and the golem's
(x2) when it raises a golem.

Without the pack, Raise Lich and Raise Bone Golem answer that the pack is not installed, and nothing else changes.
Undead saved in a world stay saved while the pack is missing (one note when you load) and come back once the pack is
installed.

To rebuild it from its sources (for modders; the sources are not in the repository): `python -I
tools/models/make_mod.py tools/models/packs/necromancer_undead.json`. The pack holds the new model and its texture, plus
instructions that copy and change the game's skeleton mage and Vukah elite at load time; no game files are copied into it.

## Good to know

- It runs where the world runs: the host or a dedicated server. Without the undead pack, players who join need nothing:
  they use `/necromancer` in the chat, and each player has their own undead, kept with the world
  (`emf\config\necromancer\<world slot>.txt`; the game's saves are never changed). **With the undead pack, everyone
  needs it:** the host (or the server) and every player who joins, because it changes the game's data (an EML patch).
  The wheel and the line on screen are the host's.
- The bodies are the game's own: skeleton warriors (`Enemy_Skeleton_Melee01`) made your allies, and the flying skull
  the game's skull summons use (`Player_Companion_FlyingSkull`); the lich and the golem are the undead pack's.
- Your undead are taken out of the world while their owner is away, and come back when they return.
- Old saves (version 1.0) load as they were: their skeletons come back as skeleton warriors.
- Raising and casting work with the wheel or the chat command; there are no spell items for them. The undead you can
  raise are the skeleton warrior, the lich and the bone golem.
