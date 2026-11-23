# Beastmaster (`beastmaster`, a Lua mod)

A Beastmaster class for Enshrouded. Tame wild wolves, boars, sabretooths, hounds, rats, bears, aurochs and snakes, keep up to 8 in
your collection, and call as many to fight beside you as you have active slots (1 at the start, up to 4 as your character
levels up). Build a stable to choose which beasts are active. They follow you, come back when they stray, step aside
while you glide, rest after they fall and then come back by themselves, and are kept with the world. Their kills are yours: XP and loot. Loyal beasts earn talent points: make each
one your own (stronger, tougher, faster, a second wind, an ambush, a guard, a menace, a taunt). Monsters split between
you and your beasts: a tank beast draws half of them.

It needs the `lua` mod 1.25 or newer (in the EMF zip). It is installed **switched off**: switch it on in the Mods tab
(the key under Esc) or in EMF.exe, then restart the game. The bear, the aurochs and the snake come in the **EMF creature packs**
download (the **bear pack** and the **beasts pack**, below): install them with EMF.exe ("Install a downloaded file")
and EMF builds them when the game starts (no EML needed; with EML or ShroudForge installed, the card on EMF.exe's EMF
page says who applies them). Without them everything else works and those beasts are simply not there.

## How it goes

1. **Tame a beast**, in one of two ways:
   - **By defeat:** bring it under **25 %** health and choose **Tame** (wheel) or type `/beastmaster tame` with it
     within 8 m: the beast under your crosshair, else the nearest beaten-down one (in a fight it is often at your side).
     It calms down for those 3 s: it stops fighting and is held where it is (one that still runs off is put back),
     and your beasts leave it alone. Stay within 8 m. If it dies or you walk away, taming stops and it turns wild
     again. A beast someone else is taming or feeding is theirs to finish.
   - **By bait:** choose **Feed** 3 times within 10 s (the beast you look at, else the nearest within 8 m). Each feed
     takes one bait from your bag, one that suits the beast and when you meet it: raw lean meat (goats, hophares and
     rats drop it) for wolves, boars and snakes; the game's animal baits from the Farmer for the rest: dog bait for
     hounds, cat bait for sabretooths, goat bait for aurochs; red berries for bears and rats; for the rare snakes game meat
     (crimson), turkey bait (golden king) and yak bait (albino).
2. A tamed beast becomes your ally, guards you, and joins your collection with a name: "Wolf", "Wolf 2", "Boar"... For its first
   5 s it cannot fall (a swing it began as a wild beast can land on itself).
3. **Call a beast** from your collection: **Call <name>** on the wheel, or `/beastmaster call <name or number>`. It costs
   15 % of your maximum mana, with 10 s between calls. As many are out at once as you have **active slots** (see The
   stable), of any kinds: with two slots, two wolves.
4. **Fight together:** they defend you on their own. **Attack** (wheel) or `/beastmaster attack` sends every beast that is
   out at once: at the creature you look at (an animal too), up to 40 m away; looking at no creature, at the nearest
   monster in the direction you look (within 30 degrees; never one the game keeps peaceful toward you); else at the
   creature that hit you in the last 10 s. They go
   for it for 20 s or until it falls. When a beast's hit leaves a foe at 25 % health or less, you land the killing blow, so the XP and loot are
   yours.
5. **They stay close.** A beast that chases something more than 45 m from you drops the chase and runs back; one more
   than 60 m away for 3 s, or 120 m away at once, comes back beside you. One the game took out of the world behind you
   (a long glide, a fast travel) comes back beside you by itself (1.20.0).
6. **Modes**, for all your beasts at once: **Mode** on the wheel steps through them, or `/beastmaster mode <mode>`;
   the line at the bottom left shows it ("Beasts (passive): ...").
   - **Defending** (the first): they fight what attacks you or them, and what you send them at with Attack.
   - **Passive:** they ignore fights and keep following you; Attack still sends them at a creature (for 20 s, or until
     it falls, turns friendly or they drop the chase: then they are calm again at once).
   - **Aggressive:** they also go after hostile creatures within 25 m of you (never wildlife, farm animals, bosses or
     players).
   - In every mode they leave alone creatures the game keeps peaceful toward you, such as the Vukah after the game's
     Vukah Language skill or wild beasts near you after its Elf skill (1.20.0). A creature you hit loses its peace and
     is a foe again; to send your beasts at a peaceful one, look right at it and use Attack.
7. **A beast that falls** rests for 60 seconds, then comes back beside you by itself, free (the line at the bottom left
   and `/beastmaster list` show "rests 0:40, then comes back"). It keeps its active slot meanwhile: **Dismiss** it (the
   wheel shows "Dismiss Wolf (rests 0:40)") to leave it at home and call another. With **A fallen beast comes back by
   itself after its rest** (`back_after_rest`) off it waits at home after its rest until you call it. Its body is gone
   a second after, so our own beasts never drop loot.
8. **Dismiss** a beast to send it home: **Dismiss <name>** on the wheel (one for each beast that is out), **Dismiss
   all** (with two or more out), or `/beastmaster dismiss <name or number>`; `/beastmaster dismiss` alone sends every
   beast home. A dismissed beast waits at home: call it again whenever you like. Beasts that are out stay out when you
   reload the world, and they come back beside you, free of mana.
9. **Building:** the game most likely does not let you build or place things next to a creature that runs the enemy
   AI (that is what its code suggests; not yet seen in game), and your beasts are such creatures. So while you, or
   another player within 20 m of them, hold something to build or place, your beasts that are out **step aside**:
   they leave for a moment and come back beside you, free, a few seconds after the building stops (the first time a
   note says so). **Beasts step aside while someone builds next to them** (`make_room`) turns this off.
   `/beastmaster aside` sends them away by hand, for example when they stand where you want to work; **Come back** on
   the wheel, `/beastmaster aside` again, calling or taming a beast, or sending the last one home ends it.
   `/beastmaster list` shows them as "aside".
