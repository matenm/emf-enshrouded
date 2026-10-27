-- Emberbrand: an example of new content for Enshrouded (an EML data patch; see docs/modding/content.md).
-- It adds one item, a sword cloned from the Bronze Sword with its own name and stats (tested in game: it shows with
-- its name, damage and fire). Hand it out with EMF ("put Weapon_T1_1H_Sword_Emberbrand 1").
-- It also adds a recipe, which is in the game's recipe table but NOT listed in the crafting menu: the menu gets its
-- recipes from somewhere else (knowledge unlocks; where exactly is not known, docs/modding/content.md).
--
-- The GUIDs below are FIXED FOREVER: a save stores items by ItemId = FNV-1a(GUID), so a new GUID would make every
-- saved Emberbrand vanish. Copy this mod for your own item and pick new GUIDs once (any GUID generator), then never
-- change them.
if not loader.features.patch then
	return
end

local ITEM_GUID   = "5e3a1f4c-2b7d-4e8a-9c61-7f0d2a4b8e11"
local NAME_GUID   = "5e3a1f4c-2b7d-4e8a-9c61-7f0d2a4b8e12"
local RECIPE_GUID = "5e3a1f4c-2b7d-4e8a-9c61-7f0d2a4b8e13"
local DEBUG_NAME  = "Weapon_T1_1H_Sword_Emberbrand"   -- what EMF's "items" and "give" commands use
local DISPLAY     = "Emberbrand"                       -- what players see (every language)

local SOURCE_ITEM   = "1d622bd9-8170-41b1-822e-d3984b386d95" -- Weapon_T1_1H_SwordEpic_01, the Bronze Sword
local SOURCE_RECIPE = "Consumable_Tool_Campfire_PK"          -- the campfire: own crafting menu, known from the start
local WOOD_GUID     = "3f4093ae-75ed-4e5d-bd0b-784ba83b88f5" -- Material_T1_Wood_Logs

local function log(...) print("emf_emberbrand:", ...) end

local itemId = game.guid.hash(ITEM_GUID)
local nameId = game.guid.hash(NAME_GUID)
local recipeId = game.guid.hash(RECIPE_GUID)

-- 1) the item: a copy of the Bronze Sword's whole resource (model, icon, animations, perks), then our changes
local src = game.assets.get_resource(SOURCE_ITEM, "keen::ItemInfo")
assert(src, "the Bronze Sword was not found (the game changed?)")
local item = game.assets.create_resource(src.data, "keen::ItemInfo", ITEM_GUID, 0).data
item.itemId = { value = itemId }
item.objectId = ITEM_GUID
item.name = NAME_GUID
item.debugName = DEBUG_NAME
item.damageSetup.dmgMod = 1.75                          -- the Bronze Sword: 1.0
item.damageSetup.distribution.magicalFireDamage = 0.5   -- and some fire

-- its icon: the game shows an item's icon from a UiTextureResource stored under the item's own GUID, and stops with
-- "corrupted game ui texture" when that is missing. The Bronze Sword's icon fits (the same model).
local icon = game.assets.get_resource(SOURCE_ITEM, "keen::UiTextureResource")
assert(icon, "the Bronze Sword's icon was not found (the game changed?)")
game.assets.create_resource(icon.data, "keen::UiTextureResource", ITEM_GUID, 0)

-- 2) register it: itemRefs and dbgNames are parallel lists
local registry = game.assets.get_resources_by_type("keen::ItemRegistryResource")[1].data
table.insert(registry.itemRefs, ITEM_GUID)
table.insert(registry.dbgNames, DEBUG_NAME)

-- 3) the recipe (not listed in the crafting menu, see the top): a copy of the campfire's, our output, 5 wood logs
local recipes = game.assets.get_resources_by_type("keen::RecipeRegistryResource")[1].data
local source = nil
for _, r in ipairs(recipes.recipes) do
	if r.debugName == SOURCE_RECIPE then source = r break end
end
assert(source, "the recipe " .. SOURCE_RECIPE .. " was not found (the game changed?)")
table.insert(recipes.recipes, source)
local recipe = recipes.recipes[#recipes.recipes]
recipe.recipeGuid = RECIPE_GUID
recipe.recipeId = { value = recipeId }
recipe.debugName = "Craft_" .. DEBUG_NAME
recipe.output[1].itemRef = ITEM_GUID
recipe.output[1].item = { value = itemId }
recipe.output[1].count = 1
while #recipe.input > 1 do table.remove(recipe.input) end
recipe.input[1].itemStack.itemRef = WOOD_GUID
recipe.input[1].itemStack.item = { value = game.guid.hash(WOOD_GUID) }
recipe.input[1].itemStack.count = 5
recipe.input[1].inputItemCategory.categoryRef = "00000000-0000-0000-0000-000000000000" -- the campfire takes "any wood"
recipe.input[1].inputItemCategory.category = { value = 0 }
recipe.input[1].inputItemCategory.count = 0
recipe.requiresSheltered = false
recipe.completionRequirementQuery.knowledgeOrQueryId = { value = recipeId } -- "crafted once" is tracked per recipe

-- 4) the name: one new tag in the keenglish table (the game falls back to it for every language), in sorted order
local collection = game.assets.get_resources_by_type("keen::LocaTagCollectionResource")[1].data
local content = game.assets.get_content(game.guid.from_content_hash(collection.keenglishData.hash))
local loca = content:read_data():read_resource("keen::LocaTagCollectionResourceData")
local at = #loca.tags + 1
for i, tag in ipairs(loca.tags) do
	assert(tag.id.value ~= nameId, "the name's loca id is already taken: pick another NAME_GUID")
	if tag.id.value > nameId then at = i break end
end
table.insert(loca.tags, at, { id = { value = nameId }, text = DISPLAY, arguments = {}, genericArguments = 0 })
local buf = buffer.create()
buf:write_resource("keen::LocaTagCollectionResourceData", loca)
collection.keenglishData.hash = game.assets.create_content(buf)

log(string.format("added %s (item 0x%08x, recipe 0x%08x)", DEBUG_NAME, itemId, recipeId))
