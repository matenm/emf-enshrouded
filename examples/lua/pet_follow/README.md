# Pet Follow (Lua example)

`pet_follow.summon`: a rabbit appears 2 m from you and follows you. `pet_follow.dismiss` removes it from the world.

## What it teaches

- **Spawning** with `emf.spawn(world, guid, x, y, z)` (the host's server world, inside a callback) and removing with
  `emf.destroy` (which never removes a player). Spawned creatures are **not saved** with the world: after a reload or a
  restart the pet is gone.
- **Writing a component**: `world:get("Faction", pet)` gives the pet's faction; `f.faction = 11` makes it an ally
  (PlayerAlly). The write is checked (300 into a byte is an error) and only valid inside the callback that got it.
- **Sending one of the game's events**: `emf.emits({"StartGuardingEvent"})` at the top of `main.lua`, then
  `emf.emit("server", "StartGuardingEvent", {guardingEntityId = pet, guardedEntityId = you})`. The game's own guard
  behaviour does the rest: the pet follows and stays near you.
- **Ids are reused**, after a reload and **within a session too**: when the pet dies and its corpse goes, its id can
  soon name a villager, a chest or a wall. The mod forgets the pet when the game reports its death (`EntityDiedEvent`),
  and before it touches the id again it checks it is still the pet: alive (`NetworkHealth`), and still an ally or still
  guarding you. If not, it forgets the id for good. After a reload (`emf.session()`) it forgets every pet.
- **Checking on it**: every 5 seconds it makes the pet an ally again if the game set its faction back, sends the guard
  event again if it lost you (`DynamicEnemy01BehaviorState.guardEntityId`), and removes the pet of a player who left.

The rabbit is `Enemy_Companion_Bunny`, which runs the enemy AI (the one that obeys `StartGuardingEvent`). A wild
rabbit runs the animal AI and would only sit there. Other templates are in `export/template_guids.csv`, which the
`guid_dump` EML mod writes; `inspect <entity>` in the console shows what a creature has.

The mod keeps its pets only in memory: saving `main.lua` reloads the mod and it forgets them. Those rabbits stay in the
world, following nobody's commands, until the world loads again (they are not saved).

## Install

Copy this folder into `<game>\emf\mods\` (so you have `<game>\emf\mods\pet_follow\main.lua`) and start the game. It
needs EMF's `lua` mod, which the EMF installer puts in and switches on.

## Try it

1. Load a world you host, open the console (the key under Esc) and type `pet_follow.summon`: a rabbit appears next to
   you, and the console says its entity id.
2. Walk and run: it follows you and stays near you. Type `pet_follow.summon` again: you already have one.
3. `pet_follow.dismiss`: it is removed.
4. Summon one, then load the world again: it is gone, and `pet_follow.dismiss` says you have no pet.
5. Admins can run the commands from the game's chat too (`[chat] admins`, `docs/modding/server.md`): each player gets
   their own pet, next to them.
