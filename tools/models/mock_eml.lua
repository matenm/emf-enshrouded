-- Runs a make_mod.py mod.lua against a mock of EML's API (plain Lua 5.4) to catch logic errors before a game run.
-- usage: lua mock_eml.lua <mod src folder>
local src = arg[1]
package.path = src .. "/?.lua;" .. package.path

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

local resources = {
	["keen::RenderModel"] = {}, ["keen::RenderMaterialResource"] = {}, ["keen::ecs::TemplateResource"] = {},
}
local function model(guid)
	return { guid = guid, data = { debugName = "src", wholeModelRadius = 1.0, meshlets = {}, meshes = { {} }, lods = { {} },
		skinningMap = { 1 }, blendShapes = {}, blendShapeMask = 0, renderData = { hash = {} },
		-- a cloth simulation over the source's own vertices (the skeleton mage's robe, the knight's cape)
		cloth = { hasSkinning = true },
		materials = { { material = "mat-0" }, { material = "mat-1" } } } }
end
for _, c in ipairs(require("creatures")) do
	resources["keen::RenderModel"][c.source_model_guid] = model(c.source_model_guid)
end
for _, g in ipairs({ "mat-0", "mat-1" }) do
	resources["keen::RenderMaterialResource"][g] = { guid = g, data = { images = {
		{ texture = texture("Material_x_albedo_map_0", "BC7_srgb_block", 2048) },
		{ texture = texture("Material_x_normal_map_0", "BC5_unorm_block", 2048) },
		{ texture = texture("Material_x_material_map_0", "BC1_RGB_unorm_block", 2048) },
		{ texture = texture("Material_x_height_map_0", "BC4_unorm_block", 4096) } } } }
end
local templates = {}
for _, c in ipairs(require("creatures")) do
	templates[#templates + 1] = { guid = "tpl-" .. c.source_template, data = { name = c.source_template,
		components = { { type = "keen::ecs::ModelResource", value = { model = "old" } }, { type = "keen::ecs::Faction", value = {} },
			-- the game wolf's attribute storage (DamageMod: value, base, multiplier, adder; Movement: 18 speeds)
			{ type = "keen::ecs::DamageMod", value = { dataStorage = { 0, 0.5, 1, 0 } } },
			{ type = "keen::ecs::Movement", value = { dataStorage = { 0, 1, 0.5, 2.3, 6, 10, 1.8, 0, 2.3, 3.6, 1, 1, 2, 3.5, 2, 3.5, 1, 1 } } } } } }
end
local created = {}
game = {
	guid = { from_content_hash = function(h) return h end },
	assets = {
		get_resource = function(guid, typ) return resources[typ][guid] end,
		get_resources_by_type = function(typ)
			if typ == "keen::ecs::TemplateResource" then return templates end
			if typ == "keen::GuidRegistryResource" then return { { data = { guids = { "tpl-Animal_Wildlife_Dog_04_black_AG2" } } } } end
			return {}
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
for g, r in pairs(resources["keen::RenderModel"]) do
	print(string.format("model %s cloth %s", g, r.data.cloth == nil and "none" or "kept"))
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
