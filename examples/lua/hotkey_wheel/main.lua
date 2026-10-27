-- hotkey_wheel: a key opens a radial wheel (an EMF example mod in Lua). Hold F3 (or the key you bind in the Mods tab),
-- move the mouse to a time of day and let go: the world's clock jumps there. Only the host can change the time.
--
-- How it sets the time: Lua has no call for that. EMF's console command "time <hours>" does it through the game's own
-- setTimeOfDay, and EMF runs each line written into <game>\emf\commands.txt within a second, as if it was typed into
-- the console. So a pick writes "time 6" into that file.
-- Careful with this trick: a line in that file runs as the console, with every right the host has (give, spawn,
-- destroy...). Write only fixed text of your own, never words a player typed, and never from a command marked
-- {players = true}. A line waits until a world you host runs. (The hand-off is safe: EMF renames the file before it
-- reads it, and while this mod has it open the rename fails and is tried again a second later.)

-- The wheel's entries. "Name (details)": the wheel shows the details on a second line.
local TIMES = {
  {label = "Dawn (06:00)", name = "Dawn", hours = 6},
  {label = "Noon (12:00)", name = "Noon", hours = 12},
  {label = "Dusk (18:00)", name = "Dusk", hours = 18},
  {label = "Midnight (00:00)", name = "Midnight", hours = 0},
}

-- <game>\emf\commands.txt: this mod's own folder (emf.dir) is <game>\emf\mods\hotkey_wheel.
local COMMANDS = emf.dir .. "\\..\\..\\commands.txt"

local function run_console_command(line)
  local f = io.open(COMMANDS, "a")  -- "a" adds to the end: other lines may be waiting there
  if not f then return false end
  f:write(line, "\n")
  f:close()
  emf.log("wrote \"" .. line .. "\" into emf\\commands.txt")
  return true
end

-- The wheel's answer, on your client world: choice is 1-4, or nil when you cancelled (Esc or a right click).
local function picked(world, choice)
  local t = TIMES[choice]
  if not t then return end
  if run_console_command("time " .. t.hours) then
    emf.notify("Time of day: " .. t.name .. " (EMF's time command runs within a second)")
  else
    emf.notify("Could not write " .. COMMANDS)
  end
end

-- The key in the "wheel_key" setting. In a world you host this runs on the server world; in someone else's game on
-- your client world, and their clock is not yours to change.
emf.on_key("wheel_key", function(world)
  if not world:is_server() then
    emf.notify("Only the host can change the time of day")
    return
  end
  local labels = {}
  for i, t in ipairs(TIMES) do labels[i] = t.label end
  -- Passing the same key: held while the wheel opens, letting go picks the entry under the mouse (a quick tap: click
  -- one). "" would mean click only.
  emf.wheel("Time of day", labels, emf.setting("wheel_key"), picked)
end)
