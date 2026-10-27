-- The beasts out in the world: calling (mana, cooldown, limits), dismissing, resting after a fall, and every second:
-- checking each beast is still ours, bringing beasts back when their owner is in the world (after a reload, or a
-- guest who returns), and taking them out of the world while their owner is away.
-- M.new(env) where env has: setting(key), now(world), tell(world, player, text), beasts() (the roster), save(),
-- and optionally on_spawn(world, id, species) after a beast was put into the world.
local species = require("species")

local M = {}
local P = {}
P.__index = P

local PLAYER_ALLY = 11
local PLAYER_SIDE = { [7] = true, [8] = true, [PLAYER_ALLY] = true }  -- Player, AutomatedPlayer, PlayerAlly
local STRAY_CHECKS = 3  -- a beast that guards someone else this many checks (seconds) in a row is not ours any more
local REFUSALS = 3      -- a pack beast's call-back refused this many checks in a row: its pack is not in this game

function M.new(env)
	local p = setmetatable({ env = env }, P)
	p:reset()
	return p
end

-- A new world (or a reload): entity ids from before mean nothing now.
function P:reset()
	self.ids = {}        -- entity id -> {account, beast} for every beast out with a body
	self.last_call = {}  -- account -> time of the last call
	self.bodies = {}     -- fallen beasts' bodies to remove: id -> time, or true (not timed yet)
	self.fallen = {}     -- {account, beast} fallen since the last step (to tell their owner)
	self.last = nil
	self.next_check = 0
	self.next_save = 0
	self.far = {}        -- entity id -> when it was first seen beyond leash_m
	self.moved = {}      -- entity id -> when it was last teleported back
	self.credited = {}   -- foes whose killing blow went to an owner already
	self.strays = {}     -- entity id -> checks in a row it guarded someone else
	self.refused = {}    -- beast -> checks in a row the game refused to bring it back
end

local function mmss(s)
	return string.format("%d:%02d", math.floor(s / 60), math.floor(s % 60))
end

-- Still our beast: there, alive, and an ally.
local function ours(world, id)
	if not id or not world:position(id) then return false end
	local cur = emf.health(world, id)
	if not cur or cur <= 0 then return false end
	local f = world:get("Faction", id)
	return f ~= nil and f.faction == PLAYER_ALLY
end

local function level_of(world, id)
	local l = world:get("Level", id)
	return l and l.dataStorage[1] or nil
end

function P:out_count(account)
	local n = 0
	for _, b in ipairs(self.env.beasts():list(account)) do
		if b.out then n = n + 1 end
	end
	return n
end

-- Puts the beast into the world beside its owner, as their ally at their level. Returns the id, or nil, why and
-- whether the game refused the spawn (rather than the owner having no position yet).
function P:spawn(world, owner, account, beast)
	local sp = species.by_id(beast.species)
	if not sp then return nil, "unknown species " .. tostring(beast.species) end
	local x, y, z = world:position(owner)
	if not x then return nil, "your position is not known" end
	local id = emf.spawn(world, sp.template, x + 2, y + 0.5, z)
	if not id or id == 0 then
		if sp.pack then return nil, sp.pack .. " is not installed in this game", true end
		return nil, "the game refused the call (see the log)", true
	end
	local f = world:get("Faction", id)
	if f then f.faction = PLAYER_ALLY end
	local lvl, mine = world:get("Level", id), level_of(world, owner)
	if lvl and mine then lvl.dataStorage[1] = mine end
	local scaling = world:get("DynamicEnemyScaling", id)
	if scaling then scaling.nextUpdateTime = 0 end  -- the game rescales health and damage to the new level
	emf.emit("server", "StartGuardingEvent", { guardingEntityId = id, guardedEntityId = owner })
	if self.env.on_spawn then self.env.on_spawn(world, id, sp) end
	beast.entity = id
	beast.out = true
	self.ids[id] = { account = account, beast = beast }
	return id
end

-- A beast the player just tamed (already an ally in the world).
function P:adopt(account, beast, id)
	beast.entity = id
	beast.out = true
	self.ids[id] = { account = account, beast = beast }
end

