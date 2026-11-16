"""Blender (headless): writes a simple four-legged animal (boxes: a body, four legs, a head, a thin tail behind; facing
-Y like the game's creatures after the glTF import) as a .glb, a test figure for fit_to_game_skeleton.py.

  blender --background --factory-startup --python make_test_quadruped.py -- <out.glb> [--hind-bar]

Body 2.0 m long (y -0.8..1.2: a big rump well behind the hind legs, like an aurochs), 0.6 m wide, 0.5 m deep at
0.6-1.1 m; legs at y -0.6 and 0.6; the head in front at y -1.0; the tail 0.06 m thick from the rump back to y 1.9.
--hind-bar adds a thin bar from one hind leg to the other at 0.45 m (a skeleton's pelvis between its legs), with
vertices on the centre line. Every box is subdivided, so there are vertices to weigh.
"""
import os
import sys

import bpy

args = sys.argv[sys.argv.index("--") + 1:]
out = os.path.abspath(args[0])
hind_bar = "--hind-bar" in args[1:]
bpy.ops.wm.read_factory_settings(use_empty=True)


def box(loc, size, cuts=4):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=loc)
    o = bpy.context.active_object
    o.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.subdivide(number_cuts=cuts)
    bpy.ops.object.mode_set(mode="OBJECT")
    return o


parts = [box((0, 0.2, 0.85), (0.6, 2.0, 0.5))]
for x in (-0.2, 0.2):
    for y in (-0.6, 0.6):
        parts.append(box((x, y, 0.3), (0.15, 0.15, 0.6)))
parts.append(box((0, -1.0, 1.05), (0.35, 0.45, 0.35)))
parts.append(box((0, 1.55, 1.0), (0.06, 0.7, 0.06), cuts=8))
if hind_bar:
    parts.append(box((0, 0.6, 0.45), (0.5, 0.06, 0.06), cuts=5))  # 6 segments across: a vertex row at x = 0
bpy.ops.object.select_all(action="DESELECT")
for o in parts:
    o.select_set(True)
bpy.context.view_layer.objects.active = parts[0]
bpy.ops.object.join()
bpy.ops.export_scene.gltf(filepath=out, export_format="GLB")
print("TEST QUADRUPED", out)
