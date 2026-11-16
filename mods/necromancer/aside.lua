-- Summons that step aside: they leave the world for a while and come back beside their owner, free, when it ends.
-- While a player builds next to them (their owner, or another player within NEAR_M): the game most likely keeps a
-- no-build zone around every creature that runs the enemy AI, whatever its faction (the program
-- add_no_buildzones_enemy, docs/ENGINE_MAP.md; not seen in game), so one at your side would block building and placing
-- there. While their owner is in the air (on the glider, or a fall of AIR_FALL_S): one brought back beside them stood in
-- the air (the game's ground search reaches about 10 m) and fell (a player's report: a wolf died at every glide). And
-- after the aside command, until it is typed again, a summons is called up, or none is left.
-- (A copy of mods/beastmaster/aside.lua: each mod ships on its own.)
-- M.new(env) where env has setting(key); the setting "make_room" switches stepping aside while building on and off.
local M = {}
local A = {}
A.__index = A

local GRACE_S = 3   -- they come back this long after the building stopped (the hammer goes back and forth)
local NEAR_M = 20   -- another player building this close to the summons, or to their owner, counts too
local AIR_FALL_S = 1.5  -- a fall (or a hook swing) this long is being in the air; a jump is not
local AIR_LAND_S = 1    -- they come back this long after their owner landed
local AIR_MOVE_M = 0.5  -- in the air only while the owner moved this far in the last AIR_STILL_S (a glide or a fall
local AIR_STILL_S = 2   --   always moves; a state the game left over on a player standing still does not count)
-- The game's movement states (DynamicLocomotion.state): in the air at once (Flying 3, HangGliding 4, BuildingFlying 11),
-- or once it lasted AIR_FALL_S (Falling 2, HookPulling 5, HookSwinging 6).
local GLIDE = { [3] = true, [4] = true, [11] = true }
local FALL = { [2] = true, [5] = true, [6] = true }

function M.new(env)
	local a = setmetatable({ env = env }, A)
	a:reset()
	return a
end

-- A new world (or a reload): no one is aside.
function A:reset()
	self.manual = {}   -- account -> true: stepped aside by the command
	self.seen = {}     -- player entity -> when it was last seen building
	self.players = {}  -- the player entities present at the last check (their cursors are read every step)
	self.away = {}     -- account -> why its summons are aside ("build", "near", "air", "command"), as of the last check
	self.told = {}     -- "account why" -> true: told once
	self.air = {}      -- player entity -> {x, y, z, moved, since, up}: where they were, when they last moved AIR_MOVE_M,
	                   -- since when they are off the ground (and moving), when they were last counted in the air
end

-- The player holds something to build or place: the game's building cursor has an item (ServerCursor, which the
-- building programs write on the server; NetworkCursor is its copy for the clients).
function M.building(world, player)
	local c = world:get("ServerCursor", player)
	if c and c.currentBuildingItemInstance.itemId ~= 0 then return true end
	local n = world:get("NetworkCursor", player)
	return n ~= nil and n.currentBuildingItemId ~= 0
end

local function recent(t, now)
	return t ~= nil and now >= t and now - t <= GRACE_S
end

local function in_air(a, now)
	return a ~= nil and a.up ~= nil and now >= a.up and now - a.up <= AIR_LAND_S
end

-- Player `p`'s movement this step (its position and the game's movement state): true when it just went into the air.
function A:watch_air(world, p, now)
	local a = self.air[p]
	if not a then
		a = {}
		self.air[p] = a
	end
	local x, y, z = world:position(p)
	if x then
		if a.x == nil then
			a.x, a.y, a.z = x, y, z
		elseif (x - a.x) ^ 2 + (y - a.y) ^ 2 + (z - a.z) ^ 2 >= AIR_MOVE_M * AIR_MOVE_M then
			a.x, a.y, a.z, a.moved = x, y, z, now
		end
	end
	local loc = world:get("DynamicLocomotion", p)
	local state = loc and loc.state
	local moving = a.moved ~= nil and now - a.moved <= AIR_STILL_S
	if not (GLIDE[state] or FALL[state]) or not moving then  -- on the ground, or a state left over while standing still
		a.since = nil
		return false
	end
	a.since = a.since or now
	if not (GLIDE[state] or now - a.since >= AIR_FALL_S) then return false end
	local was = in_air(a, now)
	a.up = now
	return not was
