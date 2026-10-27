"""Blender (headless): a texture -> albedo_mip00.png ... (square power of two, every level down to 1x1, box filtered).

  blender --background --factory-startup --python texture_mips.py -- <texture.png> <out_dir> [--size 1024]
      [--tint r,g,b] [--add r,g,b]

--tint multiplies and --add adds to every pixel (0-1, sRGB as stored): a "spirit" version of a painted texture is for
example --tint 0.5,0.75,1.0 --add 0.1,0.2,0.35. The result feeds tools/models/make_mod.py ("texture": {"albedo_dir"}).
"""
import os
import sys

import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
src, out_dir = os.path.abspath(argv[0]), os.path.abspath(argv[1])
opts = argv[2:]


def opt(name, default=None):
    return opts[opts.index(name) + 1] if name in opts else default


size = int(opt("--size", "1024"))
tint = [float(x) for x in opt("--tint", "1,1,1").split(",")]
add = [float(x) for x in opt("--add", "0,0,0").split(",")]
os.makedirs(out_dir, exist_ok=True)
img = bpy.data.images.load(src)
img.scale(size, size)
if tint != [1.0, 1.0, 1.0] or add != [0.0, 0.0, 0.0]:
    px = list(img.pixels)
    for i in range(0, len(px), 4):
        for k in range(3):
            px[i + k] = max(0.0, min(1.0, px[i + k] * tint[k] + add[k]))
    img.pixels = px
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
print("MIPS %d levels from %dx%d in %s" % (level + 1, size, size, out_dir))
