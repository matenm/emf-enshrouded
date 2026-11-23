# EMF next to other mods and loaders

Many Enshrouded mods are DLLs that get into the game under the name of a Windows DLL the game loads at start:
`dinput8.dll`, `dbghelp.dll`, `winmm.dll`, `dbgcore.dll`, `POWRPROF.dll`. Only one file can have each name. This page
says what EMF does about that, for players and for authors of such mods.

## What EMF does

- EMF's loader is `dinput8.dll`. Mods under other names run next to it untouched.
- When `install.cmd` finds another mod's `dinput8.dll`, it renames it, lists the new name in the `[EMF]` section of
  `dinput8-chain.ini` and records it in `emf\moved.txt`. `uninstall.cmd` puts it back. The new name is `<tool>.dll` when it recognises the
  tool from `emf\known_tools.txt` (a text inside its DLL, as plain text or UTF-16), else a name from the DLL's version
  information (its product name), else `dinput8-previous.dll`.
- Nothing EMF moves is overwritten or deleted. A newer copy of a recognised tool gets `<tool>-2.dll` and the older copy
  is switched off (`; ` in front of its line). Two unrecognised DLLs both run, and `install.cmd` says so: if the
  second is a newer copy of the first, switch the first off.
- A copy of EMF's own loader listed in the chain file (a player following another tool's chain instructions) does
  nothing at start (one EMF loader per process), and `install.cmd` takes its line out.
- A tool that ships the same loader under two names (ShroudForge: `dbghelp.dll` and `dinput8.dll`) already runs
  through the other one: its `dinput8.dll` is kept as `<tool>.dll` but not listed, so it does not start twice.
- At every start EMF's loader reads `dinput8-chain.ini` next to the exe (`[Chain]`, `[EMF]` and lines before any
  section) and starts each listed DLL in order from its
  own `DllMain`, before it starts EMF's core: the way the community's chain loaders do it and the moment the mod would
  have started as `dinput8.dll` itself (during the game's start-up, before any game code). EMF does not patch the
  game's entry point for this (ShroudForge does, and two patches there would fight).
- A DLL that is missing or fails to load is skipped and reported; the game goes on. A crash in a DLL's own `DllMain`
  comes back from Windows as error 1114 and is reported the same way. A `DllMain` that hangs, or ends the process
  (EML does that for a broken mod), stops the game as it would have without EMF.
- The game's DirectInput calls go to Windows' own `dinput8.dll`, not to a chained mod's exports, unless ShroudForge
  hands EMF's loader a provider (see ShroudForge below): a mod that does its work there (rather than from `DllMain`
  or a thread it starts) loses that part otherwise. None of the known ones does.
- `chainloader.ini` is ModLoader Proxy's own file (with settings such as delayed loading and a mods folder). When
  ModLoader Proxy was the `dinput8.dll`, EMF starts it, and it reads its file as before. EMF can also be one of the
  DLLs ModLoader Proxy starts (ModLoader Proxy as `dinput8.dll`, EMF's loader renamed and listed under `[Load]`).
  EMF reads `chainloader.ini` only to know what runs: when ModLoader Proxy runs (as a proxy DLL, or started by EMF),
  the DLLs it starts (`ForwardTo`, the `[Load]` list, and the `ModsFolder` DLLs with `LoadFromFolder = true`) count
  as running, and `emf\emf_core.log` lists them ("started by ModLoader Proxy (chainloader.ini): ..."). EMF reads the
  `chainloader.ini` next to ModLoader Proxy's DLL (also in a subfolder) and paths relative to it, as ModLoader Proxy
  does; a path outside the game folder (a drive, `..`, a `ModsFolder` elsewhere) is listed but not read, and a device
  name such as `CON` or `COM1` is never opened.
- `emf\emf_loader.log` has one line per chained DLL, `emf\chain_status.txt` the same as a table, and
  `emf\emf_core.log` says at start-up which tool owns each proxy DLL next to the game. A loader that is installed but
  does not run is named with the tool that holds the file it wants ("EML has dbghelp.dll, which both want");
  ShroudForge's `winmm.dll` alone does not count as running it (it helps ShroudForge's loader, which runs as
  `dbghelp.dll` or `dinput8.dll`).
