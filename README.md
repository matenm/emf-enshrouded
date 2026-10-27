# EMF: Enshrouded Modding Framework

EMF is a fan-made modding framework and mod manager for [Enshrouded](https://enshrouded.com).
- It loads mods: EMF's own (Lua or C++), EML mods, and the DLL mods other tools use.
- It shows every installed mod in one place, with switches, settings and clash warnings.
- It installs mods from Nexus Mods safely, and gives modders a stable API with an in-game console.

EMF is unofficial and not affiliated with Keen Games.

## Players

1. Download EMF from its [Nexus Mods page](https://www.nexusmods.com/enshrouded/mods/246). There are two files:
   - **with the mod manager:** the recommended one;
   - **manual install:** no .exe.
2. Extract the zip and double-click `install.cmd`. It finds the game through Steam.
3. Start the game. The key under Esc opens EMF's console. Its **Mods** tab has switches and settings.
4. Optional: `emf\EMF.exe` in the game folder is the mod manager. With it you can:
   - see every mod;
   - switch mods and change settings;
   - install a downloaded file with a plan and backups;
   - back up and restore your saves (the Saves tab).

   EMF.exe does not contact Nexus Mods: its Browse tab, update checks and account features are switched off (a tester
   setting switches them on). See [docs/app.md](docs/app.md).

EMF backs up your saves before it installs anything, and again at each game start when a save changed. Restoring a
backup never removes it, and uninstalling EMF never deletes backups. See [docs/saves.md](docs/saves.md).

EMF keeps your other mods working. A mod that also wants the name `dinput8.dll` is renamed and started by EMF, and is
put back when you run `uninstall.cmd`.

## Modders

- **Lua, no compiler:** [Your first mod in 10 minutes](docs/modding/first_mod.md), with the starter in
  `template_lua/`.
- **C++:** [docs/modding/README.md](docs/modding/README.md) covers the C API (versioned, stable), hooks that chain
  with other mods, entities, items, events, commands and settings. A ready CMake template is in `template/`.
- **Reference:** [reference.md](docs/modding/reference.md) for every mod.json field and API function,
  [lua.md](docs/modding/lua.md), [cookbook.md](docs/modding/cookbook.md),
  [server.md](docs/modding/server.md) for dedicated servers, and [updating.md](docs/modding/updating.md) for
  game updates.
- **How the engine works:** [docs/ENGINE_MAP.md](docs/ENGINE_MAP.md).
- **AI coding assistants:** an MCP server, test scenarios and a compact API summary:
  [ai_agents.md](docs/modding/ai_agents.md). [AGENTS.md](AGENTS.md) has the rules and commands for assistants that
  work in this repository.

## Building EMF

You need Visual Studio 2022 (or its Build Tools) with C++, plus CMake and Ninja:

```
scripts\dev.cmd cmake --preset msvc
scripts\dev.cmd cmake --build --preset msvc
build\msvc\emf_tests.exe
powershell -File scripts\package.ps1
```

[CONTRIBUTING.md](CONTRIBUTING.md) has the tests and the house rules. [README_EMF.md](README_EMF.md) maps the whole
repository.

## Licence

MIT ([LICENSE](LICENSE)). Third-party code and its licences: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
