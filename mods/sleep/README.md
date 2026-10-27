# Sleep Through the Day (`sleep`)

Lie down in a bed by day and the day passes 60 times faster until the evening (17:30 by default); then the clock
runs normally again. Vanilla Enshrouded only skips the night (127 votes on the official board ask for the day too:
"Skip daytime / sleep through the day").

It runs where the world runs, so on the host or a dedicated server it works for every player, also those who joined
without EMF.

## Switch it on

Sleep Through the Day is installed **switched off**: it changes the world's clock for every player. Switch it on in
the Mods tab (the key under Esc) or in EMF.exe, then restart the game (or the server).

## How it works

- **Who has to sleep** (`who`):
  - **Everyone** (the default): every player in a bed. Alone, that is just you.
  - **Each sleeper speeds it up**: speed × sleepers / players, as the game's own night skip (2 of 4 in bed: 30x).
  - **A share of the players**: at least **Share in bed** % (50 by default) in bed gives the full speed. This one also
    brings the **night** to full speed when that share sleeps (the game's own night skip alone would run at
    60 × sleepers / players).
- The day races until **Skip to** (17:30 by default, 12:00-18:00), then the clock runs normally. A note says when it
  starts and when the evening comes (on the host's screen; a server writes it to its log).
- Night falls at 18:00. Stay in bed past it and the game's own night skip carries on through the night, as in vanilla.
  Get up in the evening to play the night.
- A sleep that reaches the evening, or that began at night or after the wake hour, never skips the next day: someone
  who stays in bed is not carried through day after day. Get up, lie down again by day, and it skips again. Someone
  still in bed from the night does not block the others: when another player lies down by day, the skip is armed
  again.
- `/sleep.status` (every player, also from the game's chat): who is asleep and what happens, e.g. "1 of 2 asleep: not
  enough to skip the day (everyone must be in bed)".
- `sleep.pretend [on|off]` (host, for tests): every player counts as asleep, until `sleep.pretend off` or the next
  world load, so you can watch the day race without a bed.

## Settings (Mods tab, EMF.exe, or `set sleep <key> <value>`)

| Setting | Default | What it does |
|---|---|---|
| Skip to (hour) (`wake_hour`) | 17.5 | 12-18, in half hours |
| Who has to sleep (`who`) | everyone | `all`, `share`, `percent` |
| Share in bed (%) (`percent`) | 50 | 10-100, with `who = percent` |
| How much faster (`speed`) | 60 | 10-60 (60 = the game's night skip) |
| Notes on screen (`notes`) | on | |

## How it is done (for the curious)

The game's clock (`g38_daytime::Daytime`) keeps an anchor (game time at an ECS time) and a speed. Its own night skip
(the program `server_nighttime_skip`) counts the players whose Actor state has flag 11 (asleep) and sets the speed to
60 × sleeping / players at night, and back to 1 by day, every step it differs. So this mod leaves the speed alone and
adds the extra game time to the anchor each step instead, with the clock's change counter raised (at most every
250 ms) so the players' clocks follow, as they do for the game's own changes. It counts sleepers exactly as that program does
(`sleep_glue.h`), and refuses to touch a clock whose numbers look wrong (a game update) or a game build EMF does not
know. The maths and the rules are in `sleep_logic.h`, unit-tested in `tests/test_sleep.cpp`.

## Good to know

- Weather follows the game time, so rain comes and goes faster while the day races, as during the game's night skip.
- In someone else's game it does nothing (that game's host decides).
