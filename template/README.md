# mymod: an EMF mod template

A complete, buildable EMF mod: a console command, an on-screen line with your position, and a log line. Copy this
folder, rename the mod, replace the examples.

## Build

You need Visual Studio 2022 (or its Build Tools) with the "Desktop development with C++" workload; it brings CMake
and Ninja.

- **Visual Studio:** File > Open > Folder... > this folder. Pick the `Release (x64)` configuration, then
  Build > Build All. The mod is the folder `build/release/mymod/` (`mymod.dll` and `mod.json`).
- **Command line** (a "x64 Native Tools Command Prompt for VS 2022"):

  ```
  cmake --preset release
  cmake --build --preset release
  ```

The SDK is found at `../sdk/include` (where it sits in the EMF package). If you moved this folder, pass
`-DEMF_SDK_DIR=<path to sdk/include>` to the first command (or set it in `CMakePresets.json`, with forward slashes:
`"EMF_SDK_DIR": "D:/EMF/sdk/include"`; a single backslash is not valid JSON).

## Try it

Copy the folder `build/release/mymod/` into `<game>\emf\mods\` (or configure with
`-DEMF_GAME_DIR="<your Enshrouded folder>"` and every build copies it there; close the game first, a running game
keeps the DLL locked). Start the game, load a world: your position shows in the bottom left corner. Open the console
(the key under Esc) and type `mymod.hello`; the console's Mods tab shows the mod and its settings, which apply at
once. F8 hides and shows the position (change the key in the Mods tab).
`<game>\emf\emf_core.log` shows `[mymod] loaded`, or why it did not load.

## mod.json

`mod.json` describes the mod (name, version, authors, description), the EMF API version it needs, other mods it needs
with the versions it works with (`"depends": {"other_mod": "^1.0"}`; `{"version": "^1.0", "optional": true}` for one it
can do without) and its settings (shown in the Mods tab and in EMF.exe, stored in `<game>\emf\config\<id>.ini`). The
template's settings show every kind: a switch, a list (`select`), a colour, text with a length limit, a slider with a
step and a key bind, under two headings (`group`), the position ones greyed out while "Show the position" is off
(`enabledWhen`). The build copies it next to the DLL and sets its `"id"` from `project()`.
`docs/modding/reference.md` lists every field; `docs/modding/guide.md` explains version ranges.

## Rename it

Change the name in `project(mymod ...)` at the top of `CMakeLists.txt` (1-48 of a-z 0-9 _ . -; CMake stops with a
message for any other name, which EMF would not load). It becomes the mod's
id everywhere: the DLL and folder name, the `"id"` in the built `mod.json`, and `MOD_ID` in the code (the log prefix,
the settings, the command `<id>.hello` and the HUD line). Edit the name, description and settings in `mod.json`.
(Renaming `src/mymod.cpp` is optional; then change it in `add_library` too.)

Text you show with `EMF_NOTIFY`, `EMF_REPLY` or `EMF_LOG` is a printf format: show text that came from a player or the
game as `EMF_NOTIFY("%s", text)`, never `EMF_NOTIFY(text)` (a `%` in it would read garbage or crash).

## Next

- `docs/modding/guide.md`: worlds, threads, components, events, items, saves: the rules behind the API.
- `docs/modding/cookbook.md`: short recipes (spawn, items, events, menus, villagers, per-world files).
- `docs/modding/reference.md`: every function.
