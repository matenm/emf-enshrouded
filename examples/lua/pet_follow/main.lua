-- pet_follow: a rabbit that follows you (an EMF example mod in Lua).
--   pet_follow.summon    a rabbit appears 2 m from you and follows you
--   pet_follow.dismiss   it is removed from the world
-- Three ways to change the world: spawn an entity (emf.spawn), write one of its components (Faction) and send the game
-- one of its own events (StartGuardingEvent: "follow and guard this one"). Host only.
-- Spawned creatures are not saved with the world: after a reload or a restart the pet is gone.
--
-- Entity ids are reused, even within one session: when the pet dies and its corpse goes, its id can soon name a
-- villager, a chest or a wall. So before the mod touches the id again it checks that the id is still its pet: alive,
-- and still an ally or still guarding you. If not, it forgets the id and never touches it again.

-- The creature: Enemy_Companion_Bunny, a rabbit made for companion duty. It runs the enemy AI, which obeys
-- StartGuardingEvent. A wild rabbit (Animal_Wildlife_T2_Bunny_AG2, 7e389a60-f74b-4133-a5aa-68e91623985c) runs the
-- animal AI, which does not. More templates: export/template_guids.csv (the guid_dump EML mod writes it).
local PET = "6b264270-bc49-447d-918a-89ffcb143a06"
local PLAYER_ALLY = 11  -- FactionType 11: on the players' side (enum FactionType in components.hpp)

emf.emits({"StartGuardingEvent"})  -- the events this mod sends, declared at the top of main.lua

local pets = {}                -- player entity -> their pet's entity id
local session = emf.session()  -- entity ids only mean something in the session they came from
local steps = 0

-- A world reloaded or another one loaded: the pets went with it, and their ids now name other entities.
local function same_session()
  if session ~= emf.session() then
    session = emf.session()
    pets = {}
  end
end

-- "You" in a command: a player who typed it in the chat, else the host.
local function owner()
  local who = emf.sender()
  if who and who.chat then return who.entity end
  return emf.local_player()
end

-- Still our pet? Alive (a corpse keeps its place but has 0 health; a chest has no health at all), and still an ally
-- or still guarding its player. Anything else is not ours (any more).
local function still_ours(world, pet, player)
  local hp = world:get("NetworkHealth", pet)
  if not world:position(pet) or not hp or hp.health == 0 then return false, "is gone" end
  local f = world:get("Faction", pet)
  local ai = world:get("DynamicEnemy01BehaviorState", pet)  -- the enemy AI's state: whom it guards now
  if (f and f.faction == PLAYER_ALLY) or (ai and ai.guardEntityId == player) then return true end
  return false, "is not the pet any more"
end

-- Writing a component: change a field of the table world:get gave you, inside the same callback.
local function make_ally(world, pet)
  local f = world:get("Faction", pet)
  if f and f.faction ~= PLAYER_ALLY then f.faction = PLAYER_ALLY end
end

-- Sending an event: the game handles it on its next step, as if one of its own programs had sent it.
local function follow(pet, player)
  emf.emit("server", "StartGuardingEvent", {guardingEntityId = pet, guardedEntityId = player})
end

emf.command("summon", ": a rabbit that follows you", function(world, args)
  same_session()
  local player = owner()
  local x, y, z = world:position(player)
  if not x then emf.reply("Your position is not known") return end
  if pets[player] and still_ours(world, pets[player], player) then
    emf.reply("You already have a pet (pet_follow.dismiss sends it away)")
    return
  end
  local pet = emf.spawn(world, PET, x + 2, y, z)  -- 2 m along x from you, at your height
  if pet == 0 then emf.reply("The game refused the spawn (the log says why)") return end
  pets[player] = pet
  make_ally(world, pet)  -- right away, before its AI picks a target
  follow(pet, player)
  emf.reply("A rabbit follows you (entity " .. pet .. "). Spawned creatures are not saved: it is gone after a reload")
end)

emf.command("dismiss", ": your rabbit is removed from the world", function(world, args)
  same_session()
  local player = owner()
  local pet = pets[player]
  pets[player] = nil
  if not pet or not still_ours(world, pet, player) then emf.reply("You have no pet") return end
  emf.destroy(world, pet)
  emf.reply("Your pet is removed from the world")
end)

-- The game says when something dies. The pet's id is forgotten at once: its corpse goes later, and the id with it.
emf.on_event("EntityDiedEvent", function(world, e)
  same_session()
  for player, pet in pairs(pets) do
    if pet == e.deadEntityId then
      pets[player] = nil
      emf.log("the pet of player " .. player .. " died")
    end
  end
end)

-- Every 5 seconds: is each pet still ours, still an ally, still following? The game may set a faction back, and a
-- guard can lose its target (a fight, a long run). A pet whose player left the game is removed.
emf.on_tick(function(world)
  if not world:is_server() then return end  -- the world the game runs on (yours when you host)
  same_session()
  steps = steps + 1
  if steps % 300 ~= 0 then return end  -- 60 steps a second
  for player, pet in pairs(pets) do
    local ours, why = still_ours(world, pet, player)
    if not ours then
      pets[player] = nil
      emf.log("the pet of player " .. player .. " " .. why)
    elseif not world:position(player) then
      pets[player] = nil
      emf.destroy(world, pet)
      emf.log("player " .. player .. " left: their pet is removed")
    else
      make_ally(world, pet)
      local ai = world:get("DynamicEnemy01BehaviorState", pet)
      if ai and ai.guardEntityId ~= player then follow(pet, player) end
    end
  end
end)
