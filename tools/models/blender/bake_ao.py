"""Blender (headless): a model's ambient occlusion baked into a picture on its own UVs.

  blender --background --factory-startup --python bake_ao.py -- <model.glb> <out.png> [--size 1024] [--samples 64]
      [--distance 0.25] [--margin 8]

Every mesh of the file is baked together in its rest pose (they shadow each other); white is open, dark is hidden.
--distance is how far a ray looks for a blocker, in the file's units (metres): a short one darkens folds and the
roots of horns and legs without darkening the whole belly. The picture is the --ao of blender/material_maps.py.
A mesh without UVs stops the script.
"""
import os
import sys

import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
src, out = os.path.abspath(argv[0]), os.path.abspath(argv[1])
opts = argv[2:]


def opt(name, default):
    return opts[opts.index(name) + 1] if name in opts else default


size, samples = int(opt("--size", "1024")), int(opt("--samples", "64"))
distance, margin = float(opt("--distance", "0.25")), int(opt("--margin", "8"))

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
bpy.ops.import_scene.gltf(filepath=src)
meshes = [o for o in bpy.data.objects if o.type == "MESH"]
if not meshes:
    raise SystemExit("bake_ao.py: no mesh in %s" % src)
for o in meshes:
    if not o.data.uv_layers:
        raise SystemExit("bake_ao.py: the mesh %s has no UVs" % o.name)

img = bpy.data.images.new("ao", size, size, alpha=False, is_data=True)
img.generated_color = (1.0, 1.0, 1.0, 1.0)
# every material gets the picture as its active image node: the bake writes there
for o in meshes:
    if not o.data.materials:
        o.data.materials.append(bpy.data.materials.new("ao"))
    for i, mat in enumerate(o.data.materials):
        if mat is None:
            mat = o.data.materials[i] = bpy.data.materials.new("ao")
        mat.use_nodes = True
        node = mat.node_tree.nodes.new("ShaderNodeTexImage")
        node.image = img
        mat.node_tree.nodes.active = node

scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.samples = samples
scene.cycles.device = "CPU"
scene.world = scene.world or bpy.data.worlds.new("world")
scene.world.light_settings.distance = distance
scene.render.bake.margin = margin
scene.render.bake.use_clear = False  # texels no face covers stay white (open)

bpy.ops.object.select_all(action="DESELECT")
for o in meshes:
    o.select_set(True)
bpy.context.view_layer.objects.active = meshes[0]
bpy.ops.object.bake(type="AO")

img.filepath_raw = out
img.file_format = "PNG"
img.save()
print("AO %dx%d, %d samples, distance %.2f: %s" % (size, size, samples, distance, out))
