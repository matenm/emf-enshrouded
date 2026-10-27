"""Where tools/gen_headers.py and the tools/re scripts read the game's files: a path on the command line, else the game
folder in EMF_GAME_DIR, else a plain error (never a folder of one PC).
Run: python -m unittest discover -s tests/tools"""

import os
import sys
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, os.path.join(REPO, "tools", "re"))
import gamefiles  # noqa: E402
import gen_headers  # noqa: E402


def without_game_dir():
    env = dict(os.environ)
    env.pop("EMF_GAME_DIR", None)
    return env


class GenHeadersTypes(unittest.TestCase):
    def test_types_json_in_emf_game_dir(self):
        with mock.patch.dict(os.environ, {"EMF_GAME_DIR": r"D:\Games\Enshrouded"}):
            self.assertEqual(gen_headers.default_types(), os.path.join(r"D:\Games\Enshrouded", ".cache", "types.json"))

    def test_no_path_and_no_emf_game_dir_says_what_to_pass(self):
        with mock.patch.dict(os.environ, without_game_dir(), clear=True):
            with self.assertRaises(SystemExit) as e:
                gen_headers.default_types()
        self.assertIn("types.json", str(e.exception.code))
        self.assertIn("EMF_GAME_DIR", str(e.exception.code))


class ReGameFiles(unittest.TestCase):
    def test_exe_flag_wins_and_leaves_the_scripts_own_arguments(self):
        argv = ["sig.py", "--exe", r"C:\x\enshrouded.exe", "0x140456790"]
        with mock.patch.object(sys, "argv", argv), mock.patch.dict(os.environ, {"EMF_GAME_DIR": r"D:\other"}):
            self.assertEqual(gamefiles.exe_path(), r"C:\x\enshrouded.exe")
            self.assertEqual(sys.argv, ["sig.py", "0x140456790"])

    def test_flag_with_equals(self):
        argv = ["show2.py", r"--types=C:\x\types.json", "keen::ecs::Health"]
        with mock.patch.object(sys, "argv", argv):
            self.assertEqual(gamefiles.types_path(), r"C:\x\types.json")
            self.assertEqual(sys.argv, ["show2.py", "keen::ecs::Health"])

    def test_emf_game_dir(self):
        with mock.patch.object(sys, "argv", ["pe.py"]), \
                mock.patch.dict(os.environ, {"EMF_GAME_DIR": r"D:\Games\Enshrouded"}):
            self.assertEqual(gamefiles.exe_path(), os.path.join(r"D:\Games\Enshrouded", "enshrouded.exe"))
            self.assertEqual(gamefiles.types_path(), os.path.join(r"D:\Games\Enshrouded", ".cache", "types.json"))

    def test_neither_is_a_plain_error(self):
        with mock.patch.object(sys, "argv", ["anchor.py", "text"]), \
                mock.patch.dict(os.environ, without_game_dir(), clear=True):
            with self.assertRaises(SystemExit) as e:
                gamefiles.exe_path()
            self.assertIn("--exe", str(e.exception.code))
            self.assertIn("EMF_GAME_DIR", str(e.exception.code))
            with self.assertRaises(SystemExit) as e:
                gamefiles.types_path()
            self.assertIn("--types", str(e.exception.code))


class NoMachinePaths(unittest.TestCase):
    def test_the_tools_name_no_folder_of_one_pc(self):
        files = [os.path.join(REPO, "tools", "gen_headers.py")]
        re_dir = os.path.join(REPO, "tools", "re")
        files += [os.path.join(re_dir, f) for f in os.listdir(re_dir) if f.endswith(".py")]
        for path in files:
            with open(path, encoding="utf-8") as f:
                text = f.read()
            self.assertNotIn("SteamLibrary", text, path)


if __name__ == "__main__":
    unittest.main()
