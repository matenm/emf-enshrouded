"""Make an EML content mod that adds new RenderModels and creature templates that use them.

  python tools/models/make_mod.py pack.json [--leave-out <creature name>]...

--leave-out builds the pack without that creature (one whose licence is not settled), its files removed from the out
folder; the mod's description is the pack's and each kept creature's own "description" (its sentence and credit).

pack.json:
  {
    "id": "my_beasts", "title": "My beasts", "out": "content/my_beasts",
    "creatures": [
      {
        "name": "beast_wolf",                         # debugName of the new model; also the Lua module names
        "description": "And a wolf.",                 # optional: added to the mod's description while it is in it
        "glb": "beast.glb",                           # a skinned glb (bones named jNNN...), or
        "game_dump": "<survey>/dump/pet_dog_01",      # a dumped game model (<stem>.json + .render.bin): round trip
        "hierarchy": "<survey>/hierarchies/<guid>.json",   # the skeleton the glb is skinned to (needed with glb)
        "source_model_guid": "...",                   # the game model whose materials and other fields are kept
        "source_model_json": "<survey>/dump/pet_dog_01.json",  # a dumped record with the same material list
        "source_template": "Animal_Wildlife_Dog_04_black_AG2",  # behaviour, stats, animations
        "template_name": "Beast_Wolf",
        "model_guid": "...", "template_guid": "...",  # FIXED forever once shared (saves refer to them)
        "faction": "PlayerAlly",                      # optional: another Faction
        "texture": {"albedo_dir": "tex/wolf", "slot": 0, "flat_size": 128},  # optional: our albedo (albedo_mipNN.png)
                                                      #   and, where the folder has them, normal_mipNN.png and
                                                      #   material_mipNN.png (blender/material_maps.py)
        "template_add": [{"type": "ScaleComponent", "value": {"uniformScale": 1.4}}],  # optional: whole components
        "template_set": {"DamageMod": {"dataStorage[2]": {"mul": 1.5}}},  # optional: fields (a value or {"mul": n})
        "attacks": {                                  # optional: its own copy of the source template's arsenal
          "arsenal_guid": "...",                      #   FIXED forever
          "sequences": {"<attack sequence guid>": {   #   attacks that play a copy of their sequence, every event
            "guid": "...",                            #   moved by [old s, new s] anchors from [0, 0] (after the last:
            "times": [[0, 0], [0.53, 0.94], [2.4, 3.03]]}}  # by its shift): hits that land when our swings do
        },
        "own_proportions": {                          # optional: a skeleton of its own (own_creature.py), with
          "sidecar": "panther_own.json",              #   blender/fit_to_game_skeleton.py --own-proportions's sidecar
          "skeleton_guid": "...",                     #   the skeleton copy's GUID (FIXED forever)
          "reference_clip": "<clips>/sabretooth_<guid>.json",  # a game clip whose first frame stands (the idle)
          "clips": "<clips>/sabretooth_*.json",       #   every game clip the graph plays (EML dumps <owner>_<guid>.json)
          "graph": "<guid>", "graph_guid": "..."      #   the game's animation graph and its copy's GUID
        },
        "new_skeleton": {                             # optional, instead: the glb's own skeleton and animations
          "scale": 0.5,                               #   (new_creature.py; "glb" then needs no jNNN bone names)
          "skeleton_guid": "...",                     #   FIXED forever; the clips' GUIDs follow from it
          "graph": "<guid>", "graph_guid": "...",     #   the game's animation graph and its copy's GUID
          "clips": {"walk": {"animation": "Walk", "loop": true}},   # our clips: the glb's animations
          "roles": {"<game clip guid>": "walk"},      #   our clip wherever the graph plays that game clip
          "default_role": "walk",                     #   our clip for every other game clip of the graph
          "slots": {"Head": "Head", "Pelvis": "Hips"},#   the graph's slots: our joints (others: the root)
          "model_glb": "wolf_palette.glb",            #   optional: the model from a retextured copy (new_creature.py)
          "widen": 1.2                                #   optional: the model this much wider across the body
        }
      }
    ],
    "props": [                                        # optional: still models with no template of their own
      {
        "name": "beastmaster_stable",                 # debugName of the model; also the Lua module names
        "glb": "stable.glb",                          # a glb without a skeleton (gltf_in.load_static_groups)
        "source_model_guid": "...", "source_model_json": "<dump>.json",  # a game model whose record and materials
        "model_guid": "...",                          # FIXED forever once shared   are kept
        "width": 4.0, "yaw": 0, "material": 2,        # fitted to this width (metres), turned, every mesh in one slot
        "texture": {"albedo_dir": "tex/stable", "slot": 2},  # optional, as a creature's, or instead:
        "materials": {"0": "<game material GUID>"}    # each slot wears a game material (blender/game_materials.py)
      }
    ],
    "content_scripts": ["beastmaster_stable.lua"]     # optional: Lua files run after the creatures with EMF's helper
  }
Relative paths are relative to pack.json. Missing GUIDs are generated and written back into pack.json.
A content script (lowercase letters, digits and _) is copied into the mod as content_<name>.lua with
content/lib/emf_content.lua (docs/modding/content.md) next to it; one that fails when the mod is applied is logged and
the creatures stay.
Component names without "::" are keen::ecs:: ones. template_set field paths are names and 0-based [n] indices
(dataStorage[2] is the third value). template_add replaces a component the template has already. A creature with
own_proportions needs "hierarchy" (the game skeleton's dump); its clips and graph copy are made at build time from the
dumps, and a clip or graph a game update removed is skipped (logged) when the mod is applied. A creature with
new_skeleton gets a whole skeleton (made from the source model's skeleton record, every array replaced), its clips and
a graph copy whose every clip reference is ours; a graph with IK chains or foot IK is refused (they name the game
skeleton's joints).
"""
import json
import os
import re
import shutil
import struct
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import encode  # noqa: E402
import gltf_in  # noqa: E402
import new_creature  # noqa: E402
import own_creature  # noqa: E402
import rendermodel as rm  # noqa: E402

NAME = re.compile(r"^[a-z][a-z0-9_]{0,47}$")


def lua_value(v, indent=""):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return repr(v) if v == v else "0.0"
    if isinstance(v, str):
        return '"%s"' % v.replace("\\", "\\\\").replace('"', '\\"')
    if isinstance(v, (list, tuple)):
        if not v:
            return "{}"
        if all(not isinstance(x, (dict, list, tuple)) for x in v):
            return "{" + ", ".join(lua_value(x) for x in v) + "}"
        inner = indent + "  "
        return "{\n" + ",\n".join(inner + lua_value(x, inner) for x in v) + "\n" + indent + "}"
    if isinstance(v, dict):
        return "{" + ", ".join("%s = %s" % (k if LUA_NAME.match(k) else "[%s]" % lua_value(k), lua_value(x, indent))
                               for k, x in v.items()) + "}"
    raise TypeError(type(v))


LUA_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def lua_bytes(data):
    """Bytes as a Lua string literal: printable ASCII as is, everything else as a 3-digit decimal escape."""
    out = []
    for c in data:
        if 32 <= c < 127 and c not in (34, 92):
            out.append(chr(c))
        else:
            out.append("\\%03d" % c)
    return '"' + "".join(out) + '"'


