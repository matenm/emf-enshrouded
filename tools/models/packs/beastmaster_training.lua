-- The Training Grounds: a round wooden platform with a training dummy that players craft at the Workbench and place
-- in their base; Beastmaster opens its Beasts window with E at it (mods/beastmaster/stable.lua knows it by its render
-- model) and shows the beasts in training on it (mods/beastmaster/grounds.lua). A content script of the beasts pack
-- (make_mod.py "content_scripts"), run with EMF's helper emf_content.lua (docs/modding/content.md).
--
-- A copy of the large animal bed's object (as the Stable is) without the parts farm animals and villagers use, nor the
-- ones that let monsters target and break it, with the pack's prop model beastmaster_training_grounds under its own
-- model GUID, which Beastmaster knows it by, and boxes of its own to stand on and bump into. When its model was not made,
-- it takes a copy of the bed's record under the same GUID (the bed's look). A dedicated server has no render data:
-- there the template names the model all the same.
--
-- The GUIDs below are FIXED FOREVER: saves keep the item by its GUID and placed Training Grounds by the template's GUID,
-- and Beastmaster knows the model by its GUID.
local content = require("emf_content")

local BED_ITEM    = "a8076625-0529-415a-bf32-a9e0619e8ce5" -- Animal_Farm_Housing_Sleeping_Bed_YAK_Sanddigger
local BED_MODEL   = "7dd9139a-fba7-4e60-8b5a-fa853c250ef7" -- animalprops_farm_animal_sleep_large_01
local WOOD_LOGS   = "3f4093ae-75ed-4e5d-bd0b-784ba83b88f5" -- Material_T1_Wood_Logs
local PLANT_FIBRE = "ff00a9f6-fd7e-4933-9c6b-33c36223cd2b" -- Material_T1_Plant_Fibre
local ANIMAL_FUR  = "66ce7d4c-ce66-4ad4-800a-ab37f7e604a2" -- Material_T1_Animal_Fur
local LIKE        = "Prop_Decoration_Comfort1_T0_Chair"     -- the Workbench, known with the first furniture

local GROUNDS = {
	item = "494d6061-2553-4636-9ba8-e254276d8076", template = "e8992a73-e950-42a7-89f6-06113a049f2c",
	name_guid = "75f595bf-e9a3-4fd8-954e-f50f0b3a4be6", desc_guid = "133507c9-5a9a-470d-b7f4-f99a49955e1a",
	recipe = "760447df-f144-4286-a5c3-b298848c9462", model = "bb3549a0-5088-4c3b-8057-3a7e48a59ad6",
	debug_name = "Prop_Beastmaster_Training_Grounds", model_name = "beastmaster_training_grounds",
	name = "Training Grounds",
	description = "A training ground for your tamed beasts. Press E at it to open the Beasts window and send a beast to "
		.. "train: it earns loyalty while you are in the world, and you can watch it go for the dummy (needs the "
		.. "Beastmaster mod).",
	inputs = { { WOOD_LOGS, 10 }, { PLANT_FIBRE, 12 }, { ANIMAL_FUR, 3 } },
}

-- Its solid parts, in metres from its middle on the ground (the prop 5 x 5 m, its planks 0.32 m up, the dummy 2 m tall
-- in the middle): centre and half size of each box. Three boxes make the round platform walkable (a square and a cross:
-- every point within 2.47 m of the middle along the axes and the diagonals), the fourth is the dummy.
local BOXES = {
	{ at = { 0.0, 0.16, 0.0 }, half = { 1.75, 0.16, 1.75 } },
	{ at = { 0.0, 0.16, 0.0 }, half = { 2.45, 0.16, 1.0 } },
	{ at = { 0.0, 0.16, 0.0 }, half = { 1.0, 0.16, 2.45 } },
	{ at = { 0.0, 1.32, 0.0 }, half = { 0.3, 1.0, 0.3 } },
}

-- The bed's collider (one box) becomes the boxes: the box copied (EML copies a value put into a game array) and each
-- placed, all in the collider's one solid. A collider that is not one box is kept as it is.
local function fit_collider(tpl)
	for _, c in ipairs(tpl.components) do
		if c.type == "keen::ecs::ColliderResourceComponent" then
			local col = c.value.colliders[1]
			local data = col and col.dataArray
			if not (data and #data == 1 and tostring(data[1].type) == "keen::ecs::BoxColliderData") then
				print("beastmaster training grounds: the bed's collider is not one box: kept as it is")
				return
			end
			for _ = 2, #BOXES do table.insert(data, data[1]) end
			for i, b in ipairs(BOXES) do
				local v = data[i].value
				v.offset.x, v.offset.y, v.offset.z = b.at[1], b.at[2], b.at[3]
				v.shape.halfSize.x, v.shape.halfSize.y, v.shape.halfSize.z = b.half[1], b.half[2], b.half[3]
			end
			local list = col.joltObjects[1].shapeIndexList
			for i = #list + 1, #BOXES do table.insert(list, i - 1) end
		end
	end
end

-- Its render model: only where the game has render data (not on a dedicated server). The pack built it (its prop);
-- when it did not, a copy of the bed's record under its GUID.
local function model(s)
	if #game.assets.get_resources_by_type("keen::RenderModel") == 0
		or game.assets.get_resource(s.model, "keen::RenderModel") then
		return
	end
	local bed = game.assets.get_resource(BED_MODEL, "keen::RenderModel")
	if not bed then error("the large animal bed's model " .. BED_MODEL .. " is not in the game (the game changed?)") end
	local m = game.assets.create_resource(bed.data, "keen::RenderModel", s.model, 0).data
	m.debugName = s.model_name
	print("beastmaster training grounds: the pack's model was not made: it has the large animal bed's look")
end

local function recipe(s, like)
	return {
		guid = s.recipe, debug_name = "Craft_" .. s.debug_name,
		output = { s.item, 1 },
		inputs = s.inputs,
		like = like,
	}
end

local function add(s)
	model(s)
	content.add_placeable{
		guid = s.item, template_guid = s.template, name_guid = s.name_guid, debug_name = s.debug_name,
		name = s.name, description_guid = s.desc_guid, description = s.description,
		clone_of = BED_ITEM,
		-- farm animals and villagers leave it alone; monsters neither target nor break it
		strip = { "keen::ecs::AnimalTarget", "keen::ecs::ActiveNpcTarget", "keen::ecs::ServerTarget",
			"keen::ecs::Destructible", "keen::ecs::OnDestroy" },
		edit_template = function(tpl)
			for _, c in ipairs(tpl.components) do
				if c.type == "keen::ecs::ModelResource" then c.value.model = s.model end
			end
			fit_collider(tpl)
		end,
	}
	local done, why = pcall(content.add_recipe, recipe(s, LIKE))
	if not done then
		print("beastmaster training grounds: the Workbench recipe could not be copied (" .. tostring(why) .. "): it is "
			.. "crafted in the own crafting menu instead")
		content.add_recipe(recipe(s, nil))
	end
	print("beastmaster training grounds: added " .. s.debug_name .. " (template " .. s.template .. ", model " .. s.model
		.. ")" .. (loader.is_server and " on the dedicated server (no model, icon, name or menu entry there)" or ""))
end

local ok, err = pcall(add, GROUNDS)
content.finish()  -- the name of what was added, also when a step failed
if not ok then error(err, 0) end
