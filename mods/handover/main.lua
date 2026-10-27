-- handover (Hand-over): give items to the player next to you. "/handover wood 20" in the game's chat hands 20 wood
-- logs to the nearest other player within reach; "/handover.to Alice wood 20" or "/handover.to #2 wood" picks one
-- ("/handover.who" numbers the players near you). Runs where the world runs (the host or a dedicated server): players
-- who joined need no EMF. The host also gets a wheel on a key: a player, then a stack, then how many.
--
-- Items are never lost: each stack moves with emf.move (Lua host 1.6), which takes exactly that stack from the giver
-- (all or nothing), puts it into the receiver, gives back what did not fit and drops what still does not fit beside the
-- giver (the receiver, then the nearest player, if the giver has gone). emf.move finishes in the Lua host even if this
-- script errors or reloads meanwhile, and it does nothing with an answer from another session. Worn equipment is never
-- offered; gear (a stack with its own item data: level, perks, gem) is refused unless allow_gear is on: the game would
-- make a fresh copy without its gems and perks. A player can refuse hand-overs with /handover.block.
-- Commands: /handover, /handover.to, /handover.who, /handover.block (every player); handover.log,
-- handover.debug_target (admins).

local DEFAULT_RANGE = 5       -- metres, when the setting is not there (yet)
local LOG_KEEP = 20           -- hand-overs kept for handover.log
local MAX_COUNT = 65535       -- the most one request may move
local SKIP_CATEGORY = {[1] = true, [2] = true}  -- inventories not offered: customization (1), worn equipment (2)

local history = {}            -- the last hand-overs, newest last (text lines)
local last_at = {}            -- giver key -> world tick of their last hand-over (the cooldown)
local debug_target = nil      -- an entity (villager, chest) that receives every hand-over while set
local pending = {}            -- wheel picks waiting for the next server step
local wheel_state = nil       -- the host's wheel in progress {receiver, receiver_name, items}
local blocked = {}            -- account text -> true: refuses hand-overs (saved with the world)
local session = emf.session()

-- ---- small helpers ----
local function setting_number(key, fallback)
  local v = emf.setting(key)
  if type(v) ~= "number" or v <= 0 then return fallback end
  return v
end
local function range() return setting_number("range", DEFAULT_RANGE) end
local function cooldown_ticks() return math.floor((tonumber(emf.setting("cooldown_seconds")) or 0) * 60) end
local function notes_on() return emf.setting("notes") ~= false end
local function gear_allowed() return emf.setting("allow_gear") == true end

local function new_session()
  local s = emf.session()
  if s ~= session then  -- another world (or a reload of it): entity ids mean other things now
    session = s
    debug_target = nil
    pending = {}
    wheel_state = nil
    last_at = {}
  end
end

local function pos(world, id)
  local x, y, z = world:position(id)
  if not x then return nil end
  return {x = x, y = y, z = z}
end
local function dist(a, b) return math.sqrt((a.x - b.x) ^ 2 + (a.y - b.y) ^ 2 + (a.z - b.z) ^ 2) end

local function player_name(world, id)
  local n = world.name and world:name(id)
  if n and n ~= "" then return n end
  return "player #" .. id
end
local function receiver_name(world, id)
  if id == debug_target then return "entity " .. id end
  return player_name(world, id)
end

local function item_name(id)
  local info = emf.item_info(id)
  return info and info.name or ("item " .. id)
end