end

-- Every step: reads the building cursors and the movement of the players present at the last check (a few component
-- reads each). True when one of them started building or went into the air just now: the caller then checks at once
-- instead of at the next second.
function A:watch(world, now)
	local started = false
	for _, p in ipairs(self.players) do
		if self:watch_air(world, p, now) then started = true end
	end
	if not self.env.setting("make_room") then
		self.seen = {}
		return started
	end
	for _, p in ipairs(self.players) do
		if M.building(world, p) then
			if not recent(self.seen[p], now) then started = true end
			self.seen[p] = now
		end
	end
	return started
end

-- Player `p` is in the air (on the glider, or falling for AIR_FALL_S), or landed less than AIR_LAND_S ago.
function A:in_air(p, now)
	return in_air(self.air[p], now)
end

-- At each check: the players present now (account -> entity), whose cursors watch reads.
function A:set_players(present)
	local list, here = {}, {}
	for _, e in pairs(present) do
		list[#list + 1] = e
		here[e] = true
	end
	self.players = list
	for e in pairs(self.seen) do
		if not here[e] then self.seen[e] = nil end
	end
	for e in pairs(self.air) do
		if not here[e] then self.air[e] = nil end
	end
end

local function within(world, a, x, y, z)
	local ax, ay, az = world:position(a)
	return ax ~= nil and math.sqrt((ax - x) ^ 2 + (ay - y) ^ 2 + (az - z) ^ 2) <= NEAR_M
end

-- Whether `account`'s summons stay aside now, and why: "build" (their owner builds, or stopped less than GRACE_S ago),
-- "near" (another player does, within NEAR_M of the owner, where they come back, or of one of `spots`, the summons'
-- entity ids in the world), "air" (their owner is in the air, or landed less than AIR_LAND_S ago), "command" (the aside
-- command).
function A:check(world, account, owner, now, spots)
	local why
	if self.env.setting("make_room") and owner then
		if M.building(world, owner) then self.seen[owner] = now end
		if recent(self.seen[owner], now) then
			why = "build"
		else
			for _, p in ipairs(self.players) do
				if p ~= owner and recent(self.seen[p], now) then
					local x, y, z = world:position(p)
					local close = x ~= nil and within(world, owner, x, y, z)
					for _, id in ipairs(x and not close and spots or {}) do
						if within(world, id, x, y, z) then
							close = true
							break
						end
					end
					if close then
						why = "near"
						break
					end
				end
			end
		end
	end
	if not why and owner and self:in_air(owner, now) then why = "air" end
	if not why and self.manual[account] then why = "command" end
	self.away[account] = why
	return why ~= nil, why
end

-- Building or being in the air keeps them aside now ("build", "near" or "air"), whatever the command says: a call or
-- raise would only go aside.
function A:building(world, account, owner, now, spots)
	local _, why = self:check(world, account, owner, now, spots)
	return why == "build" or why == "near" or why == "air", why
end

-- The aside command: true when the summons step aside now, false when the command's aside ends.
function A:toggle(account)
	self.manual[account] = not self.manual[account] or nil
	if not self.manual[account] and self.away[account] == "command" then self.away[account] = nil end
	return self.manual[account] == true
end

-- A call, raise, gather, the Come back item, or no summons left ends the command's aside: true when it was on.
function A:release(account)
	local was = self.manual[account] == true
	self.manual[account] = nil
	if self.away[account] == "command" then self.away[account] = nil end
	return was
end

-- Aside by the command (until it ends).
function A:by_command(account)
	return self.manual[account] == true
end

-- Aside by the command, while building or while in the air (as the last check saw it).
function A:is_aside(account)
	return self.manual[account] == true or self.away[account] ~= nil
end

-- Why `account`'s summons are aside as of the last check ("build", "near", "air", "command"), or nil.
function A:why(account)
	return self.away[account]
end

-- True the first time `account`'s summons step aside for `why` (to say why once).
function A:first(account, why)
	local key = account .. " " .. tostring(why)
	if self.told[key] then return false end
	self.told[key] = true
	return true
end

return M
