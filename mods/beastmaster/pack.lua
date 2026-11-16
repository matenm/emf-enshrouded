-- The beasts out in the world: calling (mana, cooldown, limits), dismissing, resting after a fall (and coming back
-- after it by themselves while back_after_rest is on), and every second: checking each beast is still ours, bringing
-- beasts back when their owner is in the world (after a reload, or a guest who returns), and taking them out of the
-- world while their owner is away or they step aside (aside.lua: while a player builds next to them, while their owner
-- is in the air, or after beastmaster.aside).
-- M.new(env) where env has: setting(key), now(world), tell(world, player, text), beasts() (the roster), save(),
-- slots(world, owner) (how many beasts the owner may have out: their active slots), stabled() (the world has had a
-- stable: only active beasts are called), and optionally on_spawn(world, id, species, health_mul) after a beast was
-- put into the world or its loyalty grew, leave_home(world, beast) before a beast is put into the world (it leaves the
-- Large Stable: home.lua) and at_home(id) (a beast at home in a Large Stable: idle.lua leaves it asleep).
local species = require("species")
local loyalty = require("loyalty")
local aside_mod = require("aside")
local talents = require("talents")
local fight_mod = require("fight")
local threat_mod = require("threat")
local idle_mod = require("idle")
local stable_mod = require("stable")

local M = {}
local P = {}
P.__index = P

