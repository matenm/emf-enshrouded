"""Blender (headless): a model's glTF normal and roughness maps -> the game's normal and material map levels.

  blender --background --factory-startup --python material_maps.py -- <out_dir> [--normal normal.png]
      [--size 1024] [--keep-green] [--strength 1.0] [--roughness roughness.png] [--ao ao.png] [--red 0]
      [--material-size 512]

Writes normal_mip00.png ... and material_mip00.png ... (square powers of two, every level down to 1x1) into
<out_dir>, the folder of the albedo levels (blender/texture_mips.py); tools/models/make_mod.py reads them from there.

The game's normal map holds x in red and y in green (two channels; blue is written for viewing only). Its green grows
up the picture; a glTF normal map's as Meshy writes it grows down, so green is turned unless --keep-green. --strength
scales the slope (x and y) before the normal is made a unit vector again. Every level is box filtered from the level
above and made unit vectors again.

The game's material map holds roughness in green and ambient occlusion in blue (white: none). Red is --red on every
pixel (0 to 1; the game's furred bodies have 0 there).
"""
import os
import sys

import bpy
import numpy as np

argv = sys.argv[sys.argv.index("--") + 1:]
if not argv or argv[0].startswith("--"):
    raise SystemExit("material_maps.py -- <out_dir> [--normal png] [--roughness png] [--ao png] ...")
out_dir = os.path.abspath(argv[0])
opts = argv[1:]


def opt(name, default=None):
    return opts[opts.index(name) + 1] if name in opts else default


def load(path, size):
    """The picture's values as stored (no colour conversion), size x size x 4, rows from the bottom."""
    img = bpy.data.images.load(os.path.abspath(path))
    img.colorspace_settings.name = "Non-Color"
    img.scale(size, size)
    px = np.empty(size * size * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    return px.reshape(size, size, 4)


def save(px, path):
    size = px.shape[0]
    img = bpy.data.images.new("out", size, size, alpha=False, is_data=True)
    img.pixels.foreach_set(np.clip(px, 0.0, 1.0).astype(np.float32).ravel())
    img.filepath_raw = path
    img.file_format = "PNG"
    img.save()
    bpy.data.images.remove(img)


def half(px):
    s = px.shape[0] // 2
    return px.reshape(s, 2, s, 2, px.shape[2]).mean(axis=(1, 3))


def unit(px):
    """Red and green as a unit normal's x and y, blue as its z."""
    xy = px[..., :2] * 2.0 - 1.0
    z = np.sqrt(np.clip(1.0 - (xy * xy).sum(axis=-1), 0.0, 1.0))
    v = np.dstack([xy, z])
    v /= np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-6)
    out = np.ones(px.shape, dtype=np.float32)
    out[..., :3] = v * 0.5 + 0.5
    return out


def write_levels(px, kind, fix=lambda p: p):
    level = 0
    while True:
        save(px, os.path.join(out_dir, "%s_mip%02d.png" % (kind, level)))
        if px.shape[0] == 1:
            return level + 1
        px = fix(half(px))
        level += 1


normal, roughness = opt("--normal"), opt("--roughness")
if not normal and not roughness:
    raise SystemExit("material_maps.py: name --normal or --roughness (or both)")
os.makedirs(out_dir, exist_ok=True)
done = []
if normal:
    size = int(opt("--size", "1024"))
    px = load(normal, size)
    xy = (px[..., :2] * 2.0 - 1.0) * float(opt("--strength", "1.0"))
    if "--keep-green" not in opts:
        xy[..., 1] = -xy[..., 1]
    px[..., :2] = np.clip(xy, -1.0, 1.0) * 0.5 + 0.5
    done.append("normal %d levels from %dx%d" % (write_levels(unit(px), "normal", unit), size, size))
if roughness:
    size = int(opt("--material-size", "512"))
    px = np.ones((size, size, 4), dtype=np.float32)
    px[..., 0] = float(opt("--red", "0"))
    px[..., 1] = load(roughness, size)[..., 0]
    if opt("--ao"):
        px[..., 2] = load(opt("--ao"), size)[..., 0]
    done.append("material %d levels from %dx%d" % (write_levels(px, "material"), size, size))
print("MAPS %s in %s" % ("; ".join(done), out_dir))
