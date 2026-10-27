-- Taming: by defeat (a beast at or under tame_health_pct, a tame_channel_s channel while you stay within
-- tame_range_m) or by bait (feed_count baits within feed_window_s, one taken from you each time).
-- M.new(env) where env has: setting(key), now(world), tell(world, player, text), on_tamed(world, player, id, species).
-- One channel per player: a new tame or feed ends the one before (its calmed beast turns wild again).
-- The beast is held for the channel (seen in game: a beaten panther ran out of range though calmed): its Movement
-- multiplier at 0, given back when the channel ends any way, and a teleport back to where it was held when it still
-- gets more than HOLD_M away from there (a player who walks away does not drag it along: the channel breaks).
local aim = require("aim")
local species = require("species")

local M = {}
local T = {}
T.__index = T

local PLAYER_ALLY = 11
local HOLD_M = 5        -- a beast being tamed this far from where it was held is put back there
local HOLD_EVERY_S = 1  -- at most one such teleport a second

function M.new(env)
	return setmetatable({ env = env, channels = {}, dead = {} }, T)
end

-- A new world (or a reload): channels on entity ids from before mean nothing now (and are never touched again).
function T:reset()
	self.channels = {}
	self.dead = {}
end

local function alive(world, id)
	local cur = emf.health(world, id)
	return cur ~= nil and cur > 0
end

-- The species of a living, wild (not Faction 11) creature that is not a player, or nil: by the animation graph its
-- template runs, its faction and its scale (species.lua).
function M.species_of(world, id)
	if world:get("PlayerState", id) then return nil end
	local f = world:get("Faction", id)
	if not f or f.faction == PLAYER_ALLY then return nil end
	if not alive(world, id) then return nil end
	local g = world:template("AnimationGraphResourceComponent2", id)
	if not g then return nil end
	local sc = world:get("ScaleComponent", id)
	return species.of(g.animationGraph, f.faction, sc and sc.uniformScale or nil)
end

