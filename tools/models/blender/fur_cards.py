"""Blender (headless): a coat of fur cards on a creature fitted to a game skeleton.

  blender --background --factory-startup --python fur_cards.py -- <fitted.glb> <out.glb> --tufts <tufts.json> --slot N
      [--count 1200] [--length 0.22] [--lift 25] [--legs 0.3] [--face 0.12] [--albedo body.png --fur-colour r,g,b]
      [--seed 1]

The game's furred animals are a plain body under alpha-tested cards that each show one tuft of a strand atlas. This
adds such cards as a second mesh, named game_material_<N>: the slot of the base creature's own fur material, which
the pack then leaves as the game's (tools/models/make_mod.py replaces only the body's slot). No game texture is copied.

--tufts   a JSON list of the atlas's tufts as [u0, v0, u1, v1] (v down the picture; a tuft's root is its bottom edge)
--count   how many cards (four triangles each)
--length  a card's length in metres (each varies from 0.8 to 1.2 of it; its width follows its tuft's shape)
--lift    degrees a card stands off the skin at its root (half of that at its tip, so it droops)
--legs    no cards below this part of the body's height (bare lower legs)
--face    no cards on the front this part of the body's length (face and horns)
--albedo  the body's colour picture: each card's vertex colour is the body's colour at its root divided by
          --fur-colour (the fur atlas's own colour, 0-1), so the game's fur shader, which multiplies the two, shows the
          body's colour; without it the cards keep the atlas's colour

Cards lie along the coat: toward the tail and down, in the skin's plane. A card takes the bone weights of the body
vertex nearest its root, so it moves as one piece. The creature faces -Y (as the fitter leaves it).
"""
import json
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Vector
from mathutils.kdtree import KDTree

argv = sys.argv[sys.argv.index("--") + 1:]
src, out = os.path.abspath(argv[0]), os.path.abspath(argv[1])
opts = argv[2:]


def opt(name, default=None):
    return opts[opts.index(name) + 1] if name in opts else default


if not opt("--tufts") or opt("--slot") is None:
    raise SystemExit("fur_cards.py: --tufts <json> and --slot <n> are needed")
with open(opt("--tufts"), encoding="utf-8") as f:
    tufts = json.load(f)
slot, count, seed = int(opt("--slot")), int(opt("--count", "1200")), int(opt("--seed", "1"))
length, lift = float(opt("--length", "0.22")), math.radians(float(opt("--lift", "25")))
legs, face = float(opt("--legs", "0.3")), float(opt("--face", "0.12"))
fur_colour = [float(x) for x in opt("--fur-colour", "1,1,1").split(",")]

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
bpy.ops.import_scene.gltf(filepath=src)
arm = next((o for o in bpy.data.objects if o.type == "ARMATURE"), None)
meshes = [o for o in bpy.data.objects if o.type == "MESH"]
if arm is None or not meshes:
    raise SystemExit("fur_cards.py: %s needs a skinned mesh" % src)
body = max(meshes, key=lambda o: len(o.data.vertices))
me = body.data
mw = body.matrix_world
verts = np.array([tuple(mw @ v.co) for v in me.vertices])
me.calc_loop_triangles()
tris = np.array([t.vertices[:] for t in me.loop_triangles])
tri_loops = np.array([t.loops[:] for t in me.loop_triangles])
uv_layer = me.uv_layers.active
uvs = np.array([tuple(l.uv) for l in uv_layer.data]) if uv_layer else None

lo, hi = verts.min(axis=0), verts.max(axis=0)
min_z = lo[2] + legs * (hi[2] - lo[2])
min_y = lo[1] + face * (hi[1] - lo[1])

a, b, c = verts[tris[:, 0]], verts[tris[:, 1]], verts[tris[:, 2]]
cross = np.cross(b - a, c - a)
area = np.linalg.norm(cross, axis=1) / 2
rng = np.random.default_rng(seed)