MOD_LUA = r'''-- %(title)s: an EML data patch made by EMF's tools/models/make_mod.py.
-- It adds new render models and creature templates that use them. Spawn one with EMF: spawn <template guid>
-- (the GUIDs are in creatures.lua). The GUIDs are fixed: keep them when you change a model, or saves and other
-- mods lose track of it.
if not loader.features.patch then
	return
end

local function log(...) print("%(id)s:", ...) end
-- a part that could not be added: EML's warn, so the build's reply lists it and EMF tells the player
local function problem(...) (warn or print)("%(id)s:", ...) end

local function fill(list, items)
	while #list > 0 do table.remove(list) end
	for _, item in ipairs(items) do table.insert(list, item) end
end

-- a generated module whose data is copied: let Lua free it (EML's Lua has no global package table: then it stays)
local function unload(name)
	if package and package.loaded then package.loaded[name] = nil end
end

-- textures: a level's bytes in a block format; levels under 4 KB go into the mip tail (as in the game's own files)
local function block_bytes(format)
	if format:find("BC1") or format:find("BC4") then return 8 end
	return 16
end

-- block encoders work on 4x4 blocks: a level under 4x4 (2x2, 1x1) is encoded as a 4x4 image that repeats its pixels,
-- which gives the one block the game stores for such a level
local function at_least_4x4(img)
	if img.width >= 4 and img.height >= 4 then return img end
	local big = image.create(4, 4)
	for y = 0, 3 do
		for x = 0, 3 do
			local r, g, b, a = img:get_pixel(x %% img.width, y %% img.height)
			big:set_pixel(x, y, r, g, b, a)
		end
	end
	return big
end

local function write_texture(tex, levels)
	local format = tostring(tex.format)
	local chain, tail = buffer.create(), buffer.create()
	local starts, sizes, total = {}, {}, 0
	for i, img in ipairs(levels) do
		local enc = image.encode_texture(at_least_4x4(img), tex.format):to_string()
		starts[i], sizes[i] = total, #enc
		total = total + #enc
		chain:write_string(enc)
		if #enc < 4096 then tail:write_string(enc) end
	end
	local ranges = {}
	for i = 1, #levels do
		if sizes[i] >= 4096 then table.insert(ranges, { start = starts[i], count = total - starts[i] }) end
	end
	tex.width = levels[1].width
	tex.height = levels[1].height
	tex.levelCount = #levels
	tex.content.hash = game.assets.create_content(chain)
	local t = game.assets.create_content(tail)
	tex.mipTailContent.hash = t
	tex.mipTailContent.sortKey = t.hash2
	fill(tex.mipChainContentRanges, ranges)
	return format
end

-- the average colour of a game texture: its 1x1 level, the last block of its mip tail
local function average_color(tex)
	local data = game.assets.get_content(game.guid.from_content_hash(tex.mipTailContent.hash)):read_data():to_string()
	local block = data:sub(-block_bytes(tostring(tex.format)))
	local img = image.decode_texture(buffer.wrap(block), tex.format, 4, 4, 0)  -- one block decodes as 4x4
	return img:get_pixel(0, 0)
end

local function flat_levels(size, r, g, b, a)
	local levels, w = {}, size
	while true do
		local img = image.create(w, w)
		for y = 0, w - 1 do
			for x = 0, w - 1 do img:set_pixel(x, y, r, g, b, a) end
		end
		table.insert(levels, img)
		if w == 1 then break end
		w = w // 2
	end
	return levels
end

-- a copy of the model's material in one slot: our albedo and the maps named in texture.maps (PNG levels), the other
-- maps flat at the source's average
local function make_material(c, m)
	local slot = m.materials[c.texture.slot + 1]
	local srcmat = game.assets.get_resource(tostring(slot.material), "keen::RenderMaterialResource")
	assert(srcmat, "the source material was not found")
	local mat = game.assets.create_resource(srcmat.data, "keen::RenderMaterialResource", c.texture.material_guid, 0).data
	local ours = { albedo = true }
	for _, kind in ipairs(c.texture.maps or {}) do ours[kind] = true end
	local done = {}
	for _, im in ipairs(mat.images) do
		local name = tostring(im.texture.debugName)
		local kind = name:find("albedo") and "albedo" or name:match("_([a-z]+)_map")
		if kind and ours[kind] then
			local levels = {}
			for _, png in ipairs(require(c.name .. "_" .. kind)) do table.insert(levels, image.decode(buffer.wrap(png), "png")) end
			done[#done + 1] = kind .. " " .. write_texture(im.texture, levels)
		else
			local r, g, b, a = average_color(im.texture)
			done[#done + 1] = string.format("flat %%s(%%d,%%d,%%d) %%s", kind or "?", r, g, b,
				write_texture(im.texture, flat_levels(c.texture.flat_size, r, g, b, a)))
		end
	end
	slot.material = c.texture.material_guid
	return table.concat(done, "; ")
end

local templates = {}
for _, t in ipairs(game.assets.get_resources_by_type("keen::ecs::TemplateResource")) do
	templates[t.data.name] = t
end
local registry = game.assets.get_resources_by_type("keen::GuidRegistryResource")[1]
local registered = {}
if registry then
	for _, g in ipairs(registry.data.guids) do registered[tostring(g)] = true end
end

-- a dedicated server's data has no render data (no keen::RenderModel at all, no materials): there a creature gets
-- everything but its model and material (its template still names the model, as the game's own templates do there)
local render_data
local function has_render_data()
	if render_data == nil then render_data = #game.assets.get_resources_by_type("keen::RenderModel") > 0 end
	return render_data
end

-- a model (a creature's or a prop's): a copy of the source model's record, then our geometry and our material; nil on a
-- dedicated server (no render data). Returns the model record and our record fields.
local function make_model(c)
	local data, m
	local src = game.assets.get_resource(c.source_model_guid, "keen::RenderModel")
	if not src then
		assert(not has_render_data(), c.name .. ": the source model was not found (the game changed?)")
		log(c.name .. ": no render data here (a dedicated server): the model and its material are left out")
	else
		data = require(c.name .. "_data")
		m = game.assets.create_resource(src.data, "keen::RenderModel", c.model_guid, 0).data
		m.debugName = data.debugName
		m.boundingBox = data.boundingBox
		m.boundingSphere = data.boundingSphere
		m.positionScale = data.positionScale
		m.positionOffset = data.positionOffset
		if data.wholeModelRadius > m.wholeModelRadius then m.wholeModelRadius = data.wholeModelRadius end
		fill(m.meshlets, {})
		fill(m.meshes, data.meshes)
		fill(m.lods, data.lods)
		fill(m.skinningMap, data.skinningMap)
		fill(m.blendShapes, {})
		m.blendShapeMask = 0
		-- the source's cloth simulation (the mage's robe, the knight's cape) runs over the source's own vertices: never on
		-- our geometry (seen in game: the lich and the bone golem glitched as they walked)
		if m.cloth ~= nil then
			local ok, err = pcall(function() m.cloth = nil end)
			if not ok then log(c.name .. ": the source's cloth could not be removed: " .. tostring(err)) end
		end
		m.jointCount = data.jointCount
		m.indexData = data.indexData
		m.meshletData = data.meshletData
		m.vertexData = data.vertexData
		m.skinningData = data.skinningData
		m.blendShapeData = data.blendShapeData
		m.renderData.hash = game.assets.create_content(buffer.wrap(require(c.name .. "_blob")))
		-- a prop on the game's own materials: each slot names one (the game's art stays the game's)
		for _, s in ipairs(c.materials or {}) do
			m.materials[s.slot + 1].material = s.material
			log(string.format("%%s: slot %%d wears the game material %%s", c.name, s.slot, s.material))
		end
		if c.texture then
			local ok, result = pcall(make_material, c, m)
			log(c.name .. (ok and (": material " .. c.texture.material_guid .. ": " .. result)
				or (": texture failed, keeping the source material: " .. tostring(result))))
		end
	end
	return m, data
end

for _, c in ipairs(require("creatures")) do
	-- one creature that cannot be added (a game update) does not stop the others
	local ok, err = pcall(function()

	-- 1) the model
	local m, data = make_model(c)

	-- 1b) a skeleton of its own (new_skeleton): the whole skeleton, its own clips, and a copy of a game creature's
	-- animation graph whose every clip reference plays ours (a game clip carries the game skeleton's hash)
	if c.skeleton and c.skeleton.new then
		local GRAPH = "keen::anim_graph::runtime_graph::AnimationGraphResource2_0"
		local HIER = "keen::ModelHierarchyResource"
		local CLIP = "keen::JointAnimation"
		local NONE = "00000000-0000-0000-0000-000000000000"
		-- the graph first: without it nothing of this creature is changed (the creature is not added)
		local src_g = game.assets.get_resource(c.animation.graph, GRAPH)
		assert(src_g, "the game graph " .. c.animation.graph .. " was not found (the game changed?)")
		local gd = src_g.data
		assert(#gd.ikChains == 0 and gd.bipedFeetIkNodeId.value == 0 and gd.genericFeetIkNodeId.value == 0
			and tostring(gd.feetIKSettingsReference) == NONE,
			"the game graph " .. c.animation.graph .. " uses IK on its own skeleton's joints: it cannot play on ours")
		for _, node in ipairs(gd.nodeDefinitions) do
			if tostring(node.type):find("LayerBlend", 1, true) then
				assert(tostring(node.value.blendOptions.boneMask) == NONE, "the game graph " .. c.animation.graph ..
					" has a bone mask (it names its own skeleton's joints): it cannot play on ours")
			end
		end
		local sk = require(c.name .. "_skeleton")
		-- a game skeleton record holds ours, every array replaced: the source model's (without render data, the game
		-- graph's)
		local shape_h = tostring(m and m.hierarchy or gd.hierarchy)
		local src_h = game.assets.get_resource(shape_h, HIER)
		assert(src_h, c.name .. ": the game skeleton " .. shape_h .. " was not found")
		local h = game.assets.create_resource(src_h.data, HIER, sk.guid, 0).data
		h.hierarchyId = sk.guid
		fill(h.jointNames, sk.jointNames)
		fill(h.jointParentIndices, sk.jointParentIndices)
		fill(h.unbendableJoints, {})
		fill(h.skinToJointMatrices, sk.skinToJointMatrices)
		fill(h.defaultPoseJoints, sk.defaultPoseJoints)
		local units = {}
		for i = 1, #sk.pose, 16 do units[#units + 1] = { data = { sk.pose:byte(i, i + 15) } } end
		fill(h.defaultPose.data, units)
		h.defaultPose.alignedJointCount = sk.alignedJointCount
		h.jointCount = sk.jointCount
		if m then
			m.hierarchy = sk.guid
			m.hierarchyJointCount = sk.jointCount
			-- per-joint data the source model keeps for its own skeleton (skinned bounds, colliders): not for ours
			for _, part in ipairs({ { m.skinnedTree, { "jointMatrix", "jointParent", "jointSpringData", "leafJoints",
				"inverseTransformTree" } }, { m.collider, { "spheres", "capsules", "instances" } }, { m.sdf, { "parts" } } }) do
				local obj = part[1]
				if obj ~= nil then
					for _, f in ipairs(part[2]) do
						if obj[f] ~= nil and #obj[f] > 0 then
							log(string.format("%%s: the source model's %%s (%%d) name its skeleton's joints: removed", c.name,
								f, #obj[f]))
							fill(obj[f], {})
						end
					end
				end
			end
		end
		-- our clips, each in a copy of a game clip's record with every field replaced
		local shape
		for old in pairs(c.animation.roles) do shape = shape or game.assets.get_resource(old, CLIP) end
		assert(shape, c.name .. ": none of the game clips the pack names was found (the game changed?)")
		local seconds, made = {}, 0
		for _, cl in ipairs(require(c.name .. "_clips")) do
			local anim = game.assets.create_resource(shape.data, CLIP, cl.guid, 0).data
			anim.hierarchyHash, anim.jointCount = cl.hierarchyHash, cl.jointCount
			anim.positionFactor, anim.positionTangent, anim.scaleFactor = cl.positionFactor, cl.positionTangent,
				cl.scaleFactor
			anim.fps, anim.frameCount, anim.flags = cl.fps, cl.frameCount, cl.flags
			fill(anim.headers, cl.headers)
			local bytes = {}
			for i = 1, #cl.bytes, 16 do bytes[#bytes + 1] = { data = { cl.bytes:byte(i, i + 15) } } end
			fill(anim.data, bytes)
			seconds[cl.guid] = cl.frameCount / cl.fps
			made = made + 1
		end
		unload(c.name .. "_clips")
		-- the graph copy: every clip reference plays our clip for its role, with our clip's length; the slots point
		-- at our joints. rootMotionAnimation and the animation hashes stay the game's (how far a clip moves the
		-- creature; the hashes key the game clip's events)
		local g = game.assets.create_resource(gd, GRAPH, c.animation.graph_guid, 0).data
		g.hierarchy = sk.guid
		-- a node whose clips are offsets (animationContentAdditive) adds them to the pose: it plays an offset of ours
		-- (a whole pose there would double every joint's scale)
		local ours, offset_swaps, defaulted, seen = 0, 0, {}, {}
		local function swap(obj, field, length, offsets)
			local old = tostring(obj[field])
			if old == "nil" or old == NONE then return end
			local new = c.animation.roles[old]
			if not new then
				new = c.animation.default
				if not seen[old] then
					seen[old] = true
					defaulted[#defaulted + 1] = old
				end
			end
			if offsets and not c.animation.offsets[new] then
				new = c.animation.zero
				offset_swaps = offset_swaps + 1
			end
			obj[field] = new
			ours = ours + 1
			if length then obj[length] = seconds[new] end
		end
		for _, node in ipairs(g.nodeDefinitions) do
			local t = tostring(node.type)
			if t:find("SampleAnimationClip", 1, true) or t:find("BlendSpace", 1, true) then
				local offsets = node.value.animationContentAdditive == true
				swap(node.value, "additiveReferenceAnimation", nil, offsets)
				swap(node.value, "additiveReferenceAnimationGenderAlternative", nil, offsets)
				if t:find("SampleAnimationClip", 1, true) then
					swap(node.value, "animation", "animationDuration", offsets)
					swap(node.value, "animationGenderAlternative", nil, offsets)
				else
					for _, it in ipairs(node.value.items) do
						swap(it, "animation", "animationDuration", offsets)
						swap(it, "animationGenderAlternative", nil, offsets)
					end
				end
			end
		end
		for i, j in ipairs(sk.slots) do g.slotBoneIndexMapping[i] = j end
		unload(c.name .. "_skeleton")
		log(string.format("%%s: new skeleton %%s (%%d joints), %%d clips, graph %%s: %%d clip references ours, %%d by default (%%s), %%d in offset nodes: the zero offset",
			c.name, sk.guid, sk.jointCount, made, c.animation.graph_guid, ours, #defaulted, table.concat(defaulted, " "),
			offset_swaps))
	elseif c.skeleton then
		-- 1c) own_proportions: a copy of the game skeleton fitted to the creature, the game creature's clips
		-- retargeted to it, and a copy of its animation graph that plays them
		-- the graph first: without it nothing of this creature is changed (the creature is not added)
		local GRAPH = "keen::anim_graph::runtime_graph::AnimationGraphResource2_0"
		local src_g = game.assets.get_resource(c.animation.graph, GRAPH)
		assert(src_g, "the game graph " .. c.animation.graph .. " was not found (the game changed?)")
		local sk = require(c.name .. "_skeleton")
		local src_h = game.assets.get_resource(sk.source, "keen::ModelHierarchyResource")
		assert(src_h, c.name .. ": the game skeleton " .. sk.source .. " was not found (the game changed?)")
		local h = game.assets.create_resource(src_h.data, "keen::ModelHierarchyResource", sk.guid, 0).data
		h.hierarchyId = sk.guid
		local n = #sk.joints
		assert(h.jointCount == n, c.name .. ": the game skeleton has " .. tostring(h.jointCount) .. " joints, ours " .. n)
		-- the default pose vectors: rotations, then positions, then scales, each padded to a multiple of 4 joints
		local a = h.defaultPose.alignedJointCount
		for i, j in ipairs(sk.joints) do
			local d = h.defaultPoseJoints[i]
			d.orientation.x, d.orientation.y, d.orientation.z, d.orientation.w = j.q[1], j.q[2], j.q[3], j.q[4]
			d.position.x, d.position.y, d.position.z = j.t[1], j.t[2], j.t[3]
			h.defaultPose.data[i] = { data = j.qb }
			h.defaultPose.data[a + i] = { data = j.tb }
			local b = h.skinToJointMatrices[i]
			for _, col in ipairs({ "x", "y", "z" }) do
				for _, k in ipairs({ "x", "y", "z" }) do b.rot[col][k] = j.ibm.rot[col][k] end
			end
			b.pos.x, b.pos.y, b.pos.z = j.ibm.pos.x, j.ibm.pos.y, j.ibm.pos.z
		end
		if m then m.hierarchy = sk.guid end
		local map, made = {}, 0
		for _, cl in ipairs(require(c.name .. "_clips")) do
			local src_c = game.assets.get_resource(cl.source, "keen::JointAnimation")
			if not src_c then  -- a game update removed it: the graph copy keeps the game's (same skeleton hash)
				log(string.format("%%s: the game clip %%s is gone: it stays the game's", c.name, cl.source))
			else
				local anim = game.assets.create_resource(src_c.data, "keen::JointAnimation", cl.guid, 0).data
				anim.positionFactor, anim.positionTangent, anim.scaleFactor = cl.positionFactor, cl.positionTangent,
					cl.scaleFactor
				anim.fps, anim.frameCount, anim.flags = cl.fps, cl.frameCount, cl.flags
				fill(anim.headers, cl.headers)
				local units = {}
				for i = 1, #cl.bytes, 16 do units[#units + 1] = { data = { cl.bytes:byte(i, i + 15) } } end
				fill(anim.data, units)
				map[cl.source] = cl.guid
				made = made + 1
			end
		end
		unload(c.name .. "_clips")
		local g = game.assets.create_resource(src_g.data, GRAPH, c.animation.graph_guid, 0).data
		g.hierarchy = sk.guid
		local ours, left = 0, 0
		local function swap(obj, field)
			local old = tostring(obj[field])
			if map[old] then
				obj[field] = map[old]
				ours = ours + 1
			elseif old ~= "nil" and old ~= "00000000-0000-0000-0000-000000000000" then
				left = left + 1
			end
		end
		for _, node in ipairs(g.nodeDefinitions) do
			local t = tostring(node.type)
			-- rootMotionAnimation is left alone: it names a keen::RootMotionAnimation (how far a clip moves the
			-- creature), a resource of its own that shares the clip's GUID
			if t:find("SampleAnimationClip", 1, true) then
				for _, f in ipairs({ "animation", "animationGenderAlternative", "additiveReferenceAnimation",
					"additiveReferenceAnimationGenderAlternative" }) do swap(node.value, f) end
			elseif t:find("BlendSpace", 1, true) then
				swap(node.value, "additiveReferenceAnimation")
				swap(node.value, "additiveReferenceAnimationGenderAlternative")
				for _, it in ipairs(node.value.items) do
					swap(it, "animation")
					swap(it, "animationGenderAlternative")
				end
			end
		end
		log(string.format("%%s: skeleton %%s (%%d joints), %%d clips, graph %%s: %%d clip references ours, %%d left as the game's",
			c.name, sk.guid, n, made, c.animation.graph_guid, ours, left))
	end

	-- 2) the creature: a copy of the source template, pointing at the new model
	local source = templates[c.source_template]
	assert(source, c.name .. ": the source template " .. c.source_template .. " was not found (the game changed?)")
	local tpl = game.assets.create_resource(source.data, "keen::ecs::TemplateResource", c.template_guid, 0).data
	tpl.name = c.template_name
	local set = false
	for _, comp in ipairs(tpl.components) do
		if comp.type == "keen::ecs::ModelResource" then
			comp.value.model = c.model_guid
			set = true
		end
	end
	assert(set, c.name .. ": the source template has no ModelResource")
	if c.animation then
		local found = false
		for _, comp in ipairs(tpl.components) do
			if comp.type == "keen::ecs::AnimationGraphResourceComponent2" then
				if tostring(comp.value.animationGraph) ~= c.animation.graph then
					log(string.format("%%s: the source template's graph is %%s, not %%s: it plays the copy anyway", c.name,
						tostring(comp.value.animationGraph), c.animation.graph))
				end
				comp.value.animationGraph = c.animation.graph_guid
				found = true
			end
		end
		assert(found, c.name .. ": the source template has no AnimationGraphResourceComponent2")
	end
	if c.faction then
		for _, comp in ipairs(tpl.components) do
			if comp.type == "keen::ecs::Faction" then comp.value.faction = c.faction end
		end
	end

	-- 3) extra components (a whole component; one the template has already is replaced), then field edits
	for _, add in ipairs(c.template_add or {}) do
		local replaced = false
		for i, comp in ipairs(tpl.components) do
			if comp.type == add.type then
				tpl.components[i] = add
				replaced = true
			end
		end
		if not replaced then table.insert(tpl.components, add) end
	end
	for _, e in ipairs(c.template_set or {}) do
		local done = false
		for _, comp in ipairs(tpl.components) do
			if comp.type == e.component then
				local obj = comp.value
				for i = 1, #e.path - 1 do obj = obj[e.path[i]] end
				local key = e.path[#e.path]
				if e.mul then obj[key] = obj[key] * e.mul else obj[key] = e.value end
				done = true
			end
		end
		if not done then log(c.name .. ": the template has no " .. e.component .. " (the game changed?): not set") end
	end

	-- 3b) attacks: a copy of the template's arsenal (the game's own creatures keep theirs) whose attacks play copies
	-- of their sequences with every event moved by the anchors, so the hits land when this creature's swings do
	if c.attacks then
		local ref
		for _, comp in ipairs(tpl.components) do
			if comp.type == "keen::ecs::EnemyArsenalReference" then ref = comp.value end
		end
		assert(ref, c.name .. ": the source template has no EnemyArsenalReference (no attacks to retime)")
		local areg = game.assets.get_resources_by_type("keen::enemy::EnemyArsenalRegistryResource")[1]
		assert(areg, c.name .. ": the game has no enemy arsenal registry (the game changed?)")
		local from = tostring(ref.arsenal)
		local src
		for _, a in ipairs(areg.data.arsenals) do
			if tostring(a.guid) == from then src = a end
		end
		assert(src, c.name .. ": the arsenal " .. from .. " is not in the registry (the game changed?)")
		local seqs = {}
		for old in pairs(c.attacks.sequences) do
			seqs[old] = game.assets.get_resource(old, "keen::actor::ActorSequenceResource")
			assert(seqs[old], c.name .. ": the sequence " .. old .. " was not found (the game changed?)")
		end
		table.insert(areg.data.arsenals, src)
		local copy = areg.data.arsenals[#areg.data.arsenals]
		copy.guid = c.attacks.arsenal_guid
		assert(tostring(src.guid) == from, c.name .. ": copying the arsenal changed the game's own")
		local retimed = 0
		for _, atk in ipairs(copy.attacks) do
			local s = c.attacks.sequences[tostring(atk.description.actionSequence)]
			if s then
				atk.description.actionSequence = s.guid
				retimed = retimed + 1
			end
		end
		local function moved(times, t)
			local last = times[#times]
			if t >= last[1] then return t + (last[2] - last[1]) end
			for i = 1, #times - 1 do
				local a, b = times[i], times[i + 1]
				if t < b[1] then
					return math.floor(a[2] + ((t - a[1]) * (b[2] - a[2]) + (b[1] - a[1]) // 2) // (b[1] - a[1]))
				end
			end
		end
		local function field(v, k)  -- an event without the field (a proxy may raise instead of giving nil)
			local ok, x = pcall(function() return v[k] end)
			if ok then return x end
		end
		for old, s in pairs(c.attacks.sequences) do
			local d = game.assets.create_resource(seqs[old].data, "keen::actor::ActorSequenceResource", s.guid, 0).data
			d.resourceId = s.guid
			for _, sub in ipairs(d.subSequences) do
				sub.length.value = moved(s.times, sub.length.value)
				for _, ev in ipairs(sub.events) do
					local tm, dur = field(ev.value, "time"), field(ev.value, "duration")
					if tm then
						local t = tm.value
						local nt = moved(s.times, t)
						if dur then dur.value = moved(s.times, t + dur.value) - nt end
						tm.value = nt
					end
				end
			end
		end
		-- the game plays only the sequences its actor sequence collection lists: list each copy where its source is
		local coll = game.assets.get_resources_by_type("keen::ActorSequenceCollectionResource")[1]
		local listed = 0
		if coll then
			local has = {}
			for _, g in ipairs(coll.data.sequences) do has[tostring(g)] = true end
			for old, s in pairs(c.attacks.sequences) do
				if has[old] then
					table.insert(coll.data.sequences, s.guid)
					listed = listed + 1
				end
			end
		end
		log(string.format("%%s: %%d sequence copies listed in the game's sequence collection", c.name, listed))
		ref.arsenal = c.attacks.arsenal_guid
		log(string.format("%%s: arsenal %%s (a copy of %%s): %%d of %%d attacks retimed", c.name,
			c.attacks.arsenal_guid, from, retimed, #copy.attacks))
	end

	-- 4) the GUID registry: if the game lists the source template there, list ours too
	if registry and registered[tostring(source.guid)] then
		table.insert(registry.data.guids, c.template_guid)
	end
	if m then
		log(string.format("added %%s: model %%s (%%d meshes), template %%s %%s", c.name, c.model_guid, #data.meshes,
			c.template_name, c.template_guid))
	else
		log(string.format("added %%s: no model here (no render data), template %%s %%s", c.name, c.template_name,
			c.template_guid))
	end
	end)
	if not ok then problem(c.name .. ": not added: " .. tostring(err)) end
end
'''


