-- Beast and stable markers (Beastmaster 1.16, EMF's markers: lua 1.25). Once a second, for every player in the world:
-- a marker over each of their beasts that is out (its name, the distance, its health as a bar (1.19, lua 1.40), an
-- arrow at the screen's edge, and on the map), and with stable markers the world's stables (they are everyone's): on the map, and in the world within 200 m.
-- The settings beast_markers and stable_markers are the host's or the server's; a player chooses for themselves with
-- /beastmaster markers on|off and /beastmaster stable_markers on|off (kept in their player data). Only what changed is
-- sent; what went (a beast dismissed, fallen or home, a stable taken down, a choice switched off) is taken off again.
-- M.new(env) where env has: setting(key), now(world) -> seconds.
local M = {}
local K = {}
K.__index = K

local STABLE_NEAR_M = 200   -- stables show in the world within this distance (always on the map)
local HEIGHT = 1.7          -- metres above a beast's position
local TALL = { bear = 2.4, aurochs = 2.4 }
local KEYS = { markers = "beast_markers", stable_markers = "stable_markers" }
local BAR_STEP = 0.05       -- the health bar moves in 5 % steps (each step is a marker sent again)
local bars = true           -- false on an older EMF, whose markers have no bar (Lua 1.40): sent without

function M.new(env)
	return setmetatable({ env = env, sent = {}, next_t = 0 }, K)
end

-- A new world: nothing was sent there yet.
function K:reset()
	self.sent, self.next_t = {}, 0
end

-- The player's own choices from their player data ("markers=on" lines); nil when they made none.
local function choices(account)
	local text = emf.player_data and emf.player_data(account) or nil
	local out = {}
	for k, v in (text or ""):gmatch("([%w_]+)=(%w+)") do out[k] = v == "on" end
	return out
end

function K:wants(account, word)
	local own = choices(account)[word]
	if own ~= nil then return own end
	return self.env.setting(KEYS[word]) and true or false
end

-- /beastmaster markers on|off and /beastmaster stable_markers on|off: the player's own choice (the answer's text).
function K:choose(account, word, value)
	if value ~= "on" and value ~= "off" then
		return string.format("/beastmaster %s on|off (now %s)", word, self:wants(account, word) and "on" or "off")
	end
	local own = choices(account)
	own[word] = value == "on"
	local lines = {}
	for k, v in pairs(own) do lines[#lines + 1] = k .. "=" .. (v and "on" or "off") end
	table.sort(lines)
	if not (emf.save_player_data and emf.save_player_data(account, table.concat(lines, "\n"))) then
		return "Your choice could not be kept (this world's data is not loaded yet); try again in a moment."
	end
	self.next_t = 0  -- applied at the next step
	local what = word == "markers" and "Markers over your beasts" or "Stable markers"
	return string.format("%s are %s for you.", what, value)
end

local function round(v) return math.floor(v + 0.5) end

-- The beast's health as a share of its maximum in BAR_STEP steps; nil when it is not known.
local function health_share(world, id)
	local cur, max = emf.health(world, id)
	if not cur or not max or max <= 0 then return nil end
	return round(math.max(0, math.min(1, cur / max)) / BAR_STEP) * BAR_STEP
end

-- Sends a marker; one with a bar goes again without it (for good) on an older EMF, which refuses the key.
local function send(player, id, spec)
	if spec.bar ~= nil and bars then
		if pcall(emf.send_marker, player, id, spec) then return end
		bars = false
	end
	spec.bar = nil
	emf.send_marker(player, id, spec)
end

-- What a player should have: id -> {spec, sig} (sig: what makes it change).
local function wanted(self, world, account, ids, stables)
	local want = {}
	if self:wants(account, "markers") then
		for id, o in pairs(ids) do
			-- a beast with no position (not placed yet, or out of the world) gets none: it would show at the world's origin
			local x, y, z = world:position(id)
			if o.account == account and o.beast and x then
				local bar = bars and health_share(world, id) or nil
				local spec = { entity = id, x = x, y = y, z = z, height = TALL[o.beast.species] or HEIGHT,
					text = o.beast.name, shape = "diamond", game_map = true, edge_arrow = true, bar = bar }
				want["beast." .. id] = { spec = spec, sig = o.beast.name .. "@" .. id .. "#" .. tostring(bar) }
			end
		end
	end
	if self:wants(account, "stable_markers") then
		for _, s in ipairs(stables) do
			if s.x and s.z then
				local key = string.format("stable.%d_%d", round(s.x), round(s.z))
				local name = s.name or "Stable"
				local spec = { x = s.x, y = s.y or 0, z = s.z, height = 1.5, text = name, shape = "square",
					game_map = true, max_distance = STABLE_NEAR_M }
				want[key] = { spec = spec, sig = name }
			end
		end
	end
	return want
end

-- Once a second on the server world: present = account -> player entity; ids = entity -> {account, beast};
-- stables = the world's stables ({x, y, z, name}).
function K:step(world, present, ids, stables)
	if not emf.send_marker then return end
	present = present or {}
	local t = self.env.now(world)
	if t < self.next_t then return end
	self.next_t = t + 1
	for account, sent in pairs(self.sent) do  -- players who left: their game is gone, the core forgets them
		local player = sent.player
		if not present[account] or present[account] ~= player then
			for id in pairs(sent.ids) do emf.unsend_marker(player, id) end
			self.sent[account] = nil
		end
	end
	for account, player in pairs(present) do
		local sent = self.sent[account]
		if not sent then
			sent = { player = player, ids = {} }
			self.sent[account] = sent
		end
		local want = wanted(self, world, account, ids or {}, stables or {})
		for id, w in pairs(want) do
			if sent.ids[id] ~= w.sig then
				send(player, id, w.spec)
				sent.ids[id] = w.sig
			end
		end
		for id in pairs(sent.ids) do
			if not want[id] then
				emf.unsend_marker(player, id)
				sent.ids[id] = nil
			end
		end
	end
end

return M
