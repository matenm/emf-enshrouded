# Bulk Craft (`bulk_craft`)

Craft one by hand, then craft many. Look at a crafting station and press **U**: a wheel offers the recipes you
crafted there by hand, then **x5 / x10 / x25 / max**. Or type **`/craft 25`** in the chat. Vanilla Enshrouded crafts
one item per click; "Craft All / craft multiple", mass-crafting building blocks, "cook all" and a crafting queue
together have about 150 votes on the official feature board.

It runs where the world runs, so on the host or a dedicated server it works for every player, also those who joined
without EMF.

## Switch it on

Bulk Craft is installed **switched off**. Switch it on in the Mods tab (the key under Esc) or in EMF.exe, then restart
the game (or the server).

## How it works

- **The game crafts, not the mod.** For each craft the mod sends the game's own craft request with you as the crafter,
  exactly as the game's crafting window does: the game takes the materials from your inventory and puts the item into
  it, all or nothing per craft. The mod never takes or gives an item itself, so nothing can be lost or copied.
- **Only what you crafted by hand.** The game's crafting window offers only recipes you have unlocked, but the game's
  craft request itself does not check unlocks (tested in game: a level-1 character could have made level-47 fire bolts).
  So Bulk Craft repeats only recipes you crafted at least once by hand, through the game's own window. It remembers them
  per world and per platform account (`emf\config\bulk_craft\<world slot>.txt`): a second character of the same account
  shares them. Craft one, then bulk-craft the rest.
- **The items you chose.** For a recipe with a choice of items ("any fibre", "any wood"), the crafting window sends
  which item you picked. Bulk Craft keeps that choice with the recipe and sends exactly it every time, and counts those
  items when it checks your materials, so it never spends a rare material you did not pick. Pick other items by hand
  once and that becomes the choice. A recipe with such a choice that it never saw crafted by hand is refused.
- **At a station within reach.** The station under your crosshair, or the nearest one within 5 m (`range`); walk
  farther away and the order stops. Without a station near, your hand recipes (the ones from the inventory screen).
  Never a factory (kilns, smelters: they run their own timers), the flame altar or an upgrade.
- **No recipe with station requirements.** A recipe that needs crafting props nearby, comfort, a level, world
  progress, happy villagers, energy, water or a shelter is refused ("craft it by hand"): the game's craft request may not
  check these, and EMF cannot check them. (In this game build, no hand or workbench recipe has any.)
- **At the game's pace.** One craft at a time, each after the recipe's own crafting time, at least 0.5 s
  (`seconds`). With `pace = instant`, recipes without a crafting time are made all at once; recipes with one still wait
  for it. The pace carries over from one order to the next (a quick `/craft 1` again, or a stop and a new start, waits
  for it too). Nothing is ever faster than crafting by hand.
- **As many as your materials pay for.** "Max" makes as many as you have materials for, up to 100 per order
  (`max_amount`). An order stops when the materials run out, your bag is full, or the game refuses a craft (a
  requirement not met), and says how many it made: "Bulk craft: made 250 Stone Rough (25 crafts)".
- **From your bag only.** After each craft Bulk Craft checks that the materials left your bag. If they did not, the
  game took them from somewhere else (your base's storage, maybe another player's chest): the order stops and that
  recipe is refused at that kind of station until the world is loaded again (the log says so).
- Press the key again (or `/craft.stop`) to stop an order. A player who leaves loses their order; whoever joins next
  into that player slot is told nothing about it.

## Commands (every player, also from the game's chat)

| Command | What it does |
|---|---|
| `/craft <how many> [part of the name]` | `/craft 10`: 10 more of what you crafted last here; `/craft max string`: as much string as you have materials for. Amounts: a number, `x10`, or `max` |
| `/craft.stop` | stops your order |
| `/craft.list` | what you can bulk-craft where you stand |
| `craft.teach <recipe id> [player]` | host only (the console or `emf\commands.txt`, never the chat): lets a player bulk-craft a recipe as if crafted by hand, for recipes crafted before the mod was installed. When a player asks for a recipe they never crafted by hand, the log names its id. A recipe with a choice of items still needs one hand craft (for the choice) |

## Settings (Mods tab, EMF.exe, or `set bulk_craft <key> <value>`)

| Setting | Default | What it does |
|---|---|---|
| Bulk craft key (host) (`key`) | U | the wheel; again to stop |
| Pace (`pace`) | paced | `paced`: one at a time; `instant`: recipes without a crafting time all at once |
| Seconds per craft (`seconds`) | 0.5 | 0.1-5, for recipes without a crafting time (paced) |
| Most crafts per order (`max_amount`) | 100 | 5-500 |
| Station within (m) (`range`) | 5 | 2-15 |
| Notes on screen (`notes`) | on | the host's notes; players who joined get chat lines |

Players who joined get the "made 25 ..." line at the end of an order as a chat line, which needs EMF API v19 (EMF 0.3
or newer). With an older EMF they get only the first answer to `/craft`; the end goes to the log.

## How it is done (for the curious)

A test in game (`docs/engine_parts/inventory_crafting.md`, section 4, Crafting) sent the game's `CraftRecipeEvent` with a
player as the operator: the game crafts `recipeAmount` times from the player's inventory, as many as the materials pay
for, with no timer, no unlock check, no distance check and no need to have the station open; the station must make the
recipe. The mod adds what the game leaves out: it learns a player's recipes and their category choice from the game's
own craft requests (the crafting window sends the same event; the mod tells its own sends apart by counting them),
checks the station, the distance and the requirements, paces the crafts per account, counts each craft by its item
arriving in the player's inventory (`PickupEvent` from nowhere), and checks that its materials left the bag. The
category items come from the recipe registry's category map (layout from the game's type data, not seen in game). The
rules are in `bc_logic.h` and the game side in `bc_glue.h`, unit-tested in `tests/test_bulk_craft.cpp` and
`tests/test_bulk_craft_glue.cpp`.

## Good to know

- Crafting from chests: the game's own window can take materials from your base's storage; Bulk Craft's requests use
  only what you carry (seen in game). Carry the materials.
- Another mod that sends the game's craft request with a player as the crafter (none of the mods shipped with EMF does,
  but a Lua mod could) counts as that player's hand craft and teaches Bulk Craft the recipe.
- The pinned recipe is not used: the game keeps it only on the player's own PC, where a server cannot read it.
- In someone else's game it does nothing (that game's host decides).
