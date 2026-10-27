-- grave (Grave Keeper): find your tombstone, and bring its items back from a distance.
--   /grave.where  where your tombstone is: distance, direction, how many stacks (every player, also from the chat)
--   /grave.claim  its stacks come to you, within the claim range (0 = from anywhere), after a cooldown, at a cost
--   grave.list, grave.adopt (host / admins)
-- Runs where the world runs (the host or a dedicated server); players who join need nothing.
--
-- Whose tombstone is it? The game's tombstone carries no owner a mod can read (in game it has CreateItemLoot,
-- DestroyOnLoot, a LifeTime, an InventorySetup and a CustomPlayerString whose meaning is unknown). So Grave Keeper notes
-- where each player dies, and a tombstone that APPEARS within MATCH_RADIUS of that spot after the death (one that was
-- not there at the death) becomes that player's, by account. Only those can be claimed: never one that was there
-- before, never one far from your death, never one about as near to another player's death. Tombstones from before
-- Grave Keeper ran have no owner: the host or an admin gives one with grave.adopt.
-- The claim: stacks in the tombstone's own Inventory (one loaded from a save keeps them there) come with the game's own
-- loot event {guest = you, host = the tombstone, lootAll, bypassInteraction} (seen in game), gear with its gems and perks;
-- what does not fit stays in the tombstone. A fresh tombstone keeps its stacks in linked inventories, which that event
-- does not reach (seen in game): each plain stack is taken out and put into you, and what does not fit is dropped at
-- your feet; gear stays there (a take would lose its gems and perks).
--
-- The world is searched for tombstones a slice at a time (SCAN_PER_STEP entities per server step), never all at once:
-- a busy server can hold far more entities than the ~22,000 near the start.

emf.emits({"LootInteractionEvent"})

local MATCH_RADIUS = 10   -- m: a tombstone appearing this close to where you died, after it, is yours
local RESPAWN_GRACE = 60  -- s after the player is back up: a tombstone that has not appeared by then is not coming
local MAX_WAIT = 1800     -- s: the longest a death waits (a player who left while on the death screen)
local SAME_PLACE = 1.5    -- m: a known tombstone is found again here (ids change after a reload)
local GONE_RADIUS = 50    -- m: with a player this close and no tombstone at its place, it is gone (looted, expired)
local ADOPT_RADIUS = 10   -- m: grave.adopt takes the nearest tombstone this close
local CLAIM_WAIT = 5      -- s: the stacks come within this; after it the claim is over
local AMBIGUOUS = 4       -- m: a tombstone less than this nearer to one player's death than to another's is nobody's
local LOAD_GRACE = 30     -- s after a world loads: places may still be streaming in, so no tombstone counts as gone
local GONE_AFTER = 10     -- s: missed this long (fresh looks, a player near its place): the tombstone is gone
local SAVE_EVERY = 30     -- s: the play clock (cooldowns) is saved this often
local SCAN_PER_STEP = 1000  -- entities looked at per server step while the world is searched
local SCAN_GAP = 2        -- s between the end of one search and the start of the next (a death starts one at once)
local HUD_RESCAN = 30     -- s: the host's line asks for a new search this often when its tombstone's id is not known