-- beastmaster.call: returns the reply text.
function P:call(world, owner, account, beast)
	local s = self.env.setting
	if beast.out then return beast.name .. " is out already." end
	for _, b in ipairs(self.env.beasts():list(account)) do
		if b.out and b.species == beast.species then
			return string.format("Your %s %s is out already: one of each kind at a time.", b.species, b.name)
		end
	end
	if beast.rest > 0 then return string.format("%s rests for %s.", beast.name, mmss(beast.rest)) end
	local n, max = self:out_count(account), s("max_out")
	if n >= max then return string.format("You have %d beasts out (at most %d): dismiss one first.", n, max) end
	local now = self.env.now(world)
	local since = now - (self.last_call[account] or -math.huge)
	if since < s("call_cooldown_s") then
		return string.format("Calling again needs a moment: wait %d s.", math.ceil(s("call_cooldown_s") - since))
	end
	local mana, max_mana = emf.attribute(world, owner, "mana"), emf.attribute(world, owner, "max_mana")
	local cost = max_mana and math.ceil(max_mana * s("call_mana_pct") / 100) or 0
	if cost > 0 and (not mana or mana < cost) then return string.format("Not enough mana: calling costs %d.", cost) end
	local id, why = self:spawn(world, owner, account, beast)
	if not id then return "The call failed: " .. why end
	if cost > 0 then emf.attribute_add(world, owner, "mana", -cost) end
	self.last_call[account] = now
	self.env.save()
	return string.format("%s the %s comes.", beast.name, beast.species)
end

-- beastmaster.dismiss: the beast leaves the world and waits at home.
function P:dismiss(world, account, beast)
	if not beast.out then return beast.name .. " is not out." end
	if beast.entity and ours(world, beast.entity) then emf.destroy(world, beast.entity) end
	if beast.entity then self.ids[beast.entity] = nil end
	beast.entity = nil
	beast.out = false
	self.env.save()
	return beast.name .. " goes home."
end

-- beastmaster.attack: every beast of `account` that is out attacks `target`.
function P:attack(world, account, target)
	local n = 0
	for _, b in ipairs(self.env.beasts():list(account)) do
		if b.out and b.entity and ours(world, b.entity) then
			emf.emit("server", "AddThreatTargetEvent",
				{ attackerId = b.entity, targetId = target, threatValue = 1000, threatPoolId = 0 })
			n = n + 1
		end
	end
	return n
end

-- XP credit: when our beast's hit leaves a foe at or under 25 % health, its owner lands the killing blow (a real game
-- hit: kill credit, XP and loot go to the owner). Once per foe; never on players or allies.
function P:on_hit(world, source, target)
	if not self.env.setting("xp_credit") then return end
	local e = self.ids[source]
	if not e or self.credited[target] then return end
	if world:get("PlayerState", target) then return end
	-- never a boss or a mini-boss (the killing blow would skip the last quarter of the fight)
	if world:get("Boss", target) or world:get("BossHealthBar", target) then return end
	local f = world:get("Faction", target)
	if not f or PLAYER_SIDE[f.faction] then return end
	local cur, max = emf.health(world, target)
	if not cur or not max or max <= 0 or cur <= 0 or cur * 100 > max * 25 then return end
	local owner = self:owner(world, e.account)
	if not owner then return end
	self.credited[target] = true
	emf.damage(world, owner, target, cur)
end