10. **In the air:** beasts cannot glide, so while you are on the glider (or falling for more than 1.5 s, not a jump)
    your beasts step aside too, and come back beside you, free, a second after you land (the first time a note says
    so). A fall counts once you dropped 6 m, a glide while your height changes. Before 1.18.0 a beast far from you was brought to your side while you glided, in the air, and fell to its
    death.

## The stable

**Active slots.** You start with one: one beast out at a time. Slot 2 opens at character level 10, slot 3 at 20, slot 4
at 30 (settings; 0 turns a slot off). `/beastmaster slots` tells you yours and when the next one opens.

**Before your world has a stable**, you tame and call as before: any beast of your collection, as many out as your slots
(dismiss one on the wheel and call another to swap). A beast you tame while your slots are full goes home.

**The stable** is a prop Beastmaster knows: the **Stable** of the beasts pack (crafted at the Workbench from 6 wood
logs, 8 plant fibre and 2 animal fur, listed after the crude chair; a small open stable with a thatched roof, 4.5 m
wide), its **Large Stable** (12 wood logs, 16 plant fibre and 4 animal fur; the same stable 1.6 times as big, 7.2 m
wide, where your beasts at home live: see below),
or the game's **animal bed** (the straw
bed for farm animals, large or small), built in your base, within 80 m of a flame altar (on the host a new one counts
within seconds), or any prop the host
marks with `beastmaster.stable mark` while looking at it (on a dedicated server a stable counts once a player's game
with Beastmaster sees it, see Playing on a server; an admin can still type `/beastmaster.stable mark` in the chat). An animal bed your farm animals already use counts too; it keeps working for them. Once your world has a stable (it
stays so, kept with the world):
- only **active** beasts can be called, and the wheel lists only those; the others wait in the stable. Calling one in
  the stable says to make it active at a stable;
- at the first stable, each player's beasts that are out become active, then the first of their list, up to their
  slots;
- a beast you tame takes a free active slot; with none free it goes to your stable.

**Beasts at home in the Large Stable.** While you are within 50 m of a Large Stable, the beasts of your collection that
are not out with you (active or in the stable) show up in it: each on a spot of its own on its floor, facing out of its
open side. They stroll about near it for a while, then rest: wolves, hounds and bears lie down, the others stand still.
One that wanders off is put back on its spot. At night they sleep the whole night (no strolls) and get up one by one in
the morning. Each has the game's **E Pet** prompt: a pet turns it to you and settles it for 20 s, and your own beast
earns a loyalty point from your first pet of each game day (a day starts at each dawn while the world runs; the next
pet that day only makes it happy). They never fight: when a monster hits one of them, or a player at the
stable, they go inside out of sight and come back 30 s after the last hit. They have the health they have when you call
them and are healed every second; one that dies anyway comes back a minute later, and you lose nothing. A Large Stable has room for 4 beasts, all players
together (the beasts shown stay; then by player and list order); more beasts need another Large Stable. They leave when
you call them (the called one comes to your side as always), go farther than 70 m away, someone builds within 20 m of
the stable (with the setting `make_room`; back 5 s after the building stops: the game blocks building next to
creatures), or the setting `home_beasts` is off. The small Stable and the animal beds stay empty. `beastmaster.home` (console) says where yours
are and what they do.

**The Beasts window:** every stable shows the game's own **E Open** prompt; press E there (the host). Where a stable has
no prompt (a marked prop with an E use of its own, a prop without a collider, or one whose prompt the game dropped
until the next search), E opens the window too, looking at it within 4 m. It has two tabs at the top: **Stable**
and the selected beast's name (the open one lit). The Stable tab shows your active slots (a beast and its loyalty, an
empty slot, or a locked one with the level that opens it; an active beast that rests shows its time under them) and
the stable: the beasts not in a slot, with their kind, loyalty and whether they are at home or resting. The stable holds
8 beasts besides the active ones (setting `collection_max`; before your world has a stable, that is your whole
collection): a full stable takes no beast you store, and a beast you tame then needs a free active slot. Click a beast (or a slot) to select it: its own tab opens (see Talents); the
**Stable** tab brings you back, the beast still selected. On both tabs:
- **Make active**: into a free slot (it can be called);
- **Store**: out of its slot (a beast that is out goes home);
- **Release**: it leaves your collection for good (a beast that is out leaves the world); click it twice within 5 s.

E or Esc closes the window. **Players who joined** with Beastmaster switched on in their own game get the same window
(see Playing on a server). The others get the stable's commands in the chat when they press E at its prompt, and use
them within 10 m of a stable: `/beastmaster active <name or
number>`, `/beastmaster store <name or number>` and `/beastmaster release <name or number>` (the same command again
within 10 s lets the beast go). `/beastmaster list` shows which beasts are active. Rename a beast with `/beastmaster rename <name or number> <new name>`.

**Your beasts' strength.** A beast out is always at your level (also when you level up while it is out), with the
health and damage a wild one of its kind has at that level, changed by its role:

| Kind | Role | Health | Damage |
|---|---|---|---|
| Wolf | fast hunter | x1.2 | x1.15 |
| Hound | like the wolf, a bit sturdier | x1.3 | x1.1 |
| Rat | a small friend from houses and ruins (it fights as a rat does) | x1.1 | x1 |
| Sabretooth | the striker | x1.15 | x1.25 |
| Snake | quick biter | x1.2 | x1.15 |
| Crimson, golden king, albino snake | (their bites are strong already) | x1.2 | x1, x0.95, x0.9 |
| Boar | tank | x1.7 | x0.9 |
| Aurochs | charging tank | x2.0 | x1.0 |
| Bear | heavy tank | x2.2 | x1.1 |

Between fights it heals: 5 s after its last hit, 5 % of its health a second.

**Loyalty** (1 to 10) grows with the fights a beast takes part in: a point for each hostile creature it hit that dies
within 30 s (wildlife and farm animals give none); 10 points to loyalty 2, 20 more to 3, and so on (450 to loyalty 10;
the **Loyalty grows** setting changes the pace). Each loyalty makes it a little stronger and bigger: at loyalty 10 it
has 20 % more health and damage and is 1.25x its size. The strength comes at once; the size the next time you call it
(the game draws a creature at the size it is made with). `/beastmaster list` shows it; a new loyalty is told to you,
with the talent point it brings.

