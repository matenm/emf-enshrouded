# Server Herald (`herald`)

For people who run a dedicated server (or host a game for friends): the server talks to its players. A message of the
day when someone joins, a line in the chat when players join, leave or fall, announcements in turn, a countdown before
a restart, a chat log, playtime per player, and an opt-in Discord webhook. It runs inside the server: players who join
need nothing, not even EMF.

It answers the server owners' requests on the official feature board that vanilla 1.0 still leaves open: broadcast /
MOTD, join announcements, "send a message from the CLI to the game" before a restart, chat in a log, manual save.

**Off by default**, because it changes what players see. Switch it on in EMF.exe's Mods tab for the server folder, or
set `enabled=1` in `emf\config\herald.ini` (section `[herald]`), then restart the server. Switched off while the server
runs, it stops at once: no more lines, and its commands answer that it is switched off.

## What players see

Names are the ones the game's chat shows (the player's platform name, e.g. their Steam name; EMF API v19
`player_name`); when EMF cannot read one, a player is `player #<entity>`.

All of these are chat lines; the game shows them under the reader's own name, the way EMF's command replies appear
(the game shows no chat line without a speaker). Every line starts with the prefix (the setting **In front of every
line**, `[Server]` by default).

- **Message of the day:** a few seconds after a player is in the world, only they get the MOTD. `{name}` and `{online}`
  in it are filled in: `Welcome {name}! {online} online. Type /help.`
- **Joins and leaves:** `[Server] Ember joined (3 online)`, `[Server] Ember left (2 online)`. A player counts as joined
  once their game says it is ready (not while it is still loading).
- **Deaths** (off by default): `[Server] Ember fell`.
- **Announcements:** the lines of `emf\config\herald\announcements.txt`, one every N minutes (the setting **An
  announcement every**), in turn, only while someone is online (empty lines and lines starting with `#` are skipped; `herald.reload` reads the
  file again).
- **Restart countdown:** `herald.restart 10 nightly` says `[Server] Server restart in 10 minutes: nightly` at once, then
  at 5, 2 and 1 minutes and 30 seconds left, then `Server restarting now: nightly` and (with **Save when a restart
  countdown ends**) saves the world. `herald.cancel` stops it and says so.

**Herald does not stop the server.** Stop it outside EMF when the countdown ends: EMF.exe's Stop button (a clean stop
that saves), your host panel, or a scheduled task. A scheduled task can start the countdown too: it writes one line,
`herald.restart 10 nightly`, into `emf\commands.txt` of the server folder (EMF reads that file every second), and runs
its stop ten minutes later.

## Commands

| Command | Who | What |
|---|---|---|
| `/players` | **any player** | who is online, numbered `#1`, `#2`... (lowest entity first), and how long they have been on |
| `/playtime` | **any player** | your playtime on this world and your rank |
| `/motd` | **any player** | the message of the day again |
| `herald.say <text>` | admins | a line for everyone |
| `herald.tell <name\|#k> <text>` | admins | a line for one player; `#k` as `/players` numbers them, or a name (names with spaces work; a unique start of a name too) |
| `herald.restart <minutes> [reason]` | admins | the countdown (`0.5` = 30 s) |
| `herald.cancel` | admins | stops the countdown |
| `herald.save` | admins | saves the world now (the log says `world saved` once it is written) |
| `herald.seen <name>` | admins | when a player was last on this world, first seen, playtime and account hash |
| `herald.reload` | admins | reads `announcements.txt` and `webhook.txt` again |

Admins are the console, `emf\commands.txt`, and the accounts in `[chat] admins` of `emf\emf_core.ini`
([docs/modding/server.md](../../docs/modding/server.md)). "Any player" works from the game's chat (`/players`).

## Settings (Mods tab, EMF.exe, or `set herald <key> <value>`)

| Setting | Default | What it does |
|---|---|---|
| Message of the day (`motd`) | empty (none) | up to 256 characters, `{name}` and `{online}` filled in |
| After joining (`motd_delay_seconds`) | 5 s | 0-60 |
| Say who joins / leaves / falls (`announce_joins`, `announce_leaves`, `announce_deaths`) | on / on / off | |
| In front of every line (`prefix`) | `[Server]` | up to 24 characters; empty = nothing |
| An announcement every (`announce_every_minutes`) | 30 | 0-120 minutes, 0 = off |
| Save when a restart countdown ends (`restart_save`) | on | |
| Chat log (`chat_log`) | on | |
| Count playtime (`playtime`) | on | |
| Discord webhook (`webhook`) | **off** | see below |
| What goes to Discord (`webhook_events`) | joins, leaves and restarts | also deaths; also players' chat and `herald.say` |
| Allow other https hosts (`webhook_any_host`) | off | |

Every setting applies at once.

## Files

| File | What |
|---|---|
| `emf\config\herald\announcements.txt` | your announcements, one per line (you make it) |
| `emf\config\herald\webhook.txt` | the Discord webhook URL, on the first line (you make it) |
| `emf\logs\chat-YYYY-MM-DD.log` | the chat log, one file per day (local time): `14:05:12 0x<account> Ember: hello`, joins, leaves, deaths and the herald's own lines as `14:05:00 -- ...` |
| `emf\config\herald\<world slot>.txt` | playtime per account for that world (first seen, last seen, last name) |

The chat log holds what players wrote, with their account hashes: treat it as private. Its lines are written together
once per server step (at once when 64 wait), so a flood of chat lines does not slow the server; the last ones are
written when the server closes normally.

## The Discord webhook

Off unless you switch on **Discord webhook** and put a webhook URL (Discord: channel settings, Integrations, Webhooks,
Copy Webhook URL) into `emf\config\herald\webhook.txt`.

- The URL is a secret (anyone who has it can post to your channel): Herald never writes it to the log, the console or
  a reply, and `settings` does not show it (it is not a setting).
- Only `https://discord.com/api/webhooks/...` or `https://discordapp.com/api/webhooks/...` (also `ptb.` and `canary.`)
  are used; anything else is refused with a log line that says why (without the URL). **Allow other https hosts**
  lifts the host rule for other services that take Discord-style posts; plain `http://` is never used, and neither are
  URLs with a user name (`https://x@host/`). Redirects are not followed.
- Messages go from a thread of their own, at most one post every 2 seconds, with the lines of those seconds batched
  together. If Discord says "too many requests", Herald waits 10 seconds. At most 200 lines wait; older ones are dropped
  and the next post says how many. Posts carry no pings: `@everyone` in a chat line stays text.
- Switching the webhook or Server Herald off ends that thread within a moment; switched on again, a new one starts.
  When the server closes, Windows ends it (EMF has no "closing" call for mods): a post under way is cut off.
- The log has one line for the first post and at most one line a minute for refused posts (the HTTP status only).

## In a game you host

It works the same in a game you host, with `players=1` in the `[chat]` section of your game's `emf\emf_core.ini`
(that is what lets the server side of your game read and send players' chat lines). Lines meant for you appear as notes
at the top of your screen instead (your own game does not show the server's chat lines to you). Without `players=1`
only you see the herald's lines and the chat log has no chat lines.

## Good to know

- Names are the names the game knows for the players; when EMF cannot read one, the player is `player #<entity>`.
- `/players` numbers are only valid until someone joins or leaves.
- Playtime counts while a player is in the world, per world (a world save slot), kept across restarts; it is saved
  every minute and when someone leaves.
- The log (`emf\emf_core.log`, lines `[herald]`) has every join and leave with the account hash.
