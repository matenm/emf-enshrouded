-- Read-only research mod: dumps the game's resource types and samples of
-- anything that looks related to NPCs, AI, companions, villagers or trading.
if not loader.features.export then
	return
end

local KEYWORDS = {
	"npc", "villager", "ai", "behavio", "companion", "summon", "pet", "follow",
	"mount", "task", "job", "work", "schedule", "trade", "vendor", "shop",
	"merchant", "animal", "farm", "crop", "plant", "spawn", "character",
	"actor", "party", "minion", "ally", "faction", "dialog", "quest", "bed",
	"settle", "craft", "inventory", "equip", "navigation", "nav", "path",
}

local MAX_SAMPLES = 3
local MAX_CHARS = 200000

local function matches(type_name)
	local lower = type_name:lower()
	for _, kw in ipairs(KEYWORDS) do
		if lower:find(kw, 1, true) then
			return kw
		end
	end
	return nil
end

local types = {}
for _, t in ipairs(game.assets.get_resource_types()) do
	types[#types + 1] = t.qualified_name
end
table.sort(types)

local index = { "type,count,matched_keyword" }
local matched = {}

for _, type_name in ipairs(types) do
	local ok, resources = pcall(game.assets.get_resources_by_type, type_name)
	local count = ok and #resources or -1
	local kw = matches(type_name)
	index[#index + 1] = string.format("%s,%d,%s", type_name, count, kw or "")
	if kw and ok then
		matched[#matched + 1] = { name = type_name, resources = resources }
	end
end

io.export("types_index.csv", table.concat(index, "\n"))
print(string.format("npc_explorer: %d resource types, %d matched keywords", #types, #matched))

for _, entry in ipairs(matched) do
	local parts = {}
	for i = 1, math.min(MAX_SAMPLES, #entry.resources) do
		local res = entry.resources[i]
		local ok, text = pcall(tostring, res.data)
		parts[#parts + 1] = string.format("### [%d/%d] guid=%s\n%s\n",
			i, #entry.resources, tostring(res.guid),
			ok and text or ("<tostring failed: " .. tostring(text) .. ">"))
	end
	local body = table.concat(parts, "\n")
	if #body > MAX_CHARS then
		body = body:sub(1, MAX_CHARS) .. "\n... [truncated]"
	end
	local safe = entry.name:gsub("[^%w_%-]", "_")
	io.export("samples/" .. safe .. ".txt", body)
end

require("templates")

print("npc_explorer: done")
