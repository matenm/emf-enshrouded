# Modding Enshrouded with EMF: start here

EMF (the Enshrouded Modding Framework) loads your own DLLs into the game and gives them a C API (with a C++ layer)
to read and change the running game: entities and their components, the game's events, items and inventories,
spawning, villagers, the player's camera, a radial menu, on-screen notes, console commands and more. It works on
the **host** (the player who runs the world, alone or hosting); a client of someone else's game can read but not
change gameplay.

**New to modding? Start with [first_mod.md](first_mod.md):** your first mod in 10 minutes, in Lua, with no compiler.
Copy the `template_lua` folder from the EMF zip into the game, see it on screen, change it while the game runs, and
give it a setting that players change in the Mods tab.

This folder is the modder's documentation:

| Read | For |
|---|---|
| [first_mod.md](first_mod.md) | **start here**: your first mod in 10 minutes (Lua, the `template_lua` starter) |
| this page | install EMF, build and load your first C++ mod (about 15 minutes) |
| [lua.md](lua.md) | **Lua mods**: every `emf.*` function; no compiler, live reload |
| [guide.md](guide.md) | how the game and EMF work: worlds, threads, components, templates, events, programs, items, saves |
| [cookbook.md](cookbook.md) | short complete recipes for common tasks (spawn, items, events, villagers, crops, menus, saves...) |
| [reference.md](reference.md) | every API function, its rules and failure modes; the SDK headers; the console commands |
| [content.md](content.md) | **new items** with an EML data patch (the Emberbrand example), what works and what does not |
| [server.md](server.md) | **dedicated server**: install EMF there, players' `/commands` from chat, who may |
| [other_mods.md](other_mods.md) | EMF next to **other mods and loaders**: renamed `dinput8.dll` mods, the chain files, notes for DLL mod authors |
| [updating.md](updating.md) | what happens when the game updates, and how to check your mod still works |
| [debug_bridge.md](debug_bridge.md) | **test from a script**: run commands, read the state, wait, take screenshots (off by default) |
| [ai_agents.md](ai_agents.md) | **tools for AI coding assistants**: an MCP server, JSON test scenarios, a game session script; [llms.txt](llms.txt) is the whole API in one file for an assistant's context |

The repository's [README_EMF.md](../../README_EMF.md) describes the shipped mods (companion, villager jobs, big
stacks...), which are also full examples of everything below.

## Examples

