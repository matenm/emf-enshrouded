-- Runs a make_mod.py mod.lua against a mock of EML's API (plain Lua 5.4) to catch logic errors before a game run.
-- usage: lua mock_eml.lua <mod src folder>
local src = arg[1]
package.path = src .. "/?.lua;" .. package.path
-- EML's warn goes to its log as a warning (and into the build's reply); here: a line that says so
warn = function(...) print("warning:", ...) end

local function deepcopy(v)
	if type(v) ~= "table" then return v end
	local t = {}
	for k, x in pairs(v) do t[k] = deepcopy(x) end
	return t
end

local function texture(name, fmt, size)
	return { debugName = name, format = fmt, width = size, height = size, levelCount = 3,
		content = { hash = { size = 1 }, bundleId = 7, sortKey = 0 },
		mipTailContent = { hash = { size = 1 }, sortKey = 0 },
		mipChainContentRanges = { { start = 0, count = 10 } } }
end

local GRAPH = "keen::anim_graph::runtime_graph::AnimationGraphResource2_0"
local NONE = "00000000-0000-0000-0000-000000000000"
local resources = {
	["keen::RenderModel"] = {}, ["keen::RenderMaterialResource"] = {}, ["keen::ecs::TemplateResource"] = {},
	["keen::ModelHierarchyResource"] = {}, ["keen::JointAnimation"] = {}, [GRAPH] = {},
}
-- a creature on a skeleton of its own: the game skeleton, the game clips and the game graph it copies (the graph: a
-- clip node per clip, and a blend space naming every clip and one more); MOCK_MISSING=<guid> leaves one out (a game
-- update that removed it: a clip, a graph or a source model)
local missing = os.getenv("MOCK_MISSING") or ""
-- a creature on a skeleton of its own (new_skeleton): its source model's game skeleton (3 joints), a game clip for
-- each role, and the game graph: a clip node per role (the game's non-additive nodes name their clip as the additive
-- reference too), a clip node for a clip the pack does not name, a blend space, a blend space of offsets
-- (animationContentAdditive, as the wolf's lean), the slot map, and MOCK_IK=chains|feet|mask: an IK chain, foot IK
-- settings or a layer blend with a bone mask
local MOCK_HIERARCHY = "55555555-5555-4555-8555-555555555555"
local new_skeleton = {}
for _, c in ipairs(require("creatures")) do
	if c.skeleton and c.skeleton.new then
		new_skeleton[c.source_model_guid] = true
		local unit = { data = {} }
		resources["keen::ModelHierarchyResource"][MOCK_HIERARCHY] = { guid = MOCK_HIERARCHY, data = {
			hierarchyId = MOCK_HIERARCHY, jointCount = 3, jointNames = { 1, 2, 3 }, jointParentIndices = { 65535, 0, 1 },
			unbendableJoints = {}, skinToJointMatrices = { {}, {}, {} }, defaultPoseJoints = { {}, {}, {}, {} },
			defaultPose = { data = { unit, unit, unit, unit, unit, unit, unit, unit, unit, unit, unit, unit },
				alignedJointCount = 4 } } }
		local olds = {}
		for old in pairs(c.animation.roles) do olds[#olds + 1] = old end
		table.sort(olds)
		local nodes = {}
		for i, old in ipairs(olds) do
			resources["keen::JointAnimation"][old] = { guid = old, data = { headers = {}, data = {}, hierarchyHash = 7,
				jointCount = 3 } }
			nodes[#nodes + 1] = { type = "keen::anim_graph::runtime_graph::SampleAnimationClipNodeDefinition",
				value = { animation = old, additiveReferenceAnimation = i == 1 and old or NONE, animationDuration = 4 } }
		end
		nodes[#nodes + 1] = { type = "keen::anim_graph::runtime_graph::SampleAnimationClipNodeDefinition",
			value = { animation = "cccccccc-0000-4000-8000-000000000003", additiveReferenceAnimation = NONE,
				animationDuration = 4 } }
		nodes[#nodes + 1] = { type = "keen::anim_graph::runtime_graph::BlendSpace_1DNodeDefinition",
			value = { additiveReferenceAnimation = NONE, animationContentAdditive = false,
				items = { { animation = olds[1], animationDuration = 0.95 },
					{ animation = "dddddddd-0000-4000-8000-000000000004", animationDuration = 0.95 } } } }
		nodes[#nodes + 1] = { type = "keen::anim_graph::runtime_graph::BlendSpace_1DNodeDefinition",
			value = { additiveReferenceAnimation = NONE, animationContentAdditive = true,
				items = { { animation = "eeeeeeee-0000-4000-8000-000000000005", animationDuration = 0.15 },
					{ animation = "ffffffff-0000-4000-8000-000000000006", animationDuration = 0.15 } } } }
		local ik = os.getenv("MOCK_IK")
		nodes[#nodes + 1] = { type = "keen::anim_graph::runtime_graph::LayerBlendNodeDefinition",
			value = { blendOptions = { boneMask = ik == "mask" and "88888888-8888-4888-8888-888888888888" or NONE } } }
		local slots = {}
		for i = 1, 36 do slots[i] = 7 end
		resources[GRAPH][c.animation.graph] = { guid = c.animation.graph, data = { hierarchy = MOCK_HIERARCHY,
			nodeDefinitions = nodes, slotBoneIndexMapping = slots,
			ikChains = (ik == "chains" or ik == "1") and { {} } or {},
			feetIKSettingsReference = ik == "feet" and "77777777-7777-4777-8777-777777777777" or NONE,
			bipedFeetIkNodeId = { value = 0 }, genericFeetIkNodeId = { value = 0 } } }
	end
end
for _, c in ipairs(require("creatures")) do
	if c.skeleton and not c.skeleton.new then
		local sk = require(c.name .. "_skeleton")
		local n = #sk.joints
		local a = (n + 3) // 4 * 4  -- the game pads the default pose vectors to a multiple of 4 joints
		local joints, units, binds = {}, {}, {}
		for i = 1, n do
			joints[i] = { orientation = { x = 0, y = 0, z = 0, w = 1 }, position = { x = 0, y = 0, z = 0 }, scale = { x = 1, y = 1, z = 1 } }
			binds[i] = { rot = { x = { x = 1, y = 0, z = 0 }, y = { x = 0, y = 1, z = 0 }, z = { x = 0, y = 0, z = 1 } }, pos = { x = 0, y = 0, z = 0 } }
		end
		for i = 1, 3 * a do units[i] = { data = {} } end
		resources["keen::ModelHierarchyResource"][sk.source] = { guid = sk.source, data = { hierarchyId = sk.source,
			jointCount = n, defaultPoseJoints = joints, defaultPose = { data = units, alignedJointCount = a },
			skinToJointMatrices = binds } }
		local nodes, items = {}, {}
		for _, cl in ipairs(require(c.name .. "_clips")) do
			if cl.source ~= missing then
				resources["keen::JointAnimation"][cl.source] = { guid = cl.source, data = { headers = {}, data = {} } }
			end
			nodes[#nodes + 1] = { type = "keen::anim_graph::runtime_graph::SampleAnimationClipNodeDefinition",
				value = { animation = cl.source, additiveReferenceAnimation = NONE } }
			items[#items + 1] = { animation = cl.source }
		end
		items[#items + 1] = { animation = "99999999-9999-4999-8999-999999999999" }
		nodes[#nodes + 1] = { type = "keen::anim_graph::runtime_graph::BlendSpace_1DNodeDefinition",
			value = { items = items, additiveReferenceAnimation = NONE } }
		if c.animation.graph ~= missing then
			resources[GRAPH][c.animation.graph] = { guid = c.animation.graph, data = { hierarchy = sk.source, nodeDefinitions = nodes } }
		end
	end
end
local function model(guid)
	return { guid = guid, data = { debugName = "src", wholeModelRadius = 1.0, meshlets = {}, meshes = { {} }, lods = { {} },
		skinningMap = { 1 }, blendShapes = {}, blendShapeMask = 0, renderData = { hash = {} },
		-- a cloth simulation over the source's own vertices (the skeleton mage's robe, the knight's cape)
		cloth = { hasSkinning = true },
		materials = { { material = "mat-0" }, { material = "mat-1" } } } }
end
-- MOCK_SERVER=1: a dedicated server's data, which has no render data (no models, no materials) but has skeletons,
-- clips, graphs, templates and the attack data
local server = os.getenv("MOCK_SERVER") == "1"
for _, c in ipairs(require("creatures")) do
	if not server and c.source_model_guid ~= missing then
		resources["keen::RenderModel"][c.source_model_guid] = model(c.source_model_guid)
		if new_skeleton[c.source_model_guid] then
			resources["keen::RenderModel"][c.source_model_guid].data.hierarchy = MOCK_HIERARCHY
		end
	end
end
-- a model the packs do not use: the game has render data even when one source model is gone
if not server then resources["keen::RenderModel"]["other-model"] = model("other-model") end
for _, g in ipairs(server and {} or { "mat-0", "mat-1" }) do
	resources["keen::RenderMaterialResource"][g] = { guid = g, data = { images = {
		{ texture = texture("Material_x_albedo_map_0", "BC7_srgb_block", 2048) },
		{ texture = texture("Material_x_normal_map_0", "BC5_unorm_block", 2048) },
		{ texture = texture("Material_x_material_map_0", "BC1_RGB_unorm_block", 2048) },
		{ texture = texture("Material_x_height_map_0", "BC4_unorm_block", 4096) } } } }
end
-- the source templates' attacks: an arsenal in the registry (EML copies a value put into a game array: so does the
-- mock's array) with a combo (hits at 0.53 s for 0.10 s and 0.92 s for 0.08 s, a sound at 2.5 s) and a kick;
-- MOCK_NO_ARSENAL=1: templates without an EnemyArsenalReference
local MOCK_ARSENAL = "66666666-6666-4666-8666-666666666666"
local SEQ_COMBO, SEQ_KICK = "77777777-7777-4777-8777-777777777771", "77777777-7777-4777-8777-777777777772"
local SEQ = "keen::actor::ActorSequenceResource"
local function event(kind, t, d) return { type = kind, value = { time = { value = t }, duration = { value = d } } } end
resources[SEQ] = {
	[SEQ_COMBO] = { guid = SEQ_COMBO, data = { resourceId = SEQ_COMBO, subSequences = { { length = { value = 2400000000 },
		events = { event("keen::actor::SetAnimationEvent", 0, 0), event("keen::actor::SpawnImpact", 530000000, 100000000),
			event("keen::actor::SpawnImpact", 920000000, 80000000), event("keen::SfxNotifierEvent", 2500000000, 0) } } } } },
	[SEQ_KICK] = { guid = SEQ_KICK, data = { resourceId = SEQ_KICK, subSequences = { { length = { value = 1500000000 },
		events = { event("keen::actor::SpawnImpact", 700000000, 100000000) } } } } },
}
local arsenals = setmetatable({ { guid = MOCK_ARSENAL, attacks = {
	{ guid = "attack-1", description = { actionSequence = SEQ_COMBO } },
	{ guid = "attack-2", description = { actionSequence = SEQ_KICK } } } } },
	{ __newindex = function(t, k, v) rawset(t, k, deepcopy(v)) end })
local arsenal_registry = { guid = "arsenal-registry", data = { arsenals = arsenals } }
-- the game's list of every actor sequence (the game plays only the sequences listed there)
local sequence_collection = { guid = "sequence-collection", data = { sequences = { SEQ_COMBO, SEQ_KICK } } }
local templates = {}
for _, c in ipairs(require("creatures")) do
	local comps = { { type = "keen::ecs::ModelResource", value = { model = "old" } }, { type = "keen::ecs::Faction", value = {} },
		{ type = "keen::ecs::AnimationGraphResourceComponent2", value = { animationGraph = "source-graph" } },
		-- the game wolf's attribute storage (DamageMod: value, base, multiplier, adder; Movement: 18 speeds)
		{ type = "keen::ecs::DamageMod", value = { dataStorage = { 0, 0.5, 1, 0 } } },
		{ type = "keen::ecs::Movement", value = { dataStorage = { 0, 1, 0.5, 2.3, 6, 10, 1.8, 0, 2.3, 3.6, 1, 1, 2, 3.5, 2, 3.5, 1, 1 } } } }
	if os.getenv("MOCK_NO_ARSENAL") ~= "1" then
		comps[#comps + 1] = { type = "keen::ecs::EnemyArsenalReference", value = { arsenal = MOCK_ARSENAL } }
	end
	templates[#templates + 1] = { guid = "tpl-" .. c.source_template, data = { name = c.source_template, components = comps } }
end
local created = {}
game = {
	guid = { from_content_hash = function(h) return h end },
	assets = {
		get_resource = function(guid, typ) return (resources[typ] or {})[guid] end,
		get_resources_by_type = function(typ)
			if typ == "keen::ecs::TemplateResource" then return templates end
			if typ == "keen::GuidRegistryResource" then return { { data = { guids = { "tpl-Animal_Wildlife_Dog_04_black_AG2" } } } } end
			if typ == "keen::enemy::EnemyArsenalRegistryResource" then return { arsenal_registry } end
			if typ == "keen::ActorSequenceCollectionResource" then return { sequence_collection } end
			local list = {}
			for _, r in pairs(resources[typ] or {}) do list[#list + 1] = r end
			return list
		end,
		create_resource = function(value, typ, guid, part)
			local r = { guid = guid, data = deepcopy(value) }
			created[#created + 1] = typ .. " " .. guid
			resources[typ] = resources[typ] or {}
			resources[typ][guid] = r
			return r
		end,
		create_content = function(buf)
			return { size = #buf.s, hash0 = 1, hash1 = 2, hash2 = 12345 }
		end,
		get_content = function(h)
			return { read_data = function() return { to_string = function() return string.rep("\0", 64) end } end }
		end,
	},
}
local Buf = {}
Buf.__index = Buf
function Buf:to_string() return self.s end
function Buf:write_string(x) self.s = self.s .. x end
buffer = {
	create = function() return setmetatable({ s = "" }, Buf) end,
	wrap = function(s) assert(type(s) == "string", "buffer.wrap needs a string") return setmetatable({ s = s }, Buf) end,
}
local Img = {}
Img.__index = Img
function Img:set_pixel(x, y, r, g, b, a) assert(x < self.width and y < self.height) end
function Img:get_pixel(x, y) return 10, 20, 30, 255 end
image = {
	create = function(w, h) return setmetatable({ width = w, height = h }, Img) end,
	decode = function(buf, fmt)
		assert(buf.s:sub(2, 4) == "PNG", "not a PNG")
		local w = string.unpack(">I4", buf.s, 17)
		return setmetatable({ width = w, height = w }, Img)
	end,
	decode_texture = function(buf, fmt, w, h, lvl) return setmetatable({ width = w, height = h }, Img) end,
	encode_texture = function(img, fmt)
		local block = (tostring(fmt):find("BC1") or tostring(fmt):find("BC4")) and 8 or 16
		local n = math.max(1, (img.width + 3) // 4) * math.max(1, (img.height + 3) // 4) * block
		return setmetatable({ s = string.rep("x", n) }, Buf)
	end,
}
loader = { features = { patch = true } }
-- EML's Lua has require but no global package table (require keeps working: it uses the registry)
package = nil
dofile(src .. "/mod.lua")
print("created:")
for _, c in ipairs(created) do print("  " .. c) end

-- every template's components: "template <name> <type> <field> <values>"
local function num(v)
	return type(v) == "number" and string.format("%.6g", v) or tostring(v)
end
local function show_template(t)
	for _, comp in ipairs(t.data.components) do
		local keys = {}
		for k in pairs(comp.value) do keys[#keys + 1] = k end
		table.sort(keys)
		for _, k in ipairs(keys) do
			local v = comp.value[k]
			local text = num(v)
			if type(v) == "table" then
				local parts = {}
				for _, x in ipairs(v) do parts[#parts + 1] = num(x) end
				text = table.concat(parts, " ")
			end
			print(string.format("template %s %s %s %s", t.data.name, comp.type, k, text))
		end
	end
end
for _, t in ipairs(templates) do show_template(t) end
for _, t in pairs(resources["keen::ecs::TemplateResource"]) do show_template(t) end
-- arsenals ("arsenal <guid> sequences <each attack's sequence>") and sequences ("sequence <guid> id <resourceId>
-- length <ns> events <Type>@<time>+<duration> ...")
for _, a in ipairs(arsenals) do
	local seqs = {}
	for _, atk in ipairs(a.attacks) do seqs[#seqs + 1] = tostring(atk.description.actionSequence) end
	print(string.format("arsenal %s sequences %s", tostring(a.guid), table.concat(seqs, " ")))
end
local listed = {}
for _, g in ipairs(sequence_collection.data.sequences) do listed[#listed + 1] = tostring(g) end
print("sequence collection " .. table.concat(listed, " "))
for g, r in pairs(resources[SEQ]) do
	for _, sub in ipairs(r.data.subSequences) do
		local evs = {}
		for _, ev in ipairs(sub.events) do
			evs[#evs + 1] = string.format("%s@%d+%d", ev.type:match("([^:]+)$"), ev.value.time.value, ev.value.duration.value)
		end
		print(string.format("sequence %s id %s length %d events %s", g, tostring(r.data.resourceId), sub.length.value,
			table.concat(evs, " ")))
	end
end
for g, r in pairs(resources["keen::RenderModel"]) do
	print(string.format("model %s cloth %s", g, r.data.cloth == nil and "none" or "kept"))
	if r.data.hierarchy then print(string.format("model %s hierarchy %s", g, tostring(r.data.hierarchy))) end
	if r.data.hierarchyJointCount then
		print(string.format("model %s hierarchyJointCount %s", g, tostring(r.data.hierarchyJointCount)))
	end
end
-- skeleton copies, clip copies and graph copies
local copies = {}
for _, line in ipairs(created) do
	local typ, guid = line:match("^(%S+) (%S+)$")
	copies[guid] = typ
end
for g, r in pairs(resources["keen::ModelHierarchyResource"]) do
	if copies[g] then
		local d = r.data
		print(string.format("skeleton %s joints %d id %s", g, #d.defaultPoseJoints, tostring(d.hierarchyId)))
		print(string.format("skeleton %s jointCount %s names %d parents %d binds %d aligned %s poses %d data %d id %s", g,
			tostring(d.jointCount), #(d.jointNames or {}), #(d.jointParentIndices or {}), #d.skinToJointMatrices,
			tostring(d.defaultPose.alignedJointCount), #d.defaultPoseJoints, #d.defaultPose.data, tostring(d.hierarchyId)))
		local o = d.defaultPoseJoints[4] and d.defaultPoseJoints[4].orientation
		if o then print(string.format("pose 4 %.4f %.4f %.4f %.4f", o.x, o.y, o.z, o.w)) end
		-- the last joint's position in the default pose vectors (after the padded rotations)
		local n, a = #d.defaultPoseJoints, d.defaultPose.alignedJointCount
		local b = d.defaultPose.data[a + n].data
		if b and #b == 16 then
			print(string.format("position %d %.4f %.4f %.4f", n, string.unpack("<fff", string.char(table.unpack(b)))))
		end
	end
end
for g, r in pairs(resources["keen::JointAnimation"]) do
	if copies[g] then
		print(string.format("clip copy %s hash %s joints %s frames %s", g, tostring(r.data.hierarchyHash),
			tostring(r.data.jointCount), tostring(r.data.frameCount)))
	end
end
for g, r in pairs(resources[GRAPH]) do
	if copies[g] then
		local ours, left = 0, 0
		local function count(v)
			if v == nil or tostring(v) == NONE then return end
			if copies[tostring(v)] == "keen::JointAnimation" then ours = ours + 1 else left = left + 1 end
		end
		for _, node in ipairs(r.data.nodeDefinitions) do
			count(node.value.animation)
			count(node.value.additiveReferenceAnimation)
			for _, it in ipairs(node.value.items or {}) do count(it.animation) end
		end
		print(string.format("graph %s hierarchy %s ours %d left %d", g, tostring(r.data.hierarchy), ours, left))
		for i, node in ipairs(r.data.nodeDefinitions) do
			if node.value.animationDuration then
				print(string.format("graph %s node %d %s duration %s", g, i, tostring(node.value.animation),
					num(node.value.animationDuration)))
			end
			for k, it in ipairs(node.value.items or {}) do
				print(string.format("graph %s item %d %s duration %s", g, k, tostring(it.animation), num(it.animationDuration)))
			end
		end
		if r.data.slotBoneIndexMapping then
			print(string.format("graph %s slots %s", g, table.concat(r.data.slotBoneIndexMapping, " ")))
		end
	end
end
for g, r in pairs(resources["keen::RenderMaterialResource"]) do
	if g:find("-") and #g > 10 then
		for i, im in ipairs(r.data.images) do
			local t = im.texture
			local rs = {}
			for _, x in ipairs(t.mipChainContentRanges) do rs[#rs + 1] = x.start .. "+" .. x.count end
			print(string.format("  %s image %d %s %dx%d levels %d tail sort %s ranges %s", g, i, t.format, t.width, t.height,
				t.levelCount, tostring(t.mipTailContent.sortKey), table.concat(rs, " ")))
		end
	end
end
