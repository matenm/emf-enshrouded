-- Training: beasts their owners sent to a Training Grounds (the beasts pack's; stable.lua S:grounds()) train there and
-- earn loyalty points by the game's clock (emf.time_of_day): the setting training_points a game day (24 game hours),
-- counted once a second for the beasts of the players in the world, wherever they are (exploring counts; the beasts of
-- players who are not in the world do not train), whole points through env.gain (the loyalty note, talents and size
-- follow). A jump of the clock by more than MAX_JUMP_H hours (a clock set back, or set far ahead by a command) counts
-- nothing; a night slept through counts its hours. A beast at loyalty 10 earns nothing more.
--   A beast trains at one Training Grounds (its key: "x,y,z" to 0.1 m, from where it was first known: one found again
--   within SAME_M of a known one is that one, as places come rounded differently from a joined player's report), which
--   holds training_room beasts, all players together: sending one more is refused. A beast is sent only to a Training
--   Grounds within SEND_M of its owner. The Training Grounds the world had are kept with it ("q" lines: place and turn)
--   until a player within SEE_M of one has not seen it for GONE_S; its beasts then go back to the stable, with a note to
--   their owners in the world. A beast whose Training Grounds is not known (nothing found yet) earns nothing.
--   A beast stops training when it is called (main.lua's leave_home), brought back (back), or released (it leaves the
--   roster).
-- Kept with the world: the roster's g lines (roster.lua: the beast, its fraction of a point, its Training Grounds) and
-- this module's q lines.
-- M.new(env) where env has: now(world), setting(key), beasts() (the roster), grounds() (stable.lua S:grounds(): the
-- Training Grounds found now, {x, y, z, q}), present() (account -> player entity, nil before the first check),
-- gain(world, account, beast) (a loyalty point), tell(world, player, text), save(), log(text).
local loyalty = require("loyalty")

local M = {}
local T = {}
T.__index = T

M.MAX_JUMP_H = 12  -- game hours: a bigger jump of the clock in one check counts nothing (it was set, not passed)
M.SEND_M = 50      -- m: a beast is sent only to a Training Grounds this near its owner
local SAME_M = 1   -- m: a Training Grounds found this near a known one is that one
local SEE_M = 30   -- m: a player this near a known Training Grounds would see it
local GONE_S = 30  -- s: not seen that long by a player near it: it was taken down
local SAVE_S = 60  -- s: the fractions earned are saved at most this often (a point saves at once)

function M.new(env)
	local t = setmetatable({ env = env }, T)
	t:reset()
	return t
end

-- A new world (or a reload): the known Training Grounds come from the world's data (load).
function T:reset()
	self.known = {}      -- key -> {x, y, z, q}
	self.missing = {}    -- key -> since when a player near it did not see it
	self.last_hour = nil -- the game's clock at the last check
	self.next_check = 0
	self.dirty_at = nil  -- a fraction changed since the last save, first at this time
end

-- The key of Training Grounds `g` ({x, y, z}).
function M.key(g)
	return string.format("%.1f,%.1f,%.1f", g.x, g.y, g.z)
end

local function place_of(key)
	local x, y, z = key:match("^(%-?[%d.]+),(%-?[%d.]+),(%-?[%d.]+)$")
	return tonumber(x), tonumber(y), tonumber(z)
end

-- The q lines of the world's data: "q <x> <y> <z> <qx> <qy> <qz> <qw>" or "q <x> <y> <z> -".
function T:load(text)
	self:reset()
	for line in (text or ""):gmatch("[^\r\n]+") do
		local x, y, z, rest = line:match("^q (%S+) (%S+) (%S+) (.+)$")
		x, y, z = tonumber(x), tonumber(y), tonumber(z)
		if x and y and z then
			local g = { x = x, y = y, z = z }
			local qx, qy, qz, qw = rest:match("^(%S+) (%S+) (%S+) (%S+)$")
			qx, qy, qz, qw = tonumber(qx), tonumber(qy), tonumber(qz), tonumber(qw)
			if qx and qy and qz and qw then g.q = { x = qx, y = qy, z = qz, w = qw } end
			self.known[M.key(g)] = g
		end
	end
end

function T:serialize()
	local keys = {}
	for k in pairs(self.known) do keys[#keys + 1] = k end
	table.sort(keys)
	local lines = {}
	for _, k in ipairs(keys) do
		local g = self.known[k]
		local q = g.q and string.format("%.4f %.4f %.4f %.4f", g.q.x, g.q.y, g.q.z, g.q.w) or "-"
		lines[#lines + 1] = string.format("q %.1f %.1f %.1f %s", g.x, g.y, g.z, q)
	end
	return table.concat(lines, "\n")
end

-- The known Training Grounds, {x, y, z, q, key}, sorted by key.
function T:grounds()
	local out = {}
	for k, g in pairs(self.known) do out[#out + 1] = { x = g.x, y = g.y, z = g.z, q = g.q, key = k } end
	table.sort(out, function(a, b) return a.key < b.key end)
	return out
end

function T:known_at(key)
	return self.known[key]
end

-- The key of Training Grounds `g` ({x, y, z}): a known one's within SAME_M, else its own.
function T:key_of(g)
	for k, kg in pairs(self.known) do
		if (kg.x - g.x) ^ 2 + (kg.y - g.y) ^ 2 + (kg.z - g.z) ^ 2 <= SAME_M * SAME_M then return k end
	end
	return M.key(g)
end

-- How many beasts (all players) train at the Training Grounds `key`.
function T:count(key)
	local n = 0
	local roster = self.env.beasts()
	for _, account in ipairs(roster:accounts()) do
		for _, b in ipairs(roster:list(account)) do
			if b.training == key then n = n + 1 end
		end
	end
	return n
end

function T:room()
	local r = tonumber(self.env.setting("training_room")) or 2
	return math.max(1, math.min(4, math.floor(r)))
end

-- The known Training Grounds within `within` metres of (x, y, z) nearest it with room, or nil; and the nearest of them
-- with room or not (nil when none is that near).
function T:nearest(x, y, z, within)
	local best, best_d, any, any_d
	for k, g in pairs(self.known) do
		local d = (g.x - x) ^ 2 + (g.y - y) ^ 2 + (g.z - z) ^ 2
		if d > within * within then goto continue end
		if not any_d or d < any_d then any, any_d = k, d end
		if self:count(k) < self:room() and (not best_d or d < best_d) then best, best_d = k, d end
		::continue::
	end
	return best, any
end

-- "x, y, z" of the Training Grounds `key`, rounded, for the replies.
function T:where(key)
	local x, y, z = place_of(key)
	if not x then return key end
	return string.format("%d, %d, %d", math.floor(x + 0.5), math.floor(y + 0.5), math.floor(z + 0.5))
end

-- Sends beast `b` to the Training Grounds `key`: true, or false and why.
function T:send(b, key)
	if b.out then return false, "out" end
	if not self.known[key] then return false, "unknown" end
	if b.training == key then return true end
	if self:count(key) >= self:room() then return false, "full" end
	b.training, b.trained = key, 0
	self.env.save()
	return true
end

-- Beast `b` stops training (brought back, called, sent away): true when it was training.
function T:back(b)
	if not b.training then return false end
	b.training, b.trained = nil, nil
	self.env.save()
	return true
end

-- Game hours since the last check (0 the first time or without a clock), a jump counting MAX_JUMP_H at most.
function T:hours(world)
	local h = emf.time_of_day and emf.time_of_day(world)
	if not h then return 0 end
	local last = self.last_hour
	self.last_hour = h
	if not last then return 0 end
	local d = h - last
	if d < 0 then d = d + 24 end
	if d > M.MAX_JUMP_H then return 0 end  -- set back, or set far ahead: no time passed for the beasts
	return d
end

-- The Training Grounds found now join the known ones; a known one a player near it has not seen for GONE_S is gone:
-- its beasts go back to the stable.
function T:refresh(world, now)
	local found, changed = {}, false
	for _, g in ipairs(self.env.grounds() or {}) do
		local k = self:key_of(g)
		found[k] = true
		if not self.known[k] then
			changed = true
			self.known[k] = { x = g.x, y = g.y, z = g.z, q = g.q }
		elseif g.q and not self.known[k].q then
			self.known[k].q = g.q
			changed = true
		end
	end
	local present = self.env.present() or {}
	for k, g in pairs(self.known) do
		if found[k] then
			self.missing[k] = nil
		else
			local near = false
			for _, player in pairs(present) do
				local px, py, pz = world:position(player)
				if px and (px - g.x) ^ 2 + (py - g.y) ^ 2 + (pz - g.z) ^ 2 <= SEE_M * SEE_M then near = true end
			end
			if not near then
				self.missing[k] = nil
			elseif not self.missing[k] then
				self.missing[k] = now
			elseif now - self.missing[k] >= GONE_S then
				self:gone(world, k, present)
				changed = true
			end
		end
	end
	if changed then self.env.save() end
end

-- The Training Grounds `key` was taken down: its beasts go back to the stable.
function T:gone(world, key, present)
	self.known[key], self.missing[key] = nil, nil
	local roster = self.env.beasts()
	for _, account in ipairs(roster:accounts()) do
		for _, b in ipairs(roster:list(account)) do
			if b.training == key then
				b.training, b.trained = nil, nil
				local owner = present[account]
				if owner and self.env.tell then
					self.env.tell(world, owner, string.format("%s's Training Grounds is gone: it went back to the stable.",
						b.name))
				end
			end
		end
	end
	self.env.log("training: the Training Grounds at " .. key .. " is gone; its beasts went back to the stable")
end

-- The points of the game hours `hours` for the beasts in training of the players in the world.
function T:count_points(world, hours, now)
	if hours <= 0 then return end
	local per_day = tonumber(self.env.setting("training_points")) or 3
	if per_day <= 0 then return end
	local pace = self.env.setting("loyalty_pace")
	local roster = self.env.beasts()
	for account in pairs(self.env.present() or {}) do
		for _, b in ipairs(roster:list(account)) do
			if b.training and self.known[b.training] then
				if loyalty.level(b.points, pace) >= loyalty.MAX then
					b.trained = 0
				else
					b.trained = (b.trained or 0) + hours * per_day / 24
					self.dirty_at = self.dirty_at or now
					while b.trained >= 1 and loyalty.level(b.points, pace) < loyalty.MAX do
						b.trained = b.trained - 1
						self.env.gain(world, account, b)  -- saves
						self.dirty_at = nil
					end
				end
			end
		end
	end
end

-- Every step of the server world; once a second: the clock, the Training Grounds, the points.
function T:step(world)
	local now = self.env.now(world)
	if now < self.next_check then return end
	self.next_check = now + 1
	local hours = self:hours(world)
	self:refresh(world, now)
	self:count_points(world, hours, now)
	if self.dirty_at and now - self.dirty_at >= SAVE_S then
		self.dirty_at = nil
		self.env.save()
	end
end

-- Where `account`'s beasts train (the console's beastmaster.train with no name).
function T:text(account)
	local lines = {}
	local pace = self.env.setting("loyalty_pace")
	for _, b in ipairs(self.env.beasts():list(account)) do
		if b.training then
			local x, y, z = place_of(b.training)
			local at = x and string.format("%d, %d, %d", math.floor(x + 0.5), math.floor(y + 0.5), math.floor(z + 0.5))
				or b.training
			local lv = loyalty.level(b.points, pace)
			lines[#lines + 1] = string.format("%s trains at the Training Grounds at %s (loyalty %d%s)%s", b.name, at, lv,
				lv < loyalty.MAX and string.format(", %d %% of a point", math.floor((b.trained or 0) * 100)) or ", the most",
				self.known[b.training] and "" or ": that Training Grounds is not found yet")
		end
	end
	if #lines == 0 then return "None of your beasts is training." end
	return table.concat(lines, "\n")
end

return M
