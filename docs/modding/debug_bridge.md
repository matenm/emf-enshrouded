# The debug bridge

The debug bridge is a local text channel into the running game. A script can run console commands and read their
output, read the game's state as JSON, wait for a world or a log line, take a screenshot, press a mod's hotkey and close
the game. It is for testing: your own mod while you write it, or EMF itself. It is off unless you switch it on.

## Switch it on

1. Close the game.
2. Add these lines to `emf\emf_core.ini` in the game folder (create the file if it is not there):

   ```ini
   [debug]
   bridge=1
   ```

   From the repository, `scripts\install_emf.ps1 -Bridge` writes them. Without `-Bridge` it sets `bridge=0` again.
3. Start the game.

With the bridge on:

- `emf\emf_core.log` says so once: `debug bridge: on, pipe \\.\pipe\emf-bridge-<pid> (emf\bridge.txt)...`.
- The in-game console shows "debug bridge on" at the right end of its tab bar.
- `emf\bridge.txt` names the pipe and the game's process id while the game runs. EMF removes it when the game ends.
- The log notes the first client that connects. `bridge_verbose=1` in the same `[debug]` section logs every client's
  connect and leave.

To switch it off, set `bridge=0` (or remove the lines) and restart the game. Then EMF creates no pipe and starts no
thread. Only `bridge=1` switches it on. The release zips never do.

It works on the dedicated server too (`enshrouded_server.exe`), without the screenshot, the mod keys and `quit`.

## Who can connect

- Only programs that run as your Windows user, at the game's integrity level or higher, and SYSTEM. If you run the
  game as administrator, run the client as administrator too.
- EMF checks each client's process again when it connects: the same user, and an integrity level at least the game's.
  Other clients are closed at once (the log says why, at most once a minute).
- Not other Windows users, not another computer: the pipe refuses remote clients and denies network logons.
- Not a web page: browsers cannot open named pipes.
- If the pipe's name is ever free and another program takes it, EMF stops the bridge instead of sharing the name.
- At most 8 clients at once. A ninth is refused until one leaves (the clients below wait 30 s for that).

A client can do what you can do in the console: run commands that exist, and read. It cannot run other code. (A
program of your user can already write `emf\commands.txt`; the bridge adds the replies.)

The clients check the game too. They only use a pipe called `\\.\pipe\emf-bridge-<pid>` on this computer, where the
pid is the one `bridge.txt` names and belongs to a running `enshrouded*.exe`, and the pipe must be served by that very
process. They connect at "identification" level, so the game can tell who connects but can never act as you.

## The clients

Both are in the repository (`scripts\emf_bridge.ps1`, `tools\bridge\emf_bridge.py`). The release zip installs them as
`emf\tools\` in the game folder.

They find the game through the folder `-Game` names (`--game`), `-GamePid` (`--pid`), their own place (when they sit in
`<game>\emf\tools\`), the running `enshrouded.exe` (or `enshrouded_server.exe`; when several run, only those that
serve a bridge count), or the Steam library. Exit codes: 0
done, 1 the bridge said no (or a wait timed out, or a console command did not run), 2 no connection. `-Json`
(`--json`) prints the game's reply as it came, for any command. A reply must come within the command's own time (its
`timeout_ms`, settle or hold) plus 15 s; a game that does not answer by then (crashed, paused in a debugger) ends the
client with exit 2 instead of a wait for ever. For `load_world` the command's own time is its `timeout_ms` (default
180 s) plus the game's own 10 s backstop: the reply comes when the local player is in the world, about 12 s on a fast
PC.

### PowerShell: `emf_bridge.ps1`

| Command | What it does |
|---|---|
| `emf_bridge.ps1 hello` | EMF's version, the API version, the game build, client or server, the pid |
| `emf_bridge.ps1 console "near 10"` | runs a console command and prints its output (`-TimeoutMs`, default 10 s; `-SettleMs`, default 300) |
| `emf_bridge.ps1 state` | a one-line summary, then the whole state as JSON |
| `emf_bridge.ps1 mods` | the mods and what became of them |
| `emf_bridge.ps1 wait world 120` | waits up to 120 s for a world to run (`wait player` for the local player) |
| `emf_bridge.ps1 wait log "[life_skills]" 60` | waits for a new log line that contains the text (`-Since <n>`: from that line) |
| `emf_bridge.ps1 log` | the last 100 log lines (`-Since <n>`, `-Max <n>`); `-Follow` keeps printing new ones |
| `emf_bridge.ps1 events pickup,kill` | prints game events as they happen (Ctrl+C stops) |
| `emf_bridge.ps1 shot` | a screenshot into `emf\screens` (`-NoOverlay`, `-MaxWidth <px>`) |
| `emf_bridge.ps1 key L` | presses L for mods (`-HoldMs`, `-Down`, `-Up`) |
| `emf_bridge.ps1 quit` | the game saves and closes |
| `emf_bridge.ps1 load_world 1 1 "<character name>"` | from the main menu: starts LOCAL world slot 1 with LOCAL character 1 offline, answers when the world runs (`load_world` below) |
| `emf_bridge.ps1 raw '{"cmd":"hello"}'` | sends one request and prints the reply as it came |

### Python: `emf_bridge.py`

One file, standard library only. The same commands from the command line (`python emf_bridge.py wait world 120
--game <folder>`, `python emf_bridge.py load_world 1 1 "<character name>" [--timeout-ms n]`), or as a library:

```python
from emf_bridge import Bridge, LoadWorldError

