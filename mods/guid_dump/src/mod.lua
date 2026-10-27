-- Read-only: exports every ECS template's name, GUID and family as CSV.
if not loader.features.export then
	return
end

local rows = { "name,guid,family" }
for _, res in ipairs(game.assets.get_resources_by_type("keen::ecs::TemplateResource")) do
	local data = res.data
	rows[#rows + 1] = string.format("%s,%s,%s", tostring(data.name), tostring(res.guid), tostring(data.family))
end
io.export("template_guids.csv", table.concat(rows, "\n"))
