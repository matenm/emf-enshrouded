EMF - Enshrouded Modding Framework
==================================

FOR PLAYERS
-----------
1. Extract this zip into a folder first (right-click > Extract All...). The scripts do not work from inside the zip.
2. Close Enshrouded.
3. Double-click install.cmd. It finds the game through Steam and copies EMF and its mods next to enshrouded.exe.
   Game somewhere Steam does not know? Drag the game folder (the one with enshrouded.exe) onto install.cmd.
4. Start the game normally. The key under Esc opens the EMF console; type  help  to see every command.
5. Optional: emf\EMF.exe in the game folder is EMF's mod manager: every installed mod with switches and settings,
   clash warnings, installing mods you downloaded and save backups. Windows may say "Windows protected your PC" the
   first time (EMF.exe is not signed): click "More info", then "Run anyway". You can also do without it: switches and
   settings are in the game's console too.

Switching mods on or off, and their settings: open the console and click its Mods tab (or press Ctrl+Tab). Pick a
mod to switch it on or off (from the next game start) and to change its settings. Or type  set <mod> <key> <value>
in the console ( settings <mod>  lists the keys;  set big_stacks enabled 1  switches a mod on). No settings files
come with EMF: every mod starts with its defaults, and your changes are saved in <game>\emf\config\<mod>.ini.

Included mods:
  companion      G: a companion that follows you, fights, takes orders (stay, attack) and wears gear you give it
  villager_jobs  K: villagers farm, tend animals, haul between chests and craft at a chest
  quick_stack    L: put what you carry into nearby chests that already hold it
  clock          the time of day in a corner of the screen
  kill_counter   counts the creatures you kill (a Lua mod; to change it, copy its folder under a new id first:
                 installing EMF again replaces emf\mods\kill_counter)
  life_skills    woodcutting, mining, foraging, hunting and fishing level up as you gather (a Lua mod)
  lua            runs the Lua mods (kill_counter, life_skills, beastmaster, ...)
  big_stacks     bigger item stacks (OFF by default: the game saves the bigger stacks; switch it on in the Mods tab
                 and restart the game)
  loot_scale     more materials per loot stack (OFF by default; switch it on in the Mods tab and restart the game;
                 its factor, 2 by default, changes at once)
  handover       /handover wood 20 gives items to the nearest other player; nothing is lost (a Lua mod; OFF by
                 default; /handover.block refuses them)
  herald         Server Herald, for dedicated servers: message of the day, join lines, announcements, restart
                 countdown, chat log, playtime, Discord webhook (OFF by default; see mods\herald\README.md)
  teleports      Waypoint Teleports: /unstuck and /home for every player, tp and bring for admins (OFF by default;
                 switch it on in the Mods tab and restart the game)
  bulk_craft     craft one by hand, then U at the station (or /craft 25) crafts many through the game's own
                 crafting, at the recipe's pace, from what you carry (OFF by default; see mods\bulk_craft\README.md)
  beastmaster    tame wild beasts and call them to fight beside you; hold B for the beast wheel, /beastmaster for
                 every player (a Lua mod; OFF by default; the bear is an EML pack, content\emf_beastmaster_bear, when
                 this download has it; see game\emf\mods\beastmaster\README.md)
  necromancer    raise skeleton warriors and cast raging spirits; hold V for the summoning wheel (shared with
                 Beastmaster), /necromancer for every player (a Lua mod; OFF by default; see
                 game\emf\mods\necromancer\README.md)

Other mods and loaders (Creative Mode, Auto Loot Plus, XHL mods, EML, ShroudForge, Shroudweave, ...) keep working:
  - Mods that use their own file name (EML's dbghelp.dll, Shroudweave's winmm.dll, Greeneye's dbgcore.dll, Aldrich
    Mod Menu's POWRPROF.dll) simply run next to EMF.
  - A mod that also wants the name dinput8.dll (Creative Mode, Auto Loot Plus, XHL-Core, ...) is renamed by
    install.cmd (for example to dinput8-previous.dll) and listed in dinput8-chain.ini; EMF starts it during the
    game's start-up, as it would have started on its own. install.cmd says what it moved. (ShroudForge's dinput8.dll
    is kept aside but not started: ShroudForge already runs through its dbghelp.dll.)
  - Installing such a mod AFTER EMF overwrites EMF's dinput8.dll: run install.cmd again and it moves that mod too.
    A newer copy of a tool EMF recognises switches the older copy off; nothing is ever deleted.
  - uninstall.cmd puts the other mod's dinput8.dll back (the newest, if there were several).
  - A mod that does its work in the game's DirectInput calls rather than when it starts would lose that part; none
    of the known ones does.
  - emf\emf_core.log lists at every start which tool owns each of these files and what EMF started.

Mods change the world on the HOST (your own game, or the game you host).

SAVE BACKUPS: install.cmd backs up your saves (Steam Cloud and Local) before it changes anything: the "Before EMF"
backup. The game backs them up again each time it starts with EMF and a save changed (the newest 10 of those are
kept). They are in %LOCALAPPDATA%\EMF\save-backups. EMF.exe's Saves tab lists them, makes more and restores them;
restoring never removes the backup. In the game's console, at the main menu:  saves.backup [label]  and  saves.list.
To restore by hand (no EMF.exe): close the game, open the backup's folder and copy each file back to the same name:
  steam\<id>\<file>           to  <Steam>\userdata\<id>\1203620\remote\<file>
  local\default\<file>        to  %USERPROFILE%\Saved Games\keen games\Enshrouded\default\<file>
  local\local_settings.json   to  %USERPROFILE%\Saved Games\keen games\Enshrouded\local_settings.json
Copy the files you replace somewhere first, and put nothing else into those folders. For Steam Cloud saves: if Steam
says the files on this PC and in the cloud are in conflict, keep the files on this PC. docs\saves.md has more.

Remove EMF: double-click uninstall.cmd. Your settings stay (emf\config, emf\emf_core.ini), and so do
your mod profiles (emf\profiles). Save backups always stay, also with -All.
To remove the settings too, run  uninstall.cmd -All  in a command prompt in this folder.
Gear you gave the companion exists only in its settings files (emf\config\companion): take it back in game
(G, Gear) before  uninstall.cmd -All  (it refuses while the companion holds gear; -All -Force deletes it anyway).
Installing a newer EMF over an older one keeps your settings.

FOR MODDERS
-----------
docs\modding\first_mod.md  your first mod in 10 minutes (Lua): copy template_lua\, change it, see it in game
template_lua\            the Lua starter: a line on screen, a key, one setting of each kind
docs\modding\lua.md      Lua mods: two text files, no compiler, reloaded while the game runs
docs\modding\server.md   EMF on a dedicated server: install.cmd "<server folder>", players' /commands
docs\modding\README.md   C++ mods: your first mod in about 15 minutes
template\                a ready mod project (CMake, Visual Studio 2022): copy it, rename it, build
sdk\include\             the SDK (header-only C/C++, nothing to link)
examples\hello\          the example from the docs
mods\                    the sources of the included mods
tools\emf_sigcheck.exe   checks EMF's engine signatures against a game exe without starting the game

licenses\ holds the licences of the third-party code inside EMF (MinHook, Dear ImGui, Vulkan headers, Lua).
