-- life_skills: woodcutting, mining, foraging, hunting and fishing level up as you gather (an EMF Lua example mod).
-- What you pick up from the world (not from a chest) gives XP to the skill of that item; every level adds a chance to
-- get the same items once more. Kept per world and per player (by account), so friends in your game have their own.
-- Console or chat: life_skills.skills.

local SKILLS = {"woodcutting", "mining", "foraging", "hunting", "fishing"}
local NAME = {woodcutting = "Woodcutting", mining = "Mining", foraging = "Foraging", hunting = "Hunting", fishing = "Fishing"}
local SHORT = {woodcutting = "Wood", mining = "Mine", foraging = "Forage", hunting = "Hunt", fishing = "Fish"}
local TOP_LEVEL = 20

-- An item's debug name decides its skill. Only raw materials, raw food and terrain blocks count (weapons, blocks,
-- props and furniture share words with them); made things never; then the first list with a part of the (lower case)
-- name wins. Checked against every item of the game (export/items.csv): woodcutting 9, fishing 22, mining 80 (60
-- terrain blocks), hunting 30, foraging 65 items, all raw.
local KINDS = {"material_", "food_", "_food_", "build_terrainmaterial_"}
local NEVER = {"fishingbait", "_dish_", "processed", "feeding", "_dust", "powder", "grilled", "roasted", "ground_",
               "dried", "charcoal"}
local PATTERNS = {
  {"woodcutting", {"wood_logs", "hardwood", "resin", "_bark"}},
  {"fishing", {"_fish_", "raw_fish"}},
  {"mining", {"build_terrainmaterial_", "_ore", "stone", "salt", "clay", "sulfur", "coal", "nitrate", "obsidian",
              "marble", "crystal", "amethyst", "aquamarine", "lapislazuli", "metal_scraps"}},
  {"hunting", {"raw_bad_meat", "_fur", "hide", "feather", "bones", "critterparts", "scales", "tusk", "gland", "talon",
               "ribs", "wool", "raw_egg", "carapace"}},
  {"foraging", {"plant_fibre", "twigs", "flax", "raw_fruit", "raw_mushroom", "raw_herb", "raw_nightplant",
                "raw_vegetable", "raw_grain", "raw_bad_mushroom", "raw_bad_fruit", "raw_bad_herb", "raw_bad_vegetable",
                "sugarcane", "raw_rice", "coffeebeans", "daylily", "mushroomdrop", "overgrowth_", "chamomile", "indigo",
                "aloe", "saffron", "rooibos", "straw", "wheat", "night_", "desert_flower", "wolfsbane"}},
}

local skills = {}          -- player key -> {skill = xp}
local loaded = false       -- this world's data arrived
local dirty, last_save, steps = false, 0, 0
local save_now = false     -- a level-up: save at the next step, not up to 10 s later
local last_session = nil   -- the hosted session whose data is shown
local gifts = {}           -- "player:item" -> {count, step}: bonus items; their pickup (if the game reports one) is no XP
local skill_of_item = {}   -- item id -> skill or false
local refresh = true       -- the HUD line needs drawing (it needs a world)

local function level_of(xp)
  return math.min(TOP_LEVEL, math.floor(math.sqrt(xp / 50)) + 1)
end
local function xp_for(level) return 50 * (level - 1) ^ 2 end
local function bonus_per_level() return emf.setting("bonus_per_level") or 2 end

