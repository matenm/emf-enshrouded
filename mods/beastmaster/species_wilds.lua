-- The wilds pack's animals (emf_beastmaster_wilds, tools/models/packs/beastmaster_wilds.json, a separate download):
-- animals made in the game's style, each fitted straight onto a game animal's skeleton, so it runs that animal's own
-- animation graph (a fox runs the wolf's). The server world has no render models for creatures, so nothing tells a fox
-- from a wolf there but Beastmaster's list of the animals it put into the world (replace.lua). GUIDs are the pack's,
-- fixed forever (tests/tools/test_species_wilds.py holds them to the pack).
--   beasts: the fighting ones, tamed and called like species.lua's own (its fields; `wilds` keeps them out of
--     species.of and of wild.lua's rare beasts; `creature` names the pack's creature). A colour version's role
--     numbers are about 10 % above its animal's; `colour_of` names that animal. The panther's id is wilds_panther:
--     "panther" in a world's data is Beastmaster 1.1's panther, which loads as a sabretooth (roster.lua).
--   kinds: the game creatures that are now and then one of ours instead (replace.lua): a wild creature on `graph` in
--     `faction`; `model` is the game's render model, read on the host's client world (it shares the server's entity
--     ids); a kind with `needs_model` is only known by it (otherwise the first kind of its graph and faction stands in:
--     a dedicated server has no client world). `animals` come at wild_replace_pct, `coats` (stronger colour
--     versions) at wild_coat_pct; `beast` names a beasts entry (none: the goat, ram, deer and elk run the wildlife AI
--     and cannot fight, so they are never tamed).
local WOLF_GRAPH = "3180813f-a50d-47fc-be92-29a2383bd47d"
local SABRE_GRAPH = "9c1b120e-4901-495c-9cc0-5de134fbe140"
local GOAT_GRAPH = "1fc6e01e-f25b-4aeb-a5a0-2d3fb2c727f5"
local WILDLIFE, WILDBEAST = 5, 6
local PACK = "the wilds pack (emf_beastmaster_wilds)"

local M = {}

