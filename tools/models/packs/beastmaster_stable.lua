-- The Stable, the Large Stable and the Training Grounds: places for tamed beasts that players craft at the Workbench
-- and place in their base; Beastmaster opens its Beasts window with E at them (mods/beastmaster/stable.lua knows them by
-- their render models), shows the beasts a player left at home inside the Large Stable (mods/beastmaster/home.lua) and
-- the beasts in training on the Training Grounds (mods/beastmaster/grounds.lua). A content script of the beasts pack
-- (make_mod.py "content_scripts"), run with EMF's helper emf_content.lua (docs/modding/content.md); one script, so the
-- helper's finish (which reads the game's whole name table) runs once.
--
-- Each is a copy of the large animal bed's object without the parts farm animals and villagers use, nor the ones that
-- let monsters target and break it (furniture such as the crafting stations has none of them), with a prop model of
-- the pack (beastmaster_stable, beastmaster_large_stable: a stable with a thatched roof; beastmaster_training_grounds:
-- a round platform with a training dummy; make_mod "props") under its own model GUID, which Beastmaster knows it by, and boxes of its own to stand on and bump into. When its model was not made,
-- it takes a copy of the bed's record under the same GUID (the bed's look). A dedicated server has no render data: there
-- the template names the model all the same, as the game's own templates do.
--
-- The GUIDs below are FIXED FOREVER: saves keep the items by their GUIDs and placed stables by their templates' GUIDs,
-- and Beastmaster knows the models by their GUIDs.
local content = require("emf_content")

local BED_ITEM    = "a8076625-0529-415a-bf32-a9e0619e8ce5" -- Animal_Farm_Housing_Sleeping_Bed_YAK_Sanddigger
local BED_MODEL   = "7dd9139a-fba7-4e60-8b5a-fa853c250ef7" -- animalprops_farm_animal_sleep_large_01
local WOOD_LOGS   = "3f4093ae-75ed-4e5d-bd0b-784ba83b88f5" -- Material_T1_Wood_Logs
local PLANT_FIBRE = "ff00a9f6-fd7e-4933-9c6b-33c36223cd2b" -- Material_T1_Plant_Fibre
local ANIMAL_FUR  = "66ce7d4c-ce66-4ad4-800a-ab37f7e604a2" -- Material_T1_Animal_Fur
local LIKE        = "Prop_Decoration_Comfort1_T0_Chair"     -- the Workbench, known with the first furniture

local STABLES = {
	{
		item = "a84e96fa-f27a-48b8-9815-89093bd142b4", template = "96a057d5-a1d9-4fc1-b313-9b923e9302fe",
		name_guid = "3c51352b-ce1b-437b-805f-f2eaa4da7930", desc_guid = "4869485a-5566-4137-95b8-115e1ccccc89",
		recipe = "0410f9da-8a00-4b5e-a537-33b52d43ccd9", model = "1a49170a-b3d2-498d-9062-8946f6469613",
		debug_name = "Prop_Beastmaster_Stable", model_name = "beastmaster_stable", scale = 1,
		name = "Stable",
		description = "A bed for your tamed beasts. Press E at it to open the Beasts window, where you choose which "
			.. "beasts go with you (needs the Beastmaster mod).",
		inputs = { { WOOD_LOGS, 6 }, { PLANT_FIBRE, 8 }, { ANIMAL_FUR, 2 } },
	},
	{
		item = "439e5967-5b96-4c28-afd2-13db09d7e605", template = "c110e001-4b38-462f-a317-3a6b0d627511",
		name_guid = "4782e479-fad2-448d-b34f-f77ffa5858af", desc_guid = "b9fa7b45-d5ed-47e1-98ab-51e2a7463a18",
		recipe = "9a2fcde6-2dfc-48f6-92ca-0ad394df23da", model = "5879f304-0050-4ecb-96f4-5cc0e66155d9",
		debug_name = "Prop_Beastmaster_Large_Stable", model_name = "beastmaster_large_stable", scale = 1.6,
		name = "Large Stable",
		description = "A big stable for your tamed beasts: the beasts you left at home live in it, walking about and "
			.. "lying down. Press E at it to open the Beasts window, where you choose which beasts go with you (needs the "
			.. "Beastmaster mod).",
		inputs = { { WOOD_LOGS, 12 }, { PLANT_FIBRE, 16 }, { ANIMAL_FUR, 4 } },
	},
	{
		item = "494d6061-2553-4636-9ba8-e254276d8076", template = "e8992a73-e950-42a7-89f6-06113a049f2c",
		name_guid = "75f595bf-e9a3-4fd8-954e-f50f0b3a4be6", desc_guid = "133507c9-5a9a-470d-b7f4-f99a49955e1a",
		recipe = "760447df-f144-4286-a5c3-b298848c9462", model = "bb3549a0-5088-4c3b-8057-3a7e48a59ad6",
		debug_name = "Prop_Beastmaster_Training_Grounds", model_name = "beastmaster_training_grounds", scale = 1,
		name = "Training Grounds",
		description = "A training ground for your tamed beasts. Press E at it to open the Beasts window and send a beast "
			.. "to train: it earns loyalty while you are in the world, and you can watch it go for the dummy (needs the "
			.. "Beastmaster mod).",
		inputs = { { WOOD_LOGS, 10 }, { PLANT_FIBRE, 12 }, { ANIMAL_FUR, 3 } },
		-- the prop 5 x 5 m, its planks 0.32 m up, the dummy 2 m tall in the middle: three boxes make the round platform
		-- walkable (a square and a cross: every point within 2.47 m of the middle along the axes and the diagonals), the
		-- fourth is the dummy
		boxes = {
			{ at = { 0.0, 0.16, 0.0 }, half = { 1.75, 0.16, 1.75 } },
			{ at = { 0.0, 0.16, 0.0 }, half = { 2.45, 0.16, 1.0 } },
			{ at = { 0.0, 0.16, 0.0 }, half = { 1.0, 0.16, 2.45 } },
			{ at = { 0.0, 1.32, 0.0 }, half = { 0.3, 1.0, 0.3 } },
		},
	},
}

