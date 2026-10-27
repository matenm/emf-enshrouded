# My First Mod: EMF's Lua starter

A complete Lua mod: a line of text on screen, a key that shows and hides it, and one setting of each kind in the
Mods tab (a switch, a text, a list, a colour, a slider and a key).

1. Copy this folder into `<game>\emf\mods\` and rename it `my_first_mod`.
2. Start the game and load a world: the text shows in the bottom left corner; F7 hides and shows it.
3. Open `main.lua` in a text editor, change something and save: the game reloads the mod within a second.

`docs/modding/first_mod.md` walks through it step by step; `docs/modding/lua.md` lists every `emf.*` function.
To make it your own mod, change `"id"` and `"name"` in `mod.json` (the id is 1-48 of a-z 0-9 _ . -).
