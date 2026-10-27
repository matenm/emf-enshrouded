-- hud_widget: a tidy corner widget (an EMF example mod in Lua):
--   X 412  Y 85  Z -1290
--   Day   [#######.....] 13:30
--   Dusk in 4h 30m
-- Each line is sent to the screen only when its text changes; the corner and the colour come from the settings.
-- Save this file while the game runs: EMF reloads the mod within a second.

local DAWN, DUSK = 6, 18  -- the game's night lasts from 18:00 to 6:00
local CELLS = 12          -- the bar's width

local shown = {}  -- what each line shows now

-- "4h 30m", or "15m" under an hour.
local function span(hours)
  local minutes = math.floor(hours * 60 + 0.5)
  if minutes < 60 then return minutes .. "m" end
  return string.format("%dh %02dm", minutes // 60, minutes % 60)
end

-- How much of the day (or night) has passed, as a bar: [#######.....]. The screen's font has plain letters only.
local function bar(done, total)
  local n = math.floor(done * CELLS / total + 1e-9)
  return "[" .. string.rep("#", n) .. string.rep(".", CELLS - n) .. "]"
end

-- The three lines as they should be now. "" removes a line.
local function lines(world)
  local out = {"", "", ""}
  if emf.setting("show_position") then
    local x, y, z = world:position(emf.local_player())  -- metres; nil before your player exists
    if x then out[1] = string.format("X %d  Y %d  Z %d", math.floor(x + 0.5), math.floor(y + 0.5), math.floor(z + 0.5)) end
  end
  local h = emf.time_of_day(world)  -- hours: 19.5 is 19:30; nil when the world's clock is not known
  if not h then
    out[2] = "Time of day unknown"
    return out
  end
  local clock = string.format("%02d:%02d", math.floor(h), math.floor(h * 60) % 60)
  if h >= DAWN and h < DUSK then
    out[2] = "Day   " .. bar(h - DAWN, DUSK - DAWN) .. " " .. clock
    out[3] = "Dusk in " .. span(DUSK - h)
  else
    local night = (DAWN - DUSK) % 24   -- 12 hours
    local since = (h - DUSK) % 24      -- hours since dusk (after midnight too)
    out[2] = "Night " .. bar(since, night) .. " " .. clock
    out[3] = "Dawn in " .. span(night - since)
  end
  return out
end

-- After every step of every world. The widget is on your own screen: the client world draws it (host and guests).
emf.on_tick(function(world)
  if not world:is_client() then return end
  for i, text in ipairs(lines(world)) do
    if text ~= shown[i] then  -- most steps change nothing: then nothing is sent
      shown[i] = text
      -- the corner setting is a select ("top_right", ...); the colour a color setting ("#rrggbb")
      -- line ids "01", "02", "03": lines of one corner are listed by id (zero-padded, "10" stays after "09")
      emf.hud(string.format("%02d", i), text, emf.setting("corner"), emf.setting("color"))
    end
  end
end)

-- A setting changed (the Mods tab, EMF.exe, the console's set): send every line again, in the new corner and colour.
emf.on_setting_changed(function(key)
  shown = {}
end)
