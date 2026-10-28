"""Blender (headless): fit a standing humanoid (A-pose, facing -Y, e.g. from Meshy) onto a game humanoid skeleton
exported by gamegltf.py, and bind it to that skeleton so it moves with the game's animations.

  blender --background --factory-startup --python fit_humanoid.py -- <game.glb> <creature.glb> <out.glb>
      [--keep-uvs] [--material N] [--height-scale 1.0] [--skirt F]

Steps:
 1. Import the game model (armature with jNNN bones + its body meshes) and the creature (any rig: it is dropped).
 2. Scale the creature uniformly to the game body's height (ground to the top of the head; --height-scale makes it
    taller or shorter than that), feet on the ground, centred on the pelvis.
 3. Bind it with Blender's automatic (heat) weights to the bones the game body is skinned to; a vertex the heat
    solve leaves out goes to its nearest such bone. At most 4 weights per vertex, normalised.
 4. Refuse (exit 2) when a vertex is still unweighted.
    --skirt F: a robe hanging below the knees (vertices farther than F x the height from the shin and foot bones)
    follows the thigh (0.6) and the pelvis (0.4) instead of the shins, so a bent knee does not fold it into a point
    (the lich).
 5. One material: with --keep-uvs the creature's own (its UVs and texture), renamed for the game's material slot.
 6. Export a skinned .glb that tools/models/make_mod.py reads.
Blender axes after the glTF import: forward is -Y, up is +Z, width is X (the game's right hand at -X).
"""
import os
import re
import sys

import bpy
from mathutils import Vector
from mathutils.geometry import intersect_point_line

argv = sys.argv[sys.argv.index("--") + 1:]
game_glb, creature_glb, out_glb = (os.path.abspath(p) for p in argv[:3])
opts = argv[3:]


def opt(name, default=None, cast=str):
    if name in opts:
        i = opts.index(name)
        return cast(opts[i + 1]) if i + 1 < len(opts) and not opts[i + 1].startswith("--") else True
    return default


keep_uvs = bool(opt("--keep-uvs", False))
material_index = opt("--material", None, int)
height_scale = opt("--height-scale", 1.0, float)
skirt = opt("--skirt", None, float)

bpy.ops.wm.read_factory_settings(use_empty=True)


