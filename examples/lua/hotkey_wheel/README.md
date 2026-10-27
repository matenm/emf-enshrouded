# Hotkey Wheel (Lua example)

Hold F3 to open a radial wheel, move the mouse to a time of day (dawn, noon, dusk, midnight) and let go: the world's
clock jumps there. Only the host can change the time.

## What it teaches

- **A key bind setting** (`"type": "keybind"`) and `emf.on_key("wheel_key", fn)`: once per press, and a key bound anew
  in the Mods tab works at once. In a world you host `fn` runs on the server world; in someone else's game on your
  client world, so `world:is_server()` tells you whether you are the host.
- **The radial wheel** with `emf.wheel(title, labels, key, fn)`: `"Name (details)"` labels; passing the same key means
  "held while it opens: letting go picks"; `fn(world, choice)` gets 1-4, or `nil` when cancelled.
- **Using a console command from Lua**: Lua has no call to set the time. EMF's `time <hours>` command does it through
  the game's own `setTimeOfDay`, and EMF runs every line written into `<game>\emf\commands.txt` within a second. The mod
  writes `time 6` there (the log's `[cmd]` lines show the answer).

**Careful with that trick.** A line in `commands.txt` runs as the console, with every right the host has (`give`,
`spawn`, `destroy`...). Write only fixed text of your own: never words a player typed, and never from a command marked
`{players = true}`. A line waits until a world you host runs. The hand-off itself is safe: EMF renames the file before
it reads it, and while a mod has it open the rename fails and is tried again a second later.

The default key is F3 because no EMF mod uses it (the developer `sample_mod` has F7 to F12, the companion F6). If the
game or another tool uses it on your PC, bind another key in the Mods tab.

## Install

Copy this folder into `<game>\emf\mods\` (so you have `<game>\emf\mods\hotkey_wheel\main.lua`) and start the game. It
needs EMF's `lua` mod, which the EMF installer puts in and switches on.

## Try it

1. Load a world you host. Hold F3: the wheel opens in the middle of the screen. Move the mouse to "Dusk" and let go:
   a note says "Time of day: Dusk", and within a second the sky turns to evening. `<game>\emf\emf_core.log` has
   `[hotkey_wheel] wrote "time 18" into emf\commands.txt` and EMF's `[cmd] time of day is now 18.00 h` answer.
2. Tap F3 instead: the wheel stays open and a click picks an entry. Esc or a right click closes it without a change.
3. Bind another key in the Mods tab (the key under Esc, then Ctrl+Tab, "Hotkey Wheel"): it opens the wheel at once.
4. Join someone else's game: F3 only says "Only the host can change the time of day".
5. Open `main.lua`, add `{label = "Evening (20:00)", name = "Evening", hours = 20},` to `TIMES` and save: the wheel has
   a fifth entry.
