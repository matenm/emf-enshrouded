# Settings Tour (Lua example)

One setting of every kind in the console's Mods tab, shown on screen while you change them.

## What it teaches

- **Every kind of setting** in `mod.json`: a switch (`bool`), number boxes (`int`, `float`), sliders (whole numbers
  and decimals), two kinds of list (`choice` is read as its number, `select` as its value), a colour, a text with
  `maxLength` and a key bind, under `group` headings, some greyed out with `enabledWhen`.
- **Live or next start**: `"live": true` means the mod applies a change at once. The greeting has no `live`, so the
  Mods tab says "next start" for it: the mod reads it once when it loads.
- **Reading settings** with `emf.setting(key)` each time you need one, and **hearing changes** with
  `emf.on_setting_changed`.
- **Changing a setting from code** with `emf.set_setting(key, value)`: the same checks as the Mods tab, `false` when a
  value does not fit.
- **A key bind** (`mood_key`) with `emf.on_key`: a key chosen anew in the Mods tab works at once.

## Install

Copy this folder into `<game>\emf\mods\` (so you have `<game>\emf\mods\settings_tour\main.lua`) and start the game.
It needs EMF's `lua` mod, which the EMF installer puts in and switches on.

## Try it

1. Load a world: three lines show in the bottom left corner, and a note says "Welcome to the settings tour".
2. Open the console (the key under Esc), then the Mods tab (Ctrl+Tab), and pick "Settings Tour". Move the sliders,
   pick another corner or colour, type a new text: the lines follow at once, and `<game>\emf\emf_core.log` gets a line
   per change (`[settings_tour] count is now 7`).
3. "A whole-number slider" is greyed out until the mood is "wild". Switch "Show the values on screen" off: the corner,
   colour and text settings grey out and the lines go.
4. Press F4 (outside the console): the mood moves on (calm, busy, wild). Bind another key in the Mods tab and it
   works at once. (F4 because no EMF mod uses it; the developer `sample_mod` has F7 to F12.)
5. Type `settings_tour.set count 12` in the console: `count is now 12`. `settings_tour.set wildness 99` is refused
   (the slider goes from 1 to 10), and so is a label longer than 12 characters.
6. Change the greeting: nothing happens until the mod loads again (save `main.lua`, or restart the game).

The settings are saved in `<game>\emf\config\settings_tour.ini`. `docs/modding/reference.md` ("Settings") lists every
field a setting can have.
