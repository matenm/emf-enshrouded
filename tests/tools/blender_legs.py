"""Blender (headless), for tests: how the legs of a fitted humanoid glb sit on the game skeleton's leg bones.

  blender --background --factory-startup --python blender_legs.py -- <fitted.glb> <skirt_fraction>

Prints "HELPERS <weight>": the skin weight below the hips on bones off the skeleton's main chains (head-to-pelvis,
chest-to-palms, pelvis-to-feet and what hangs below hands, feet and head; e.g. the helper bones the game's bodies hang
from the pelvis at knee height, or the root). Then, per side, "LEGS <side> knee_offset <m> skirt_on_shin <n>": the
horizontal distance between the knee bone and
the centroid of the vertices mostly weighted to that leg at knee height; and how many vertices farther than
skirt_fraction x the height from the shin and foot bones (a robe hanging below the knee) still carry shin or foot
weight. Bones are found by gamegltf.py's slot names (_Pelvis, _FootL, _FootR).
"""
import sys

import bpy
from mathutils.geometry import intersect_point_line

argv = sys.argv[sys.argv.index("--") + 1:]
path, skirt = argv[0], float(argv[1])
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=path)
arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
mesh = next(o for o in bpy.data.objects if o.type == "MESH" and o.parent is not None)
mw = arm.matrix_world
bones = arm.data.bones


def find(suffix):
    return next(b for b in bones if b.name.endswith(suffix))


def chain_to(b, stop):
    out = []
    while b is not None and b.name != stop.name:
        out.append(b)
        b = b.parent
    return list(reversed(out))


def subtree(b):
    out = {b.name}
    for c in b.children:
        out |= subtree(c)
    return out


pelvis = find("_Pelvis")
names = {g.index: g.name for g in mesh.vertex_groups}
main = set()
for s_ in ("_FootL", "_FootR"):
    f = find(s_)
    main |= {b.name for b in chain_to(f, pelvis)} | subtree(f)
for s_ in ("_HandPalmL", "_HandPalmR"):
    h = find(s_)
    main |= {b.name for b in chain_to(h, find("_Chest"))} | subtree(h)
head = find("_Head")
main |= {b.name for b in chain_to(head, pelvis)} | subtree(head) | {pelvis.name}
hip_z = (mw @ pelvis.head_local).z
helpers = sum(g.weight for v in mesh.data.vertices if (mesh.matrix_world @ v.co).z < hip_z
              for g in v.groups if names.get(g.group) not in main)
print("HELPERS %.1f" % helpers)
pts = [(v, mesh.matrix_world @ v.co) for v in mesh.data.vertices]
height = max(p.z for _, p in pts) - min(p.z for _, p in pts)
for side in ("L", "R"):
    chain = chain_to(find("_Foot" + side), pelvis)
    leg, lower = subtree(chain[0]), subtree(chain[1])
    knee = mw @ chain[1].head_local
    near = [p for v, p in pts if abs(p.z - knee.z) < 0.03 * height
            and sum(g.weight for g in v.groups if names.get(g.group) in leg) > 0.5]
    if near:
        cx = sum(p.x for p in near) / len(near)
        cy = sum(p.y for p in near) / len(near)
        offset = ((cx - knee.x) ** 2 + (cy - knee.y) ** 2) ** 0.5
    else:
        offset = float("nan")
    segs = [(mw @ b.head_local, mw @ b.tail_local) for b in chain[1:]]
    on_shin = 0
    for v, p in pts:
        if p.z >= knee.z or not any(names.get(g.group) in lower and g.weight > 0 for g in v.groups):
            continue
        d = min((p - (h if f < 0 else t if f > 1 else q)).length
                for h, t in segs for q, f in [intersect_point_line(p, h, t)])
        if d > skirt * height:
            on_shin += 1
    print("LEGS %s knee_offset %.3f skirt_on_shin %d" % (side, offset, on_shin))
