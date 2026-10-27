-- Every player's collection of tamed beasts, kept with the world (one line per beast):
--   b <account> <species> <rest seconds left> <out 0|1> <name>
-- account is world:account's text ("0x" + 16 hex digits); the name is last and may hold spaces.
local M = {}
local R = {}
R.__index = R

function M.new()
	return setmetatable({ by = {} }, R)
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
	local beast = { species = species_id, name = name, rest = 0, out = false }
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
	for _, account in ipairs(self:accounts()) do
		for _, b in ipairs(self.by[account]) do
			lines[#lines + 1] = string.format("b %s %s %d %d %s", account, b.species, math.max(0, math.ceil(b.rest)),
				b.out and 1 or 0, b.name)
		end
	end
	return table.concat(lines, "\n")
end

function M.parse(text)
	local r = M.new()
	for line in (text or ""):gmatch("[^\r\n]+") do
		local account, species, rest, out, name = line:match("^b (%S+) (%S+) (%d+) ([01]) (.+)$")
		if account then
			local list = r.by[account] or {}
			list[#list + 1] = { species = species, name = name, rest = tonumber(rest), out = out == "1" }
			r.by[account] = list
		end
	end
	return r
end

return M