Small mods that each teach one part of the API, short enough to read in a few minutes. They come in the EMF zip's
`examples\` folder and are not installed: copy a Lua example's folder into `<game>\emf\mods\` (it needs the `lua` mod,
which is on by default), or build the C++ one with its own preset, as the template. Each folder's `README.md` says what
it teaches and what to try. In a Lua example's `mod.json`, `depends` names the Lua host it needs and `"emf"` the API
version that host needs: `"lua": ">=1.4"` with `"emf": 17`, `"lua": ">=1.3"` with `"emf": 14`.

| Example | Kind | Teaches |
|---|---|---|
| [settings_tour](../../examples/lua/settings_tour/) | Lua | every kind of setting (switch, number boxes, sliders, `choice`, `select`, colour, text with `maxLength`, key bind), `group`, `enabledWhen`, live or next start; `emf.setting`, `emf.on_setting_changed`, `emf.set_setting` |
| [chat_command](../../examples/lua/chat_command/) | Lua | commands every player may run from the game's chat (`{players = true}`), answering the sender (`emf.sender()` and its `admin`), the chat's reply limits, finding flame altars |
| [hud_widget](../../examples/lua/hud_widget/) | Lua | a tidy HUD widget: lines sent only when they change, corner and colour from settings, `emf.time_of_day` |
| [hotkey_wheel](../../examples/lua/hotkey_wheel/) | Lua | a key bind (`emf.on_key`), the radial wheel (`emf.wheel`), an action for the host only, a console command run through `emf\commands.txt` |
| [pet_follow](../../examples/lua/pet_follow/) | Lua | `emf.spawn` and `emf.destroy`, writing a component (`Faction`), sending a game event (`StartGuardingEvent`), sessions |
| [loot_ticker](../../examples/lua/loot_ticker/) | Lua | a game event (`PickupEvent`), item names, fading HUD lines |
| [waypoints](../../examples/cpp/waypoints/) | C++ | commands with arguments, a save per world (`emf::WorldFile`), a HUD line from the camera, logic kept apart and unit-tested; its own CMake preset |
| [hello](../../examples/hello/hello.cpp) | C++ | the smallest mod: a command and `on_tick` (section 2 below) |

The shipped mods are bigger examples. Their sources are in the zip's `mods\` folder; the Lua ones are installed, so
read them in `<game>\emf\mods\<id>\`:

| Mod | Kind | Teaches |
|---|---|---|
| `kill_counter` | Lua | game events (`HitEvent`, `EntityDiedEvent`), a HUD line, a save per world, a command |
| `life_skills` | Lua | data per player by account (`world:account`), putting items into an inventory, a command for players |
| `champion` (Wandering Champion) | Lua | a whole gameplay mod: spawns, events, timers, sessions, rewards (the big example) |
| [event_log](../../mods/event_log/) | C++ | `on_event` for many engine events (it only logs them) |
| [sim_rate](../../mods/sim_rate/) | C++ | a pattern hook (`hook_pattern`) with a `.patterns` file that `emf_sigcheck` checks |
| [clock](../../mods/clock/) | C++ | a HUD line from a world singleton (`g38_daytime::Daytime`) with live settings |

## 1. Install EMF

**From the release zip** (players and modders): extract `EMF-<version>.zip` into a folder, close the game and
double-click `install.cmd`. It finds the game through Steam (or drag the game folder, the one with `enshrouded.exe`,
onto `install.cmd`), copies EMF and the included mods next to `enshrouded.exe`, and never overwrites your settings
in `emf\config`, so installing a newer EMF over an older one keeps them. `uninstall.cmd` removes EMF again
(`uninstall.cmd -All` also removes the settings; it refuses while the companion holds gear you gave it). The zip also
holds everything for modders: `sdk\include`, the C++ mod template (`template\`), the Lua starter (`template_lua\`),
these docs, the sources of the included mods (`mods\`) and `tools\emf_sigcheck.exe`.

In the game, the key under Esc opens EMF's console. Its **Mods** tab (click it, or Ctrl+Tab) lists the mods, switches
them on or off (from the next game start) and changes their settings; `settings <mod>` and `set <mod> <key> <value>`
do the same by typing. No settings files are shipped: mods start with the defaults from their `mod.json`, and a change
is saved in `<game>\emf\config\<mod>.ini`.

**From source** (working on EMF itself): you need Windows, the game (Steam), Visual Studio 2022 (or its Build Tools)
with the C++ workload, and CMake.

```
cmd /c scripts\dev.cmd cmake --preset msvc          (once)
cmd /c scripts\dev.cmd cmake --build --preset msvc  (builds the core, the mods and the tests)
powershell -ExecutionPolicy Bypass -File scripts\install_emf.ps1 [-EnableWrite]
```

(Other DLL mods that also want the name `dinput8.dll` keep working: [other_mods.md](other_mods.md).)
`install_emf.ps1` copies `dinput8.dll` (the loader) next to `enshrouded.exe` and the core and mods into
`<game>\emf\`. It finds the game through Steam, as the player's installer does; `-Game <folder>` or the environment
variable `EMF_GAME_DIR` names another folder. Start the game normally. `<game>\emf\emf_core.log` shows what loaded:

```
emf_core 0.3.0 starting (game dir C:\SteamLibrary\steamapps\common\Enshrouded)
game sha256 d64966e2... (known build 1273043)
signatures: 38 of 38 usable
...
[mymod] loaded
mod C:\...\emf\mods\mymod\mymod.dll: loaded
6 mod(s) loaded
```

(The last three lines are what your own mod adds once it is built and installed.)

On a game build EMF does not know, the "signatures" line says how many of the engine functions it found; see
[updating.md](updating.md).

`scripts\uninstall_probe.ps1` removes everything again (it archives the logs first). The log is rewritten at every
game start: copy it if you want to keep it.

**Saves:** EMF itself never writes your saves, but the game autosaves the world every 30 s, and anything a mod
changes in a world is saved with it. Try mods in a separate local world, and back up
`%USERPROFILE%\Saved Games\keen games\Enshrouded\` first.

## 2. Your first mod

A mod is a 64-bit DLL that exports one function, `emf_mod_load`. The SDK is header-only: add `sdk/include` to your
include path; nothing to link.

**The quick way: the template.** Copy the package's `template\` folder (keep it next to `sdk\`, or pass
`-DEMF_SDK_DIR=<path to sdk\include>`), open the folder in Visual Studio 2022 (File > Open > Folder), pick
`Release (x64)` and build: you get the folder `build\release\mymod\` (`mymod.dll` and its `mod.json`), a working mod
with a console command (`mymod.hello`), your position in a corner, a key that hides it and one setting of each kind
(a switch, a list, a colour, text, a slider and a key bind) in the Mods tab. Configure with
`-DEMF_GAME_DIR="<your Enshrouded folder>"` and every build copies it into `<game>\emf\mods\mymod\`.
`template\README.md` says how to rename it.

The rest of this section shows the same ideas step by step.

`examples/hello/hello.cpp` (built with the repository, so it always compiles):

```cpp
#include <windows.h>  // GetTickCount64

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"

