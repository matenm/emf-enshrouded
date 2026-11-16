# New content: items (what works and what does not)

EMF changes the running game; new items are game **data**. They come from a data patch made with EML, the community's
Enshrouded Mod Loader (github.com/Brabb3l/kfc-parser): a short Lua file that adds resources to the game's data files
before the game reads them. EMF's own tools then work with the new item like with any other (`items`, `give`, `put`,
`take`, mods).

`content\emberbrand\` is a complete example: a new sword, the **Emberbrand** (a Bronze Sword with more damage and some
fire), and a table to place in your base, the **Ember Table** (a Workbench copy that crafts nothing), each with its own
name in every language and a recipe in the crafting menu. Read `src\mod.lua`: it uses EMF's helper (below) and is
about 70 lines.

## The helper: `emf_content.lua`

`content\lib\emf_content.lua` does the steps below for you. Copy it into your mod's `src` folder, next to `mod.lua`, and:

```lua
local content = require("emf_content")
content.add_item{guid = ..., name_guid = ..., debug_name = "Weapon_..._Mine", name = "My Sword", clone_of = <item GUID>,
                 level = 3, edit = function(item) item.damageSetup.dmgMod = 1.5 end}
content.add_recipe{guid = ..., debug_name = "Craft_Mine", output = {<item GUID>, 1}, inputs = {{<item GUID>, 5}},
                   like = "Consumable_Tool_Campfire_PK", menu = {group = "<group GUID>"}}
content.finish()   -- once, at the end: writes the names back
```

- `add_item` copies an item (model, icon, animations, perks), registers it and gives it its name. `level` (a number
  or `{min, max}`) sets its level range: set it for gear players craft. A loot item's range is wide (the Bronze Sword:
  1 to 55) and a craft picks a level inside it (33 for a level-3 character in a test); the game's crafted items have
  one level (its own crafting weapons: 3).
- `add_recipe` adds the recipe **and lists it in the crafting menu**. The menu is not built from the recipe table but
  from a fixed tree in the game's UI data (per workshop: groups, sets, recipes); a recipe that is not in that tree
  never shows. `like` names a vanilla recipe whose station, unlock and duration are copied (the campfire: the
  player's own crafting, known once you have had wood logs); `unlock = "always"` or `unlock = {item = <GUID>}`
  overrides the unlock; `menu = {group = ..., set = 1}` picks the place (`content.menu_groups(0)` lists the own
  crafting menu's groups; Emberbrand uses the weapons group `2632c31c-37fe-48f6-b788-dfa649065edb`). Without `menu`
  the recipe is listed right after the copied one.
- `add_placeable` adds an object players place in their base (below).
- `add_text(id, text)` adds a line to the English text table.
- `add_prompt_words("My words")` adds the words of an E prompt of your own: a Lua mod's
  `emf.offer(w, id, {text = "My words"}, fn)` (EMF API v25) then shows "E My words". Without a content mod that adds
  them, such a prompt shows "E" with an empty label (checked in game: "E Beasts").
- New items and recipes also get their entries in the game's knowledge tables ("has had this item", "has crafted this
  recipe"), copied from the source item's and the copied recipe's. Without them the menu keeps the new recipe marked
  as new after crafting, and a recipe unlocked by your new item never unlocks. The helper checks those tables' layout
  first: if a game update changed it, new items and recipes get no entries (one warning in the content log) and the
  build goes on.
- The UI parts (menu, icons, text) are skipped on the dedicated server for you.

## Placeable objects

```lua
content.add_placeable{guid = <item GUID>, template_guid = <object GUID>, name_guid = ..., debug_name = "Prop_Mine",
                      name = "My Table", description_guid = ..., description = "...",
                      clone_of = "2f04a05f-697d-489b-a32f-76bb3439f5bc",   -- the Workbench's item
                      strip = {content.STATION_PARTS, content.PROMPT_PARTS}}