with Bridge(game=r"C:\SteamLibrary\steamapps\common\Enshrouded") as b:
    r = b.load_world(1, 1, "<character name>")            # from the main menu: LOCAL world 1, LOCAL character 1
    print(r["world"], r["character_name"], r["ms"])       # once the local player is in the world
    b.wait("world", timeout_ms=120000)
    b.wait("player", timeout_ms=30000)
    print(b.state()["player"]["position"])
    r = b.console("put Material_T1_Wood_Logs 5")          # status, output (late lines too), log_seq
    print(r["status"], r["output"])
    b.wait("log", match="[life_skills]", since=r["log_seq"], timeout_ms=60000)
    print(b.screenshot(max_width=1280)["path"])
```

`Bridge.request(cmd, **fields)` sends any request and raises `BridgeError` (with `.code`) when the reply is
`ok: false`; `Bridge.send` returns the reply either way. `Bridge.load_world(slot, character, expect_name=None,
timeout_ms=None)` raises `LoadWorldError` (a `BridgeError`) when nothing started; its text says in plain words what
happened and what to do (`refused`: a slot that does not exist, another name, or the guard; `unavailable`: not at the
main menu, or `load_world` is off on this build; `busy`; `timeout`: the game may still start the world;
`bad_request`). The command line prints the same words and exits 1.

### Without the game

`build\msvc\tools\emf_bridge_fake.exe <folder>` serves the same protocol with made-up game data and writes
`<folder>\emf\bridge.txt`. Point a client at it with `-Game <folder>` (or `--game`) and set `EMF_BRIDGE_TEST_PROCESS`
to `emf_bridge_fake` (the clients otherwise only accept a running `enshrouded*.exe`; this variable is for tests).
`scripts\bridge_clients_test.ps1` checks both clients against it. Its world runs 2 s after it starts; its
`load_world` answers like the game's for LOCAL world slot 1 with LOCAL character slot 1 (with made-up names, in
`tools/bridge/fake_game.cpp`) and refuses other slots and names, but not a request "not at the main menu".

## The protocol

The pipe is `\\.\pipe\emf-bridge-<pid>`. Messages are one JSON object per line, in UTF-8.

- A request: `{"id": <number or string>, "cmd": "<name>", ...}`. At most 64 KiB.
- The reply: `{"id": ..., "ok": true, ...}`, or `{"id": ..., "ok": false, "code": "<code>", "error": "<plain words>"}`.
  At most 1 MiB.
- The `code` is one of: `timeout`, `busy` (try again), `unavailable` (not possible here or now), `bad_request`,
  `unknown_command`, `refused`. A timeout always has `ok: false` and `code: "timeout"`, with what came so far when
  there is something (a console command's output).
- Pushed events have no id: `{"event": "log" | "game" | "dropped", ...}`. They only come to a client that asked for
  them.

A line that is too long, not JSON or without an id gets an error reply. Five bad lines in a row end the connection, and
so does a line left unfinished for 30 s. A client can have at most 8 requests waiting for an answer, and at most 8
console lines the game has not run yet. A client that stops reading is disconnected (8 MiB waiting for it); before
that, pushed events are dropped and it gets `{"event": "dropped", "lines": <n>}` once it catches up. Requests may be
answered out of order (a `wait` answers when its condition holds); match replies by id.

### hello

```json
{"id":1,"cmd":"hello"}
{"id":1,"ok":true,"emf":"0.3.0","api":16,"game_build":1273043,"process":"client","pid":4242,"protocol":1,
 "commands":["hello","console",...],"events":["pickup","kill","damage","save","load"],"max_clients":8}
