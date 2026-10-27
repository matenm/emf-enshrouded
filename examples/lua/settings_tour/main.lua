-- settings_tour: one setting of every kind (an EMF example mod in Lua).
-- mod.json declares the settings; players change them in the console's Mods tab (the key under Esc, then Ctrl+Tab)
-- or in EMF.exe. This file reads them with emf.setting(key), each time it needs one, so a change shows at once.
-- Save this file while the game runs: EMF reloads the mod within a second.

-- Not live (mod.json leaves out "live"): read once when the mod loads. The Mods tab says "next start" for it.
local GREETING = emf.setting("greeting")

local MOODS = {"calm", "busy", "wild"}  -- the "mood" choice: emf.setting gives its index, 0 for the first entry
local shown = {}                        -- what each line shows now: a line is sent again only when it changes
local greeted = false

-- The three lines, from the settings as they are now. "" removes a line.
local function lines()
  if not emf.setting("show") then return {"", "", ""} end            -- bool: true or false
  local mood = emf.setting("mood")                                     -- choice: 0, 1 or 2
  local key = emf.setting("mood_key")                                  -- keybind: the key's name, "" for none
  return {
    string.format("%s  count %d | scale %.2f | volume %.2f",
      emf.setting("label"),                                            -- text (at most 12 characters)
      emf.setting("count"),                                            -- int: a whole number
      emf.setting("scale"),                                            -- float: a decimal number
      emf.setting("volume")),                                          -- decimal slider: a number
    string.format("mood %s (%d) | wildness %d", MOODS[mood + 1], mood,
      emf.setting("wildness")),                                        -- whole-number slider: a whole number
    string.format("corner %s | colour %s | mood key %s",
      emf.setting("corner"),                                           -- select: its option's value
      emf.setting("color"),                                            -- color: "#rrggbb"
      key ~= "" and key or "none"),
  }
end

-- After every step of every world. The lines are on your own screen, so the client world draws them.
emf.on_tick(function(world)
  if not world:is_client() then return end
  if not greeted then
    greeted = true
    if GREETING ~= "" then emf.notify(GREETING) end  -- a note at the top of the screen
  end
  for i, text in ipairs(lines()) do
    if text ~= shown[i] then
      shown[i] = text
      -- emf.hud takes the select's value as the corner and the color setting's "#rrggbb" as it is
      -- line ids "01", "02", ...: lines of one corner are listed by id, so "10" would come after "09"
      emf.hud(string.format("%02d", i), text, emf.setting("corner"), emf.setting("color"))
    end
  end
end)

-- What a setting holds now, as text for the log and the replies.
local function describe(key)
  local value = emf.setting(key)
  if key == "mood" then return value .. " (" .. MOODS[value + 1] .. ")" end
  return tostring(value)
end

-- After each change: from the Mods tab, the console's "set" command, EMF.exe, or this mod's own emf.set_setting.
-- ("enabled", the mod's own switch, comes here too: it is not one of the settings in mod.json, so emf.setting gives nil)
emf.on_setting_changed(function(key)
  if emf.setting(key) == nil then return end
  emf.log(key .. " is now " .. describe(key))
  shown = {}  -- draw every line again: the corner or the colour may have changed
end)

-- The key in the "mood_key" setting moves the mood on. A key chosen anew in the Mods tab works at once.
emf.on_key("mood_key", function(world)
  emf.set_setting("mood", (emf.setting("mood") + 1) % #MOODS)  -- saved, so the Mods tab shows it too
end)

-- settings_tour.set <key> <value>: changes a setting with the same checks as the Mods tab (a slider's range, a text's
-- length, a list's entries). emf.set_setting answers false when the value does not fit; the log says why.
emf.command("set", "<key> <value>: change one of this mod's settings", function(world, args)
  local key, value = args[1], args[2]
  if not key or not value then
    emf.reply("usage: settings_tour.set <key> <value>   (for example: settings_tour.set count 12)")
  elseif emf.setting(key) == nil then  -- nil: not one of this mod's settings
    emf.reply("no setting " .. key .. " (the console's \"settings settings_tour\" lists them)")
  elseif emf.set_setting(key, value) then
    emf.reply(key .. " is now " .. describe(key))
  else
    emf.reply("refused: " .. value .. " does not fit " .. key .. " (the log says why)")
  end
end)