-- Which way is north (the champion mod's calibration, x/z of the world): change both lines if the map disagrees.
local NORTH = {x = 0, z = 1}
local EAST = {x = 1, z = 0}

local graves = {}      -- account -> list of {x, y, z, id} (the tombstone's place; id = its entity this session), oldest first
local claimed = {}     -- account -> play second of their last claim (the cooldown's start)
local waiting = {}     -- deaths whose tombstone has not appeared yet: {account, player, x, y, z, at, before, up_at, bound}
local claims = {}      -- account -> {player, tomb, place, stacks, got, until_s, paid, total}
local play_s = 0       -- seconds this world has run while Grave Keeper ran (the clock of the cooldowns)
local last_tick = nil
local loaded = false
local dirty = false
local saved_at = 0
local session = emf.session()
local hud_shown = false
local client_steps = 0
local hud_at = -1
local loaded_at = 0      -- play_s when this world's data came
local miss_since = {}    -- place -> play second of the first fresh miss with a player near it
local ambiguous = {}     -- tombstone ids already noted as nobody's
local retired = {}       -- places of tombstones dropped from a list (keep) this session: never bound again
local scan = nil         -- the search under way: {ids, i, out, started}
local found, found_at = nil, -1e9  -- the last complete search: its tombstones, and when it started
local want_scan = false  -- someone needs a new search (a death, a command, the host's line)
local scan_after = 0     -- no new search before this play second

-- ---- small helpers ----
local function dist(a, b) return math.sqrt((a.x - b.x) ^ 2 + (a.y - b.y) ^ 2 + (a.z - b.z) ^ 2) end
local function round(v) return math.floor(v + 0.5) end

local function pos(world, id)
  local x, y, z = world:position(id)
  if not x then return nil end
  return {x = x, y = y, z = z}
end

local function compass(from, to)
  local dx, dz = to.x - from.x, to.z - from.z
  local n = dx * NORTH.x + dz * NORTH.z
  local e = dx * EAST.x + dz * EAST.z
  local names = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"}
  local a = math.deg(math.atan(e, n)) % 360
  return names[math.floor((a + 22.5) / 45) % 8 + 1]
end

local function stacks_text(n)
  return n == 1 and "1 stack" or (n .. " stacks")
end

-- true / false, or nil when the component could not be read (a game update renamed it): callers skip then.
local function has(world, component, id)
  local ok, value = pcall(world.get, world, component, id)
  if not ok then return nil end
  return value ~= nil
end

local function count_stacks(inv)
  local n, items = 0, 0
  for i = 1, #inv.slots do
    local s = inv.slots[i]
    if s.id ~= 0 and s.data.count > 0 then
      n = n + 1
      items = items + s.data.count
    end
  end
  return n, items
end

-- The stacks of an entity (and how many items in all): its own Inventory and the inventories its InventorySetup links
-- (a tombstone right after a death has no Inventory of its own: its stacks sit in linked inventories, as in a chest).
-- nil when it has neither.
local function stack_count(world, id)
  local okI, inv = pcall(world.get, world, "Inventory", id)
  local okS, setup = pcall(world.get, world, "InventorySetup", id)
  inv = okI and inv or nil
  setup = okS and setup or nil
  if not inv and not setup then return nil end
  local n, items = 0, 0
  if inv then n, items = count_stacks(inv) end
  if setup then
    for i = 1, #setup.linksEntities do
      local link = setup.linksEntities[i]
      if link ~= 0 and link ~= id then
        local ok, linked = pcall(world.get, world, "Inventory", link)
        if ok and linked then
          local a, b = count_stacks(linked)
          n, items = n + a, items + b
        end
      end
    end
  end
  return n, items
end

-- A tombstone as the game leaves one: CreateItemLoot and DestroyOnLoot, an Inventory (one loaded from a save, seen in game)
-- or an InventorySetup with linked inventories (a fresh one, seen in game), no Health (a creature's corpse), no
-- PlayerState, no PickupItem (a loose drop). Players (ids 1-1023) never.
local function tombstone_rest(world, id)  -- the checks after CreateItemLoot
  return has(world, "DestroyOnLoot", id) == true and
         (has(world, "Inventory", id) == true or has(world, "InventorySetup", id) == true) and
         has(world, "Health", id) == false and has(world, "PlayerState", id) == false and
         has(world, "PickupItem", id) == false
end
local function is_tombstone(world, id)
  if id >= 1 and id <= 1023 then return false end
  return has(world, "CreateItemLoot", id) == true and tombstone_rest(world, id)
end

-- One slice of the search for tombstones (SCAN_PER_STEP entities); true when this slice finished it.
local function scan_step(world)
  if not scan then scan = {ids = world:entities(), i = 1, out = {}, started = play_s} end
  local last = math.min(#scan.ids, scan.i + SCAN_PER_STEP - 1)
  for k = scan.i, last do
    local id = scan.ids[k]
    local ok, loot = pcall(world.get, world, "CreateItemLoot", id)  -- rare: the other reads only for those that have it
    if ok and loot and id > 1023 and tombstone_rest(world, id) then
      local p = pos(world, id)
      if p then
        p.id = id
        p.stacks = stack_count(world, id) or 0
        scan.out[#scan.out + 1] = p
      end
    end
  end
  scan.i = last + 1
  if scan.i <= #scan.ids then return false end
  found, found_at, scan = scan.out, scan.started, nil
  scan_after = play_s + SCAN_GAP
  return true
end

-- A listed tombstone is still there (it may have been looted since the list was made): a few reads, no search.
local function still_there(world, t)
  if not t.id or not is_tombstone(world, t.id) then return false end
  local p = pos(world, t.id)
  if not p or dist(p, t) > SAME_PLACE then return false end
  t.stacks = stack_count(world, t.id) or 0
  return true
end

-- The tombstone at a known grave's place, or nil: its remembered id first (a few reads), else the last search.
local function tomb_at(world, g)
  if g.id then
    local t = {id = g.id, x = g.x, y = g.y, z = g.z}
    if still_there(world, t) then return t end
    g.id = nil
  end
  for _, t in ipairs(found or {}) do
    if dist(t, g) <= SAME_PLACE and still_there(world, t) then
      g.id = t.id
      return t
    end
  end
  return nil
end

-- A tombstone someone owns (or that was dropped from a list this session): by its id when the record knows it (this
-- session), else by its place (after a reload ids change).
local function bound_anywhere(t)
  local function same(g)
    if g.id and t.id then return g.id == t.id end
    return dist(g, t) <= SAME_PLACE
  end
  for _, r in ipairs(retired) do
    if same(r) then return true end
  end
  for _, list in pairs(graves) do
    for _, g in ipairs(list) do
      if same(g) then return true end
    end
  end
  return false
end

-- The players in this world (host and guests): {id, x, y, z}.
local function players_now(world)
  local out = {}
  for id = 1, 1023 do
    if world:account(id) and world:get("PlayerState", id) then
      local p = pos(world, id)
      if p then
        p.id = id
        out[#out + 1] = p
      end
    end
  end
  return out
end

local function someone_near(world, place)
  for _, p in ipairs(players_now(world)) do
    if dist(p, place) <= GONE_RADIUS then return true end
  end
  return false
end

-- Drops items at `first`, else at `second`, else at the nearest player; never silently (a give needs the entity to
-- still be there: a player who left, a tombstone the game removed).
local function drop(world, first, second, item, count, near)
  if first and first ~= 0 and emf.give(world, first, item, count) ~= 0 then return true end
  if second and second ~= 0 and emf.give(world, second, item, count) ~= 0 then
    emf.log(string.format("%d x item %d dropped at %d (%d is gone)", count, item, second, first or 0))
    return true
  end
  local list = players_now(world)  -- the nearest player who is still here
  table.sort(list, function(a, b) return near and dist(a, near) < dist(b, near) end)
  for _, p in ipairs(list) do
    if p.id ~= first and emf.give(world, p.id, item, count) ~= 0 then
      emf.log(string.format("%d x item %d dropped at player %d (%d and %d are gone)", count, item, p.id, first or 0, second or 0))
      return true
    end
  end
  emf.log(string.format("LOST %d x item %d: there was nobody to drop them at (please report)", count, item))
  return false
end

-- ---- saving ----
local function serialize()
  local lines = {string.format("play %d", math.floor(play_s))}
  local keys = {}
  for k in pairs(graves) do keys[#keys + 1] = k end
  table.sort(keys)
  for _, k in ipairs(keys) do
    for _, g in ipairs(graves[k]) do lines[#lines + 1] = string.format("grave %s %.2f %.2f %.2f", k, g.x, g.y, g.z) end
  end
  keys = {}
  for k, at in pairs(claimed) do
    if play_s - at < 3600 then keys[#keys + 1] = k end  -- the longest cooldown is 60 minutes
  end
  table.sort(keys)
  for _, k in ipairs(keys) do lines[#lines + 1] = string.format("claimed %s %d", k, math.floor(claimed[k])) end
  return table.concat(lines, "\n") .. "\n"
end

local function save()
  if not loaded then return end
  if emf.save_world_data(serialize()) then
    dirty = false
    saved_at = play_s
  end
end

emf.on_world_data(function(world, text)
  graves, claimed, waiting, claims = {}, {}, {}, {}
  play_s = tonumber(text:match("play (%d+)")) or 0
  for account, x, y, z in text:gmatch("grave (%S+) (%S+) (%S+) (%S+)") do
    x, y, z = tonumber(x), tonumber(y), tonumber(z)
    if x and y and z then
      graves[account] = graves[account] or {}
      table.insert(graves[account], {x = x, y = y, z = z})
    end
  end
  for account, s in text:gmatch("claimed (%S+) (%d+)") do claimed[account] = tonumber(s) end
  loaded = true
  session = emf.session()
  saved_at, loaded_at, hud_at = play_s, play_s, -1
  miss_since, ambiguous, retired = {}, {}, {}
  scan, found, found_at, scan_after = nil, nil, -1e9, 0
  want_scan = next(graves) ~= nil
end)

local function keep() return math.max(1, emf.setting("keep") or 3) end

local function add_grave(account, t)
  local list = graves[account] or {}
  table.insert(list, {x = t.x, y = t.y, z = t.z, id = t.id})
  while #list > keep() do retired[#retired + 1] = table.remove(list, 1) end
  graves[account] = list
  dirty = true
  save()
end

local function drop_grave(account, place)
  local list = graves[account]
  if not list then return end
  for i = #list, 1, -1 do
    if dist(list[i], place) <= SAME_PLACE then table.remove(list, i) end
  end
  if #list == 0 then graves[account] = nil end
  dirty = true
  save()
end

-- A new hosted session (another world, a reload): ids of the old one mean other things now, and the old world's data
-- is not this one's: nothing acts until this world's data has come (on_world_data).
local function same_session()
  if session == emf.session() then return end
  session = emf.session()
  waiting, claims, retired = {}, {}, {}
  loaded = false
  scan, found, found_at = nil, nil, -1e9
  emf.hud("track", "")
  hud_shown = false
end

-- ---- what a player has ----
local function place_key(g) return string.format("%.1f:%.1f:%.1f", g.x, g.y, g.z) end

-- The player's known tombstones, newest first: {place, tomb (nil when not found now), gone}. A tombstone is gone (looted,
-- or the game removed it) when it was missed for GONE_AFTER seconds by searches that began after the first miss, while
-- a player is near its place (so the place is loaded), never in the first LOAD_GRACE seconds after a load; gone ones are
-- forgotten. A miss asks for a new search.
local function mine(world, account)
  local out = {}
  local list = graves[account]
  if not list then return out end
  for i = #list, 1, -1 do
    local g = list[i]
    local k = place_key(g)
    local t = tomb_at(world, g)
    if t then
      miss_since[k] = nil
      out[#out + 1] = {place = g, tomb = t}
    elseif someone_near(world, g) and play_s - loaded_at >= LOAD_GRACE then
      want_scan = true
      miss_since[k] = miss_since[k] or play_s
      -- gone only when a search that began after the first miss missed it too
      out[#out + 1] = {place = g, gone = play_s - miss_since[k] >= GONE_AFTER and found_at >= miss_since[k]}
    else
      if not found or play_s - found_at > HUD_RESCAN then want_scan = true end
      miss_since[k] = nil
      out[#out + 1] = {place = g}  -- that place is not loaded now (or not searched yet): the record stays
    end
  end
  local kept = {}
  for _, m in ipairs(out) do
    if m.gone then drop_grave(account, m.place) else kept[#kept + 1] = m end
  end
  return kept
end

local function count_item(world, player, item)
  local n = 0
  local function add(id)
    local ok, inv = pcall(world.get, world, "Inventory", id)
    if not ok or not inv then return end
    for i = 1, #inv.slots do
      local s = inv.slots[i]
      if s.id == item then n = n + s.data.count end
    end
  end
  add(player)
  local ok, setup = pcall(world.get, world, "InventorySetup", player)
  if ok and setup then
    for i = 1, #setup.linksEntities do
      local id = setup.linksEntities[i]
      if id ~= 0 and id ~= player then add(id) end
    end
  end
  return n
end

-- The player a command means: a player in the chat, else the host.
local function sender(world)
  local who = emf.sender()
  local id = (who and who.chat) and who.entity or emf.local_player()
  if not id or id == 0 then return nil end
  local p = pos(world, id)
  local account = world:account(id)
  if not p or not account then return nil end
  return {id = id, pos = p, account = account, admin = (not who) or (not who.chat) or who.admin}
end

-- Commands answer only once this world's data has come (after a load the old world's data means nothing here).
local function ready_for_commands()
  if loaded then return true end
  emf.reply("Grave Keeper: this world's data is not loaded yet; try again in a moment")
  return false
end

local function range_text()
  local r = emf.setting("claim_radius") or 150
  return r == 0 and "from anywhere" or ("within " .. r .. " m")
end

local function no_grave_text(account)
  for _, w in ipairs(waiting) do
    if w.account == account then
      return string.format("No tombstone of yours is known yet: you died %d s ago and the game has not left one there yet.",
        math.floor(play_s - w.at))
    end
  end
  return "No tombstone of yours is known (only deaths while Grave Keeper runs count; the host can give an older one with grave.adopt)."
end

local function where_text(me, m, older)
  local d = round(dist(me.pos, m.place))
  local text = string.format("Your tombstone: %d m %s", d, compass(me.pos, m.place))
  local dy = m.place.y - me.pos.y
  if math.abs(dy) >= 5 then text = text .. string.format(", %d m %s", round(math.abs(dy)), dy > 0 and "above" or "below") end
  if m.tomb then text = text .. ", " .. stacks_text(m.tomb.stacks) else text = text .. " (not found there yet: the place may not be loaded)" end
  if older > 0 then text = text .. string.format(" (and %d older)", older) end
  if emf.setting("claim") then
    text = text .. ". /grave.claim brings them back (" .. range_text() .. ")."
  else
    text = text .. "."
  end
  return text
end

-- ---- the HUD (the host's own tombstone) ----
local function show(world)
  local me = emf.local_player()
  local account = me and me ~= 0 and world:account(me)
  local p = me and me ~= 0 and pos(world, me)
  if not emf.setting("hud") or not account or not p then
    if hud_shown then emf.hud("track", "") end
    hud_shown = false
    return
  end
  local list = mine(world, account)
  if #list == 0 then
    if hud_shown then emf.hud("track", "") end
    hud_shown = false
    return
  end
  local m = list[1]
  local text = string.format("Tombstone: %d m %s", round(dist(p, m.place)), compass(p, m.place))
  if m.tomb then text = text .. " (" .. stacks_text(m.tomb.stacks) .. ")" end
  emf.hud("track", text, emf.setting("corner"), emf.setting("color"))
  hud_shown = true
end

-- ---- deaths ----
emf.on_event("EntityDiedEvent", function(world, e)
  same_session()
  local id = e.deadEntityId
  if id < 1 or id > 1023 or not world:get("PlayerState", id) then return end
  local account = world:account(id)
  local p = pos(world, id)
  if not account or not p then
    emf.log("player " .. id .. " died, but EMF could not tell who or where: not tracked")
    return
  end
  -- `before` (the tombstones already near the spot) comes from the first search that begins after the death
  waiting[#waiting + 1] = {account = account, player = id, x = p.x, y = p.y, z = p.z, at = play_s}
  scan = nil  -- a search under way began before the death: start again
  want_scan, scan_after = true, 0
  emf.log(string.format("player %d (%s) died at (%.1f, %.1f, %.1f): looking for the tombstone", id, account, p.x, p.y, p.z))
end, "server")

-- Back up after a death: health again (a respawn, or a revive).
local function is_up(world, player)
  local ok, h = pcall(world.get, world, "NetworkHealth", player)
  if ok and h then return h.health > 0 end
  local okH, health = pcall(world.get, world, "Health", player)
  return okH and health ~= nil and health.dataStorage[1] > 0
end

-- Deaths waiting for their tombstone, once a search completed: the deaths that have no `before` yet get it (the
-- tombstones near their spot then); each new tombstone (not there at that death, owned by nobody) goes to the waiting
-- death nearest to it within MATCH_RADIUS; one about as near to the deaths of two different players (less than
-- AMBIGUOUS between them) is nobody's: grave.adopt hands it over.
local function bind_found(world)
  for _, w in ipairs(waiting) do
    if not w.before and found_at >= w.at then
      w.before = {}
      local seen = {}
      for _, t in ipairs(found) do
        if dist(t, w) <= MATCH_RADIUS then
          w.before[t.id] = true
          seen[#seen + 1] = string.format("%d at %.1f m", t.id, dist(t, w))
        end
      end
      if #seen > 0 then emf.log("tombstones near that spot at the death: " .. table.concat(seen, ", ")) end
    end
  end
  for _, t in ipairs(found) do
    if not bound_anywhere(t) then
      local best, best_d = nil, math.huge
      local rival_d = math.huge  -- the nearest death of another player
      local cands = {}
      for _, w in ipairs(waiting) do
        if not w.bound and w.before and not w.before[t.id] then
          local d = dist(t, w)
          if d <= MATCH_RADIUS then
            cands[#cands + 1] = {w = w, d = d}
            if d < best_d then best, best_d = w, d end
          end
        end
      end
      for _, c in ipairs(cands) do
        if best and c.w.account ~= best.account and c.d < rival_d then rival_d = c.d end
      end
      if best and rival_d - best_d < AMBIGUOUS then
        if not ambiguous[t.id] then
          ambiguous[t.id] = true
          emf.log(string.format("tombstone %d at (%.1f, %.1f, %.1f) is about as near to two players' deaths: nobody's (grave.adopt hands it over)",
            t.id, t.x, t.y, t.z))
        end
      elseif best then
        best.bound = true
        add_grave(best.account, t)
        emf.log(string.format("tombstone %d (%s) at (%.1f, %.1f, %.1f), %.1f m from the death, is %s's", t.id,
          stacks_text(t.stacks), t.x, t.y, t.z, best_d, best.account))
      end
    end
  end
end

-- Deaths stop waiting once found, or when their player has been back up for RESPAWN_GRACE seconds (the game leaves the
-- tombstone at the respawn: seen in game), or after MAX_WAIT (a player who left while dead).
local function age_waiting(world)
  for i = #waiting, 1, -1 do
    local w = waiting[i]
    if not w.bound and not w.up_at and is_up(world, w.player) then w.up_at = play_s end
    local over = (w.up_at and play_s - w.up_at >= RESPAWN_GRACE) or play_s - w.at >= MAX_WAIT
    if w.bound then
      table.remove(waiting, i)
    elseif over then
      emf.log(string.format("no tombstone appeared for %s's death at (%.1f, %.1f, %.1f)", w.account, w.x, w.y, w.z))
      table.remove(waiting, i)
    end
  end
  if #waiting > 0 then want_scan = true end
end

-- ---- claims ----
emf.on_event("PickupEvent", function(world, e)
  for _, c in pairs(claims) do
    if e.pickedUpByEntityId == c.player and e.pickedUpFromEntityId == c.tomb then c.got = c.got + 1 end
  end
end, "server")

local function finish_claims(world)
  for account, c in pairs(claims) do
    if c.paying and play_s >= c.until_s then  -- the cost's take never answered: nothing was claimed
      claims[account] = nil
      emf.log(string.format("claim: the cost's take for player %d (%s) never answered: nothing was claimed", c.player, account))
    elseif play_s >= c.until_s then
      claims[account] = nil
      -- what left the tombstone: its items now against those when the claim was sent (a PickupEvent naming it is not
      -- always reported, so the tombstone itself decides)
      local stacks, items = stack_count(world, c.tomb)
      local moved = not stacks or not still_there(world, {id = c.tomb, x = c.place.x, y = c.place.y, z = c.place.z}) or
                    items ~= c.total
      local text
      if moved then
        local came = c.got > 0 and stacks_text(c.got) or "its stacks"
        text = string.format("Grave Keeper: %s back from your tombstone", came)
        if stacks and stacks > 0 and items ~= c.total then
          text = text .. string.format("; %s stay in it", stacks_text(stacks))
        else
          drop_grave(account, c.place)  -- emptied: the game removes it
        end
      else
        claimed[account] = nil  -- nothing left the tombstone: no cooldown
        dirty = true
        text = "Grave Keeper: nothing came from your tombstone (is your inventory full?)"
        if c.paid then  -- the cost goes back; what does not fit is dropped at the player's feet, never lost
          local player, item, count, tomb = c.player, c.paid.item, c.paid.count, c.tomb
          local queued = emf.put(world, player, item, count, function(w, added)
            if added < count then drop(w, player, tomb, item, count - added, c.place) end
          end)
          if not queued then drop(world, player, tomb, item, count, c.place) end
          text = text .. "; the cost was given back"
        end
      end
      emf.log(text .. string.format(" (player %d, %s)", c.player, account))
      if c.player == emf.local_player() then emf.notify(text) end
      save()
    end
  end
end

-- What a tombstone holds, by how it can be claimed: stacks in its own Inventory (the game's loot event takes those, gear
-- with its gems and perks), plain stacks in the inventories its InventorySetup links (a fresh tombstone keeps everything
-- there, and the loot event does not reach them: seen in game) and gear among those (a take would lose its data).
local function tomb_parts(world, id)
  local parts = {own = 0, linked = {}, gear = 0}
  local okI, inv = pcall(world.get, world, "Inventory", id)
  if okI and inv then parts.own = count_stacks(inv) end
  local okS, setup = pcall(world.get, world, "InventorySetup", id)
  if okS and setup then
    for i = 1, #setup.linksEntities do
      local link = setup.linksEntities[i]
      if link ~= 0 and link ~= id then
        local ok, linked = pcall(world.get, world, "Inventory", link)
        if ok and linked then
          for k = 1, #linked.slots do
            local st = linked.slots[k]
            if st.id ~= 0 and st.data.count > 0 then
              if st.data.pide ~= 0 then
                parts.gear = parts.gear + 1
              else
                parts.linked[#parts.linked + 1] = {item = st.id, count = st.data.count}
              end
            end
          end
        end
      end
    end
  end
  parts.movable = parts.own + #parts.linked
  return parts
end

local function gear_text(n)
  if n == 0 then return "" end
  return n == 1 and " (1 piece of gear stays: take it from the tombstone by hand)"
                or string.format(" (%d pieces of gear stay: take them from the tombstone by hand)", n)
end

-- The claim: the game's loot event for the tombstone's own Inventory (seen in game), and a take out of the tombstone and a
-- put into the player for each plain stack of its linked inventories; what does not fit is dropped at the player's feet
-- (else back at the tombstone, else at the nearest player). The cooldown starts now.
local function send_claim(world, account, player, tomb, paid)
  local parts = tomb_parts(world, tomb.id)
  local _, total = stack_count(world, tomb.id)
  local c = {player = player, tomb = tomb.id, place = {x = tomb.x, y = tomb.y, z = tomb.z}, stacks = parts.movable, got = 0,
             until_s = play_s + CLAIM_WAIT, paid = paid, total = total or 0}
  claims[account] = c
  claimed[account] = play_s
  dirty = true
  if parts.own > 0 then
    emf.emit("server", "LootInteractionEvent", {guestId = player, hostId = tomb.id, offerId = 0, lootAll = true,
                                                bypassInteraction = true})
  end
  for _, st in ipairs(parts.linked) do
    local item, count, from = st.item, st.count, tomb.id
    emf.take(world, from, item, count, function(w, taken)
      if taken <= 0 then return end
      c.got = c.got + 1
      local queued = emf.put(w, player, item, taken, function(w2, added)
        if added < taken then drop(w2, player, from, item, taken - added, c.place) end
      end)
      if not queued then drop(w, player, from, item, taken, c.place) end
    end)
  end
  emf.log(string.format("claim: tombstone %d (%d stack(s) in its own inventory, %d linked, %d gear) into player %d (%s)", tomb.id,
    parts.own, #parts.linked, parts.gear, player, account))
  return parts
end

emf.command("where", ": where your tombstone is", function(world, args)
  same_session()
  if not ready_for_commands() then return end
  local me = sender(world)
  if not me then
    emf.reply("Grave Keeper: EMF could not tell which player you are; try again in a moment")
    return
  end
  local list = mine(world, me.account)
  if #list == 0 then
    emf.reply(no_grave_text(me.account))
    return
  end
  emf.reply(where_text(me, list[1], #list - 1))
end, {players = true})

emf.command("claim", ": your tombstone's items come to you", function(world, args)
  same_session()
  if not ready_for_commands() then return end
  local me = sender(world)
  if not me then
    emf.reply("Grave Keeper: EMF could not tell which player you are; try again in a moment")
    return
  end
  if not emf.setting("claim") then
    emf.reply("Claiming tombstones is switched off here (grave.where still finds yours).")
    return
  end
  if claims[me.account] then
    emf.reply("Your tombstone is being claimed: wait a moment.")
    return
  end
  local list = mine(world, me.account)
  local m = nil
  for _, x in ipairs(list) do
    if x.tomb then
      m = x
      break
    end
  end
  if not m then
    if #list > 0 then
      emf.reply(string.format("Your tombstone is %d m away and was not found there yet (the place may not be loaded, or the "
        .. "world is being searched): try again in a moment, or come closer.", round(dist(me.pos, list[1].place))))
    else
      emf.reply(no_grave_text(me.account))
    end
    return
  end
  local d = round(dist(me.pos, m.tomb))
  local radius = emf.setting("claim_radius") or 150
  local parts = tomb_parts(world, m.tomb.id)
  if parts.movable == 0 then
    emf.reply("Your tombstone holds only gear" .. (parts.gear > 0 and ": take it from the tombstone by hand." or "."))
    return
  end
  if radius > 0 and d > radius then
    emf.reply(string.format("Your tombstone is %d m away: come within %d m to claim it.", d, radius))
    return
  end
  -- the wait follows the setting as it is now (lowering it shortens a cooldown that already runs)
  local wait = claimed[me.account] and (emf.setting("claim_cooldown_minutes") or 0) * 60 - (play_s - claimed[me.account]) or 0
  if wait > 0 then
    emf.reply(string.format("You can claim again in %d min.", math.ceil(wait / 60)))
    return
  end
  local cost_name = emf.setting("claim_cost_item") or ""
  local cost_count = emf.setting("claim_cost_count") or 0
  if cost_name ~= "" and cost_count > 0 then
    local item = emf.item(cost_name)
    if not item then
      emf.reply("Grave Keeper: the cost item " .. cost_name .. " is not an item of the game (ask the host)")
      return
    end
    local have = count_item(world, me.id, item)
    if have < cost_count then
      emf.reply(string.format("Claiming costs %d x %s; you have %d.", cost_count, cost_name, have))
      return
    end
    local account, player, tomb = me.account, me.id, m.tomb
    claims[account] = {player = player, tomb = tomb.id, place = {x = tomb.x, y = tomb.y, z = tomb.z}, stacks = tomb.stacks,
                       got = 0, until_s = play_s + 60, paying = true}  -- paying first
    local queued = emf.take(world, player, item, cost_count, function(w, taken)
      if taken < cost_count then
        claims[account] = nil
        if taken > 0 then drop(w, player, 0, item, taken) end
        emf.log(string.format("claim: the cost could not be taken from player %d (%s)", player, account))
        if player == emf.local_player() then emf.notify("Grave Keeper: the cost could not be taken; nothing was claimed") end
        return
      end
      send_claim(w, account, player, tomb, {item = item, count = cost_count})
    end)
    if not queued then
      claims[account] = nil
      emf.reply("Grave Keeper: the cost could not be taken right now; try again")
      return
    end
    emf.reply(string.format("Claiming your tombstone %d m away for %d x %s: its %s come to you%s.", d, cost_count, cost_name,
      stacks_text(parts.movable), gear_text(parts.gear)))
    return
  end
  parts = send_claim(world, me.account, me.id, m.tomb, nil)
  emf.reply(string.format("Claiming your tombstone %d m away: its %s come to you%s.", d, stacks_text(parts.movable),
    gear_text(parts.gear)))
end, {players = true})

emf.command("list", ": every known tombstone and its owner (host)", function(world, args)
  same_session()
  if not ready_for_commands() then return end
  local me = sender(world)
  local n = 0
  local keys = {}
  for k in pairs(graves) do keys[#keys + 1] = k end
  table.sort(keys)
  for _, account in ipairs(keys) do
    for _, g in ipairs(graves[account]) do
      local t = tomb_at(world, g)
      local where = me and string.format("%d m from you", round(dist(me.pos, g))) or
                    string.format("at (%.0f, %.0f, %.0f)", g.x, g.y, g.z)
      emf.reply(string.format("%s: %s, %s", account, where, t and stacks_text(t.stacks) or "not found there now"))
      n = n + 1
    end
  end
  if n == 0 then emf.reply("No tombstones are known yet.") end
  if #waiting > 0 then emf.reply(#waiting .. " death(s) wait for their tombstone.") end
end)

emf.command("adopt", ": the nearest tombstone within 10 m becomes yours (host; for tombstones from before Grave Keeper ran)",
  function(world, args)
  same_session()
  if not ready_for_commands() then return end
  local me = sender(world)
  if not me then
    emf.reply("Grave Keeper: EMF could not tell which player you are")
    return
  end
  local best, best_d = nil, ADOPT_RADIUS
  for _, t in ipairs(found or {}) do
    local d = dist(t, me.pos)
    if d <= best_d and still_there(world, t) then best, best_d = t, d end
  end
  if not best then
    want_scan = true
    emf.reply("No tombstone within " .. ADOPT_RADIUS .. " m of you (as last searched; the world is searched again now).")
    return
  end
  for account, list in pairs(graves) do  -- it changes owner
    for i = #list, 1, -1 do
      if dist(list[i], best) <= SAME_PLACE then table.remove(list, i) end
    end
    if #list == 0 then graves[account] = nil end
  end
  add_grave(me.account, best)
  emf.reply(string.format("Tombstone %d (%d m from you, %s) is yours now: /grave.claim brings it back.", best.id,
    round(best_d), stacks_text(best.stacks)))
end)

-- ---- every step ----
emf.on_tick(function(world)
  if world:is_client() then  -- in someone else's game: the line goes
    client_steps = client_steps + 1
    if client_steps > 180 and hud_shown then
      emf.hud("track", "")
      hud_shown = false
    end
    return
  end
  if not world:is_server() then return end
  client_steps = 0
  same_session()
  local t = world:tick()
  if last_tick and t > last_tick and t - last_tick < 600 then play_s = play_s + (t - last_tick) / 60 end
  last_tick = t
  if not loaded then return end
  age_waiting(world)
  if scan or (want_scan and play_s >= scan_after) then  -- one slice of the search per step
    want_scan = false
    if scan_step(world) then bind_found(world) end
  end
  finish_claims(world)
  if play_s - saved_at >= SAVE_EVERY or dirty then save() end
  local s = math.floor(play_s)
  if s ~= hud_at then  -- the host's line, once a second
    hud_at = s
    show(world)
  end
end)

emf.on_setting_changed(function(key) hud_at = -1 end)
