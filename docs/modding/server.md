# EMF on a dedicated server

EMF runs inside the official Enshrouded Dedicated Server (`enshrouded_server.exe`, Steam app 2278520) the same way it
runs in the game: the server loads EMF's `dinput8.dll`, EMF loads its mods. Server-side mods change the world for every
player on it, whether or not they have EMF themselves. Players with the right to do so can type EMF commands into the
game's chat.

## Install

1. Stop the server.
2. Run `install.cmd` from the EMF zip with the server's folder (the one with `enshrouded_server.exe`):
   `install.cmd "D:\EnshroudedServer"`, or drag the folder onto `install.cmd`.
3. Start the server. Its window shows EMF's lines (`[EMF] ...`) next to its own; the full log is
   `emf\emf_core.log` in the server folder.

`uninstall.cmd "D:\EnshroudedServer"` removes it again. The server must use the same game version EMF knows
(`tools\emf_sigcheck.exe "D:\EnshroudedServer\enshrouded_server.exe"` checks it without starting anything).

**With EMF.exe:** the folder list at the top of EMF.exe has "Add a server folder..." (the Steam server is found by
itself; a SteamCMD folder is chosen by hand). For a server folder EMF.exe shows its mods with where each has to run,
and a Server tab: the server's name, slots, port and user groups (passwords only as "set"), the chat settings below,
Start and Stop (a clean stop, as Ctrl+C in its window: the server saves first; closing its window is not the same,
Windows ends it about 5 s later and a longer save is cut), "Install EMF" with this installer's own rules, and the server's
log and `emf_core.log` as they grow (EMF's docs/app.md, "Servers", has the details).

## What runs on a server

- Mods that change the world (stacks, villagers, loot, mod programs, events, Lua mods) work as in a game you host.
- The server builds its world within a second of starting; EMF holds its start (about a second) until the mods are
  loaded, so their programs and events are part of that world. `emf\emf_loader.log` says how long.
- So on the server, mods load before the server exe's own start-up code runs (EMF's loader holds the exe's entry
  point until EMF has started): in `emf_mod_load` (and a Lua mod's main chunk) the game's globals are not set up yet.
  Register hooks, programs, events and callbacks there, and read game data only from world callbacks (`on_tick`...).
- The server has no screen: the console overlay, HUD lines, the wheel and notes do nothing there (notes go to the log).
  A mod written for the screen loads but shows nothing.
- `emf.local_player()` is 0: there is no local player. Commands act on the player who typed them (below).
- Commands from the server owner: write them into `emf\commands.txt` (one per line); EMF runs them and writes the
  replies to the log.

## Commands from players' chat

A player types `/help`, `/give Material_T1_Wood_Logs 20` (item names as `/items wood` lists them) or any EMF command
(`/<mod>.<command>` for a mod's own) into the game's chat. EMF runs it, nobody else sees the line, and the replies
come back as chat lines that only that player sees, under their own name: `Ember: [EMF] gave 20 x
Material_T1_Wood_Logs to player 1` (the game shows no chat line without a speaker). At most 10 lines per command; the
server log has the rest. "You" in a command (`give`, `pos`, `spawn`) is the player who typed it: EMF finds their
player by the account hash the chat line carries. If a command answers "player position unknown" or "player
unknown", EMF could not tell which player you are; commands with an explicit target still work. The results of `put`
and `take` come one server step later and reach the chat the same way. (Tested on a dedicated server, except the chat
results of `put` and `take`.)

A player who has EMF installed in their own game can use the server's commands too: in a game someone else hosts,
EMF's chat box leaves `/command` lines alone and they go to the server.

Who may: nobody until you say so, in `emf\emf_core.ini` of the server folder:

```ini
[chat]
; account hashes of the players who may run EMF commands, comma-separated
admins=0x0123456789abcdef, 0xfedcba9876543210
; everyone=1 lets every player run them (only on a server for friends)
everyone=0
```

To find your account hash, type an EMF command for admins (`/pos`) into the chat: EMF answers that you may not, and
the server log says `chat command refused from account 0x...`. Put that value into `admins`, then restart the server.
Or write `chat.players` into `emf\commands.txt` while you are on the server: the log lists every player with their
account hash (EMF API v19).

**Commands for every player** (EMF API v17): mods can mark commands that any player may run, without being in
`admins`: `/help` (it lists only what that player may run), `/raids.status`, `/loot.toggle`, `/loot.status`,
`/champion.where`, `/champion.stats`, `/life_skills.skills`, `/qs`, `/quick_stack`, `/qs.chests`, `/bounty.board`,
`/bounty.claim`, `/bounty.reroll`, `/bounty.top`, `/grave.where`, `/grave.claim`, `/sleep.status`, and with
Hand-over on `/handover`, `/handover.who`, `/handover.to`, `/handover.block`, with Server Herald on `/players`,
`/playtime`, `/motd`, with Waypoint Teleports on `/unstuck`, `/home`, with Bulk Craft on `/craft`, `/craft.stop`,
`/craft.list`, with Beastmaster on `/beastmaster` (`list`, `tame`, `feed`, `call`, `dismiss`, `attack`), with Necromancer
on `/necromancer` (`raise`, `spirits`, `attack`, `gather`, `dismiss`, `list`). Such commands only
read or act on the player who typed them. `everyone=1` is not needed for those; it opens every command (`give`, `spawn`...) to everyone.
A player who is no admin may send at most 5 commands in 10 seconds: the next one is answered "too many commands", the
rest of those 10 seconds are dropped without a reply (admins are not limited). At most 256 players' lines wait for the
server's next step; more are answered "too many commands are waiting". In the chat `/help` comes in pages of 8
(`/help 2`, ...).

To take a command for players away from players who are no admins, list it in `deny` (commas or spaces):

```ini
[chat]
deny=loot.toggle, champion.where
```

**Defaults of the shipped mods on a server:** Night Raids is on, but every world starts without raids (`raids.world
on`); Loot Magnet, Wandering Champion, Bounty Board, Grave Keeper, Sleep Through the Day, Server Herald, Hand-over,
Waypoint Teleports, Bulk Craft, Beastmaster and Necromancer are installed switched off: set `enabled=1` in
`emf\config\<id>.ini` (`loot_magnet`, `champion`, `bounty`, `grave`, `sleep`, `herald`, `handover`, `teleports`,
`bulk_craft`, `beastmaster`, `necromancer`; section `[<id>]`), or switch
them on in EMF.exe, then restart the server. Quick Stack's
chest machines work on a server too: an admin marks chests with `/qs.mark sort` (the nearest chest within 3 m); its
recyclers stay off until `recycler=1`. A
mod marks a command with `register_command_ex` and `EMF_COMMAND_PLAYERS` (C++: `emf::add_command(name, help, f,
EMF_COMMAND_PLAYERS)`; Lua: `emf.command(name, help, fn, {players = true})`; [reference.md](reference.md), v17).
A Steam64 id (17 digits) is not an account hash: an entry that is not one is skipped, with a warning in the log.
EMF.exe's Server tab edits the same `[chat]` keys (admins one per line, the file's other keys and comments kept) and
lists the accounts the log says were refused, each with "Add as admin".

**Waypoint Teleports on a server:** every player may type `/unstuck` and `/home` (a 3 s countdown standing still,
then the teleport; cooldowns, no `/home` from inside the shroud). An admin moves players with `tp #k #m` (player k to
player m: this one works from the server's console, as `tp.pos #k x y z [ground]` and `tp.back #k` do), `bring #k` (to
the admin in the chat) and `tp.list` (who is `#k`). The server moves the player (EMF API v20 `teleport`, the game's own
teleport); the player's game needs nothing and snaps to the new place. The countdown, cancels and results reach each
player as chat lines only they see (API v19 `chat_tell`); admins may name players by name (`tp Ember`), or by `#k` when
two share a name.
Details: [mods/teleports/README.md](../../mods/teleports/README.md).

Text chat must be on in the server's `enshrouded_server.json` (`"enableTextChat": true`).

### Chat lines from mods (EMF API v19)

Mods can write into the game's chat and read it, where players' chat is on (a dedicated server; a game you host with
`[chat] players=1`):

- **A line for one player** (`chat_tell`, Lua `emf.tell`) reaches only that player, outside any command, and shows under
  their own name (`Ember: [Server] Welcome!`), as command replies do: the game shows no chat line without a speaker. In
  a game you host, a line for the host becomes a note at the top of the host's screen.
- **A line for everyone** (`chat_broadcast`, Lua `emf.broadcast`): each player sees it under their own name.
- **Players' lines** (`on_chat`, Lua `emf.on_chat`): every chat line a player sends that is no EMF command reaches the
  mods that ask, read-only (a chat log, a Discord mirror).
- **A save now** (`request_save`, Lua `emf.request_save`): the world save the game makes every 30 s, at once.

At most 10 lines per call and 30 lines per mod in 10 seconds (each Lua mod 30); the server log has every line EMF
sends. The shipped
**Server Herald** (`mods/herald`, off by default) uses all of them: a message of the day, join and leave lines,
announcements, a restart countdown (`herald.restart 10 nightly` in `emf\commands.txt`), a chat log per day in
`emf\logs\`, playtime, and an opt-in Discord webhook. **Hand-over** (`mods/handover`, Lua, off by default) lets
players give items to each other with `/handover`; a player who does not want any types `/handover.block`.

### In a game you host

Your own `/commands` already work from your chat (the client side, `[overlay] chat=1`). To let your friends run
commands in a game you host, add `players=1` to the `[chat]` section of your game's `emf\emf_core.ini`; the same
`admins`/`everyone` rules apply, and commands for every player work for every player.

Players allowed to run commands can also run `profile load <name>` (mod profiles, [docs/app.md](../app.md#profiles)):
it switches the mods of this folder for the next start, as `set <mod> enabled 0` already can. With `everyone=1` that
is every player.

## Keeping a test server private

The server registers in Steam's public server list when it starts; there is no unlisted mode. For a private server:

- Do not forward its port (UDP 15637) on your router; when Windows asks about the firewall on the first start, allow
  nothing (untick both boxes). Windows may already have rules that allow `enshrouded_server.exe` (from an earlier
  start): see "Allow an app through Windows Firewall".
- Give every user group in `enshrouded_server.json` a long password: a group with an empty password lets anyone in.
- Join it from the same PC: Play, Join, "Show dedicated server", "Search by IPv4" `127.0.0.1:15637`.
