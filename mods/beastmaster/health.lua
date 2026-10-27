-- Tougher beasts: a species with health_mul (the bear: 2.5) keeps that many times the game's base health.
-- The game's Health attribute storage: [1] health, [2] min, [3] Health_Max_Base (the game sets it from the level),
-- [4] Health_Max_Adder, ..., [8] the maximum, which the game computes as base + adder (seen in game: a level-1 wolf with
-- base 34 and adder 51 had 85 of 85). So the adder is kept at (mul - 1) x base, also after the game rescales the base
-- for another level. (DynamicEnemyScaling.initialBaseMaxHealth stays 0 for these creatures, and writing it does
-- nothing.)
local M = {}
local H = {}
H.__index = H

local WAIT_S = 10  -- a beast not in the world this long after its spawn is dropped

function M.new()
	return setmetatable({ watched = {} }, H)
end

-- A new world (or a reload): entity ids from before mean nothing now.
function H:reset()
	self.watched = {}
end

-- Watch a beast just put into the world.
function H:watch(world, id, sp)
	if not sp or not sp.health_mul or sp.health_mul == 1 then return end
	self.watched[id] = { mul = sp.health_mul, until_s = world:tick() / 60 + WAIT_S }
end

function H:step(world)
	local now = world:tick() / 60
	for id, w in pairs(self.watched) do
		local h = world:position(id) and world:get("Health", id)
		if not h then
			if w.seen or now > w.until_s then self.watched[id] = nil end  -- gone (died, dismissed) or never came
		else
			w.seen = true
			local base = h.dataStorage[3]
			if base and base > 0 then
				local want = math.floor(base * (w.mul - 1) + 0.5)
				if h.dataStorage[4] ~= want then h.dataStorage[4] = want end
			end
		end
	end
end

return M