function P:on_died(id)
	self.credited[id] = nil
	local e = self.ids[id]
	if not e then return end
	self.ids[id] = nil
	e.beast.entity = nil
	e.beast.out = false
	e.beast.rest = self.env.setting("rest_s")
	self.bodies[id] = true
	self.fallen[#self.fallen + 1] = e
	self.env.save()
end

-- Brings a beast that strayed back beside its owner: beyond leash_m for 3 s, or beyond leash_far_m at once. Through
-- the game's own teleport (ground search); when that is refused, the beast is called again beside its owner.
function P:leash(world, owner, b, now)
	local ox, oy, oz = world:position(owner)
	local bx, by, bz = world:position(b.entity)
	if not ox or not bx then return end
	local d = math.sqrt((ox - bx) ^ 2 + (oy - by) ^ 2 + (oz - bz) ^ 2)
	local s = self.env.setting
	if d <= s("leash_m") then
		self.far[b.entity] = nil
		return
	end
	self.far[b.entity] = self.far[b.entity] or now
	if d <= s("leash_far_m") and now - self.far[b.entity] < 3 then return end
	if now - (self.moved[b.entity] or -math.huge) < 3 then return end  -- a teleport takes a moment: no repeats
	self.moved[b.entity] = now
	self.far[b.entity] = nil
	if not emf.teleport(world, b.entity, ox + 2, oy + 0.5, oz, { ground = true }) then
		local account
		for id, e in pairs(self.ids) do
			if id == b.entity then account = e.account end
		end
		emf.destroy(world, b.entity)
		self.ids[b.entity] = nil
		b.entity = nil
		if account then self:spawn(world, owner, account, b) end
	end
end

-- account text -> player entity, for every player in the world
local function players(world)
	local out = {}
	for _, id in ipairs(world:entities()) do
		if world:get("PlayerState", id) then
			local account = world:account(id)
			if account then out[account] = id end
		end
	end
	return out
end

-- The entity of the player with `account` in the world, or nil.
function P:owner(world, account)
	return players(world)[account]
end

function P:step(world)
	local now = self.env.now(world)
	local dt = self.last and math.max(0, now - self.last) or 0
	self.last = now
	local beasts = self.env.beasts()
	local resting = false
	for _, account in ipairs(beasts:accounts()) do
		for _, b in ipairs(beasts:list(account)) do
			if b.rest > 0 then
				b.rest = math.max(0, b.rest - dt)
				resting = true
				if b.rest == 0 then self.env.save() end
			end
		end
	end
	if resting and now >= self.next_save then
		self.next_save = now + 10
		self.env.save()  -- the rest left survives a reload
	end
	for id, due in pairs(self.bodies) do
		if due == true then
			self.bodies[id] = now + 1
		elseif now >= due then
			self.bodies[id] = nil
			local cur = emf.health(world, id)
			-- no loot from our own beasts; only while it is still a body (the game may have given the id away)
			if world:position(id) and (not cur or cur <= 0) then emf.destroy(world, id) end
		end
	end
	if now < self.next_check then return end
	self.next_check = now + 1
	local present = players(world)
	for _, e in ipairs(self.fallen) do
		local owner = present[e.account]
		if owner then
			self.env.tell(world, owner, string.format("%s fell; it rests for %s.", e.beast.name, mmss(e.beast.rest)))
		end
	end
	self.fallen = {}
	for _, account in ipairs(beasts:accounts()) do
		local owner = present[account]
		for _, b in ipairs(beasts:list(account)) do
			if b.out and b.entity then
				if not ours(world, b.entity) then
					self.ids[b.entity] = nil  -- gone, or the id names another entity now: never touched again
					b.entity = nil
					b.out = false
					self.env.save()
				elseif not owner then
					emf.destroy(world, b.entity)  -- the owner left: back when they return
					self.ids[b.entity] = nil
					b.entity = nil
				else
					local ai = world:get("DynamicEnemy01BehaviorState", b.entity)
					local guard = ai and ai.guardEntityId or 0
					if guard ~= 0 and guard ~= owner then
						-- it guards someone else: after STRAY_CHECKS in a row, the id names another entity now
						self.strays[b.entity] = (self.strays[b.entity] or 0) + 1
					else
						self.strays[b.entity] = nil
					end
					if (self.strays[b.entity] or 0) >= STRAY_CHECKS then
						self.strays[b.entity] = nil
						self.ids[b.entity] = nil  -- never touched again
						b.entity = nil
						b.out = false
						self.env.save()
					else
						self:leash(world, owner, b, now)
						if guard ~= owner then
							emf.emit("server", "StartGuardingEvent", { guardingEntityId = b.entity, guardedEntityId = owner })
						end
					end
				end
			elseif b.out and owner and b.rest == 0 then
				local id, why, refused = self:spawn(world, owner, account, b)  -- after a reload, or its owner came back
				local sp = species.by_id(b.species)
				if id then
					self.refused[b] = nil
				elseif not sp or (sp.pack and refused) then
					-- refused REFUSALS checks in a row: this game has no such beast (its pack is not installed), so it
					-- waits at home instead of being tried every second; an unknown species at once
					self.refused[b] = (self.refused[b] or 0) + 1
					if not sp or self.refused[b] >= REFUSALS then
						self.refused[b] = nil
						b.out = false
						self.env.save()
						self.env.tell(world, owner, string.format("%s stays home: %s.", b.name, why))
					end
				end
			end
		end
	end
end

return M