```

`game_build` is `"unknown"` on a game build EMF does not know. `process` is `"client"` or `"dedicated server"`.

### console

`{"id":2,"cmd":"console","text":"put Material_T1_Wood_Logs 1"}` runs the line exactly as if you typed it into the
console and answers with what happened:

```json
{"id":2,"ok":true,"status":"ran","output":["put 1 x Material_T1_Wood_Logs into entity 1: 1 added"],"log_seq":1834}
```

- `status`: `ran`, `unknown` (no such command), `refused` (disabled after a crash), `needs_world` (it needs a world
  you host), `too_old` (typed while no world you host ran) or `crashed`.
- `output`: the command's lines, without the echo. A command that ran can send lines a step later (`put`'s result):
  the reply waits until no new line came for `settle_ms` (0 to 5000, default 300; 5 s at most in all). Later lines
  still go to the log.
- `log_seq`: the newest log line's number when the reply was made. `wait` for a log line `since` it has no race.
- In a world you host, the line runs on the world's next step, like a typed one. Its lines also go to the log and the
  console. Without a world you host, `help`, `mods`, `settings`, `set`, `hooks` and `pattern` still run.
- `timeout_ms` is 1 to 120000 (default 10000). After it the reply is `ok: false`, `code: "timeout"`, with the output so
  far. A line still queued when the world you hosted goes away is answered with `needs_world` at once.
- More than 512 KiB of output is cut (`"truncated": true`).

### state

`{"id":3,"cmd":"state"}` answers with one snapshot:

```json
{"id":3,"ok":true,
 "world":{"status":"running","kind":"server","tick":51234,"ecs_time_s":853.9,"time_of_day_h":13.25,"slot":1,
          "name":"<world name>"},
 "character":{"storage":"local","slot":1,"name":"<character name>"},
 "player":{"id":5,"position":{"x":1024.5,"y":88.0,"z":-310.2},
           "health":{"value":60,"max":100,"source":"attributes"}},
 "overlay":{"running":true,"console_open":false,"wheel_open":false,"hud":[{"id":"clock","text":"13:15","place":1}]},
 "mods":[{"id":"clock","name":"Clock","version":"1.1.0","status":"loaded","reason":"","source":"clock\\",...}],
 "hooks":{"total":11,"emf":9,"mods":2},
 "ticks":{"server":{"steps":52011,"interval_ms":16.7},"client":{"steps":51990,"interval_ms":16.7}},
 "frame":{"presents":61002,"frame_ms":16.6},"log_seq":1834}
