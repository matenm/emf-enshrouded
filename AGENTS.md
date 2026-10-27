# AGENTS.md: rules and commands for AI coding assistants

This file has the rules and commands for AI coding assistants (agents) working in this repository.

EMF (the Enshrouded Modding Framework) is a native mod framework for the game Enshrouded on Windows. A `dinput8.dll`
loader starts `emf\emf_core.dll` inside the game; the core hooks the game's ECS step and loads mods (C++ DLLs with a
versioned C API, and Lua scripts) from `<game>\emf\mods\<id>\`. It has an in-game console (a Vulkan layer with Dear
ImGui), a desktop mod manager (`EMF.exe`, WebView2), a Nexus client, and a debug bridge for tests. The game is not
ours: everything about the engine was found by analysis and is written down in `docs/ENGINE_MAP.md`.

These rules are for agents that change EMF itself. To write a mod, read [docs/modding/first_mod.md](docs/modding/first_mod.md)
(Lua) or [docs/modding/README.md](docs/modding/README.md) (C++), and [docs/modding/reference.md](docs/modding/reference.md);
[docs/modding/llms.txt](docs/modding/llms.txt) has the whole API in about 45 KB.

## Safety rules on the user's PC (always)

- **Never the Steam Cloud character or world.** The main menu may show "[STEAM] <name>". `scripts\game_session.ps1
  start -Character local` starts the world through the bridge's `load_world`, which selects LOCAL character slot 1
  itself and makes the game refuse any Steam save meanwhile; on an older build without it, switch to LOCAL first
  (`-Character switch`). Test only with the user's **LOCAL test character** and **LOCAL test world** (named with
  `-CharacterName "<name>"` and `-WorldName "<name>"`).
- **No input while a person uses the PC.** Before any synthetic click or key: `GetLastInputInfo` must show no input
  in the last 2 minutes that was not your own, whichever window is in front (a person may be playing the game
  itself), and before each further click, no input newer than your own last one. `game_session.ps1` does exactly
  this and remembers its own input; `scripts\ui.ps1` also refuses input unless the game window is in front.
  `-IgnoreIdle` is only for when the user hands the PC over. Never launch the game or send input while the user
  plays (online with a friend, for example). If the PC is in use, skip the in-game check and say so.
- **Saves:** `scripts\save_guard.ps1 -Check` before and after a game run. When it says CHANGED, stop and report it:
  `save_guard.ps1 -Record` is the user's step, after their own play, never an agent's.
- **Leave the game folder clean:** uninstall (`scripts\uninstall_probe.ps1`) and set `"windowMode": "Fullscreen"` in
  `%USERPROFILE%\Saved Games\keen games\Enshrouded\local_settings.json`. `game_session.ps1 stop` does all of it.
- **Several agents:** `game_session.ps1` always checks a game lock (default `%LOCALAPPDATA%\EMF\game_session\game.lock`;
  the one your task names with `-LockFile <path>` or `EMF_GAME_LOCK`) and needs `-LockOwner <you>`. It refuses a
  game it did not start and someone else's lock; wait or skip then. `stop` releases the lock once the game is closed
  and the folder is clean.
- **The tools' LOCAL rule:** the MCP server's tools that change the game, the scenario runner's console, mod_key
  and edit steps and game_session act on a running world only when it is a LOCAL save played with a LOCAL character
  (the bridge's `state.character.storage` is `local`). A Steam character, or one the bridge cannot tell, refuses
  them; then go back to the main menu and use `load_world`. Reading (hello, state, log, shot) stays allowed.
  `load_world` (the bridge command, the scenario step, `emf_load_world`) starts LOCAL saves only and checks the world
  and the character afterwards. Never pass `--allow-cloud-world` (or `--any-game` to the MCP server for the user's
  own game) on the user's PC.
- **The dedicated server lists itself publicly.** Ask the user before running one.
- Report exactly what you ran in the game.

## Build and test

```
scripts\dev.cmd cmake --preset msvc            (once)
scripts\dev.cmd cmake --build --preset msvc
build\msvc\emf_tests.exe                       (C++ unit tests, doctest)
bash scripts/verify.sh                         (everything, with the game closed: unit tests, the package test, the
                                                EMF.exe smoke test, the bridge clients and agent tools against fake
                                                games, the byte signatures against the installed game exe)
```

The scripts find the game through Steam's library folders (`scripts\find_game.ps1`); `-Game <folder>` or
`EMF_GAME_DIR` names another one. Without the game, `verify.sh` skips the signature check and says so. `verify.sh`
(and `test.sh` for the unit tests) give the tests a `%TEMP%` of their own and fail when a test leaves anything there:
a test removes what it makes, also when it fails (a `finally`, `addCleanup`, or `test_scratch::remove_at_exit`).

From Git Bash: `cmd //c "scripts\\dev.cmd cmake --build --preset msvc"`. Other checks:

| What | Command |
|---|---|
| The agent tools (Python unit tests, llms.txt up to date, game_session without the game) | `powershell -ExecutionPolicy Bypass -File scripts\test_tools.ps1` |
| game_session.ps1 alone, against a fake PC | `powershell -ExecutionPolicy Bypass -File scripts\game_session_test.ps1` |
| The bridge clients against `emf_bridge_fake.exe` | `powershell -ExecutionPolicy Bypass -File scripts\bridge_clients_test.ps1` |
| The release package | `powershell -ExecutionPolicy Bypass -File scripts\test_package.ps1` |
| EMF.exe against a fake game folder | `powershell -ExecutionPolicy Bypass -File scripts\app_smoke.ps1` |
| Python tools only | `python -m unittest discover -s tests\tools` |
| Regenerate `docs/modding/llms.txt` (after changing the API, lua.md, reference.md, commands or the bridge) | `python tools\gen_llms_txt.py` |