## The Training Grounds (1.21.0, with the beasts pack 2.3.0)

The beasts pack's **Training Grounds** (crafted at the Workbench, placed in your base like the Stable) is a round
wooden platform with a training dummy in the middle. At it, as at a stable, E opens the Beasts window: select a beast
and **Send to training** (or `/beastmaster train <name>`; **Back to the stable** or `/beastmaster untrain <name>`
brings it back).

- A beast in training earns **loyalty** by the game's clock: 3 points a game day by default (the **Training gives**
  setting; 0 = it only trains for the look). It counts while you are in the world, also when you are out exploring,
  and not while you are not in the world. At loyalty 10 it earns nothing more.
- Each Training Grounds holds **2 beasts** (the **Beasts a Training Grounds holds** setting, 1 to 4), all players
  together; one more is refused until a beast comes back or you build another.
- While you are within 50 m, your beasts in training show up on the platform and pass the time: they go for the dummy
  and swing at it, rest facing it, sniff about, and lie down by the hay, the bowl, the rope or the bones. At night they
  rest. Nothing breaks and they never fight there.
- Calling a beast in training ends its training (it comes to you as usual). A Training Grounds taken down sends its
  beasts back to the stable.
- `/beastmaster train` with no name says where your beasts train and how far they are to the next point.

## The pack llama (with the wilds pack and EMF's Lua 1.42)

A llama that carries your things. It follows you like your beasts but never fights: it is never sent at a monster,
the game's fights are taken off it at once, and monsters are never drawn to it. It has a slot of its own: it comes
whatever your active slots, one per player, and it is not counted in your collection or your stable.

- **Call it** with `/beastmaster pack` (again: it goes home), the wheel's **Call Pack llama**, or **Call** in the Beasts
  window's **Pack beast** row.
- **Its bag:** **E Open** at the llama shows the game's own chest screen, 16 slots. Only its owner opens it; anyone
  else is told whose bag it is.
- **Nothing is lost:** what is in the bag is kept with the world (second by second, and when it goes home, steps aside
  or you leave) and goes back in when it comes again. Gear keeps its level and rarity; its wear is mended.
- **Let go for good** (Release at a stable): what it carried goes into your inventory, the rest drops at your feet.
- **Another player near it:** the bag is not offered while they stand at the llama (the game's chest screen would open
  for them too); it comes back when they leave.
- **If the game ends without saving** (a crash, Alt+F4) right after you put things into the bag, they may be both in the
  bag and in your inventory: the bag is kept at once, your character only when the game saves.
- For now it is given by the host (`beastmaster.give packllama`); a harness to craft is coming.

## Talents

Each loyalty above 1 gives a beast a **talent point** (9 at loyalty 10). Spend them at a stable: on the beast's tab of
the Beasts window (its **+** buttons), or `/beastmaster talent <name or number> <talent>` within 10 m of a stable.

