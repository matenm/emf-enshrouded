"""tools/models/blender/fit_to_game_skeleton.py on a test animal and the game boar: detail bones (the boar's tail) keep
weight only on the animal's own tail. Needs Blender (environment variable BLENDER, or blender on PATH) and the boar glb
made by gamegltf.py from a model_survey dump (analysis/models/game_enemy_wildboar_01.glb, not in the repository):
skipped without."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
BLENDER_DIR = os.path.join(ROOT, "tools", "models", "blender")
BOAR = os.path.join(ROOT, "analysis", "models", "game_enemy_wildboar_01.glb")
BLENDER = os.environ.get("BLENDER") or shutil.which("blender")
TAIL_ROOT = "j044"  # the boar's tail: j044 < j045 < j046 < j047 under the pelvis
LEG_ROOTS = ("j032", "j037", "j048", "j052")  # the boar's legs (no named bones): front, chest; hind, pelvis
FEET = ("j036", "j041", "j051", "j055")  # the boar's lowest joints: front left/right, hind left/right
BOAR_SKELETON = os.path.join(ROOT, "analysis", "model_survey", "hierarchies", "d3cb046d-2c4a-47da-b8b0-0780d4752ba6.json")
sys.path.insert(0, os.path.join(ROOT, "tools", "models"))
import own_skeleton as osk  # noqa: E402


def blender(script, *args):
    path = script if os.path.isabs(script) else os.path.join(BLENDER_DIR, script)
    out = subprocess.run([BLENDER, "--background", "--factory-startup", "--python-exit-code", "1",
                          "--python", path, "--", *args], capture_output=True, text=True, timeout=600)
    return out.returncode, out.stdout + out.stderr


def weights(fitted):
    """(rump, tail, legs): the mean weight on the boar's tail bones of the rump and of the animal's own tail, and on
    its leg bones of the animal's lower legs."""
    code, log = blender(os.path.join(HERE, "blender_tail_weights.py"), fitted, TAIL_ROOT, *LEG_ROOTS)
    if code != 0:
        raise AssertionError(log[-2000:])
    tail = next(line for line in log.splitlines() if line.startswith("TAILW ")).split()
    legs = next(line for line in log.splitlines() if line.startswith("LEGW ")).split()
    return float(tail[1]), float(tail[2]), float(legs[1])


