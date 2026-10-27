"""Blender (headless) check of an exported .glb: armature, skin binding, a posed render.

Run: blender --background --factory-startup --python check_glb.py -- <file.glb> <out_prefix> [bone_to_bend]
Writes <out_prefix>_rest.png, <out_prefix>_posed.png and prints a JSON summary line starting with 'CHECK '.
"""
import json
import math
import sys

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
import os
path, prefix = os.path.abspath(argv[0]), os.path.abspath(argv[1])
bend = argv[2] if len(argv) > 2 and argv[2] != "-" else None
texture = os.path.abspath(argv[3]) if len(argv) > 3 else None

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=path)
arm = next(o for o in bpy.context.scene.objects if o.type == "ARMATURE")
meshes = [o for o in bpy.context.scene.objects if o.type == "MESH" and o.parent is not None]
for o in [o for o in bpy.context.scene.objects if o.type == "MESH" and o.parent is None]:
    bpy.data.objects.remove(o)  # bone-shape helpers the importer adds
def weight_counts(m):
    """(vertices with no weight above 0, the most weights any vertex has)"""
    unweighted, most = 0, 0
    for v in m.data.vertices:
        n = sum(1 for g in v.groups if g.weight > 0)
        unweighted += n == 0
        most = max(most, n)
    return unweighted, most


summary = {
    "bones": len(arm.data.bones),
    "named_bones": [b.name for b in arm.data.bones if "_" in b.name],
    "meshes": [{"name": m.name, "verts": len(m.data.vertices), "polys": len(m.data.polygons),
                "groups": len(m.vertex_groups),
                "unweighted": weight_counts(m)[0], "max_influences": weight_counts(m)[1],
                "armature_mod": any(md.type == "ARMATURE" and md.object == arm for md in m.modifiers)} for m in meshes],
}


def bounds():
    deps = bpy.context.evaluated_depsgraph_get()
    pts = []
    for m in meshes:
        ev = m.evaluated_get(deps)
        me = ev.to_mesh()
        pts += [ev.matrix_world @ v.co for v in me.vertices]
        ev.to_mesh_clear()
    return [round(min(p[k] for p in pts), 3) for k in range(3)], [round(max(p[k] for p in pts), 3) for k in range(3)]


summary["rest_bounds"] = bounds()
if texture:
    tm = bpy.data.materials.new("preview")
    tm.use_nodes = True
    node = tm.node_tree.nodes.new("ShaderNodeTexImage")
    node.image = bpy.data.images.load(texture)
    node.interpolation = "Closest"
    bsdf = next(n for n in tm.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    tm.node_tree.links.new(node.outputs["Color"], bsdf.inputs["Base Color"])
    for m in meshes:
        m.data.materials.clear()
        m.data.materials.append(tm)

# camera + light + workbench render
scene = bpy.context.scene
scene.render.engine = "BLENDER_WORKBENCH"
scene.display.shading.light = "STUDIO"
scene.display.shading.color_type = "TEXTURE" if texture else "MATERIAL"
scene.render.resolution_x, scene.render.resolution_y = 900, 640
lo, hi = summary["rest_bounds"]
center = Vector([(lo[k] + hi[k]) / 2 for k in range(3)])
size = max(hi[k] - lo[k] for k in range(3))
cam_data = bpy.data.cameras.new("cam")
cam_data.type = "ORTHO"
cam_data.ortho_scale = size * 1.25
cam = bpy.data.objects.new("cam", cam_data)
scene.collection.objects.link(cam)
# glTF +Y up becomes Blender +Z up, glTF +Z forward becomes Blender -Y; view the model's left side, slightly front.
cam.location = center + Vector((size * 2.0, -size * 1.2, size * 0.5))
direction = center - cam.location
cam.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
scene.camera = cam
scene.render.filepath = prefix + "_rest.png"
bpy.ops.render.render(write_still=True)

if bend:
    pb = arm.pose.bones.get(bend) or next((b for b in arm.pose.bones if b.name.endswith(bend)), None)
    if pb:
        pb.rotation_mode = "XYZ"
        pb.rotation_euler = (math.radians(35), 0, math.radians(25))
        bpy.context.view_layer.update()
        summary["posed_bone"] = pb.name
        summary["posed_bounds"] = bounds()
        scene.render.filepath = prefix + "_posed.png"
        bpy.ops.render.render(write_still=True)
print("CHECK " + json.dumps(summary))
