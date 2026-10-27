-- My First Mod: a line of text on screen, and a key that shows and hides it.
--
-- Save this file while the game runs: EMF reloads the mod within a second ("Lua mod my_first_mod reloaded").
-- The settings are in mod.json; players change them in the console's Mods tab (the key under Esc, then Ctrl+Tab).
-- docs/modding/first_mod.md walks through this file; docs/modding/lua.md lists every emf.* function.

local shown = nil  -- what the line shows now: it is only sent again when it changes

-- The line's text: your text, then where you are (in metres).
local function line_text(world)
  local text = emf.setting("text")
  local x, y, z = world:position(emf.local_player())
  if x then
    local d = emf.setting("decimals")                 -- a slider: 0, 1 or 2
    local f = "%." .. d .. "f"
    text = text .. string.format("  (" .. f .. ", " .. f .. ", " .. f .. ")", x, y, z)
  end
  return text
end

-- After every step of every world. The line is on your own screen, so only the client world draws it.
emf.on_tick(function(world)
  if not world:is_client() then return end
  local text = ""                                     -- "" removes the line
  if emf.setting("show") then text = line_text(world) end
  if text ~= shown then
    shown = text
    -- the corner ("top_right", "bottom_right", "bottom_left") and the colour ("#ffd070") come straight from the settings
    emf.hud("line", text, emf.setting("corner"), emf.setting("color"))
  end
end)

-- The key in the "toggle_key" setting shows and hides the line. A new key chosen in the Mods tab works at once.
emf.on_key("toggle_key", function(world)
  emf.set_setting("show", not emf.setting("show"))
end)

-- A setting changed (in the Mods tab, with the console's set command, or by the key above): draw the line again.
emf.on_setting_changed(function(key)
  shown = nil
end)

-- Type my_first_mod.hello in the console.
emf.command("hello", ": say hello", function(world, args)
  emf.reply("hello from " .. emf.id)
  emf.notify("Hello from " .. emf.id .. "!")          -- a note at the top of the screen
end)
