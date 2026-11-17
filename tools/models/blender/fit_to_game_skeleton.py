"""Blender (headless): fit a four-legged creature mesh onto a game skeleton exported by gamegltf.py.

  blender --background --factory-startup --python fit_to_game_skeleton.py -- \
      <game.glb> <creature.glb> <out.glb> [--uv-from 1] [--material 1] [--size 1.0] [--vertex-colors]
      [--width fit|keep|<scale>] [--tail own|hips|fit] [--own-proportions <sidecar.json>] [--rigid <bone>,...]
      [--sides]

Steps:
 1. Import the game model (armature with jNNN bones + its body meshes) and the creature (any rig; it is dropped).
 2. Fit the creature onto the game body with a piecewise-linear warp along the body (nose, front feet, back feet)
    and scales for height (top of the back between the legs) and width, so its legs sit on the game's leg bones.
    When the game's rest pose holds one pair of feet lower than the other (the sabretooth's front paws), the
    creature's legs stretch below the belly to reach them (../quad_landmarks.py). The width is fitted to the game
    body's (--width fit, the default); --width keep keeps the creature's own proportions (the mean of its length and
    height scales), for a creature stretched much more in length and height than in width.
 3. Copy bone weights from all game meshes (nearest surface) and UVs from one game mesh (--uv-from), so the creature
    can use that mesh's material and textures (--material, the game material index). Detail bones then keep weight
    only where the creature has that detail (../quad_landmarks.py clean_detail_weights): a big rump on a small tail's
    bones would swing with the tail, a head on a crest's bones would bend with the crest. The spine and the legs keep
    their weights (the legs: the four branches off the spine that reach lowest, named or not; the fit stops when the
    skeleton has no four); bones under the head keep theirs when the game animal itself uses them on its head (the jaw).
    --tail hips gives the creature's own tail to the hips too (a long tail driven by a short tail's animation moves
    strangely); the default, own, keeps the game animal's tail bones on the creature's tail; fit (with
    --own-proportions) moves the game's tail bones along the creature's own tail, keeping their spacing, and weights
    the tail along its length (a long or curled tail on a short tail's bones). --rigid <bone>,... puts
    everything below those bones on them (a cat's paw on the wrist, not on the game creature's clawed fingers).
    --sides keeps each leg's weight on its own side of the centre line, fading out towards it, the rest going to the
    spine bone the leg hangs from (../quad_landmarks.py side_weights): thin parts between two legs (a skeleton's
    pelvis and ribs) follow the body instead of tearing apart when the legs swing opposite ways.
 4. --own-proportions <sidecar.json>: the creature keeps its own shape. After the weights it goes back to its
    original proportions (uniformly scaled to the game body's height, times --size), and the sidecar lists every game
    joint moved into that shape by the inverse of the fit (../quad_landmarks.py unwarp, own_space): positions in this
    script's space for a copy of the game skeleton with the creature's proportions (tools/models/make_mod.py).
    The .glb keeps the game armature as it is.
 5. --vertex-colors stores the creature's own material colours as vertex colours (COLOR_0).
 6. Export a skinned .glb that tools/models/make_mod.py reads.
Blender axes after the glTF import: forward is -Y (the head), up is +Z, width is X.
"""
import json
import os
import sys

import bpy
from mathutils import Vector

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from quad_landmarks import (centre_x, clean_detail_weights, fit_z, kept_head_bones, landmarks, leg_chains,  # noqa: E402
                            main_bones, own_space, rigid_weights, side_weights, tail_slices, unwarp, warp_y,
                            width_scale)
from quad_landmarks import along, tail_chain_weights, tail_curve, tail_distances  # noqa: E402

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
tail_mode = opt("--tail", "own")
if tail_mode not in ("own", "hips", "fit"):
    raise SystemExit("fit_to_game_skeleton: --tail needs own, hips or fit")
rigid = opt("--rigid", "")  # bones whose parts follow them as one piece: "j010_HandL,j054_HandR" (paws on wrists)
if rigid is True:
    raise SystemExit("fit_to_game_skeleton: --rigid needs bone names, comma-separated")
rigid = {b for b in rigid.split(",") if b}
own_path = opt("--own-proportions", None)  # a sidecar .json: the creature keeps its own shape (see the docstring)
if own_path is True:
    raise SystemExit("fit_to_game_skeleton: --own-proportions needs the sidecar .json to write")
if own_path and keep_head:  # the mesh returns to its own shape anyway; the head must line up for the weights
    keep_head = False
    print("FIT note: --keep-head is not used with --own-proportions (the head lines up with the game's head for the "
          "weights, and the game's neck and head bones move into the creature's head)")
sides = bool(opt("--sides", False))  # each leg's weight only on its own side of the centre line
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
# the creature's centre line onto the game's (x = 0): a tail out to one side puts a model's body off its middle
off_x = centre_x(world_points([body]), c)
for v in body.data.vertices:
    v.co.x -= off_x
body.data.update()
c = landmarks(world_points([body]))
print("FIT centre line: the creature moved %.3f sideways" % -off_x)


