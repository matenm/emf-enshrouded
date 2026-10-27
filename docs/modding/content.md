# New content: items (what works and what does not)

EMF changes the running game; new items are game **data**. They come from a data patch made with EML, the community's
Enshrouded Mod Loader (github.com/Brabb3l/kfc-parser): a short Lua file that adds resources to the game's data files
before the game reads them. EMF's own tools then work with the new item like with any other (`items`, `give`, `put`,
`take`, mods).

`content\emberbrand\` is a complete example: a new sword, the **Emberbrand** (a Bronze Sword with more damage and some
fire), with its own name in every language. Read `src\mod.lua`; it is about 90 lines with comments.

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
- **Removing the mod removes the items.** From the game's code: it deletes stacks of unknown items when it next
  touches them (not tested). Take the items out first, and never remove it while one is equipped.
- **Game updates** replace the data files. EML's loader (`dbghelp.dll`) patches again at the next start; if you
  patch offline with `emm.exe`, run it again after every update **before loading a world**, or the item is unknown
  and the game deletes saved copies when it next touches them.
- **Recipes do not work.** The example also adds a recipe (a copy of the campfire's, 5 wood logs), and it is in
  the game's recipe table, but the crafting menu does not list it: the menu seems to show recipes that knowledge
  unlocks, not every recipe whose requirement passes. Hand items out with EMF (`give`, `put`) or your own mod.
  (`docs/engine_parts/re_content.md` has the details.)

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
server too (not tested on a running server). Switching it off takes effect at the next start; factor 1 is "off"
at once. Set
`factor` in the Mods tab or `emf\config\loot_scale.ini`. It is also a short example of hooking a game function by its
bytes (`hook_pattern`).
