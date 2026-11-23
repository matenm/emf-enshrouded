-- Unit tests for tools/models/packs/beastmaster_harness.lua (the wilds pack's craftable Llama Harness and Camel
-- Saddle), run by test_beast_harness.py with a Lua 5.4 interpreter: lua beast_harness_spec.lua <path of the script>.
-- A stand-in for EMF's content helper records what the script asks of it.
local script = assert(arg[1], "usage: lua beast_harness_spec.lua <beastmaster_harness.lua>")

local HARNESS, HARNESS_RECIPE = "68cd9bab-6160-4fd0-b284-8b863dd147a9", "37871d39-a771-4f6f-9995-db22ee5ffeac"
local SADDLE, SADDLE_RECIPE = "ef50d499-52a8-49f1-a2e5-3c1f874e8a28", "b0585f21-b185-4829-abc7-2651648ee902"
local LEATHER = "b798d3b9-dafd-4aff-b805-94b607678136"
local RED_FABRIC = "a8d8c269-d97b-4ff6-8de2-2433828b4abc"

local failures, ran = 0, 0
local function test(name, fn)
	ran = ran + 1
	local ok, err = pcall(fn)
	if not ok then
		failures = failures + 1
		io.stderr:write("FAIL ", name, ": ", tostring(err), "\n")
	end
end
local function eq(a, b, what)
	if a ~= b then error(string.format("%s: %s ~= %s", what, tostring(a), tostring(b)), 2) end
end

-- runs the script with a stand-in helper; `refuse` makes every recipe that copies a game recipe fail, `broken_item`
-- names an item whose add_item fails
local function run(refuse, broken_item)
	local rec = { items = {}, recipes = {}, finished = 0 }
	local helper = {
		add_item = function(def)
			if def.guid == broken_item then error("no such item to copy") end
			rec.items[#rec.items + 1] = def
		end,
		add_recipe = function(def)
			if def.like and refuse then error("no recipe " .. def.like) end
			rec.recipes[#rec.recipes + 1] = def
		end,
		finish = function() rec.finished = rec.finished + 1 end,
	}
	local env = setmetatable({
		require = function(n) assert(n == "emf_content", n) return helper end,
		print = function() end,
	}, { __index = _G })
	local chunk = assert(loadfile(script, "t", env))
	rec.ok, rec.err = pcall(chunk)
	return rec
end

test("both items are copies of game materials with names and descriptions of their own", function()
	local r = run()
	assert(r.ok, r.err)
	eq(#r.items, 2, "items")
	eq(r.items[1].guid, HARNESS, "harness guid")
	eq(r.items[1].name, "Llama Harness", "harness name")
	eq(r.items[1].clone_of, LEATHER, "harness copy")
	eq(r.items[1].debug_name, "Item_Beastmaster_Llama_Harness", "harness debug name")
	eq(r.items[2].guid, SADDLE, "saddle guid")
	eq(r.items[2].name, "Camel Saddle", "saddle name")
	eq(r.items[2].clone_of, RED_FABRIC, "saddle copy")
	local seen = {}
	for _, it in ipairs(r.items) do
		assert(it.description and #it.description > 20, "description")
		for _, g in ipairs({ it.guid, it.name_guid, it.description_guid }) do
			assert(#g == 36 and not seen[g], "a GUID of its own: " .. tostring(g))
			seen[g] = true
		end
	end
	eq(r.finished, 1, "finish once")
end)

test("each has a Workbench recipe that gives one", function()
	local r = run()
	eq(#r.recipes, 2, "recipes")
	eq(r.recipes[1].guid, HARNESS_RECIPE, "harness recipe")
	eq(r.recipes[1].output[1], HARNESS, "harness output")
	eq(r.recipes[1].output[2], 1, "one harness")
	eq(r.recipes[1].like, "Prop_Decoration_Comfort1_T0_Chair", "at the Workbench")
	eq(r.recipes[2].guid, SADDLE_RECIPE, "saddle recipe")
	eq(r.recipes[2].output[1], SADDLE, "saddle output")
	for _, rc in ipairs(r.recipes) do assert(#rc.inputs >= 2, "inputs") end
end)

test("without the Workbench recipe to copy, both are crafted in the own crafting menu", function()
	local r = run(true)
	assert(r.ok, r.err)
	eq(#r.recipes, 2, "recipes")
	eq(r.recipes[1].like, nil, "no copied recipe")
end)

test("an item that fails leaves the other added, the names written and the error raised", function()
	local r = run(false, HARNESS)
	eq(r.ok, false, "raised")
	eq(#r.items, 1, "the saddle")
	eq(r.items[1].guid, SADDLE, "saddle")
	eq(r.finished, 1, "finish")
end)

print(string.format("%d tests, %d failures", ran, failures))
os.exit(failures == 0 and 0 or 1)
