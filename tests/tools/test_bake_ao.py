"""tools/models/blender/bake_ao.py: a model's ambient occlusion on its own UVs. Needs Blender (environment variable
BLENDER, or blender on PATH): skipped without."""
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
SCRIPT = os.path.join(ROOT, "tools", "models", "blender", "bake_ao.py")

# a floor with a box standing on it, unwrapped, as a glb (uvs=False: without UVs)
MAKE = """
import bpy, sys
out, uvs = sys.argv[-2], sys.argv[-1] == "uvs"
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
bpy.ops.mesh.primitive_plane_add(size=4)
bpy.ops.mesh.primitive_cube_add(size=1, location=(0, 0, 0.5))
for o in bpy.data.objects:
    o.select_set(True)
bpy.context.view_layer.objects.active = bpy.data.objects[0]
bpy.ops.object.join()
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.mesh.select_all(action="SELECT")
bpy.ops.uv.smart_project()
bpy.ops.object.mode_set(mode="OBJECT")
if not uvs:
    mesh = bpy.context.view_layer.objects.active.data
    while mesh.uv_layers:
        mesh.uv_layers.remove(mesh.uv_layers[0])
bpy.ops.export_scene.gltf(filepath=out)
"""


def blender(*args):
    return subprocess.run([BLENDER, "--background", "--factory-startup", "--python-exit-code", "1", *args],
                          capture_output=True, text=True, timeout=600)


@unittest.skipUnless(BLENDER, "needs Blender")
class BakeAoTests(unittest.TestCase):
    def model(self, tmp, uvs):
        make = os.path.join(tmp, "make.py")
        with open(make, "w", encoding="utf-8") as f:
            f.write(MAKE)
        glb = os.path.join(tmp, "box.glb")
        r = blender("--python", make, "--", glb, "uvs" if uvs else "none")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return glb

    def test_open_faces_are_white_and_the_foot_of_the_box_is_dark(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "ao.png")
            r = blender("--python", SCRIPT, "--", self.model(tmp, True), out, "--size", "64", "--samples", "16",
                        "--distance", "1.0")
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            w, h, bpp, rows = png_crop.read_png(out)
            self.assertEqual((w, h), (64, 64))
            values = [row[x * bpp] for row in rows for x in range(w)]
            self.assertGreater(sum(v > 240 for v in values), 200)  # the box's top and the open floor
            self.assertGreater(sum(v < 150 for v in values), 20)   # the floor under and beside the box

    def test_a_mesh_without_uvs_stops_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "ao.png")
            r = blender("--python", SCRIPT, "--", self.model(tmp, False), out, "--size", "64")
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("no UVs", r.stdout + r.stderr)
            self.assertFalse(os.path.exists(out))


if __name__ == "__main__":
    unittest.main()