@unittest.skipUnless(BLENDER and os.path.exists(BOAR), "needs Blender and analysis/models/game_enemy_wildboar_01.glb")
class FitQuadrupedTests(unittest.TestCase):
    def test_the_rump_leaves_the_tail_bones_and_the_tail_keeps_them(self):
        """Weights copied from the nearest point of the game animal's skin put a big rump on the boar's small tail
        (it starts at the boar's rump), so the rump swings with the tail as the boar walks."""
        with tempfile.TemporaryDirectory() as d:
            fig, fitted = os.path.join(d, "animal.glb"), os.path.join(d, "fitted.glb")
            code, log = blender("make_test_quadruped.py", fig)
            self.assertEqual(code, 0, log[-2000:])
            code, log = blender("fit_to_game_skeleton.py", BOAR, fig, fitted)
            self.assertEqual(code, 0, log[-2000:])
            rump, tail, legs = weights(fitted)
            self.assertLess(rump, 0.02)  # the rump: none of the tail's weight
            self.assertGreater(tail, 0.3)  # the animal's own tail: on the tail bones
            self.assertGreater(legs, 0.5)  # the legs: on the boar's leg bones (which have no names), so they walk

    def test_with_tail_hips_the_tail_follows_the_hips(self):
        """A long tail driven by a short tail's animation moves strangely (the aurochs on the boar): --tail hips gives
        the creature's whole tail to the hips."""
        with tempfile.TemporaryDirectory() as d:
            fig, fitted = os.path.join(d, "animal.glb"), os.path.join(d, "fitted.glb")
            self.assertEqual(blender("make_test_quadruped.py", fig)[0], 0)
            code, log = blender("fit_to_game_skeleton.py", BOAR, fig, fitted, "--tail", "hips")
            self.assertEqual(code, 0, log[-2000:])
            rump, tail, legs = weights(fitted)
            self.assertLess(rump, 0.02)
            self.assertLess(tail, 0.02)
            self.assertGreater(legs, 0.5)

    def test_own_proportions_keep_the_creatures_shape_and_move_the_joints_into_it(self):
        """--own-proportions: after the weights, the creature goes back to its own shape (at the game body's height)
        and the sidecar moves every game joint into that shape: the boar's feet joints stand on the animal's feet."""
        with tempfile.TemporaryDirectory() as d:
            fig, fitted, side = (os.path.join(d, n) for n in ("animal.glb", "fitted.glb", "own.json"))
            self.assertEqual(blender("make_test_quadruped.py", fig)[0], 0)
            code, log = blender("fit_to_game_skeleton.py", BOAR, fig, fitted, "--own-proportions", side, "--keep-head")
            self.assertEqual(code, 0, log[-2000:])
            # --keep-head would keep the game's head bones out of the creature's head: dropped, with a note
            self.assertIn("--keep-head is not used with --own-proportions", log)
            src, out = bbox(fig), bbox(fitted)
            for i in (0, 1):  # width and length against height: the animal's own, not the boar's
                self.assertAlmostEqual(out[i] / out[2], src[i] / src[2], delta=0.01 * src[i] / src[2])
            with open(side, encoding="utf-8") as f:
                own = json.load(f)
            self.assertEqual(len(own["joints"]), 58)
            joints = {j["name"]: j["own"] for j in own["joints"]}
            # the root stays where the game skeleton has it (the creature's origin)
            root = next(j for j in own["joints"] if j["name"] == "j000_Root")
            self.assertEqual(root["own"], root["game"])
            ground = own["ground"]
            height = out[2]
            for foot in FEET:
                self.assertLess(joints[foot][2] - ground, 0.15 * height, foot)
            front = [joints[f][1] for f in FEET[:2]]
            hind = [joints[f][1] for f in FEET[2:]]
            # the animal's legs are 1.2 m apart (y -0.6 and 0.6) before the scale to the boar's height
            self.assertAlmostEqual((sum(hind) - sum(front)) / 2 / own["scale"], 1.2, delta=0.15)
            if os.path.exists(BOAR_SKELETON):  # the sidecar's space against the game's: the boar's own rest pose
                with open(BOAR_SKELETON, encoding="utf-8") as f:
                    rest, parents = osk.rest_of(json.load(f))
                world = osk.world(rest, parents)
                for j, joint in enumerate(own["joints"]):
                    for a, b in zip(osk.blender_to_game(joint["game"]), world[j][1]):
                        self.assertAlmostEqual(a, b, delta=1e-4, msg=joint["name"])

    def test_rigid_parts_follow_one_bone(self):
        """--rigid: everything below the named bones (the boar's lower front legs) is on those bones."""
        with tempfile.TemporaryDirectory() as d:
            fig, fitted = os.path.join(d, "animal.glb"), os.path.join(d, "fitted.glb")
            self.assertEqual(blender("make_test_quadruped.py", fig)[0], 0)
            code, log = blender("fit_to_game_skeleton.py", BOAR, fig, fitted, "--rigid", "j034,j039")
            self.assertEqual(code, 0, log[-2000:])
            code, log = blender(os.path.join(HERE, "blender_bone_weights.py"), fitted)
            self.assertEqual(code, 0, log[-2000:])
            w = {parts[1]: float(parts[2]) for parts in (l.split() for l in log.splitlines() if l.startswith("BW "))}
            for below in ("j035", "j036", "j040", "j041"):
                self.assertEqual(w[below], 0.0, below)
            self.assertGreater(w["j034"], 1.0)
            self.assertGreater(w["j039"], 1.0)

    def test_tail_fit_puts_the_tail_bones_along_the_animals_own_tail(self):
        """--tail fit (with --own-proportions): the boar's short tail bones move along the animal's long tail, and the
        tail is weighted along its length (its tip on the last tail bone)."""
        with tempfile.TemporaryDirectory() as d:
            fig, fitted, side = (os.path.join(d, n) for n in ("animal.glb", "fitted.glb", "own.json"))
            self.assertEqual(blender("make_test_quadruped.py", fig)[0], 0)
            code, log = blender("fit_to_game_skeleton.py", BOAR, fig, fitted, "--tail", "fit", "--own-proportions", side)
            self.assertEqual(code, 0, log[-2000:])
            with open(side, encoding="utf-8") as f:
                own = json.load(f)
            joints = {j["name"]: j["own"] for j in own["joints"]}
            k = own["scale"]
            # the animal's tail: 0.7 m from the rump (y 1.2) to y 1.9 at z 1.0 (Blender axes, before the scale k)
            base, tip = joints["j044"], joints["j047"]
            self.assertAlmostEqual((tip[1] - base[1]) / k, 0.7, delta=0.12)
            for name in ("j044", "j045", "j046", "j047"):
                self.assertAlmostEqual((joints[name][2] - own["ground"]) / k, 1.0, delta=0.08, msg=name)
            ys = [joints[n][1] for n in ("j044", "j045", "j046", "j047")]
            self.assertEqual(ys, sorted(ys))
            code, log = blender(os.path.join(HERE, "blender_tail_weights.py"), fitted, TAIL_ROOT)
            self.assertEqual(code, 0, log[-2000:])
            _, rump, tail = next(l for l in log.splitlines() if l.startswith("TAILW ")).split()
            self.assertLess(float(rump), 0.05)
            self.assertGreater(float(tail), 0.8)  # the tail's first 15% blends from the hips

    def test_sides_keep_each_leg_on_its_own_side_of_the_centre_line(self):
        """--sides: a bar between the hind legs (a skeleton's pelvis) hangs on the pelvis on the centre line instead of
        on a leg, no vertex carries weight from a leg on the other side, and the lower legs still walk on the leg
        bones."""
        with tempfile.TemporaryDirectory() as d:
            fig, plain, sided = (os.path.join(d, n) for n in ("animal.glb", "plain.glb", "sided.glb"))
            self.assertEqual(blender("make_test_quadruped.py", fig, "--hind-bar")[0], 0)
            self.assertEqual(blender("fit_to_game_skeleton.py", BOAR, fig, plain)[0], 0)
            code, log = blender("fit_to_game_skeleton.py", BOAR, fig, sided, "--sides")
            self.assertEqual(code, 0, log[-2000:])
            self.assertIn("FIT sides:", log)
            self.assertGreater(sides(plain)[1], 0.05)  # without: the bar's middle hangs on the legs
            cross, mid = sides(sided)
            self.assertLess(cross, 1e-3)
            self.assertLess(mid, 1e-3)
            self.assertGreater(weights(sided)[2], 0.5)


def sides(fitted):
    """(cross, mid): the most weight a vertex carries from the boar's legs on the other side of the centre line, and
    from any leg on the centre line."""
    code, log = blender(os.path.join(HERE, "blender_side_weights.py"), fitted, *LEG_ROOTS)
    if code != 0:
        raise AssertionError(log[-2000:])
    found = {parts[0]: float(parts[1]) for parts in (line.split() for line in log.splitlines())
             if len(parts) == 2 and parts[0] in ("CROSS", "MID")}
    return found["CROSS"], found["MID"]


def bbox(path):
    code, log = blender(os.path.join(HERE, "blender_bbox.py"), path)
    if code != 0:
        raise AssertionError(log[-2000:])
    return [float(v) for v in next(line for line in log.splitlines() if line.startswith("BBOX ")).split()[1:]]


if __name__ == "__main__":
    unittest.main()
