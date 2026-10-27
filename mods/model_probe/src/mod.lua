-- Read-only probe for the custom-model research: which EML texture calls work (export mode, nothing is written to
-- the game). A Rust panic in EML aborts the whole run, so every step is written to probe_steps.txt BEFORE it runs:
-- the last line of that file is the step that crashed.
if not loader.features.export then return end
local steps = {}
local function step(text)
	steps[#steps + 1] = text
	io.export("probe_steps.txt", table.concat(steps, "\n"))
end

-- animation research (before the texture tests, which may crash): a few clips of the dog (66 joints) and game wolf
-- (68 joints) skeletons, and the dog's animation graph
step("dump animations")
local per = { [66] = 0, [68] = 0 }
for _, res in ipairs(game.assets.get_resources_by_type("keen::JointAnimation")) do
	local n = res.data.jointCount
	if per[n] and per[n] < 3 and res.data.frameCount >= 20 and res.data.frameCount <= 120 then
		per[n] = per[n] + 1
		io.export(string.format("anim/j%d_%s.json", n, tostring(res.guid)), tostring(res.data))
	end
end
local graph = game.assets.get_resource("7465e6df-ef34-43d2-b17d-00ecde61e4f7",
	"keen::anim_graph::runtime_graph::AnimationGraphResource2_0")
if graph then io.export("anim/dog_graph.json", tostring(graph.data)) end
step("  -> dog clips " .. per[66] .. ", wolf clips " .. per[68] .. ", graph " .. tostring(graph ~= nil))

local function flat(w, h)
	local img = image.create(w, h)
	for y = 0, h - 1 do
		for x = 0, w - 1 do img:set_pixel(x, y, 90, 120, 200, 255) end
	end
	return img
end

for _, fmt in ipairs({ "BC7_srgb_block", "BC5_unorm_block", "BC1_RGB_unorm_block", "BC4_unorm_block" }) do
	for _, size in ipairs({ 128, 8, 4, 2, 1 }) do
		step("encode " .. fmt .. " " .. size .. "x" .. size)
		local ok, r = pcall(function() return #image.encode_texture(flat(size, size), fmt):to_string() end)
		step("  -> " .. tostring(ok) .. " " .. tostring(r))
	end
	step("decode one block of " .. fmt .. " as 4x4")
	local enc = image.encode_texture(flat(4, 4), fmt)
	local ok, r, g, b = pcall(function()
		local img = image.decode_texture(enc, fmt, 4, 4, 0)
		return img:get_pixel(0, 0)
	end)
	step("  -> " .. tostring(ok) .. " " .. tostring(r) .. "," .. tostring(g) .. "," .. tostring(b))
end
step("decode PNG")
local png = image.encode(flat(16, 16), "png")
local ok, w = pcall(function() return image.decode(png, "png").width end)
step("  -> " .. tostring(ok) .. " width " .. tostring(w))
step("done")
print("model_probe: done")