SEGMENT = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)((?:\[\d+\])*)$")


def component_type(name):
    """"DamageMod" -> "keen::ecs::DamageMod"; a full type name stays as it is."""
    return name if "::" in name else "keen::ecs::" + name


def field_path(text):
    """"dataStorage[2]" -> ["dataStorage", 3]: dotted field names, 0-based [n] indices (as the game's docs write
    them) turned into Lua's 1-based ones."""
    path = []
    for part in text.split("."):
        m = SEGMENT.match(part)
        if not m:
            raise SystemExit("template_set field '%s': use names and [n] indices, like dataStorage[2] or a.b" % text)
        path.append(m.group(1))
        path.extend(int(i) + 1 for i in re.findall(r"\[(\d+)\]", m.group(2)))
    return path


def listing_entry(c):
    """What the generated mod.lua needs to know about one creature (creatures.lua)."""
    entry = {k: c[k] for k in ("name", "model_guid", "template_guid", "source_model_guid",
                               "source_template", "template_name")}
    if c.get("faction"):
        entry["faction"] = c["faction"]  # e.g. "PlayerAlly": the creature never attacks players
    if c.get("template_add"):
        entry["template_add"] = [{"type": component_type(a["type"]), "value": a["value"]} for a in c["template_add"]]
    if c.get("template_set"):
        entry["template_set"] = [set_entry(component_type(comp), field_path(f), v)
                                 for comp, fields in c["template_set"].items() for f, v in fields.items()]
    if c.get("attacks") is not None:
        entry["attacks"] = attacks_entry(c["name"], c["attacks"])
    if c.get("own_proportions"):
        op = c["own_proportions"]
        missing = [k for k in OWN_KEYS if not op.get(k)]
        if missing:
            raise SystemExit("%s: own_proportions needs %s" % (c["name"], ", ".join(missing)))
        if not c.get("hierarchy"):
            raise SystemExit("%s: own_proportions needs \"hierarchy\" (the game skeleton's dump)" % c["name"])
        entry["skeleton"] = {"guid": op["skeleton_guid"]}
        entry["animation"] = {"graph": op["graph"], "graph_guid": op["graph_guid"]}
    if c.get("new_skeleton"):
        if c.get("own_proportions"):
            raise SystemExit("%s: own_proportions or new_skeleton, not both" % c["name"])
        cfg = c["new_skeleton"]
        new_creature.check(cfg, c["name"])
        roles, default = new_creature.roles(cfg)
        entry["skeleton"] = {"guid": cfg["skeleton_guid"], "new": True}
        entry["animation"] = {"graph": cfg["graph"], "graph_guid": cfg["graph_guid"], "roles": roles,
                              "default": default, "zero": new_creature.clip_guid(cfg["skeleton_guid"], new_creature.ZERO),
                              "offsets": new_creature.offsets(cfg)}
    return entry


