-- kill_counter: counts the creatures you kill (an EMF Lua example mod).
-- HitEvent says who hit whom; when something dies and you were the last to hit it, it counts as your kill.
-- The count is kept per world (emf.on_world_data / emf.save_world_data): load the world again and it is still there.
-- Edit this file while the game runs: EMF reloads it within a second.

local last_hitter = {}   -- entity id -> the entity that hit it last
local tracked = 0
local kills = 0

local function show()
  emf.hud("count", "Kills: " .. kills, emf.setting("corner"))
end

local function save()
  emf.save_world_data("kills=" .. kills)   -- false before a world is loaded: nothing to keep yet
end

-- What this world saved last time ("" the first time).
emf.on_world_data(function(world, text)
  kills = tonumber(text:match("kills=(%d+)")) or 0
  show()
end)

emf.on_event("HitEvent", function(world, hit)
  if hit.targetId == 0 then return end
  if last_hitter[hit.targetId] == nil then tracked = tracked + 1 end
  last_hitter[hit.targetId] = hit.rootSourceId
  if tracked > 2000 then          -- forget old fights now and then
    last_hitter, tracked = {}, 0
  end
end)

emf.on_event("EntityDiedEvent", function(world, died)
  local id = died.deadEntityId
  local hitter = last_hitter[id]
  if hitter ~= nil then
    last_hitter[id] = nil
    tracked = tracked - 1
    if hitter == emf.local_player() then
      kills = kills + 1
      show()
      save()
    end
  end
end)

emf.command("reset", ": count from 0 again", function(world, args)
  kills = 0
  show()
  save()
  emf.reply("kill counter reset")
end)

emf.on_setting_changed(function(key) show() end)

show()
