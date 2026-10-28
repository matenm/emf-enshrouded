"""Blender (headless), for tests: copy a game glb with one bone's slot name replaced (a skeleton whose slot map lacks
that name).

  blender --background --factory-startup --python blender_rename_bone.py -- <in.glb> <out.glb> <suffix> <new suffix>
"""
import sys

import bpy

src, out, old, new = sys.argv[sys.argv.index("--") + 1:][:4]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
bone = next(b for b in arm.data.bones if b.name.endswith(old))
renamed = bone.name[: len(bone.name) - len(old)] + new
for m in bpy.data.objects:
    if m.type == "MESH" and bone.name in m.vertex_groups:
        m.vertex_groups[bone.name].name = renamed
bone.name = renamed
bpy.ops.export_scene.gltf(filepath=out, export_format="GLB", export_skins=True, export_animations=False)
print("RENAMED", renamed)
