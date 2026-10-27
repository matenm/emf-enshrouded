# Your first mod in 10 minutes

You will copy a ready-made Lua mod into the game, see it on screen, change it while the game runs, and give it a
setting of your own that players can change in the Mods tab. No compiler and no programming experience needed: Lua
is a small scripting language, and you only edit two text files.

**You need:**

- Enshrouded with EMF installed (`install.cmd` from the EMF zip; see [README.md](README.md), part 1);
- the `template_lua` folder from the EMF zip;
- a text editor. Notepad works; [VS Code](https://code.visualstudio.com/) colours the code and is easier to read.

Try mods in a spare local world, not in the world you care about.

## 1. Copy the starter (1 minute)

1. Open the game's folder: in Steam, right-click Enshrouded, then **Manage > Browse local files**. Go into `emf\mods`.
2. Copy the `template_lua` folder from the EMF zip into `emf\mods`, and rename the copy to `my_first_mod`.

You now have `emf\mods\my_first_mod\` with two files in it:

- `mod.json` says who the mod is (its id, name and version), what it needs (the `lua` mod that runs Lua scripts) and
  which settings it has.
- `main.lua` is the code.

## 2. See it in the game (2 minutes)

Start the game and load a world. In the bottom left corner you see:

```
Hello from my first mod!  (123, 45, 678)
```

The numbers are where you stand, in metres. Walk and they change. Press **F7**: the line disappears. Press F7 again
and it comes back.

Nothing there? Open EMF's console with the key under Esc and type `mods`. It lists every mod and whether it runs; for
a mod that does not, it says why (a typing mistake in `mod.json` is reported with its line and column).

## 3. Change the code while the game runs (2 minutes)

Leave the game running. Open `main.lua` in your editor and find this line near the top:

```lua
  local text = emf.setting("text")
```

Change it to:

```lua
  local text = "[" .. emf.setting("text") .. "]"
```

(`..` joins two pieces of text.) Save the file. Within a second the game says "Lua mod my_first_mod reloaded", and the
line now reads `[Hello from my first mod!]  (123, 45, 678)`.

Now make a mistake on purpose: delete the last `)` of that line and save. The game shows the error, with the file
and the line where Lua noticed it:

```
Lua mod my_first_mod: my_first_mod/main.lua:12: ')' expected (to close '(' at line 11) near 'local'
```

and pauses the mod. The game itself keeps running: a Lua error never crashes it. Put the `)` back, save, and the mod
runs again.

## 4. Change its settings in the Mods tab (1 minute)

Open the console (the key under Esc) and click the **Mods** tab (or press Ctrl+Tab). Choose **My First Mod**. Its
settings are listed under two headings, "On screen" and "Keys":

| Setting | What it is |
|---|---|
| Show the line | a switch |
| Text | a text field (at most 40 letters) |
| Corner | a list: top right, bottom right, bottom left |
| Colour | a colour picker |
| Decimals of your position | a slider from 0 to 2 |
| Key that shows and hides the line | a key: click it, then press the key you want |

Change any of them: the line follows at once. Switch "Show the line" off and the settings that need it are greyed
out. The same settings are in EMF.exe, the desktop mod manager, and they are saved in `emf\config\my_first_mod.ini`.

All of this comes from `mod.json`. Open it to see how each setting is written, for example the corner:

```json
{"key": "corner", "type": "select", "default": "bottom_left", "label": "Corner", "live": true, "group": "On screen",
 "options": [{"value": "top_right", "label": "Top right"}, {"value": "bottom_right", "label": "Bottom right"},
             {"value": "bottom_left", "label": "Bottom left"}],
 "enabledWhen": {"setting": "show", "equals": true}},
```

`key` is the name the code uses, `label` what players see, `live: true` means the mod applies a change at once,
`group` is the heading it sits under, and `enabledWhen` greys it out unless "show" is on.

## 5. Add a setting of your own (3 minutes)

Give players a switch to hide the position and keep only the text.

1. In `mod.json`, find the line `"settings": [` and add this right after it (mind the comma at the end):

   ```json
       {"key": "show_position", "type": "bool", "default": true, "label": "Show your position", "live": true,
        "group": "On screen"},
   ```

2. In `main.lua`, find:

   ```lua
     if x then
   ```

   and change it to:

   ```lua
     if x and emf.setting("show_position") then
   ```

3. Save both files.

Within a second, with a world loaded, the mod reloads and "Show your position" appears under "On screen" in the Mods
tab. Switch it off:
the numbers go, the text stays. (A typing mistake in `mod.json`, such as a missing comma, pauses the mod with the line
and column of the mistake; fix it and save.)

## Make it yours

- Change `"id"` and `"name"` in `mod.json`, then restart the game (a new id takes effect at the next start). The id
  is 1-48 of `a-z 0-9 _ . -` and must be unique; it names the settings file and the console commands
  (`my_first_mod.hello` becomes `<your id>.hello`).
- Read through `main.lua`: it has an `on_tick` that draws the line, an `on_key` for the key, an
  `on_setting_changed` and a console command, with comments that say what each part does.
- [lua.md](lua.md) lists every `emf.*` function: reading the game's entities, reacting to its events (kills, pickups,
  crafting), giving items, spawning, the radial menu, saving data per world.
- [reference.md](reference.md) lists every `mod.json` field and setting kind; [cookbook.md](cookbook.md) has short
  recipes.
- When you want more than Lua offers (hooks into the engine, your own ECS programs), the C++ template is in the zip's
  `template\` folder; [README.md](README.md) shows how to build it.
