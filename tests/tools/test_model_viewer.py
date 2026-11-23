"""tools/models/model_viewer/main.lua (the developer mod that shows creature models in the game) against
model_viewer_spec.lua."""
import pathlib
import shutil
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


class ModelViewerTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_the_order_commands(self):
        spec = REPO / "tests" / "tools" / "model_viewer_spec.lua"
        script = REPO / "tools" / "models" / "model_viewer" / "main.lua"
        out = subprocess.run(["lua", str(spec), str(script)], capture_output=True, text=True, timeout=60)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)


if __name__ == "__main__":
    unittest.main()
