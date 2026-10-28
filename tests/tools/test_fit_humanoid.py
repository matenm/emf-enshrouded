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

    def legs(self, fitted, skirt=0.06):
        code, log = blender(os.path.join(HERE, "blender_legs.py"), fitted, str(skirt))
        self.assertEqual(code, 0, log[-2000:])
        out = {}
        for line in log.splitlines():
            if line.startswith("LEGS "):
                _, side, _, offset, _, on_shin, _, seams = line.split()
                out[side] = (float(offset), int(on_shin), int(seams))
            elif line.startswith("HELPERS "):
                out["helpers"] = float(line.split()[1])
        return out

    def test_legs_are_bound_to_the_leg_bones_not_to_helper_bones(self):
        """The game's bodies hang helper bones from the pelvis at knee height (the mage's j074/j075, the Vukah's
        j100), which do not bend with the knee: a creature's legs bound to them poke out at the knees when the game's
        walk bends them. Only the main chains may carry weight."""
        with tempfile.TemporaryDirectory() as d:
            fig, fitted = os.path.join(d, "figure.glb"), os.path.join(d, "fitted.glb")
            self.assertEqual(blender("make_test_humanoid.py", fig, "--wide-legs")[0], 0)
            code, log = blender("fit_humanoid.py", GAME, fig, fitted)
            self.assertEqual(code, 0, log[-2000:])
            legs = self.legs(fitted)
            self.assertLess(legs["helpers"], 0.5, legs)
            for side in ("L", "R"):
                # each leg on its own leg bones: this figure's legs stand 7 cm wider than the mage's; a leg bound to
                # the other leg's bones has its knee centroid about 0.46 m from the knee bone
                self.assertLess(legs[side][0], 0.2, legs)

    def test_a_skeleton_without_a_limb_slot_keeps_every_skinned_bone_with_a_warning(self):
        """Slot names differ between skeletons; when a main chain cannot be built (here the left foot's slot name is
        gone), leaving its bones out would bind that limb to the body. The fitter then keeps every bone the game body
        is skinned to, and says so."""
        with tempfile.TemporaryDirectory() as d:
            game, fig, fitted = (os.path.join(d, n) for n in ("game.glb", "figure.glb", "fitted.glb"))
            code, log = blender(os.path.join(HERE, "blender_rename_bone.py"), GAME, game, "_FootL", "")
            self.assertEqual(code, 0, log[-2000:])
            self.assertEqual(blender("make_test_humanoid.py", fig)[0], 0)
            code, log = blender("fit_humanoid.py", game, fig, fitted)
            self.assertEqual(code, 0, log[-2000:])
            self.assertIn("main chains incomplete", log)
            fit = next(l for l in log.splitlines() if l.startswith("FIT_HUMANOID"))
            self.assertIn("helper bones left out 0", fit)

    def test_skirt_makes_a_robe_below_the_knees_follow_the_thighs(self):
        """A robe hanging below the knees bound to the shins folds into a point when a knee bends. --skirt F gives
        the robe (vertices farther than F x height from the shin bones) to the thigh and the pelvis instead, gradually
        up to 3F, without a seam a bent knee would tear open."""
        with tempfile.TemporaryDirectory() as d:
            fig, plain, skirted = (os.path.join(d, n) for n in ("figure.glb", "plain.glb", "skirted.glb"))
            self.assertEqual(blender("make_test_humanoid.py", fig, "--messy")[0], 0)
            self.assertEqual(blender("fit_humanoid.py", GAME, fig, plain)[0], 0)
            code, log = blender("fit_humanoid.py", GAME, fig, skirted, "--skirt", "0.06")
            self.assertEqual(code, 0, log[-2000:])
            # beyond the hand-over (3 x 0.06 of the height from the shins) the robe carries no shin weight
            before, after = self.legs(plain, 0.18), self.legs(skirted, 0.18)
            self.assertGreater(before["L"][1] + before["R"][1], 0, before)  # the robe is on the shins
            self.assertEqual(after["L"][1] + after["R"][1], 0, after)
            for side in ("L", "R"):  # a gradual hand-over, no seam a bent knee would tear open
                self.assertLessEqual(after[side][2], before[side][2] + 3, (before, after))
            code, log = blender("check_glb.py", skirted, os.path.join(d, "check"))
            summary = json.loads(next(l for l in log.splitlines() if l.startswith("CHECK "))[len("CHECK "):])
            self.assertEqual(summary["meshes"][0]["unweighted"], 0)
            self.assertLessEqual(summary["meshes"][0]["max_influences"], 4)


if __name__ == "__main__":
    unittest.main()
