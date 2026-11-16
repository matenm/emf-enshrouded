-- Undead minions of several kinds (skeleton warriors; a lich from the undead pack): each kind is raised for mana up to
-- its own cap. They stay until they fall, follow and guard their owner, come back beside them when they stray, leave
-- the world while their owner is away or they step aside (aside.lua: while a player builds next to them, while their
-- owner is in the air, or after necromancer.aside) and come back after; after a load the saved counts come back (no
-- mana).
-- Bodies are removed while dead (no loot from our own minions). Their hits hand the killing blow (XP, loot) to the
-- owner, never on bosses or the player side.
-- M.new(env) where env has: setting(key), now(world), tell(world, player, text), save(), kinds: an ordered list of
--   { id = "lich", name = "lich", title = "Lich", plural = "Liches", template = GUID, cap = setting key,
--     mana = setting key, health_mul = 1.5 (optional), pack = "the ... pack" (optional: a creature from a content pack,
--     which a game without that pack does not have) }. The first kind is the default (raise without a kind).
local health_mod = require("health")
local aside_mod = require("aside")

local M = {}
local P = {}
P.__index = P

local PLAYER_ALLY = 11
local PLAYER_SIDE = { [7] = true, [8] = true, [PLAYER_ALLY] = true }  -- Player, AutomatedPlayer, PlayerAlly
local STRAY_CHECKS = 3  -- guarding someone else this many checks (seconds) in a row: the id names another entity now
local REFUSALS = 3      -- a pack creature's spawn refused this many checks in a row: its pack is not in this game

function M.new(env)
	local p = setmetatable({ env = env, kinds = env.kinds, by_id = {}, counts = {}, health = health_mod.new(),
		aside = aside_mod.new({ setting = env.setting }) }, P)
	for _, k in ipairs(env.kinds) do p.by_id[k.id] = k end
	p:reset()
	return p
end

-- A new world (or a reload): entity ids and clocks from before mean nothing now. The counts come from the world's data.
function P:reset()
	self.ids = {}         -- entity id -> account
	self.kind_of = {}     -- entity id -> kind id
	self.out = {}         -- account -> the entity ids of its minions in the world
	self.last_raise = {}  -- account -> time of the last raise
	self.bodies = {}      -- fallen minions' bodies to remove: id -> time, or true (not timed yet)
	self.fallen = {}      -- {account, kind} of minions that fell since the last check (to tell their owner)
	self.missing = {}     -- "account kind" -> true: its creature is not in this game (told once per load, not retried)
	self.refused = {}     -- "account kind" -> checks in a row the game refused its spawn
	self.far, self.moved, self.credited, self.strays = {}, {}, {}, {}
	self.next_check = 0
	self.health:reset()
	self.aside:reset()
end

-- A kind by its id or its name (singular or plural, any case); the first kind for none given; nil for an unknown word.
function P:kind(word)
	if not word then return self.kinds[1] end
	word = tostring(word):lower()
	for _, k in ipairs(self.kinds) do
		if word == k.id or word == k.name:lower() or word == k.plural:lower() then return k end
	end
	return nil
end

function P:count(account, kind)
	local c = self.counts[account]
	return c and c[kind or self.kinds[1].id] or 0
end

local function set_count(self, account, kind, n)
	self.counts[account] = self.counts[account] or {}
	self.counts[account][kind] = math.max(0, n)
end