OWN_KEYS = ("sidecar", "skeleton_guid", "reference_clip", "clips", "graph", "graph_guid")


def attacks_entry(name, a):
    """A creature's "attacks" for the mod: the arsenal copy's GUID, and per source sequence (GUID, lower case) its
    copy's GUID and the time anchors in nanoseconds (the game's unit), checked: from [0, 0], both columns forward."""
    if not a.get("arsenal_guid"):
        raise SystemExit("%s: attacks needs \"arsenal_guid\" (the arsenal copy's GUID, fixed forever)" % name)
    if not a.get("sequences"):
        raise SystemExit("%s: attacks needs \"sequences\" ({source sequence GUID: {guid, times}})" % name)
    out = {}
    for old, s in a["sequences"].items():
        key = old.lower()
        if not new_creature.GUID.match(key):
            raise SystemExit("%s: attacks: %r is not a sequence GUID" % (name, old))
        if not s.get("guid"):
            raise SystemExit("%s: attacks: the copy of %s needs a \"guid\" (fixed forever)" % (name, key))
        times = [[int(round(float(o) * 1e9)), int(round(float(n) * 1e9))] for o, n in s.get("times", [])]
        if len(times) < 2 or times[0] != [0, 0]:
            raise SystemExit("%s: attacks: the times of %s start at [0, 0] and have two anchors or more" % (name, key))
        if any(o1 <= o0 or n1 <= n0 for (o0, n0), (o1, n1) in zip(times, times[1:])):
            raise SystemExit("%s: attacks: the times of %s go forward in both columns" % (name, key))
        out[key] = {"guid": s["guid"], "times": times}
    return {"arsenal_guid": a["arsenal_guid"], "sequences": out}


