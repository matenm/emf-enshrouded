-- Read-only survey for custom creature models (tools/models):
-- models (mesh counts, vertex formats, skeleton), skeletons, animations, retarget
-- data, and which creature/NPC templates bind which model + animation graph.
if not loader.features.export then
	return
end

local function s(v)
	local ok, text = pcall(tostring, v)
	return ok and text or "?"
end

local function csv(name, header, rows)
	table.insert(rows, 1, header)
	io.export(name, table.concat(rows, "\n"))
end

-- Render models
local rows = {}
for _, res in ipairs(game.assets.get_resources_by_type("keen::RenderModel")) do
	local ok, err = pcall(function()
		local d = res.data
		local verts, idx, formats, quant, meshCount = 0, 0, {}, {}, 0
		for _, m in ipairs(d.meshes) do
			meshCount = meshCount + 1
			verts = verts + m.vertexCount
			idx = idx + m.indexCount
			formats[s(m.vertexFormat) .. "/" .. s(m.vertexStride)] = true
			quant[s(m.positionQuantization)] = true
		end
		local function keys(t)
			local out = {}
			for k in pairs(t) do out[#out + 1] = k end
			table.sort(out)
			return table.concat(out, " ")
		end
		rows[#rows + 1] = string.format('%s,"%s",%s,%d,%d,%d,%d,%d,%d,%d,%s,%s,%s,%s,%d',
			s(res.guid), s(d.debugName), s(d.hierarchy), d.jointCount, d.hierarchyJointCount,
			meshCount, #d.lods, #d.meshlets, verts, idx // 3, keys(formats), keys(quant),
			d.cloth ~= nil and "cloth" or "", #d.blendShapes > 0 and "blend" or "", #d.materials)
	end)
	if not ok then rows[#rows + 1] = s(res.guid) .. ',"<error ' .. s(err):gsub('"', "'") .. '>"' end
end
csv("models.csv", "guid,debugName,hierarchy,jointCount,hierarchyJointCount,meshes,lods,meshlets,vertices,triangles,vertexFormat/stride,positionQuantization,cloth,blendShapes,materials", rows)

-- Skeletons
rows = {}
for _, res in ipairs(game.assets.get_resources_by_type("keen::ModelHierarchyResource")) do
	local d = res.data
	rows[#rows + 1] = string.format("%s,%s,%d", s(res.guid), s(d.hierarchyId), d.jointCount)
end
csv("hierarchies.csv", "guid,hierarchyId,jointCount", rows)

-- Animations
rows = {}
for _, res in ipairs(game.assets.get_resources_by_type("keen::JointAnimation")) do
	local d = res.data
	rows[#rows + 1] = string.format("%s,%s,%d,%d,%d,%s", s(res.guid), s(d.hierarchyHash), d.fps, d.frameCount, d.jointCount, s(d.flags))
end
csv("animations.csv", "guid,hierarchyHash,fps,frameCount,jointCount,flags", rows)

-- Retarget data
rows = {}
for _, res in ipairs(game.assets.get_resources_by_type("keen::anim_graph::RetargetDataResource")) do
	rows[#rows + 1] = string.format("%s,%d,%s", s(res.guid), #res.data.entries, s(res.data.verticalOffset))
end
csv("retarget.csv", "guid,entries,verticalOffset", rows)

-- Template bindings: model + animation graph + retarget per template
rows = {}
for _, res in ipairs(game.assets.get_resources_by_type("keen::ecs::TemplateResource")) do
	local d = res.data
	local model, graph, retarget, faction, scale = "", "", "", "", ""
	for _, v in ipairs(d.components) do
		local t = v.type
		if t == "keen::ecs::ModelResource" then
			model = s(v.value.model)
		elseif t == "keen::ecs::AnimationGraphResourceComponent2" then
			graph = s(v.value.animationGraph)
			retarget = s(v.value.retargetData)
		elseif t == "keen::ecs::Faction" then
			faction = s(v.value.faction)
		end
	end
	if model ~= "" and graph ~= "" then
		rows[#rows + 1] = string.format('"%s",%s,%s,%s,%s', d.name, faction, model, graph, retarget)
	end
end
csv("bindings.csv", "template,faction,model,animationGraph,retargetData", rows)

-- One full model sample (the first skinned model) so the field encoding can be read.
for _, res in ipairs(game.assets.get_resources_by_type("keen::RenderModel")) do
	if res.data.jointCount > 20 then
		io.export("sample_model.txt", s(res.guid) .. "\n" .. s(res.data))
		break
	end
end

-- Animation graphs: skeleton + slot -> bone mapping (names some bones: Head, HandR, FootL, ...).
rows = {}
for _, res in ipairs(game.assets.get_resources_by_type("keen::anim_graph::runtime_graph::AnimationGraphResource2_0")) do
	local d = res.data
	local map = {}
	for i, b in ipairs(d.slotBoneIndexMapping) do
		map[i] = tostring(b)
	end
	rows[#rows + 1] = string.format("%s,%s,%s", s(res.guid), s(d.hierarchy), table.concat(map, " "))
end
csv("graph_slots.csv", "guid,hierarchy,slotBoneIndexMapping", rows)

-- Every skeleton in full (rest pose, inverse binds, parents, name hashes) for the glTF exporter.
for _, res in ipairs(game.assets.get_resources_by_type("keen::ModelHierarchyResource")) do
	io.export("hierarchies/" .. s(res.guid) .. ".json", s(res.data))
end

-- Raw dumps of a few named models (record, render-data blob, skeleton) for decoding.
local DUMP = {
	pet_dog_01 = true,
	pet_dog_04 = true,
	char_enemy_wolf_01 = true,                               -- Beastmaster: wolves (combat skeleton)
	enemy_wildboar_01 = true,                                -- Beastmaster: boars
	char_enemy_sabretooth_01 = true,                         -- Beastmaster: big cat; a bear candidate skeleton
	char_enemy_skeleton_melee_02_GemOfNecromancy = true,     -- Summoner: the game's summoned skeleton
	enemy_wildlife_plague = true,                            -- Summoner: flying skull / wisp body
	char_enemy_skeleton_mage_01 = true,                      -- Summoner: skeleton mage
	char_enemy_skeleton_hound_01_Skeleton_Hound = true,      -- Summoner: skeleton hound
	enemy_skeleton_heavy_melee_01 = true,                    -- Necromancer: the heavy skeleton (a 9 m boss: not used)
	char_enemy_skeleton_elite_02 = true,                     -- Necromancer: the bone golem's body (Heavy Hero, 4 m)
}
for _, res in ipairs(game.assets.get_resources_by_type("keen::RenderModel")) do
	local name = s(res.data.debugName)
	if DUMP[name] then
		io.export("dump/" .. name .. ".json", s(res.data))
		local content = game.assets.get_content(res.data.renderData.hash)
		if content then
			io.export("dump/" .. name .. ".render.bin", content:read_data())
		end
		local h = game.assets.get_resource(s(res.data.hierarchy), "keen::ModelHierarchyResource")
		if h then
			io.export("dump/" .. name .. ".hierarchy.json", s(h.data))
		end
		for i, m in ipairs(res.data.materials) do
			local mat = game.assets.get_resource(s(m.material), "keen::RenderMaterialResource")
			if mat then
				io.export(string.format("dump/%s.material%d.json", name, i), s(mat.data))
			end
		end
	end
end

print("model_survey: done")
