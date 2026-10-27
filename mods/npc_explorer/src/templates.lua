-- Catalog every ECS template: name, family, component types, plus flags for
-- anything that hints at allies / followers / minions / tame animals / villagers.
local templates = game.assets.get_resources_by_type("keen::ecs::TemplateResource")

local rows = { "name,family,flags,components" }
local interesting = {}

local function short(t)
	return (t:gsub("^keen::ecs::", ""):gsub("^keen::", ""))
end

for _, res in ipairs(templates) do
	local data = res.data
	local comps = {}
	local flags = {}
	for _, variant in ipairs(data.components) do
		local t = variant.type
		comps[#comps + 1] = short(t)
		if t == "keen::ecs::Faction" then
			local f = tostring(variant.value.faction)
			flags[#flags + 1] = "faction=" .. f
			if f == "PlayerAlly" or f == "AutomatedPlayer" then
				interesting[#interesting + 1] = res
			end
		elseif t == "keen::ecs::Enemy01BehaviorState" then
			if variant.value.guardSetup.canGuardPlayer then
				flags[#flags + 1] = "canGuardPlayer"
				interesting[#interesting + 1] = res
			end
		elseif t == "keen::ecs::IAmAMinion" then
			flags[#flags + 1] = "minion"
			interesting[#interesting + 1] = res
		elseif t == "keen::ecs::IsTame" then
			flags[#flags + 1] = "tame"
		elseif t == "keen::ecs::ActiveNpcState" then
			flags[#flags + 1] = "activeNpc:" .. tostring(variant.value.npcType)
		end
	end
	rows[#rows + 1] = string.format('"%s",%s,%s,"%s"',
		data.name, tostring(data.family), table.concat(flags, "|"), table.concat(comps, " "))
end

io.export("templates.csv", table.concat(rows, "\n"))

-- Full dumps of the interesting ones (deduplicated, capped).
local seen, n = {}, 0
for _, res in ipairs(interesting) do
	if not seen[res.guid] and n < 40 then
		seen[res.guid] = true
		n = n + 1
		local safe = res.data.name:gsub("[^%w_%-]", "_")
		local lines = {}
		for _, variant in ipairs(res.data.components) do
			local ok, text = pcall(tostring, variant.value)
			lines[#lines + 1] = "## " .. variant.type .. "\n" .. (ok and text or "<unprintable>")
		end
		io.export("interesting/" .. safe .. ".txt", table.concat(lines, "\n"))
	end
end

-- Items that sound like summons / companions / pets.
local items = game.assets.get_resources_by_type("keen::ItemInfo")
local item_rows = { "debugName" }
for _, res in ipairs(items) do
	local name = res.data.debugName or ""
	local l = name:lower()
	if l:find("summon") or l:find("compan") or l:find("minion") or l:find("pet")
		or l:find("tame") or l:find("ally") or l:find("follow") or l:find("npc")
		or l:find("villag") or l:find("townsfolk") then
		item_rows[#item_rows + 1] = name
	end
end
io.export("items_of_interest.csv", table.concat(item_rows, "\n"))

print(string.format("templates: %d total, %d interesting", #templates, n))