sx = width_scale(c, g, width_mode)
original = [v.co.copy() for v in body.data.vertices] if own_path else None
for v in body.data.vertices:
    p = v.co
    y = warp_y(p.y, c, g, keep_head)  # --keep-head: the head keeps the body's slope (its own length)
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

# detail bones only where the creature has that detail
bones = arm.data.bones


def slot(suffix):
    return next((b for b in bones if b.name.endswith(suffix)), None)


def subtree(b):
    out = {b.name}
    for c_ in b.children:
        out |= subtree(c_)
    return out


head_b = slot("_Head")
tail_pos = {}  # --tail fit: the game's tail bones' new places (this script's space, on the creature's tail)
if tail_mode == "fit" and not own_path:
    raise SystemExit("fit_to_game_skeleton: --tail fit needs --own-proportions (the game skeleton itself cannot move)")
if tail_mode == "fit" and head_b is None:
    raise SystemExit("fit_to_game_skeleton: --tail fit needs a _Head bone on the game skeleton (to find its spine)")
if sides and head_b is None:
    raise SystemExit("fit_to_game_skeleton: --sides needs a _Head bone on the game skeleton (to find its legs)")
if head_b is not None:
    try:
        main = main_bones({b.name: (b.parent.name if b.parent else None) for b in bones},
                          {b.name: (arm.matrix_world @ b.head_local).z for b in bones}, head_b.name)
    except ValueError as e:
        raise SystemExit("fit_to_game_skeleton: no four legs on the game skeleton: %s" % e)
    under_head = subtree(head_b) - {head_b.name}
    use, total = {}, 0.0  # the game animal's own weights on its head
    for gm in game_meshes:
        gnames = {g_.index: g_.name for g_ in gm.vertex_groups}
        for v in gm.data.vertices:
            ws = [(gnames[g_.group], g_.weight) for g_ in v.groups if g_.weight > 0]
            if ws and max(ws, key=lambda t: t[1])[0] in under_head | {head_b.name}:
                for n, w in ws:
                    total += w
                    if n in under_head:
                        use[n] = use.get(n, 0.0) + w
    keep = kept_head_bones(use, total)
    names = {g_.index: g_.name for g_ in body.vertex_groups}
    world = [body.matrix_world @ v.co for v in body.data.vertices]
    tail = tail_slices(world, g) if tail_mode in ("own", "fit") else set()
    weights = [{names[g_.group]: g_.weight for g_ in v.groups if g_.weight > 0} for v in body.data.vertices]
    cleaned = clean_detail_weights(weights, [round(p.y / 0.05) in tail for p in world],
                                   {b.name: (b.parent.name if b.parent else None) for b in bones}, main, keep,
                                   head_b.name)
    changed = 0
    for v, old, new in zip(body.data.vertices, weights, cleaned):
        if new == old:
            continue
        changed += 1
        for n in old:
            body.vertex_groups[n].remove([v.index])
        for n, w in new.items():
            if n not in body.vertex_groups:
                body.vertex_groups.new(name=n)
            body.vertex_groups[n].add([v.index], w, "REPLACE")
    print("FIT detail bones: kept under the head %s, tail slices %d, vertices changed %d"
          % (sorted(keep), len(tail), changed))

    # --tail fit: the game's tail bones go along the creature's own tail (its centre line, by distance along the mesh
    # from the rump), keeping their spacing, and the tail is weighted along its length
    if tail_mode == "fit":
        in_tail = [round(p.y / 0.05) in tail for p in world]
        dist = tail_distances(world, [tuple(e.vertices) for e in body.data.edges], in_tail)
        spine = set()
        b = head_b
        while b is not None:
            spine.add(b.name)
            b = b.parent
        branches = [c_ for b_ in bones if b_.name in spine and b_ is not head_b
                    for c_ in b_.children if c_.name not in spine and c_.name not in main]
        if not dist or not branches:
            raise SystemExit("fit_to_game_skeleton: --tail fit: no tail on the creature or on the game skeleton")
        root = max(branches, key=lambda r: max((arm.matrix_world @ bones[n].head_local).y for n in subtree(r)))
        chain = [root]
        while chain[-1].children:
            chain.append(max(chain[-1].children, key=lambda c_: len(subtree(c_))))
        heads = [arm.matrix_world @ b_.head_local for b_ in chain]
        seg = [(q - p).length for p, q in zip(heads, heads[1:])]
        fractions = [0.0]
        for s_ in seg:
            fractions.append(fractions[-1] + s_ / (sum(seg) or 1.0))
        curve = tail_curve(world, dist, 16)
        if len(curve) < 2:
            raise SystemExit("fit_to_game_skeleton: --tail fit: the creature's tail is too short to follow")
        tail_pos = {b_.name: Vector(p) for b_, p in zip(chain, along(curve, fractions))}
        # the first part of the tail blends from the hips (the bone the tail hangs from) into the tail bones
        ramp = min(0.15, fractions[1] if len(fractions) > 1 else 0.15)
        for vi, new in tail_chain_weights(dist, fractions, [b_.name for b_ in chain], parent=root.parent.name,
                                          ramp=ramp).items():
            v = body.data.vertices[vi]
            for g_ in list(v.groups):
                body.vertex_groups[g_.group].remove([vi])
            for n, w in new.items():
                if n not in body.vertex_groups:
                    body.vertex_groups.new(name=n)
                body.vertex_groups[n].add([vi], w, "REPLACE")
        print("FIT tail fit: %d game tail bones (%s..%s) along the creature's tail, %d of %d tail vertices reached"
              % (len(chain), chain[0].name, chain[-1].name, len(dist), sum(in_tail)))

    # --sides: each leg's weight only on its own side of the centre line (the x of the spine bone it hangs from)
    if sides:
        chains = leg_chains({b.name: (b.parent.name if b.parent else None) for b in bones},
                            {b.name: (arm.matrix_world @ b.head_local).z for b in bones}, head_b.name)
        legs = [(chain_bones, (arm.matrix_world @ bones[root].head_local).x,
                 (arm.matrix_world @ bones[parent].head_local).x, parent) for root, parent, chain_bones in chains]
        names = {g_.index: g_.name for g_ in body.vertex_groups}
        weights = [{names[g_.group]: g_.weight for g_ in v.groups if g_.weight > 0} for v in body.data.vertices]
        new_weights = side_weights(weights, [(body.matrix_world @ v.co).x for v in body.data.vertices], legs)
        changed = 0
        for v, old, new in zip(body.data.vertices, weights, new_weights):
            if new == old:
                continue
            changed += 1
            for n in old:
                body.vertex_groups[n].remove([v.index])
            for n, w in new.items():
                if n not in body.vertex_groups:
                    body.vertex_groups.new(name=n)
                body.vertex_groups[n].add([v.index], w, "REPLACE")
        print("FIT sides: legs %s on their own side, vertices changed %d"
              % (", ".join("%s (from %s)" % (root, parent) for root, parent, _ in chains), changed))

