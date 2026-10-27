-- The beasts a Beastmaster can tame and call.
--   template:   the game template a call spawns (it runs the enemy AI, which follows StartGuardingEvent)
--   graph:      the animation graph a wild creature of that kind runs (its template data on the server world:
--               world:template("AnimationGraphResourceComponent2", id).animationGraph; render models are client-side)
--   factions:   when several kinds share a graph: the factions that make it this kind (the wolf's graph is also the
--               scavengers' hound's)
--   min_scale / max_scale: when kinds share a graph and a faction, the creature's ScaleComponent tells them apart
--               (the bear pack's bear runs the wolf's graph at 1.4x: seen in game, game wolves have no ScaleComponent;
--               the beasts pack's aurochs runs the boar's graph at 1.3x, its panther the sabretooth's at 0.85x).
--               M.of takes the first that fits, in list order
--   pack:       the content pack (an EML data patch) the beast comes from, named for notes; a game without that pack
--               has no such template: the call fails, a wild one never comes
--   health_mul: times the game's base health (health.lua); the pack's template sets size, damage and speed
-- Baits are settings ("bait_<id>"), read when used.
local M = {}

local WILDBEAST, SCAVENGER = 6, 4
local WOLF_GRAPH = "3180813f-a50d-47fc-be92-29a2383bd47d"
local BOAR_GRAPH = "aed0ecec-8a98-4fd3-a450-ab4c508b0ad4"
local BEAR_SCALE = 1.3     -- the pack's bear is 1.4
local AUROCHS_SCALE = 1.2  -- the pack's aurochs is 1.3
local PANTHER_SCALE = 0.92  -- the pack's panther is 0.85
local PACK = "the beasts pack (emf_beastmaster_beasts)"

M.list = {
	{ id = "wolf", name = "Wolf", template = "478381c5-585f-4264-b373-dbb1ea596eca",
		graph = WOLF_GRAPH, factions = { [WILDBEAST] = true }, max_scale = BEAR_SCALE },
	{ id = "boar", name = "Boar", template = "255fd8d9-4447-445d-8ffc-4d9dc0afdd16",
		graph = BOAR_GRAPH, max_scale = AUROCHS_SCALE },
	-- the beasts pack's panther, before the sabretooth (the same graph: a wild one at its size is a panther)
	{ id = "panther", name = "Panther", template = "f3c9a1d7-5b2e-4f8a-9c64-2e7b1d9a5f02",
		model = "f3c9a1d7-5b2e-4f8a-9c64-2e7b1d9a5f01", pack = PACK, plural = "panthers",
		graph = "9c1b120e-4901-495c-9cc0-5de134fbe140", factions = { [WILDBEAST] = true }, max_scale = PANTHER_SCALE },
	{ id = "sabretooth", name = "Sabretooth", template = "50b96064-3ffc-47a2-8c7d-1d592a3c459c",
		graph = "9c1b120e-4901-495c-9cc0-5de134fbe140" },
	{ id = "hound", name = "Hound", template = "87575915-bc75-4611-ab71-0cd4e79535b1",
		graph = WOLF_GRAPH, factions = { [SCAVENGER] = true } },
	-- the bear pack (tools/models/packs/beastmaster_bear.json); GUIDs fixed forever
	{ id = "bear", name = "Bear", template = "b3a7c2e1-5f4d-4c8b-9e21-7a0d4b6e2f02",
		model = "b3a7c2e1-5f4d-4c8b-9e21-7a0d4b6e2f01", pack = "the bear pack (emf_beastmaster_bear)", health_mul = 2.5,
		plural = "bears",
		graph = WOLF_GRAPH, factions = { [WILDBEAST] = true }, min_scale = BEAR_SCALE },
	-- the beasts pack (tools/models/packs/beastmaster_beasts.json); GUIDs fixed forever
	{ id = "aurochs", name = "Aurochs", template = "d5f2b8e3-7c4a-4e9f-8b21-6a3c8d4e0f02",
		model = "d5f2b8e3-7c4a-4e9f-8b21-6a3c8d4e0f01", pack = PACK, health_mul = 3,
		plural = "aurochs",
		graph = BOAR_GRAPH, factions = { [WILDBEAST] = true }, min_scale = AUROCHS_SCALE },
}

local by_id = {}
for _, s in ipairs(M.list) do by_id[s.id] = s end

-- The kind of a creature running `graph` (GUID text) in `faction` (a number) at `scale` (its ScaleComponent, nil when
-- it has none: 1), or nil.
function M.of(graph, faction, scale)
	if not graph then return nil end
	graph = tostring(graph):lower()
	scale = scale or 1
	for _, s in ipairs(M.list) do
		if s.graph == graph and (not s.factions or s.factions[faction])
			and (not s.min_scale or scale >= s.min_scale) and (not s.max_scale or scale < s.max_scale) then
			return s
		end
	end
	return nil
end

function M.by_id(id)
	return id and by_id[tostring(id):lower()] or nil
end

return M
