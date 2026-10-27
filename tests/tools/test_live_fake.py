"""The scenario runner and the MCP server as programs, over the real pipe, against emf_bridge_fake.exe.
scripts/test_tools.ps1 starts the fake game and sets EMF_FAKE_GAME to its folder (with emf\\mods\\demo\\main.lua);
without it these tests are skipped."""

import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SCENARIO = os.path.join(REPO, "tools", "bridge", "emf_scenario.py")
MCP = os.path.join(REPO, "tools", "mcp", "emf_mcp.py")
FAKE = os.environ.get("EMF_FAKE_GAME")


@unittest.skipUnless(FAKE, "needs EMF_FAKE_GAME (scripts/test_tools.ps1 starts emf_bridge_fake.exe)")
class LiveFakeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="emf_live_fake_")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)  # also when setUp fails after this
        # runs without --out keep their folder in this test's own folder, not in %TEMP%\emf_scenario
        self.root = os.path.join(self.tmp, "runs")
        self.env = dict(os.environ, EMF_SCENARIO_ROOT=self.root)
        self.demo = os.path.join(FAKE, "emf", "mods", "demo", "main.lua")
        with open(self.demo, "rb") as f:
            self.demo_before = f.read()

    def run_scenario(self, *args):
        p = subprocess.run([sys.executable, "-I", SCENARIO] + list(args), capture_output=True, text=True,
                           timeout=120, env=self.env)
        return p.returncode, p.stdout, p.stderr

    def test_load_world_over_the_pipe(self):
        sys.path.insert(0, os.path.join(REPO, "tools", "bridge"))
        import emf_bridge
        with emf_bridge.Bridge(game=FAKE) as b:
            r = b.load_world(1, 1, "Character 2")
            self.assertEqual((r["world"], r["character_name"], r["slot"]), ("My World 1", "Character 2", 1))
            with self.assertRaises(emf_bridge.LoadWorldError) as e:
                b.load_world(9, 1)
            self.assertIn("LOCAL world slot 9 does not exist", str(e.exception))
            self.assertIn("nothing started", str(e.exception))
            with self.assertRaises(emf_bridge.LoadWorldError) as e:
                b.load_world(11, 1)
            self.assertEqual(e.exception.code, "bad_request")

    def test_the_character_rule_over_the_pipe(self):
        """state.character from the fake (LOCAL by default; steam or unknown through emf\\fake_character.txt), and the
        scenario runner and the MCP server refusing world-changing work for steam and unknown."""
        sys.path.insert(0, os.path.join(REPO, "tools", "bridge"))
        import emf_bridge
        marker = os.path.join(FAKE, "emf", "fake_character.txt")

        def remove_marker():
            if os.path.exists(marker):
                os.remove(marker)
        self.addCleanup(remove_marker)
        with emf_bridge.Bridge(game=FAKE) as b:
            b.wait("world", timeout_ms=10000)
            self.assertEqual(b.state()["character"], {"storage": "local", "slot": 1, "name": "Character 2"})
        console = os.path.join(self.tmp, "console.json")
        with open(console, "w", encoding="utf-8") as f:
            json.dump({"name": "console", "steps": [{"do": "console", "text": "near 10"}]}, f)
        code, out, err = self.run_scenario(console, "--game", FAKE, "--out", os.path.join(self.tmp, "local"))
        self.assertEqual(code, 0, out + err)
        call = [{"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                 "params": {"name": "emf_console", "arguments": {"text": "near 10"}}},
                {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "emf_state", "arguments": {}}}]
        for kind, words in (("steam", "uses a Steam character"), ("unknown", "can't tell which character runs")):
            with self.subTest(kind=kind):
                with open(marker, "w", encoding="ascii") as f:
                    f.write(kind)
                with emf_bridge.Bridge(game=FAKE) as b:
                    self.assertEqual(b.state()["character"]["storage"], kind)
                code, out, err = self.run_scenario(console, "--game", FAKE, "--out", os.path.join(self.tmp, kind))
                self.assertEqual(code, 1, out + err)
                self.assertIn(words, out)
                p = subprocess.run([sys.executable, "-I", MCP, "--game", FAKE, "--any-game"],
                                   input="".join(json.dumps(m) + "\n" for m in call).encode("utf-8"),
                                   capture_output=True, timeout=60, env=self.env)
                replies = {r["id"]: r for r in (json.loads(l) for l in p.stdout.decode("utf-8").splitlines())}
                self.assertTrue(replies[1]["result"]["isError"])
                self.assertIn(words, replies[1]["result"]["content"][0]["text"])
                self.assertFalse(replies[2]["result"]["isError"])  # reading stays allowed

    def test_scenario_runner_passes_every_step_kind(self):
        report = os.path.join(self.tmp, "report.json")
        code, out, err = self.run_scenario(os.path.join(HERE, "fake_game.json"), "--game", FAKE, "--out",
                                           os.path.join(self.tmp, "out"), "--json", report)
        self.assertEqual(code, 0, out + err)
        with open(report, encoding="utf-8") as f:
            r = json.load(f)
        self.assertTrue(r["ok"])
        self.assertEqual(r["failed"], 0)
        shot = [s for s in r["steps"] if s["do"] == "screenshot"][0]["data"]["copy"]
        with open(shot, "rb") as f:
            self.assertEqual(f.read(8), b"\x89PNG\r\n\x1a\n")
        with open(self.demo, "rb") as f:
            self.assertEqual(f.read(), self.demo_before, "edit_file put the file back")
        self.assertFalse(os.path.exists(os.path.join(FAKE, "emf", "mods", "demo", "extra.lua")))

    def test_scenario_runner_failure_and_no_connection(self):
        bad = os.path.join(self.tmp, "bad.json")
        with open(bad, "w", encoding="utf-8") as f:
            json.dump({"name": "bad", "steps": [
                {"do": "edit_file", "path": "emf/mods/demo/main.lua", "find": "hello", "replace": "x"},
                {"do": "console", "text": "near 10", "expect": "4 entities"}]}, f)
        code, out, err = self.run_scenario(bad, "--game", FAKE, "--out", os.path.join(self.tmp, "out"))
        self.assertEqual(code, 1, out + err)
        self.assertIn("FAIL", out)
        with open(self.demo, "rb") as f:
            self.assertEqual(f.read(), self.demo_before)
        empty = os.path.join(self.tmp, "nogame")
        os.makedirs(os.path.join(empty, "emf"))
        code, out, err = self.run_scenario(bad, "--game", empty)
        self.assertEqual(code, 2, out + err)
        self.assertIn("bridge.txt", err)

    def test_mcp_server_over_stdio(self):
        scen_dir = os.path.join(FAKE, "emf", "scenarios")
        os.makedirs(scen_dir, exist_ok=True)
        shutil.copy(os.path.join(HERE, "fake_game.json"), os.path.join(scen_dir, "fake_game.json"))
        msgs = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize",
             "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "t", "version": "1"}}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "emf_hello", "arguments": {}}},
            {"jsonrpc": "2.0", "id": 9, "method": "tools/call",
             "params": {"name": "emf_wait", "arguments": {"for": "player", "timeout_s": 10}}},
            {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
             "params": {"name": "emf_state", "arguments": {"path": "player.position.x"}}},
            {"jsonrpc": "2.0", "id": 5, "method": "tools/call",
             "params": {"name": "emf_console", "arguments": {"text": "near 10"}}},
            {"jsonrpc": "2.0", "id": 6, "method": "tools/call",
             "params": {"name": "emf_screenshot", "arguments": {"max_width": 32}}},
            {"jsonrpc": "2.0", "id": 7, "method": "tools/call",
             "params": {"name": "emf_run_scenario", "arguments": {"path": os.path.join(scen_dir, "fake_game.json")}}},
            {"jsonrpc": "2.0", "id": 8, "method": "tools/call",
             "params": {"name": "emf_console", "arguments": {"text": "nosuch"}}},
        ]
        data = "".join(json.dumps(m) + "\n" for m in msgs).encode("utf-8")
        p = subprocess.run([sys.executable, "-I", MCP, "--game", FAKE, "--any-game"], input=data, capture_output=True,
                           timeout=180, env=self.env)
        self.assertEqual(p.returncode, 0, p.stderr.decode("utf-8", "replace"))
        replies = {r["id"]: r for r in (json.loads(l) for l in p.stdout.decode("utf-8").splitlines())}
        self.assertEqual(sorted(replies), [1, 2, 3, 4, 5, 6, 7, 8, 9])
        self.assertFalse(replies[9]["result"]["isError"])
        self.assertEqual(replies[1]["result"]["serverInfo"]["name"], "emf")
        self.assertEqual(len(replies[2]["result"]["tools"]), 10)
        self.assertIn("EMF fake", replies[3]["result"]["content"][0]["text"])
        self.assertEqual(replies[4]["result"]["content"][0]["text"], "100.5")
        self.assertIn("3 entities within 10 m", replies[5]["result"]["content"][0]["text"])
        image = replies[6]["result"]["content"][0]
        self.assertEqual(image["type"], "image")
        self.assertEqual(base64.b64decode(image["data"])[:8], b"\x89PNG\r\n\x1a\n")
        self.assertFalse(replies[7]["result"]["isError"], replies[7]["result"]["content"][0]["text"])
        self.assertIn("21 passed", replies[7]["result"]["content"][0]["text"])
        self.assertTrue(os.listdir(self.root), "the scenario's run folder is under EMF_SCENARIO_ROOT")
        self.assertTrue(replies[8]["result"]["isError"])
        with open(self.demo, "rb") as f:
            self.assertEqual(f.read(), self.demo_before)


if __name__ == "__main__":
    unittest.main()
