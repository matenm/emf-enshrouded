# Tools for AI coding assistants (and people) who make, test and debug mods

These tools let an AI coding assistant, or you, drive the running game from text: run console commands, read the game's state,
wait for a world, take screenshots, press a mod's key, change a mod's file and check what happened. They are built on
the [debug bridge](debug_bridge.md), which is off unless you switch it on.

| Tool | What it is for |
|---|---|
| [MCP server](#the-mcp-server) (`tools/mcp/emf_mcp.py`) | an AI assistant in an MCP client uses the game as tools: `emf_console`, `emf_state`, `emf_screenshot`... |
| [Scenario runner](#the-scenario-runner) (`tools/bridge/emf_scenario.py`) | a test as a JSON file: steps, expected output, ok/FAIL per step, an exit code |
| [`scripts/game_session.ps1`](#a-game-session-game_sessionps1) | in this repository: install, start the game, load the test world, and clean up afterwards |
| [`llms.txt`](llms.txt) | a compact summary of the C API, the Lua API, mod.json, the console commands and the bridge, for an assistant's context |
| [`AGENTS.md`](../../AGENTS.md) | rules and commands for AI coding assistants working in this repository |

All of them need Python 3.8 or newer (standard library only), except `game_session.ps1` (Windows PowerShell).

## Switch the bridge on first

Add these lines to `emf\emf_core.ini` in the game folder and start the game (from the repository,
`scripts\install_emf.ps1 -Bridge` writes them, and `game_session.ps1 start` does it all):

```ini
[debug]
bridge=1
```

Only programs of your Windows user can connect. [debug_bridge.md](debug_bridge.md) explains what a client can do.

## The MCP server

`tools/mcp/emf_mcp.py` speaks the Model Context Protocol over stdio (JSON-RPC 2.0, one message per line; protocol
versions 2025-06-18 and 2024-11-05). Every tool call opens its own connection to the game, so you can restart the game
between calls. When something fails (no game, a command that did not run, a timeout), the tool answers with an error
in plain words; the server does not crash.

| Tool | Arguments | What it does |
|---|---|---|
| `emf_hello` | | EMF's version, the API, the game build, the bridge commands. Call it first: an error means no game or no bridge. |
| `emf_console` | `text`, `timeout_s`, `settle_ms` | runs a console line as if typed; returns the status (`ran`, `unknown`, `needs_world`...), the output and `log_seq`. A status other than `ran` is an error. Changes the game (see below). |
| `emf_state` | `path` | the game's state as JSON, or one part of it: `player.position`, `world.status`, `overlay.hud[id=clock.time].text`, `mods[id=lua].status` |
| `emf_mods` | | every mod, its version and status, and why it does not run |
| `emf_log` | `since`, `limit`, `contains` | log lines (the game keeps the last 5000); ends with the number to pass as `since` next time |
| `emf_wait` | `for` (`world`, `player`, `log`), `match`, `since`, `timeout_s` | waits for a world, the local player, or a log line |
| `emf_screenshot` | `max_width` (default 1280, at most 1920), `overlay` | the next frame as a PNG image (works behind other windows and at the main menu); a PNG over 3.7 MB is an error (ask for a smaller one) |
| `emf_mod_key` | `key`, `action`, `hold_ms` | presses a key for mods only (not the game); says how often mods read it. Changes the game. |
| `emf_run_scenario` | `path`, `keep_going` | runs a scenario file from `tests/game/` (or just its name, `smoke`) or the game's `emf\` folder. Changes the game. |
| `emf_load_world` | `slot` (1-10, default 1), `character` (1-16, default 1), `expect_name` (the LOCAL character's name: pass yours; the default is a built-in test name; `""` = any name), `timeout_s` (default 180) | from the main menu, starts a LOCAL save offline without clicks (the bridge's [`load_world`](debug_bridge.md#load_world)) and answers when the local player is in the world. When that world runs already, it starts nothing and says so. Changes the game (see below). |

**Which game the tools that change it act on.** Reading works on any game with the bridge on. `emf_console`,
`emf_mod_key`, `emf_run_scenario` and `emf_load_world` act only:

- on the game `scripts\game_session.ps1` started: its session file records that game's pid and start time
  (`%LOCALAPPDATA%\EMF\game_session\session.json`, or `EMF_GAME_SESSION`), so another game, or a reused pid, is
  refused. For a game you started yourself (a modder with the release zip), start the server with `--any-game`;
- at the main menu, or in a LOCAL save (the state's `world.slot`) played with a LOCAL character (the state's
  `character.storage` is `local`). A Steam Cloud save, a running world whose slot is unknown, a world played with a
  Steam character ("the running world uses a Steam character") and one whose character the bridge cannot tell
  ("can't tell which character runs: go back to the main menu and use load_world") are refused. Unknown counts as a
  refusal. The dedicated server has no character of its own and needs none. `--allow-cloud-world` lifts this; never
  use it on a PC whose Steam Cloud saves matter (the repository's `.mcp.json` does not).

Reading (`emf_hello`, `emf_state`, `emf_mods`, `emf_log`, `emf_wait`, `emf_screenshot`) stays allowed whatever runs.

`emf_load_world` also checks the world it started. While `load_world` runs, the game itself refuses any Steam Cloud
character or world. Afterwards the reply must name the LOCAL slot and character asked for, and the state's
`world.slot`, `world.name` and `character` must agree with it. A Steam Cloud world, another LOCAL slot, another world
name, a Steam character or another character is an error, and the recorded game is ended at once, without saving
(then run `game_session.ps1 stop`). With `--any-game` the tool only says so. A state without a slot or a character
is an error that ends nothing. When the world asked for runs already, it starts nothing, but only if it runs with a
LOCAL character of that name (`expect_name`; `""`: any LOCAL character).

A call the client cancels (`notifications/cancelled`) gets no reply. A game that stops answering ends a call after
the command's own time plus 15 s. There is no tool to quit the game: closing the game and cleaning up belongs to
`game_session.ps1 stop` (or the person at the PC).

### In Claude Code

The repository's `.mcp.json` registers the server as `emf`:

```json
{"mcpServers": {"emf": {"command": "python", "args": ["-I", "tools/mcp/emf_mcp.py"]}}}
```

- Claude Code asks you to approve a project's MCP server the first time it starts in the repository. Approve `emf`
  to use it. `/mcp` shows whether it runs; `claude mcp reset-project-choices` asks again.
- It needs `python` on the PATH. If `python` only opens the Microsoft Store, install Python, or use
  `"command": "py", "args": ["-3", "-I", "tools/mcp/emf_mcp.py"]`.
- The server finds the running game itself. For a game in another folder, add `"env": {"EMF_GAME": "D:\\Games\\Enshrouded"}`.
- Long waits, world starts and scenarios take as long as they take; Claude Code's `MCP_TOOL_TIMEOUT` (milliseconds)
  limits a call.
- Outside this repository: `claude mcp add emf -- python -I C:\path\to\enshrouded\tools\mcp\emf_mcp.py`.

### In other MCP clients, and from the release zip

Any client that starts stdio servers works. The command is `python` with the arguments `-I <path>/tools/mcp/emf_mcp.py`
and, as needed, `--game <game folder>`, `--any-game`, `--session-file <path>`. Most clients use the same `mcpServers`
JSON as above in their own settings file.

The release zip installs the server and the scenario runner next to the bridge clients, in `<game>\emf\tools\`. There
the game is usually your own: add `--any-game`. The server then runs scenarios from the game's `emf\` folder.

### What an assistant can do with it

- Read everything: the state, the mods, the log, the screen.
- Start a LOCAL save of the recorded game from the main menu (`emf_load_world`), never a Steam Cloud one.
- Run any console command that exists, in a LOCAL world of the recorded game. Some change the world (`give`, `spawn`,
  `destroy`, `set`).
- Press keys for mods. It never sends keyboard or mouse input to the game itself.
- Change files inside `emf\mods\<id>\` through a scenario's `edit_file` step. They are put back at the end.

## The scenario runner

A scenario is a JSON file with a name and steps. The runner sends each step to the game through the bridge, prints
`ok` or `FAIL` with the reason, and stops at the first failure (`--keep-going` runs the rest anyway):

```
python tools\bridge\emf_scenario.py tests\game\smoke.json
ok    1 hello: EMF 0.3.0 (API 17), game build 1273043, client, pid 84672
ok    2 wait world: a world is running
...
ok    9 screenshot world: C:\...\emf_scenario\smoke_20261027-004953_3f9a1c\09_world.png (1280x663, HDR tone-mapped)
smoke: 9 passed, 0 failed, 0 skipped (0.6 s)
```

- Exit codes: 0 every step passed, 1 a step failed (or a file could not be put back), 2 no connection to the game,
  3 the file is not valid.
- `--json report.json` writes a report for programs (`--json -` prints it; the step lines then go to stderr).
- `--out <folder>`: where screenshots and file backups go (default `%TEMP%\emf_scenario\<name>_<time>_<random>`, a
  new folder for every run). The folder stays after the run, with the backups of the files the run changed.
- `--check <files>` checks scenario files without the game. `--steps` lists every step kind and its fields.
- `--game <folder>` or `--pid <n>` when more than one game runs.
- `console`, `mod_key` and `edit_file` steps run only at the main menu, or in a LOCAL save played with a LOCAL
  character (the state's `character.storage` is `local`; and named as the scenario's own `load_world.expect_name`
  when the scenario gives one). In a Steam Cloud save, with a Steam character, or when the bridge cannot tell the
  world's slot or its character, they fail and change nothing. `--allow-cloud-world` lifts this (never on a
  PC whose Steam Cloud saves matter). Reading steps (hello, wait, state, mods, screenshot, log marks) always run.
- A `load_world` step starts a LOCAL save from the main menu through the bridge; it never starts a Steam Cloud one.
- It never sends input to the game window and never touches saves.

### Writing a scenario

```json
{
  "name": "my_mod",
  "description": "My mod's command and its line on screen.",
  "load_world": {"slot": 1, "character": 1, "expect_name": "<your LOCAL character>"},
  "steps": [
    {"do": "load_world"},
    {"do": "wait", "for": "player", "timeout_s": 300},
    {"do": "mods", "id": "my_mod", "status": "script"},
    {"do": "console", "text": "my_mod.hello", "expect": "hello from my_mod"},
    {"do": "state", "path": "overlay.hud[id=my_mod.line].text", "op": "contains", "value": "Hello", "timeout_s": 5},
    {"do": "log_mark", "name": "edit"},
    {"do": "edit_file", "path": "emf/mods/my_mod/main.lua", "find": "Hello", "replace": "Bye"},
    {"do": "log_since_mark", "mark": "edit", "match": "[my_mod] reloaded", "timeout_s": 15},
    {"do": "log_since_mark", "mark": "edit", "match": "my_mod/main.lua:", "absent": true},
    {"do": "screenshot", "name": "after_edit", "max_width": 1280}
  ]
}
```

A scenario has `"name"`, `"steps"`, and may have `"description"` and `"load_world"`: the defaults of its
`load_world` steps (`slot`, `character`, `expect_name`; without it slot 1, character 1 and the tools' built-in test
character name: give `expect_name` for yours, or `""` for any). Every step has `"do"` (its kind) and may have a `"label"` that is printed instead of
its own title.

| Kind | Fields (* required) | Passes when |
|---|---|---|
| `hello` | `expect` (fields of the reply, e.g. `{"process": "client"}`) | the game answers (with those values) |
| `wait` | `for`* (`world`, `player`, `log`), `match` (for log), `since`, `timeout_s` (default 30) | the world runs / the player exists / a new log line contains `match` |
| `console` | `text`*, `expect` (text or list), `expect_re`, `status` (default `ran`), `timeout_s`, `settle_ms` | the command ends with that status and its output contains / matches |
| `state` | `path`*, `op`*, `value`, `timeout_s` (keeps asking until it holds) | the value at `path` passes `op` |
| `mods` | `id`*, `status` | the mod is listed (with that status: `loaded`, `script`, `disabled`, `failed`...) |
| `mod_key` | `key`*, `action` (`press`, `down`, `up`), `hold_ms`, `min_reads` | the bridge pressed it (and mods read it at least `min_reads` times) |
| `screenshot` | `name`, `overlay`, `max_width` | the PNG is copied into the output folder as `<step>_<name>.png` |
| `sleep` | `s`* | always (waits `s` seconds) |
| `edit_file` | `path`*, `find` + `replace` (once, or every time with `all`), or `write` | the file inside `emf/mods/<id>/` was changed; it is put back at the end, also after a failure or Ctrl+C (a new file is deleted) |
| `restore_file` | `path`* | puts an edited file back now |
| `log_mark` | `name`* | remembers the newest log line |
| `log_since_mark` | `mark`*, `match` or `match_re`, `absent`, `timeout_s` | a line after the mark matches (within `timeout_s`); with `absent`, none does |
| `load_world` | `slot` (1-10), `character` (1-16), `expect_name` (`""` = any name), `timeout_s` (default 180); the defaults come from the scenario's `load_world`, else 1, 1 and the built-in test character name | from the main menu, the bridge's [`load_world`](debug_bridge.md#load_world) started that LOCAL save without clicks, its reply names that slot and character, and the state's `world.slot`, `world.name` and `character` agree. It passes at once, starting nothing, when that LOCAL slot runs already in a world you host with a LOCAL character of that name; a Steam or unknown character, or another running world, fails it |

A `load_world` step that finds another world after the start (a Steam Cloud slot, another LOCAL slot, another name)
fails with "WRONG WORLD": end the game at once without saving and check the saves. The runner never ends the game
itself.

State paths are dotted: `player.position.x`. `[n]` picks a list element (`mods[0]`, `[-1]` the last) and
`[key=value]` the element whose key has that value (`overlay.hud[id=clock.time]`, `mods[id=lua]`); the value may
contain dots. The ops: `exists`, `missing` (not there or null), `==`, `!=`, `<`, `>`, `<=`, `>=`, `contains` (text in a
text; an element in a list, where an object matches when it has the given fields; a key in an object), `matches` (a
regular expression on a text). A failed check says what it found: `no element with id=probe in overlay.hud (3
elements: kill_counter.count, life_skills.skills, clock.time)`.

`edit_file` takes paths relative to the game folder and refuses anything that is not a file in a mod's folder in
`emf\mods` (also through a junction), a stream (`file:name`), a device name (`CON`, `NUL`, `COM1`...) and a name ending
in a dot or a space. Text in `find` with `\n` also matches a file with Windows line ends. While files are changed, the
run keeps a journal (`restore.json` in its output folder). If a run is cut off (killed, its window closed), the next
run, or `emf_scenario.py --recover`, puts the files back from that journal: only where a file still holds exactly
what the cut-off run wrote, and only mod files of that run's game with backups from that run's own folder. A file a
modder changed meanwhile is left alone and reported; `--recover` then exits 1. The same holds at the end of a run: a
file someone else saved during the run is kept, reported, and counts as a failure.

EMF's own scenarios are in `tests/game/`:

| Scenario | Checks | Needs |
|---|---|---|
| `smoke.json` | hello, the world and the player, the position, `pos`, the clock mod, a screenshot | a world |
| `hud.json` | `hud probe 2 text` shows in the state, `hud probe` removes it | a world you host |
| `lua_starter.json` | [first_mod.md](first_mod.md) part 3: change `main.lua`, the mod reloads, the line changes, no Lua error, and back | `template_lua` installed as `my_first_mod`, a world |

## A game session: `game_session.ps1`

In this repository, `scripts\game_session.ps1` runs a whole test session on your PC. It follows the rules in
[AGENTS.md](../../AGENTS.md): never the Steam Cloud character or world, only your LOCAL test character and the LOCAL
test world, no input while a person uses the PC, and a clean game folder afterwards.

```powershell
$s = ".\scripts\game_session.ps1"   # (powershell -ExecutionPolicy Bypass -File scripts\game_session.ps1 ... from another shell)
& $s start -LockOwner me -Mods template_lua   # exit 2: look at the menu screenshot it names
& $s start -LockOwner me -Character local     # the menu showed your [LOCAL] character: load_world starts the test world
python tools\bridge\emf_scenario.py tests\game\lua_starter.json
& $s stop -LockOwner me
```

Put your test character in LOCAL character slot 1 and your test world in LOCAL world slot 1, and name them with
`-CharacterName "<name>"` and `-WorldName "<name>"`.

**What it records**, so that it never guesses which game is its own:

- **The session file** (`%LOCALAPPDATA%\EMF\game_session\session.json`, or `EMF_GAME_SESSION`, or `-StateFile`): the
  game process `start` launched (its pid and start time), the pid a menu screenshot was shown for, and when the
  script last sent input. Any other game is someone else's: `start` and `stop` refuse it (`-AdoptGame` takes it over),
  and so do the MCP server's tools that change the game.
- **The game lock** (`%LOCALAPPDATA%\EMF\game_session\game.lock`, or `EMF_GAME_LOCK`, or `-LockFile`), always checked:
  `<owner> <time> pid=<game pid>`. `-LockOwner <you>` is required for `start` and `stop` (the same word for both;
  or `EMF_GAME_LOCK_OWNER`). Someone else's lock refuses both (`-IgnoreLock`). A lock whose pid no longer runs, or an
  empty one older than a minute, is stale and taken over. To share another session's lock file (several agents on one PC),
  pass it with `-LockFile` or set `EMF_GAME_LOCK`.
- **A marker in the game folder** (`emf\game_session.json`): EMF that `start` installed, and the `-Mods` folders it
  made. `start` refuses an EMF it did not install (one a person installed), `-Mods` refuses to replace a
  folder it did not make, and `stop` uninstalls only an install with the marker.

**start**

1. Refuses (exit 3) when someone used the PC in the last 2 minutes and the last input was not the script's own.
   Which window is in front does not matter (a person may be playing the game itself), and the script's own input
   is remembered across runs. `-IgnoreIdle` is for when the person at the PC hands it over: their input from before
   this run is accepted.
2. Runs `save_guard.ps1 -Check` and stops when the Steam Cloud saves changed. Recording them again
   (`save_guard.ps1 -Record`) is the PC user's step after their own play; an agent stops and reports.
3. Installs the dev build with the bridge on (`install_emf.ps1 -Bridge`), the `-Mods` folders (`template_lua` goes in
   as its mod.json id, `my_first_mod`; `<folder>=<id>` names the target) and the marker.
4. Sets windowed mode, starts the game through Steam, and records it.
5. Waits for the bridge and the main menu (the game's log line `[GameUi] Entered PreGame` after the renderer is ready,
   then a frame that is not black; a menu that stays black stops `start`, exit 1).
6. **After it launched the game, `start` always stops at the menu** with a screenshot and exit 2, whatever
   `-Character` says: the game shows the character played last, which after your own play may be your "[STEAM]
   <name>". Look at it: the label on the right must read "[LOCAL] <your test character>". Then run `start -Character local`; it
   goes on only for a game whose menu screenshot the script showed (the session file says so). If it shows "[STEAM]
   <name>" on a build without `load_world`, run `start -Character switch`: it clicks Change, the LOCAL tab and
   slot 1 (coordinates for a 3840x2160 screen), presses Esc twice and stops again with a new screenshot. `load_world`
   selects the LOCAL character itself.
7. With `-Character local`, after the person check, it starts the world through the bridge's
   [`load_world`](debug_bridge.md#load_world): LOCAL world slot 1 with LOCAL character slot 1, which must be named
   `-CharacterName`. No clicks, no input, any screen size; while it runs the game itself refuses any Steam Cloud
   character or world. A refusal (a slot that does not exist, another name, the guard), a timeout or no answer is
   reported and stops `start` (exit 1, no clicks; the game stays at the menu for `stop`).
8. Only when the game's bridge has no `load_world`, or its `load_world` is off on this game build (nothing was
   started), it falls back to the menu clicks. They are made for one screen size: it checks that the screen
   (3840x2160) and the game window (2880x1537) are what the coordinates in `start_local_world.ps1` are for, and
   refuses (exit 3) otherwise. Then it clicks Play > Start a Session > slot 1 > Offline Start. Before every click it
   checks that nobody else touched the mouse or keyboard since its own last input (no switch skips this). The output
   says which way the world was started.
9. Either way it watches the world's load event, which comes before the world's first step: a world in any other
   slot than the LOCAL slot 1 is ended at once, before it runs. So is a world whose name is not `-WorldName` (the one
   `load_world`'s reply names, then the state's `world.name`), and one the state's `character` says runs with a Steam
   character or another character than `-CharacterName`. Then the saves are checked, EMF is removed, Fullscreen is
   set, the lock is released, and it says so loudly (exit 1). An unknown slot or a character the bridge cannot tell
   stops `start` (exit 1) without ending anything; a build without `world.name` is said, and the slot decides. A
   world this run did not open is never ended: with a Steam character `start` refuses it (exit 3). Then it waits for
   the player and prints the time it took.
10. Builds that need the click fallback cannot tell the character (they have no working main-menu hooks), so after
    the clicks `start` stops at the character check (exit 1); the world stays up for `stop`.

`start` again while the game runs goes on from where it is (a world that is loading gets no start). A `start` that
fails before the game runs removes what it installed and releases the lock. Ctrl+C (or an unexpected error) prints
what to run next.

**stop** asks the game to quit through the bridge (it saves and exits in about 5 seconds; without the bridge and with
`-AllowUnansweredBridge`, it asks its window to close). If the game does not close within 60 s:

- with no world running (the main menu), it is ended at once;
- a running world, or one whose state is unknown, is ended only if it is the LOCAL slot 1: after the next "world
  saved" line and 15 s more for the game to exit by itself, or after 120 s without such a line;
- any other world is never ended by force (`-AllowKill` does): `stop` leaves the game running, keeps EMF and the lock,
  says so, and exits 1.

Then it uninstalls EMF (`uninstall_probe.ps1`, which archives the logs to `analysis\runs` in the repository; only
game_session's install), sets `"windowMode": "Fullscreen"`, runs `save_guard.ps1 -Check` and releases the lock. When the uninstall
fails it keeps the lock and says so. It refuses a game whose bridge does not answer (`-AllowUnansweredBridge`),
someone else's game (`-AdoptGame`) and someone else's lock (`-IgnoreLock`). `-KeepInstalled` leaves EMF in place.

**status** prints the game (and whether it is this session's), the bridge (and whether it has `load_world`), the
world and its slot, the player, the install, the window mode, the lock and the person check (`-Json` for programs).

Good to know:

- While the game window is behind other windows, the game runs slower and answers the bridge more slowly.
- Exit codes: 0 done, 1 a step failed (stop: or the saves changed, or the game was left running), 2 start stopped at
  the menu for the character check, 3 refused, 4 a wrong command line.

## llms.txt and AGENTS.md

[`llms.txt`](llms.txt) is generated: `python tools\gen_llms_txt.py` rebuilds it from `sdk/include/emf/emf_api.h`, the
Lua host's sources, [lua.md](lua.md), [reference.md](reference.md), the console commands' help texts (the core's and
the shipped mods', C++ and Lua) and [debug_bridge.md](debug_bridge.md). `scripts\test_tools.ps1` (part of `verify.sh`)
fails when it is out of date, when lua.md misses a function the Lua host registers, or when debug_bridge.md misses a
bridge command. Give an AI assistant the file (about 35 KB) when it writes a mod.

[`AGENTS.md`](../../AGENTS.md) at the repository's root has the rules and commands for AI coding assistants that work
on EMF itself: the build, the tests, the map of the repository, the safety rules for testing in the game and the
commit rules.