namespace gen = emf::gen::ecs;

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;        // refuses (and says why in the log) if the core is too old

    // A console command: type "hello.where" into the in-game console (the key under Esc) or emf\commands.txt.
    emf::add_command("hello.where", ": where am I?", [](emf::World w, const std::vector<std::string>&) {
        auto* t = w.get<gen::CurrentTransform>(emf::local_player());
        if (!t) {
            EMF_REPLY("no player yet");
            return;
        }
        EMF_REPLY("you are at %.1f, %.1f, %.1f m", t->transform.position.x / 4294967296.0,
                  t->transform.position.y / 4294967296.0, t->transform.position.z / 4294967296.0);
    });

    // Something every second, on the host's server world.
    emf::on_tick([](emf::World w) {
        static uint64_t last = 0;
        if (!w.is_server() || GetTickCount64() - last < 1000) return;
        last = GetTickCount64();
        // ... read components, spawn, give items ...
    });

    EMF_LOG("hello", "loaded");
    return true;
}
```

Build it as a DLL. Inside this repository, give it a folder `mods/mymod/` with `mymod.cpp` and a `mod.json`
(`{"id": "mymod", "version": "0.1.0", "emf": 13}` is enough; see the reference for names, dependencies and
settings), and add to `CMakeLists.txt`:

```cmake
emf_mod(mymod mods/mymod/mymod.cpp)   # builds dist/emf/mods/mymod/mymod.dll next to its mod.json
```

(Copy `examples/hello/hello.cpp` to `mods/mymod/mymod.cpp` first: the repository already has a target named `hello`,
which it builds into `build\msvc\examples\` so that the example is not installed with the shipped mods. Your
`dist/emf/mods` folder is what `install_emf.ps1` copies into the game.)

Outside it, use the template (above) or any MSVC x64 DLL project: include `sdk/include`, compile as C++17 or newer, and
put the DLL with its `mod.json` into `<game>\emf\mods\<id>\` (a loose DLL in `<game>\emf\mods\` works too, without
settings or dependencies). Mods load when the game starts: dependencies first, then by folder name.

Start the game, load a world, open the console (the key under Esc) and type `hello.where`. `help` lists every
command, including those of other mods.

## 3. Rules you must know

- **Touch the game only inside EMF callbacks** (`on_tick`, `run_on_world`, `on_event`, command callbacks, put/take
  results, wheel choices) and only on the world they pass you. The engine's data is not thread safe; `spawn`,
  `destroy` and `give_item` refuse anywhere else. Component pointers are valid until your callback returns.
- **Gameplay changes happen on the server world** (`w.is_server()`), which exists only on the host.
- A callback of yours that crashes is caught, logged with your DLL's name and disabled; the game keeps running.
  Returning false from `emf_mod_load` does not unload your DLL, so check first, register after.
- Mod ECS programs and event subscriptions must be registered in `emf_mod_load` (before the first world starts).
- Entity ids are reused: after a reload the same id can be another entity. Remember positions, NPC ids, or use
  `emf/storage.hpp`'s per-world file and the session number (`emf::server_session()`).

[guide.md](guide.md) explains why, and [cookbook.md](cookbook.md) shows each of these in context.

## 4. Test your mod from a script

The debug bridge lets a script drive the running game: run console commands and read their output, read the state
(the world, the player's position, the mods), wait for a world or a log line, take a screenshot and press your mod's
hotkey. Switch it on with `[debug] bridge=1` in `emf\emf_core.ini`; only your Windows user can connect. Then:

```powershell
.\scripts\emf_bridge.ps1 wait world 300
.\scripts\emf_bridge.ps1 console "hello.where"
.\scripts\emf_bridge.ps1 shot
```

[debug_bridge.md](debug_bridge.md) has the clients (PowerShell and Python), the protocol and the limits.
[ai_agents.md](ai_agents.md) turns it into tests (a JSON scenario per check, with an exit code) and into tools for an AI
coding assistant (an MCP server).