- EMF.exe's **Play without any mods** starts the game once without the chained DLLs (and without EMF); their lines in
  `emf\chain_status.txt` say `skipped` then. **Play without EMF mods** still starts them ([../app.md](../app.md#play)).
- On a dedicated server whose start another tool already holds (ShroudForge patches the entry point too), EMF does not
  hold it as well; `emf_loader.log` says so and names the DLL the entry point jumps into. Mods may then load after the
  server's world setup.

## dinput8-chain.ini

The rules every loader can follow are in [dinput8-chain.md](dinput8-chain.md); this section is what EMF does. Plain
text, one DLL per line, relative to the game folder:

```ini
[Chain]
mods\mymod\mymod.dll

; [EMF]: DLLs EMF renamed, started by EMF only. [Chain]: read by every chain loader.
[EMF]
creative.dll
```

- Lines in `[Chain]`, in `[EMF]` and before any section count for EMF; other sections belong to other tools.
- `[Chain]` is shared: XHL-Core, Auto Loot Plus and ShroudForge read it too (each with its own rules, see below).
  `[EMF]` holds the DLLs EMF renamed (its installer and EMF.exe put them there), and only EMF reads it. An install
  over EMF 0.3.x moves EMF's own lines (those in `emf\moved.txt`) from `[Chain]` to `[EMF]`; a player's lines stay
  where they are. When EMF's last line leaves `[EMF]`, the section and its note go too.
- Blank lines and `;` `#` `//` comments are skipped; a `key=value` line counts with its value when that names a `.dll`
  (other settings are ignored). `; ` in front of a line switches that DLL off.
- Only DLLs inside the game folder: absolute paths, drive letters, UNC paths and `..` are refused. `/` is read as `\`.
- UTF-8 (with or without BOM), the ANSI code page and UTF-16 with BOM are read; the installer writes the file back in
  the encoding it found. At most 256 entries count.
- `dinput8.dll` itself is never chained, and each DLL is listed once.
- XHL-Core and Auto Loot Plus read this file too. A chain loader that EMF starts and that reads the same file starts
  the same DLLs again; Windows then hands back the already loaded DLL, so nothing runs twice.

## ShroudForge

ShroudForge's updater and uninstaller manage `dinput8.dll` as their own file: updating ShroudForge puts its
`dinput8.dll` back over EMF's, and uninstalling ShroudForge deletes EMF's. Run `install.cmd` again afterwards; it
sets the new copy aside again.

ShroudForge 1.7.7 reads `[Chain]` in `dinput8-chain.ini` too, with rules of its own:

- every line must be a DLL inside its own folder under `mods\` (`mods\<folder>\name.dll`); a game-root line shows as
  "Load failed" in ShroudForge's Modloader (F9, Compatibility > DLL Manager);
- it loads those lines through the game folder's `dinput8.dll`, its "dinput8 chain host". EMF's loader offers the same
  contract (the export `ShroudforgeSetDinput8Provider`; the game's `DirectInput8Create` goes to the first DLL with its
  own one, as with ShroudForge's own `dinput8.dll`), so `mods\` lines set up ShroudForge's way load in ShroudForge
  while EMF is `dinput8.dll`. EMF starts them as well; Windows hands the second start the DLL already loaded.

What this means for players with ShroudForge and EMF:

- The DLLs EMF renamed are in `[EMF]`: ShroudForge does not see them, and they show no error there.
- A game-root line a player put in `[Chain]` (as XHL-Core and Auto Loot Plus describe it) shows as failed in
  ShroudForge, but EMF still starts it. Leave it where it is: moving a DLL into `mods\` can break it (Ultimate ASI
  Loader looks for its `.asi` plugins next to itself). `install.cmd` names such lines when it finds ShroudForge.
- Plain EML (and ShroudForge set to "eml") stops with "Errors loading some mods" when a folder in `mods\` has no
  `mod.json`: a DLL folder there needs one, or belongs elsewhere.

## Kiln

Kiln runs EMF mods itself (its `kiln-emf` layer is its own implementation of EMF's C and Lua APIs, see
[hosting.md](hosting.md)), so EMF is not needed next to it for that. What its layer offers depends on Kiln's version;
Kiln's own notes list what is not there yet.

- With Kiln alone, an EMF mod's log lines go to `kiln\logs\kiln.log`, each with an `[emf:<mod id>]` prefix; there is
  no `emf\emf_core.log`.
- With EMF's loader chain-loaded next to Kiln, Kiln's layer steps aside and EMF runs the EMF mods. EMF's whole game
  folder brings its `emf\mods` folder too: every mod there runs as it is set there (switch off the ones you do not want
  in EMF.exe or the in-game Mods list).
- EMF leaves EML content to Kiln: with `kiln\kiln.json` in the game folder it never builds the EML mods itself, since
  Kiln applies them (its EML layer and ShroudForge) and two builds of the game data would undo each other's.
- Never copy EMF's `dinput8.dll` over Kiln's: Kiln would no longer start, and EMF would still leave the EML content to
  it. Follow Kiln's own instructions for running EMF next to it.

## For authors of DLL mods

- Your mod keeps working as `dinput8.dll` without EMF. With EMF it runs under another name: do not depend on your own
  file name, find your files relative to your module (`GetModuleFileNameW(your HMODULE)`), not as `dinput8.dll`.
- You are started from EMF's `DllMain` during the game's start-up, inside the loader lock as before: a `DllMain`
  that only starts a thread (the usual way) behaves the same. Do not wait for anything in `DllMain`.
- To name your tool in EMF's messages, add a line to `emf\known_tools.txt` in a pull request, or put a unique text in
  your DLL (for example your mod's name) and tell us.
- Or write it as an EMF mod: hooks that chain with other mods (`hook_pattern`), game events, settings in EMF's Mods
  tab, and no file-name fight at all ([README.md](README.md)).