| Talent | Per point | Most | What it does |
|---|---|---|---|
| Strength | +4 % damage | 5 | on top of its role and loyalty |
| Toughness | +5 % health | 5 | on top of its role and loyalty |
| Swiftness | +4 % speed | 3 | it moves faster |
| Second Wind | | 1 | a killing blow leaves it at 20 % health instead; then 5 minutes before it works again |
| Ambush | +50 % | 2 | its first hit on a foe in a fight deals that much more (a bonus that kills is your kill: XP and loot, with You get your beasts' kills on) |
| Guard | 3 s, 5 s with 2 points | 2 | it takes no damage for the first seconds of a fight (the hit that starts it is healed back), once every 30 s |
| Menace | +10 % of the monsters | 3 | it draws more of the monsters in a fight (see Monsters and your beasts) |
| Taunt | | 2 | when a monster hits you within 10 m of it, it roars: the monsters of the fight within 10 m of it turn on it for 6 s; then 45 s before it roars again (30 s with 2 points) |

A fight starts with the first hit to or from the beast after 10 s without any. A beast that is out gets Strength,
Toughness, Swiftness, Second Wind, Menace and Taunt at once, Ambush and Guard from its next fight. Talents act only
while the beast is out. **Reset talents** (on its tab, or `/beastmaster talents <name or number> reset` at a stable)
gives every point back, free, at once. When a beast has more points spent than its loyalty gives (its loyalty was set
lower, or the Loyalty grows setting turned down), it keeps them, but no new point can be spent until a reset ("2 too
many: reset to choose again"). A release forgets a beast's talents; a rename keeps them. The numbers are in one table at
the top of `talents.lua`.

**The beast's tab** shows the beast under its name, slowly turning (a picture EMF draws from your own game, see Good to
know), then its kind and level (yours), its loyalty and the kills to the next, what its role, loyalty and
talents make of its health, damage and speed ("Health x1.32, damage x1.27, speed x1.08"), whether it is out, at home or
resting and active or in the stable, then "Talents: 3 points (1 left)", a row per talent with its points ("Strength 2/5:
+8 % damage") and a **+** (off when the talent is full or no point is left; Second Wind says "ready" or when it works
again while the beast is out), **Reset talents**, and Make active, Store and Release.

**The wheel (hold F3):** Tame, Feed, Attack, **Mode** (steps passive, defending, aggressive), **Stable: animal bed** (with the beasts pack: **Stable: Workbench**) while your world has no stable (it says what to build), **Come back** while your beasts stepped aside by `/beastmaster aside`,
**Dismiss <name>** for each beast that is out (and **Dismiss all** with two or more out), then **Call <name>** for up
to 6 beasts that are at home or resting (once your world has a stable: the active ones); a resting one shows its time
left. The labels are short so they fit the
wheel; the mouse wheel turns the page when there are more than 8 items. With **Necromancer** installed too, F3 and F2
open one "Summons" wheel with "Beasts: ..." and "Undead: ..." items; turn **One wheel** (`shared_wheel`) off to keep
Beastmaster's own wheel. A line at the bottom left lists your beasts that are out, aside or resting: "Beasts: Wolf
(out), Bear (rests 1:20)".

**Wild bears, aurochs and snakes (with their packs):** about every 30 minutes of play, when there is no wild one yet, one of
them (at random, from the packs you have) appears 50-70 m from a player who is out in the open, at that player's level;
never within 40 m of a flame altar. Beat it down or feed it its bait to tame it.
- **Bear** (bear pack): 1.4x a wolf's size, 2.5x its health, 1.5x its damage, a little slower.
- **Aurochs** (beasts pack): a huge horned wild bull on the boar's skeleton, it charges like a boar: 1.3x a boar's
  size, 3x its health, 1.4x its damage, a little slower.
- **Snake** (beasts pack): a 5 m snake from a 3D scan of a pit viper, on a spine and animations of its own: it
  slithers, rears up like a cobra and strikes; 2x a wolf's health, 1.3x its damage, a little slower.
- **Rare snakes** (beasts pack): now and then a wild snake is a rare one, bigger and tougher. Whoever slays it (the
  last player to hit it, else the nearest player within 60 m) gets a bonus in their bag: runes, by chance a tier-2
  rune, and by chance a weapon of that rarity at their level.

  | Kind | Of the wild snakes | Size | Health / damage (x a wolf's) | Runes | Tier-2 rune | Weapon |
  |---|---|---|---|---|---|---|
  | Crimson snake | 1 % | 1.15x | 3x / 1.6x | 3-5 | 15 % | 5 %, epic |
  | Golden king snake | 0.4 % | 1.25x | 4x / 1.8x | 4-6 | 25 % | 10 %, epic |
  | Albino snake | 0.25 % | 1.35x | 5x / 2.0x | 6-10 | 50 % | 25 %, legendary |

## Monsters and your beasts

Monsters in a fight with you or your beasts split between you and the beasts that are out. Each beast draws a share of
them:

| Beasts | Draws | With Menace (3 points) |
|---|---|---|
| Tanks: boar, bear, aurochs | 50 % | 80 % |
| The others: wolf, hound, rat, sabretooth, snakes | 20 % | 50 % |

The shares of several beasts add up; you keep the rest. Your beasts draw at most 90 % of the monsters in all, and of a
boss (or a mini-boss) at most 35 %, also when they stand much nearer to it than you do. The beast's tab says what it
draws ("Draws 50 % of the monsters in a fight").

- **Who a monster goes for:** when it joins a fight with you or your beasts (it hits you or a beast, you or a beast hit
  it, or it goes for you or a beast), it picks between you and your beasts within 30 m of it by those shares, the nearer
  of your beasts weighing more. A monster that goes for another player or another player's beast is theirs.
- **Groups split:** at least 1 in 4 of the monsters in a fight stay on you, from 2 monsters on and rounded up (1 of 2,
  2 of 5, 3 of 9), however high the shares; a single monster can go to a beast.
- **No flip-flopping:** a monster keeps its target at least 8 s, then picks again. It picks again at once when its beast
  falls, is sent home or is out of its reach; when none of your beasts is left near it, it is the game's again.
- **Taunt** (a talent) sends the monsters near a beast at it for 6 s when one of them hits you; a boss in the middle of
  an attack turns when the attack ends. A roar that turns no monster is not spent.
- **Passive mode:** your beasts draw no monsters; the game decides alone.

A monster picked for a beast is held on it: Beastmaster writes the beast as its pick (the game's own target choice).
One picked for you is left to the game, which picks for it (it counts for the 1 in 4). A boss in the middle of an
attack keeps its target until the attack ends. The game gets its pick back when the fight ends for that monster (no hit
between it and you or your beasts for twice the hold, 16 s, or it goes home, flees or stands idle), the beast falls, you
leave, you switch to passive mode or turn **Beasts draw monsters** off. `beastmaster.threat` (console) lists the
monsters in your fights now and who they are on.

## When you stand still

When you have stood still (moved less than 1 m) for 10 s, out of a fight (no hit to or from you or your beasts for
10 s, no monster held in your fight) and not building, your beasts pass the time, each in a scene of its own, and pick
a new one every 30 to 60 s. A beast takes part while it follows you within 15 m and is not busy (no Attack order, not
falling, in water or fighting); one that wanders farther, or that the game moves on, drops out alone and joins again
when it is back. The wolf, the hound and the bear can lie down; the other beasts stand still where a scene keeps them.

- **Settle:** a wolf, a hound or a bear lies down where it is. The other beasts do not settle.
- **By the fire:** with a campfire, a fireplace or a base's flame altar within 15 m of it, the beast turns to face the
  nearest and stays there (a wolf, a hound or a bear lies down facing it).
- **Watching:** with a villager within 15 m of it, it turns to face them and stays there (a wolf, a hound or a bear
  lies down facing them).
- **Night watch:** at night (18:00 to 06:00) with two or more beasts taking part, one that is not a wolf, a hound or a
  bear keeps watch, standing and facing away from you, while the wolves, hounds and bears settle. With only wolves,
  hounds and bears, all of them settle. A lone wolf, hound or bear settles; another lone beast picks as by day.
- **Sniffing about:** it wanders around you, as it does anyway.

A beast that turned keeps facing that way while it stays. They all get up and follow you the moment you are 2 m from
where you stood, a fight starts (a hit to or from you or one of them, or a monster joins a fight with you), you call,
dismiss or store a beast or send them with Attack, or you start building; then they wait for another 10 s of
stillness. Each player's beasts follow only that player's stillness. **Beasts pass the time while you stand still**
(`idle`) turns it off; **After you stand still for** (`idle_s`) sets the 10 s. `beastmaster.idle` (console) says what
each beast does now.

## Playing on a server

Beastmaster runs where the world runs (the host, or a dedicated server). A player who joins:

- **with EMF and Beastmaster switched on in their own game** gets everything the host gets, drawn on their own screen:
  the Beasts window (E at a stable), the beast wheel (hold F3, their own key), the beast line at the bottom left, and
  the news as notes at the top of the screen. The beast's turning picture is drawn from their own game. The server
  decides everything: their game only shows what the server sends and passes clicks back, so nothing can happen that
  the server did not allow. It needs the same Beastmaster version on both sides (1.15 or newer). Their game shows a
  Beasts window the server opens only at a stable it shows (the game's E prompt, whatever key or controller they use;
  not again within 3 s of closing one) or within 5 s after they pressed E or their stable key, so a server cannot keep
  opening it on them; while it is open, their game counts its rests down itself (with 1.15.0 on their side they move
  every 15 s).
- **without it** plays as before: the stable's commands and everything else with `/beastmaster` in the chat, the news
  as chat lines. When they join, EMF's greeting may show in their chat, at most twice: "This server runs EMF mods. Get
  EMF to see their windows; you can play without it." Only they see it. (On a server with content mods, such as the
  creature packs, or one that refuses games without its mods, it says they need EMF and the same mods.)

**For server owners:** a dedicated server has no models to see a stable by, so the games of players with Beastmaster
tell it where the stables they see are (a Stable, a Large Stable or an animal bed within 80 m of a flame altar). Those
stables count for everyone, are kept with the world, and stop counting when a player's game near one no longer sees
it. Marking stables (`/beastmaster.stable mark`) still works. The Large Stable's beasts at home show up there too. The
first stable a world gets makes it one where only active beasts can be called (as in a game someone hosts): players
without EMF then choose their active beasts with the chat commands at a stable. Any player's game can report a stable,
so a player could make a prop count as one; a stable only opens the window and holds the beasts at home.

`beastmaster.as #2 give wolf`, `beastmaster.as #2 open` and the like (console only) run a word as another player, for
testing on a dedicated server, where the console has no player of its own. `open` shows the window on that player's
game once without a key press; after that, only at a stable or within 5 s after they pressed E or their stable key.

## Markers

With **Beast markers** on, every beast you have out shows its name, how far it is and its health (a bar, with EMF
0.6.19 or newer on your game) over its head, an arrow at the
edge of the screen when it is behind you or off screen, and a marker on the map (M). With **Stable markers** on, the
world's stables show on the map and, within 200 m, in the world. Both are **off** at the start.

- The settings are the world's (the host's or the server's). Each player can choose for themselves:
  `/beastmaster markers on|off` and `/beastmaster stable_markers on|off` (kept with the world for that player).
- You see only your own beasts; the stables are everyone's.
- Players who joined need EMF on their own game (EMF draws the markers there); Beastmaster on their game is not needed.
- The markers hide while a menu or the map shows its own screen, and while EMF's console is open.

## Commands

All of these work from the game's chat for every player (`/beastmaster <word>`), and in the console
(`beastmaster.<word>`). Without a word, `/beastmaster` lists them.

| Command | What |
|---|---|
| `/beastmaster list` | your beasts and their loyalty: "1. Wolf - wolf, loyalty 3, out, active", "2. Bear - bear, loyalty 1, rests 0:40" (", active" once your world has a stable) |
| `/beastmaster slots` | your active slots and the level that opens the next one |
| `/beastmaster active <name or number>` | within 10 m of a stable: make a beast active (into a free slot) |
| `/beastmaster store <name or number>` | within 10 m of a stable: put a beast into the stable (one that is out goes home) |
| `/beastmaster release <name or number>` | within 10 m of a stable: let a beast go for good (the same command again within 10 s) |
| `/beastmaster talents <name or number>` | a beast's talents: its points and each talent's |
| `/beastmaster talent <name or number> <talent>` | within 10 m of a stable: spend a talent point (strength, toughness, swiftness, second wind, ambush or guard) |
| `/beastmaster talents <name or number> reset` | within 10 m of a stable: every talent point back, free |
| `/beastmaster rename <name or number> <new name>` | rename a beast (1-20 characters, not a name your collection has) |
| `/beastmaster train <name or number>` | send a beast that is not out to the nearest Training Grounds with room; no name: where your beasts train |
| `/beastmaster untrain <name or number>` | bring a beast in training back to the stable |
| `/beastmaster tame` | tame the beast you look at, else the nearest beaten-down one (under 25 % health, within 8 m) |
| `/beastmaster markers on\|off` | markers over your beasts, for you (see Markers) |
| `/beastmaster stable_markers on\|off` | the stables on your map and nearby, for you |
| `/beastmaster feed` | feed the beast you look at (else the nearest) its bait: 3 times to tame it |
| `/beastmaster call <name or number>` | call a beast from your collection |
| `/beastmaster dismiss <name or number>` | send that beast home |
| `/beastmaster dismiss` (or `dismiss all`) | send every beast that is out home |
| `/beastmaster attack` | your beasts that are out attack the creature you look at at once (else the nearest monster where you look, else what hit you last) |
| `/beastmaster aside` | your beasts that are out step aside (leave for now), or come back; they do it on their own while someone builds next to them |
| `/beastmaster mode [passive, defending or aggressive]` | how all your beasts fight; without a word, the mode now |
| `beastmaster.stable mark`, `unmark`, `list` | **host or admins:** the prop you look at (no camera data: the one within 3 m of you) is a stable; the marked prop nearest you (within 8 m) is no stable; this world's stables |
| `beastmaster.threat` | **console:** the monsters held on you or your beasts now, and for how long |
| `beastmaster.idle` | **console:** what your beasts do while you stand still now (their scenes), or how long you have stood still |
| `beastmaster.home` | **console:** where your beasts at home are (in a Large Stable) and whether they stroll or rest |
| `beastmaster.air` | **console:** each player's movement state as this world sees it, and whether their beasts count them in the air (on a dedicated server: whether a joined player's glide reaches the server) |
| `beastmaster.spawn <species>` | **console, admin:** puts a wild beast of that species 5 m in front of you, to try taming it. Since 1.16.1 a beast being tamed stops fighting you, a tamed beast loses its debuffs (a bleed no longer kills it after taming), and a beast brought back from far away drops its chase and follows you again |
| `beastmaster.loyalty <name or number> <1-10>` | **host console only:** set a beast's loyalty (tests); the reply says its talent points |
| `beastmaster.give <species>` | **host console only:** add a beast to your collection without taming it (wolf, boar, sabretooth, hound, bear, aurochs, snake, crimson_snake, golden_snake, albino_snake; with the wilds pack fox, arctic_fox, silver_fox, sand_fox, white_wolf, black_wolf, red_wolf, brown_bear, black_bear, snow_bear, cinnamon_bear, wilds_panther, black_panther, jaguar, snow_leopard, tiger, lynx, snow_lynx, dark_lynx, red_lynx) |
| `beastmaster.wild_swap <entity> [roll 0-1]` | **console:** a wild wolf, sabretooth, goat or yak becomes one of the wilds pack's animals now (a roll under `wild_coat_pct`: a colour version), for checks in game (1.21.0) |
| `beastmaster.as <#k or player entity> <word> [...]` | **host or server console only:** run a word as that player (#2: the second player, lowest entity id first): `give`, `loyalty`, `open [name]` (their Beasts window, on that beast's tab), or any `/beastmaster` word; for tests on a dedicated server |
| `beastmaster.species <graph guid> <faction> [scale]` | for modders: which beast a creature is, by the animation graph its template runs, its faction number and its scale (render models are not on the server world; the bear its own copy of the wolf's graph at 1.4x (the bear pack 1.0's: the wolf's graph at 1.4x), the aurochs the boar's at 1.3x, the snake and each rare snake its own copy of the wolf's graph) |

## Settings (Mods tab, EMF.exe, or `set beastmaster <key> <value>`)

| Setting | Default | What it does |
|---|---|---|
| Beast wheel (`wheel_key`) | F3 | hold it for the wheel (any key the game does not use; B is its backpack key) |
| Beasts window (at a stable without the prompt) (`stable_key`) | E | a stable has the game's E Open prompt; this key opens the window only at a stable without the prompt (a marked prop with its own E use or no collider, a prompt the game or EMF dropped), under your crosshair within 4 m; elsewhere E does what the game does |
| Active slot 2, 3, 4 at level (`slot2_level`, `slot3_level`, `slot4_level`) | 10, 20, 30 | 0: that slot never opens |
| Beasts at home live in a Large Stable (`home_beasts`) | on | off: the Large Stable stays empty, like the Stable |
| One wheel with Necromancer's undead (`shared_wheel`) | on | off: Beastmaster's own wheel |
| Mana to call a beast (`call_mana_pct`) | 15 % | of your maximum mana; 0 = free |
| Seconds between calls (`call_cooldown_s`) | 10 | |
| Rest after a beast falls (`rest_s`) | 60 s | |
| A fallen beast comes back by itself after its rest (`back_after_rest`) | on | free, beside you, until you dismiss it; it keeps its active slot while it rests. Off: it waits at home after its rest |
| Beasts in the stable, besides the active ones (`collection_max`; before a stable, the whole collection) | 8 | 1-16 |
| Tame by defeat below (`tame_health_pct`) | 25 % | |
| Taming takes (`tame_channel_s`) | 3 s | |
| Taming range (`tame_range_m`) | 8 m | |
| Baits to tame by feeding (`feed_count`), Feeding window (`feed_window_s`) | 3, 10 s | |
| Bait for wolves, hounds, boars, sabretooths, bears, aurochs, snakes, the rare snakes (`bait_<kind>`) | see above | an item's debug name (`items meat` in the console) |
| Beasts drop a chase at (`chase_m`) | 45 m | they turn from the fight and run back |
| Beast health (`beast_health`) | 150 % | every beast's health against its kind's own (its role, loyalty and talents on top); applies at once to the beasts out and (1.20.1) the beasts at home (1.20.0; before: 100 %) |
| Beast damage (`beast_damage`) | 125 % | the same for the damage they deal (1.20.0; before: 100 %) |
| Loyalty grows (`loyalty_pace`) | 1x | 2x: half the kills per loyalty |
| Training gives (`training_points`) | 3 | loyalty points a game day for a beast at a Training Grounds (0-12) |
| Beasts a Training Grounds holds (`training_room`) | 2 | 1-4, all players together |
| Beasts come back from (`leash_m`), At once from (`leash_far_m`) | 60 m, 120 m | |
| Aggressive beasts go at foes within (`aggro_m`) | 25 m | |
| You get your beasts' kills (`xp_credit`) | on | XP and loot for the owner |
| Beasts leave small critters alone (`spare_critters`) | on | rats in houses and ruins: beasts neither go after them nor fight them, unless one bites you or a beast (then for 30 s); Attack on one is still your call (1.21.0). A rat can also be tamed as a beast with red berries |
| Beasts step aside while someone builds next to them (`make_room`) | on | while you, or a player within 20 m of them, hold something to build or place, your beasts leave and come back after |
| Beasts pass the time while you stand still (`idle`) | on | off: they keep following you as before (see When you stand still) |
| After you stand still for (`idle_s`) | 10 s | 5-60 |
| Beasts draw monsters to them (`threat`) | on | off: monsters pick their targets as the game does |
| A monster keeps its target at least (`threat_hold_s`) | 8 s | |
| A tank (boar, bear, aurochs) draws, Another beast draws (`tank_share_pct`, `damage_share_pct`) | 50 %, 20 % | of the monsters in a fight, before Menace |
| All your beasts draw at most (`beasts_share_max_pct`), Of a boss (`boss_share_max_pct`) | 90 %, 35 % | |
| Rare wild bears, aurochs and snakes (`wild_bears`), About every (`wild_bear_minutes`) | on, 30 min | needs their pack |
| Our animals in place of the game's wolves, sabretooths, goats and yaks (`wild_replace_pct`), Rare colour versions of them (`wild_coat_pct`), Rare colours are stronger (`wild_coat_strength`) | 15 %, 4 %, 125 % | needs the wilds pack; 0-50 %, 0-20 %, 100-200 % health and damage (1.21.0) |
| Markers over your beasts (`beast_markers`) | off | each player can choose: `/beastmaster markers on\|off` |
| Stables on the map (`stable_markers`) | off | each player can choose: `/beastmaster stable_markers on\|off` |

## The bear pack (`emf_beastmaster_bear`)

An EML data patch that adds one new creature, `Beastmaster_Bear` (template `b3a7c2e1-5f4d-4c8b-9e21-7a0d4b6e2f02`). Since
3.0 it is the wilds pack's broad brown bear, made in the game's style and fitted straight onto the game sabretooth, so
it moves and fights with the sabretooth's own animations and AI; the earlier bear (on a skeleton of its own) is gone,
and the bears you tamed come as this one, with the same name, loyalty and talents. It is in
the **EMF creature packs** download on EMF's Nexus page. Install it with EMF.exe ("Install a downloaded file") or put
its folder in the `mods` folder next to the game; EMF builds it when the game starts (it is EML content: EML or
ShroudForge can apply it too). The template sets the size (`ScaleComponent` 1.4, which is also how Beastmaster tells a
bear from a sabretooth). The health (x2.5) comes from Beastmaster when it spawns a bear.

To rebuild it from its sources (for modders; the sources are not in the repository): `python -I
tools/models/make_mod.py tools/models/packs/beastmaster_bear.json`. The pack holds the new model and its texture, plus
instructions that copy and change the game's sabretooth at load time; no game files are copied into it.

## The beasts pack (`emf_beastmaster_beasts`)

An EML data patch with new beasts:
- the Aurochs, `Beastmaster_Aurochs` (template `d5f2b8e3-7c4a-4e9f-8b21-6a3c8d4e0f02`), made with Meshy and fitted to
  the boar's skeleton (its animations and AI): size (`ScaleComponent`) 1.3, damage x1.4, speeds x0.9; the health (x3)
  comes from Beastmaster. Its tail follows its hips.
- the Snake, `Beastmaster_Snake` (template `d4c87337-8c53-41df-8488-8079046e1502`), on a spine and animations of
  its own: "CC0 Japanese Pit Viper" by ffish.asia / floraZia.com (CC0), a 3D scan scaled to 5 m, its slither, rearing,
  strikes and death made for it, played by a copy of the wolf's animation graph (the wolf's AI); damage x1.3, walk
  1.6 and run 4.5 m/s; the health (x2) comes from Beastmaster. Pack 1.0's snake was a cartoon snake.
- the rare snakes, `Beastmaster_CrimsonSnake`, `Beastmaster_GoldenKingSnake` and `Beastmaster_AlbinoSnake` (templates
  `d4c87337-8c53-41df-8488-8079046e1512`, `...1522`, `...1532`): the snake with the scan's skin recoloured, each
  its own copy of the skeleton and graph; sizes 1.15, 1.25 and 1.35, damage x1.6, x1.8 and x2.0; the health (x3, x4,
  x5) comes from Beastmaster. With an older beasts pack that lacks them, a wild snake is always a plain one.
- the Stable, `Prop_Beastmaster_Stable` (template `96a057d5-a1d9-4fc1-b313-9b923e9302fe`, pack 2.1.0): a small open
  stable with a thatched roof (its shape made with Meshy, its thatch and planks the game's own materials), 4.5 m wide,
  3 m deep and 3.4 m high, on a copy of the game's large animal bed without the parts farm animals and villagers use;
  its model is its own (Beastmaster knows the Stable by it), and you walk on its plank floor and not through its back
  and left walls or the low rail on the right. Monsters neither target nor break it (the animal bed's parts for that are
  taken off, as furniture has none). Crafted at the Workbench. Like every
  placed object it is saved only inside a base, and removing the pack removes placed Stables from the world when it
  next loads (pick them up first).
- the Large Stable, `Prop_Beastmaster_Large_Stable` (template `c110e001-4b38-462f-a317-3a6b0d627511`, pack 2.2.0): the
  Stable 1.6 times as big (7.2 m wide, 4.7 m deep, 5.5 m high), its thatch and planks as big as on the Stable, its own
  model (Beastmaster shows your beasts at home in it), its floor, walls and rail solid like the Stable's. Crafted at the
  Workbench; saved and removed like the Stable.

Installed and rebuilt like the bear pack (`tools/models/packs/beastmaster_beasts.json`). Without the pack (or when its
content was not built), an aurochs or snake in your collection stays at home: calling it says that it comes with the
EMF creature packs and how to install them, and one that was out when you saved goes home with one note when you
load.

## Our animals in the wild: the wilds pack (`emf_beastmaster_wilds`, 1.21.0)

An EML data patch with 43 animals made in the game's style, each fitted straight onto a game animal's skeleton, so it
moves with that animal's own animations and AI: the fox and the coloured wolves on the wolf's, the bear, panther and
lynx on the sabretooth's, the goat, ram, deer and elk on the goat's, the Wild Ox on the yak's, and the pack llama
("The pack llama" above). With it installed:
- **In the wild:** a game wolf, sabretooth, goat or yak that turns up is now and then one of these instead (15 %), and more
  rarely a colour version (4 %) with more health and damage (125 %). A wolf can be a fox, or an arctic, silver or sand
  fox or a white, black or red wolf. A sabretooth can be a bear, panther or lynx, or a black or cinnamon bear, a black
  panther, jaguar or tiger, or a dark or red lynx. A snow sabretooth of the Cold Heights can be a bear or lynx, or a
  snow bear, snow leopard or snow lynx. A goat can be a goat or deer (a black or piebald goat, a dark or fallow deer);
  a mountain goat a ram or elk (a chamois, a black or golden ram, a mouflon, a white, dark or grey elk, a white deer);
  a yak a Wild Ox (a white, black or red ox). The goat's, ram's, deer's, elk's and ox's kinds keep the game's wild
  animal mind, so they are never tamed.
  Each has the level of the creature it replaced and that creature's loot.
- Never a creature within 25 m of a player (nothing changes in front of you), within 40 m of a flame altar, hurt, or
  one that was there when the world loaded.
- The server world has no render models for creatures; on a game you host your own game's world tells a snow
  sabretooth or a mountain goat by its model. A dedicated server cannot: there they get the sabretooth's or the goat's
  animals.
- **Taming:** the foxes, coloured wolves, bears, panthers and lynxes are tamed like any beast (beaten down, or fed:
  lean meat for foxes and wolves, red berries for bears, cat bait for panthers and lynxes) and called from your
  collection; a colour version is about 10 % stronger than its animal. The goat, ram, deer and elk run the game's
  wildlife AI and cannot fight, so they are never tamed.
- Beastmaster knows these animals by its list of the ones it put into the world (nothing else on the server tells a fox
  from a wolf): after a script reload the ones already about cannot be tamed until the next world load.
- Without the pack nothing is replaced: the first spawn the game refuses turns this off until the next world load, with
  one line in the log, and takes nothing out.

## More beasts: `species_extra.lua` (for modders)

A file `species_extra.lua` in Beastmaster's folder (beside `species.lua`) adds beasts: a creature of your own, or one
from a content pack you cannot share. It returns a list of entries like the ones in `species.lua`:

```lua
return {
	{ id = "troll", name = "Troll", plural = "trolls",
		template = "<GUID of the template a call spawns>",
		graph = "<GUID of the animation graph the creature runs>",
		factions = { [3] = true },  -- optional: only creatures of these factions (3: Vukah)
		health_mul = 3,             -- optional: times the game's base health
		bait = "Food_T2_Dog_Bait",  -- optional: the item it is fed; without one it is tamed only by beating it down
		pack = "my content pack" }, -- optional: it also comes as a rare wild beast, like the packs' beasts
}
```

Each entry needs `id`, `name`, `template` and `graph`. An entry without them, or with an id that is taken, is
skipped. EMF's log lists the skipped entries and the beasts added, and says when the file does not load. Beastmaster
does not ship this file. A beast added here has no picture on its tab (the pictures are listed in Beastmaster's
`mod.json`).

## Good to know

- It runs where the world runs: the host or a dedicated server. Without the packs, players who join need nothing:
  they see the beasts and use `/beastmaster` in the chat (with Beastmaster switched on in their own game, the window,
  the wheel and the line too: see Playing on a server). **With a pack, everyone needs it:** the host (or the server)
  and every player who joins, because it changes the game's data (an EML patch).
- Render models are only on the game's client side: a dedicated server sees a built stable through the games of the
  players with Beastmaster (they report it), or an admin marks it (`/beastmaster.stable mark`).
- A stable's prompt is on the stable's entity in the world the host or the server runs; Beastmaster offers it again at
  every search of the world (every 30 s), so one the game dropped (the bed streamed out of view) comes back. With
  EMF's Lua 1.15 it also sees when EMF dropped it (`emf.offered`): the E key opens the window there meanwhile.
- Your collection is kept with the world, per player (by Steam account).
- The picture on a beast's tab: EMF draws one of each kind from your own game files when the game starts (the
  `portraits` list in `mod.json`; EMF 0.5.2 or newer). The first start after installing shows a short splash while it
  draws. Nothing of the game's art ships with the mod. A pack beast's picture is drawn when its pack is installed at
  that start; with a pack added later it comes at the next start (with EMF 0.5.2 itself only after the next game or EMF
  update: that version does not try a failed picture again before then). Until a picture is there, or with an older
  EMF, the tab shows the rest as before.
- A beast that is out is taken out of the world while its owner is away, and comes back when they return.
- A beast another mod walks somewhere (EMF move orders) is left alone until that walk ends: one out is not brought back
  to you meanwhile, one at home in a Large Stable is not put back on its spot (it is after the walk). A beast out that
  such a walk takes out of the world (too far away) comes back beside you.
- Beasts are ordinary game creatures with the enemy AI: they guard you as the game's companions do, and sometimes take
  their own path to a foe.
- The bear's and the aurochs' health boost needs Beastmaster to spawn them: one made another way (the `spawn` console
  command) has a wolf's or a boar's health.
- Beasts are called with the wheel or the chat command; there are no spell items for it. The species are the six
  above.
- Second Wind and Guard work through the game's minimum health of the beast (as the 5 s after a taming do); Swiftness
  through its Movement multiplier, the one value `SPEED_ENTRIES` in `talents.lua` names.
- A monster's target is held through its `SelectedTargets` pick, written every server step with its target switches
  (`DynamicAggro.allowTargetSwitches`) off; the game's threat event alone does not move a monster. A `species_extra.lua`
  beast is a tank with `tank = true` in its entry.
- Second Wind's 5 minutes are not kept with the world: after a reload it is ready again.
- A beast stays put through the game's own sleep: Beastmaster switches the Sleep feature on in the beast's AI
  (`DynamicEnemy01BehaviorState`: the Sleep bit in `enabledFeatures`, state Sleep, `sleepEndTime` 2 minutes ahead and
  renewed every 5 s, the game's own sleep check pushed far ahead), and the game plays the sleep animation of a graph
  that has one (the wolf's, which the hound runs too, and the bear pack's copy of it, whose sleep clips are the bear's
  lying idle); a beast whose graph has none stands there. If Beastmaster stops (an error), a lying beast gets up by
  itself within about two minutes. A turn is the game's teleport in place with a yaw; while a turned beast stays, its
  AI's own turning (Rotate, RotateWhileStanding, IdleTurn in `disabledFeatures`) is off, and back as it was when it
  gets up. Lua is not given the game's time: Beastmaster takes it from a called beast's `SpawnTime`, or from an AI's
  state change; a beast tamed in this session and not called since waits for that before it lies down. A player-side
  creature left with the Sleep bit (after a reload of the mod, for example) is woken at the next search of the world.
  A `species_extra.lua` beast lies down with `sleeps = true` in its entry; `stays = false` keeps a kind out of the Sleep
  state (it only turns).