M.beasts = {
	{ id = "fox", name = "Fox", plural = "foxes", role_health = 1.4, role_damage = 1.25,
		template = "2e377d42-ff1e-4e6e-b77a-1596e9404afd", graph = WOLF_GRAPH, factions = { [WILDBEAST] = true },
		scale = 0.65, creature = "wilds_fox", pack = PACK, wilds = true, bait = "Food_T1_raw_bad_meat_LeanMeat",
		sleeps = true },
	{ id = "arctic_fox", name = "Arctic fox", plural = "arctic foxes", role_health = 1.55, role_damage = 1.35,
		template = "9c54a6f2-8075-4688-8668-533b44fddd8c", graph = WOLF_GRAPH, factions = { [WILDBEAST] = true },
		scale = 0.715, creature = "wilds_fox_arctic", pack = PACK, wilds = true,
		bait = "Food_T1_raw_bad_meat_LeanMeat", sleeps = true, colour_of = "fox" },
	{ id = "silver_fox", name = "Silver fox", plural = "silver foxes", role_health = 1.55, role_damage = 1.35,
		template = "f08e4f1f-ac91-455b-b2a3-5fee230cbd39", graph = WOLF_GRAPH, factions = { [WILDBEAST] = true },
		scale = 0.715, creature = "wilds_fox_silver", pack = PACK, wilds = true,
		bait = "Food_T1_raw_bad_meat_LeanMeat", sleeps = true, colour_of = "fox" },
	{ id = "sand_fox", name = "Sand fox", plural = "sand foxes", role_health = 1.55, role_damage = 1.35,
		template = "4ecf0cd1-ffe3-4416-b95c-71f20f4ea3da", graph = WOLF_GRAPH, factions = { [WILDBEAST] = true },
		scale = 0.715, creature = "wilds_fox_sand", pack = PACK, wilds = true,
		bait = "Food_T1_raw_bad_meat_LeanMeat", sleeps = true, colour_of = "fox" },
	{ id = "white_wolf", name = "White wolf", plural = "white wolves", role_health = 1.35, role_damage = 1.25,
		template = "856d751b-f604-4237-b14d-06833ad90ea2", graph = WOLF_GRAPH, factions = { [WILDBEAST] = true },
		scale = 1.1, creature = "wilds_wolf_white", pack = PACK, wilds = true,
		bait = "Food_T1_raw_bad_meat_LeanMeat", sleeps = true, colour_of = "wolf" },
	{ id = "black_wolf", name = "Black wolf", plural = "black wolves", role_health = 1.35, role_damage = 1.25,
		template = "53ae5a6f-8cbc-4294-813c-155894104aec", graph = WOLF_GRAPH, factions = { [WILDBEAST] = true },
		scale = 1.1, creature = "wilds_wolf_black", pack = PACK, wilds = true,
		bait = "Food_T1_raw_bad_meat_LeanMeat", sleeps = true, colour_of = "wolf" },
	{ id = "red_wolf", name = "Red wolf", plural = "red wolves", role_health = 1.35, role_damage = 1.25,
		template = "54f4bc35-24fe-4a9f-8456-98e5d1f60587", graph = WOLF_GRAPH, factions = { [WILDBEAST] = true },
		scale = 1.1, creature = "wilds_wolf_red", pack = PACK, wilds = true, bait = "Food_T1_raw_bad_meat_LeanMeat",
		sleeps = true, colour_of = "wolf" },
	{ id = "brown_bear", name = "Brown bear", plural = "brown bears", tank = true, role_health = 2.0,
		role_damage = 1.1, template = "ebb758bf-64b9-4b77-ad45-931e2dcfa78c", graph = SABRE_GRAPH,
		factions = { [WILDBEAST] = true }, scale = 1.0, creature = "wilds_bear", pack = PACK, wilds = true,
		bait = "Food_T1_raw_fruit_Red_Berries" },
	{ id = "black_bear", name = "Black bear", plural = "black bears", tank = true, role_health = 2.2,
		role_damage = 1.2, template = "797eac81-cf61-413b-8ae4-81297aac0ae6", graph = SABRE_GRAPH,
		factions = { [WILDBEAST] = true }, scale = 1.1, creature = "wilds_bear_black", pack = PACK, wilds = true,
		bait = "Food_T1_raw_fruit_Red_Berries", colour_of = "brown_bear" },
	{ id = "snow_bear", name = "Snow bear", plural = "snow bears", tank = true, role_health = 2.2, role_damage = 1.2,
		template = "8027e2cc-db55-4242-9977-a0dcf5fabbb6", graph = SABRE_GRAPH, factions = { [WILDBEAST] = true },
		scale = 1.1, creature = "wilds_bear_snow", pack = PACK, wilds = true, bait = "Food_T1_raw_fruit_Red_Berries",
		colour_of = "brown_bear" },
	{ id = "cinnamon_bear", name = "Cinnamon bear", plural = "cinnamon bears", tank = true, role_health = 2.2,
		role_damage = 1.2, template = "ffa09987-f202-43c0-915c-cbcee5fe02c8", graph = SABRE_GRAPH,
		factions = { [WILDBEAST] = true }, scale = 1.1, creature = "wilds_bear_cinnamon", pack = PACK, wilds = true,
		bait = "Food_T1_raw_fruit_Red_Berries", colour_of = "brown_bear" },
	{ id = "wilds_panther", name = "Panther", plural = "panthers", role_health = 1.15, role_damage = 1.3,
		template = "e7503d65-67fc-4bc6-a61e-9c3330a96c67", graph = SABRE_GRAPH, factions = { [WILDBEAST] = true },
		scale = 0.8, creature = "wilds_panther", pack = PACK, wilds = true, bait = "Food_T2_Cat_Bait" },
	{ id = "black_panther", name = "Black panther", plural = "black panthers", role_health = 1.25,
		role_damage = 1.45, template = "96ae9a28-0684-4a01-b7fc-37be9c05622e", graph = SABRE_GRAPH,
		factions = { [WILDBEAST] = true }, scale = 0.88, creature = "wilds_panther_black", pack = PACK, wilds = true,
		bait = "Food_T2_Cat_Bait", colour_of = "wilds_panther" },
	{ id = "jaguar", name = "Jaguar", plural = "jaguars", role_health = 1.25, role_damage = 1.45,
		template = "faadafd6-add1-4fb2-963d-63918576e805", graph = SABRE_GRAPH, factions = { [WILDBEAST] = true },
		scale = 0.88, creature = "wilds_panther_golden", pack = PACK, wilds = true, bait = "Food_T2_Cat_Bait",
		colour_of = "wilds_panther" },
	{ id = "snow_leopard", name = "Snow leopard", plural = "snow leopards", role_health = 1.25, role_damage = 1.45,
		template = "a5bb5bae-429d-43f7-9e0f-4cf2a4c066a9", graph = SABRE_GRAPH, factions = { [WILDBEAST] = true },
		scale = 0.88, creature = "wilds_panther_snow", pack = PACK, wilds = true, bait = "Food_T2_Cat_Bait",
		colour_of = "wilds_panther" },
	{ id = "tiger", name = "Tiger", plural = "tigers", role_health = 1.25, role_damage = 1.45,
		template = "741654d6-1e0f-43f2-8c7f-c03c721be9fc", graph = SABRE_GRAPH, factions = { [WILDBEAST] = true },
		scale = 0.88, creature = "wilds_panther_tiger", pack = PACK, wilds = true, bait = "Food_T2_Cat_Bait",
		colour_of = "wilds_panther" },
	{ id = "lynx", name = "Lynx", plural = "lynxes", role_health = 1.0, role_damage = 1.2,
		template = "7e1aac14-7949-4c89-8463-a8a58b57e029", graph = SABRE_GRAPH, factions = { [WILDBEAST] = true },
		scale = 0.6, creature = "wilds_lynx", pack = PACK, wilds = true, bait = "Food_T2_Cat_Bait" },
	{ id = "snow_lynx", name = "Snow lynx", plural = "snow lynxes", role_health = 1.1, role_damage = 1.3,
		template = "b810787c-a8c5-4aab-8b4f-03ed113fbd66", graph = SABRE_GRAPH, factions = { [WILDBEAST] = true },
		scale = 0.66, creature = "wilds_lynx_snow", pack = PACK, wilds = true, bait = "Food_T2_Cat_Bait",
		colour_of = "lynx" },
	{ id = "dark_lynx", name = "Dark lynx", plural = "dark lynxes", role_health = 1.1, role_damage = 1.3,
		template = "112dbfc5-a3a6-4238-ad04-3bf0a4a79a56", graph = SABRE_GRAPH, factions = { [WILDBEAST] = true },
		scale = 0.66, creature = "wilds_lynx_dark", pack = PACK, wilds = true, bait = "Food_T2_Cat_Bait",
		colour_of = "lynx" },
	{ id = "red_lynx", name = "Red lynx", plural = "red lynxes", role_health = 1.1, role_damage = 1.3,
		template = "f268e168-2086-4425-aa0f-03f1145a7413", graph = SABRE_GRAPH, factions = { [WILDBEAST] = true },
		scale = 0.66, creature = "wilds_lynx_red", pack = PACK, wilds = true, bait = "Food_T2_Cat_Bait",
		colour_of = "lynx" },
}

