-- Our animals in the wild (the wilds pack, emf_beastmaster_wilds): the game's wolves, sabretooths, goats and yaks are
-- now and then one of the pack's animals instead (wild_replace_pct of them) or a stronger colour version (wild_coat_pct,
-- stronger by wild_coat_strength in health and damage). The kinds and their pools are in species_wilds.lua.
--   When: the server world is walked every WALK_S in slices; a wild creature of a kind there that the walk had not
--   seen is rolled once, after the walk that found it (its players complete). What the first walk with a player in the
--   world finds stays the game's (it was there when the player came), and so does one within NEAR_M of a player (no
--   swap in view), within BASE_M of a flame altar, hurt, or with a ScaleComponent (the game's wolves, sabretooths and
--   goats have none; our packs' creatures and other mods' bigger ones have). At its roll the creature on the id is
--   checked again (the game gives ids out again).
--   Which kind: graph and faction on the server world; on a game someone hosts, the host's client world shares the
--   server's entity ids and has the render model, which tells the snow sabretooth and the mountain goat apart (asked
--   for ASK_S; no answer: the first kind of that graph and faction).
--   A hit: ours is spawned on its spot facing its way, then the game's taken out; ours gets its level. A colour
--   version's damage bonus goes on top of the game's DamageMod value each time the game writes it (its rescale to a
--   level), while it is wild; R:tamed takes it off (a tamed one is stronger by its role numbers). A spawn the game
--   refuses (no pack) turns the swaps off until the next world load, with one log line, and takes nothing out.
--   The animals it put into the world are listed (by id, while they exist at their template's size): taming asks
--   R:species_of first, since the server cannot tell a fox from a wolf by anything else.
-- M.new(env) where env has: setting(key), now(world), altars() (the flame altars' places, {x, z} each, from the
-- stables' search of the world), and optionally on_spawn(world, id, species, health multiplier).
local species = require("species")
local wilds = require("species_wilds")

local M = {}
local R = {}
R.__index = R

M.WALK_S = 10        -- s between the starts of two walks of the server world
M.SLICE = 500        -- entities looked at per server step during a walk
M.NEAR_M = 25        -- m: a creature this close to a player stays the game's
M.BASE_M = 40        -- m around a flame altar (horizontally) where nothing is swapped
M.ASK_S = 1          -- s the host's client world has to name a creature's model
local SIZE_EPS = 0.005

local GRAPHS = {}  -- graph -> true for the kinds' graphs
for _, k in ipairs(wilds.kinds) do GRAPHS[k.graph] = true end

function M.new(env)
	local r = setmetatable({ env = env }, R)
	r:reset()
	return r
end

-- A new world (or a reload): ids from before mean nothing now; the next walk is a first one again.
function R:reset()
	self.seen = {}      -- id -> true: a creature of a kind the walks found (rolled, or there at the first walk)
	self.ours = {}      -- id -> { entry, coat, bonus, keep, wrote }: the animals put into the world (bonus: a colour
	                    -- version's damage multiplier, kept while it is wild; wrote: the DamageMod value written last)
	self.cands = {}     -- creatures waiting for their roll: { id, graph, faction, at }
	self.asks = {}      -- id -> true: a model the host's client world is asked for
	self.models = {}    -- id -> model GUID text the client world named
	self.players = {}   -- the players the last walk found (their positions are read when a creature is rolled)
	self.walk = nil
	self.next_walk = nil
	self.first = true
	self.off = false
end

local function get(world, component, id)
	local ok, v = pcall(world.get, world, component, id)
	return ok and v or nil
end

local function template(world, component, id)
	local ok, v = pcall(world.template, world, component, id)
	return ok and v or nil
end

-- The kind entry for a creature on `graph` in `faction` whose render model is `model` (nil: not known).
local function kind_of(graph, faction, model)
	local plain
	for _, k in ipairs(wilds.kinds) do
		if k.graph == graph and k.faction == faction then
			if model and k.model == model then return k end
			if not k.needs_model and not plain then plain = k end
		end
	end
	if model then return nil end  -- a model none of the kinds has: another creature on that graph
	return plain
end

-- The yaw (degrees; 0 = +z, 90 = +x) creature `id` faces.
local function yaw_of(world, id)
	local t = get(world, "CurrentTransform", id)
	local q = t and t.transform and t.transform.orientation
	if not q then return 0 end
	local x, y, z, w = q.x, q.y, q.z, q.w
	-- (0, 0, 1) turned by the quaternion
	local dx = 2 * (x * z + w * y)
	local dz = 1 - 2 * (x * x + y * y)
	return math.deg(math.atan(dx, dz))
end

-- Whether (x, z) is within BASE_M of a flame altar of `altars` ({x, z} each).
local function in_base(altars, x, z)
	for _, a in ipairs(altars or {}) do
		if (a.x - x) ^ 2 + (a.z - z) ^ 2 <= M.BASE_M * M.BASE_M then return true end
	end
	return false
end

-- Whether (x, z) is within `m` metres of an entity of `ids`.
local function near(world, ids, x, z, m)
	for _, e in ipairs(ids) do
		local ex, _, ez = world:position(e)
		if ex and (ex - x) ^ 2 + (ez - z) ^ 2 <= m * m then return true end
	end
	return false
end

-- A wild creature (Faction `f`) of one of the kinds: its graph, or nil.
local function kind_graph(world, id, f)
	if f.faction ~= 5 and f.faction ~= 6 then return nil end
	local g = template(world, "AnimationGraphResourceComponent2", id)
	local graph = g and g.animationGraph and tostring(g.animationGraph):lower()
	if not graph or not GRAPHS[graph] then return nil end
	return graph
end

-- Whether the creature on `id` is still the animal `e` put there (the game gives ids out again).
local function still_ours(world, id, e)
	if not world:position(id) then return false end
	local sc = get(world, "ScaleComponent", id)
	return sc ~= nil and math.abs(sc.uniformScale - e.entry.scale) < SIZE_EPS
end

-- The beast species a creature it put into the world is (species.lua's entry), false for one of its animals that is
-- no beast (the goat, ram, deer, elk), nil for any other creature.
function R:species_of(world, id)
	local e = self.ours[id]
	if not e then return nil end
	if not still_ours(world, id, e) then
		self.ours[id] = nil
		return nil
	end
	return e.entry.beast and species.by_id(e.entry.beast) or false
end

-- A wilds beast of species `sp` put into the world on `id` another way (beastmaster.spawn): known like a swapped one.
function R:adopt(id, sp)
	self.ours[id] = { entry = { name = sp.creature, template = sp.template, scale = sp.scale, beast = sp.id } }
	self.seen[id] = true
end

function R:on_died(id)
	self.ours[id] = nil
	self.seen[id] = nil
end

-- A colour version's damage bonus on top of the DamageMod value the game holds now (it writes its own when it rescales
-- the creature to a level).
local function keep_damage(world, id, e)
	local dm = get(world, "DamageMod", id)
	local v = dm and dm.dataStorage and dm.dataStorage[3]
	if not v or (e.wrote and math.abs(v - e.wrote) <= 1e-4) then return end
	e.wrote = v * e.bonus
	dm.dataStorage[3] = e.wrote
end

-- The colour versions' damage, each while it is that wild animal (tamed or gone: no more).
local function keep_bonuses(self, world)
	for id, e in pairs(self.ours) do
		if e.keep then
			local f = get(world, "Faction", id)
			if still_ours(world, id, e) and f and (f.faction == 5 or f.faction == 6) then
				keep_damage(world, id, e)
			else
				e.keep = nil
			end
		end
	end
end

-- Creature `id` was tamed: its wild damage bonus comes off before Beastmaster gives it its role (main.lua calls this
-- before the pack takes it), and it is no longer one of the wild animals.
function R:tamed(world, id)
	local e = self.ours[id]
	self.ours[id] = nil
	if not e or not e.wrote then return end
	local dm = get(world, "DamageMod", id)
	local v = dm and dm.dataStorage and dm.dataStorage[3]
	if v and math.abs(v - e.wrote) <= 1e-4 then dm.dataStorage[3] = v / e.bonus end
end

-- Puts `entry` (a kinds pool entry; `coat`: a colour version) into the world in place of game creature `id`.
function R:swap(world, id, entry, coat)
	local x, y, z = world:position(id)
	if not x then return nil end
	local new = emf.spawn(world, entry.template, x, y, z, nil, { yaw = yaw_of(world, id) })
	if not new or new == 0 then
		self.off = true
		emf.log("our animals in the wild are off until the next world load: the game has no " .. entry.name ..
			" (it comes with the wilds pack, emf_beastmaster_wilds)")
		return nil
	end
	local old_level = get(world, "Level", id)
	local level = old_level and old_level.dataStorage[1]
	emf.destroy(world, id)
	self.seen[id] = true
	local lvl = get(world, "Level", new)
	if lvl and level then lvl.dataStorage[1] = level end
	local scaling = get(world, "DynamicEnemyScaling", new)
	if scaling then scaling.nextUpdateTime = 0 end  -- the game rescales health and damage to the new level
	local e = { entry = entry, coat = coat }
	self.ours[new] = e
	self.seen[new] = true
	if coat then
		local mul = (self.env.setting("wild_coat_strength") or 100) / 100
		if mul ~= 1 then
			if self.env.on_spawn then self.env.on_spawn(world, new, nil, mul) end
			e.bonus, e.keep = mul, true
			keep_damage(world, new, e)
		end
	end
	emf.log(string.format("our animals in the wild: entity %d is %s now (entity %d)", id, entry.name, new))
	return new
end

-- Rolls creature `c` once: maybe a swap.
local function decide(self, world, c)
	if self.off then return end
	-- still the creature the walk found (the game gives ids out again)
	local f = get(world, "Faction", c.id)
	if not f or f.faction ~= c.faction or self.ours[c.id] or get(world, "PlayerState", c.id)
		or get(world, "ScaleComponent", c.id) or kind_graph(world, c.id, f) ~= c.graph then
		return
	end
	if #self.players == 0 then return end
	local s = self.env.setting
	local coat_p = math.max(0, s("wild_coat_pct") or 0) / 100
	local rep_p = math.max(0, s("wild_replace_pct") or 0) / 100
	if coat_p + rep_p <= 0 then return end
	local x, _, z = world:position(c.id)
	if not x then return end
	local cur, max = emf.health(world, c.id)
	if not cur or cur <= 0 or (max and cur < max) then return end
	if near(world, self.players, x, z, M.NEAR_M) or in_base(self.env.altars(), x, z) then return end
	local k = kind_of(c.graph, c.faction, self.models[c.id])
	if not k then return end
	local roll = math.random()
	local pool, coat
	if roll < coat_p then
		pool, coat = k.coats, true
	elseif roll < coat_p + rep_p then
		pool, coat = k.animals, false
	else
		return
	end
	if #pool == 0 then return end
	self:swap(world, c.id, pool[math.random(#pool)], coat)
end

-- A slice of the walk: new creatures of a kind become candidates (or, on the first walk, stay the game's).
local function walk_slice(self, world, now)
	local w = self.walk
	local last = math.min(#w.ids, w.i + M.SLICE - 1)
	for k = w.i, last do
		local id = w.ids[k]
		w.present[id] = true
		local f = get(world, "Faction", id)
		if f then
			if get(world, "PlayerState", id) then
				w.players[#w.players + 1] = id
			elseif not self.seen[id] and not self.ours[id] then
				local graph = kind_graph(world, id, f)
				if graph then
					self.seen[id] = true
					if not w.first and not get(world, "ScaleComponent", id) then
						self.cands[#self.cands + 1] = { id = id, graph = graph, faction = f.faction, at = now }
						self.asks[id] = true
					end
				end
			end
		end
	end
	w.i = last + 1
	if w.i <= #w.ids then return end
	for id in pairs(self.seen) do
		if not w.present[id] then self.seen[id] = nil end
	end
	for id in pairs(self.ours) do
		if not w.present[id] then self.ours[id] = nil end
	end
	self.players = w.players
	if w.first and #w.players == 0 then self.first = true end  -- nobody in the world yet: the next walk marks too
	self.walk = nil
end

function R:step(world)
	local now = self.env.now(world)
	keep_bonuses(self, world)
	if self.off then
		self.cands, self.asks = {}, {}
		return
	end
	if not self.walk and (not self.next_walk or now >= self.next_walk) then
		self.next_walk = now + M.WALK_S
		self.walk = { ids = world:entities(), i = 1, present = {}, players = {}, first = self.first }
		self.first = false
	end
	if self.walk then walk_slice(self, world, now) end
	if self.walk or #self.cands == 0 then return end  -- rolled after the walk that found them: its players complete
	local keep = {}
	for _, c in ipairs(self.cands) do
		if self.models[c.id] or now >= c.at + M.ASK_S then
			self.asks[c.id] = nil
			decide(self, world, c)
			self.models[c.id] = nil
		else
			keep[#keep + 1] = c
		end
	end
	self.cands = keep
end

-- The host's client world: the render models of the creatures waiting for their roll (same entity ids).
function R:client_step(world)
	for id in pairs(self.asks) do
		local m = template(world, "ModelResource", id)
		if m and m.model then
			self.models[id] = tostring(m.model):lower()
			self.asks[id] = nil
		end
	end
end

-- For checks in game: rolls creature `id` now with roll `roll` (0..1), whatever the walk saw.
function R:roll_now(world, id, roll)
	local f = get(world, "Faction", id)
	local graph = f and not get(world, "PlayerState", id) and kind_graph(world, id, f)
	if not graph then return "That is not a wild wolf, sabretooth, goat or yak." end
	local k = kind_of(graph, f.faction, nil)
	if not k then return "No kind of ours replaces it." end
	local s = self.env.setting
	local coat = roll < (s("wild_coat_pct") or 0) / 100
	local pool = coat and k.coats or k.animals
	local new = self:swap(world, id, pool[math.random(#pool)], coat)
	return new and string.format("Entity %d is %s now (entity %d).", id, self.ours[new].entry.name, new)
		or "The spawn was refused (is the wilds pack installed?)."
end

return M