local PLAYER_ALLY = 11
local AI_IDLE, AI_COMBAT = 2, 6  -- enemy01 states (DynamicEnemy01BehaviorState.state)
local PLAYER_SIDE = { [7] = true, [8] = true, [PLAYER_ALLY] = true }  -- Player, AutomatedPlayer, PlayerAlly
-- The factions aggressive beasts go after: Fogger, Vukah, Scavenger, WildBeast, Skeleton, Cyclops, Drak, Ancients (never
-- wildlife or farm animals: goats and hares, a player's animals)
local HOSTILE = { [2] = true, [3] = true, [4] = true, [6] = true, [10] = true, [13] = true, [14] = true, [15] = true }
local SENT_S = 20       -- a passive beast that Attack sent at a creature fights it this long
local RECALL_S = 3      -- a beast that dropped a chase fights nothing this long while it comes back
local HEAL_AFTER_S = 5  -- a beast heals between fights: this long after its last hit to or from it,
local HEAL_SHARE = 0.05 -- this share of its maximum health a second
local KILL_HIT_S = 30   -- a foe a beast hit this long before it dies counts as its kill (a loyalty point)
local STRAY_CHECKS = 3  -- a beast that guards someone else this many checks (seconds) in a row is not ours any more
local REFUSALS = 3      -- a pack beast's call-back refused this many checks in a row: its pack is not in this game

function M.new(env)
	local p = setmetatable({ env = env, aside = aside_mod.new({ setting = env.setting }) }, P)
	p.threat = threat_mod.new({
		now = env.now,
		setting = env.setting,
		tell = env.tell,
		ids = function() return p.ids end,
		mode = function(account) return env.mode and env.mode(account) or "defending" end,
		hostile = function(world, id) return M.hostile(world, id) end,
		boss = function(world, id) return M.boss(world, id) end,
	})
	p.fight = fight_mod.new({
		now = env.now,
		tell = env.tell,
		owner = function(world, account) return p:owner(world, account) end,
		strike = function(world, e, id, target, amount) p:strike(world, e, id, target, amount) end,
	})
	p.idle = idle_mod.new({
		now = env.now,
		setting = env.setting,
		ids = function() return p.ids end,
		busy = function(world, id)
			local now = env.now(world)
			return now < (p.sent[id] or 0) or now < (p.recall[id] or 0)
		end,
		held = function(account) return p.threat:holds(account) end,
		building = function(world, account, owner) return aside_mod.building(world, owner) or p.aside:is_aside(account) end,
		called_at = function(id) return p.called[id] end,
		kept = function(id) return env.at_home ~= nil and env.at_home(id) end,
	})
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
	self.walked = {}     -- entity id -> true: on another mod's move at the last check
	self.refused = {}    -- beast -> checks in a row the game refused to bring it back
	self.sent = {}       -- entity id -> until when Attack's order holds (a passive beast fights it until then)
	self.orders = {}     -- entity id -> the creature Attack sent it at (written as its pick until the order ends)
	self.recall = {}     -- entity id -> until when a beast that dropped a chase fights nothing
	self.base = {}       -- entity id -> {damage, wrote}: its template's damage multiplier, and the one written
	self.fought = {}     -- entity id -> when it last hit or was hit (it heals HEAL_AFTER_S after that)
	self.hits = {}       -- hostile creature id -> {beast entity id -> when it hit it} (loyalty for the kill)
	self.speeds = {}     -- entity id -> {base, wrote} by Movement entry: the game's values and Swiftness's (talents.lua)
	self.present = nil   -- account -> player entity, for the players in the world (as of the last check)
	self.called = {}     -- entity id -> when Beastmaster spawned it (idle.lua's clock: its SpawnTime is the game's then)
	self.back = {}       -- beast -> true: it fell and comes back after its rest (its owner hears when it is back)
	self.aside:reset()
	self.fight:reset()
	self.threat:reset()
	self.idle:reset()
end

local function mmss(s)
	return string.format("%d:%02d", math.floor(s / 60), math.floor(s % 60))
end

local function plural(n, word)
	return string.format("%d %s%s", n, word, n == 1 and "" or "s")
end

-- Why a pack beast cannot come: the game has no such creature (its pack is not installed, or its content was not built
-- at the game's start).
local function no_pack(sp)
	return string.format("%s come with the EMF creature packs", sp.plural or (sp.id .. "s"))
end
local INSTALL = "Install them with EMF.exe (Install a downloaded file) and restart the game; EMF.exe's EML content " ..
	"card says whether they were built."

-- What Attack may send beasts at: a living creature, never a player, an automated player, an ally (a beast) or a farm
-- animal (a player's), nor one with no side; a wild animal is fine (when the player looks right at it).
local NEVER_FOUGHT = { [0] = true, [7] = true, [8] = true, [11] = true, [12] = true }
function M.fightable(world, id)
	if not world:position(id) or world:get("PlayerState", id) then return false end
	local f = world:get("Faction", id)
	if not f or NEVER_FOUGHT[f.faction] then return false end
	local cur = emf.health(world, id)
	return cur ~= nil and cur > 0
end

-- A living hostile creature (HOSTILE), never a player.
function M.hostile(world, id)
	local f = world:get("Faction", id)
	if not (f and HOSTILE[f.faction]) or world:get("PlayerState", id) then return false end
	local cur = emf.health(world, id)
	return cur ~= nil and cur > 0
end

-- A boss or a mini-boss.
function M.boss(world, id)
	return world:get("Boss", id) ~= nil or world:get("BossHealthBar", id) ~= nil
end

-- Clears what a beast's AI fights (every aggro target and every chosen target), so it turns from a fight: the game
-- picks targets again from what it senses and what hits it, so a passive beast is cleared every step.
-- `players_only`: only its fights against the player side (players and their allies), as for a beast just tamed (it
-- may still fight a monster that bites it: seen, a wild wolf killed a tamed one kept from fighting back).
local function calm(world, id, players_only)
	local function clears(e)
		if not players_only then return true end
		if not e or e == 0 then return false end
		if world:account(e) then return true end
		local f = world:get("Faction", e)
		return f ~= nil and PLAYER_SIDE[f.faction] == true
	end
	local a = world:get("DynamicAggro", id)
	if a and a.targets then
		for i = 1, #a.targets do
			local t = a.targets[i]
			if (t.state ~= 0 or t.aggro ~= 0) and clears(t.entityId) then
				t.state = 0
				t.aggro = 0
			end
		end
	end
	local s = world:get("SelectedTargets", id)
	if s and s.targets then
		for i = 1, #s.targets do
			local t = s.targets[i]
			if t.targetEntityId ~= 0 and clears(t.targetEntityId) then
				t.targetEntityId = 0
				t.targetRootEntityId = 0
			end
		end
	end
	-- a beast left in the game's Combat state with nothing to fight stands there (seen: a tamed wolf stood 15 m from
	-- its owner); from Idle the game has it guard its owner again (seen: it came at once)
	local ai = world:get("DynamicEnemy01BehaviorState", id)
	local fights = s and s.targets and s.targets[1] and s.targets[1].targetEntityId ~= 0
	if ai and ai.state == AI_COMBAT and not fights then ai.state = AI_IDLE end
end

M.calm = calm

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
	if self.env.leave_home then self.env.leave_home(world, beast) end  -- out of the Large Stable first
	local size = loyalty.spawn_size(sp, beast.points, self.env.setting("loyalty_pace"))
	local id = emf.spawn(world, sp.template, x + 2, y + 0.5, z, size)
	if not id or id == 0 then
		if sp.pack then return nil, no_pack(sp), true end
		return nil, "the game refused the call (see the log)", true
	end
	local f = world:get("Faction", id)
	if f then f.faction = PLAYER_ALLY end
	local lvl, mine = world:get("Level", id), level_of(world, owner)
	if lvl and mine then lvl.dataStorage[1] = mine end
	local scaling = world:get("DynamicEnemyScaling", id)
	if scaling then scaling.nextUpdateTime = 0 end  -- the game rescales health and damage to the new level
	emf.emit("server", "StartGuardingEvent", { guardingEntityId = id, guardedEntityId = owner })
	self:fresh(id)
	self.fought[id] = self.env.now(world)
	self.called[id] = self.env.now(world)
	self:strengthen(world, id, beast)
	beast.entity = id
	beast.out = true
	self.ids[id] = { account = account, beast = beast }
	return id
end

-- A beast the player just tamed (already an ally in the world).
-- A fresh entity for a beast: whatever was kept for an earlier creature on the same id (the game gives ids out again)
-- goes.
function P:fresh(id)
	self.base[id], self.sent[id], self.recall[id], self.fought[id], self.orders[id] = nil, nil, nil, nil, nil
	self.walked[id] = nil
	self.speeds[id], self.called[id] = nil, nil
	self.fight:forget(id)
end

-- When the game sets a beast's damage multiplier again (its rescale to a level), its role and loyalty go on top of the
-- game's new value.
function P:keep_damage(world, id, beast)
	local base = self.base[id]
	local dm = base and base.wrote and world:get("DamageMod", id)
	if dm and dm.dataStorage and math.abs(dm.dataStorage[3] - base.wrote) > 1e-4 then
		base.damage = dm.dataStorage[3]
		self:strengthen(world, id, beast)
	end
end

-- The game's speeds back on `id` (Swiftness written on it before), each while it still has the value written last (else
-- the game wrote its own); nothing more is kept for it.
function P:give_back_speed(world, id)
	local kept = self.speeds[id]
	self.speeds[id] = nil
	local m = kept and world:get("Movement", id)
	if not (m and m.dataStorage) then return end
	for _, i in ipairs(talents.SPEED_ENTRIES) do
		if kept.wrote[i] and math.abs(m.dataStorage[i] - kept.wrote[i]) <= 1e-4 then m.dataStorage[i] = kept.base[i] end
	end
end

-- A beast out at its Swiftness: the Movement entries talents.SPEED_ENTRIES at the game's values times its speed. The
-- game's values are kept from the first write and taken again when the game writes them anew (kept up every step); a
-- beast without Swiftness is left as the game made it (after a reset, from the step its game values are back). Only on
-- the players' ally: an id that is not (no beast of ours now) gets the game's speeds back.
function P:set_speed(world, id, beast)
	local factor = talents.speed(beast)
	local kept = self.speeds[id]
	if factor == 1 and not kept then return end
	local f = world:get("Faction", id)
	if not (f and f.faction == PLAYER_ALLY) then
		self:give_back_speed(world, id)
		return
	end
	local m = world:get("Movement", id)
	if not (m and m.dataStorage) then return end
	if not kept then
		kept = { base = {}, wrote = {} }
		for _, i in ipairs(talents.SPEED_ENTRIES) do kept.base[i] = m.dataStorage[i] end
		self.speeds[id] = kept
	end
	for _, i in ipairs(talents.SPEED_ENTRIES) do
		local cur = m.dataStorage[i]
		if kept.wrote[i] and math.abs(cur - kept.wrote[i]) > 1e-4 then kept.base[i] = cur end  -- the game wrote it anew
		local want = kept.base[i] * factor
		if math.abs(cur - want) > 1e-4 then m.dataStorage[i] = want end
		kept.wrote[i] = want
	end
	if factor == 1 then self.speeds[id] = nil end
end

-- A tamed beast's health against a wild one of its kind at the same level: its role, loyalty and talents.
function M.health_mul(beast, pace)
	local role_health = species.role(species.by_id(beast.species))
	return role_health * loyalty.factor(loyalty.level(beast.points, pace)) * talents.health(beast)
end

-- A beast out as strong as its role, loyalty and talents: its health through the health watch (on_spawn with the
-- multiplier), its DamageMod multiplier against its template's, kept from the first time, and its speed. (Its size is
-- given at the spawn.)
function P:strengthen(world, id, beast)
	local sp = species.by_id(beast.species)
	local _, role_damage = species.role(sp)
	local lvl = loyalty.level(beast.points, self.env.setting("loyalty_pace"))
	local f = loyalty.factor(lvl)
	local base = self.base[id]
	if not base then
		local dm = world:get("DamageMod", id)
		base = { damage = dm and dm.dataStorage and dm.dataStorage[3] or nil }
		self.base[id] = base
	end
	if self.env.on_spawn then self.env.on_spawn(world, id, sp, M.health_mul(beast, self.env.setting("loyalty_pace"))) end
	local dm = base.damage and world:get("DamageMod", id)
	if dm and dm.dataStorage then
		base.wrote = base.damage * role_damage * f * talents.damage(beast)
		dm.dataStorage[3] = base.wrote
	end
	self:set_speed(world, id, beast)
