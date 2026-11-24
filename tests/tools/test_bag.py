"""mods/beastmaster/bag.lua (the pack beast's bag) against bag_spec.lua."""
import pathlib
import shutil
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


class BagTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_the_bag(self):
        spec = REPO / "tests" / "tools" / "bag_spec.lua"
        bag = REPO / "mods" / "beastmaster" / "bag.lua"
        out = subprocess.run(["lua", str(spec), str(bag)], capture_output=True, text=True, timeout=60,
                             stdin=subprocess.DEVNULL)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)


if __name__ == "__main__":
    unittest.main()
