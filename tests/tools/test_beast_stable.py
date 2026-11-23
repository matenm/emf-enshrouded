"""tools/models/packs/beastmaster_stable.lua (the beasts pack's craftable Stable, Large Stable and Training Grounds) against
beast_stable_spec.lua."""
import pathlib
import shutil
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


class BeastStableTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_the_stable_script(self):
        spec = REPO / "tests" / "tools" / "beast_stable_spec.lua"
        script = REPO / "tools" / "models" / "packs" / "beastmaster_stable.lua"
        out = subprocess.run(["lua", str(spec), str(script)], capture_output=True, text=True, timeout=60)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)

    def test_the_beasts_pack_runs_it(self):
        import json
        pack = json.loads((REPO / "tools" / "models" / "packs" / "beastmaster_beasts.json").read_text(encoding="utf-8"))
        self.assertIn("beastmaster_stable.lua", pack.get("content_scripts", []))

    def test_the_pack_builds_a_prop_for_each_stable_model_the_script_names(self):
        import json
        import re
        pack = json.loads((REPO / "tools" / "models" / "packs" / "beastmaster_beasts.json").read_text(encoding="utf-8"))
        text = (REPO / "tools" / "models" / "packs" / "beastmaster_stable.lua").read_text(encoding="utf-8")
        named = set(re.findall(r'model = "([0-9a-f-]{36})"', text))
        props = {p["model_guid"]: p for p in pack["props"]}
        self.assertEqual(named, {"1a49170a-b3d2-498d-9062-8946f6469613", "5879f304-0050-4ecb-96f4-5cc0e66155d9",
                                 "bb3549a0-5088-4c3b-8057-3a7e48a59ad6"})
        self.assertEqual(set(props), named)
        small = props["1a49170a-b3d2-498d-9062-8946f6469613"]
        large = props["5879f304-0050-4ecb-96f4-5cc0e66155d9"]
        self.assertEqual(large["name"], "beastmaster_large_stable")
        self.assertAlmostEqual(large["width"], small["width"] * 1.6)  # the script's colliders are 1.6x the Stable's
        self.assertEqual(large["materials"], small["materials"])
        training = props["bb3549a0-5088-4c3b-8057-3a7e48a59ad6"]
        self.assertEqual(training["name"], "beastmaster_training_grounds")
        self.assertAlmostEqual(training["width"], 5.0)  # the script's boxes are for a 5 m platform
        self.assertEqual(training["materials"], small["materials"])  # the game's thatch and shed planks

    def test_the_script_never_touches_package(self):
        # EML's Lua has require but no global package table (it stopped every creature with clips once)
        text = (REPO / "tools" / "models" / "packs" / "beastmaster_stable.lua").read_text(encoding="utf-8")
        self.assertNotIn("package", text)


if __name__ == "__main__":
    unittest.main()
