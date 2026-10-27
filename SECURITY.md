# Security

EMF loads code into the game, runs inside dedicated servers, downloads from Nexus Mods and installs files into the game
folder. If you find a way to misuse any of that, please tell us privately first.

## How to report

Use GitHub's **private vulnerability reporting** on this repository (the "Security" tab, then "Report a
vulnerability"). Please include:
- what an attacker can do, and what they need first (a mod the player installs, a file on a Nexus page, a chat line on a
  server, access to the same PC);
- the steps, and the EMF version (`VERSION`, or the first line of `emf\emf_core.log`).

We answer as soon as we can. Please give us time to fix it before you publish.

## What counts

- EMF.exe: installing a downloaded file (archive checks, paths outside the game folder, overwriting other mods' files),
  `nxm://` links, the Nexus account and key storage, the page's lockdown (it must not reach the network or run code
  from mod texts).
- The installers (`install.cmd`, `uninstall.cmd`): files outside the game folder, other mods' files lost.
- The core in a dedicated server: what players can do through chat commands, and the chat lines mods send.
- The debug bridge (off by default): reaching it from another PC or another Windows user.
- Server Herald's Discord webhook: the address leaking into logs or chat.

## What does not count

- What a mod you chose to install can do: EMF mods are native code (or Lua with EMF's API) and run with the game's
  rights. Only install mods you trust.
- Cheating in your own game, or on a server whose owner runs the mods.
- Windows' "unknown publisher" warning: EMF.exe is not code-signed.