end

-- A beast's talents changed: when it is out, its health, damage and speed follow at once.
function P:retalent(world, beast)
	if beast.entity and self.ids[beast.entity] then self:strengthen(world, beast.entity, beast) end
end

function P:adopt(account, beast, id, world)
	beast.entity = id
	beast.out = true
	self.ids[id] = { account = account, beast = beast }
	self.aside:release(account)  -- it stays, and the beasts that stepped aside by the command come back
	if world then
		self:fresh(id)
		self.fought[id] = self.env.now(world)
		self:strengthen(world, id, beast)
	end
end

-- The entity ids of `account`'s beasts in the world (where another player's building counts).
function P:spots(account)
	local out = {}
	for _, b in ipairs(self.env.beasts():list(account)) do
		if b.out and b.entity then out[#out + 1] = b.entity end
	end
	return out
end

-- What a player hears while building ("build": their own, "near": another player's) or being in the air ("air") keeps
-- the beasts aside.
local function waits(why, what)
	if why == "air" then return "Your beasts wait while you are in the air: " .. what .. " once you land." end
	if why == "near" then return "Your beasts wait while a player builds near you: " .. what .. " a few seconds after that." end
	return "Your beasts wait while you build: " .. what .. " a few seconds after you stop building."
end

-- A beast out that fell and rests: it comes back by itself after its rest.
local function resting(beast)
	return beast.out and beast.rest > 0
end

-- beastmaster.call: returns the reply text.
function P:call(world, owner, account, beast)
	local s = self.env.setting
	if resting(beast) then
		return string.format("%s rests for %s, then comes back by itself.", beast.name, mmss(beast.rest))
	end
	if beast.out then return beast.name .. " is out already." end
	local busy, busy_why = self.aside:building(world, account, owner, self.env.now(world), self:spots(account))
	if busy then return waits(busy_why, "call again") end
	if self.env.stabled() and not beast.active then
		return beast.name .. " is in the stable: make it active at a stable " .. stable_mod.what_it_is() .. "."
	end
	if beast.rest > 0 then return string.format("%s rests for %s.", beast.name, mmss(beast.rest)) end
	local n, max = self:out_count(account), self.env.slots(world, owner) or 1
	if n >= max then
		local back
		for _, b in ipairs(self.env.beasts():list(account)) do
			if resting(b) then back = back or b.name end
		end
		return string.format("You have %s out (%s): dismiss one first.%s", plural(n, "beast"), plural(max, "active slot"),
			back and string.format(" (%s rests and comes back by itself.)", back) or "")
	end
	local now = self.env.now(world)
	local since = now - (self.last_call[account] or -math.huge)
	if since < s("call_cooldown_s") then
		return string.format("Calling again needs a moment: wait %d s.", math.ceil(s("call_cooldown_s") - since))
	end
	local mana, max_mana = emf.attribute(world, owner, "mana"), emf.attribute(world, owner, "max_mana")
	local cost = max_mana and math.ceil(max_mana * s("call_mana_pct") / 100) or 0
	if cost > 0 and (not mana or mana < cost) then return string.format("Not enough mana: calling costs %d.", cost) end
	local id, why, refused = self:spawn(world, owner, account, beast)
	if not id then
		local sp = species.by_id(beast.species)
		if refused and sp and sp.pack then return "The call failed: " .. why .. ". " .. INSTALL end
		return "The call failed: " .. why
	end
	if cost > 0 then emf.attribute_add(world, owner, "mana", -cost) end
	self.idle:stop(world, account)  -- the beasts out get up
	self.last_call[account] = now
	self.aside:release(account)  -- the beasts that stepped aside come back too
	self.back[beast] = nil
	self.env.save()
	return string.format("%s the %s comes.", beast.name, species.word(beast.species))
end

-- The reply when the command's aside ends: they come at once, or once the building next to them stops.
local function coming_back(self, world, account, owner)
	local busy, why = self.aside:building(world, account, owner, self.env.now(world), self:spots(account))
	if not busy then return "Your beasts come back." end
	if why == "air" then return "Your beasts come back once you land." end
	if why == "near" then return "Your beasts come back a few seconds after the player near you stops building." end
	return "Your beasts come back a few seconds after you stop building."
end

-- beastmaster.aside: the beasts that are out step aside (leave the world, still out) or come back. Returns the reply.
function P:toggle_aside(world, account, owner)
	if self:out_count(account) == 0 then
		self.aside:release(account)
		return "No beast of yours is out."
	end
	if self.aside:toggle(account) then
		return "Your beasts step aside. Choose Come back on the wheel, type /beastmaster aside again, or call one, to "
			.. "bring them back."
	end
	return coming_back(self, world, account, owner)
end

-- The wheel's Come back: the command's aside ends.
function P:come_back(world, account, owner)
	if not self.aside:release(account) then return "Your beasts are not aside." end
	return coming_back(self, world, account, owner)
end

-- Aside now (by the command, or while building): the HUD and the list say so.
function P:is_aside(account)
	return self.aside:is_aside(account)
end

-- Aside by the command: the wheel offers Come back.
function P:aside_by_command(account)
	return self.aside:by_command(account)
end

-- beastmaster.dismiss: the beast leaves the world and waits at home.
function P:dismiss(world, account, beast)
	if not beast.out then return beast.name .. " is not out." end
	self.idle:stop(world, account)  -- every beast of theirs gets up
	if beast.entity and ours(world, beast.entity) then emf.destroy(world, beast.entity) end
	if beast.entity then
		self.ids[beast.entity] = nil
		self.sent[beast.entity], self.recall[beast.entity], self.base[beast.entity], self.orders[beast.entity] = nil, nil, nil, nil
		self.fought[beast.entity] = nil
	end
	beast.entity = nil
	beast.out = false
	self.back[beast] = nil
	if self:out_count(account) == 0 then self.aside:release(account) end  -- none left to step aside
	self.env.save()
	return beast.name .. " goes home."
end

-- The beasts out fit the owner's active slots: once the world has had a stable only active beasts stay out, and
-- never more than the slots (the first in list order stay; a save from before the slots, or a slot setting raised).
-- While the owner's level cannot be read (slots nil) the number out is left as it is.
function P:fit_slots(world, account, owner)
	local max = self.env.slots(world, owner)
	local stabled = self.env.stabled()
	local out = 0
	for _, b in ipairs(self.env.beasts():list(account)) do
		if b.out then
			if (stabled and not b.active) or (max and out >= max) then
				self:dismiss(world, account, b)
				self.env.tell(world, owner, string.format("%s goes home: %s.", b.name,
					(stabled and not b.active) and "it is in the stable" or plural(max, "active slot")))
			else
				out = out + 1
			end
		end
	end
end

-- Release (the Beasts window): the beast leaves the collection for good; a beast that is out leaves the world as when
-- it is sent home.
function P:release(world, account, beast)
	if beast.out then self:dismiss(world, account, beast) end
	self.env.beasts():remove(account, beast)
	self.env.save()
end

-- Attack's order: `target` written as beast `id`'s pick (SelectedTargets slot 0, its root too) with its target switches
-- off, the way the threat program holds a monster on a beast; false when it has no pick to write. The game's
-- AddThreatTargetEvent only raises a player's threat (research enemy_targets_2026-11-12): beasts sent with it went only
-- once their own senses found the creature (seen: "Wolf attacks (2)", then a wait).
local function order(world, id, target)
	local a = world:get("DynamicAggro", id)
	local s = a and world:get("SelectedTargets", id)
	if not (s and s.targets) then return false end
	local t = s.targets[1]
	if t.targetEntityId ~= target then t.targetEntityId = target end
	if t.targetRootEntityId ~= target then t.targetRootEntityId = target end
	if a.allowTargetSwitches then a.allowTargetSwitches = false end
	return true
end

-- The order over: the game picks for the beast again.
function P:end_order(world, id)
	if not self.orders[id] then return end
	self.orders[id] = nil
	local a = world:position(id) and world:get("DynamicAggro", id)
	if a and not a.allowTargetSwitches then a.allowTargetSwitches = true end
end

-- beastmaster.attack: every beast of `account` that is out goes for `target` at once, for SENT_S or until it dies.
-- `hunting` (aggressive mode): a beast with an order keeps it (the player's Attack, or the foe it hunts already).
function P:attack(world, account, target, hunting)
	local n = 0
	local now = self.env.now(world)
	self.idle:stop(world, account)  -- every beast of theirs gets up
	for _, b in ipairs(self.env.beasts():list(account)) do
		if b.out and b.entity and ours(world, b.entity) and not (hunting and self.orders[b.entity]) then
			self.sent[b.entity] = now + SENT_S
			self.orders[b.entity] = target
			order(world, b.entity, target)
			n = n + 1
		end
	end
	return n
end

-- Each check: an order holds while its time runs and its target may be fought (alive, and not turned friendly: a
-- creature being tamed becomes an ally at once).
function P:keep_order(world, id, now)
	local target = self.orders[id]
	if not target then return end
	if now >= (self.sent[id] or 0) or not M.fightable(world, target) then
		self:end_order(world, id)
	else
		order(world, id, target)
	end
end

-- The nearest living hostile creature (HOSTILE) within `range` metres of `owner`, never a boss or a player; nil when
-- there is none.
local function nearest_foe(world, owner, range)
	local ox, oy, oz = world:position(owner)
	if not ox then return nil end
	local best, best_d
	for _, id in ipairs(world:entities()) do
		local f = world:get("Faction", id)
		if f and HOSTILE[f.faction] and not world:get("PlayerState", id) then
			local x, y, z = world:position(id)
			local d = x and math.sqrt((x - ox) ^ 2 + (y - oy) ^ 2 + (z - oz) ^ 2)
			if d and d <= range and (not best_d or d < best_d) and not world:get("Boss", id)
				and not world:get("BossHealthBar", id) then
				local cur = emf.health(world, id)
				if cur and cur > 0 then best, best_d = id, d end
			end
		end
	end
	return best
end

-- Aggressive beasts: every beast of `account` that is out goes at the nearest hostile creature near its owner.
function P:hunt(world, account, owner)
	local foe = nearest_foe(world, owner, self.env.setting("aggro_m"))
	if foe then self:attack(world, account, foe, true) end
end

-- Every order of `account`'s beasts ends (their mode changed: passive ones stop at once).
function P:end_orders(world, account)
	for _, b in ipairs(self.env.beasts():list(account)) do
		if b.entity and self.orders[b.entity] then
			self:end_order(world, b.entity)
			self.sent[b.entity] = nil
		end
	end
end

-- XP credit: when our beast's hit leaves a foe at or under 25 % health, its owner lands the killing blow (a real game
-- hit: kill credit, XP and loot go to the owner). Once per foe; never on players or allies.
function P:credit(world, source, target)
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

-- Every hit to or from our beast is noted (it heals HEAL_AFTER_S after the last one), may hand a foe's killing blow to
-- its owner (P:credit), and counts for its talents in fights (fight.lua). `change`: the hit's health change.
function P:on_hit(world, source, target, change)
	self.threat:on_hit(world, source, target)
	self.idle:on_hit(world, source, target)
	if not (self.ids[source] or self.ids[target]) then return end
	local now = self.env.now(world)
	if self.ids[source] then self.fought[source] = now end
	if self.ids[target] then self.fought[target] = now end
	local f = self.ids[source] and world:get("Faction", target)
	if f and HOSTILE[f.faction] and not world:get("PlayerState", target) then
		self.hits[target] = self.hits[target] or {}
		self.hits[target][source] = now
	end
	self:credit(world, source, target)
	self.fight:on_hit(world, source, target, change or 0, self.ids[source], self.ids[target], self.credited[target])
end

-- Ambush's bonus (fight.lua): a real hit from the beast on the next step; one that kills is its owner's while xp_credit
-- is on (the XP and loot, as the XP credit gives them).
function P:strike(world, e, id, target, amount)
	local cur = emf.health(world, target)
	local owner = self.env.setting("xp_credit") and cur and amount >= cur and self:owner(world, e.account)
	if owner then
		self.credited[target] = true
		emf.damage(world, owner, target, amount)
	else
		emf.damage(world, id, target, amount)
	end
end

-- A loyalty point for the beast of entity `e`: a new loyalty is told to its owner and makes it stronger at once.
function P:gain(world, e)
	local b = e.beast
	local pace = self.env.setting("loyalty_pace")
	local before = loyalty.level(b.points, pace)
	b.points = (b.points or 0) + 1
	local after = loyalty.level(b.points, pace)
	self.env.save()
	if after > before then
		local owner = self:owner(world, e.account)
		if owner then
			self.env.tell(world, owner, string.format("%s reached loyalty %d: a talent point to spend at a stable.", b.name, after))
		end
		if b.entity then self:strengthen(world, b.entity, b) end
	end
end

function P:on_died(id, world)
	local hitters = self.hits[id]
	self.hits[id] = nil
	if hitters and world then
		local now = self.env.now(world)
		for entity, at in pairs(hitters) do
			local e = self.ids[entity]
			if e and now - at <= KILL_HIT_S then self:gain(world, e) end
		end
	end
	self.credited[id] = nil
	self.threat:forget(id)
	self.idle:forget(id)
	self.sent[id], self.recall[id], self.base[id], self.fought[id], self.orders[id] = nil, nil, nil, nil, nil
	local e = self.ids[id]
	if not e then return end
	self.ids[id] = nil
	e.beast.entity = nil
	-- with back_after_rest it stays out (and holds its slot) without a body, and comes back after its rest
	e.beast.out = self.env.setting("back_after_rest") == true
	e.beast.rest = self.env.setting("rest_s")
	self.back[e.beast] = e.beast.out or nil
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
	-- beyond chase_m it drops its chase and runs back: it fights nothing for RECALL_S and guards its owner again
	if d > s("chase_m") and d <= s("leash_m") and now >= (self.recall[b.entity] or 0) then
		self.recall[b.entity] = now + RECALL_S
		self:end_order(world, b.entity)  -- an Attack order ends there too
		calm(world, b.entity)
		emf.emit("server", "StartGuardingEvent", { guardingEntityId = b.entity, guardedEntityId = owner })
	end
	if d <= s("leash_m") then
		self.far[b.entity] = nil
		return
	end
	self.far[b.entity] = self.far[b.entity] or now
	if d <= s("leash_far_m") and now - self.far[b.entity] < 3 then return end
	if now - (self.moved[b.entity] or -math.huge) < 3 then return end  -- a teleport takes a moment: no repeats
	self.moved[b.entity] = now
	self.far[b.entity] = nil
	if emf.teleport(world, b.entity, ox + 2, oy + 0.5, oz, { ground = true }) then
		-- it drops its chase and guards its owner again (seen: a wolf brought back from a far hare chase stood
		-- still); the recall window keeps it calm while it lands
		self.recall[b.entity] = now + RECALL_S
		self:end_order(world, b.entity)
		calm(world, b.entity)
		emf.emit("server", "StartGuardingEvent", { guardingEntityId = b.entity, guardedEntityId = owner })
	else
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
	local any_rest = false
	local comes_back = self.env.setting("back_after_rest") == true
	for _, account in ipairs(beasts:accounts()) do
		for _, b in ipairs(beasts:list(account)) do
			if b.rest > 0 then
				if b.out and not b.entity and not comes_back then  -- the setting went off: it stays home
					b.out = false
					self.back[b] = nil
					self.env.save()
				elseif b.out and not b.entity then
					self.back[b] = true  -- it comes back after its rest (also a save's beast after a reload)
				end
				b.rest = math.max(0, b.rest - dt)
				any_rest = true
				if b.rest == 0 then self.env.save() end
			end
		end
	end
	if any_rest and now >= self.next_save then
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
	-- every step: passive beasts (unless Attack sent them) and beasts coming back from a chase fight nothing
	for id, e in pairs(self.ids) do
		local passive = self.env.mode and self.env.mode(e.account) == "passive" and now >= (self.sent[id] or 0)
		if passive or now < (self.recall[id] or 0) then calm(world, id) end
		if self.speeds[id] then self:set_speed(world, id, e.beast) end
	end
	-- ids that left (sent home, fallen, aside, not ours): the game's speeds and minimum health back
	for id in pairs(self.speeds) do
		if not self.ids[id] then self:give_back_speed(world, id) end
	end
	self.fight:step(world, self.ids)
	self.threat:step(world)
	self.idle:step(world, self.present or {})
	if self.aside:watch(world, now) then self.next_check = now end  -- someone started building: step aside now
	if now < self.next_check then return end
	self.next_check = now + 1
	-- hits on foes that did not die within KILL_HIT_S count for nothing any more
	for target, hitters in pairs(self.hits) do
		for entity, at in pairs(hitters) do
			if now - at > KILL_HIT_S then hitters[entity] = nil end
		end
		if next(hitters) == nil then self.hits[target] = nil end
	end
	local present = players(world)
	self.present = present
	self.threat:scan(world, present)
	self.aside:set_players(present)
	for _, e in ipairs(self.fallen) do
		local owner = present[e.account]
		if owner then
			local back = self.back[e.beast]
			self.env.tell(world, owner, string.format(back and "%s fell; it comes back in %s." or "%s fell; it rests for %s.",
				e.beast.name, mmss(e.beast.rest)))
		end
	end
	self.fallen = {}
	for _, account in ipairs(beasts:accounts()) do
		local owner = present[account]
		if owner then self:fit_slots(world, account, owner) end
		if self:out_count(account) == 0 then self.aside:release(account) end  -- none left to step aside
		local away, why = false, nil
		if owner then away, why = self.aside:check(world, account, owner, now, self:spots(account)) end
		local stepped = false
		for _, b in ipairs(beasts:list(account)) do
			local cur = b.out and b.entity and world:position(b.entity) and emf.health(world, b.entity)
			local f = cur and cur <= 0 and self.ids[b.entity] and world:get("Faction", b.entity)
			local fell = f and f.faction == PLAYER_ALLY
			if fell then
				-- dead, the game's EntityDiedEvent not here yet (it comes a step or two after; seen live): it fell. Its
				-- owner hears it at the next check, before it can come back
				self:on_died(b.entity, world)
			elseif b.out and b.entity then
				if not ours(world, b.entity) then
					-- gone, or the id names another entity now: never touched again. One that left the world on another mod's
					-- move (it is not leashed then: a far move unloads it) stays out and comes back beside its owner
					local lost = self.walked[b.entity] and not world:position(b.entity)
					self.walked[b.entity] = nil
					self.ids[b.entity] = nil
					b.entity = nil
					if not lost then
						b.out = false
						self.env.save()
					end
				elseif not owner or away then
					emf.destroy(world, b.entity)  -- the owner left, or it steps aside: back when that ends (still out)
					self.ids[b.entity] = nil
					b.entity = nil
					stepped = stepped or away
				else
					local ai = world:get("DynamicEnemy01BehaviorState", b.entity)
					local guard = ai and ai.guardEntityId or 0
					-- another mod walks it somewhere (EMF move orders, Lua 1.33): it guards the move's waypoint meanwhile; it is
					-- not taken for another creature, not leashed and not sent back (EMF gives it back to its owner after)
					local moving = emf.enemy_moving ~= nil and emf.enemy_moving(world, b.entity) ~= nil
					if moving then guard = owner end
					self.walked[b.entity] = moving or nil
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
						if not moving then self:leash(world, owner, b, now) end
						if b.entity then self:keep_order(world, b.entity, now) end
						-- its level follows its owner's (a level up while it is out); the leash may have let it go
						local lvl, mine = b.entity and world:get("Level", b.entity), level_of(world, owner)
						if lvl and mine and lvl.dataStorage[1] ~= mine then
							lvl.dataStorage[1] = mine
							local scaling = world:get("DynamicEnemyScaling", b.entity)
							if scaling then scaling.nextUpdateTime = 0 end
						end
						if b.entity then
							self:keep_damage(world, b.entity, b)
							-- between fights it heals
							local cur, max = emf.health(world, b.entity)
							if cur and max and cur > 0 and cur < max and now - (self.fought[b.entity] or now) >= HEAL_AFTER_S then
								emf.attribute_add(world, b.entity, "health", math.min(math.ceil(max * HEAL_SHARE), max - cur))
							end
						end
						if guard ~= owner then
							emf.emit("server", "StartGuardingEvent", { guardingEntityId = b.entity, guardedEntityId = owner })
						end
					end
				end
			elseif b.out and owner and b.rest == 0 and not away then
				-- after a reload, its owner came back, it stepped aside, or it rested after a fall
				local id, why, refused = self:spawn(world, owner, account, b)
				local sp = species.by_id(b.species)
				if id then
					self.refused[b] = nil
					if self.back[b] then
						self.back[b] = nil
						self.env.tell(world, owner, b.name .. " is back.")
					end
				elseif not sp or (sp.pack and refused) then
					-- refused REFUSALS checks in a row: this game has no such beast (its pack is not installed), so it
					-- waits at home instead of being tried every second; an unknown species at once
					self.refused[b] = (self.refused[b] or 0) + 1
					if not sp or self.refused[b] >= REFUSALS then
						self.refused[b] = nil
						b.out = false
						self.back[b] = nil
						self.env.save()
						self.env.tell(world, owner, string.format("%s stays home: %s.", b.name, why))
					end
				end
			end
		end
		if owner and not away and self.env.mode and self.env.mode(account) == "aggressive" and #self:spots(account) > 0 then
			self:hunt(world, account, owner)
		end
		if stepped and why == "build" and self.aside:first(account, why) then
			self.env.tell(world, owner, "Your beasts step aside while you build (the game most likely blocks building "
				.. "next to them). They come back a few seconds after you stop building.")
		elseif stepped and why == "near" and self.aside:first(account, why) then
			self.env.tell(world, owner, "Your beasts step aside while a player builds near them (the game most likely "
				.. "blocks building next to them). They come back a few seconds after that.")
		elseif stepped and why == "air" and self.aside:first(account, why) then
			self.env.tell(world, owner, "Your beasts step aside while you are in the air (on the glider, or a long fall): "
				.. "they cannot follow you there. They come back beside you once you land.")
		end
	end
end

return M