# --rigid: everything below the named bones moves onto them (a paw follows the wrist, not the game creature's fingers)
if rigid:
    missing = rigid - {b.name for b in arm.data.bones}
    if missing:
        raise SystemExit("fit_to_game_skeleton: --rigid: no bone %s in the game skeleton" % ", ".join(sorted(missing)))
    names = {g_.index: g_.name for g_ in body.vertex_groups}
    weights = [{names[g_.group]: g_.weight for g_ in v.groups if g_.weight > 0} for v in body.data.vertices]
    moved = rigid_weights(weights, {b.name: (b.parent.name if b.parent else None) for b in arm.data.bones}, rigid)
    changed = 0
    for v, old, new in zip(body.data.vertices, weights, moved):
        if new == old:
            continue
        changed += 1
        for n_ in old:
            body.vertex_groups[n_].remove([v.index])
        for n_, w in new.items():
            if n_ not in body.vertex_groups:
                body.vertex_groups.new(name=n_)
            body.vertex_groups[n_].add([v.index], w, "REPLACE")
    print("FIT rigid %s: vertices changed %d" % (sorted(rigid), changed))

# keep at most 4 weights per vertex, normalised
bpy.ops.object.vertex_group_limit_total(limit=4)
bpy.ops.object.vertex_group_normalize_all(lock_active=False)

# --own-proportions: the creature back in its own shape (at the game body's height, times --size), and every game
# joint moved into that shape by the inverse of the fit; the weights stay as the fit gave them
if own_path:
    for v, o in zip(body.data.vertices, original):
        v.co = Vector(own_space(o, c, g, size))
    joints = []
    for b in arm.data.bones:
        w = arm.matrix_world @ b.head_local
        if b.parent is None:  # the root stays at the creature's origin, where the game skeleton has it
            joints.append({"name": b.name, "game": list(w), "own": list(w)})
            continue
        at = tail_pos.get(b.name, w)  # a fitted tail bone: its place on the creature's tail
        joints.append({"name": b.name, "game": list(w), "own": list(own_space(Vector(unwarp(at, c, g, sx, keep_head)),
                                                                              c, g, size))})
    base = max(g["ground_front"], g["ground_back"])
    with open(os.path.abspath(own_path), "w", encoding="utf-8") as f:
        json.dump({"space": "Blender after the glTF import (forward -y, up +z, width x), the game armature's world",
                   "ground": base, "scale": (g["top"] - base) / ((c["top"] - max(c["ground_front"], c["ground_back"])) or 1e-6) * size,
                   "joints": joints}, f, indent=1)
    print("FIT own proportions: %d joints moved into the creature's shape, sidecar %s" % (len(joints), own_path))

# overall size (1.0 = the game body's size): scale about the ground under the body centre
if abs(size - 1.0) > 1e-6 and not own_path:
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
sz = (g["top"] - max(g["ground_front"], g["ground_back"])) / ((c["top"] - max(c["ground_front"], c["ground_back"])) or 1e-6)
print("FIT game", {k: round(v, 3) for k, v in g.items()}, "creature", {k: round(v, 3) for k, v in c.items()},
      "scales x %.3f z %.3f" % (sx, sz), "verts", len(body.data.vertices))
