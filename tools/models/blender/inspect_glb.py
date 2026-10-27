"""Blender (headless): describe a .glb (meshes, bounds, armature bones, materials, textures, animations) + a render.

Run: blender --background --factory-startup --python inspect_glb.py -- <file.glb> <out.png>
"""
import json
import os
import sys

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
path, png = os.path.abspath(argv[0]), os.path.abspath(argv[1])
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=path)
for o in [o for o in bpy.context.scene.objects if o.type == "MESH" and o.parent is None and not o.data.polygons]:
    bpy.data.objects.remove(o)
meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
arms = [o for o in bpy.context.scene.objects if o.type == "ARMATURE"]
pts = [o.matrix_world @ v.co for o in meshes for v in o.data.vertices]
lo = [min(p[k] for p in pts) for k in range(3)]
hi = [max(p[k] for p in pts) for k in range(3)]
info = {
    "meshes": [{"name": o.name, "verts": len(o.data.vertices), "tris": sum(len(p.vertices) - 2 for p in o.data.polygons),
                "uv_layers": [u.name for u in o.data.uv_layers], "materials": [m.name for m in o.data.materials if m],
                "groups": len(o.vertex_groups), "shape_keys": bool(o.data.shape_keys)} for o in meshes],
    "bounds_blender": [[round(x, 3) for x in lo], [round(x, 3) for x in hi]],
    "armatures": [{"name": a.name, "bones": [b.name for b in a.data.bones][:60], "count": len(a.data.bones)} for a in arms],
    "head_bone": None,
    "images": [{"name": i.name, "size": list(i.size)} for i in bpy.data.images],
    "actions": [a.name for a in bpy.data.actions][:30],
    "materials": {},
}
for m in bpy.data.materials:
    d = {}
    if m.use_nodes:
        b = next((n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
        if b:
            inp = b.inputs["Base Color"]
            d["base_color"] = [round(c, 3) for c in inp.default_value]
            d["base_color_linked"] = inp.is_linked
    info["materials"][m.name] = d
for a in arms:
    hb = next((b for b in a.data.bones if "head" in b.name.lower()), None)
    if hb:
        info["head_bone"] = {"name": hb.name, "pos": [round(x, 3) for x in (a.matrix_world @ hb.head_local)]}
print("INSPECT " + json.dumps(info))

scene = bpy.context.scene
scene.render.engine = "BLENDER_WORKBENCH"
scene.display.shading.light = "STUDIO"
scene.display.shading.color_type = "MATERIAL"
scene.render.resolution_x, scene.render.resolution_y = 900, 640
center = Vector([(lo[k] + hi[k]) / 2 for k in range(3)])
size = max(hi[k] - lo[k] for k in range(3))
cam_data = bpy.data.cameras.new("cam")
cam_data.type = "ORTHO"
cam_data.ortho_scale = size * 1.25
cam = bpy.data.objects.new("cam", cam_data)
scene.collection.objects.link(cam)
cam.location = center + Vector((size * 2.0, -size * 1.2, size * 0.5))
cam.rotation_euler = (center - cam.location).to_track_quat("-Z", "Y").to_euler()
scene.camera = cam
scene.frame_set(0)
scene.render.filepath = png
bpy.ops.render.render(write_still=True)