-- "Skeletons 2/3, Liches 0/1": every kind (only those the player has, with `only_had`).
function P:summary(account, only_had)
	local parts = {}
	for _, k in ipairs(self.kinds) do
		local n = self:count(account, k.id)
		if n > 0 or not only_had then
			parts[#parts + 1] = string.format("%s %d/%d", k.plural, n, self.env.setting(k.cap))
		end
	end
	return table.concat(parts, ", ")
end

-- Save format, per player with minions: "s <account> <count>" for the first kind (the skeleton warriors, as v1 saved
-- them), "k <account> <kind> <count>" for each other kind. Kinds this version does not know are kept as they were.
function P:serialize()
	local accounts = {}
	for a, c in pairs(self.counts) do
		for _, n in pairs(c) do
			if n > 0 then
				accounts[#accounts + 1] = a
				break
			end
		end
	end
	table.sort(accounts)
	local first = self.kinds[1].id
	local lines = {}
	for _, a in ipairs(accounts) do
		local c = self.counts[a]
		if (c[first] or 0) > 0 then lines[#lines + 1] = string.format("s %s %d", a, c[first]) end
		local others = {}
		for kind, n in pairs(c) do
			if kind ~= first and n > 0 then others[#others + 1] = kind end
		end
		table.sort(others)
		for _, kind in ipairs(others) do lines[#lines + 1] = string.format("k %s %s %d", a, kind, c[kind]) end
	end
	return table.concat(lines, "\n")
end

function P:parse(text)
	self.counts = {}
	local first = self.kinds[1].id
	for line in tostring(text or ""):gmatch("[^\r\n]+") do
		local a, n = line:match("^s (%S+) (%d+)$")
		if a then
			set_count(self, a, first, tonumber(n))
		else
			local b, kind, m = line:match("^k (%S+) (%S+) (%d+)$")
			if b and kind ~= first then set_count(self, b, kind, tonumber(m)) end
		end
	end
end

-- Still ours: there, alive, and an ally.
local function ours(world, id)
	if not id or not world:position(id) then return false end
	local cur = emf.health(world, id)
	if not cur or cur <= 0 then return false end
	local f = world:get("Faction", id)
	return f ~= nil and f.faction == PLAYER_ALLY
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

function P:owner(world, account)
	return players(world)[account]
end

local function forget(self, account, id)
	self.ids[id], self.kind_of[id] = nil, nil
	self.far[id], self.moved[id], self.strays[id] = nil, nil, nil
	local list = self.out[account] or {}
	for i = #list, 1, -1 do
		if list[i] == id then table.remove(list, i) end
	end
end

-- Why a pack creature cannot rise: the game has no such creature (its pack is not installed, or its content was not
-- built at the game's start).
local function no_pack(kind)
	return string.format("%s come with the EMF creature packs", kind.plural:lower())
end
local INSTALL = "Install them with EMF.exe (Install a downloaded file) and restart the game; EMF.exe's EML content " ..
	"card says whether they were built."

-- The number of `account`'s minions of `kind` in the world.
local function out_of_kind(self, account, kind)
	local n = 0
	for _, id in ipairs(self.out[account] or {}) do
		if self.kind_of[id] == kind then n = n + 1 end
	end
	return n
end

-- Puts a minion of `kind` into the world beside its owner, as their ally at their level. Returns the id, or nil, why
-- and whether the game refused the spawn (rather than the owner having no position yet).
function P:spawn(world, owner, account, kind)
	local x, y, z = world:position(owner)
	if not x then return nil, "your position is not known" end
	self.out[account] = self.out[account] or {}
	local a = #self.out[account] * 2.1  -- side by side
	local id = emf.spawn(world, kind.template, x + 2 * math.cos(a), y + 0.5, z + 2 * math.sin(a))
	if not id or id == 0 then
		if kind.pack then return nil, no_pack(kind), true end
		return nil, "the game refused it (see the log)", true
	end
	local f = world:get("Faction", id)
	if f then f.faction = PLAYER_ALLY end
	local lvl, mine = world:get("Level", id), world:get("Level", owner)
	if lvl and mine then lvl.dataStorage[1] = mine.dataStorage[1] end
	local scaling = world:get("DynamicEnemyScaling", id)
	if scaling then scaling.nextUpdateTime = 0 end  -- the game rescales health and damage to the new level
	emf.emit("server", "StartGuardingEvent", { guardingEntityId = id, guardedEntityId = owner })
	self.ids[id], self.kind_of[id] = account, kind.id
	table.insert(self.out[account], id)
	self.health:watch(world, id, kind)
	return id
end

-- All of `account`'s undead, of every kind.
function P:total(account)
	local n = 0
	for _, c in pairs(self.counts[account] or {}) do n = n + c end
	return n
end

-- A player builds next to them (their owner, or another player near), or their owner is in the air: the undead wait
-- aside. Returns true and why ("build", "near" or "air").
function P:building(world, account, owner)
	return self.aside:building(world, account, owner, self.env.now(world), self.out[account] or {})
end

-- What a player hears while building ("build": their own, "near": another player's) or being in the air ("air") keeps
-- the undead aside.
function P:waits(why, what)
	if why == "air" then return "Your undead wait while you are in the air: " .. what .. " once you land." end
	if why == "near" then return "Your undead wait while a player builds near you: " .. what .. " a few seconds after that." end
	return "Your undead wait while you build: " .. what .. " a few seconds after you stop building."
end

-- The reply when the command's aside ends: they come at once, or once the building next to them stops.
function P:coming_back(world, account, owner)
	local busy, why = self:building(world, account, owner)
	if not busy then return string.format("Your undead come back (%d).", self:total(account)) end
	if why == "air" then return "Your undead come back once you land." end
	if why == "near" then return "Your undead come back a few seconds after the player near you stops building." end
	return "Your undead come back a few seconds after you stop building."
end

-- necromancer.aside: the undead step aside (leave the world, still counted) or come back. Returns the reply.
function P:toggle_aside(world, account, owner)
	if self:total(account) == 0 then
		self.aside:release(account)
		return "You have no undead."
	end
	if self.aside:toggle(account) then
		return "Your undead step aside. Choose Come back on the wheel, Gather, or type /necromancer aside again, to "
			.. "bring them back."
	end
	return self:coming_back(world, account, owner)
end

-- necromancer.raise [kind]: returns the reply text.
function P:raise(world, owner, account, word)
	local s = self.env.setting
	local kind = self:kind(word)
	if not kind then
		local names = {}
		for _, k in ipairs(self.kinds) do names[#names + 1] = k.id end
		return "Raise what? " .. table.concat(names, " | ")
	end
	local busy, why = self:building(world, account, owner)
	if busy then return self:waits(why, "raise again") end
	local n, cap = self:count(account, kind.id), s(kind.cap)
	if cap <= 0 then return string.format("%s are switched off in the settings.", kind.plural) end
	if n >= cap then
		local what = n == 1 and kind.name or kind.plural:lower()
		return string.format("You have %d %s (at most %d): one must fall first.", n, what, cap)
	end
	local now = self.env.now(world)
	local since = now - (self.last_raise[account] or -math.huge)
	if since < s("raise_cooldown_s") then
		return string.format("Raising again needs a moment: wait %d s.", math.ceil(s("raise_cooldown_s") - since))
	end
	local mana, max_mana = emf.attribute(world, owner, "mana"), emf.attribute(world, owner, "max_mana")
	local cost = max_mana and math.ceil(max_mana * s(kind.mana) / 100) or 0
	if cost > 0 and (not mana or mana < cost) then
		return string.format("Not enough mana: raising a %s costs %d.", kind.name, cost)
	end
	local id, why, refused = self:spawn(world, owner, account, kind)
	if not id then
		if refused and kind.pack then return "The raise failed: " .. why .. ". " .. INSTALL end
		return "The raise failed: " .. why .. "."
	end
	if cost > 0 then emf.attribute_add(world, owner, "mana", -cost) end
	self.missing[account .. " " .. kind.id], self.refused[account .. " " .. kind.id] = nil, nil
	set_count(self, account, kind.id, n + 1)
	self.last_raise[account] = now
	self.aside:release(account)  -- the undead that stepped aside come back too
	self.env.save()
	return string.format("A %s rises (%d/%d).", kind.name, n + 1, cap)
end

-- necromancer.dismiss: every minion of the account returns to the ground.
function P:dismiss_all(world, account)
	local n = 0
	for _, id in ipairs(self.out[account] or {}) do
		if ours(world, id) then emf.destroy(world, id) end
		self.ids[id], self.kind_of[id] = nil, nil
		n = n + 1
	end
	self.out[account] = {}
	local counted = 0
	for _, c in pairs(self.counts[account] or {}) do counted = counted + c end
	n = math.max(n, counted)
	self.counts[account] = nil
	self.aside:release(account)  -- none left to step aside
	self.env.save()
	if n == 0 then return "You have no undead." end
	return string.format("%d undead return%s to the ground.", n, n == 1 and "s" or "")
end

-- necromancer.dismiss <kind>: the newest minion of `kind` returns to the ground (one counted but not in the world, after
-- a load or while it is away, is taken off the count).
function P:dismiss_one(world, account, kind)
	local n = self:count(account, kind.id)
	if n == 0 then return string.format("You have no %s.", kind.plural:lower()) end
	local list = self.out[account] or {}
	for i = #list, 1, -1 do
		local id = list[i]
		if self.kind_of[id] == kind.id then
			if ours(world, id) then emf.destroy(world, id) end
			forget(self, account, id)
			break
		end
	end
	set_count(self, account, kind.id, n - 1)
	if self:total(account) == 0 then self.aside:release(account) end  -- none left to step aside
	self.env.save()
	return string.format("A %s returns to the ground (%d/%d).", kind.name, n - 1, self.env.setting(kind.cap))
end

-- necromancer.gather: every minion guards its owner again and comes beside them. Returns the number.
function P:gather(world, owner, account)
	local x, y, z = world:position(owner)
	local n = 0
	for _, id in ipairs(self.out[account] or {}) do
		if ours(world, id) then
			emf.emit("server", "StartGuardingEvent", { guardingEntityId = id, guardedEntityId = owner })
			if x and emf.teleport then emf.teleport(world, id, x + 2, y + 0.5, z, { ground = true }) end
			n = n + 1
		end
	end
	return n
end

-- necromancer.attack: every minion of `account` attacks `target`. Returns the number.
function P:attack(world, account, target)
	local n = 0
	for _, id in ipairs(self.out[account] or {}) do
		if ours(world, id) then
			emf.emit("server", "AddThreatTargetEvent", { attackerId = id, targetId = target, threatValue = 1000, threatPoolId = 0 })
			n = n + 1
		end
	end
	return n
end

-- The killing blow for the owner when our minion's hit leaves a foe at or under 25 %; once per foe.
function P:on_hit(world, source, target)
	local account = self.ids[source]
	if account then self:credit(world, account, target) end
end

-- The killing blow for `account`'s player (XP, loot), when a minion of theirs (a skeleton, a spirit) left `target` at or
-- under 25 %: once per foe, never a player, the player side or a boss.
function P:credit(world, account, target)
	if not self.env.setting("xp_credit") or self.credited[target] then return end
	if world:get("PlayerState", target) then return end
	if world:get("Boss", target) or world:get("BossHealthBar", target) then return end
	local f = world:get("Faction", target)
	if not f or PLAYER_SIDE[f.faction] then return end
	local cur, max = emf.health(world, target)
	if not cur or not max or max <= 0 or cur <= 0 or cur * 100 > max * 25 then return end
	local owner = self:owner(world, account)
	if not owner then return end
	self.credited[target] = true
	emf.damage(world, owner, target, cur)
end

-- One of `account`'s minions of `kind` is gone for good.
local function lost(self, account, kind)
	set_count(self, account, kind, self:count(account, kind) - 1)
	self.env.save()
end

function P:on_died(id)
	self.credited[id] = nil
	local account, kind = self.ids[id], self.kind_of[id]
	if not account then return end
	forget(self, account, id)
	lost(self, account, kind)
	self.bodies[id] = true
	self.fallen[#self.fallen + 1] = { account = account, kind = kind }
end

-- Brings a minion that strayed back beside its owner: beyond leash_m for 3 s, or beyond leash_far_m at once.
function P:leash(world, owner, account, id, now)
	local ox, oy, oz = world:position(owner)
	local bx, by, bz = world:position(id)
	if not ox or not bx then return end
	local d = math.sqrt((ox - bx) ^ 2 + (oy - by) ^ 2 + (oz - bz) ^ 2)
	local s = self.env.setting
	if d <= s("leash_m") then
		self.far[id] = nil
		return
	end
	self.far[id] = self.far[id] or now
	if d <= s("leash_far_m") and now - self.far[id] < 3 then return end
	if now - (self.moved[id] or -math.huge) < 3 then return end  -- a teleport takes a moment: no repeats
	self.moved[id] = now
	self.far[id] = nil
	if not emf.teleport or not emf.teleport(world, id, ox + 2, oy + 0.5, oz, { ground = true }) then
		local kind = self.by_id[self.kind_of[id]]
		emf.destroy(world, id)  -- refused: a new one beside the owner instead
		forget(self, account, id)
		if kind then self:spawn(world, owner, account, kind) end
	end
end

-- After a load, or the owner came back: the saved counts, never more (no mana). A kind whose creature the game refuses
-- REFUSALS checks in a row (its pack is not installed) is told once and kept in the save, not tried again until the
-- next load; a single refusal, or no position yet just after a load, is only tried again.
function P:restore(world, owner, account)
	for _, kind in ipairs(self.kinds) do
		local key = account .. " " .. kind.id
		while not self.missing[key] and out_of_kind(self, account, kind.id) < self:count(account, kind.id) do
			local id, why, refused = self:spawn(world, owner, account, kind)
			if not id then
				if kind.pack and refused then
					self.refused[key] = (self.refused[key] or 0) + 1
					if self.refused[key] >= REFUSALS then
						self.missing[key] = true
						self.env.tell(world, owner, string.format("Your %s stays saved: %s. It comes back once they load.",
							kind.name, why))
					end
				end
				break
			end
			self.refused[key] = nil
		end
	end
end

function P:step(world)
	local now = self.env.now(world)
	self.health:step(world)
	for id, due in pairs(self.bodies) do
		if due == true then
			self.bodies[id] = now + 1
		elseif now >= due then
			self.bodies[id] = nil
			local cur = emf.health(world, id)
			if world:position(id) and (not cur or cur <= 0) then emf.destroy(world, id) end  -- only while still a body
		end
	end
	if self.aside:watch(world, now) then self.next_check = now end  -- someone started building: step aside now
	if now < self.next_check then return end
	self.next_check = now + 1
	local present = players(world)
	self.aside:set_players(present)
	for _, f in ipairs(self.fallen) do
		local owner, kind = present[f.account], self.by_id[f.kind]
		if owner and kind then
			self.env.tell(world, owner, string.format("A %s fell (%d/%d).", kind.name, self:count(f.account, f.kind),
				self.env.setting(kind.cap)))
		end
	end
	self.fallen = {}
	for account in pairs(self.counts) do
		local owner = present[account]
		local away, why = false, nil
		if self:total(account) == 0 then self.aside:release(account) end  -- none left to step aside
		if owner then away, why = self.aside:check(world, account, owner, now, self.out[account] or {}) end
		local stepped = false
		local list = self.out[account] or {}
		for i = #list, 1, -1 do
			local id = list[i]
			local kind = self.kind_of[id]
			if not ours(world, id) then
				forget(self, account, id)  -- gone, or the id names another entity now: never touched again
				lost(self, account, kind)
			elseif not owner or away then
				emf.destroy(world, id)  -- the owner left, or it steps aside: back when that ends (still counted)
				forget(self, account, id)
				stepped = stepped or away
			else
				local ai = world:get("DynamicEnemy01BehaviorState", id)
				local guard = ai and ai.guardEntityId or 0
				if guard ~= 0 and guard ~= owner then
					self.strays[id] = (self.strays[id] or 0) + 1
				else
					self.strays[id] = nil
				end
				if (self.strays[id] or 0) >= STRAY_CHECKS then
					forget(self, account, id)
					lost(self, account, kind)
				else
					self:leash(world, owner, account, id, now)
					if guard ~= owner then
						emf.emit("server", "StartGuardingEvent", { guardingEntityId = id, guardedEntityId = owner })
					end
				end
			end
		end
		if owner and not away then self:restore(world, owner, account) end
		if stepped and why == "build" and self.aside:first(account, why) then
			self.env.tell(world, owner, "Your undead step aside while you build (the game most likely blocks building "
				.. "next to them). They come back a few seconds after you stop building.")
		elseif stepped and why == "near" and self.aside:first(account, why) then
			self.env.tell(world, owner, "Your undead step aside while a player builds near them (the game most likely "
				.. "blocks building next to them). They come back a few seconds after that.")
		elseif stepped and why == "air" and self.aside:first(account, why) then
			self.env.tell(world, owner, "Your undead step aside while you are in the air (on the glider, or a long fall): "
				.. "they cannot follow you there. They come back beside you once you land.")
		end
	end
end

return M
