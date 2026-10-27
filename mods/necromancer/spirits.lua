-- Raging spirits: flaming skulls cast in bursts. Each cast costs mana and raises spirits_per_cast spirits beside the
-- caster, sent at the target (or guarding the caster when there is none). They last spirit_life_s and are removed then,
-- or when they fall (their bodies drop no loot). At most spirit_cap are about per caster: the oldest go first. Spirits
-- are never saved.
-- M.new(env) where env has: setting(key), now(world), template (the body's GUID).
local M = {}
local S = {}
S.__index = S

local PLAYER_ALLY = 11

function M.new(env)
	local s = setmetatable({ env = env }, S)
	s:reset()
	return s
end

-- A new world (or a reload): spirits from before are gone with it.
function S:reset()
	self.live = {}  -- {id, account, ends} in casting order
end

function S:count(account)
	local n = 0
	for _, x in ipairs(self.live) do
		if x.account == account then n = n + 1 end
	end
	return n
end

-- Removes a spirit, alive or a body (no loot), but only while the id is still a spirit of ours: a spirit can leave the
-- world without a death (streamed out), and the game may give its id to another entity before its time is up.
local function remove(world, x)
	if not world:position(x.id) or world:get("PlayerState", x.id) then return end
	local f = world:get("Faction", x.id)
	if f and f.faction == PLAYER_ALLY then emf.destroy(world, x.id) end
end

-- necromancer.spirits: returns the reply text. `target` is the creature to rush, or nil (they guard the caster).
function S:cast(world, owner, account, target)
	local s = self.env.setting
	local mana, max_mana = emf.attribute(world, owner, "mana"), emf.attribute(world, owner, "max_mana")
	local cost = max_mana and math.ceil(max_mana * s("spirit_mana_pct") / 100) or 0
	if cost > 0 and (not mana or mana < cost) then return string.format("Not enough mana: raging spirits cost %d.", cost) end
	local x, y, z = world:position(owner)
	if not x then return "Your position is not known." end
	local now = self.env.now(world)
	local made = 0
	for i = 1, s("spirits_per_cast") do
		local a = i * 2.4
		local id = emf.spawn(world, self.env.template, x + 1.5 * math.cos(a), y + 1.5, z + 1.5 * math.sin(a))
		if id and id ~= 0 then
			local f = world:get("Faction", id)
			if f then f.faction = PLAYER_ALLY end
			local lvl, mine = world:get("Level", id), world:get("Level", owner)
			if lvl and mine then lvl.dataStorage[1] = mine.dataStorage[1] end
			local scaling = world:get("DynamicEnemyScaling", id)
			if scaling then scaling.nextUpdateTime = 0 end
			if target then
				emf.emit("server", "AddThreatTargetEvent", { attackerId = id, targetId = target, threatValue = 1000, threatPoolId = 0 })
			else
				emf.emit("server", "StartGuardingEvent", { guardingEntityId = id, guardedEntityId = owner })
			end
			self.live[#self.live + 1] = { id = id, account = account, ends = now + s("spirit_life_s") }
			made = made + 1
		end
	end
	if made == 0 then return "The spirits did not come (see the log)." end
	if cost > 0 then emf.attribute_add(world, owner, "mana", -cost) end
	-- the cap: the caster's oldest spirits go first
	local over = self:count(account) - s("spirit_cap")
	local i = 1
	while over > 0 and i <= #self.live do
		if self.live[i].account == account then
			remove(world, self.live[i])
			table.remove(self.live, i)
			over = over - 1
		else
			i = i + 1
		end
	end
	return string.format("Raging spirits (%d)%s.", made, target and "" or ": no foe near, they guard you")
end

-- The account whose spirit `id` is, or nil.
function S:account_of(id)
	for _, x in ipairs(self.live) do
		if x.id == id then return x.account end
	end
	return nil
end

-- A spirit that fell: its body goes at the next step.
function S:on_died(id)
	for _, x in ipairs(self.live) do
		if x.id == id then x.ends = 0 end
	end
end

function S:step(world)
	local now = self.env.now(world)
	for i = #self.live, 1, -1 do
		local x = self.live[i]
		if now >= x.ends then
			remove(world, x)
			table.remove(self.live, i)
		end
	end
end

return M