def check_texture_slot(name, slot, groups):
    """SystemExit when the texture replaces a material slot none of the model's meshes draw with: the model would
    wear the source model's own texture of its slot through our UVs (game_material_N names a mesh's slot)."""
    drawn = sorted({g["material"] for g in groups})
    if slot not in drawn:
        raise SystemExit("%s: the texture is for slot %d, but the model draws only with slot %s (name its materials "
                         "game_material_%d or set the texture's slot)" % (name, slot, ", ".join(map(str, drawn)), slot))


def write_own(src, name, built):
    """<name>_skeleton.lua and <name>_clips.lua from own_creature.build: the skeleton copy's rest pose and inverse
    binds, and the retargeted clips."""
    joints = []
    for (q, t, _), ibm in zip(built["skeleton"]["rest"], built["skeleton"]["skinToJointMatrices"]):
        joints.append({"q": list(q), "t": list(t), "qb": list(struct.pack("<4f", *q)),
                       "tb": list(struct.pack("<4f", t[0], t[1], t[2], 0.0)), "ibm": ibm})
    with open(os.path.join(src, name + "_skeleton.lua"), "w", encoding="utf-8", newline="\n") as f:
        f.write("-- generated by tools/models/make_mod.py: the skeleton of %s (a copy of %s fitted to it)\nreturn %s\n"
                % (name, built["skeleton"]["source"], lua_value({"source": built["skeleton"]["source"],
                                                                 "guid": built["skeleton"]["guid"], "joints": joints})))
    parts = []
    for cl in built["clips"]:
        k = cl["clip"]
        raw = bytes(x for u in k["data"] for x in u["data"])
        head = {"source": cl["source"], "guid": cl["guid"], "positionFactor": k["positionFactor"],
                "positionTangent": k["positionTangent"], "scaleFactor": k["scaleFactor"], "fps": k["fps"],
                "frameCount": k["frameCount"], "flags": k["flags"], "headers": k["headers"]}
        parts.append(lua_value(head)[:-1] + ", bytes = " + lua_bytes(raw) + "}")
    with open(os.path.join(src, name + "_clips.lua"), "w", encoding="ascii", newline="\n") as f:
        f.write("-- generated by tools/models/make_mod.py: the clips of %s, retargeted from the game creature's\n"
                "return {\n%s\n}\n" % (name, ",\n".join(parts)))