local function classify(name)
  local n = string.lower(name)
  local raw = false
  for _, k in ipairs(KINDS) do
    if n:sub(1, #k) == k then raw = true end
  end
  if not raw or n:find("_bars?$") then return false end  -- metal bars are smelted
  for _, p in ipairs(NEVER) do
    if n:find(p, 1, true) then return false end
  end
  for _, entry in ipairs(PATTERNS) do
    for _, p in ipairs(entry[2]) do
      if n:find(p, 1, true) then return entry[1] end
    end
  end
  return false
end

local function skill_for(item)
  local s = skill_of_item[item]
  if s == nil then
    local info = emf.item_info(item)
    if not info then return false end  -- not known yet: ask again next time
    s = classify(info.name)
    skill_of_item[item] = s
  end
  return s
end

-- A player's key: their account (the same in every session), or their entity id when it is not known.
local function key_of(world, player)
  return world:account(player) or ("entity:" .. player)
end

-- true / false, or nil when the game's component could not be read (a game update?): callers then skip the pickup.
local function has(world, component, id)
  local ok, value = pcall(world.get, world, component, id)
  if not ok then return nil end
  return value ~= nil
end

local function show(world)
  if not emf.setting("hud") then
    emf.hud("skills", "")
    refresh = false
    return
  end
  local me = emf.local_player()
  if not me or me == 0 then
    refresh = true  -- right after a load the game has not set the local player yet: try again next step
    return
  end
  local account = world:account(me)
  refresh = account == nil  -- the players' accounts may come a moment later: draw again then
  local mine = skills[account or ("entity:" .. me)] or {}
  local parts = {}
  for _, s in ipairs(SKILLS) do parts[#parts + 1] = SHORT[s] .. " " .. level_of(mine[s] or 0) end
  emf.hud("skills", table.concat(parts, " | "), emf.setting("corner"))
end

local function serialize()
  local keys = {}
  for k in pairs(skills) do keys[#keys + 1] = k end
  table.sort(keys)
  local lines = {}
  for _, k in ipairs(keys) do
    for _, s in ipairs(SKILLS) do
      local xp = skills[k][s]
      if xp and xp > 0 then lines[#lines + 1] = k .. " " .. s .. " " .. math.floor(xp) end
    end
  end
  return table.concat(lines, "\n") .. (#lines > 0 and "\n" or "")
end

emf.on_world_data(function(world, text)
  skills = {}
  for key, skill, xp in text:gmatch("(%S+) (%a+) (%d+)") do
    if NAME[skill] then
      skills[key] = skills[key] or {}
      skills[key][skill] = tonumber(xp)
    end
  end
  loaded, dirty = true, false
  last_session = emf.session()
  show(world)
end)

emf.on_event("PickupEvent", function(world, e)
  if not loaded then return end  -- before this world's data came (a moment after a load)
  local player, from = e.pickedUpByEntityId, e.pickedUpFromEntityId
  if player == 0 or has(world, "PlayerState", player) ~= true then return end
  if from ~= 0 and has(world, "InventorySetup", from) ~= false then return end  -- out of a chest or a death bag
  local item, count = e.itemId, e.itemCount
  local gift_key = player .. ":" .. item
  local gift = gifts[gift_key]
  if gift and from == 0 and gift.count >= count then  -- the bonus we gave (no source), reported as a pickup
    gift.count = gift.count - count
    if gift.count == 0 then gifts[gift_key] = nil end
    return
  end
  local skill = skill_for(item)
  if not skill then return end
  local key = key_of(world, player)
  local mine = skills[key] or {}
  skills[key] = mine
  local before = level_of(mine[skill] or 0)
  mine[skill] = (mine[skill] or 0) + count
  dirty = true
  local chance = (before - 1) * bonus_per_level()
  if chance > 0 and math.random(100) <= chance then
    local queued = emf.put(world, player, item, count, function(w, added)
      local g = gifts[gift_key]  -- what the game could not add (a full inventory) will never be reported
      if g and added < count then
        g.count = g.count - (count - added)
        if g.count <= 0 then gifts[gift_key] = nil end
      end
    end)
    if queued then gifts[gift_key] = {count = (gift and gift.count or 0) + count, step = steps} end
  end
  local after = level_of(mine[skill])
  if after > before then save_now = true end  -- every player's level-up is saved at once (a server has no local player)
  if player == emf.local_player() then
    if after > before then
      emf.notify(string.format("%s reached level %d: %d%% chance of extra yield", NAME[skill], after,
                               (after - 1) * bonus_per_level()))
    end
    show(world)
  end
end)

emf.on_tick(function(world)
  if not world:is_server() then return end
  steps = steps + 1
  for k, g in pairs(gifts) do
    if steps - g.step > 120 then gifts[k] = nil end  -- a gift the game did not report as a pickup
  end
  local session = emf.session()
  if last_session ~= nil and session ~= last_session then  -- another world: its data comes after its save loads
    last_session, loaded, skills = session, false, {}
    emf.hud("skills", "")
  end
  if refresh and loaded then show(world) end
  if dirty and loaded and (save_now or steps - last_save >= 600) then  -- at most every 10 s, at once on a level-up
    if emf.save_world_data(serialize()) then dirty = false end
    last_save, save_now = steps, false  -- a failed save is tried again in 10 s, not every step
  end
end)

emf.command("skills", ": your life skills", function(world, args)
  local sender = emf.sender()
  local player = (sender and sender.chat) and sender.entity or emf.local_player()
  if not player or player == 0 then
    emf.reply("player unknown")
    return
  end
  local mine = skills[key_of(world, player)] or {}
  for _, s in ipairs(SKILLS) do
    local xp = math.floor(mine[s] or 0)
    local level = level_of(xp)
    if level >= TOP_LEVEL then
      emf.reply(string.format("%s %d (top level)", NAME[s], level))
    else
      emf.reply(string.format("%s %d: %d/%d XP", NAME[s], level, xp, xp_for(level + 1)))
    end
  end
end, {players = true})  -- every player may ask for their own skills from the chat (lua 1.4; an older host: admins only)

emf.on_setting_changed(function(key) refresh = true end)