content.add_recipe{guid = ..., debug_name = "Craft_Prop_Mine", output = {<item GUID>, 1}, inputs = {...}}
```

- A placed object is a game **entity** made from a template (`keen::ecs::TemplateResource`). `add_placeable` copies
  the template of the object `clone_of` places (its model, collider and behaviour) and the placing item, and points
  the new item at the new template. No code is needed to place, save, load or pick it up.
- `strip` removes parts of the copy. `content.STATION_PARTS` makes a Workbench copy stop being a crafting station
  (and a "Workbench nearby" for other recipes, and a place villagers work). `content.PROMPT_PARTS` removes the game's
  E prompt (without it, E would still run the Workbench's crafting animation). A Lua mod can then put its own prompt
  on a placed one with `emf.offer`. A name that is not a component type is an error, so a typo can't leave a second
  Workbench behind; a real part the template lacks is only noted in the content log (the dedicated server's data may
  not carry the game-only parts).
- It is saved only when placed **inside a base**. A Workbench copy can only be placed there (its item needs a build
  zone); copies of objects that can stand anywhere are not saved outside a base. Picking it up gives the item back.
- The save keeps it by `template_guid`: that GUID is fixed forever like the item's.
- **Removing the mod deletes placed objects for good.** When a world loads, the game silently drops every saved object
  whose template it doesn't know (seen in game: a placed Ember Table was gone when the world loaded without the mod),
  and the next autosave (30 s) writes the world without them; reinstalling the mod doesn't bring them back (from the
  game's code). A container's contents go with it, and the placing items
  in inventories are deleted when the game next touches them.

## What a new item needs

All four come from the example, and the game stops or shows nothing if one is missing:

1. **The item** (`keen::ItemInfo`): a copy of an existing item's whole resource under a new GUID, then your changes
   (`itemId`, `objectId`, `name`, `debugName`, damage, perks...). A copy keeps the model, animations and sounds.
2. **Its icon** (`keen::UiTextureResource` under the item's own GUID): copy the source item's. Without it the game
   stops with "corrupted game ui texture" the moment the item shows.
3. **Its registration**: the GUID in `ItemRegistryResource.itemRefs` and the debug name in `dbgNames` (same position).
4. **Its name**: one tag in the keenglish loca table (the game falls back to it for every language), in sorted order.

## Rules that matter

- **Fixed GUIDs, forever.** Saves keep items by ItemId = FNV-1a of the GUID. Pick new GUIDs once for your item (any
  GUID generator) and never change them, or every saved copy disappears.
- **Everyone needs the same patch.** Each game builds its tables from its own files: the host and every player need
  the same content mod (with the same GUIDs). Nothing checks this; from the game's code, a player without it sees
  "[Invalid Item Stack]" (not tested with two players).
- **Removing the mod removes the items and placed objects.** From the game's code: it deletes stacks of unknown items
  when it next touches them (not tested), and drops placed objects of unknown templates at the world's load (gone
  after the next autosave). Take the items out and pick the objects up first, and never remove it while one is
  equipped.
- **Game updates** replace the data files. EML's loader (`dbghelp.dll`) patches again at the next start; if you
  patch offline with `emm.exe`, run it again after every update **before loading a world**, or the item is unknown
  and the game deletes saved copies when it next touches them.
- **A recipe needs its menu entry.** The game's recipe table alone is not enough: the crafting menu lists only the
  recipes in its own tree (the helper's `add_recipe` adds both). The server never checks the unlock when crafting:
  every lock is the menu's.
- **The dedicated server has no render or UI data.** Its files (`enshrouded_server.*`) carry no models, meshes,
  textures, icons (`keen::UiTextureResource` is not even a type there) or text tables, and EML tells your script with
  `loader.is_server`. Make those parts conditional (`if not loader.is_server then ... end`) and keep the rest (items,
  templates, registries, recipes) for both sides, or the server's build stops at the first missing resource or type.
  Emberbrand does this: on a server it adds the item and the recipe without the icon and the name. EMF builds a
  server's content at the server's start in `emf\content\server\` (`docs/engine_parts/content_loader.md`).
- **Say when a part could not be added.** A script that catches its own failure with `pcall` (one creature of
  several, say) must not only `print` it, or EMF reports the build as fine and the player never learns the part is
  missing. Call EML's `warn("<what>: not added: " .. tostring(err))`. Then:
  - EMF's builder (emf_content.dll) lists it in its reply: at most 3 warnings of one mod, each cut at 300 characters.
  - `emf\content.log` keeps every line.
  - Once a world loads, the player sees the first warning on screen and how many more there are. The saves are backed
    up before that world loads, because a missing template removes its placed objects.
  - The console says it again at every start until the content changes.

  The creature packs from `tools/models/make_mod.py` do this.

## Apply it

EML applies patches either at every game start (its `dbghelp.dll` loader) or once, offline:
`emm.exe run --patch -g "<game folder>"` with the mod in `<game folder>\mods\<id>\`. `emm.exe restore` puts the
original `enshrouded.kfc` back but leaves the data EML appended.

For testing, `scripts\content_test.ps1` from the EMF source (not in the release zip) does it with full backups (in
`%LOCALAPPDATA%\EMF\content_test_backup`, out of reach of temp cleaners; `-Backup <folder>` picks another). It
restores the folder it backed up, puts back containers an earlier EML patch had written, and checks every file:

```powershell
scripts\content_test.ps1 -Apply -Mod content\emberbrand -Emm <path to emm.exe>   # backs up, patches
# ... play: items Emberbrand, put Weapon_T1_1H_Sword_Emberbrand 1 ...
scripts\content_test.ps1 -Restore                                              # byte-for-byte back
```

## More loot without new data

`loot_scale` (an EMF mod, off by default) multiplies the world's "Loot Amount" setting: chests, enemy drops and
pickups give more (or fewer) materials, and nothing is saved into the world. Its function is found in the dedicated
server too (not tested on a running server). Switching it off takes effect at the next start; factor 1 brings
chests, enemy drops and pickups back to the world's amounts at once (harvesting, chopping and mining follow it unless
they have their own factor). Set
`factor` in the Mods tab or `emf\config\loot_scale.ini`. It is also a short example of hooking a game function by its
bytes (`hook_pattern`). Plants picked with E, chopped trees and mined rocks never read that setting: for those it
watches the game's `PickupEvent` and gives the difference (`put_items_ex`) or takes it back (`take_items`), with
their own factors (`harvest`, `chopping`, `mining`; 0 = the same as `factor`). Fruit out of a tree's own inventory
follows `factor` (that inventory was filled with the game's setting). See `mods/loot_scale/README.md`.
