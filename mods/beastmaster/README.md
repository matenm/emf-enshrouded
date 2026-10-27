# Beastmaster (`beastmaster`, a Lua mod)

A Beastmaster class for Enshrouded. Tame wild wolves, boars, sabretooths, hounds, bears, aurochs and panthers, keep up
to 8 in your collection, and call up to 2 at a time to fight beside you. They follow you, come back when they stray,
rest after they fall, and are kept with the world. Their kills are yours: XP and loot.

It needs the `lua` mod 1.9 or newer (in the EMF zip). It is installed **switched off**: switch it on in the Mods tab
(the key under Esc) or in EMF.exe, then restart the game. The bear, the aurochs and the panther come in separate
downloads, the **bear pack** (`emf_beastmaster_bear`) and the **beasts pack** (`emf_beastmaster_beasts`), below;
without them everything else works and those beasts are simply not there.

## How it goes

1. **Tame a beast**, in one of two ways:
   - **By defeat:** bring it under **25 %** health and choose **Tame** (wheel) or type `/beastmaster tame` with it
     within 8 m: the beast under your crosshair, else the nearest beaten-down one (in a fight it is often at your side).
     It calms down for those 3 s: it comes to you and stops fighting, and your beasts leave it alone. Stay within 8 m.
     If it dies or you walk away, taming stops and it turns wild again.
   - **By bait:** choose **Feed** 3 times within 10 s (the beast you look at, else the nearest within 8 m). Each feed
     takes one bait from your bag: dog bait for wolves and hounds, lean meat for boars, game meat for sabretooths and panthers, red
     berries for bears and aurochs.
2. A tamed beast becomes your ally, guards you, and joins your collection with a name: "Wolf", "Wolf 2", "Boar"...
3. **Call a beast** from your collection: **Call <name>** on the wheel, or `/beastmaster call <name or number>`. It costs
   15 % of your maximum mana, with 10 s between calls. At most 2 are out, one of each kind.
4. **Fight together:** they defend you on their own. **Attack** (wheel) or `/beastmaster attack` sends every beast that is
   out at the creature you look at, up to 40 m away; when you look at nothing, at the creature that hit you in the last
   10 s. When a beast's hit leaves a foe at 25 % health or less, you land the killing blow, so the XP and loot are
   yours.
5. **They stay close.** A beast more than 40 m from you comes back after 3 s; one more than 80 m away comes back at
   once.
6. **A beast that falls** rests for 2 minutes at home, and then you can call it again. Its body is gone a second after,
   so our own beasts never drop loot.
7. **Dismiss** a beast (`/beastmaster dismiss <name>` or **Dismiss all**) to send it home. Beasts that are out stay out
   when you reload the world, and they come back beside you, free of mana.

**The wheel (hold B):** Tame, Feed, Attack, Dismiss all, then **Call <name>** for up to 6 beasts that are at home or
resting; a resting one shows its time left. With **Necromancer** installed too, B and V open one "Summons" wheel with
"Beasts: ..." and "Undead: ..." items; turn **One wheel** (`shared_wheel`) off to keep Beastmaster's own wheel. A line at the bottom left lists your beasts that are out or resting:
"Beasts: Wolf (out), Bear (rests 1:20)".

**Wild bears, aurochs and panthers (with their packs):** about every 30 minutes of play, when there is no wild one
yet, one of them (at random, from the packs you have) appears 50-70 m from a player who is out in the open, at that
player's level; never within 40 m of a flame altar. Beat it down or feed it its bait to tame it.
- **Bear** (bear pack): 1.4x a wolf's size, 2.5x its health, 1.5x its damage, a little slower.
- **Aurochs** (beasts pack): a huge horned wild bull on the boar's skeleton, it charges like a boar: 1.3x a boar's
  size, 3x its health, 1.4x its damage, a little slower.
- **Panther** (beasts pack): a sleek black big cat on the sabretooth's skeleton: 0.85x a sabretooth's size, quicker
  (1.15x its speed), 1.2x its damage.

## Commands

All of these work from the game's chat for every player (`/beastmaster <word>`), and in the console
(`beastmaster.<word>`). Without a word, `/beastmaster` lists them.