## Testing in the game

The debug bridge ([docs/modding/debug_bridge.md](docs/modding/debug_bridge.md)) is a local named pipe into the running
game: console commands with their output, the state as JSON, waits, log lines, screenshots, mod keys and quit. It is
off unless `emf\emf_core.ini` has `[debug] bridge=1`. On top of it ([docs/modding/ai_agents.md](docs/modding/ai_agents.md)):

- **`scripts\game_session.ps1 start | stop | status -LockOwner <you>`**: installs the dev build with the bridge and
  `-Mods`, starts the game, and always stops at the main menu after it launched it (exit 2: check the character in
  the screenshot, then `start -Character local`); loads the LOCAL test world (slot 1) through the bridge's
  `load_world` (no clicks, any screen; the menu clicks, made for a 3840x2160 screen, only on a build without
  `load_world`) and ends at once a world in any other slot or with another name than `-WorldName`; `stop` quits (never ends by force a world that is not the LOCAL slot 1),
  uninstalls its own install, restores Fullscreen and checks the saves. It records its game, so it never acts on
  someone else's.
- **`tools\bridge\emf_scenario.py <scenario.json>`**: a test as JSON steps (load_world, wait, console with expected
  output, state checks by path, mods, mod keys, screenshots, file edits that are put back, log marks). EMF's own are
  in `tests\game\` (`smoke`, `hud`, `lua_starter`).
- **`tools\mcp\emf_mcp.py`**: the same as MCP tools (`emf_console`, `emf_state`, `emf_screenshot`, `emf_run_scenario`,
  `emf_load_world`...). The tools that change the game act only on the game game_session started, in a LOCAL world.
  The repository's `.mcp.json` registers it; the MCP client asks you to approve it.
- **Clients:** `scripts\emf_bridge.ps1` and `tools\bridge\emf_bridge.py` (`hello`, `console`, `state`, `wait`,
  `shot`, `key`, `quit`, `load_world`...).
- Without the bridge: `scripts\ui.ps1` (screenshots and synthetic input, game window only), `emf\commands.txt` (read
  while a world runs) and `emf\emf_core.log` (truncated at each game start; `uninstall_probe.ps1` archives it).

A typical check (from Git Bash or cmd: `powershell -ExecutionPolicy Bypass -File scripts\game_session.ps1 ...`):
`start -LockOwner me -Mods template_lua`, look at the screenshot, `start -LockOwner me -Character local`, run
scenarios or MCP tools, `stop -LockOwner me`.

## Where things are

| Path | What |
|---|---|
| `src/loader/`, `src/proxy/` | the `dinput8.dll` loader (chain-loads other `dinput8` mods) |
| `src/core/` | `emf_core.dll`: hooks, the C API (`api.cpp`), mod loading, settings, commands, events, items, the overlay (`overlay/`), the debug bridge (`bridge/`) |
| `src/common/` | signatures, code offsets, scanner, hook wrapper, logging |
| `src/lua_host/` | the Lua host (the `lua` mod's engine): `host.cpp` and `bindings.cpp` register the `emf.*` functions |
| `src/nexus/`, `app/` | the Nexus client and `EMF.exe` |
| `sdk/include/emf/` | the mod SDK: `emf_api.h` (the C API, only ever appended), `emf.hpp`, `generated/` |
| `mods/` | the shipped mods (C++ and Lua); full examples |
| `template/`, `template_lua/`, `examples/` | the C++ template, the Lua starter (installs as `my_first_mod`), small examples |
| `tools/bridge/` | the Python bridge client, the scenario runner, the fake game for tests (`fake_game.cpp`) |
| `tools/mcp/` | the MCP server |
| `tools/re/`, `tools/sigcheck/` | static-analysis scripts (use them before opening a disassembler), the offline signature check |
| `scripts/` | build, install, package, test and game-session scripts |
| `tests/` | C++ unit tests (`test_*.cpp`), `tests/tools/` (Python tests of the tools), `tests/game/` (scenarios) |
| `docs/modding/` | the modders' docs |
| `docs/ENGINE_MAP.md`, `docs/engine_parts/` | engine facts: read them before analysing anything |

`README_EMF.md` maps the repository in more detail.

## How we work

- **Tests first.** Logic gets a unit test that fails before the change. Engine facts go into `docs/ENGINE_MAP.md`
  with how they were confirmed.
- **No game files in the repository:** no binaries, extracted assets or decompiled listings. Signatures, offsets and
  layouts found by analysis are fine.
- **Never break a player's setup.** Installers never delete or overwrite another mod's files.
- **The C API only grows.** A new function means a new `EMF_API_VERSION` and minimum struct sizes in
  `src/core/api_sizes.h`; regenerate `llms.txt`.
- **Plain words** in docs, logs and messages: short sentences that say what happened and what to do.
- **Gameplay runs on the host's server world.** Touch ECS data only in world callbacks on the world passed in.
- **Commits:** one topic each, with a plain message that says what changed and why. **No AI attribution:** no
  `Co-Authored-By` lines and no "generated with" trailers. Commit locally; never push unless asked.
- **Shell notes (Windows):** bash heredocs here collapse backslashes, so write files with an editor tool, not
  `cat <<EOF`; scripts target Windows PowerShell 5.1; `windows.h` macros (`near`, `far`, `small`) clash with names.