M.kinds = {
	{ id = "wolf", graph = WOLF_GRAPH, faction = WILDBEAST, model = "91e2d4cc-01c9-4203-a92e-b08769364522",
		animals = {
			{ name = "wilds_fox", template = "2e377d42-ff1e-4e6e-b77a-1596e9404afd", scale = 0.65, beast = "fox" },
		},
		coats = {
			{ name = "wilds_fox_arctic", template = "9c54a6f2-8075-4688-8668-533b44fddd8c", scale = 0.715,
				beast = "arctic_fox" },
			{ name = "wilds_fox_silver", template = "f08e4f1f-ac91-455b-b2a3-5fee230cbd39", scale = 0.715,
				beast = "silver_fox" },
			{ name = "wilds_fox_sand", template = "4ecf0cd1-ffe3-4416-b95c-71f20f4ea3da", scale = 0.715,
				beast = "sand_fox" },
			{ name = "wilds_wolf_white", template = "856d751b-f604-4237-b14d-06833ad90ea2", scale = 1.1,
				beast = "white_wolf" },
			{ name = "wilds_wolf_black", template = "53ae5a6f-8cbc-4294-813c-155894104aec", scale = 1.1,
				beast = "black_wolf" },
			{ name = "wilds_wolf_red", template = "54f4bc35-24fe-4a9f-8456-98e5d1f60587", scale = 1.1,
				beast = "red_wolf" },
		},
	},
	{ id = "sabretooth", graph = SABRE_GRAPH, faction = WILDBEAST, model = "5a45d5a3-c91f-4a96-bc75-ec80f2b18ae8",
		animals = {
			{ name = "wilds_bear", template = "ebb758bf-64b9-4b77-ad45-931e2dcfa78c", scale = 1.0,
				beast = "brown_bear" },
			{ name = "wilds_panther", template = "e7503d65-67fc-4bc6-a61e-9c3330a96c67", scale = 0.8,
				beast = "wilds_panther" },
			{ name = "wilds_lynx", template = "7e1aac14-7949-4c89-8463-a8a58b57e029", scale = 0.6, beast = "lynx" },
		},
		coats = {
			{ name = "wilds_bear_black", template = "797eac81-cf61-413b-8ae4-81297aac0ae6", scale = 1.1,
				beast = "black_bear" },
			{ name = "wilds_bear_cinnamon", template = "ffa09987-f202-43c0-915c-cbcee5fe02c8", scale = 1.1,
				beast = "cinnamon_bear" },
			{ name = "wilds_panther_black", template = "96ae9a28-0684-4a01-b7fc-37be9c05622e", scale = 0.88,
				beast = "black_panther" },
			{ name = "wilds_panther_golden", template = "faadafd6-add1-4fb2-963d-63918576e805", scale = 0.88,
				beast = "jaguar" },
			{ name = "wilds_panther_tiger", template = "741654d6-1e0f-43f2-8c7f-c03c721be9fc", scale = 0.88,
				beast = "tiger" },
			{ name = "wilds_lynx_dark", template = "112dbfc5-a3a6-4238-ad04-3bf0a4a79a56", scale = 0.66,
				beast = "dark_lynx" },
			{ name = "wilds_lynx_red", template = "f268e168-2086-4425-aa0f-03f1145a7413", scale = 0.66,
				beast = "red_lynx" },
		},
	},
	{ id = "snow_sabretooth", graph = SABRE_GRAPH, faction = WILDBEAST, model = "3d555869-f7ef-4462-b341-06cad1b2d7e6",
		needs_model = true,
		animals = {
			{ name = "wilds_bear", template = "ebb758bf-64b9-4b77-ad45-931e2dcfa78c", scale = 1.0,
				beast = "brown_bear" },
			{ name = "wilds_lynx", template = "7e1aac14-7949-4c89-8463-a8a58b57e029", scale = 0.6, beast = "lynx" },
		},
		coats = {
			{ name = "wilds_bear_snow", template = "8027e2cc-db55-4242-9977-a0dcf5fabbb6", scale = 1.1,
				beast = "snow_bear" },
			{ name = "wilds_panther_snow", template = "a5bb5bae-429d-43f7-9e0f-4cf2a4c066a9", scale = 0.88,
				beast = "snow_leopard" },
			{ name = "wilds_lynx_snow", template = "b810787c-a8c5-4aab-8b4f-03ed113fbd66", scale = 0.66,
				beast = "snow_lynx" },
		},
	},
	{ id = "goat", graph = GOAT_GRAPH, faction = WILDLIFE, model = "64c9c247-ac76-4b72-a632-5cafda86caa2",
		animals = {
			{ name = "wilds_goat", template = "2d1e9269-b64b-44b5-94c5-7543f5be2ae6", scale = 1.0 },
			{ name = "wilds_deer", template = "50173bb8-9057-45f8-8269-d2049469f9fb", scale = 1.2 },
		},
		coats = {
			{ name = "wilds_goat_black", template = "9fd8da53-63c1-4a0a-ab8f-2c096089d876", scale = 1.1 },
			{ name = "wilds_goat_piebald", template = "918db95d-e945-40b3-8382-bfc9d3a70f78", scale = 1.1 },
			{ name = "wilds_deer_dark", template = "a5e4052a-f4f7-4872-9124-3c7e862fa925", scale = 1.32 },
			{ name = "wilds_deer_fallow", template = "4e6c70fb-52ae-4e3f-a8d3-efe6214a982a", scale = 1.32 },
		},
	},
	{ id = "mountain_goat", graph = GOAT_GRAPH, faction = WILDLIFE, model = "7bab51e4-2877-4d63-990f-ce46a2178479",
		needs_model = true,
		animals = {
			{ name = "wilds_ram", template = "c13b1c2f-ba75-47b0-b0ba-bf92ffc759b7", scale = 1.0 },
			{ name = "wilds_elk", template = "a04ae5ce-7ce9-4daa-b128-d96da02411c6", scale = 1.5 },
		},
		coats = {
			{ name = "wilds_goat_chamois", template = "621380f8-caab-4234-be8b-3f64ac8180a1", scale = 1.1 },
			{ name = "wilds_ram_black", template = "0605d235-d7e2-46c4-ba3f-e0e0c475b576", scale = 1.1 },
			{ name = "wilds_ram_golden", template = "ffb8bdb0-c9c1-43ac-b96b-d58125602c97", scale = 1.1 },
			{ name = "wilds_ram_mouflon", template = "83c3422f-63bb-4b37-9164-a4837f6f7241", scale = 1.1 },
			{ name = "wilds_elk_white", template = "0632d636-df6d-4f91-b1c6-9c0b1986978e", scale = 1.65 },
			{ name = "wilds_elk_dark", template = "c87dcfeb-7073-4cac-ad06-04509bb82346", scale = 1.65 },
			{ name = "wilds_elk_grey", template = "e7fcf30a-5f3d-48d0-8a33-3d3114731aae", scale = 1.65 },
			{ name = "wilds_deer_white", template = "2f439967-180c-4156-8ded-ccf31e055c00", scale = 1.32 },
		},
	},
}

return M