| Command | What |
|---|---|
| `/beastmaster list` | your beasts: "1. Wolf - wolf, out", "2. Bear - bear, rests 1:20", "3. Boar - boar, at home" |
| `/beastmaster tame` | tame the beast you look at, else the nearest beaten-down one (under 25 % health, within 8 m) |
| `/beastmaster feed` | feed the beast you look at (else the nearest) its bait: 3 times to tame it |
| `/beastmaster call <name or number>` | call a beast from your collection |
| `/beastmaster dismiss <name, number or all>` | send a beast home |
| `/beastmaster attack` | your beasts that are out attack the creature you look at (else what hit you last) |
| `beastmaster.give <species>` | **host console only:** add a beast to your collection without taming it (wolf, boar, sabretooth, hound, bear, aurochs, panther) |
| `beastmaster.species <graph guid> <faction> [scale]` | for modders: which beast a creature is, by the animation graph its template runs, its faction number and its scale (render models are not on the server world; the bear is the wolf's graph at 1.4x, the aurochs the boar's at 1.3x, the panther the sabretooth's at 0.85x) |

## Settings (Mods tab, EMF.exe, or `set beastmaster <key> <value>`)

| Setting | Default | What it does |
|---|---|---|
| Beast wheel (`wheel_key`) | B | hold it for the wheel |
| One wheel with Necromancer's undead (`shared_wheel`) | on | off: Beastmaster's own wheel |
| Mana to call a beast (`call_mana_pct`) | 15 % | of your maximum mana; 0 = free |
| Seconds between calls (`call_cooldown_s`) | 10 | |
| Beasts out at once (`max_out`) | 2 | 1-4, never two of the same kind |
| Rest after a beast falls (`rest_s`) | 120 s | |
| Beasts in your collection (`collection_max`) | 8 | 1-16 |
| Tame by defeat below (`tame_health_pct`) | 25 % | |
| Taming takes (`tame_channel_s`) | 3 s | |
| Taming range (`tame_range_m`) | 8 m | |
| Baits to tame by feeding (`feed_count`), Feeding window (`feed_window_s`) | 3, 10 s | |
| Bait for wolves, hounds, boars, sabretooths, bears, aurochs, panthers (`bait_<kind>`) | see above | an item's debug name (`items meat` in the console) |
| Beasts come back from (`leash_m`), At once from (`leash_far_m`) | 40 m, 80 m | |
| You get your beasts' kills (`xp_credit`) | on | XP and loot for the owner |
| Rare wild bears, aurochs and panthers (`wild_bears`), About every (`wild_bear_minutes`) | on, 30 min | needs their pack |

## The bear pack (`emf_beastmaster_bear`)

An EML data patch that adds one new creature, `Beastmaster_Bear` (template `b3a7c2e1-5f4d-4c8b-9e21-7a0d4b6e2f02`). Its
model was made with Meshy and fitted to the game wolf's skeleton, so it moves with the wolf's animations and AI. Install
it like any EML mod (the `mods` folder next to the game, run through EML's `emm.exe`). The template sets the size
(`ScaleComponent` 1.4), the damage (DamageMod multiplier x1.5) and the speeds (x0.85). The health (x2.5) comes from
Beastmaster when it spawns a bear: the game's template has no base health of its own.

To rebuild it from its sources (for modders; the sources are not in the repository): `python -I
tools/models/make_mod.py tools/models/packs/beastmaster_bear.json`. The pack holds the new model and its texture, plus
instructions that copy and change the game's wolf at load time; no game files are copied into it.

## The beasts pack (`emf_beastmaster_beasts`)

An EML data patch with new beasts, made with Meshy and fitted to game skeletons (their animations and AI):
- the Aurochs, `Beastmaster_Aurochs` (template `d5f2b8e3-7c4a-4e9f-8b21-6a3c8d4e0f02`), on the boar: size
  (`ScaleComponent`) 1.3, damage x1.4, speeds x0.9; the health (x3) comes from Beastmaster;
- the Panther, `Beastmaster_Panther` (template `f3c9a1d7-5b2e-4f8a-9c64-2e7b1d9a5f02`), on the sabretooth: size 0.85,
  damage x1.2, speeds x1.15.

Installed and rebuilt like the bear pack (`tools/models/packs/beastmaster_beasts.json`). Without the pack, an aurochs
or a panther in your collection stays at home: calling it answers that the pack is not installed, and one that was out
when you saved goes home with one note when you load.

## Good to know

- It runs where the world runs: the host or a dedicated server. Without the packs, players who join need nothing:
  they see the beasts and use `/beastmaster` in the chat. **With a pack, everyone needs it:** the host (or the
  server) and every player who joins, because it changes the game's data (an EML patch). The wheel and the line on
  screen are the host's.
- Your collection is kept with the world, per player (by Steam account).
- A beast that is out is taken out of the world while its owner is away, and comes back when they return.
- Beasts are ordinary game creatures with the enemy AI: they guard you as the game's companions do, and sometimes take
  their own path to a foe.
- The bear's and the aurochs' health boost needs Beastmaster to spawn them: one made another way (the `spawn` console
  command) has a wolf's or a boar's health.
- Beasts are called with the wheel or the chat command; there are no spell items for it. The species are the seven
  above.
