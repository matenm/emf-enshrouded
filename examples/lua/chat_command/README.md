# Chat Commands (Lua example)

Two commands any player may type into the game's chat: where they stand, and how far the nearest flame altar is.

## What it teaches

- **Commands for every player**: `emf.command(name, help, fn, {players = true})`. Without the mark only the admins
  (`[chat] admins` in `emf\emf_core.ini`) may run a command from the chat. Mark a command only when it is harmless from
  anyone: it reads, or it acts on the player who typed it.
- **Answering the sender**: `emf.sender()` says who typed the command: `chat` (true for another player in the chat;
  the console and the host's own chat say false), `entity`, `account` and `admin`. "You" in a command means them, not
  the host.
- **More for admins, failing closed**: `home` gives only a distance; `home all` lists where every altar is, other
  players' bases too, so only admins get it. The check is `who and who.admin`: when the sender is unknown, no list.
- **The reply limits**: the chat shows 10 lines of one command's answer (an 11th becomes "... (more in the server
  log)"), each at most 1 KB, and a player who is no admin may send 5 commands in 10 seconds. `home all` stays within
  10 lines.
- **Finding bases**: each flame altar has an entity with a `BaseIdComponent`, as the Night Raids mod finds them. The
  component does not say whose base it is, so the nearest altar may be anyone's. Finding them walks every entity, so
  the mod keeps the list for 5 seconds instead of walking the world on every command.

## Install

Copy this folder into `<game>\emf\mods\` (or the dedicated server's `emf\mods\`) and start the game or the server. It
needs EMF's `lua` mod, which the EMF installer puts in and switches on. Only the game that runs the world (the host,
or the dedicated server) needs it.

## Try it

1. Load a world you host and open the console (the key under Esc). Type `chat_command.where`: `You are at ...` and
   `asked by the host (the console or their own chat)`. Type `chat_command.home`: how far the nearest flame altar is,
   and how much higher or lower.
2. In the game's chat, `/chat_command.where` answers the same (the host's own chat runs every command).
3. With a friend: add `players=1` to the `[chat]` section of your `emf\emf_core.ini` (or use a dedicated server, see
   `docs/modding/server.md`). Your friend types `/chat_command.where` and `/chat_command.home`: the answers are about
   them, and only they see them. `/chat_command.home all` tells them only admins may; put their account in
   `[chat] admins` (the log shows it) and they get the list.
4. `help` lists the commands; for a player who is no admin it lists only those marked for players.

Lua commands always start with the mod's id. To type `/w.where`, copy the folder as `w` and change `"id"` in
`mod.json` to `"w"`. `docs/modding/server.md` explains who may run what.
