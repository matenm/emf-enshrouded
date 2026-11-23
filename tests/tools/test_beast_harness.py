"""tools/models/packs/beastmaster_harness.lua (the wilds pack's craftable Llama Harness and Camel Saddle) against
beast_harness_spec.lua."""
import json
import pathlib
import shutil
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "models" / "packs" / "beastmaster_harness.lua"


class BeastHarnessTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_the_harness_script(self):
        spec = REPO / "tests" / "tools" / "beast_harness_spec.lua"
        out = subprocess.run(["lua", str(spec), str(SCRIPT)], capture_output=True, text=True, timeout=60)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)

    def test_the_wilds_pack_runs_it(self):
        pack = json.loads((REPO / "tools" / "models" / "packs" / "beastmaster_wilds.json").read_text(encoding="utf-8"))
        self.assertIn("beastmaster_harness.lua", pack.get("content_scripts", []))

    def test_the_script_never_touches_package(self):
        # EML's Lua has require but no global package table
        self.assertNotIn("package", SCRIPT.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
