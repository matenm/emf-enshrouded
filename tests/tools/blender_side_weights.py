"""Blender (headless), for tests: how much weight a fitted quadruped's vertices carry from legs on the other side of the
body's centre line.

  blender --background --factory-startup --python blender_side_weights.py -- <fitted.glb> <leg root bone> [...]

A leg's side is where its root bone sits against the bone it hangs from (x). Prints "CROSS <weight>": the most weight
any vertex off the centre line carries from legs on the other side, and "MID <weight>": the most weight any vertex
within 1 mm of the centre line carries from the legs.
"""
import sys

import bpy

path, *roots = sys.argv[sys.argv.index("--") + 1:]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=path)
arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
mesh = next(o for o in bpy.data.objects if o.type == "MESH" and o.parent is not None)


def subtree(b):
    out = {b.name}
    for c in b.children:
        out |= subtree(c)
    return out


legs = []
for r in roots:
    b = arm.data.bones[r]
    centre = (arm.matrix_world @ b.parent.head_local).x
    legs.append((subtree(b), 1 if (arm.matrix_world @ b.head_local).x > centre else -1, centre))
names = {g.index: g.name for g in mesh.vertex_groups}
worst, mid = 0.0, 0.0
for v in mesh.data.vertices:
    x = (mesh.matrix_world @ v.co).x
    for bones, side, centre in legs:
        w = sum(g.weight for g in v.groups if names[g.group] in bones)
        if abs(x - centre) < 1e-3:
            mid = max(mid, w)
        elif (x - centre) * side < 0:
            worst = max(worst, w)
print("CROSS %.4f" % worst)
print("MID %.4f" % mid)