def signed64(v):
    """A u64 as Lua holds it (a signed 64-bit integer; EML shows u64 fields that way)."""
    v &= 0xFFFFFFFFFFFFFFFF
    return v - (1 << 64) if v >= 1 << 63 else v


def write_new(src, name, built):
    """<name>_skeleton.lua and <name>_clips.lua from new_creature.build: the whole skeleton (names, parents, inverse
    binds, rest pose, the graph's slot map) and the clips."""
    h = built["hierarchy"]
    pose = bytes(x for u in h["defaultPose"]["data"] for x in u["data"])
    skel = {"guid": h["hierarchyId"], "new": True, "jointCount": h["jointCount"],
            "alignedJointCount": h["defaultPose"]["alignedJointCount"], "jointNames": h["jointNames"],
            "jointParentIndices": h["jointParentIndices"], "skinToJointMatrices": h["skinToJointMatrices"],
            "defaultPoseJoints": h["defaultPoseJoints"], "slots": built["slots"],
            "names": built["sk"]["names"]}
    with open(os.path.join(src, name + "_skeleton.lua"), "w", encoding="utf-8", newline="\n") as f:
        f.write("-- generated by tools/models/make_mod.py: the skeleton of %s (its own)\nreturn %s, pose = %s}\n"
                % (name, lua_value(skel)[:-1], lua_bytes(pose)))
    parts = []
    for cl in built["clips"] + [built["zero"]]:
        k = cl["clip"]
        raw = bytes(x for u in k["data"] for x in u["data"])
        head = {"name": cl["name"], "guid": cl["guid"], "hierarchyHash": signed64(k["hierarchyHash"]),
                "jointCount": k["jointCount"], "positionFactor": k["positionFactor"],
                "positionTangent": k["positionTangent"], "scaleFactor": k["scaleFactor"], "fps": k["fps"],
                "frameCount": k["frameCount"], "flags": k["flags"], "headers": k["headers"]}
        parts.append(lua_value(head)[:-1] + ", bytes = " + lua_bytes(raw) + "}")
    with open(os.path.join(src, name + "_clips.lua"), "w", encoding="ascii", newline="\n") as f:
        f.write("-- generated by tools/models/make_mod.py: the clips of %s, made from its own animations\n"
                "return {\n%s\n}\n" % (name, ",\n".join(parts)))


def set_entry(component, path, v):
    """One template_set edit: a value, or {"mul": n} to multiply the template's own value (it follows the game when
    the game changes the source creature)."""
    if isinstance(v, dict):
        if set(v) != {"mul"} or isinstance(v["mul"], bool) or not isinstance(v["mul"], (int, float)):
            raise SystemExit("template_set %s: use a value or {\"mul\": number}, not %r" % (".".join(map(str, path)), v))
        return {"component": component, "path": path, "mul": v["mul"]}
    if not isinstance(v, (int, float, str, bool)):
        raise SystemExit("template_set %s: use a value or {\"mul\": number}, not %r" % (".".join(map(str, path)), v))
    return {"component": component, "path": path, "value": v}


CONTENT_LUA = r'''
-- the pack's content scripts (make_mod's "content_scripts"), after the creatures: one that fails is logged, and the
-- creatures and the other scripts stay
for _, name in ipairs(%(modules)s) do
	local ok, err = pcall(require, name)
	if not ok then problem(name .. ": not added: " .. tostring(err)) end
end
'''


PROPS_LUA = r'''
-- the pack's props (make_mod's "props"): a model of its own each, no template (a content script's object uses it)
for _, p in ipairs(require("props")) do
	local ok, err = pcall(function()
		local m, data = make_model(p)
		if m then log(string.format("added prop %s: model %s (%d meshes)", p.name, p.model_guid, #data.meshes)) end
	end)
	if not ok then problem(p.name .. ": not added: " .. tostring(err)) end
end
'''

PROP_KEYS = ("name", "glb", "source_model_guid", "source_model_json", "model_guid")


def prop_entry(p):
    """What the generated mod.lua needs to know about one prop (props.lua); refuses an entry without its keys."""
    for k in PROP_KEYS:
        if not p.get(k):
            raise SystemExit("prop %s: \"%s\" is missing" % (p.get("name", "?"), k))
    if not NAME.match(p["name"]):
        raise SystemExit("prop name '%s': use lowercase letters, digits and _" % p["name"])
    entry = {"name": p["name"], "source_model_guid": p["source_model_guid"], "model_guid": p["model_guid"]}
    if p.get("materials"):
        if p.get("texture"):
            raise SystemExit("prop %s: \"materials\" (the game's own) or \"texture\" (ours), not both" % p["name"])
        slots = []
        for k, guid in p["materials"].items():
            if not str(k).isdigit():
                raise SystemExit("prop %s: material slots are numbers, not '%s'" % (p["name"], k))
            slots.append({"slot": int(k), "material": guid})
        entry["materials"] = sorted(slots, key=lambda s: s["slot"])
    return entry


