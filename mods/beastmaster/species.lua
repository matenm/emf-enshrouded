-- The beasts a Beastmaster can tame and call.
--   template:   the game template a call spawns (it runs the enemy AI, which follows StartGuardingEvent)
--   graph:      the animation graph a wild creature of that kind runs (its template data on the server world:
--               world:template("AnimationGraphResourceComponent2", id).animationGraph; render models are client-side)
--   also:       another graph that is this kind too (the bear pack 2's bear runs its own graph copy, the older
--               pack's ran the wolf's)
--   factions:   when several kinds share a graph: the factions that make it this kind (the wolf's graph is also the
--               scavengers' hound's)
--               a beast on a skeleton of its own (the beasts pack's snake) runs its own copy of a game graph: its
--               graph alone tells it apart
--   min_scale / max_scale: when kinds share a graph and a faction, the creature's ScaleComponent tells them apart
--               (the bear pack's bear runs the wolf's graph at 1.4x: seen in game, game wolves have no ScaleComponent;
--               the beasts pack's aurochs runs the boar's graph at 1.3x).
--               M.of takes the first that fits, in list order
--   pack:       the content pack (an EML data patch, built by EMF at the game's start) the beast comes from, named for notes; a game without that pack
--               has no such template: the call fails, a wild one never comes
--   health_mul: a wild one's health, times the game's base health (health.lua); the pack's template sets size, damage
--               and speed
--   scale:      the pack template's ScaleComponent (a called beast's loyalty size goes on top of it; none: 1)
--   role_health, role_damage: a tamed one's health and damage against a wild one of its kind at the same level (its
--               template's, without health_mul), before loyalty: a wolf hunts (x1.2, x1.15), a boar holds (x1.7, x0.9)
--   tank:       a tamed one draws the tanks' share of the monsters in a fight (threat.lua; the others the damage
--               beasts')
--   sleeps:     its animation graph has sleep states: held in the game's Sleep state while its owner stands still, it
--               lies down (idle.lua); without them it stands there
--   stays:      false: the Sleep state never holds it while its owner stands still (it only turns, and roams)
--   small:      a critter that is everywhere (the rat): taming with nothing under the crosshair takes any other kind
--               near first
--   carrier:    a pack beast (the llama): it carries a bag and never fights, has a slot of its own (one out per player)
--               and is never wild nor tamed (it has no graph to be known by)
-- Baits are settings ("bait_<id>"), read when used.
local M = {}

local WILDBEAST, SCAVENGER = 6, 4
local WOLF_GRAPH = "3180813f-a50d-47fc-be92-29a2383bd47d"
local BOAR_GRAPH = "aed0ecec-8a98-4fd3-a450-ab4c508b0ad4"
local BEAR_SCALE = 1.3     -- the pack's bear is 1.4
local AUROCHS_SCALE = 1.2  -- the pack's aurochs is 1.3
local PACK = "the beasts pack (emf_beastmaster_beasts)"

M.list = {
	{ id = "wolf", name = "Wolf", role_health = 1.2, role_damage = 1.15, template = "478381c5-585f-4264-b373-dbb1ea596eca",
		graph = WOLF_GRAPH, factions = { [WILDBEAST] = true }, max_scale = BEAR_SCALE, sleeps = true },
	{ id = "boar", name = "Boar", tank = true, role_health = 1.7, role_damage = 0.9, template = "255fd8d9-4447-445d-8ffc-4d9dc0afdd16",
		graph = BOAR_GRAPH, max_scale = AUROCHS_SCALE },
	{ id = "sabretooth", name = "Sabretooth", role_health = 1.15, role_damage = 1.25, template = "50b96064-3ffc-47a2-8c7d-1d592a3c459c",
		graph = "9c1b120e-4901-495c-9cc0-5de134fbe140" },
	{ id = "hound", name = "Hound", role_health = 1.3, role_damage = 1.1, template = "87575915-bc75-4611-ab71-0cd4e79535b1",
		graph = WOLF_GRAPH, factions = { [SCAVENGER] = true }, sleeps = true },
	-- the rats of houses and ruins (Enemy_Wildbeast_Rat_hook; its Coldheights kin runs the same graph): small, a friend
	-- more than a fighter (GitHub #47)
	{ id = "rat", name = "Rat", plural = "rats", role_health = 1.1, role_damage = 1.0, template = "927e63d6-a53e-4eda-af44-c7a229ac22a1",
		graph = "57a2f080-1f1e-4d18-8170-2d80b756fe56", factions = { [WILDBEAST] = true }, small = true },
	-- the bear pack (tools/models/packs/beastmaster_bear.json); GUIDs fixed forever. Pack 2.0: a bear on a skeleton
	-- of its own with its own graph copy; 1.0's bear ran the wolf's graph (both at 1.4x)
	{ id = "bear", name = "Bear", tank = true, scale = 1.4, role_health = 2.2, role_damage = 1.1, template = "b3a7c2e1-5f4d-4c8b-9e21-7a0d4b6e2f02",
		model = "b3a7c2e1-5f4d-4c8b-9e21-7a0d4b6e2f01", pack = "the bear pack (emf_beastmaster_bear)", health_mul = 2.5,
		plural = "bears",
		graph = "b3a7c2e1-5f4d-4c8b-9e21-7a0d4b6e2f05", also = WOLF_GRAPH,
		factions = { [WILDBEAST] = true }, min_scale = BEAR_SCALE, sleeps = true },
	-- the beasts pack (tools/models/packs/beastmaster_beasts.json); GUIDs fixed forever
	{ id = "aurochs", name = "Aurochs", tank = true, scale = 1.3, role_health = 2.0, role_damage = 1.0, template = "d5f2b8e3-7c4a-4e9f-8b21-6a3c8d4e0f02",
		model = "d5f2b8e3-7c4a-4e9f-8b21-6a3c8d4e0f01", pack = PACK, health_mul = 3,
		plural = "aurochs",
		graph = BOAR_GRAPH, factions = { [WILDBEAST] = true }, min_scale = AUROCHS_SCALE },
	{ id = "snake", name = "Snake", scale = 1.0, role_health = 1.2, role_damage = 1.15, template = "d4c87337-8c53-41df-8488-8079046e1502",
		model = "d4c87337-8c53-41df-8488-8079046e1501", pack = PACK, health_mul = 2,
		plural = "snakes",
		graph = "d4c87337-8c53-41df-8488-8079046e1505", factions = { [WILDBEAST] = true } },
	-- the beasts pack's rare snakes: a wild snake is one of them by chance (rare_of, chance: wild.lua), bigger (the
	-- pack's ScaleComponent), tougher, and with a bonus for whoever slays it (rare.lua: runes, the chance of a tier-2
	-- rune, and the chance of a gear item of that rarity at the slayer's level)
	{ id = "crimson_snake", name = "Crimson snake", scale = 1.15, role_health = 1.2, role_damage = 1.0, template = "d4c87337-8c53-41df-8488-8079046e1512",
		model = "d4c87337-8c53-41df-8488-8079046e1511", pack = PACK, health_mul = 3, plural = "crimson snakes",
		graph = "d4c87337-8c53-41df-8488-8079046e1515", factions = { [WILDBEAST] = true },
		rare_of = "snake", chance = 0.01,
		bonus = { runes = { 3, 5 }, rune2 = 0.15, gear = 0.05, gear_rarity = "epic" } },
	{ id = "golden_snake", name = "Golden king snake", scale = 1.25, role_health = 1.2, role_damage = 0.95, template = "d4c87337-8c53-41df-8488-8079046e1522",
		model = "d4c87337-8c53-41df-8488-8079046e1521", pack = PACK, health_mul = 4, plural = "golden king snakes",
		graph = "d4c87337-8c53-41df-8488-8079046e1525", factions = { [WILDBEAST] = true },
		rare_of = "snake", chance = 0.004,
		bonus = { runes = { 4, 6 }, rune2 = 0.25, gear = 0.10, gear_rarity = "epic" } },
	{ id = "albino_snake", name = "Albino snake", scale = 1.35, role_health = 1.2, role_damage = 0.9, template = "d4c87337-8c53-41df-8488-8079046e1532",
		model = "d4c87337-8c53-41df-8488-8079046e1531", pack = PACK, health_mul = 5, plural = "albino snakes",
		graph = "d4c87337-8c53-41df-8488-8079046e1535", factions = { [WILDBEAST] = true },
		rare_of = "snake", chance = 0.0025,
		bonus = { runes = { 6, 10 }, rune2 = 0.50, gear = 0.25, gear_rarity = "legendary" } },
	-- the pack beast (GitHub #48): a llama on the game's cat-deer companion (the wilds pack's wilds_packllama, with a
	-- 16-slot inventory in its template); GUIDs fixed forever
	{ id = "packllama", name = "Pack llama", plural = "pack llamas", carrier = true, template = "d9da8d97-981f-4e72-86ac-ec81ff70b6d2",
		model = "ebb344e5-9619-471d-a7dd-ee75ddc7ec65", pack = "the wilds pack (emf_beastmaster_wilds)", scale = 1.5,
		role_health = 1.0, role_damage = 1.0 },
}

-- The wilds pack's fighting animals (species_wilds.lua): beasts like the ones above, known by replace.lua's list of the
-- animals it put into the world rather than by graph and size (they run the game's graphs).
local wilds = require("species_wilds")
for _, s in ipairs(wilds.beasts) do M.list[#M.list + 1] = s end

local by_id = {}
for _, s in ipairs(M.list) do by_id[s.id] = s end

-- More beasts from species_extra.lua beside this file, when there is one: it returns a list of entries like the ones
-- above (a creature pack that cannot be shared, a modder's own creature). Each needs id, name, template and graph
-- (GUID text); bait: the item it is fed (the beasts above have settings for theirs). An entry without those, or with
-- an id that is taken, is skipped; the skips, what was added and a file that does not load are logged.
local NEEDED = { "id", "name", "template", "graph" }

local function why_not(s)
	if type(s) ~= "table" then return "not a table" end
	for _, key in ipairs(NEEDED) do
		if type(s[key]) ~= "string" or s[key] == "" then return "it has no " .. key end
	end
	if by_id[s.id:lower()] then return "the id " .. s.id .. " is taken" end
	return nil
end

local function add_extra()
	local ok, extra = pcall(require, "species_extra")
	if not ok then
		if not tostring(extra):find("module 'species_extra' not found", 1, true) then
			emf.log("species_extra.lua did not load: " .. tostring(extra))
		end
		return
	end
	if type(extra) ~= "table" then
		emf.log("species_extra.lua gave no list of beasts")
		return
	end
	local added = {}
	for i, s in ipairs(extra) do
		local why = why_not(s)
		if why then
			local named = type(s) == "table" and type(s.id) == "string" and s.id ~= "" and " (" .. s.id .. ")" or ""
			emf.log(string.format("species_extra.lua: entry %d%s skipped: %s", i, named, why))
		else
			s.id, s.graph = s.id:lower(), s.graph:lower()
			if type(s.also) == "string" then s.also = s.also:lower() end
			M.list[#M.list + 1] = s
			by_id[s.id] = s
			added[#added + 1] = s.id
		end
	end
	if #added > 0 then
		emf.log(string.format("species_extra.lua: %d beast%s added (%s)", #added, #added == 1 and "" or "s",
			table.concat(added, ", ")))
	end
end
add_extra()

-- A tamed beast's role: its health and damage against a wild one of its kind at the same level (x1 when not given).
function M.role(sp)
	return (sp and sp.role_health) or 1, (sp and sp.role_damage) or 1
end

-- Whether a beast of kind `sp` lies down in the Sleep state (its graph has sleep states).
function M.sleeps(sp)
	return sp ~= nil and sp.sleeps == true
end

-- Whether the Sleep state may hold a beast of kind `sp` still while its owner stands still (every kind unless its
-- entry says stays = false).
function M.stays(sp)
	return sp ~= nil and sp.stays ~= false
end

-- How a text names a kind (a species or its id): "albino snake", its name in lower case.
function M.word(sp)
	if type(sp) == "string" then sp = by_id[sp] or { name = sp } end
	return sp.name:lower()
end

-- "a wolf", "an aurochs", "an albino snake".
function M.a(sp)
	local w = M.word(sp)
	return (w:find("^[aeiou]") and "an " or "a ") .. w
end

-- The kind a new wild `sp` turns out to be for a roll in [0, 1): each of its rare kinds has its own share of the
-- rolls (chance; the rarest first), the rest stay `sp`.
function M.variant(sp, roll)
	local rare = {}
	for _, r in ipairs(M.list) do
		if r.rare_of == sp.id then rare[#rare + 1] = r end
	end
	table.sort(rare, function(a, b) return a.chance < b.chance end)
	local acc = 0
	for _, r in ipairs(rare) do
		acc = acc + r.chance
		if roll < acc then return r end
	end
	return sp
end

-- The kind of a creature running `graph` (GUID text) in `faction` (a number) at `scale` (its ScaleComponent, nil when
-- it has none: 1; `has_scale`: it has one), or nil. The wilds pack's beasts are never found this way (replace.lua knows
-- its own).
function M.of(graph, faction, scale, has_scale)
	if not graph then return nil end
	graph = tostring(graph):lower()
	scale = scale or 1
	-- a wilds animal (a game graph at its own size; with `has_scale`, its ScaleComponent, at 1 too: the game's wolves,
	-- sabretooths and goats have none) that replace.lua's list does not know: none of them
	if has_scale or math.abs(scale - 1) > 0.01 then
		for _, s in ipairs(wilds.beasts) do
			if s.graph == graph and math.abs(scale - s.scale) < 0.005 then return nil end
		end
	end
	for _, s in ipairs(M.list) do
		if not s.wilds and (s.graph == graph or s.also == graph) and (not s.factions or s.factions[faction])
			and (not s.min_scale or scale >= s.min_scale) and (not s.max_scale or scale < s.max_scale) then
			return s
		end
	end
	return nil
end

function M.by_id(id)
	return id and by_id[tostring(id):lower()] or nil
end

-- A pack beast's kind (species `carrier`): a slot of its own, never a fighter.
function M.is_carrier(id)
	local s = M.by_id(id)
	return s ~= nil and s.carrier == true
end

return M
