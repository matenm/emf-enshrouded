"""Blender (headless): fit a four-legged creature mesh onto a game skeleton exported by gamegltf.py.

  blender --background --factory-startup --python fit_to_game_skeleton.py -- \
      <game.glb> <creature.glb> <out.glb> [--uv-from 1] [--material 1] [--size 1.0] [--vertex-colors]
      [--width fit|keep|<scale>]

Steps:
 1. Import the game model (armature with jNNN bones + its body meshes) and the creature (any rig; it is dropped).
 2. Fit the creature onto the game body with a piecewise-linear warp along the body (nose, front feet, back feet)
    and scales for height (top of the back between the legs) and width, so its legs sit on the game's leg bones.
    When the game's rest pose holds one pair of feet lower than the other (the sabretooth's front paws), the
    creature's legs stretch below the belly to reach them (../quad_landmarks.py). The width is fitted to the game
    body's (--width fit, the default); --width keep keeps the creature's own proportions (the mean of its length and
    height scales), for a creature stretched much more in length and height than in width.
 3. Copy bone weights from all game meshes (nearest surface) and UVs from one game mesh (--uv-from), so the creature
    can use that mesh's material and textures (--material, the game material index).
 4. --vertex-colors stores the creature's own material colours as vertex colours (COLOR_0).
 5. Export a skinned .glb that tools/models/make_mod.py reads.
Blender axes after the glTF import: forward is -Y (the head), up is +Z, width is X.
"""
import os
import sys

import bpy
from mathutils import Vector

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from quad_landmarks import fit_z, landmarks, width_scale  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:]
game_glb, creature_glb, out_glb = (os.path.abspath(p) for p in argv[:3])
opts = argv[3:]


def opt(name, default=None, cast=str):
    if name in opts:
        i = opts.index(name)
        return cast(opts[i + 1]) if i + 1 < len(opts) and not opts[i + 1].startswith("--") else True
    return default


uv_from = opt("--uv-from", "auto")  # a game mesh index, or "auto": the biggest mesh that reaches the ground
material_index = opt("--material", None, int)  # default: the material of that game mesh
size = opt("--size", 1.0, float)
vertex_colors = bool(opt("--vertex-colors", False))
keep_uvs = bool(opt("--keep-uvs", False))  # a textured creature (e.g. from Meshy) keeps its own UVs and material
keep_head = bool(opt("--keep-head", False))  # don't stretch the head to the game creature's nose
width_mode = opt("--width", "fit")  # fit (the game body's width), keep (the creature's proportions) or a scale
if width_mode is True:
    raise SystemExit("fit_to_game_skeleton: --width needs fit, keep or a number")

bpy.ops.wm.read_factory_settings(use_empty=True)