def import_glb(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    for o in [o for o in new if o.type == "MESH" and o.parent is None and len(o.data.polygons) <= 80
              and o.name.startswith("Icosphere")]:
        bpy.data.objects.remove(o)  # bone-shape helpers the importer adds
        new.remove(o)
    return new


game = import_glb(game_glb)
arm = next(o for o in game if o.type == "ARMATURE")
game_meshes = [o for o in game if o.type == "MESH"]
mw = arm.matrix_world


def bone_pos(suffix):
    b = next((b for b in arm.data.bones if b.name.endswith(suffix)), None)
    return (mw @ b.head_local) if b else None


pelvis, head = bone_pos("_Pelvis"), bone_pos("_Head")
if head is None or pelvis is None:
    raise SystemExit("fit_humanoid: the game skeleton has no named Head/Pelvis bones (gamegltf --graph-slots)")

# the game body's height: its highest point near the head (staffs, weapons and cloth proxies elsewhere are ignored)
ground = 0.0
top = max(((m.matrix_world @ v.co).z for m in game_meshes for v in m.data.vertices
           if ((m.matrix_world @ v.co).xy - head.xy).length < 0.35), default=head.z + 0.2)
if top < head.z:
    top = head.z + 0.2

# the bones the game body is skinned to (vertex group names of its meshes): only those deform
used = {vg.name for m in game_meshes for vg in m.vertex_groups}
if not used:
    raise SystemExit("fit_humanoid: the game meshes have no skin weights")


def find_bone(suffix):
    return next((b for b in arm.data.bones if b.name.endswith(suffix)), None)


def chain_to(b, stop):
    """The bones from below `stop` down to `b` (top first); empty when `stop` is not above `b`."""
    out = []
    while b is not None and b != stop:
        out.append(b)
        b = b.parent
    return list(reversed(out)) if b == stop else []


def subtree(b):
    out = {b.name}
    for c in b.children:
        out |= subtree(c)
    return out


# Only the main chains carry the creature's weights: pelvis to head (and what hangs below the head), chest to each palm
# (with the fingers), pelvis to each foot (with the toes). The game's bodies also use helper bones (the skeleton
# mage's j074/j075 and the Vukah's j100 hang from the pelvis at knee height; cloth anchors; the root) that do not
# bend with the limbs: legs bound to them poked out at the knees when the game's walk bent them (seen in game).
pelvis_b, chest_b, head_b = find_bone("_Pelvis"), find_bone("_Chest"), find_bone("_Head")
main = set()
if pelvis_b and chest_b and head_b:
    main = {pelvis_b.name} | {b.name for b in chain_to(head_b, pelvis_b)} | subtree(head_b)
    for suffix in ("_FootL", "_FootR"):
        f = find_bone(suffix)
        if f:
            main |= {b.name for b in chain_to(f, pelvis_b)} | subtree(f)
    for suffix in ("_HandPalmL", "_HandPalmR"):
        h = find_bone(suffix)
        if h:
            main |= {b.name for b in chain_to(h, chest_b)} | subtree(h)
helpers = used - main if main else set()
if main and (used & main):
    used &= main

# the creature: rest pose, transforms applied, joined into one mesh, its own rig dropped
creature = import_glb(creature_glb)
c_meshes = [o for o in creature if o.type == "MESH"]
c_arms = [o for o in creature if o.type == "ARMATURE"]
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

# 2) uniform scale to the game body's height, feet on the ground, centred on the pelvis
pts = [v.co.copy() for v in body.data.vertices]
zmin, zmax = min(p.z for p in pts), max(p.z for p in pts)
cx = (min(p.x for p in pts) + max(p.x for p in pts)) / 2
cy = (min(p.y for p in pts) + max(p.y for p in pts)) / 2
s = (top - ground) * height_scale / ((zmax - zmin) or 1e-6)
for v in body.data.vertices:
    p = v.co
    v.co = Vector((pelvis.x + (p.x - cx) * s, pelvis.y + (p.y - cy) * s, ground + (p.z - zmin) * s))
body.data.update()

# 5) one material
game_mat = None
src = next((m for m in game_meshes if m.material_slots), None)
if material_index is None and src is not None:
    found = re.match(r"^game_material_(\d+)", src.material_slots[0].material.name if src.material_slots[0].material else "")
    material_index = int(found.group(1)) if found else 0
if keep_uvs and body.data.materials:
    game_mat = body.data.materials[0]
elif src is not None:
    game_mat = src.material_slots[0].material
body.data.materials.clear()
if game_mat:
    body.data.materials.append(game_mat)
for poly in body.data.polygons:
    poly.material_index = 0
if not body.data.uv_layers:
    body.data.uv_layers.new(name="UVMap")

# 3) heat weights to the used bones only, computed on a welded copy: a generated mesh arrives split at every texture
#    seam (each face its own piece, as glTF stores it), and Blender's heat solve needs connected surfaces. When the
#    welded copy still fails (open cloth, holes), a watertight voxel copy is tried. The weights then go back to every
#    vertex of the body (nearest face, interpolated); the body keeps its own split vertices and UVs.
for b in arm.data.bones:
    b.use_deform = b.name in used
for gm in game_meshes:
    bpy.data.objects.remove(gm)
height = (zmax - zmin) * s


def copy_of(obj):
    c = obj.copy()
    c.data = obj.data.copy()
    bpy.context.scene.collection.objects.link(c)
    c.vertex_groups.clear()
    return c


def heat(obj):
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.parent_set(type="ARMATURE_AUTO")
    return sum(1 for v in obj.data.vertices if not any(g.weight > 0 for g in v.groups))


def apply_modifier(obj, kind, **props):
    md = obj.modifiers.new(kind.lower(), kind)
    for k, v in props.items():
        setattr(md, k, v)
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.modifier_apply(modifier=md.name)


welded = copy_of(body)
bpy.ops.object.select_all(action="DESELECT")
welded.select_set(True)
bpy.context.view_layer.objects.active = welded
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.mesh.select_all(action="SELECT")
bpy.ops.mesh.remove_doubles(threshold=max(1e-5, height * 1e-4))
bpy.ops.object.mode_set(mode="OBJECT")
source, how = welded, "welded"
missing = heat(welded)
if missing > len(welded.data.vertices) * 0.05:
    voxel = copy_of(body)
    apply_modifier(voxel, "REMESH", mode="VOXEL", voxel_size=max(0.005, height * 0.01))
    if heat(voxel) < missing:
        source, how = voxel, "voxel"
    else:
        bpy.data.objects.remove(voxel)

for name in used:
    if name not in body.vertex_groups:
        body.vertex_groups.new(name=name)
apply_modifier(body, "DATA_TRANSFER", object=source, use_vert_data=True, data_types_verts={"VGROUP_WEIGHTS"},
               vert_mapping="POLYINTERP_NEAREST", layers_vgroup_select_src="ALL", layers_vgroup_select_dst="NAME")
for name in {welded.name, source.name}:  # the same object when the welded copy was used
    bpy.data.objects.remove(bpy.data.objects[name])
body.parent = arm
am = body.modifiers.new("Armature", "ARMATURE")
am.object = arm
bpy.context.view_layer.objects.active = body

# vertices the heat solve left out: the nearest used bone (distance to the bone's segment)
segments = [(b.name, mw @ b.head_local, mw @ b.tail_local) for b in arm.data.bones if b.name in used]
for name, _, _ in segments:
    if name not in body.vertex_groups:
        body.vertex_groups.new(name=name)
fixed = 0
for v in body.data.vertices:
    if any(g.weight > 0 for g in v.groups):
        continue
    w = body.matrix_world @ v.co
    best, best_d = None, None
    for name, h, t in segments:
        q, f = intersect_point_line(w, h, t)
        q = h if f < 0 else t if f > 1 else q
        d = (w - q).length
        if best is None or d < best_d:
            best, best_d = name, d
    body.vertex_groups[best].add([v.index], 1.0, "REPLACE")
    fixed += 1

# --skirt: a robe below the knees follows the thigh and the pelvis, not the shins
skirted = 0
if skirt and pelvis_b:
    names = {g.index: g.name for g in body.vertex_groups}
    for side in ("L", "R"):
        f = find_bone("_Foot" + side)
        chain = chain_to(f, pelvis_b) if f else []
        if len(chain) < 3:
            continue
        lower = subtree(chain[1]) & used
        segs = [(mw @ b.head_local, mw @ b.tail_local) for b in chain[1:]]
        for name in (chain[0].name, pelvis_b.name):
            if name not in body.vertex_groups:
                body.vertex_groups.new(name=name)
        for v in body.data.vertices:
            low = [(names[g.group], g.weight) for g in v.groups if names.get(g.group) in lower and g.weight > 0]
            if not low:
                continue
            p = body.matrix_world @ v.co
            d = min((p - (h if t_ < 0 else t if t_ > 1 else q)).length
                    for h, t in segs for q, t_ in [intersect_point_line(p, h, t)])
            if d <= skirt * height:
                continue
            total = sum(w for _, w in low)
            for n, _ in low:
                body.vertex_groups[n].remove([v.index])
            body.vertex_groups[chain[0].name].add([v.index], 0.6 * total, "ADD")
            body.vertex_groups[pelvis_b.name].add([v.index], 0.4 * total, "ADD")
            skirted += 1

bpy.ops.object.select_all(action="DESELECT")
body.select_set(True)
bpy.context.view_layer.objects.active = body
bpy.ops.object.vertex_group_limit_total(limit=4)
bpy.ops.object.vertex_group_normalize_all(lock_active=False)
for vg in list(body.vertex_groups):  # groups of bones that do not deform carry nothing
    if vg.name not in used:
        body.vertex_groups.remove(vg)

# 4) refuse a mesh with unweighted vertices
left = sum(1 for v in body.data.vertices if not any(g.weight > 0 for g in v.groups))
if left:
    print("FIT_HUMANOID refused: %d vertices without weights" % left)
    sys.exit(2)

body.name = "creature"
if game_mat:
    game_mat.name = "game_material_%d" % (material_index or 0)
for b in arm.data.bones:
    b.use_deform = True  # the export keeps every joint; only the used ones carry weights
kw = dict(filepath=out_glb, export_format="GLB", export_skins=True, export_tangents=True, export_animations=False,
          export_yup=True, use_selection=False)
bpy.ops.export_scene.gltf(**kw)
print("FIT_HUMANOID game top %.3f, scale %.3f, height %.3f, verts %d, nearest-bone fixes %d (heat on the %s copy), "
      "helper bones left out %d, skirt vertices %d"
      % (top, s, (zmax - zmin) * s, len(body.data.vertices), fixed, how, len(helpers), skirted))