-- The Stable's solid parts, in metres from its middle on the ground (the prop 4.5 x 3.4 x 2.95 m, its open side +z):
-- centre and half size of each box. The first is the plank floor (its top 0.19 m up, walkable); then the back wall,
-- the left wall, the low rail on the right (0.95 m high; the front is open) and the roof (from 2.25 m up). The Large
-- Stable's are these times its scale. (A place with boxes of its own names them: the Training Grounds.)
local BOXES = {
	{ at = { -0.01, 0.096, -0.017 }, half = { 2.01, 0.096, 1.434 } },
	{ at = { -0.06, 1.23, -1.125 }, half = { 1.86, 1.035, 0.225 } },
	{ at = { -1.69, 1.23, -0.11 }, half = { 0.225, 1.035, 1.24 } },
	{ at = { 1.69, 0.57, 0.0 }, half = { 0.135, 0.38, 1.125 } },
	{ at = { 0.0, 2.84, 0.0 }, half = { 2.25, 0.59, 1.6 } },
}

-- The bed's collider (one box) becomes `boxes` at `scale`: the box copied (EML copies a value put into a game array) and
-- each placed, all in the collider's one solid. A collider that is not one box is kept as it is.
local function fit_collider(tpl, scale, boxes)
	for _, c in ipairs(tpl.components) do
		if c.type == "keen::ecs::ColliderResourceComponent" then
			local col = c.value.colliders[1]
			local data = col and col.dataArray
			if not (data and #data == 1 and tostring(data[1].type) == "keen::ecs::BoxColliderData") then
				print("beastmaster stable: the bed's collider is not one box: kept as it is")
				return
			end
			for _ = 2, #boxes do table.insert(data, data[1]) end
			for i, b in ipairs(boxes) do
				local v = data[i].value
				v.offset.x, v.offset.y, v.offset.z = b.at[1] * scale, b.at[2] * scale, b.at[3] * scale
				v.shape.halfSize.x, v.shape.halfSize.y, v.shape.halfSize.z =
					b.half[1] * scale, b.half[2] * scale, b.half[3] * scale
			end
			local list = col.joltObjects[1].shapeIndexList
			for i = #list + 1, #boxes do table.insert(list, i - 1) end
		end
	end
end

-- The render model of stable `s`: only where the game has render data (not on a dedicated server). The pack built it
-- (its prop); when it did not, a copy of the bed's record under its GUID.
local function model(s)
	if #game.assets.get_resources_by_type("keen::RenderModel") == 0
		or game.assets.get_resource(s.model, "keen::RenderModel") then
		return
	end
	local bed = game.assets.get_resource(BED_MODEL, "keen::RenderModel")
	if not bed then error("the large animal bed's model " .. BED_MODEL .. " is not in the game (the game changed?)") end
	local m = game.assets.create_resource(bed.data, "keen::RenderModel", s.model, 0).data
	m.debugName = s.model_name
	print("beastmaster stable: the pack's " .. s.name .. " model was not made: it has the large animal bed's look")
end

local function recipe(s, like)
	return {
		guid = s.recipe, debug_name = "Craft_" .. s.debug_name,
		output = { s.item, 1 },
		inputs = s.inputs,
		like = like,
	}
end

-- Stable `s`: its model, the placeable and its recipe at the Workbench, listed after the crude chair; a game without that
-- recipe gets it in the own crafting menu (the helper's default), so it can still be crafted.
local function add(s)
	model(s)
	content.add_placeable{
		guid = s.item, template_guid = s.template, name_guid = s.name_guid, debug_name = s.debug_name,
		name = s.name, description_guid = s.desc_guid, description = s.description,
		clone_of = BED_ITEM,
		-- farm animals and villagers leave it alone; monsters neither target nor break it (a broken one fell to an item)
		strip = { "keen::ecs::AnimalTarget", "keen::ecs::ActiveNpcTarget", "keen::ecs::ServerTarget",
			"keen::ecs::Destructible", "keen::ecs::OnDestroy" },
		edit_template = function(tpl)
			for _, c in ipairs(tpl.components) do
				if c.type == "keen::ecs::ModelResource" then c.value.model = s.model end
			end
			fit_collider(tpl, s.scale, s.boxes or BOXES)
		end,
	}
	local done, why = pcall(content.add_recipe, recipe(s, LIKE))
	if not done then
		print("beastmaster stable: the Workbench recipe could not be copied (" .. tostring(why) .. "): the " .. s.name
			.. " is crafted in the own crafting menu instead")
		content.add_recipe(recipe(s, nil))
	end
	print("beastmaster stable: added " .. s.debug_name .. " (template " .. s.template .. ", model " .. s.model .. ")"
		.. (loader.is_server and " on the dedicated server (no model, icon, name or menu entry there)" or ""))
end

-- each on its own: one that fails leaves the others added; the first error comes after the names are written
local first_err
for _, s in ipairs(STABLES) do
	local ok, err = pcall(add, s)
	if not ok and not first_err then first_err = err end
end
content.finish()  -- the names of what was added, also when a step failed
if first_err then error(first_err, 0) end
