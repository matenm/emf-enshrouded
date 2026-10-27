# Contributing to EMF

Thanks for helping. EMF is a fan-made modding framework for Enshrouded: a loader, a core DLL with a C API and Lua, an
in-game console, and EMF.exe, a desktop mod manager. This page is for people who want to change EMF itself. To write
a mod, start with [docs/modding/README.md](docs/modding/README.md) or the Lua guide
[docs/modding/first_mod.md](docs/modding/first_mod.md).

## Build

You need:
- Windows 10 or 11, x64;
- Visual Studio 2022 or its Build Tools, with "Desktop development with C++";
- CMake 3.24+ and Ninja (both come with Visual Studio's C++ CMake tools);
- Git Bash for the `.sh` scripts.

```
scripts\dev.cmd cmake --preset msvc
scripts\dev.cmd cmake --build --preset msvc
build\msvc\emf_tests.exe
```

`scripts\dev.cmd` runs a command inside the Visual Studio developer environment (it finds Visual Studio with
`vswhere`).

## Tests

| What | Command | Needs the game |
|---|---|---|
| Unit tests (doctest) | `build\msvc\emf_tests.exe` or `bash scripts/test.sh` | no |
| The release package: install scripts against fake game folders, both zips, the mod template built from the zip | `powershell -File scripts\test_package.ps1` | no |
| EMF.exe against a fake game folder | `powershell -File scripts\app_smoke.ps1` | no (needs the WebView2 Runtime) |
| The debug bridge's clients against a fake game | `powershell -File scripts\bridge_clients_test.ps1` | no |
| The agent tools: the scenario runner, the MCP server, `llms.txt` up to date, `game_session.ps1` against a fake PC | `powershell -ExecutionPolicy Bypass -File scripts\test_tools.ps1` | no (Python 3 for the Python half) |
| Everything above, the byte-signature check against your `enshrouded.exe`, and a check that the public copy (`scripts/export_public.ps1`) holds no personal data | `bash scripts/verify.sh` | for the signature check (it reads the exe; found through Steam or `EMF_GAME_DIR`, skipped without the game) |

GitHub Actions runs the unit tests and the package test on every push (`.github/workflows/build.yml`).

Changes that touch the game also need a check in the game. Use a local test character and world, never your main
save: back up `%USERPROFILE%\Saved Games\keen games\Enshrouded\` first. `scripts\install_emf.ps1` installs a
development build; `scripts\uninstall_probe.ps1` removes it again.

`scripts\game_session.ps1` does a whole session in a few commands (each with `-LockOwner <you>`): `start` installs
the dev build with the debug bridge on (and `-Mods` folders such as `template_lua`), starts the game and stops at the
main menu with a screenshot so you can check the character; `start -Character local` starts LOCAL world slot 1 with
LOCAL character slot 1 through the debug bridge's `load_world`, without clicks, on any screen; `stop` quits,
uninstalls and restores fullscreen. In between, `python tools\bridge\emf_scenario.py tests\game\smoke.json` runs a
check as a JSON scenario (ok/FAIL per step, an exit code). Scenarios for a feature go into `tests\game\`. See
[docs/modding/ai_agents.md](docs/modding/ai_agents.md). Put your LOCAL test character and world in slot 1 and name
them with `-CharacterName "<name>"` and `-WorldName "<name>"`. Only a build whose bridge has no `load_world` falls
back to menu clicks; those are made for a 3840x2160 screen, and the script refuses to click when the screen or the
game window has another size.

AI coding assistants: read [AGENTS.md](AGENTS.md) first.

## How we work

- **Tests first.** Logic gets a unit test that fails before the change and passes after it. Engine facts get a note
  in `docs/ENGINE_MAP.md` with how they were confirmed.
- **The game is not ours.** The repository holds no game files: no binaries, no extracted assets, no decompiled
  code listings. Byte signatures, offsets, struct layouts and names found by analysing the game are fine.
- **Never break a player's setup.** Installers never delete or overwrite another mod's files. They rename, back up,
  record, and put things back on uninstall.
- **The C API is stable.** `sdk/include/emf/emf_api.h` only grows. A new function or field means a new API version,
  with minimum struct sizes in `src/core/api_sizes.h`.
- **Plain words.** Docs, log lines and messages use short, plain sentences. Say what happened and what to do next.
- **Commits:** one topic each, with a message that says what changed and why.

## Where things are

`README_EMF.md` has the map of the repository. `docs/ENGINE_MAP.md` and `docs/engine_parts/` have what is known about
the game's engine.

## Security

Found a way to misuse EMF (installer, EMF.exe, the dedicated server, the debug bridge)? Report it privately: see
[SECURITY.md](SECURITY.md).

## Licence

EMF is under the MIT licence (`LICENSE`). By contributing, you agree that your contribution is under the same licence.
Third-party code and its licences are listed in `THIRD_PARTY_NOTICES.md`.
