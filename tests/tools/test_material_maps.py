"""tools/models/blender/material_maps.py: a glTF normal map and roughness map as the game's normal and material map
levels. Needs Blender (environment variable BLENDER, or blender on PATH): skipped without."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, os.path.join(ROOT, "tools", "models"))
import png_crop  # noqa: E402

BLENDER = os.environ.get("BLENDER") or shutil.which("blender")
SCRIPT = os.path.join(ROOT, "tools", "models", "blender", "material_maps.py")


def flat_png(path, size, rgb):
    png_crop.write_png(path, size, size, [bytes(rgb) * size for _ in range(size)])
    return path


def pixel(path):
    """(width, the first pixel's r, g, b)"""
    w, h, channels, rows = png_crop.read_png(path)
    return (w,) + tuple(rows[0][:3])


def run(*args):
    return subprocess.run([BLENDER, "--background", "--factory-startup", "--python-exit-code", "1", "--python", SCRIPT,
                           "--", *args], capture_output=True, text=True, timeout=600)


@unittest.skipUnless(BLENDER, "needs Blender")
class MaterialMapTests(unittest.TestCase):
    def test_the_normal_maps_green_is_turned_and_every_level_is_a_unit_normal(self):
        """A glTF normal map's green grows down the picture, the game's up it."""
        with tempfile.TemporaryDirectory() as tmp:
            src = flat_png(os.path.join(tmp, "n.png"), 8, (128 + 60, 128 - 40, 240))
            out = os.path.join(tmp, "out")
            r = run(out, "--normal", src, "--size", "4")
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertEqual(sorted(os.listdir(out)), ["normal_mip00.png", "normal_mip01.png", "normal_mip02.png"])
            for level, width in enumerate((4, 2, 1)):
                w, red, green, blue = pixel(os.path.join(out, "normal_mip%02d.png" % level))
                self.assertEqual(w, width)
                self.assertAlmostEqual(red, 128 + 60, delta=3)
                self.assertAlmostEqual(green, 128 + 40, delta=3)
                x, y, z = (red - 127.5) / 127.5, (green - 127.5) / 127.5, (blue - 127.5) / 127.5
                self.assertAlmostEqual(x * x + y * y + z * z, 1.0, delta=0.06)

    def test_keep_green_leaves_a_map_made_for_the_game_as_it_is(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = flat_png(os.path.join(tmp, "n.png"), 4, (128, 128 - 40, 250))
            out = os.path.join(tmp, "out")
            self.assertEqual(run(out, "--normal", src, "--size", "4", "--keep-green").returncode, 0)
            self.assertAlmostEqual(pixel(os.path.join(out, "normal_mip00.png"))[2], 128 - 40, delta=3)

    def test_the_material_map_has_roughness_in_green_and_occlusion_in_blue(self):
        with tempfile.TemporaryDirectory() as tmp:
            rough = flat_png(os.path.join(tmp, "r.png"), 8, (200, 200, 200))
            ao = flat_png(os.path.join(tmp, "ao.png"), 8, (90, 90, 90))
            out = os.path.join(tmp, "out")
            r = run(out, "--roughness", rough, "--material-size", "4")
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertEqual(sorted(os.listdir(out)), ["material_mip00.png", "material_mip01.png", "material_mip02.png"])
            w, red, green, blue = pixel(os.path.join(out, "material_mip00.png"))
            self.assertEqual((w, red, blue), (4, 0, 255))  # no occlusion map: none
            self.assertAlmostEqual(green, 200, delta=2)
            self.assertEqual(run(out, "--roughness", rough, "--ao", ao, "--red", "0.25", "--material-size", "4").returncode, 0)
            w, red, green, blue = pixel(os.path.join(out, "material_mip01.png"))
            self.assertEqual(w, 2)
            self.assertAlmostEqual(red, 64, delta=2)
            self.assertAlmostEqual(green, 200, delta=2)
            self.assertAlmostEqual(blue, 90, delta=2)

    def test_it_needs_a_map(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertNotEqual(run(os.path.join(tmp, "out")).returncode, 0)


if __name__ == "__main__":
    unittest.main()
