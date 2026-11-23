-- The Llama Harness and the Camel Saddle: items players craft at the Workbench; Beastmaster's pack command takes one
-- from the bag and gives the player that pack beast (mods/beastmaster knows the items by their GUIDs). A content
-- script of the wilds pack (make_mod.py "content_scripts"), run with EMF's helper emf_content.lua
-- (docs/modding/content.md).
--
-- Each is a copy of a game crafting material (its icon, its stacking), with a name and a description of its own.
--
-- The GUIDs below are FIXED FOREVER: saves keep the items by their GUIDs and Beastmaster knows them by their GUIDs.
local content = require("emf_content")

local LEATHER     = "b798d3b9-dafd-4aff-b805-94b607678136" -- Material_T3_Leather
local RED_FABRIC  = "a8d8c269-d97b-4ff6-8de2-2433828b4abc" -- Material_T4_Fabric_Red
local PLANT_FIBRE = "ff00a9f6-fd7e-4933-9c6b-33c36223cd2b" -- Material_T1_Plant_Fibre
local ANIMAL_FUR  = "66ce7d4c-ce66-4ad4-800a-ab37f7e604a2" -- Material_T1_Animal_Fur
local STRING      = "9e9359f4-3a81-4aa7-9584-b814e406281a" -- Material_T1_String
local LIKE        = "Prop_Decoration_Comfort1_T0_Chair"     -- the Workbench, known with the first furniture

local ITEMS = {
	{
		item = "68cd9bab-6160-4fd0-b284-8b863dd147a9", name_guid = "3292b8f1-12ff-425c-b69e-12babb128222", desc_guid = "7884f438-6de2-4b35-b2d7-f029041f865a", recipe = "37871d39-a771-4f6f-9995-db22ee5ffeac",
		debug_name = "Item_Beastmaster_Llama_Harness", clone_of = LEATHER,
		name = "Llama Harness",
		description = "A harness with saddle bags for a pack llama. With one in your bag, call your pack llama from the "
			.. "Beasts window or with /beastmaster pack: it follows you and carries your things (needs the Beastmaster "
			.. "mod).",
		inputs = { { ANIMAL_FUR, 4 }, { PLANT_FIBRE, 6 }, { STRING, 2 } },
	},
	{
		item = "ef50d499-52a8-49f1-a2e5-3c1f874e8a28", name_guid = "b7240530-56dd-4aa2-94d4-48ca4a64cedd", desc_guid = "6f97b088-223d-46c9-bf9b-24d698dbd6d2", recipe = "b0585f21-b185-4829-abc7-2651648ee902",
		debug_name = "Item_Beastmaster_Camel_Saddle", clone_of = RED_FABRIC,
		name = "Camel Saddle",
		description = "A saddle with a blanket and a water skin for a pack camel. With one in your bag, call your pack "
			.. "camel from the Beasts window or with /beastmaster pack: it follows you and carries your things (needs the "
			.. "Beastmaster mod).",
		inputs = { { ANIMAL_FUR, 6 }, { PLANT_FIBRE, 6 }, { STRING, 4 } },
	},
}

local function recipe(s, like)
	return { guid = s.recipe, debug_name = "Craft_" .. s.debug_name, output = { s.item, 1 }, inputs = s.inputs, like = like }
end

-- Item `s` and its recipe at the Workbench, listed after the crude chair; a game without that recipe gets it in the
-- own crafting menu (the helper's default), so it can still be crafted.
local function add(s)
	content.add_item{
		guid = s.item, name_guid = s.name_guid, debug_name = s.debug_name, name = s.name,
		description_guid = s.desc_guid, description = s.description, clone_of = s.clone_of,
	}
	local done, why = pcall(content.add_recipe, recipe(s, LIKE))
	if not done then
		print("beastmaster harness: the Workbench recipe could not be copied (" .. tostring(why) .. "): the " .. s.name
			.. " is crafted in the own crafting menu instead")
		content.add_recipe(recipe(s, nil))
	end
	print("beastmaster harness: added " .. s.debug_name .. " (item " .. s.item .. ")")
end

-- each on its own: one that fails leaves the other added; the first error comes after the names are written
local first_err
for _, s in ipairs(ITEMS) do
	local ok, err = pcall(add, s)
	if not ok and not first_err then first_err = err end
end
content.finish()  -- the names of what was added, also when a step failed
if first_err then error(first_err, 0) end