def write_scripts(src, pack, listing, content_modules=(), props=()):
    """creatures.lua (and props.lua) and mod.lua into the mod's src folder; mod.lua makes the props' models after the
    creatures, then runs `content_modules` (copy_content's names)."""
    with open(os.path.join(src, "creatures.lua"), "w", encoding="utf-8") as f:
        f.write("-- generated by tools/models/make_mod.py: the creatures this mod adds\nreturn " + lua_value(listing) + "\n")
    props_file = os.path.join(src, "props.lua")
    if props:
        with open(props_file, "w", encoding="utf-8") as f:
            f.write("-- generated by tools/models/make_mod.py: the props' models this mod adds\nreturn "
                    + lua_value(list(props)) + "\n")
    elif os.path.exists(props_file):
        os.remove(props_file)
    with open(os.path.join(src, "mod.lua"), "w", encoding="utf-8") as f:
        f.write(MOD_LUA % {"title": pack.get("title", pack["id"]), "id": pack["id"]})
        if props:
            f.write(PROPS_LUA)
        if content_modules:
            f.write(CONTENT_LUA % {"modules": lua_value(list(content_modules))})


CONTENT_LIB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "content", "lib", "emf_content.lua")
SCRIPT_NAME = re.compile(r"^[a-z0-9_]+\.lua$")


def copy_content(src, scripts, base):
    """The pack's "content_scripts" (Lua files that use EMF's content helper, paths relative to the pack's folder
    `base`) into the mod's src folder as content_<name>.lua, with content/lib/emf_content.lua next to them; the
    content_*.lua of scripts the pack no longer lists go, and the helper too when none is left. Returns the module
    names in the pack's order."""
    names = []
    for s in scripts:
        if not SCRIPT_NAME.match(os.path.basename(s)):
            raise SystemExit("content script '%s': name it with lowercase letters, digits and _, ending in .lua" % s)
        p = s if os.path.isabs(s) else os.path.join(base, s)
        if not os.path.isfile(p):
            raise SystemExit("content script '%s' not found (paths are relative to the pack)" % s)
        names.append(("content_" + os.path.basename(s)[:-4], p))
    keep = {n + ".lua" for n, _ in names}
    for f in os.listdir(src):
        if (f.startswith("content_") and f.endswith(".lua") and f not in keep) or (f == "emf_content.lua" and not names):
            os.remove(os.path.join(src, f))
    for n, p in names:
        shutil.copyfile(p, os.path.join(src, n + ".lua"))
    if names:
        shutil.copyfile(CONTENT_LIB, os.path.join(src, "emf_content.lua"))
    return [n for n, _ in names]


GENERATED = ("_data.lua", "_blob.lua", "_albedo.lua", "_normal.lua", "_material.lua", "_skeleton.lua", "_clips.lua")


OWN_GENERATED = ("_skeleton.lua", "_clips.lua")


def remove_stale(src, names, own=None):
    """Removes the generated files of creatures not in `names` from the mod's src folder (a creature taken out of the
    pack would otherwise ship its model unused), and the skeleton and clip files of creatures not in `own` (those
    without own_proportions now; None: every creature keeps them)."""
    for f in os.listdir(src):
        for suffix in GENERATED:
            if not f.endswith(suffix):
                continue
            name = f[: -len(suffix)]
            if name not in names or (own is not None and suffix in OWN_GENERATED and name not in own):
                os.remove(os.path.join(src, f))


OPTIONAL_MAPS = ("normal", "material")


def write_maps(src, name, folder):
    """<name>_albedo.lua from the albedo_mipNN.png levels in `folder` (largest first), and <name>_normal.lua and
    <name>_material.lua from normal_mipNN.png and material_mipNN.png where the folder has them (a map it does not
    have loses its file). Returns the optional maps written."""
    written = []
    for kind in ("albedo",) + OPTIONAL_MAPS:
        pngs = sorted(f for f in os.listdir(folder) if re.match(r"^%s_mip\d+\.png$" % kind, f))
        target = os.path.join(src, "%s_%s.lua" % (name, kind))
        if not pngs:
            if kind == "albedo":
                raise SystemExit("%s: no albedo_mipNN.png in %s" % (name, folder))
            if os.path.exists(target):
                os.remove(target)
            continue
        parts = []
        for f in pngs:
            with open(os.path.join(folder, f), "rb") as fh:
                parts.append(lua_bytes(fh.read()))
        with open(target, "w", encoding="ascii", newline="\n") as f:
            f.write("-- generated by tools/models/make_mod.py: %s of %s, %d PNG levels (largest first)\n"
                    "return {\n%s\n}\n" % (kind, name, len(parts), ",\n".join(parts)))
        if kind != "albedo":
            written.append(kind)
    return written


def remove_maps(src, name):
    """Removes the texture files of an earlier build of ours."""
    for kind in ("albedo",) + OPTIONAL_MAPS:
        target = os.path.join(src, "%s_%s.lua" % (name, kind))
        if os.path.exists(target):
            os.remove(target)


def build_prop(p, src, path):
    """A prop's model files (<name>_data.lua, _blob.lua, _albedo.lua) from its still glb, fitted by load_static_groups
    ("width" metres, "yaw" degrees, all its meshes in material slot "material"); returns (props.lua entry, whether a
    GUID was added to `p`)."""
    entry = prop_entry(p)
    source = rm.load_record(path(p["source_model_json"]))
    groups, report = gltf_in.load_static_groups(path(p["glb"]), width=p.get("width"), yaw=float(p.get("yaw", 0)),
                                                material=p.get("material"), material_count=len(source["materials"]))
    groups = encode.split_groups(groups)  # a dense prop (the second Stable's thatch) is more than one mesh
    blob, fields = encode.build(groups)
    ctr = fields["boundingSphere"]["center"]
    radius = max(((q[0] - ctr["x"]) ** 2 + (q[1] - ctr["y"]) ** 2 + (q[2] - ctr["z"]) ** 2) ** 0.5
                 for g in groups for q in g["pos"])
    data = dict(fields, debugName=p["name"], wholeModelRadius=radius)
    with open(os.path.join(src, p["name"] + "_data.lua"), "w", encoding="utf-8") as f:
        f.write("-- generated by tools/models/make_mod.py: record fields of the prop %s\nreturn %s\n"
                % (p["name"], lua_value(data)))
    with open(os.path.join(src, p["name"] + "_blob.lua"), "w", encoding="ascii", newline="\n") as f:
        f.write("-- generated by tools/models/make_mod.py: render data of the prop %s (vertices, indices), %d bytes\n"
                "return %s\n" % (p["name"], len(blob), lua_bytes(blob)))
    changed = False
    if p.get("texture"):
        t = p["texture"]
        check_texture_slot(p["name"], int(t.get("slot", 1)), groups)
        if not t.get("material_guid"):
            t["material_guid"] = str(uuid.uuid4())
            changed = True
        maps = write_maps(src, p["name"], path(t["albedo_dir"]))
        entry["texture"] = {"slot": int(t.get("slot", 1)), "material_guid": t["material_guid"],
                            "flat_size": int(t.get("flat_size", 128)), "maps": maps}
    else:
        remove_maps(src, p["name"])
    print("%s: prop, %d meshes, %d bytes, %.2f x %.2f x %.2f m, model %s" % (
        p["name"], len(fields["meshes"]), len(blob), report["size"][0], report["size"][1], report["size"][2],
        p["model_guid"]))
    return entry, changed