albedo = None
if opt("--albedo") and uvs is not None:
    img = bpy.data.images.load(os.path.abspath(opt("--albedo")))
    w, h = img.size
    px = np.empty(w * h * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    albedo = px.reshape(h, w, 4)

kd = KDTree(len(verts))
for i, p in enumerate(verts):
    kd.insert(Vector(p), i)
kd.balance()

flow = Vector((0.0, 1.0, -0.6)).normalized()  # toward the tail and down
tuft_area = np.array([(t[2] - t[0]) * (t[3] - t[1]) for t in tufts])
card_v, card_uv, card_col, card_owner, card_n, faces = [], [], [], [], [], []
tries = 0
while len(card_owner) < count and tries < count * 60:
    tries += 1
    t = rng.choice(len(tris), p=area / area.sum())
    r1, r2 = rng.random(), rng.random()
    if r1 + r2 > 1:
        r1, r2 = 1 - r1, 1 - r2
    bary = (1 - r1 - r2, r1, r2)
    p = Vector(a[t] * bary[0] + b[t] * bary[1] + c[t] * bary[2])
    if p.z < min_z or p.y < min_y:
        continue
    n = Vector(cross[t]).normalized()
    along = flow - n * flow.dot(n)
    if along.length < 0.2:  # skin that faces the tail or the ground: the coat hangs down, or lies back
        along = Vector((0.0, 0.0, -1.0)) - n * (-n.z)
        if along.length < 0.2:
            along = Vector((0.0, 1.0, 0.0)) - n * n.y
    along.normalize()
    side = n.cross(along).normalized()
    rect = tufts[rng.choice(len(tufts), p=tuft_area / tuft_area.sum())]
    size = length * rng.uniform(0.8, 1.2)
    width = size * (rect[2] - rect[0]) / (rect[3] - rect[1])
    d1 = along * math.cos(lift) + n * math.sin(lift)
    d2 = along * math.cos(lift / 2) + n * math.sin(lift / 2)
    root = p - n * (0.04 * size)
    mid = root + d1 * (size / 2)
    tip = mid + d2 * (size / 2)
    base = len(card_v)
    for row, (centre, v) in enumerate(((root, rect[3]), (mid, (rect[1] + rect[3]) / 2), (tip, rect[1]))):
        for k, u in ((-0.5, rect[0]), (0.5, rect[2])):
            card_v.append(tuple(centre + side * (k * width)))
            card_uv.append((u, 1.0 - v))  # Blender's v grows up the picture
            card_n.append(tuple((n + d1 * 0.35).normalized()))
    faces += [(base, base + 1, base + 3, base + 2), (base + 2, base + 3, base + 5, base + 4)]
    colour = (1.0, 1.0, 1.0)
    if albedo is not None:
        uv = sum(uvs[tri_loops[t][k]] * bary[k] for k in range(3))
        x = min(albedo.shape[1] - 1, max(0, int(uv[0] % 1.0 * albedo.shape[1])))
        y = min(albedo.shape[0] - 1, max(0, int(uv[1] % 1.0 * albedo.shape[0])))
        colour = tuple(min(1.0, float(albedo[y, x, k]) / max(fur_colour[k], 1e-3)) for k in range(3))
    card_col.append(colour)
    card_owner.append(kd.find(p)[1])
if len(card_owner) < count:
    raise SystemExit("fur_cards.py: only %d of %d cards found a place (--legs and --face leave too little skin)"
                     % (len(card_owner), count))

mesh = bpy.data.meshes.new("fur")
mesh.from_pydata(card_v, [], faces)
mesh.update()
layer = mesh.uv_layers.new(name="UVMap")
for poly in mesh.polygons:
    for li in poly.loop_indices:
        layer.data[li].uv = card_uv[mesh.loops[li].vertex_index]
col = mesh.color_attributes.new("Col", "FLOAT_COLOR", "POINT")
for i in range(len(card_v)):
    col.data[i].color = card_col[i // 6] + (1.0,)
mesh.color_attributes.active_color = col
for poly in mesh.polygons:
    poly.use_smooth = True
mesh.normals_split_custom_set_from_vertices(card_n)
mat = bpy.data.materials.new("game_material_%d" % slot)
mesh.materials.append(mat)
fur = bpy.data.objects.new("fur", mesh)
bpy.context.scene.collection.objects.link(fur)
inv = mw.inverted()
for v in mesh.vertices:
    v.co = inv @ v.co
fur.matrix_world = mw
fur.parent = arm
mod = fur.modifiers.new("Armature", "ARMATURE")
mod.object = arm
groups = {g.index: g.name for g in body.vertex_groups}
made = {}
for card, owner in enumerate(card_owner):
    for g in me.vertices[owner].groups:
        if g.weight <= 0.0:
            continue
        name = groups[g.group]
        vg = made.get(name) or made.setdefault(name, fur.vertex_groups.new(name=name))
        vg.add(list(range(card * 6, card * 6 + 6)), g.weight, "REPLACE")

kw = dict(filepath=out, export_format="GLB", export_skins=True, export_tangents=True, export_animations=False,
          export_yup=True, use_selection=False)
try:
    bpy.ops.export_scene.gltf(export_vertex_color="ACTIVE", export_all_vertex_colors=False, **kw)
except TypeError:
    bpy.ops.export_scene.gltf(**kw)
print("CARDS %d cards, %d triangles, slot %d, above z %.2f, behind y %.2f" % (len(card_owner), len(faces) * 2, slot,
                                                                              min_z, min_y))