```

- `world.status`: `none` (main menu), `loading` (a world steps but does not run yet) or `running`. `kind`: `server`
  (a world you host), `client` (someone else's game) or `dedicated`. `slot`: the save slot of the world last loaded or
  saved. `name`: the world's display name (as the menu shows it) in a world this game hosts, read from the game's session
  (gameApp+0x34FE8, which the main-menu hooks of `load_world` find: so only with those, on build 1273043's game exe;
  null on the dedicated server and in someone else's game).
- One job on the world's next step reads the world's own values for every waiting request; the rest comes from what
  EMF tracks anyway.
- `character`: the character the game has selected, which is the one a running world was started with.
  - `storage` is `local` or `steam` (a Steam Cloud character); `slot` is its 1-based slot in that storage's table;
    `name` is its name.
  - EMF reads it from the game's selection (save2+0x1192BE, the value Start Play's character check and the
    `load_world` guard read). It reads on the game's pregame thread, the thread that changes the selection, with
    fault-guarded reads, each time the menu state runs. A running world never changes it, so the last value stays.
  - `storage` is `unknown` (with `slot` and `name` null) when nothing is selected, when the value is not one the
    layout knows, when the memory cannot be read, or when the main-menu hooks are off (another game build).
  - It is null on the dedicated server, which has no character of its own.
  - The agent tools act on a running world only when this is `local` ([ai_agents.md](ai_agents.md)).
- `player` is null without a local player (the main menu, the dedicated server).
- `health` says where it comes from in `source`:
  - `"attributes"`: in a world you host, the player's current and maximum health attributes, read through the game's
    attribute registry (API v18 `attribute_get`), with `value` and `max`.
  - `"NetworkHealth"`: the player's replicated `NetworkHealth`, with `value` and `max`. A client of someone else's
    game has it. It is read on the client world.
  - `"server Health slot 0"`: in a world you host when the attribute functions are not found on this game build: the
    server world's `Health` component, `dataStorage[0]`, which holds the current health; no max (`max` is null).
  - `health` is null when neither can be read.
- `ticks`: the steps the bridge has seen since the game started, and the recent time between steps.
- `frame`: frames through EMF's Vulkan layer and the recent time between them; null without the layer.
- A value EMF cannot read is null. It is never guessed.

### mods

`{"id":4,"cmd":"mods"}` answers `{"mods":[...]}`: every mod with `id`, `name`, `version`, `status` (loaded, failed,
disabled, not loaded, script, loading), `reason`, `source`, `authors` and `enabled_default`. It is the Mods tab's data.

### log

EMF keeps the last 5000 log lines in memory, numbered from 1.

- `{"id":5,"cmd":"log"}`: the last 100 lines.
- `{"id":5,"cmd":"log","since":1200}`: the lines after line 1200 (at most `max`, 1 to 5000, default 1000).
- The reply: `{"lines":[{"seq":1201,"line":"[20:15:00.123 t01234] ..."}],"last":1300,"more":false,"following":false}`.
  With `"more": true`, ask again with `"since": <last>`. When lines after `since` are gone already, the reply says
  how many: `"missed": <n>`.
- `"follow": true` also pushes every new line: `{"event":"log","seq":1301,"line":"..."}`. A follower that fell more
  than 5000 lines behind gets `"missed": <n>` on the next line. `"follow": false` stops it.

### wait

`{"id":6,"cmd":"wait","for":"world","timeout_ms":120000}` answers `{"ok":true}` once a world runs, or
`{"ok":false,"code":"timeout","error":"timed out"}`. `timeout_ms` is 1 to 600000 (default 30000).

- `"for": "world"`: a world runs (yours or someone else's).
- `"for": "player"`: a world runs and the local player exists.
- `"for": "log", "match": "<text>"`: a log line contains the text (case matters). Only lines that come after the
  request count, unless you give `"since": <line number>` (0 searches every kept line; a console reply's `log_seq` is a
  good one). The reply has the line: `{"ok":true,"seq":1302,"line":"..."}`.

### events

`{"id":7,"cmd":"events","names":["pickup","kill"]}` pushes those game events to this client; `"names": []` stops
them. Each one comes as `{"event":"game","name":"pickup","fields":{...}}`:

| Name | Fields |
|---|---|
| `pickup` | `item` (debug name, or null), `item_id`, `count`, `by` (who picked it up), `from`, `position` |
| `kill` | `entity` (the entity that died) |
| `damage` | `target`, `source`, `health` and `stamina` (the change) |
| `save`, `load` | `slot` (the world's save slot) |

The events are the world you host (the server world). A client gets at most 200 a second; the next one after a
dropped burst carries `"dropped": <n>`.

### screenshot

`{"id":8,"cmd":"screenshot"}` copies the next frame the game shows into a PNG and answers
`{"ok":true,"path":"C:\\...\\emf\\screens\\2026-10-26_20-15-00-123.png","width":1920,"height":1080,"hdr":false}`.

- The file always goes to `emf\screens\<date>_<time>.png`. A request cannot name a path.
- `"overlay": false` copies the frame before EMF's overlay draws on it (no console, HUD lines or notes). By default
  the copy is what the screen shows.
- `"max_width": <16-16384>` scales the picture down to that width (the height in proportion).
- Swapchains in 8-bit (B8G8R8A8, R8G8B8A8), 10-bit (A2B10G10R10, A2R10G10B10) and 16-bit float (R16G16B16A16) work.
  An HDR10 swapchain (PQ, BT.2020) or an scRGB one is turned into linear light, mapped to sRGB and tone-mapped (SDR
  white stays white, highlights roll off), and the reply says `"hdr": true`. Other formats get an error that names the
  format in words.
- It works while the window is behind other windows. If the game showed no frame in the last 2 s (minimised), the
  reply says so at once; if no frame comes within 5 s after the request, it says that (`code: "timeout"`).
- The frame does not wait for the copy: the copy runs on the GPU after the game's own work, and EMF's worker thread
  converts it and writes the PNG. The buffer for the copy is made when you ask (not while the game draws) and kept for
  the next screenshot.
- The bridge must be on when the game starts: EMF then asks for swapchain images it can copy from.
- One screenshot at a time (`code: "busy"` for a second one). The dedicated server has no screen.

### mod_key

`{"id":9,"cmd":"mod_key","key":"L"}` presses L for mods and answers after the release:
`{"ok":true,"key":"L","vk":76,"action":"press","mod_reads":9}`.

- `key`: a letter, a digit, `F1`-`F24`, a name like `Space` or `PageUp`, or a key number like `0x4C` (the names
  `emf/keys.hpp` knows).
- `action`: `press` (default), `down` or `up`. `hold_ms` (press only): 20 to 5000, default 150.
- Mods that read keys with `emf::keys` or `GetAsyncKeyState` / `GetKeyState` see the key down (the "pressed since the
  last call" bit only on the first read of a press). While a key is held, they also see the game window in front, so
  it works without window focus; and while the game is not really in front, mods read every other key as up, so keys
  you type into another window meanwhile do not reach them.
- The game, the console and wheels do not see it. The game's own controls still need real input.
- `mod_reads` is how often mods read the key while it was held. 0 means no mod looks at that key.
- A key held `down` is released by `up`, when the client leaves, or after 60 s.

### quit

`{"id":10,"cmd":"quit"}` answers `{"ok":true,"quitting":true}`, then asks the game's window to close (as its close
button does): the game saves and exits a few seconds later. Not on the dedicated server (stop it in its own window).

### load_world

`{"id":11,"cmd":"load_world","slot":1,"character":1,"expect_name":"<character name>"}` starts a **LOCAL** save offline from
the main menu, without clicks, the way "Play, Start a Session, Start..., Offline: Start" does, and answers once the
world runs and the local player is in it:
`{"ok":true,"slot":1,"character":1,"world":"<world name>","character_name":"<character name>","ms":12109}`.

- `slot` (1-10) and `character` (1-16) are both needed: 1-based **LOCAL** slots. The storage is always Local; a Steam
  slot cannot be named. `expect_name` (optional): the character must have that name, else nothing starts.
  `timeout_ms`: 1000-600000 (default 180000), for the whole start, menu to world. `test_refuse` (true): a dev check
  of the guard below: it refuses even the LOCAL pair, so the game says "No Character selected" and stays in the menu
  (the reply: `refused`).
- How: EMF hooks the game's per-UI-frame function and its pregame state handler. The bridge thread only hands the
  request over. In one UI frame EMF posts "select character" and "select world" to the menu's own command queue (as
  its buttons do); in a later frame it checks that the game took them and that it is still the main menu, in Lobby
  mode, with no world, then posts "Offline" and "Start" together, only into an empty command queue (so nothing the
  menu queued itself, a click, runs between the selection and the start).
- It waits for the main menu and its save lists (a request sent while the game is still starting is fine), and refuses
  at once: a world is running (`unavailable`, "not at the main menu", checked when the request arrives), a slot that does not exist or another
  character name (`refused`), another `load_world` in progress (`busy`). Otherwise the reply comes when the world runs,
  or `timeout` naming what it still waited for.
- **The guard:** while an EMF-started `load_world` runs, and only then, a hook on the game's "is a character selected?"
  check (called when the start begins) turns a Steam character or Steam world, or a character with another name than
  `expect_name`, into the game's own error: it logs "No Character selected" and stays in the menu, before any world or
  session starts (the reply: `refused`, "the guard stopped the start"). Outside a `load_world` the hook does nothing,
  so your own play with any character is untouched. A request that ends after its start was posted (a timeout) leaves
  the guard on until the game is back in the main menu or the world runs, at the latest 60 s after the start, because
  the start can still come out of the game's queue. `emf_core.log` says when the guard is off again.
- The selection is the game's own (the command its character screen posts), so the menu shows that character
  afterwards, as after choosing it by hand.
- Only on build 1273043's game exe (the offsets inside the game's menu objects are that build's), with the bridge on;
  not on the dedicated server. `emf_core.log` says `load_world: on` or why it is off.
- Tested in game: from the main menu the LOCAL test world ran with the LOCAL test character 12 s after the request,
  no clicks; a wrong `expect_name` and the empty world slot 9 were refused with nothing posted.
- The tools use it: `game_session.ps1 start -Character local` (menu clicks only with a bridge that has no
  `load_world`), the scenario
  runner's `load_world` step and the MCP server's `emf_load_world` ([ai_agents.md](ai_agents.md)). Each checks
  afterwards, in the state, that the LOCAL slot it asked for runs.

## A test with the bridge

```powershell
$b = ".\scripts\emf_bridge.ps1"
& $b load_world 1 1 "<character name>"   # from the main menu: the LOCAL test world, no clicks
& $b wait world 300                 # (or load a world yourself meanwhile)
& $b wait player 60
& $b console "put Material_T1_Wood_Logs 5"
& $b wait log "[life_skills]" 30    # your mod's log line
& $b key L                          # its hotkey
& $b shot -MaxWidth 1280
& $b log -Since 0 -Max 50
& $b quit
```

## Limits

- The bridge is not part of the mod API. Mods cannot use it, and the API version stays the same.
- With the bridge on, EMF copies pickup, kill and hit events every step of the world you host (for `events`), and
  the game's swapchain images allow copying. Both cost little, and only while the bridge is on.
- `load_world` starts LOCAL saves only, offline, from the main menu, on build 1273043's game exe.