USAGE = "usage: make_mod.py <pack.json> [--leave-out <creature name>]..."


def parse_args(argv):
    """(the pack's path, the creature names to leave out) from make_mod's command line."""
    if not argv or argv[0].startswith("--"):
        raise SystemExit(USAGE)
    leave, rest = [], argv[1:]
    while rest:
        if rest[0] != "--leave-out" or len(rest) < 2:
            raise SystemExit(USAGE)
        leave.append(rest[1])
        rest = rest[2:]
    return argv[0], leave


def leave_out(pack, names):
    """The pack without the creatures named (a build without a creature whose licence is not settled): a new top-level
    dict over the same creature objects, whose description is the pack's and each kept creature's own "description"
    (a creature's sentence and credit)."""
    have = [c["name"] for c in pack["creatures"]]
    unknown = [n for n in names if n not in have]
    if unknown:
        raise SystemExit("--leave-out %s: the pack has no such creature (it has %s)" % (", ".join(unknown),
                                                                                       ", ".join(have)))
    kept = [c for c in pack["creatures"] if c["name"] not in names]
    parts = [pack.get("description", "New creatures made with EMF tools/models.")]
    parts += [c["description"] for c in kept if c.get("description")]
    return dict(pack, creatures=kept, description=" ".join(parts))


def main():
    pack_arg, leave = parse_args(sys.argv[1:])
    pack_path = os.path.abspath(pack_arg)
    base = os.path.dirname(pack_path)
    with open(pack_path, encoding="utf-8") as f:
        full = json.load(f)
    pack = leave_out(full, leave)
    if leave:
        print("left out:", ", ".join(leave))

    def path(p):
        return p if os.path.isabs(p) else os.path.join(base, p)

    out = path(pack["out"])
    os.makedirs(os.path.join(out, "src"), exist_ok=True)
    remove_stale(os.path.join(out, "src"),
                 {c["name"] for c in pack["creatures"]} | {p.get("name") for p in pack.get("props", [])},
                 own={c["name"] for c in pack["creatures"] if c.get("own_proportions") or c.get("new_skeleton")})
    content_modules = copy_content(os.path.join(out, "src"), pack.get("content_scripts", []), base)
    changed = False
    listing = []
    for c in pack["creatures"]:
        if not NAME.match(c["name"]):
            raise SystemExit("creature name '%s': use lowercase letters, digits and _" % c["name"])
        for key in ("model_guid", "template_guid"):
            if not c.get(key):
                c[key] = str(uuid.uuid4())
                changed = True
        source = rm.load_record(path(c["source_model_json"]))
        built_new = None
        if c.get("new_skeleton"):
            for key in ("skeleton_guid", "graph_guid"):
                if not c["new_skeleton"].get(key):
                    c["new_skeleton"][key] = str(uuid.uuid4())
                    changed = True
        if c.get("game_dump"):
            model = rm.load_record(path(c["game_dump"]) + ".json")
            with open(path(c["game_dump"]) + ".render.bin", "rb") as f:
                groups = encode.groups_from_model(model, f.read())
            palette = model["skinningMap"]
        elif c.get("new_skeleton"):
            built_new = new_creature.build(c["new_skeleton"], path(c["glb"]), c["name"])
            model_glb = c["new_skeleton"].get("model_glb")
            widen_by = float(c["new_skeleton"].get("widen", 1.0))
            groups, report = new_creature.model_groups(path(c["glb"]), built_new,
                                                       material_count=len(source["materials"]),
                                                       model_glb=path(model_glb) if model_glb else None,
                                                       widen_by=widen_by)
            print("%s: model on the new skeleton, rest difference %.2g, %.2f times as wide"
                  % (c["name"], report["max_rest_difference"], widen_by))
            palette = None
        else:
            groups, _ = gltf_in.load_groups(path(c["glb"]), rm.load_record(path(c["hierarchy"])),
                                            material_count=len(source["materials"]))
            palette = None
        blob, fields = encode.build(groups, palette=palette)
        ctr = fields["boundingSphere"]["center"]
        radius = max(((p[0] - ctr["x"]) ** 2 + (p[1] - ctr["y"]) ** 2 + (p[2] - ctr["z"]) ** 2) ** 0.5
                     for g in groups for p in g["pos"])
        data = dict(fields, debugName=c["name"], wholeModelRadius=radius)
        with open(os.path.join(out, "src", c["name"] + "_data.lua"), "w", encoding="utf-8") as f:
            f.write("-- generated by tools/models/make_mod.py: record fields of the model %s\nreturn %s\n"
                    % (c["name"], lua_value(data)))
        with open(os.path.join(out, "src", c["name"] + "_blob.lua"), "w", encoding="ascii", newline="\n") as f:
            f.write("-- generated by tools/models/make_mod.py: render data of %s (vertices, skin, indices), %d bytes\n"
                    "return %s\n" % (c["name"], len(blob), lua_bytes(blob)))
        entry = listing_entry(c)
        if c.get("own_proportions"):
            op = dict(c["own_proportions"])
            for k in ("sidecar", "reference_clip", "clips"):
                op[k] = path(op[k])
            built = own_creature.build(op, rm.load_record(path(c["hierarchy"])))
            write_own(os.path.join(out, "src"), c["name"], built)
            print("%s: own skeleton %s, %d clips retargeted" % (c["name"], op["skeleton_guid"], len(built["clips"])))
        if built_new:
            write_new(os.path.join(out, "src"), c["name"], built_new)
            print("%s: new skeleton %s (%d joints), %d clips" % (c["name"], c["new_skeleton"]["skeleton_guid"],
                                                                built_new["hierarchy"]["jointCount"],
                                                                len(built_new["clips"])))
        if c.get("texture"):
            t = c["texture"]
            check_texture_slot(c["name"], int(t.get("slot", 1)), groups)
            if not t.get("material_guid"):
                t["material_guid"] = str(uuid.uuid4())
                changed = True
            maps = write_maps(os.path.join(out, "src"), c["name"], path(t["albedo_dir"]))
            entry["texture"] = {"slot": int(t.get("slot", 1)), "material_guid": t["material_guid"],
                                "flat_size": int(t.get("flat_size", 128)), "maps": maps}
        else:
            remove_maps(os.path.join(out, "src"), c["name"])
        listing.append(entry)
        print("%s: %d meshes, %d bytes, template %s %s" % (c["name"], len(fields["meshes"]), len(blob),
                                                            c["template_name"], c["template_guid"]))
    props = []
    for p in pack.get("props", []):
        if not p.get("model_guid"):
            p["model_guid"] = str(uuid.uuid4())
            changed = True
        entry, added = build_prop(p, os.path.join(out, "src"), path)
        changed = changed or added
        props.append(entry)
    write_scripts(os.path.join(out, "src"), pack, listing, content_modules, props)
    with open(os.path.join(out, "mod.json"), "w", encoding="utf-8") as f:
        json.dump({"id": pack["id"], "name": pack.get("title", pack["id"]), "version": pack.get("version", "0.1.0"),
                   "capabilities": ["patch"],
                   "description": pack["description"],
                   "authors": pack.get("authors", ["EMF contributors"])}, f, indent=2)
        f.write("\n")
    if changed:
        with open(pack_path, "w", encoding="utf-8") as f:
            json.dump(full, f, indent=2)
            f.write("\n")
        print("new GUIDs were written into", pack_path, "- keep them")
    print("mod written to", out)


if __name__ == "__main__":
    main()
