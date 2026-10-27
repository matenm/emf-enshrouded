-- Rare wild pack beasts (the bear pack's bear, the beasts pack's aurochs and panther): every wild_bear_minutes of play,
-- when no wild beast of ours is about, one of them (at random) comes 50-70 m from a random player who is out in the
-- open (not within 40 m of a flame altar), on a spot that is not within 40 m of an altar either: at the player's height
-- + 2 m, then onto the ground there (the game's ground search), hostile, at that player's level. It is tamed like any
-- beast (species.lua knows a pack beast by its scale). A pack the game does not have (no template): that beast is
-- skipped for the session, with one log line.
-- M.new(env) where env has: setting(key), now(world), and optionally on_spawn(world, id, species).
local species = require("species")

local M = {}
local W = {}
W.__index = W

local PLAYER_ALLY = 11
local BASE_RADIUS = 40     -- metres around a flame altar where no bear comes (as the Wandering Champion's rule)
local SPOT_TRIES = 8       -- random spots tried around the player before giving up until the next time
local GROUND_AFTER_S = 2   -- the game refuses to teleport a creature in its first moments ("dead": not set up yet)

function M.new(env)
	local w = setmetatable({ env = env, off = {} }, W)  -- off: species id -> true when the game has no such template
	w:reset()
	return w
end

-- A new world (or a reload): the beast from before is gone (spawned creatures are not saved) and the wait starts again.
function W:reset()
	self.id = nil
	self.sp = nil
	self.next = nil
	self.ground = nil
end

-- Our wild beast is still about: there, alive, not tamed, and still that beast at its pack's size (the id was not
-- given to another creature).
local function about(world, id, sp)
	if not id or not sp or not world:position(id) then return false end
	local cur = emf.health(world, id)
	if not cur or cur <= 0 then return false end
	local f = world:get("Faction", id)
	local sc = world:get("ScaleComponent", id)
	if f == nil or f.faction == PLAYER_ALLY or sc == nil then return false end
	return sc.uniformScale >= (sp.min_scale or 0) and sc.uniformScale < (sp.max_scale or math.huge)
end

-- The pack beasts this game may have, in a random order.
local function candidates(off)
	local out = {}
	for _, sp in ipairs(species.list) do
		if sp.pack and not off[sp.id] then out[#out + 1] = sp end
	end
	for i = #out, 2, -1 do
		local j = math.random(i)
		out[i], out[j] = out[j], out[i]
	end
	return out
end

local function players(world)
	local out = {}
	for _, id in ipairs(world:entities()) do
		if world:get("PlayerState", id) then out[#out + 1] = id end
	end
	return out
end

-- The flame altars' positions: {x, z} for each entity with BaseIdComponent.
local function altars(world)
	local out = {}
	for _, id in ipairs(world:entities()) do
		if world:get("BaseIdComponent", id) then
			local x, _, z = world:position(id)
			if x then out[#out + 1] = { x, z } end
		end
	end
	return out
end

local function near_base(bases, x, z)
	for _, b in ipairs(bases) do
		if (b[1] - x) ^ 2 + (b[2] - z) ^ 2 <= BASE_RADIUS * BASE_RADIUS then return true end
	end
	return false
end

-- A spot 50-70 m from a random player out in the open, away from every base; nil when there is none this time.
local function spot(world)
	local bases = altars(world)
	local open = {}
	for _, p in ipairs(players(world)) do
		local x, _, z = world:position(p)
		if x and not near_base(bases, x, z) then open[#open + 1] = p end
	end
	if #open == 0 then return nil end
	local p = open[math.random(#open)]
	local px, py, pz = world:position(p)
	for _ = 1, SPOT_TRIES do
		local a, d = math.random() * 2 * math.pi, 50 + math.random() * 20
		local x, z = px + d * math.sin(a), pz + d * math.cos(a)
		if not near_base(bases, x, z) then return p, x, py + 2, z end
	end
	return nil
end

function W:step(world)
	local s = self.env.setting
	if not s("wild_bears") then return end
	local now = self.env.now(world)
	local g = self.ground
	if g and now >= g.at then
		self.ground = nil
		if emf.teleport and about(world, g.id, self.sp) then emf.teleport(world, g.id, g.x, g.y, g.z, { ground = true }) end
	end
	local every = math.max(1, s("wild_bear_minutes")) * 60
	if not self.next then
		self.next = now + every
		return
	end
	if now < self.next then return end
	self.next = now + every
	if about(world, self.id, self.sp) then return end
	local pool = candidates(self.off)
	if #pool == 0 then return end
	local p, x, y, z = spot(world)
	if not p then return end
	local id, sp
	for _, c in ipairs(pool) do
		id = emf.spawn(world, c.template, x, y, z)
		if id and id ~= 0 then
			sp = c
			break
		end
		self.off[c.id] = true
		emf.log(string.format("wild %s are off: the game has no %s (install %s to meet them)", c.plural, c.id, c.pack))
	end
	if not sp then return end
	self.id, self.sp = id, sp
	local lvl, mine = world:get("Level", id), world:get("Level", p)
	if lvl and mine then lvl.dataStorage[1] = mine.dataStorage[1] end
	local scaling = world:get("DynamicEnemyScaling", id)
	if scaling then scaling.nextUpdateTime = 0 end  -- the game rescales health and damage to the new level
	self.ground = { id = id, x = x, y = y, z = z, at = now + GROUND_AFTER_S }
	if self.env.on_spawn then self.env.on_spawn(world, id, sp) end
end

return M
