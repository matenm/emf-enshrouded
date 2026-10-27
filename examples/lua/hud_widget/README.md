# HUD Widget (Lua example)

A tidy corner widget:

```
X 412  Y 85  Z -1290
Day   [#######.....] 13:30
Dusk in 4h 30m
```

## What it teaches

- **Lines on screen** with `emf.hud(id, text, corner, colour)`: one id per line (lines of one corner are listed by id),
  `""` removes a line. The corner is a `select` setting's value, the colour a `color` setting's `"#rrggbb"`.
- **Sending only what changed**: the widget remembers each line and calls `emf.hud` only when the text changes, not
  60 times a second.
- **The time of day** with `emf.time_of_day(world)` (Lua 1.4): hours as a number (`19.5` is 19:30), or `nil` when the
  world's clock is not known (after a game update, say). The widget says so instead of failing.
- **Drawing on the client world**: every player's own game has one, so the widget works for the host and for guests.

## Install

Copy this folder into `<game>\emf\mods\` (so you have `<game>\emf\mods\hud_widget\main.lua`) and start the game. It
needs EMF's `lua` mod, which the EMF installer puts in and switches on. Each player who wants the widget installs it in
their own game.

## Try it

1. Load a world: three lines in the top right corner, below the quest tracker. Walk: the position follows.
2. Watch the clock: the bar fills through the day (6:00 to 18:00) and through the night, and the last line counts down
   to dusk or dawn. The clock and the countdown are the game's time, which runs much faster than real time: "4h 30m"
   is gone in minutes. `time 17.9` in the console (the host) shows the switch to night a few seconds later.
3. Open the Mods tab (the key under Esc, then Ctrl+Tab), pick "HUD Widget" and change the corner and the colour: the
   widget moves at once. Switch "Show where you are" off: the first line goes.
4. Open `main.lua`, change `CELLS = 12` to `CELLS = 20` and save: the bar is wider a second later.
