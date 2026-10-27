# Waypoints (C++ example)

Name the places you find and let a line on screen guide you back to them.

## What it teaches

- **Commands with arguments** (`emf::add_command`): `args[0]` is the command, `args[1]` the first word after it, and
  every command answers with `EMF_REPLY`, even when the input is wrong.
- **A save per world** (`emf::WorldFile` from `emf/storage.hpp`): each world keeps its own waypoints in
  `<game>\emf\config\waypoints\<world slot>.txt`. The file is read once per world load and only ever written into the
  file of the world being played.
- **A HUD line from the camera** (`emf::aim::camera_ray`, `emf::hud`): how far, which way and how much higher.
- **Logic without the game**: `src/waypoint_book.h` holds the book, its text and the guiding line. It has no game in
  it, so the repository's tests check it (`tests/test_waypoints.cpp`); `src/waypoints.cpp` is the thin game side.

## Build and install

You need Visual Studio 2022 (or its Build Tools) with "Desktop development with C++". This folder builds like the
`template\` folder of the EMF package and finds the SDK three folders up (`..\..\..\sdk\include`); pass
`-DEMF_SDK_DIR=<path to sdk\include>` if you moved it.

- **Visual Studio:** File > Open > Folder... > this folder, pick `Release (x64)`, Build > Build All.
- **Command line** (a "x64 Native Tools Command Prompt for VS 2022"): `cmake --preset release`, then
  `cmake --build --preset release`.

Copy the folder `build\release\waypoints\` (`waypoints.dll` and `mod.json`) into `<game>\emf\mods\`, or configure with
`-DEMF_GAME_DIR="<your Enshrouded folder>"` and every build copies it there (close the game first).

## Try it

Start the game and load a world you host. Open the console (the key under Esc):

1. `wp.set home` at your base: `home set at ...`.
2. Walk away and `wp.set cave` somewhere else.
3. `wp.list`: both, with how far each is.
4. `wp.go home`: a line in the top right says `home: 120 m ahead left, 4 m up` and follows you as you turn and walk.
   Within 4 m a note says "Arrived at home" and the line goes. `wp.go` alone stops it.
5. `wp.del cave`, then quit and load the world again: `wp.list` still has `home`. Another world has its own list.

The Mods tab has the line's corner and colour.

**One guess to check:** which side is "right" depends on the engine's handedness, which is not checked in game
(`kCameraXIsRight` in `src/waypoint_book.h`). If the line says "left" for a waypoint on your right, set it to `false`
and build again.

The waypoints are the host's: the commands use the host's position (type them in the console or the host's own chat),
and the line is on the host's screen. Another player who types them in the chat (an admin) is told so
(`emf::command_sender` reports `from_chat`).
`docs/modding/reference.md` lists every function used here.