-- The tameable beast the player looks at; with none under the crosshair, the nearest one within range (in a fight
-- the beast is often at the player's side, under or beside the camera). `weak_first`: a beast beaten down to the
-- taming threshold goes before a nearer healthy one.
function T:target(world, player, weak_first)
	local range = self.env.setting("tame_range_m")
	local id = aim.pick(world, player, range, function(e) return M.species_of(world, e) ~= nil end)
	if id then return id end
	local px, py, pz = world:position(player)
	if not px then return nil end
	local pct = self.env.setting("tame_health_pct")
	local best, best_d, best_weak
	for _, e in ipairs(world:entities()) do
		if e ~= player then
			local x, y, z = world:position(e)
			if x then
				local d = math.sqrt((x - px) ^ 2 + (y - py) ^ 2 + (z - pz) ^ 2)
				if d <= range and M.species_of(world, e) then
					local weak = false
					if weak_first then
						local cur, max = emf.health(world, e)
						weak = max ~= nil and max > 0 and cur * 100 <= max * pct
					end
					if not best or (weak and not best_weak) or (weak == best_weak and d < best_d) then
						best, best_d, best_weak = e, d, weak
					end
				end
			end
		end
	end
	return best
end

local function distance(world, a, b)
	local ax, ay, az = world:position(a)
	local bx, by, bz = world:position(b)
	if not ax or not bx then return math.huge end
	return math.sqrt((ax - bx) ^ 2 + (ay - by) ^ 2 + (az - bz) ^ 2)
end

-- Holds the channel's beast still: Movement's multiplier (storage [2]) at 0, the old value kept in ch.held, and its
-- place in ch.spot.
local function hold(world, ch)
	local x, y, z = world:position(ch.id)
	if x then ch.spot = { x, y, z } end
	local m = world:get("Movement", ch.id)
	if m and m.dataStorage then
		ch.held = m.dataStorage[2]
		m.dataStorage[2] = 0
	end
end

-- Gives the beast its movement back (once; only while it is still there).
local function release(world, ch)
	if ch.held == nil then return end
	local m = world:position(ch.id) and world:get("Movement", ch.id)
	if m and m.dataStorage then m.dataStorage[2] = ch.held end
	ch.held = nil
end

-- The player whose channel has the beast `id`, if any (one channel per beast: a second one would fight the first's
-- hold and could leave the beast held for good).
local function holder(self, id)
	for p, ch in pairs(self.channels) do
		if ch.id == id then return p end
	end
	return nil
end

function T:succeed(world, player, ch)
	self.channels[player] = nil
	release(world, ch)
	local f = world:get("Faction", ch.id)
	if not f then return end
	f.faction = PLAYER_ALLY
	emf.emit("server", "StartGuardingEvent", { guardingEntityId = ch.id, guardedEntityId = player })
	local cur, max = emf.health(world, ch.id)
	if cur and max and cur < max / 2 then emf.attribute_add(world, ch.id, "health", math.floor(max / 2 - cur)) end
	self.env.on_tamed(world, player, ch.id, ch.sp)
end

-- Ends the player's channel, if any: a calmed beast (or its body) turns wild again.
function T:drop(world, player)
	local ch = self.channels[player]
	self.channels[player] = nil
	if ch then release(world, ch) end
	if ch and ch.wild_faction and world:position(ch.id) then
		local f = world:get("Faction", ch.id)
		if f then f.faction = ch.wild_faction end
	end
end

function T:stop(world, player, text)
	self:drop(world, player)
	self.env.tell(world, player, text)
end

-- beastmaster.tame: start the channel on the beast in front of the player. Returns the reply text.
function T:tame(world, player)
	local id = self:target(world, player, true)
	if not id then return "There is no beast to tame near you (wolf, boar, sabretooth, hound or bear, within "
		.. self.env.setting("tame_range_m") .. " m)." end
	local sp = M.species_of(world, id)
	local other = holder(self, id)
	if other and other ~= player then return "Someone else is taming the " .. sp.id .. "." end
	local cur, max = emf.health(world, id)
	local pct = self.env.setting("tame_health_pct")
	if not max or max <= 0 or cur * 100 > max * pct then
		return string.format("The %s is too strong: bring it under %d %% health, or feed it its bait (%s) with "
			.. "beastmaster.feed.", sp.id, pct, self.env.setting("bait_" .. sp.id))
	end
	local secs = self.env.setting("tame_channel_s")
	local ch = { id = id, sp = sp, mode = "defeat", ends = self.env.now(world) + secs }
	-- the beast calms down for the channel: an ally that comes to the player, so it stops fleeing and the player's
	-- beasts leave it alone (seen in game: a called bear killed the wolf being tamed); it turns wild again on a break
	self:drop(world, player)  -- one channel at a time
	local f = world:get("Faction", id)
	if f then
		ch.wild_faction = f.faction
		f.faction = PLAYER_ALLY
		emf.emit("server", "StartGuardingEvent", { guardingEntityId = id, guardedEntityId = player })
	end
	self.channels[player] = ch
	hold(world, ch)
	return string.format("Taming the %s... stay within %d m for %d s.", sp.id, self.env.setting("tame_range_m"), secs)
end

-- beastmaster.feed: one bait into the beast in front of the player. Returns the reply text.
function T:feed(world, player)
	local id = self:target(world, player, false)
	if not id then return "There is no beast to feed near you." end
	local sp = M.species_of(world, id)
	local other = holder(self, id)
	if other and other ~= player then return "Someone else is taming the " .. sp.id .. "." end
	local now = self.env.now(world)
	local ch = self.channels[player]
	local fresh = not ch or ch.mode ~= "bait" or ch.id ~= id
	if fresh then
		self:drop(world, player)  -- one channel at a time
		ch = { id = id, sp = sp, mode = "bait", feeds = 0, first = now }
		self.channels[player] = ch
		hold(world, ch)
	end
	local bait = self.env.setting("bait_" .. sp.id)
	local need = self.env.setting("feed_count")
	-- emf.take raises an error for an item name the game does not know (a typo in the setting, a game update): that
	-- must not stop the mod
	local called, ok = pcall(emf.take, world, player, bait, 1, function(w, taken)
		if self.channels[player] ~= ch then return end  -- broken off meanwhile
		if (taken or 0) < 1 then
			self:stop(w, player, "You have no " .. bait .. " to feed the " .. sp.id .. ".")
			return
		end
		ch.feeds = ch.feeds + 1
		if ch.feeds >= need then
			self:succeed(w, player, ch)
		else
			self.env.tell(w, player, string.format("The %s eats (%d/%d).", sp.id, ch.feeds, need))
		end
	end)
	if not called then
		if fresh then self:drop(world, player) end  -- lets the beast go (its hold) too
		return string.format("The game has no item named %s: check the setting bait_%s.", tostring(bait), sp.id)
	end
	if not ok then return "Feeding did not work just now; try again." end
	return string.format("You offer the %s some %s.", sp.id, bait)
end

function T:on_died(id)
	self.dead[id] = true
end

-- Every server step: channels end, break, or time out. Out of range counts after FAR_GRACE_S in a row: a calmed beast
-- that was fleeing needs a moment to turn and come to the player (seen in game: 9 m a second after the order).
local FAR_GRACE_S = 2
function T:step(world)
	local now = self.env.now(world)
	for player, ch in pairs(self.channels) do
		local what = ch.sp.id
		local bx, by, bz = world:position(ch.id)
		local spot = ch.spot
		if spot and bx and math.sqrt((bx - spot[1]) ^ 2 + (by - spot[2]) ^ 2 + (bz - spot[3]) ^ 2) > HOLD_M
			and emf.teleport and alive(world, ch.id) and not self.dead[ch.id]
			and now - (ch.moved or -math.huge) >= HOLD_EVERY_S then
			ch.moved = now  -- also when the game refuses: no retry every step
			emf.teleport(world, ch.id, spot[1], spot[2] + 0.5, spot[3], { ground = true })
		end
		local far = distance(world, player, ch.id) > self.env.setting("tame_range_m")
		if far then ch.far_since = ch.far_since or now else ch.far_since = nil end
		if self.dead[ch.id] or not alive(world, ch.id) then
			self:stop(world, player, "The " .. what .. " died: taming stopped.")
		elseif far and now - ch.far_since >= FAR_GRACE_S then
			self:stop(world, player, "You went too far from the " .. what .. ": taming stopped.")
		elseif far then
			-- waits: it may still come back
		elseif ch.mode == "defeat" and now >= ch.ends then
			self:succeed(world, player, ch)
		elseif ch.mode == "bait" and now - ch.first > self.env.setting("feed_window_s") then
			self:stop(world, player, "The " .. what .. " lost interest: feed it faster.")
		end
	end
	self.dead = {}
end

return M
