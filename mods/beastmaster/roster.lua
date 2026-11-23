-- Every player's collection of tamed beasts and their mode, kept with the world (one line per beast, one per mode):
--   b <account> <species> <rest seconds left> <out 0|1> <name>
--   m <account> <passive|defending|aggressive>
--   y <account> <loyalty points> <name>   (a beast with points; none: 0)
--   a <account> <name>                    (an active beast: in one of its owner's active slots)
--   s 1                                   (the world has had a stable: from then on only active beasts are called)
--   t <account> <talent:points,...> <name> (a beast's talent points spent: "strength:2,guard:1", talents.lua)
--   g <account> <fraction> <x,y,z> <name>  (a beast in training at the Training Grounds at x,y,z, with the fraction of
--                                           a loyalty point it has earned there: training.lua)
-- account is world:account's text ("0x" + 16 hex digits); the name is last and may hold spaces. A player without an m
-- line is defending.
local talents = require("talents")

local M = {}
local R = {}
R.__index = R

M.MODES = { "passive", "defending", "aggressive" }
local MODE = { passive = true, defending = true, aggressive = true }

function M.new()
	return setmetatable({ by = {}, modes = {}, has_stable = false }, R)
end

-- Whether the world has had a stable (it stays so).
function R:stabled()
	return self.has_stable
end

function R:set_stabled()
	self.has_stable = true
end

-- How a player's beasts fight: passive, defending (the first) or aggressive.
function R:mode(account)
	return self.modes[account] or "defending"
end

-- Sets it; false for a word that is not a mode.
function R:set_mode(account, mode)
	if not MODE[mode] then return false end
	self.modes[account] = mode ~= "defending" and mode or nil
	return true
end

function R:list(account)
	return self.by[account] or {}
end

local function taken(list, name)
	for _, b in ipairs(list) do
		if b.name:lower() == name:lower() then return true end
	end
	return false
end

-- Adds a beast; nil and why when the collection holds `max` already.
function R:add(account, species_id, base_name, max)
	local list = self.by[account] or {}
	if #list >= max then return nil, string.format("the collection is full (%d beasts)", max) end
	local name, n = base_name, 1
	while taken(list, name) do
		n = n + 1
		name = base_name .. " " .. n
	end
	local beast = { species = species_id, name = name, rest = 0, out = false, points = 0 }
	list[#list + 1] = beast
	self.by[account] = list
	return beast
end

-- By position in the list (1..n) or by name (any case).
function R:find(account, key)
	local list = self.by[account] or {}
	local n = tonumber(key)
	if n then return list[n] end
	if not key then return nil end
	for _, b in ipairs(list) do
		if b.name:lower() == tostring(key):lower() then return b end
	end
	return nil
end

-- Renames a beast: 1-20 printable characters (trimmed), not a number or "all" (commands take a beast's number, and
-- dismiss takes all), not a name the collection has (any case). Nil and why when not.
function R:rename(account, beast, new_name)
	local name = tostring(new_name or ""):match("^%s*(.-)%s*$")
	if #name < 1 or #name > 20 or name:find("%c") then return nil, "A name is 1-20 characters." end
	if tonumber(name) or name:lower() == "all" then return nil, "A name cannot be a number or \"all\"." end
	for _, b in ipairs(self.by[account] or {}) do
		if b ~= beast and b.name:lower() == name:lower() then return nil, "The name " .. name .. " is taken." end
	end
	beast.name = name
	return true
end

function R:remove(account, beast)
	local list = self.by[account] or {}
	for i, b in ipairs(list) do
		if b == beast then
			table.remove(list, i)
			return true
		end
	end
	return false
end

function R:accounts()
	local out = {}
	for account in pairs(self.by) do out[#out + 1] = account end
	table.sort(out)
	return out
end

function R:serialize()
	local lines = {}
	local moded = {}
	for account in pairs(self.modes) do moded[#moded + 1] = account end
	table.sort(moded)
	for _, account in ipairs(moded) do lines[#lines + 1] = string.format("m %s %s", account, self.modes[account]) end
	for _, account in ipairs(self:accounts()) do
		for _, b in ipairs(self.by[account]) do
			lines[#lines + 1] = string.format("b %s %s %d %d %s", account, b.species, math.max(0, math.ceil(b.rest)),
				b.out and 1 or 0, b.name)
		end
	end
	for _, account in ipairs(self:accounts()) do
		for _, b in ipairs(self.by[account]) do
			if (b.points or 0) > 0 then lines[#lines + 1] = string.format("y %s %d %s", account, b.points, b.name) end
			if b.active then lines[#lines + 1] = string.format("a %s %s", account, b.name) end
			local spent = talents.format(b)
			if spent then lines[#lines + 1] = string.format("t %s %s %s", account, spent, b.name) end
			if b.training then
				lines[#lines + 1] = string.format("g %s %.4f %s %s", account, b.trained or 0, b.training, b.name)
			end
		end
	end
	if self.has_stable then lines[#lines + 1] = "s 1" end
	return table.concat(lines, "\n")
end

-- species saved by an earlier version that this one does not have: the beast loads as the species it was made from
-- (Beastmaster 1.1's panther was a small sabretooth)
local RETIRED = { panther = "sabretooth" }

function M.parse(text)
	local r = M.new()
	local loyal = {}  -- y lines, applied once every beast is read
	local active = {}  -- a lines, the same
	local spent = {}   -- t lines, the same
	local trains = {}  -- g lines, the same
	for line in (text or ""):gmatch("[^\r\n]+") do
		local y_account, points, y_name = line:match("^y (%S+) (%d+) (.+)$")
		if y_account then loyal[#loyal + 1] = { y_account, tonumber(points), y_name } end
		local a_account, a_name = line:match("^a (%S+) (.+)$")
		if a_account then active[#active + 1] = { a_account, a_name } end
		local t_account, t_points, t_name = line:match("^t (%S+) (%S+) (.+)$")
		local t_spent = t_account and talents.parse(t_points)
		if t_spent then spent[#spent + 1] = { t_account, t_spent, t_name } end  -- a field with no known talent: no line
		local g_account, g_fraction, g_key, g_name = line:match("^g (%S+) (%S+) (%S+) (.+)$")
		if g_account and tonumber(g_fraction) and g_key:match("^%-?[%d.]+,%-?[%d.]+,%-?[%d.]+$") then
			trains[#trains + 1] = { g_account, tonumber(g_fraction), g_key, g_name }
		end
		if line == "s 1" then r.has_stable = true end
		local m_account, mode = line:match("^m (%S+) (%S+)$")
		if m_account then r:set_mode(m_account, mode) end
		local account, species, rest, out, name = line:match("^b (%S+) (%S+) (%d+) ([01]) (.+)$")
		if account then
			local list = r.by[account] or {}
			species = RETIRED[species] or species
			list[#list + 1] = { species = species, name = name, rest = tonumber(rest), out = out == "1", points = 0 }
			r.by[account] = list
		end
	end
	for _, y in ipairs(loyal) do  -- by name only (a name that looks like a number is not a place in the list)
		for _, b in ipairs(r.by[y[1]] or {}) do
			if b.name == y[3] then b.points = y[2] end
		end
	end
	for _, a in ipairs(active) do
		for _, b in ipairs(r.by[a[1]] or {}) do
			if b.name == a[2] then b.active = true end
		end
	end
	for _, t in ipairs(spent) do
		for _, b in ipairs(r.by[t[1]] or {}) do
			if b.name == t[3] then b.talents = t[2] end
		end
	end
	for _, g in ipairs(trains) do
		for _, b in ipairs(r.by[g[1]] or {}) do
			if b.name == g[4] and not b.out then b.training, b.trained = g[3], math.max(0, math.min(g[2], 1)) end
		end
	end
	return r
end

return M
