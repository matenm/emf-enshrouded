-- chat_command: two commands any player may type into the game's chat (an EMF example mod in Lua).
--   /chat_command.where   where you stand
--   /chat_command.home    how far the nearest flame altar is ("home all": every altar, for admins)
-- A command answers whoever typed it: emf.sender() says who that is. Lua commands always carry the mod's id in front
-- ("chat_command.where"); copy the mod under a short id (say "w") to type "/w.where".

-- What the game's chat shows of one command's answer: 10 lines (an 11th becomes "... (more in the server log)"), each
-- at most 1 KB. A player who is no admin may also send at most 5 commands in 10 seconds. Long answers stay short here.
local CHAT_LINES = 10

-- Whose command is this? Returns their player entity, or nil and what to answer instead.
local function asker()
  local who = emf.sender()  -- {chat, entity, account, admin}; chat false: the console or the host's own chat
  if who and who.chat then
    if who.entity == 0 then return nil, "EMF could not tell which player you are" end
    return who.entity
  end
  local me = emf.local_player()  -- the host; 0 on a dedicated server, which has no local player
  if me == 0 then return nil, "The console has no player here: ask from the game's chat" end
  return me
end

local function round(v) return math.floor(v + 0.5) end

emf.command("where", ": where you stand", function(world, args)
  local id, why = asker()
  if not id then emf.reply(why) return end
  local x, y, z = world:position(id)
  if not x then emf.reply("Your position is not known yet") return end
  emf.reply(string.format("You are at %d, %d, %d", round(x), round(y), round(z)))
  local who = emf.sender()
  if who and who.chat then
    emf.reply("asked by player " .. id .. " in the chat " .. (who.admin and "(an admin: every command is yours)"
      or "(you may run the commands for players: help lists them)"))
  else
    emf.reply("asked by the host (the console or their own chat)")
  end
end, {players = true})  -- every player may run it: it only reads, and answers about its sender

-- Every flame altar: each base has one entity with a BaseIdComponent, standing at the altar. The component does not
-- say whose base it is, so "nearest" means anyone's. Looking them up walks every entity of the world, and players may
-- ask often: the list is kept for 5 seconds (and read again at once in a new session, where ids name other entities).
local found, found_at, found_session = {}, nil, nil
local function all_altars(world)
  local t = world:tick()  -- 60 a second (0 on a game build EMF does not know yet: then it looks every time)
  if found_at and t ~= 0 and found_session == emf.session() and t - found_at < 300 then return found end
  found, found_at, found_session = {}, t, emf.session()
  for _, id in ipairs(world:entities()) do
    if world:get("BaseIdComponent", id) then
      local x, y, z = world:position(id)
      if x then found[#found + 1] = {id = id, x = x, y = y, z = z} end
    end
  end
  return found
end

-- The altars, nearest first from `from` (the distance on the ground).
local function altars(world, from)
  local list = {}
  for _, a in ipairs(all_altars(world)) do
    list[#list + 1] = {id = a.id, x = a.x, y = a.y, z = a.z, d = math.sqrt((a.x - from.x) ^ 2 + (a.z - from.z) ^ 2)}
  end
  table.sort(list, function(a, b) return a.d < b.d end)
  return list
end

emf.command("home", "[all]: how far the nearest flame altar is (all: every altar, for admins)", function(world, args)
  local id, why = asker()
  if not id then emf.reply(why) return end
  local x, y, z = world:position(id)
  if not x then emf.reply("Your position is not known yet") return end
  local list = altars(world, {x = x, y = y, z = z})
  if #list == 0 then emf.reply("No flame altar in this world yet") return end
  if args[1] == "all" then
    local who = emf.sender()
    if not (who and who.admin) then  -- fails closed: no sender known, no list
      emf.reply("Only admins may list every flame altar: the list says where every base is, other players' too")
      return
    end
    emf.reply((#list == 1 and "1 flame altar" or #list .. " flame altars") .. ", nearest first:")
    local room = CHAT_LINES - 1                     -- the first line is used
    if #list > room then room = room - 1 end        -- keep one for "... and N more"
    for i = 1, math.min(#list, room) do
      local a = list[i]
      emf.reply(string.format("%d m: entity %d at %d, %d, %d", round(a.d), a.id, round(a.x), round(a.y), round(a.z)))
    end
    if #list > room then emf.reply("... and " .. (#list - room) .. " more (a chat answer shows " .. CHAT_LINES .. " lines)") end
    return
  end
  local near = list[1]
  local text = "The nearest flame altar (anyone's) is " .. round(near.d) .. " m away"
  local up = round(near.y - y)
  if up >= 3 then text = text .. ", " .. up .. " m above you" end
  if up <= -3 then text = text .. ", " .. -up .. " m below you" end
  emf.reply(text .. " (" .. #list .. " in this world)")
end, {players = true})
