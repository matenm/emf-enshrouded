"""Blender (headless): give a flat-coloured creature (low-poly style) a palette texture.

  blender --background --factory-startup --python palette_texture.py -- <in.glb> <out.glb> <out_dir>
      [--size 128] [--tint r,g,b] [--add r,g,b]

The input needs vertex colours (fit_to_game_skeleton.py --vertex-colors stores the model's material colours).
Every distinct face colour gets a square cell in a size x size texture (8 x 8 cells); every corner of a face gets the
UV of its colour's cell centre. --tint multiplies and --add adds (in sRGB 0-1) to every colour: a pale glowing
"spirit" look is for example --tint 0.55,0.8,1.0 --add 0.15,0.25,0.35.
Writes <out_dir>/albedo_mip00.png ... (every level down to 1x1) and <out.glb> with the new UVs.
"""
import os
import sys

import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
src, dst, out_dir = os.path.abspath(argv[0]), os.path.abspath(argv[1]), os.path.abspath(argv[2])
opts = argv[3:]


def opt(name, default=None):
    return opts[opts.index(name) + 1] if name in opts else default


size = int(opt("--size", "128"))
tint = [float(x) for x in opt("--tint", "1,1,1").split(",")]
add = [float(x) for x in opt("--add", "0,0,0").split(",")]
GRID = 8
os.makedirs(out_dir, exist_ok=True)

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
for o in [o for o in bpy.context.scene.objects if o.type == "MESH" and o.parent is None and o.name.startswith("Icosphere")]:
    bpy.data.objects.remove(o)
body = next(o for o in bpy.context.scene.objects if o.type == "MESH")
mesh = body.data
mat_name = mesh.materials[0].name if mesh.materials else "game_material_1"
colors = mesh.color_attributes.active_color or (mesh.color_attributes[0] if len(mesh.color_attributes) else None)
if colors is None:
    raise SystemExit("no vertex colours: run fit_to_game_skeleton.py with --vertex-colors")


def corner_color(li):
    if colors.domain == "CORNER":
        c = colors.data[li].color_srgb
    else:
        c = colors.data[mesh.loops[li].vertex_index].color_srgb
    return tuple(round(c[k], 3) for k in range(3))


palette = {}
face_cell = []
for poly in mesh.polygons:
    c = corner_color(poly.loop_indices[0])
    if c not in palette:
        if len(palette) >= GRID * GRID:
            raise SystemExit("more than %d colours; use a baked texture instead" % (GRID * GRID))
        palette[c] = len(palette)
    face_cell.append(palette[c])

uv = mesh.uv_layers.active or mesh.uv_layers.new(name="UVMap")
for poly, cell in zip(mesh.polygons, face_cell):
    u = (cell % GRID + 0.5) / GRID
    v = 1.0 - (cell // GRID + 0.5) / GRID  # Blender's v is bottom-up
    for li in poly.loop_indices:
        uv.data[li].uv = (u, v)


def final(c):
    return [max(0.0, min(1.0, c[k] * tint[k] + add[k])) for k in range(3)]


cell_px = size // GRID
pixels = [0.0] * (size * size * 4)
for c, cell in palette.items():
    rgb = final(c)
    cx, cy = cell % GRID, cell // GRID
    for y in range(cy * cell_px, (cy + 1) * cell_px):
        row = size - 1 - y  # image rows are bottom-up
        for x in range(cx * cell_px, (cx + 1) * cell_px):
            i = (row * size + x) * 4
            pixels[i:i + 4] = [rgb[0], rgb[1], rgb[2], 1.0]
img = bpy.data.images.new("albedo", size, size, alpha=False)
img.pixels = pixels
level, w = 0, size
while True:
    lvl = img if level == 0 else img.copy()
    if level:
        lvl.scale(w, w)
    lvl.filepath_raw = os.path.join(out_dir, "albedo_mip%02d.png" % level)
    lvl.file_format = "PNG"
    lvl.save()
    if w == 1:
        break
    w //= 2
    level += 1

final_mat = bpy.data.materials.new(mat_name)
mesh.materials.clear()
mesh.materials.append(final_mat)
bpy.ops.export_scene.gltf(filepath=dst, export_format="GLB", export_skins=True, export_tangents=True,
                          export_animations=False, export_yup=True)
print("PALETTE %d colours, %d levels of %dx%d in %s; mesh: %s" % (len(palette), level + 1, size, size, out_dir, dst))
