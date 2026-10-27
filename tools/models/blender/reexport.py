"""Blender (headless): import a .glb and export it again the way an artist would (skinned, with tangents).

Run: blender --background --factory-startup --python reexport.py -- <in.glb> <out.glb>
"""
import os
import sys

import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
src, dst = os.path.abspath(argv[0]), os.path.abspath(argv[1])
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
for o in [o for o in bpy.context.scene.objects if o.type == "MESH" and o.parent is None]:
    bpy.data.objects.remove(o)  # bone-shape helpers the importer adds
bpy.ops.export_scene.gltf(filepath=dst, export_format="GLB", export_skins=True, export_tangents=True,
                          export_animations=False, export_yup=True, export_apply=False)
print("REEXPORTED", dst)
