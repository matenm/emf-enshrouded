"""tools/models/blender/fit_humanoid.py on a test figure: every vertex weighted (at most 4), feet on the ground, the
game body's height. Needs Blender (environment variable BLENDER, or blender on PATH) and the mage skeleton glb made by
gamegltf.py from a model_survey dump (analysis/models/game_skeleton_mage.glb, not in the repository): skipped without."""
import json
import os
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
BLENDER_DIR = os.path.join(ROOT, "tools", "models", "blender")
GAME = os.path.join(ROOT, "analysis", "models", "game_skeleton_mage.glb")
BLENDER = os.environ.get("BLENDER") or shutil.which("blender")


def blender(script, *args):
    out = subprocess.run([BLENDER, "--background", "--factory-startup", "--python-exit-code", "1",
                          "--python", os.path.join(BLENDER_DIR, script), "--", *args],
                         capture_output=True, text=True, timeout=600)
    return out.returncode, out.stdout + out.stderr


@unittest.skipUnless(BLENDER and os.path.exists(GAME), "needs Blender and analysis/models/game_skeleton_mage.glb")
class FitHumanoidTests(unittest.TestCase):
    def test_a_test_figure_is_fitted_weighted_and_sized(self):
        with tempfile.TemporaryDirectory() as d:
            fig, fitted = os.path.join(d, "figure.glb"), os.path.join(d, "fitted.glb")
            code, log = blender("make_test_humanoid.py", fig)
            self.assertEqual(code, 0, log[-2000:])
            code, log = blender("fit_humanoid.py", GAME, fig, fitted)
            self.assertEqual(code, 0, log[-2000:])
            code, log = blender("check_glb.py", fitted, os.path.join(d, "check"))
            self.assertEqual(code, 0, log[-2000:])
            line = next(l for l in log.splitlines() if l.startswith("CHECK "))
            summary = json.loads(line[len("CHECK "):])
            self.assertEqual(len(summary["meshes"]), 1)
            mesh = summary["meshes"][0]
            self.assertTrue(mesh["armature_mod"])
            self.assertEqual(mesh["unweighted"], 0)
            self.assertLessEqual(mesh["max_influences"], 4)
            lo, hi = summary["rest_bounds"]
            self.assertAlmostEqual(lo[2], 0.0, delta=0.05)   # feet on the ground
            self.assertTrue(2.1 <= hi[2] <= 2.5, hi)          # the mage skeleton's height (head bone 2.1 m)

    def test_a_messy_figure_still_gets_smooth_weights(self):
        """Open cloth, separate overlapping parts and doubled faces (as Meshy models have) defeat Blender's heat
        weights on the mesh itself: the fitter must still bind smoothly (blended weights, not one bone per vertex)."""
        with tempfile.TemporaryDirectory() as d:
            fig, fitted = os.path.join(d, "figure.glb"), os.path.join(d, "fitted.glb")
            code, log = blender("make_test_humanoid.py", fig, "--messy")
            self.assertEqual(code, 0, log[-2000:])
            code, log = blender("fit_humanoid.py", GAME, fig, fitted)
            self.assertEqual(code, 0, log[-2000:])
            fit = next(l for l in log.splitlines() if l.startswith("FIT_HUMANOID"))
            verts = int(fit.split("verts ")[1].split(",")[0])
            fixes = int(fit.split("nearest-bone fixes ")[1].split()[0])
            self.assertLess(fixes, verts * 0.05, fit)  # nearly every vertex got blended heat weights
            code, log = blender("check_glb.py", fitted, os.path.join(d, "check"))
            summary = json.loads(next(l for l in log.splitlines() if l.startswith("CHECK "))[len("CHECK "):])
            self.assertEqual(summary["meshes"][0]["unweighted"], 0)
            self.assertGreaterEqual(summary["meshes"][0]["max_influences"], 2)


if __name__ == "__main__":
    unittest.main()
