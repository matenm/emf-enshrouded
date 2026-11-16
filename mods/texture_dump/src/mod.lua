-- Read-only export for the custom-model research: every map of a few creatures' materials as a PNG (the first
-- level at most MAX_SIZE wide), a material's record as JSON, and textures.csv with one row per map.
-- Run: emm.exe run -g <game> --export --export-directory <dir> with this mod in <game>\mods.
if not loader.features.export then return end

local s = tostring
local MAX_SIZE = 1024

local MODELS = {
	char_enemy_wolf_01 = true,
	enemy_wildboar_01 = true,
	char_enemy_sabretooth_01 = true,
	pet_dog_01 = true,
	pet_cat_01 = true,
	enemy_goat = true,
	Enemy_yak_01 = true,
	animal_catdeer = true,
	char_enemy_vukah_elite_01 = true,
	char_enemy_skeleton_mage_01 = true,
}

-- bytes of one 4x4 block
local function block_bytes(format)
	if format:find("BC1") or format:find("BC4") then return 8 end
	return 16
end

local function level_bytes(format, w, h)
	return math.max(1, (w + 3) // 4) * math.max(1, (h + 3) // 4) * block_bytes(format)
end

local rows = { "model,material,materialName,flags,map,format,width,height,levels,file,note" }

local function dump_texture(tex, file)
	local format, w, h = s(tex.format), tex.width, tex.height
	local content = game.assets.get_content(game.guid.from_content_hash(tex.content.hash))
	if not content then return "", "no content" end
	local data = content:read_data():to_string()
	local offset = 0
	while w > MAX_SIZE or h > MAX_SIZE do
		offset = offset + level_bytes(format, w, h)
		w, h = math.max(1, w // 2), math.max(1, h // 2)
	end
	local size = level_bytes(format, w, h)
	if #data < offset + size then return "", string.format("content has %d bytes, level needs %d", #data, offset + size) end
	local img = image.decode_texture(buffer.wrap(data:sub(offset + 1, offset + size)), tex.format, w, h, 0)
	io.export(file, image.encode(img, "png"))
	return file, string.format("%dx%d", w, h)
end

local seen = {}
for _, res in ipairs(game.assets.get_resources_by_type("keen::RenderModel")) do
	local name = s(res.data.debugName)
	if MODELS[name] and not seen[name] then
		seen[name] = true
		for i, m in ipairs(res.data.materials) do
			local mat = game.assets.get_resource(s(m.material), "keen::RenderMaterialResource")
			if mat then
				io.export(string.format("textures/%s.material%d.json", name, i), s(mat.data))
				local flags = {}
				for _, f in ipairs(mat.data.flags) do flags[#flags + 1] = s(f) end
				for _, im in ipairs(mat.data.images) do
					local tex = im.texture
					local map = s(tex.debugName)
					local file = string.format("textures/%s.material%d.%s.png", name, i, map:gsub("[^%w_]", "_"))
					local ok, written, note = pcall(dump_texture, tex, file)
					if not ok then written, note = "", s(written):gsub("[\r\n,]", " ") end
					rows[#rows + 1] = table.concat({ name, i, s(mat.data.debugName), table.concat(flags, "|"), map,
						s(tex.format), tex.width, tex.height, tex.levelCount, written, note }, ",")
				end
			end
		end
	end
end
io.export("textures/textures.csv", table.concat(rows, "\n"))
print("texture_dump: done")