local function record(line)
  emf.log(line)
  history[#history + 1] = line
  while #history > LOG_KEEP do table.remove(history, 1) end
end

-- A line in the chat for a player (the host of a game you host gets it as a note); false when it could not be sent.
local function tell(world, id, text)
  if not notes_on() or not id or id == 0 then return false end
  if emf.tell then return emf.tell(world, id, "[Hand-over] " .. text) end
  return false
end

-- ---- who refuses hand-overs (per account, saved with the world) ----
emf.on_world_data(function(world, text)
  blocked = {}
  for account in (text or ""):gmatch("block=(0x%x+)") do blocked[account] = true end
end)
local function save_blocked()
  local lines = {}
  for account in pairs(blocked) do lines[#lines + 1] = "block=" .. account end
  table.sort(lines)
  return emf.save_world_data(table.concat(lines, "\n"))
end
local function refuses(world, id)
  if id == debug_target then return false end
  local account = world:account(id)
  return account ~= nil and blocked[account] == true
end

-- ---- the players ----
-- Every player of this world with a position: the entities 1..1023 with PlayerState.
local function players(world)
  local out = {}
  for id = 1, 1023 do
    if world:get("PlayerState", id) then
      local p = pos(world, id)
      if p then out[#out + 1] = {id = id, pos = p} end
    end
  end
  return out
end

-- The players within reach of `giver` (never the giver), nearest first, numbered as /handover.who lists them.
local function near(world, giver)
  local here = pos(world, giver)
  if not here then return {}, nil end
  local reach = range()
  local list = {}
  for _, p in ipairs(players(world)) do
    if p.id ~= giver then
      local d = dist(here, p.pos)
      if d <= reach then list[#list + 1] = {id = p.id, d = d} end
    end
  end
  table.sort(list, function(a, b) if a.d ~= b.d then return a.d < b.d end return a.id < b.id end)
  return list, here
end

-- ---- what someone carries ----
-- Every non-empty stack of the entity's own Inventory and of the inventories its InventorySetup links (not its worn
-- equipment or customization): {item, count, pide, slot}. slot = the stack's InventorySlotId (the inventory entity in
-- the low 32 bits, the slot index above), the way emf/inventory.hpp's slot_id makes it: emf.move takes exactly there.
local function stacks(world, id)
  local out = {}
  local function add(inv_id)
    local inv = world:get("Inventory", inv_id)
    if not inv then return end
    for s = 1, 8 do
      local st = inv.slots[s]
      local item = st.id
      if item ~= 0 and st.data.count > 0 then
        out[#out + 1] = {item = item, count = st.data.count, pide = st.data.pide, slot = inv_id | ((s - 1) << 32)}
      end
    end
  end
  add(id)
  local setup = world:get("InventorySetup", id)
  if setup then
    for i = 1, 16 do
      local link = setup.linksEntities[i]
      if link ~= 0 and link ~= id and not SKIP_CATEGORY[setup.linksCategories[i]] then add(link) end
    end
  end
  return out
end

-- The giver's items by id: {item, name, total, biggest, pide (a gear stack's item data entity, 0 for plain), stacks}.
local function carried(world, id)
  local by_id, list = {}, {}
  for _, s in ipairs(stacks(world, id)) do
    local e = by_id[s.item]
    if not e then
      e = {item = s.item, name = item_name(s.item), total = 0, biggest = 0, pide = 0, stacks = {}}
      by_id[s.item] = e
      list[#list + 1] = e
    end
    e.total = e.total + s.count
    if s.count > e.biggest then e.biggest = s.count end
    if s.pide ~= 0 and e.pide == 0 then e.pide = s.pide end
    e.stacks[#e.stacks + 1] = s
  end
  for _, e in ipairs(list) do table.sort(e.stacks, function(a, b) return a.count > b.count end) end
  table.sort(list, function(a, b) return a.name < b.name end)
  return list
end

-- The item `part` names among what the giver carries: an exact name wins, else the one name that contains it.
-- Returns the entry, or nil and why.
local function match(list, part)
  local q = part:lower()
  if q == "" then return nil, "say which item: /handover <part of its name> [count|all]" end
  local hits = {}
  for _, e in ipairs(list) do
    local n = e.name:lower()
    if n == q then return e end
    if n:find(q, 1, true) then hits[#hits + 1] = e end
  end
  if #hits == 1 then return hits[1] end
  if #hits == 0 then return nil, "you carry nothing named like '" .. part .. "'" end
  local names = {}
  for i = 1, math.min(5, #hits) do names[i] = hits[i].name end
  return nil, "which one? " .. table.concat(names, ", ") .. (#hits > 5 and (" (and " .. (#hits - 5) .. " more)") or "")
end

-- The item part and the count from the words after the command: the last word is the count when it is a number or
-- "all".
local function split_args(words, from)
  local parts = {}
  for i = from, #words do parts[#parts + 1] = words[i] end
  local count = nil
  local last = parts[#parts]
  if last and (last:lower() == "all" or last:match("^%d+$")) and #parts > 1 then
    count = last:lower()
    parts[#parts] = nil
  end
  return table.concat(parts, " "), count
end

local function is_gear(world, e)
  if e.pide ~= 0 then return true end
  local info = emf.item_info(e.item)
  return info ~= nil and info.max_stack <= 1
end

-- How many to hand over: "all", a number, or by default the biggest stack (1 for gear). nil and why when it cannot be.
local function amount(world, e, count)
  local n
  if count == "all" then n = e.total
  elseif count then n = tonumber(count)
  elseif is_gear(world, e) then n = 1
  else n = e.biggest end
  if not n or n < 1 then return nil, "the count must be 1 or more (or all)" end
  if n > e.total then return nil, "you carry only " .. e.total .. " x " .. e.name end
  if e.pide ~= 0 and n > 1 then return nil, "gear goes one piece at a time" end
  return math.min(n, MAX_COUNT)
end

-- The stacks to take n from: the biggest first, each exactly from its slot ({slot, count, pide}).
local function plan(e, n)
  local out, left = {}, n
  for _, s in ipairs(e.stacks) do
    if left <= 0 then break end
    if e.pide == 0 or s.pide ~= 0 then
      local k = math.min(s.count, left)
      out[#out + 1] = {slot = s.slot, count = k, pide = s.pide}
      left = left - k
    end
  end
  return out
end

-- ---- the hand-over itself ----
-- h = {giver, receiver, item, name, count, giver_name, receiver_name, to_player}; results add up over its moves.
local function finish(world, h)
  local moved, returned, dropped, taken = h.moved, h.returned, h.dropped, h.taken
  local lost = taken - moved - returned - dropped
  local text
  if taken == 0 then
    text = "nothing taken (the giver no longer carries them)"
    tell(world, h.giver, string.format("Nothing was handed over: you no longer carry %d x %s", h.count, h.name))
  elseif moved == taken then
    text = "delivered"
    tell(world, h.giver, string.format("Handed %d x %s to %s", moved, h.name, h.receiver_name))
  else
    text = string.format("delivered %d, %d back to the giver", moved, returned)
    if dropped > 0 then text = text .. string.format(", %d dropped", dropped) end
    if lost > 0 then text = text .. string.format(", LOST %d (see the log)", lost) end
    if moved > 0 then
      tell(world, h.giver, string.format("%s had room for %d of %d x %s; the rest came back to you", h.receiver_name, moved,
                                         taken, h.name))
    else
      tell(world, h.giver, string.format("%s had no room for %s; it came back to you", h.receiver_name, h.name))
    end
    if dropped > 0 then
      tell(world, h.giver, string.format("%d x %s did not fit back into your inventory: they were dropped on the ground",
                                         dropped, h.name))
    end
  end
  if moved > 0 and h.to_player then
    tell(world, h.receiver, string.format("%s handed you %d x %s", h.giver_name, moved, h.name))
  end
  if taken > 0 and taken < h.count then text = text .. string.format(" (%d of %d were still there)", taken, h.count) end
  record(string.format("%s (%d) -> %s (%d): %d x %s: %s", h.giver_name, h.giver, h.receiver_name, h.receiver, h.count,
                       h.name, text))
end

-- Starts the moves of a planned hand-over. False when not one could start (the item queue is full).
local function start(world, h, parts, opts)
  h.moved, h.returned, h.dropped, h.taken, h.waiting = 0, 0, 0, 0, 0
  local started = 0
  for _, part in ipairs(parts) do
    local o = {slot = part.slot, exact = true}
    if opts then o.level, o.rarity = opts.level, opts.rarity end
    local ok = emf.move(world, h.giver, h.receiver, h.item, part.count, o, function(w, moved, returned, dropped, taken)
      h.moved, h.returned, h.dropped = h.moved + moved, h.returned + returned, h.dropped + dropped
      h.taken = h.taken + (taken or (moved + returned + dropped))
      h.waiting = h.waiting - 1
      if h.waiting == 0 then finish(w, h) end
    end)
    if ok then
      h.waiting = h.waiting + 1
      started = started + part.count
    else
      h.count = h.count - part.count  -- not started: those items stay where they are
    end
  end
  return started > 0
end

-- Checks everything and starts a hand-over from `giver` to `receiver` of `part`/`count`. Returns the text to say to the
-- giver at once (or nil and why it was refused).
local function hand_over(world, giver, receiver, part, count)
  if refuses(world, receiver) then return nil, receiver_name(world, receiver) .. " does not accept hand-overs" end
  local key = world:account(giver) or ("entity " .. giver)
  local wait = cooldown_ticks()
  local before = last_at[key]
  if wait > 0 and before and world:tick() - before < wait then
    local s = math.ceil((wait - (world:tick() - before)) / 60)
    return nil, "wait " .. s .. " s before the next hand-over"
  end
  local list = carried(world, giver)
  local e, why = match(list, part)
  if not e then return nil, why end
  local warning = nil
  if e.pide ~= 0 then
    if not gear_allowed() then return nil, "gear loses its gems and perks when handed over; use a chest" end
    warning = " (gear: its gems and perks are lost)"
  end
  local n, why2 = amount(world, e, count)
  if not n then return nil, why2 end
  local parts = plan(e, n)
  local opts = nil
  if e.pide ~= 0 and parts[1] then  -- gear: keep the level and rarity of the piece taken (the copy has no gems or perks)
    local st = world:get("ItemState", parts[1].pide)
    if st then
      opts = {}
      if st.itemLevelUi >= 0 and st.itemLevelUi <= 1000 then opts.level = st.itemLevelUi end
      if st.itemRarityUi >= 0 and st.itemRarityUi <= 4 then opts.rarity = st.itemRarityUi end
    end
  end
  local h = {giver = giver, receiver = receiver, item = e.item, name = e.name, count = n,
             giver_name = player_name(world, giver), receiver_name = receiver_name(world, receiver),
             to_player = receiver ~= debug_target and world:get("PlayerState", receiver) ~= nil}
  if not start(world, h, parts, opts) then return nil, "too many hand-overs are under way; try again in a moment" end
  last_at[key] = world:tick()
  if warning then emf.log(string.format("gear handed over by %s: %s%s", h.giver_name, e.name, warning)) end
  local note = debug_target and receiver == debug_target and " [debug target]" or ""
  return string.format("Handing %d x %s to %s%s%s", h.count, e.name, h.receiver_name, warning or "", note)
end

-- ---- commands ----
-- The player who typed the command: a chat sender's entity, else the host. nil and why.
local function giver_of()
  local s = emf.sender()
  if s and s.chat then
    if s.entity == 0 then return nil, "EMF could not tell which player you are; try again in a moment" end
    return s.entity
  end
  local me = emf.local_player()
  if me == 0 then return nil, "there is no player here: type it in the game's chat" end
  return me
end

-- The default receiver: the debug target, else the nearest other player in reach.
local function nearest_receiver(world, giver)
  if debug_target then return debug_target end
  local list = near(world, giver)
  if #list == 0 then return nil, string.format("nobody is within %d m of you", range()) end
  return list[1].id
end

emf.command("handover", "<item> [count|all]: hand items to the nearest player within reach (/handover.who lists them)",
  function(world, args)
    new_session()
    local giver, why = giver_of()
    if not giver then emf.reply(why) return end
    local part, count = split_args(args, 1)
    if part == "" then emf.reply("usage: /handover <part of the item's name> [count|all]") return end
    local receiver, why2 = nearest_receiver(world, giver)
    if not receiver then emf.reply(why2) return end
    local text, why3 = hand_over(world, giver, receiver, part, count)
    emf.reply(text or why3)
  end, {players = true})

emf.command("who", ": the players within reach of you, numbered for /handover.to", function(world, args)
  new_session()
  local giver, why = giver_of()
  if not giver then emf.reply(why) return end
  local list = near(world, giver)
  if debug_target then emf.reply("debug target: entity " .. debug_target .. " receives every hand-over") end
  if #list == 0 then
    emf.reply(string.format("nobody is within %d m of you", range()))
    return
  end
  local parts = {}
  for i, p in ipairs(list) do parts[i] = string.format("#%d %s %d m", i, player_name(world, p.id), math.floor(p.d + 0.5)) end
  emf.reply(table.concat(parts, ", "))
end, {players = true})

emf.command("block", "[on|off]: refuse (or accept again) items others hand you", function(world, args)
  new_session()
  local me, why = giver_of()
  if not me then emf.reply(why) return end
  local account = world:account(me)
  if not account then emf.reply("EMF could not tell which player you are; try again in a moment") return end
  local want = args[1] and args[1]:lower()
  local on
  if want == "on" then on = true elseif want == "off" then on = false elseif want == nil then on = not blocked[account]
  else emf.reply("usage: /handover.block [on|off]") return end
  blocked[account] = on or nil
  local saved = save_blocked()
  if on then
    emf.reply("You no longer accept hand-overs (/handover.block off to accept them again)" ..
              (saved and "" or "; not saved yet: it lasts until the world is reloaded"))
  else
    emf.reply("You accept hand-overs again" .. (saved and "" or "; not saved yet"))
  end
end, {players = true})

-- "#k" (the who-list) or a name (exact, else a unique start, ignoring case) among every player; nil and why.
local function find_player(world, giver, target)
  local list, here = near(world, giver)
  local k = target:match("^#(%d+)$")
  if k then
    local p = list[tonumber(k)]
    if not p then return nil, string.format("no #%s: /handover.who lists the players within %d m", k, range()) end
    return p.id
  end
  local q = target:lower()
  local exact, starts, known = {}, {}, false
  for _, p in ipairs(players(world)) do
    if p.id ~= giver then
      local n = world.name and world:name(p.id)
      if n and n ~= "" then
        known = true
        local l = n:lower()
        if l == q then exact[#exact + 1] = p end
        if l:sub(1, #q) == q then starts[#starts + 1] = p end
      end
    end
  end
  if not known then return nil, "player names are not known here: use /handover.who and /handover.to #<number>" end
  if #exact > 1 then return nil, "more than one player is named '" .. target .. "': use /handover.who and #<number>" end
  local p = exact[1] or (#starts == 1 and starts[1]) or nil
  if not p then
    if #starts > 1 then return nil, "more than one player's name starts with '" .. target .. "': use /handover.who and #<number>" end
    return nil, "no player named '" .. target .. "'"
  end
  if here and dist(here, p.pos) > range() then
    return nil, string.format("%s is %d m away (reach %d m)", player_name(world, p.id), math.floor(dist(here, p.pos) + 0.5), range())
  end
  return p.id
end

emf.command("to", "<name|#k> <item> [count|all]: hand items to that player (within reach)", function(world, args)
  new_session()
  local giver, why = giver_of()
  if not giver then emf.reply(why) return end
  if #args < 2 then emf.reply("usage: /handover.to <name|#number> <part of the item's name> [count|all]") return end
  local receiver, why2 = find_player(world, giver, args[1])
  if not receiver then emf.reply(why2) return end
  local part, count = split_args(args, 2)
  local text, why3 = hand_over(world, giver, receiver, part, count)
  emf.reply(text or why3)
end, {players = true})

emf.command("log", ": the last hand-overs", function(world, args)
  if #history == 0 then emf.reply("no hand-overs yet") return end
  for _, line in ipairs(history) do emf.reply(line) end
end)

emf.command("debug_target", "<entity|off>: (tests) every hand-over goes to this villager or chest", function(world, args)
  new_session()
  local a = args[1]
  if not a then
    emf.reply(debug_target and ("debug target: entity " .. debug_target) or "no debug target (usage: handover.debug_target <entity|off>)")
    return
  end
  if a == "off" then
    debug_target = nil
    emf.reply("debug target off: hand-overs go to players again")
    return
  end
  local id = tonumber(a)
  if not id or id < 1 then emf.reply("usage: handover.debug_target <entity|off>") return end
  if not world:get("InventorySetup", id) and not world:get("Inventory", id) then
    emf.reply("entity " .. id .. " has no inventory")
    return
  end
  debug_target = id
  emf.reply("debug target: entity " .. id .. " receives every hand-over until handover.debug_target off")
end)

-- ---- the host's wheel: a player, then a stack, then how many (runs on the server step after the last pick) ----
local function amounts_for(e)
  local out = {}
  local function add(label, n) if n >= 1 and n <= e.total then out[#out + 1] = {label = label, n = n} end end
  if e.pide ~= 0 then
    add("1", 1)
    return out
  end
  add("1", 1)
  if e.total >= 10 then add("10", 10) end
  if e.total >= 4 then add(string.format("Half (%d)", e.total // 2), e.total // 2) end
  if e.total > 1 then add(string.format("All (%d)", e.total), e.total) end
  return out
end

local function wheel_amount(cw, choice)
  local st = wheel_state
  if not st or not choice or not st.item then wheel_state = nil return end
  local a = st.amounts[choice]
  if a then pending[#pending + 1] = {receiver = st.receiver, item = st.item.name, count = tostring(a.n)} end
  wheel_state = nil
end

local function wheel_item(cw, choice)
  local st = wheel_state
  if not st or not choice then wheel_state = nil return end
  local e = st.items[choice]
  if not e then wheel_state = nil return end
  st.item = e
  st.amounts = amounts_for(e)
  local labels = {}
  for i, a in ipairs(st.amounts) do labels[i] = a.label end
  if not emf.wheel("How many " .. e.name .. "?", labels, "", wheel_amount) then wheel_state = nil end
end

local function wheel_receiver(cw, choice)
  local st = wheel_state
  if not st or not choice then wheel_state = nil return end
  local r = st.receivers[choice]
  if not r then wheel_state = nil return end
  st.receiver = r.id
  local labels = {}
  for i, e in ipairs(st.items) do labels[i] = string.format("%s (x%d)", e.name, e.total) end
  if not emf.wheel("Hand to " .. r.name, labels, "", wheel_item) then wheel_state = nil end
end

emf.on_key("key", function(world)
  if not world:is_server() then
    emf.notify("Hand-over: the wheel works in a world you host")
    return
  end
  new_session()
  local me = emf.local_player()
  if me == 0 then return end
  local receivers = {}
  if debug_target then receivers[#receivers + 1] = {id = debug_target, name = "entity " .. debug_target .. " (debug)"} end
  for _, p in ipairs(near(world, me)) do
    receivers[#receivers + 1] = {id = p.id, name = string.format("%s (%d m)", player_name(world, p.id), math.floor(p.d + 0.5))}
  end
  if #receivers == 0 then
    emf.notify(string.format("Hand-over: nobody within %d m", range()))
    return
  end
  local items = {}
  for _, e in ipairs(carried(world, me)) do
    if (e.pide == 0 or gear_allowed()) and #items < 64 then items[#items + 1] = e end
  end
  if #items == 0 then
    emf.notify("Hand-over: you carry nothing to hand over")
    return
  end
  local labels = {}
  for i, r in ipairs(receivers) do labels[i] = r.name end
  wheel_state = {receivers = receivers, items = items}
  if not emf.wheel("Hand over to", labels, emf.setting("key") or "", wheel_receiver) then wheel_state = nil end
end)

-- The receiver of a wheel pick, checked again when it runs: still the debug target, or still a player within reach.
local function still_receiver(world, me, id)
  if id == debug_target then return true end
  if not world:get("PlayerState", id) then return false, "that player is gone" end
  local here, there = pos(world, me), pos(world, id)
  if not here or not there then return false, "that player is gone" end
  if dist(here, there) > range() then
    return false, string.format("%s is out of reach now (%d m)", player_name(world, id), math.floor(dist(here, there) + 0.5))
  end
  return true
end

-- The wheel's picks run on the next server step (put and take need the server world).
emf.on_tick(function(world)
  if not world:is_server() or #pending == 0 then return end
  new_session()
  local jobs = pending
  pending = {}
  local me = emf.local_player()
  for _, j in ipairs(jobs) do
    if me ~= 0 then
      local ok, why = still_receiver(world, me, j.receiver)
      if not ok then
        emf.notify("Hand-over: " .. why)
      else
        local text, why2 = hand_over(world, me, j.receiver, j.item, j.count)
        emf.notify("Hand-over: " .. (text or why2))
      end
    end
  end
end)