def import_glb(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    for o in [o for o in new if o.type == "MESH" and o.parent is None and len(o.data.polygons) <= 80 and o.name.startswith("Icosphere")]:
        bpy.data.objects.remove(o)
        new.remove(o)
    return new


game = import_glb(game_glb)
arm = next(o for o in game if o.type == "ARMATURE")
game_meshes = sorted([o for o in game if o.type == "MESH"], key=lambda o: o.name)
if uv_from == "auto":
    # the body: the mesh that reaches the ground (fur cards and manes usually don't), then the bigger one
    def lowest(o):
        return min((o.matrix_world @ v.co).z for v in o.data.vertices)
    floor = min(lowest(o) for o in game_meshes)
    uv_source = max((o for o in game_meshes if lowest(o) < floor + 0.02), key=lambda o: len(o.data.vertices))
else:
    uv_source = next(o for o in game_meshes if o.name.endswith("_mesh%d" % int(uv_from)))
if material_index is None:
    import re
    mname = uv_source.material_slots[0].material.name if uv_source.material_slots else ""
    found = re.match(r"^game_material_(\d+)", mname)
    material_index = int(found.group(1)) if found else 1
creature = import_glb(creature_glb)
c_meshes = [o for o in creature if o.type == "MESH"]
c_arms = [o for o in creature if o.type == "ARMATURE"]

# creature mesh in its rest pose, transforms applied, joined into one object
for a in c_arms:
    a.data.pose_position = "REST"
bpy.context.view_layer.update()
for m in c_meshes:
    for md in list(m.modifiers):
        m.modifiers.remove(md)
    if m.data.shape_keys:
        m.shape_key_clear()
    world = m.matrix_world.copy()
    m.parent = None
    m.matrix_world = world
bpy.ops.object.select_all(action="DESELECT")
for m in c_meshes:
    m.select_set(True)
bpy.context.view_layer.objects.active = c_meshes[0]
if len(c_meshes) > 1:
    bpy.ops.object.join()
body = bpy.context.view_layer.objects.active
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
for a in c_arms:
    bpy.data.objects.remove(a)
game_names = {o.name for o in game}
for o in [o for o in bpy.data.objects if o.type == "EMPTY" and o.name not in game_names]:
    bpy.data.objects.remove(o)
body.vertex_groups.clear()

# vertex colours from the creature's own material colours, before materials are replaced
if vertex_colors:
    colors = []
    for slot in body.material_slots:
        c = (1.0, 1.0, 1.0, 1.0)
        m = slot.material
        if m and m.use_nodes:
            b = next((n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
            if b:
                c = tuple(b.inputs["Base Color"].default_value)
        colors.append(c)
    attr = body.data.color_attributes.new("Col", "BYTE_COLOR", "CORNER")
    for poly in body.data.polygons:
        c = colors[poly.material_index] if poly.material_index < len(colors) else (1, 1, 1, 1)
        for li in poly.loop_indices:
            attr.data[li].color = c
    body.data.color_attributes.active_color = attr


def world_points(objs):
    return [o.matrix_world @ v.co for o in objs for v in o.data.vertices]


g = landmarks(world_points([o for o in game_meshes if o is uv_source] or game_meshes))
c = landmarks(world_points([body]))


def warp_y(y):
    # piecewise linear through (nose, front feet, back feet); beyond the back feet keep the front->back slope.
    # --keep-head: the head keeps the body's slope too (the creature's own head length, not the game nose)
    if y <= c["front"] and not keep_head:
        t = (y - c["nose"]) / ((c["front"] - c["nose"]) or 1e-6)
        return g["nose"] + t * (g["front"] - g["nose"])
    slope = (g["back"] - g["front"]) / ((c["back"] - c["front"]) or 1e-6)
    return g["front"] + (y - c["front"]) * slope


sx = width_scale(c, g, width_mode)
for v in body.data.vertices:
    p = v.co
    y = warp_y(p.y)
    v.co = Vector((p.x * sx, y, fit_z(p.z, y, c, g)))
body.data.update()

# one material for every face (the game body's, or with --keep-uvs the creature's own, renamed for the game slot),
# then weights (and without --keep-uvs, UVs) from the game meshes
game_mat = uv_source.material_slots[0].material if uv_source.material_slots else None
if keep_uvs and body.data.materials:
    game_mat = body.data.materials[0]
body.data.materials.clear()
if game_mat:
    body.data.materials.append(game_mat)
for poly in body.data.polygons:
    poly.material_index = 0
if not body.data.uv_layers:
    body.data.uv_layers.new(name="UVMap")
for gm in game_meshes:
    for vg in gm.vertex_groups:
        if vg.name not in body.vertex_groups:
            body.vertex_groups.new(name=vg.name)

bpy.ops.object.select_all(action="DESELECT")
body.select_set(True)
bpy.context.view_layer.objects.active = body
# weights: from the joined game meshes (a temporary copy), nearest face, interpolated
tmp = []
for gm in game_meshes:
    d = gm.copy()
    d.data = gm.data.copy()
    bpy.context.scene.collection.objects.link(d)
    for md in list(d.modifiers):
        d.modifiers.remove(md)
    tmp.append(d)
bpy.ops.object.select_all(action="DESELECT")
for d in tmp:
    d.select_set(True)
bpy.context.view_layer.objects.active = tmp[0]
if len(tmp) > 1:
    bpy.ops.object.join()
weight_src = bpy.context.view_layer.objects.active
mod = body.modifiers.new("weights", "DATA_TRANSFER")
mod.object = weight_src
mod.use_vert_data = True
mod.data_types_verts = {"VGROUP_WEIGHTS"}
mod.vert_mapping = "POLYINTERP_NEAREST"
mod.layers_vgroup_select_src = "ALL"
mod.layers_vgroup_select_dst = "NAME"
if not keep_uvs:
    mod2 = body.modifiers.new("uvs", "DATA_TRANSFER")
    mod2.object = uv_source
    mod2.use_loop_data = True
    mod2.data_types_loops = {"UV"}
    mod2.loop_mapping = "POLYINTERP_NEAREST"
bpy.ops.object.select_all(action="DESELECT")
body.select_set(True)
bpy.context.view_layer.objects.active = body
for name in ("weights",) if keep_uvs else ("weights", "uvs"):
    bpy.ops.object.modifier_apply(modifier=name)
bpy.data.objects.remove(weight_src)

# keep at most 4 weights per vertex, normalised
bpy.ops.object.vertex_group_limit_total(limit=4)
bpy.ops.object.vertex_group_normalize_all(lock_active=False)

# overall size (1.0 = the game body's size): scale about the ground under the body centre
if abs(size - 1.0) > 1e-6:
    centre_y = (g["front"] + g["back"]) / 2
    for v in body.data.vertices:
        p = v.co
        v.co = Vector((p.x * size, centre_y + (p.y - centre_y) * size, g["ground"] + (p.z - g["ground"]) * size))

# bind to the game armature, drop the game's own meshes
body.parent = arm
am = body.modifiers.new("Armature", "ARMATURE")
am.object = arm
for gm in game_meshes:
    bpy.data.objects.remove(gm)
body.name = "creature"
if game_mat:
    game_mat.name = "game_material_%d" % material_index

kw = dict(filepath=out_glb, export_format="GLB", export_skins=True, export_tangents=True, export_animations=False,
          export_yup=True, use_selection=False)
try:
    bpy.ops.export_scene.gltf(export_vertex_color="ACTIVE" if vertex_colors else "NONE", **kw)
except TypeError:
    bpy.ops.export_scene.gltf(**kw)
sz = (g["top"] - max(g["ground_front"], g["ground_back"])) / ((c["top"] - c["ground"]) or 1e-6)
print("FIT game", {k: round(v, 3) for k, v in g.items()}, "creature", {k: round(v, 3) for k, v in c.items()},
      "scales x %.3f z %.3f" % (sx, sz), "verts", len(body.data.vertices))
