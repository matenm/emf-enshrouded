# Loot Ticker (Lua example)

Your last 5 pickups on screen, newest first, each fading out after a few seconds:

```
+30 Wood Logs
+2 Fishing Bait
```

## What it teaches

- **Listening to a game event**: `emf.on_event("PickupEvent", fn)` at the top of `main.lua`; `fn(world, e)` gets the
  event's fields (`e.itemId`, `e.itemCount`, `e.pickedUpByEntityId`...). The events come from the world you host, for
  every player, so the mod keeps only yours (`emf.local_player()`).
- **Naming items**: `emf.item_info(id).name` is the debug name (`Material_T1_Wood_Logs`); a few `gsub`s make words of it.
- **Fading HUD lines**: a colour with alpha (`"#rrggbbaa"`) fades a line; the alpha moves in steps, so a line is sent
  again a few times while it fades, not 60 times a second.
- **Time in steps**: the client world steps 60 times a second; counting them is the mod's clock.

## Install

Copy this folder into `<game>\emf\mods\` (so you have `<game>\emf\mods\loot_ticker\main.lua`) and start the game. It
needs EMF's `lua` mod, which the EMF installer puts in and switches on. It shows the pickups in a world you host (in
someone else's game the events stay on their side).

## Try it

1. Load a world you host and pick something up (chop a tree, gather plants): `+N <item>` shows in the bottom right.
   More of the same within 3 seconds adds up on one line.
2. Pick up different things: up to 5 lines, the newest on top. Each fades over its last 2 seconds and goes after 8.
3. In the Mods tab (the key under Esc, then Ctrl+Tab, "Loot Ticker") make the lines stay 20 seconds, or move them to
   another corner or colour.
4. Open `main.lua` and change `LINES = 5` to `LINES = 8`: eight lines after you save.
