-- bounty (Bounty Board): daily bounties for every player: slay creatures of a faction, gather a material, slay a
-- champion. Rewards with /bounty.claim (or at once, a setting). Kept per world and per player (by account).
--   /bounty.board   your bounties, progress and the time to new ones (every player, also from the chat)
--   /bounty.claim   the rewards of your finished bounties
--   /bounty.reroll  <n>: swap one bounty for another (once a period by default)
--   /bounty.top     who finished the most bounties in this world
--   bounty.give <id>, bounty.reset (host / admins)
-- Runs where the world runs (the host or a dedicated server); players who join need nothing.

-- The bounties. Edit the table and save: the mod reloads (bounties already on players' boards keep what they were).
-- Installing EMF again replaces this file and your edits with it: keep your own pool in emf\config\bounty\pool.lua
-- instead (a file that returns a table like POOL; read when the mod starts and at each world load).
--   kind "kill": a kill credited to the player of a creature whose Faction is `faction` (FactionType in
--     sdk/include/emf/generated/components.hpp: 2 Fogger (the shroud's creatures), 3 Vukah, 4 Scavenger, 5 Wildlife,
--     6 WildBeast, 10 Skeleton, 14 Drak...);
--   kind "gather": items the player picks up in the world (not out of a chest or a tombstone) whose debug name holds
--     `match` (lower case); raw materials only (names starting Material_, Food_ or Build_TerrainMaterial_);
--   kind "champion": a kill of a strong creature: CHAMPION_LEVELS levels above the player or more, with at least
--     CHAMPION_HEALTH health.
-- n: how many on "normal" (easy x0.6, hard x1.5). weight: how often it is drawn (3 when left out). reward: items by
-- their debug name (`items rune` in the console); gear = true comes at the player's level and the reward_rarity
-- setting; counts are scaled by the reward_scale setting.
local RUNES = "Material_T1_Token_WeaponUpgrade_Rune01"
local POOL = {
  {id = "kill_vukah", kind = "kill", faction = 3, name = "Vukah", n = 6, reward = {{item = RUNES, count = 8}}},
  {id = "kill_scavenger", kind = "kill", faction = 4, name = "Scavengers", n = 6, reward = {{item = RUNES, count = 8}}},
  {id = "kill_shroud", kind = "kill", faction = 2, name = "Shroud creatures", n = 8, reward = {{item = RUNES, count = 8}}},
  {id = "kill_wildlife", kind = "kill", faction = 5, name = "Wildlife", n = 4,
   reward = {{item = "Material_T1_Animal_Fur", count = 6}}},
  {id = "kill_beasts", kind = "kill", faction = 6, name = "Wild beasts", n = 3, reward = {{item = RUNES, count = 10}}},
  {id = "kill_skeleton", kind = "kill", faction = 10, name = "Skeletons", n = 6, reward = {{item = RUNES, count = 8}}},
  {id = "kill_drak", kind = "kill", faction = 14, name = "Drak", n = 4, weight = 1, reward = {{item = RUNES, count = 12}}},
  {id = "gather_wood", kind = "gather", match = "wood_logs", name = "Wood logs", n = 60, reward = {{item = RUNES, count = 5}}},
  {id = "gather_stone", kind = "gather", match = "stone", name = "Stone", n = 60, reward = {{item = RUNES, count = 5}}},
  {id = "gather_fibre", kind = "gather", match = "plant_fibre", name = "Plant fibre", n = 40, reward = {{item = RUNES, count = 5}}},
  {id = "gather_metal", kind = "gather", match = "metal_scraps", name = "Metal scraps", n = 20, reward = {{item = RUNES, count = 6}}},
  {id = "gather_fur", kind = "gather", match = "animal_fur", name = "Fur", n = 10, reward = {{item = RUNES, count = 6}}},
  {id = "champion", kind = "champion", name = "champion", n = 1, weight = 1,
   reward = {{item = RUNES, count = 20}, {item = "Weapon_T1_1H_SwordEpic_01", count = 1, gear = true}}},
}
local CHAMPION_LEVELS = 2     -- a champion: at least this many levels above the player ...
local CHAMPION_HEALTH = 1000  -- ... and at least this much health (when first hit)
local SHARE_SECONDS = 10      -- with share_kills, every player who hit it this recently gets the kill
local MOVE_SECONDS = 2        -- an item a chest got from nothing (a mod's put) is "moving" this long: a player's
                              -- pickup of it from nothing then is the rest coming back, no gathering
local LEFTOVER_SECONDS = 120  -- ... and a loose drop of that item within LEFTOVER_METRES of that chest, picked up this
local LEFTOVER_METRES = 30    -- long after, is what did not fit (quick stack's and sorting chests' drops), no gathering
local DROP_SECONDS = 1800     -- this mod's own drops (a reward at a full player's feet) are known this long

local DIFFICULTY = {easy = 0.6, normal = 1.0, hard = 1.5}
local RAW = {"material_", "food_", "build_terrainmaterial_"}
local NEVER = {"processed", "_dish_", "fishingbait", "_bars", "_bar", "powder", "_dust", "dried", "charcoal"}

local players = {}   -- account -> {key, rerolls, done, list = {{id, need, have, state}}}: state 0 open, 1 done, 2 claimed
local offset = 0     -- bounty.reset adds 1: everyone's bounties are new
local dawns = 0      -- dawns seen in this world (reset = game_dawn)
local play_s = 0     -- seconds this world has run (reset = hours)
local hits = {}      -- creature id -> {faction, level, max, by = {player -> play_s}, last}
local gifts = {}     -- "player:item" -> {count, until_s}: rewards on their way (their pickup is no gathering)
local moving = {}    -- item -> play second until which it counts as moving between inventories
local filled = {}    -- item -> list of {x, y, z, until_s}: chests a mod just put that item into (their leftovers)
local dropped = {}   -- pickup entity -> {item, left, until_s}: rewards this mod dropped at a player's feet
local owed = {}    -- account -> list of {item (debug name), count, level, rarity}: rewards that could not be given
                     -- (the player left before they landed); they come with that player's next /bounty.claim
local loaded, dirty = false, false
local last_tick, last_tod = nil, nil
local session = emf.session()
local hud_at, hud_shown, client_steps = -1, false, 0
local purge_at = 0

-- ---- the pool ----
-- emf\config\bounty\pool.lua, when it is there, replaces POOL (that folder stays when EMF is installed again). It runs
-- with nothing in reach (no emf, io or os): it can only build the table. Entries that do not fit are skipped and named.
local BUILT_IN = POOL
local POOL_FILE = (emf.dir:match("^(.*)[\\/][^\\/]+[\\/][^\\/]+$") or ".") .. "\\config\\bounty\\pool.lua"
local KINDS = {kill = true, gather = true, champion = true}

local function pool_entry_ok(e, seen)
  if type(e) ~= "table" or type(e.id) ~= "string" or not e.id:match("^[%w_%-]+$") or seen[e.id] then return false end
  if not KINDS[e.kind] or type(e.n) ~= "number" or e.n < 1 then return false end
  if e.kind == "kill" and type(e.faction) ~= "number" then return false end
  if e.kind == "gather" and (type(e.match) ~= "string" or e.match == "") then return false end
  if e.name ~= nil and type(e.name) ~= "string" then return false end
  if e.weight ~= nil and (type(e.weight) ~= "number" or e.weight < 0) then return false end
  if e.reward ~= nil and type(e.reward) ~= "table" then return false end
  for _, r in ipairs(e.reward or {}) do
    if type(r) ~= "table" or type(r.item) ~= "string" or type(r.count) ~= "number" or r.count < 1 then return false end
  end
  return true
end

local function load_pool()
  local f = io.open(POOL_FILE, "r")
  if not f then
    POOL = BUILT_IN
    return
  end
  f:close()
  local chunk, err = loadfile(POOL_FILE, "t", {})
  local ok, list = false, err
  if chunk then ok, list = pcall(chunk) end
  if not ok or type(list) ~= "table" then
    emf.log("emf\\config\\bounty\\pool.lua could not be read (" .. tostring(list) .. "): main.lua's POOL is used")
    POOL = BUILT_IN
    return
  end
  local pool, seen = {}, {}
  for i, e in ipairs(list) do
    if pool_entry_ok(e, seen) then
      seen[e.id] = true
      e.name = e.name or e.id
      pool[#pool + 1] = e
    else
      emf.log(string.format("pool.lua: entry %d skipped (it needs an id of letters, digits and _, a kind of kill, " ..
        "gather or champion, n, a faction for kill, a match for gather, and rewards with item and count)", i))
    end
  end
  if #pool == 0 then
    emf.log("emf\\config\\bounty\\pool.lua holds no bounty that fits: main.lua's POOL is used")
    POOL = BUILT_IN
    return
  end
  POOL = pool
  emf.log(string.format("the pool: %d bounties from emf\\config\\bounty\\pool.lua", #pool))
end
load_pool()

local function entry(id)
  for _, e in ipairs(POOL) do
    if e.id == id then return e end
  end
  return nil
end

local function need_for(e)
  if e.kind == "champion" then return e.n end
  local f = DIFFICULTY[emf.setting("difficulty") or "normal"] or 1
  return math.max(1, math.floor(e.n * f + 0.5))
end

local function text_of(e, need)
  if e.kind == "kill" then return string.format("Slay %d %s", need, e.name) end
  if e.kind == "gather" then return string.format("Gather %d %s", need, e.name) end
  if need == 1 then return "Slay a champion" end
  return string.format("Slay %d champions", need)
end

local function short_of(e) return e.kind == "champion" and "Champion" or e.name end

local function scaled(r)
  if r.gear then return r.count end
  local s = emf.setting("reward_scale") or 1
  return math.max(1, math.floor(r.count * s + 0.5))
end

local function reward_text(e)
  local parts = {}
  for _, r in ipairs(e.reward or {}) do parts[#parts + 1] = scaled(r) .. " x " .. r.item end
  return table.concat(parts, ", ")
end

-- ---- deterministic draws: the same account, period and reroll give the same bounties ----
local function hash(s)
  local h = 2166136261
  for i = 1, #s do h = ((h ~ s:byte(i)) * 16777619) % 4294967296 end
  return h
end

local function random_from(seed)
  local state = seed % 2147483648
  return function(n)
    state = (state * 1103515245 + 12345) % 2147483648
    return (state >> 8) % n + 1
  end
end

-- `count` pool entries for `account` (none of `exclude`), weighted.
local function draw(account, key, salt, count, exclude)
  local rand = random_from(hash(account .. "|" .. key .. "|" .. salt))
  local taken = {}
  for id in pairs(exclude or {}) do taken[id] = true end
  local out = {}
  for _ = 1, count do
    local total = 0
    for _, e in ipairs(POOL) do
      if not taken[e.id] then total = total + (e.weight or 3) end
    end
    if total <= 0 then break end
    local r = rand(total)
    for _, e in ipairs(POOL) do
      if not taken[e.id] then
        r = r - (e.weight or 3)
        if r <= 0 then
          taken[e.id] = true
          out[#out + 1] = {id = e.id, need = need_for(e), have = 0, state = 0}
          break
        end
      end
    end
  end
  return out
end

-- ---- periods ----
local function period_len() return math.max(1, emf.setting("reset_hours") or 4) * 3600 end

local function period_key()
  local mode = emf.setting("reset")
  if mode == "game_dawn" then return "g" .. dawns .. "." .. offset end
  if mode == "hours" then return "h" .. math.floor(play_s / period_len()) .. "." .. offset end
  local t = os.date("*t", os.time() - (emf.setting("reset_hour") or 6) * 3600)
  return string.format("d%04d%03d.%d", t.year, t.yday, offset)
end

local function until_text()
  local mode = emf.setting("reset")
  local left
  if mode == "game_dawn" then return "New bounties at dawn" end
  if mode == "hours" then
    left = period_len() - (play_s % period_len())
  else
    local hour = emf.setting("reset_hour") or 6
    local now = os.time()
    local t = os.date("*t", now)
    local next_reset = os.time({year = t.year, month = t.month, day = t.day, hour = hour, min = 0, sec = 0})
    if next_reset <= now then next_reset = next_reset + 86400 end
    left = next_reset - now
  end
  local minutes = math.ceil(left / 60)
  return string.format("New bounties in %d h %d min", minutes // 60, minutes % 60)
end

-- The player's board for the running period (a new period draws new bounties).
local function board_of(account)
  local rec = players[account]
  local key = period_key()
  if not rec then
    rec = {done = 0}
    players[account] = rec
  end
  if rec.key ~= key then
    rec.key, rec.rerolls = key, 0
    rec.list = draw(account, key, "0", math.max(1, emf.setting("bounties_per_period") or 3))
    dirty = true
  end
  return rec
end

-- ---- saving ----
local function serialize()
  local lines = {"offset " .. offset, "dawns " .. dawns, "play " .. math.floor(play_s)}
  local keys = {}
  for k in pairs(players) do keys[#keys + 1] = k end
  table.sort(keys)
  for _, k in ipairs(keys) do
    local rec = players[k]
    lines[#lines + 1] = string.format("p %s %s %d %d", k, rec.key or "-", rec.rerolls or 0, rec.done or 0)
    for _, b in ipairs(rec.list or {}) do
      lines[#lines + 1] = string.format("b %s %s %d %d %d", k, b.id, b.need, b.have, b.state)
    end
  end
  keys = {}
  for k in pairs(owed) do keys[#keys + 1] = k end
  table.sort(keys)
  for _, k in ipairs(keys) do
    for _, o in ipairs(owed[k]) do
      lines[#lines + 1] = string.format("owe %s %s %d %d %s", k, o.item, o.count, o.level or 0, o.rarity or "-")
    end
  end
  return table.concat(lines, "\n") .. "\n"
end

local function save()
  if loaded and emf.save_world_data(serialize()) then dirty = false end
end

emf.on_world_data(function(world, text)
  load_pool()  -- pool.lua may have changed since
  players, hits, gifts, moving, owed = {}, {}, {}, {}, {}
  filled, dropped = {}, {}
  for account, item, count, level, rarity in text:gmatch("owe (%S+) (%S+) (%d+) (%d+) (%S+)") do
    owed[account] = owed[account] or {}
    table.insert(owed[account], {item = item, count = tonumber(count), level = tonumber(level),
                                 rarity = rarity ~= "-" and rarity or nil})
  end
  purge_at, last_tod = 0, nil  -- another world's clocks mean nothing here
  offset = tonumber(text:match("offset (%d+)")) or 0
  dawns = tonumber(text:match("dawns (%d+)")) or 0
  play_s = tonumber(text:match("play (%d+)")) or 0
  for account, key, rerolls, done in text:gmatch("p (%S+) (%S+) (%d+) (%d+)") do
    players[account] = {key = key, rerolls = tonumber(rerolls), done = tonumber(done), list = {}}
  end
  for account, id, need, have, state in text:gmatch("b (%S+) (%S+) (%d+) (%d+) (%d+)") do
    local rec = players[account]
    if rec and entry(id) then
      table.insert(rec.list, {id = id, need = tonumber(need), have = tonumber(have), state = tonumber(state)})
    end
  end
  loaded, dirty = true, false
  session = emf.session()
  hud_at = -1
end)

-- A new hosted session (another world, a reload): creature ids of the old one mean other things now.
local function same_session()
  if session == emf.session() then return end
  session = emf.session()
  hits, gifts, moving, filled, dropped = {}, {}, {}, {}, {}
  loaded = false  -- the old world's data is not this one's: commands wait for on_world_data
  emf.hud("board", "")
  hud_shown = false
end

-- ---- reading the game ----
local function has(world, component, id)
  local ok, value = pcall(world.get, world, component, id)
  if not ok then return nil end
  return value ~= nil
end

local function is_player(world, id)
  return id >= 1 and id <= 1023 and has(world, "PlayerState", id) == true
end

local function level_of(world, id)
  local ok, l = pcall(world.get, world, "Level", id)
  return (ok and l) and l.dataStorage[1] or 1
end

local function faction_of(world, id)
  local ok, f = pcall(world.get, world, "Faction", id)
  return (ok and f) and f.faction or nil
end

local function max_health_of(world, id)
  local ok, h = pcall(world.get, world, "NetworkHealth", id)
  return (ok and h) and h.healthMax or 0
end

-- Picked up in the world: from nothing (a tree), a loose drop (PickupItem), a plant or rock, or a creature's corpse;
-- never out of a chest, a station or a death bag (InventorySetup), nor out of a tombstone (seen in game: an Inventory,
-- CreateItemLoot and DestroyOnLoot, no Health). A component that cannot be read counts against (fails closed).
local function from_the_world(world, from)
  if from == 0 then return true end
  if has(world, "InventorySetup", from) ~= false then return false end
  if has(world, "PickupItem", from) == true then return true end
  if has(world, "Inventory", from) ~= false and has(world, "CreateItemLoot", from) ~= false and
     has(world, "DestroyOnLoot", from) ~= false and has(world, "Health", from) ~= true then
    return false
  end
  return true
end

-- A chest a mod put `item` into: its place, kept LEFTOVER_SECONDS (one place per chest and item, at most 16 an item).
local function note_filled(world, chest, item)
  local x, y, z = world:position(chest)
  if not x then return end
  local list = filled[item] or {}
  filled[item] = list
  for _, f in ipairs(list) do
    if (f.x - x) ^ 2 + (f.y - y) ^ 2 + (f.z - z) ^ 2 < 1 then
      f.until_s = play_s + LEFTOVER_SECONDS
      return
    end
  end
  if #list >= 16 then table.remove(list, 1) end
  list[#list + 1] = {x = x, y = y, z = z, until_s = play_s + LEFTOVER_SECONDS}
end

-- A loose drop a mod made, picked up: this mod's own (a reward dropped at a full player's feet: emf.give named its
-- pickup), or one of the same item within LEFTOVER_METRES of a chest a mod put that item into in the last
-- LEFTOVER_SECONDS (what did not fit there: quick stack and sorting chests drop it beside the chest or the player).
local function mod_drop(world, player, from, item, count)
  local d = dropped[from]
  if d and d.item == item then
    d.left = d.left - count
    if d.left <= 0 then dropped[from] = nil end
    return true
  end
  local list = filled[item]
  if not list or has(world, "PickupItem", from) ~= true then return false end
  local x, y, z = world:position(from)
  if not x then x, y, z = world:position(player) end
  if not x then return false end
  for _, f in ipairs(list) do
    if play_s <= f.until_s and (f.x - x) ^ 2 + (f.y - y) ^ 2 + (f.z - z) ^ 2 <= LEFTOVER_METRES ^ 2 then return true end
  end
  return false
end

local item_names = {}
local function raw_name(item)
  local n = item_names[item]
  if n == nil then
    local info = emf.item_info(item)
    if not info then return nil end  -- not known yet: asked again next time
    n = string.lower(info.name)
    local raw = false
    for _, k in ipairs(RAW) do
      if n:sub(1, #k) == k then raw = true end
    end
    for _, p in ipairs(NEVER) do
      if n:find(p, 1, true) then raw = false end
    end
    if not raw then n = false end
    item_names[item] = n
  end
  return n
end

-- ---- progress and rewards ----
local function account_of(world, player) return world:account(player) end

local function owe(account, item, count, opts)
  owed[account] = owed[account] or {}
  table.insert(owed[account], {item = item, count = count, level = opts and opts.level or 0, rarity = opts and opts.rarity})
  dirty = true
  emf.log(string.format("reward: %d x %s could not be given (the player left?): owed to %s until their next /bounty.claim",
    count, item, account))
end

-- Drops `count` of item `id` at the player's feet; the pickup is remembered, so picking it up later is no gathering.
local function drop(world, player, id, count)
  local pickup = emf.give(world, player, id, count)
  if pickup == 0 then return false end
  dropped[pickup] = {item = id, left = count, until_s = play_s + DROP_SECONDS}
  return true
end

-- One reward item into the player; what does not fit is dropped at their feet; what cannot even be dropped (the player
-- left before it landed) is owed to their account.
local function deliver(world, player, account, name, count, opts)
  local id = emf.item(name)
  if not id then
    emf.log("no item named " .. name .. " (a reward in POOL)")
    return nil
  end
  local key = player .. ":" .. id
  local g = gifts[key] or {count = 0}
  g.count, g.until_s = g.count + count, play_s + 10
  gifts[key] = g
  local queued = emf.put(world, player, id, count, function(w, added)
    if added < count then  -- no room for the rest: at the player's feet, never lost
      local left = gifts[key]
      if left then
        left.count = left.count - (count - added)
        if left.count <= 0 then gifts[key] = nil end
      end
      if drop(w, player, id, count - added) then
        emf.log(string.format("reward: %d of %d x %s fitted; the rest dropped at player %d's feet", added, count, name, player))
      else
        owe(account, name, count - added, opts)
      end
    end
  end, opts)
  if not queued then
    if drop(world, player, id, count) then
      emf.log(string.format("reward: the put of %d x %s was refused; dropped at player %d's feet", count, name, player))
    else
      owe(account, name, count, opts)
    end
  end
  return count .. " x " .. name
end

local function give_rewards(world, player, account, e)
  local got = {}
  local level = level_of(world, player)
  for _, r in ipairs(e.reward or {}) do
    local opts = nil
    if r.gear then
      opts = {level = level}
      local rarity = emf.setting("reward_rarity") or "default"
      if rarity ~= "default" then opts.rarity = rarity end
    end
    local text = deliver(world, player, account, r.item, scaled(r), opts)
    if text then got[#got + 1] = text end
  end
  return table.concat(got, ", ")
end

local function claim_all(world, player, account)
  local rec = board_of(account)
  local parts = {}
  local waiting = owed[account]  -- rewards owed from before come first
  if waiting then
    owed[account] = nil
    dirty = true
    local got = {}
    for _, o in ipairs(waiting) do
      local text = deliver(world, player, account, o.item, o.count, (o.level > 0 or o.rarity) and {level = o.level, rarity = o.rarity} or nil)
      if text then got[#got + 1] = text end
    end
    if #got > 0 then parts[#parts + 1] = table.concat(got, ", ") end
  end
  for _, b in ipairs(rec.list) do
    if b.state == 1 then
      local e = entry(b.id)
      b.state = 2
      dirty = true
      if e then parts[#parts + 1] = give_rewards(world, player, account, e) end
    end
  end
  return parts
end

local function progress(world, player, kind, match, amount)
  local account = account_of(world, player)
  if not account or not loaded then return end
  local rec = board_of(account)
  for _, b in ipairs(rec.list) do
    local e = entry(b.id)
    if b.state == 0 and e and e.kind == kind and match(e) then
      b.have = math.min(b.need, b.have + amount)
      dirty = true
      hud_at = -1
      if b.have >= b.need then
        b.state = 1
        rec.done = (rec.done or 0) + 1
        local text = "Bounty done: " .. text_of(e, b.need)
        if emf.setting("auto_claim") then
          b.state = 2
          text = text .. "! " .. give_rewards(world, player, account, e) .. " are yours"
        else
          text = text .. "! /bounty.claim for " .. reward_text(e)
        end
        emf.log(text .. string.format(" (player %d, %s)", player, account))
        if player == emf.local_player() then emf.notify(text) end
      end
    end
  end
end

-- ---- events ----
emf.on_event("HitEvent", function(world, h)
  same_session()
  local target, source = h.targetId, h.rootSourceId
  if target == 0 or target <= 1023 or not is_player(world, source) then return end
  local rec = hits[target]
  if not rec then
    rec = {by = {}, faction = faction_of(world, target), level = level_of(world, target), max = max_health_of(world, target)}
    hits[target] = rec
  end
  rec.by[source] = play_s
  rec.last = source
end, "server")

emf.on_event("EntityDiedEvent", function(world, d)
  same_session()
  local rec = hits[d.deadEntityId]
  if not rec then return end
  hits[d.deadEntityId] = nil
  local credited = {}
  if emf.setting("share_kills") then
    for player, at in pairs(rec.by) do
      if play_s - at <= SHARE_SECONDS then credited[#credited + 1] = player end
    end
  elseif rec.last and play_s - rec.by[rec.last] <= SHARE_SECONDS then
    credited[1] = rec.last
  end
  for _, player in ipairs(credited) do
    if rec.faction then progress(world, player, "kill", function(e) return e.faction == rec.faction end, 1) end
    if rec.level >= level_of(world, player) + CHAMPION_LEVELS and rec.max >= CHAMPION_HEALTH then
      progress(world, player, "champion", function() return true end, 1)
    end
  end
end, "server")

emf.on_event("PickupEvent", function(world, e)
  same_session()
  local player, from, item, count = e.pickedUpByEntityId, e.pickedUpFromEntityId, e.itemId, e.itemCount
  if not is_player(world, player) then
    -- a chest (any storage) got items from nothing: a mod's put (quick stack, a sorting chest...). Whatever of that
    -- item a player gets from nothing in the next moment is the rest coming back, no gathering; a loose drop of it near
    -- that chest a while after is what did not fit there (mod_drop).
    if from == 0 and player ~= 0 and has(world, "InventorySetup", player) == true then
      moving[item] = play_s + MOVE_SECONDS
      note_filled(world, player, item)
    end
    return
  end
  if from == 0 and moving[item] and play_s <= moving[item] then return end
  if from ~= 0 and mod_drop(world, player, from, item, count) then return end
  if from == 0 then  -- a reward on its way is no gathering
    local key = player .. ":" .. item
    local g = gifts[key]
    if g and g.count >= count then
      g.count = g.count - count
      if g.count <= 0 then gifts[key] = nil end
      return
    end
  end
  if not from_the_world(world, from) then return end
  local name = raw_name(item)
  if not name then return end
  progress(world, player, "gather", function(en) return name:find(en.match, 1, true) ~= nil end, count)
end, "server")

-- ---- the HUD (the host's own board) ----
local function show(world)
  local me = emf.local_player()
  local account = me and me ~= 0 and world:account(me)
  if not emf.setting("hud") or not account or not loaded then
    if hud_shown then emf.hud("board", "") end
    hud_shown = false
    return
  end
  local rec = board_of(account)
  local parts = {}
  for _, b in ipairs(rec.list) do
    local e = entry(b.id)
    if e then parts[#parts + 1] = string.format("%s %d/%d", short_of(e), b.have, b.need) end
  end
  if #parts == 0 then
    if hud_shown then emf.hud("board", "") end
    hud_shown = false
    return
  end
  emf.hud("board", "Bounty: " .. table.concat(parts, " | "), emf.setting("corner"), emf.setting("color"))
  hud_shown = true
end

-- ---- commands ----
local function sender(world)
  local who = emf.sender()
  local id = (who and who.chat) and who.entity or emf.local_player()
  if not id or id == 0 then return nil end
  local account = world:account(id)
  if not account then return nil end
  return {id = id, account = account}
end

local function unknown(world)
  emf.reply("Bounty Board: EMF could not tell which player you are; try again in a moment")
end

emf.command("board", ": your bounties and how far you are", function(world, args)
  same_session()
  local me = sender(world)
  if not me then return unknown(world) end
  if not loaded then
    emf.reply("Bounty Board: this world's data is not loaded yet; try again in a moment")
    return
  end
  local rec = board_of(me.account)
  for i, b in ipairs(rec.list) do
    local e = entry(b.id)
    if e then
      local state = b.state == 2 and "claimed" or b.state == 1 and "done" or (b.have .. "/" .. b.need)
      local tail = b.state == 2 and "" or (" (reward: " .. reward_text(e) .. (b.state == 1 and "; /bounty.claim)" or ")"))
      emf.reply(string.format("%d. %s: %s%s", i, text_of(e, b.need), state, tail))
    end
  end
  local left = math.max(0, (emf.setting("rerolls") or 0) - (rec.rerolls or 0))
  if owed[me.account] then
    local n = #owed[me.account]
    emf.reply(string.format("%d reward%s waiting: /bounty.claim", n, n == 1 and "" or "s"))
  end
  emf.reply(string.format("%s; %d reroll%s left (/bounty.reroll <n>).", until_text(), left, left == 1 and "" or "s"))
end, {players = true})

emf.command("claim", ": the rewards of your finished bounties", function(world, args)
  same_session()
  local me = sender(world)
  if not me then return unknown(world) end
  if not loaded then
    emf.reply("Bounty Board: this world's data is not loaded yet; try again in a moment")
    return
  end
  local parts = claim_all(world, me.id, me.account)
  if #parts == 0 then
    emf.reply("No finished bounty to claim.")
    return
  end
  hud_at = -1
  emf.reply("Claimed: " .. table.concat(parts, "; "))
end, {players = true})

emf.command("reroll", "<n>: swap one of your bounties for another", function(world, args)
  same_session()
  local me = sender(world)
  if not me then return unknown(world) end
  if not loaded then
    emf.reply("Bounty Board: this world's data is not loaded yet; try again in a moment")
    return
  end
  local rec = board_of(me.account)
  local n = tonumber(args[1] or "")
  if not n or n < 1 or n > #rec.list or n % 1 ~= 0 then
    emf.reply("Usage: /bounty.reroll <1-" .. #rec.list .. ">")
    return
  end
  if (rec.rerolls or 0) >= (emf.setting("rerolls") or 0) then
    emf.reply("No rerolls left until the next bounties.")
    return
  end
  if rec.list[n].state ~= 0 then
    emf.reply("Bounty " .. n .. " is finished: /bounty.claim")
    return
  end
  local exclude = {}
  for _, b in ipairs(rec.list) do exclude[b.id] = true end
  local used = (rec.rerolls or 0) + 1
  local new = draw(me.account, rec.key, "r" .. used .. ":" .. n, 1, exclude)[1]
  if not new then  -- nothing to swap to: the reroll is not used up
    emf.reply("The pool has no other bounty.")
    return
  end
  rec.rerolls = used
  rec.list[n] = new
  dirty = true
  hud_at = -1
  emf.reply("Bounty " .. n .. " is now: " .. text_of(entry(new.id), new.need) .. " (reward: " .. reward_text(entry(new.id)) .. ")")
end, {players = true})

emf.command("top", ": who finished the most bounties in this world", function(world, args)
  same_session()
  local me = sender(world)
  local list = {}
  for account, rec in pairs(players) do
    if (rec.done or 0) > 0 then list[#list + 1] = {account = account, done = rec.done} end
  end
  table.sort(list, function(a, b) return a.done > b.done or (a.done == b.done and a.account < b.account) end)
  if #list == 0 then
    emf.reply("Nobody has finished a bounty in this world yet.")
    return
  end
  for i = 1, math.min(5, #list) do
    local x = list[i]
    emf.reply(string.format("%d. %s: %d%s", i, x.account, x.done, (me and me.account == x.account) and " (you)" or ""))
  end
end, {players = true})

emf.command("give", "<bounty id>: put a bounty from the pool on your board (host, for tests)", function(world, args)
  same_session()
  local me = sender(world)
  if not me then return unknown(world) end
  local e = entry(args[1] or "")
  if not e then
    local ids = {}
    for _, x in ipairs(POOL) do ids[#ids + 1] = x.id end
    emf.reply("Unknown bounty; the pool has: " .. table.concat(ids, ", "))
    return
  end
  if not loaded then
    emf.reply("Bounty Board: this world's data is not loaded yet; try again in a moment")
    return
  end
  local rec = board_of(me.account)
  local b = {id = e.id, need = need_for(e), have = 0, state = 0}
  local replaced = false
  for i, x in ipairs(rec.list) do
    if x.id == e.id then
      rec.list[i] = b
      replaced = true
    end
  end
  if not replaced then table.insert(rec.list, b) end
  dirty = true
  hud_at = -1
  emf.reply("Given: " .. text_of(e, b.need) .. " (reward: " .. reward_text(e) .. ")")
end)

emf.command("reset", ": new bounties for everyone now (host)", function(world, args)
  if not loaded then
    emf.reply("Bounty Board: this world's data is not loaded yet; try again in a moment")
    return
  end
  offset = offset + 1
  dirty = true
  hud_at = -1
  emf.reply("New bounties for everyone.")
end)

-- ---- every step ----
emf.on_tick(function(world)
  if world:is_client() then  -- in someone else's game: the line goes
    client_steps = client_steps + 1
    if client_steps > 180 and hud_shown then
      emf.hud("board", "")
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
  local tod = emf.time_of_day()
  if tod then
    if last_tod and last_tod < 6 and tod >= 6 then
      dawns = dawns + 1
      dirty = true
      hud_at = -1
    end
    last_tod = tod
  end
  if play_s >= purge_at then  -- forget fights nobody finished
    purge_at = play_s + 10
    for id, rec in pairs(hits) do
      local newest = 0
      for _, at in pairs(rec.by) do newest = math.max(newest, at) end
      if play_s - newest > 60 then hits[id] = nil end
    end
    for k, g in pairs(gifts) do
      if play_s > g.until_s then gifts[k] = nil end
    end
    for k, until_s in pairs(moving) do
      if play_s > until_s then moving[k] = nil end
    end
    for item, list in pairs(filled) do
      for i = #list, 1, -1 do
        if play_s > list[i].until_s then table.remove(list, i) end
      end
      if #list == 0 then filled[item] = nil end
    end
    for k, d in pairs(dropped) do
      if play_s > d.until_s then dropped[k] = nil end
    end
  end
  if dirty then save() end
  local s = math.floor(play_s)
  if s ~= hud_at then
    hud_at = s
    show(world)
  end
end)

emf.on_setting_changed(function(key) hud_at = -1 end)
