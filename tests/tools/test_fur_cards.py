"""tools/models/blender/fur_cards.py: a coat of fur cards on a fitted creature. Needs Blender (environment variable
BLENDER, or blender on PATH) and the game boar's glb in analysis/models: skipped without."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, os.path.join(ROOT, "tools", "models"))
import gltf_in  # noqa: E402
import rendermodel as rm  # noqa: E402

BLENDER = os.environ.get("BLENDER") or shutil.which("blender")
SCRIPTS = os.path.join(ROOT, "tools", "models", "blender")
BOAR = os.path.join(ROOT, "analysis", "models", "game_enemy_wildboar_01.glb")
BOAR_SKELETON = os.path.join(ROOT, "analysis", "model_survey", "dump", "enemy_wildboar_01.hierarchy.json")
TUFTS = [[0.0, 0.0, 0.5, 0.5], [0.5, 0.25, 0.75, 0.75]]


def blender(script, *args):
    run = subprocess.run([BLENDER, "--background", "--factory-startup", "--python-exit-code", "1", "--python",
                          os.path.join(SCRIPTS, script), "--", *args], capture_output=True, text=True, timeout=900)
    return run.returncode, run.stdout + run.stderr


@unittest.skipUnless(BLENDER and os.path.exists(BOAR) and os.path.exists(BOAR_SKELETON),
                     "needs Blender and the game boar in analysis/")
class FurCardTests(unittest.TestCase):
    def fitted(self, d):
        fig, fitted = os.path.join(d, "animal.glb"), os.path.join(d, "fitted.glb")
        self.assertEqual(blender("make_test_quadruped.py", fig)[0], 0)
        code, log = blender("fit_to_game_skeleton.py", BOAR, fig, fitted, "--material", "1")
        self.assertEqual(code, 0, log[-2000:])
        tufts = os.path.join(d, "tufts.json")
        with open(tufts, "w", encoding="utf-8") as f:
            json.dump(TUFTS, f)
        return fitted, tufts

    def test_cards_cover_the_body_and_leave_the_lower_legs_and_the_face(self):
        with tempfile.TemporaryDirectory() as d:
            fitted, tufts = self.fitted(d)
            out = os.path.join(d, "furred.glb")
            code, log = blender("fur_cards.py", fitted, out, "--tufts", tufts, "--slot", "0", "--count", "200",
                                "--length", "0.2", "--legs", "0.4", "--face", "0.2")
            self.assertEqual(code, 0, log[-3000:])
            self.assertIn("CARDS 200 cards, 800 triangles, slot 0", log)
            skeleton = rm.load_record(BOAR_SKELETON)
            body = gltf_in.load_groups(fitted, skeleton, warn=lambda *a: None)[0]
            groups = {g["material"]: g for g in gltf_in.load_groups(out, skeleton, warn=lambda *a: None)[0]}
            self.assertEqual(sorted(groups), [0, 1])  # the cards on the fur slot, the body where it was
            self.assertEqual(len(groups[1]["lods"][0]), len(body[0]["lods"][0]))
            fur = groups[0]
            self.assertEqual(len(fur["lods"][0]), 800)
            # game space: y up, the creature faces +z
            ys = [p[1] for p in body[0]["pos"]]
            zs = [p[2] for p in body[0]["pos"]]
            height, length = max(ys) - min(ys), max(zs) - min(zs)
            self.assertGreater(min(p[1] for p in fur["pos"]), min(ys) + 0.4 * height - 0.25)  # a card may droop a bit
            self.assertLess(max(p[2] for p in fur["pos"]), max(zs) - 0.2 * length + 0.25)
            for u, v in fur["uv"]:
                self.assertTrue(any(t[0] - 1e-3 <= u <= t[2] + 1e-3 and t[1] - 1e-3 <= v <= t[3] + 1e-3 for t in TUFTS),
                                (u, v))
            # every card moves as one piece: its six vertices share their bones and weights
            skin = fur["skin"]
            first = skin[:6]
            self.assertTrue(all(s == first[0] for s in first))
            self.assertTrue(all(abs(sum(w) - 1.0) < 1e-3 for _, w in skin))

    def test_too_little_skin_stops_it(self):
        with tempfile.TemporaryDirectory() as d:
            fitted, tufts = self.fitted(d)
            out = os.path.join(d, "furred.glb")
            code, log = blender("fur_cards.py", fitted, out, "--tufts", tufts, "--slot", "0", "--legs", "1.5")
            self.assertNotEqual(code, 0)
            self.assertIn("found a place", log)
            self.assertFalse(os.path.exists(out))


if __name__ == "__main__":
    unittest.main()
