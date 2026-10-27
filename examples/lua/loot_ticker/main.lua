-- loot_ticker: your last 5 pickups on screen, newest first, fading out (an EMF example mod in Lua).
--   +30 Wood Logs
--   +2 Fishing Bait
-- It listens to the game's PickupEvent, names items with emf.item_info, and fades a line through its colour's alpha.
-- Save this file while the game runs: EMF reloads the mod within a second.

local LINES = 5
local MERGE_S = 3  -- the same item again within 3 s adds to its line
local FADE_S = 2   -- a line fades out over its last 2 s

local feed = {}    -- newest first: {item, count, name, born}
local now = 0      -- steps of your client world so far (60 a second)
local shown = {}   -- what each line shows now (text and colour): a line is sent again only when it changes

-- "Material_T1_Wood_Logs" -> "Wood Logs": the item's debug name without its kind and tier, in words.
local function pretty(id)
  local info = emf.item_info(id)  -- needs a world: call it inside a callback (here: the event's)
  if not info then return string.format("item 0x%x", id) end
  local name = info.name:gsub("^%a+_T%d+_", ""):gsub("_", " "):gsub("(%l)(%u)", "%1 %2")
  return name
end

-- The events arrive from the world you host (the server world), once per pickup by anyone: keep yours.
emf.on_event("PickupEvent", function(world, e)
  if e.pickedUpByEntityId ~= emf.local_player() or e.itemCount == 0 then return end
  local top = feed[1]
  if top and top.item == e.itemId and now - top.born < MERGE_S * 60 then
    top.count = top.count + e.itemCount
    top.born = now  -- its time starts again
  else
    table.insert(feed, 1, {item = e.itemId, count = e.itemCount, name = pretty(e.itemId), born = now})
    if #feed > LINES then table.remove(feed) end  -- the oldest goes
  end
end)

-- Your own screen: the client world draws.
emf.on_tick(function(world)
  if not world:is_client() then return end
  now = now + 1
  local life = emf.setting("seconds") * 60
  for i = #feed, 1, -1 do  -- lines whose time is up go
    if now - feed[i].born >= life then table.remove(feed, i) end
  end
  for i = 1, LINES do
    local f = feed[i]
    local text, alpha = "", 255
    if f then
      text = "+" .. f.count .. " " .. f.name
      local left = (life - (now - f.born)) / 60  -- seconds left
      -- in 16 steps, so a fading line is sent again a few times, not 60 times a second
      -- (at least 16: alpha 0 with the colour #000000 is 0, which emf.hud reads as "the usual colour")
      if left < FADE_S then alpha = math.max(16, math.floor(255 * left / FADE_S / 16) * 16) end
    end
    local colour = emf.setting("color") .. string.format("%02x", alpha)  -- "#rrggbb" + alpha: "#rrggbbaa"
    if text .. colour ~= shown[i] then
      shown[i] = text .. colour
      -- line ids "01" to "05": lines of one corner are listed by id (zero-padded, so LINES = 12 keeps its order)
      emf.hud(string.format("%02d", i), text, emf.setting("corner"), colour)
    end
  end
end)

emf.on_setting_changed(function(key)
  shown = {}  -- send every line again (another corner or colour)
end)
