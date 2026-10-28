"""Blender (headless): writes a simple humanoid in A-pose (boxes and spheres, about 1.8 m, facing -Y like the game's
characters after the glTF import) as a .glb, a test figure for fit_humanoid.py.

  blender --background --factory-startup --python make_test_humanoid.py -- <out.glb> [--messy] [--wide-legs]

--messy adds what generated models (Meshy) have and Blender's heat weighting fails on: open cloth panels (a robe
without a back), separate overlapping parts and doubled faces.
"""
import math
import os
import sys

import bpy

args = sys.argv[sys.argv.index("--") + 1:]
out = os.path.abspath(args[0])
messy = "--messy" in args
wide_legs = "--wide-legs" in args  # legs at x +-0.19 instead of +-0.10 (wider than the game skeleton's)
bpy.ops.wm.read_factory_settings(use_empty=True)


def box(name, loc, size, rot_y=0.0):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=loc)
    o = bpy.context.active_object
    o.name = name
    o.scale = size
    o.rotation_euler = (0.0, rot_y, 0.0)
    return o


parts = [
    box("torso", (0, 0, 1.15), (0.36, 0.2, 0.6)),
    box("hips", (0, 0, 0.82), (0.34, 0.2, 0.16)),
    box("leg_l", (0.19 if wide_legs else 0.1, 0, 0.42), (0.12, 0.12, 0.84)),
    box("leg_r", (-0.19 if wide_legs else -0.1, 0, 0.42), (0.12, 0.12, 0.84)),
    # A-pose arms: from the shoulders, 45 degrees down
    box("arm_l", (0.18 + 0.25, 0, 1.4 - 0.25), (0.6, 0.1, 0.1), rot_y=math.radians(45)),
    box("arm_r", (-0.18 - 0.25, 0, 1.4 - 0.25), (0.6, 0.1, 0.1), rot_y=math.radians(-45)),
]
if wide_legs:  # enough vertices along the legs to find them at knee height
    for o in parts[2:4]:
        bpy.ops.object.select_all(action="DESELECT")
        o.select_set(True)
        bpy.context.view_layer.objects.active = o
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.mesh.subdivide(number_cuts=6)
        bpy.ops.object.mode_set(mode="OBJECT")
bpy.ops.mesh.primitive_uv_sphere_add(radius=0.13, location=(0, 0, 1.62))
parts.append(bpy.context.active_object)
if messy:
    # an open robe panel in front (a single subdivided plane, no thickness), down to the ground
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=12, y_subdivisions=24, size=1.0, location=(0, -0.13, 0.75))
    robe = bpy.context.active_object
    robe.rotation_euler = (math.radians(90), 0, 0)
    robe.scale = (0.5, 1.45, 1.0)
    parts.append(robe)
    # sleeves: open tubes (cylinders without caps) around the arms, overlapping them
    for side in (1, -1):
        bpy.ops.mesh.primitive_cylinder_add(vertices=12, radius=0.09, depth=0.5, end_fill_type="NOTHING",
                                            location=(side * 0.45, 0, 1.13), rotation=(0, math.radians(90 - 45 * side * side), 0))
        sleeve = bpy.context.active_object
        sleeve.rotation_euler = (0, side * math.radians(45) + math.radians(90), 0)
        parts.append(sleeve)
    # a doubled head (the same sphere twice)
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.13, location=(0, 0, 1.62))
    parts.append(bpy.context.active_object)
for o in parts:
    bpy.context.view_layer.objects.active = o
    o.select_set(True)
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
bpy.ops.object.select_all(action="DESELECT")
for o in parts:
    o.select_set(True)
bpy.context.view_layer.objects.active = parts[0]
bpy.ops.object.join()
if messy:
    # every face on its own, as a glTF from Meshy arrives (vertices split at every texture seam: a triangle soup)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.edge_split(type="EDGE")
    bpy.ops.object.mode_set(mode="OBJECT")
bpy.ops.export_scene.gltf(filepath=out, export_format="GLB")
print("TEST HUMANOID", out)
