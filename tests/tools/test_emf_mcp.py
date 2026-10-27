"""Unit tests of tools/mcp/emf_mcp.py (the MCP server over the debug bridge) against a fake bridge.
Run: python -m unittest discover -s tests/tools"""

import base64
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from fake_bridge import PNG_1X1, REPO, ConnectError, FakeBridge  # noqa: E402

sys.path.insert(0, os.path.join(REPO, "tools", "mcp"))
import emf_mcp  # noqa: E402

TOOL_NAMES = ["emf_hello", "emf_console", "emf_state", "emf_mods", "emf_log", "emf_wait", "emf_screenshot",
              "emf_mod_key", "emf_run_scenario", "emf_load_world"]


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="emf_mcp_test_")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)  # also when setUp fails after this
        self.game = os.path.join(self.tmp, "game")
        os.makedirs(os.path.join(self.game, "emf", "mods"))
        self.repo = os.path.join(self.tmp, "repo")
        os.makedirs(os.path.join(self.repo, "tests", "game"))
        self.bridge = FakeBridge(self.game)
        self.opened = []
        # the game game_session.ps1 started: the fake's pid 4242, started at 1000.0
        self.session = os.path.join(self.tmp, "session.json")
        self.write_session({"owner": "test", "game_pid": 4242, "game_start_epoch": 1000.0, "game": self.game})
        self.started = {4242: 1000.5}
        self.server = self.make_server()
        self.next_id = 0

    def factory(self, game, pid):
        self.opened.append((game, pid))
        return self.bridge

    def make_server(self, **kw):
        args = dict(bridge_factory=self.factory, game=self.game, repo=self.repo, session_file=self.session,
                    process_start=lambda pid: self.started.get(pid))
        args.update(kw)
        return emf_mcp.Server(**args)

    def write_session(self, data):
        with open(self.session, "w", encoding="utf-8") as f:
            json.dump(data, f)

    def rpc(self, method, params=None):
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        reply = self.server.handle(msg)
        self.assertEqual(reply["jsonrpc"], "2.0")
        self.assertEqual(reply["id"], self.next_id)
        return reply

    def call(self, name, arguments=None):
        reply = self.rpc("tools/call", {"name": name, "arguments": arguments or {}})
        self.assertIn("result", reply, reply)
        return reply["result"]

    def text_of(self, result):
        return "\n".join(c["text"] for c in result["content"] if c["type"] == "text")


class ProtocolTests(Base):
    def test_initialize(self):
        r = self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "test", "version": "1"}})["result"]
        self.assertEqual(r["protocolVersion"], "2025-06-18")
        self.assertIn("tools", r["capabilities"])
        self.assertEqual(r["serverInfo"]["name"], "emf")
        self.assertIn("LOCAL", r["instructions"])
        r = self.rpc("initialize", {"protocolVersion": "2024-11-05"})["result"]
        self.assertEqual(r["protocolVersion"], "2024-11-05")
        r = self.rpc("initialize", {"protocolVersion": "1999-01-01"})["result"]
        self.assertEqual(r["protocolVersion"], emf_mcp.PROTOCOL_VERSIONS[0])
        # 2025-03-26 has JSON-RPC batches, which this server does not take: it answers with a version without them
        r = self.rpc("initialize", {"protocolVersion": "2025-03-26"})["result"]
        self.assertEqual(r["protocolVersion"], "2025-06-18")

    def test_notifications_get_no_reply(self):
        self.assertIsNone(self.server.handle({"jsonrpc": "2.0", "method": "notifications/initialized"}))
        self.assertIsNone(self.server.handle({"jsonrpc": "2.0", "method": "notifications/cancelled",
                                              "params": {"requestId": 3}}))

    def test_ping(self):
        self.assertEqual(self.rpc("ping")["result"], {})

    def test_unknown_method(self):
        r = self.rpc("resources/list")
        self.assertEqual(r["error"]["code"], -32601)

    def test_bad_requests(self):
        r = self.server.handle({"jsonrpc": "2.0", "id": 1})
        self.assertEqual(r["error"]["code"], -32600)
        r = self.server.handle([{"jsonrpc": "2.0", "id": 1, "method": "ping"}])
        self.assertEqual(r["error"]["code"], -32600)
        r = self.server.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": "x"})
        self.assertEqual(r["error"]["code"], -32602)

    def test_tools_list(self):
        tools = self.rpc("tools/list")["result"]["tools"]
        self.assertEqual([t["name"] for t in tools], TOOL_NAMES)
        for t in tools:
            with self.subTest(tool=t["name"]):
                self.assertGreater(len(t["description"]), 40)
                schema = t["inputSchema"]
                self.assertEqual(schema["type"], "object")
                self.assertFalse(schema.get("additionalProperties", True))
                for prop in schema.get("properties", {}).values():
                    self.assertIn("description", prop)
                    self.assertIn("type", prop)
                for req in schema.get("required", []):
                    self.assertIn(req, schema["properties"])

    def test_unknown_tool_is_a_protocol_error(self):
        r = self.rpc("tools/call", {"name": "emf_fly", "arguments": {}})
        self.assertEqual(r["error"]["code"], -32602)
        self.assertIn("emf_fly", r["error"]["message"])

    def test_serve_reads_and_writes_lines(self):
        lines = [
            json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}}),
            json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}),
            "",
            "{not json",
            json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                        "params": {"name": "emf_hello", "arguments": {}}}),
        ]
        stdin = io.BytesIO(("\n".join(lines) + "\n").encode("utf-8"))
        stdout = io.BytesIO()
        self.server.serve(stdin, stdout)
        out = stdout.getvalue().decode("utf-8")
        self.assertNotIn("\r\n", out)
        replies = [json.loads(l) for l in out.splitlines()]
        self.assertEqual([r.get("id") for r in replies], [1, None, 2])
        self.assertEqual(replies[1]["error"]["code"], -32700)
        self.assertIn("EMF 0.3.0", replies[2]["result"]["content"][0]["text"])


class EncodingTests(Base):
    def test_a_lone_surrogate_in_a_reply_does_not_stop_the_server(self):
        self.bridge.console_replies["odd"] = ["half a pair: \ud800 here"]
        lines = [json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                             "params": {"name": "emf_console", "arguments": {"text": "odd"}}}),
                 json.dumps({"jsonrpc": "2.0", "id": 2, "method": "ping"})]
        stdout = io.BytesIO()
        self.server.serve(io.BytesIO(("\n".join(lines) + "\n").encode("utf-8")), stdout)
        replies = [json.loads(l) for l in stdout.getvalue().decode("utf-8").splitlines()]
        self.assertEqual([r["id"] for r in replies], [1, 2])
        self.assertIn("half a pair", replies[0]["result"]["content"][0]["text"])


class ToolTests(Base):
    def test_hello(self):
        r = self.call("emf_hello")
        self.assertFalse(r["isError"])
        self.assertIn("EMF 0.3.0", self.text_of(r))
        self.assertIn("1273043", self.text_of(r))
        self.assertTrue(self.bridge.closed)  # one connection per call

    def test_no_game_is_a_tool_error_in_plain_words(self):
        def no_game(game, pid):
            raise ConnectError("the game is not running (or [debug] bridge=1 is not set in emf\\emf_core.ini)")
        server = emf_mcp.Server(bridge_factory=no_game, repo=self.repo)
        reply = server.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                               "params": {"name": "emf_state", "arguments": {}}})
        r = reply["result"]
        self.assertTrue(r["isError"])
        self.assertIn("not running", r["content"][0]["text"])
        self.assertIn("game_session", r["content"][0]["text"])  # says what to do

    def test_unexpected_exception_is_a_tool_error(self):
        def boom(f):
            raise RuntimeError("kaputt")
        self.bridge.handlers["state"] = boom
        with contextlib.redirect_stderr(io.StringIO()):  # the server prints the traceback to stderr
            r = self.call("emf_state")
        self.assertTrue(r["isError"])
        self.assertIn("kaputt", self.text_of(r))

    def test_bad_arguments_are_tool_errors(self):
        r = self.call("emf_console", {})
        self.assertTrue(r["isError"])
        self.assertIn("text", self.text_of(r))
        r = self.call("emf_console", {"text": 5})
        self.assertTrue(r["isError"])
        r = self.call("emf_log", {"limit": 0})
        self.assertTrue(r["isError"])
        r = self.call("emf_wait", {"for": "lunch"})
        self.assertTrue(r["isError"])
        r = self.call("emf_hello", {"extra": 1})
        self.assertTrue(r["isError"])
        self.assertEqual(self.opened, [])  # refused before connecting

    def test_console(self):
        r = self.call("emf_console", {"text": "pos", "timeout_s": 5, "settle_ms": 100})
        self.assertFalse(r["isError"])
        self.assertIn("player 5 at", self.text_of(r))
        self.assertIn("ran", self.text_of(r))
        self.assertEqual(self.bridge.calls[-1], ("console", {"text": "pos", "timeout_ms": 5000, "settle_ms": 100}))
        r = self.call("emf_console", {"text": "nosuch"})
        self.assertTrue(r["isError"])
        self.assertIn("unknown", self.text_of(r))

    def test_console_timeout(self):
        self.bridge.handlers["console"] = lambda f: {"ok": False, "code": "timeout", "error": "timed out",
                                                     "output": ["half"]}
        r = self.call("emf_console", {"text": "slow"})
        self.assertTrue(r["isError"])
        self.assertIn("timed out", self.text_of(r))
        self.assertIn("half", self.text_of(r))

    def test_state(self):
        r = self.call("emf_state")
        self.assertEqual(json.loads(self.text_of(r))["player"]["position"]["x"], 100.5)
        r = self.call("emf_state", {"path": "player.position"})
        self.assertEqual(json.loads(self.text_of(r)), {"x": 100.5, "y": 50.0, "z": -20.25})
        r = self.call("emf_state", {"path": "overlay.hud[id=nothing]"})
        self.assertTrue(r["isError"])
        self.assertIn("id=nothing", self.text_of(r))
        r = self.call("emf_state", {"path": "a["})
        self.assertTrue(r["isError"])

    def test_mods(self):
        r = self.call("emf_mods")
        text = self.text_of(r)
        self.assertIn("clock", text)
        self.assertIn("switched off", text)

    def test_log(self):
        for i in range(10):
            self.bridge.add_log("[x] line %d" % i)
        r = self.call("emf_log", {"since": 5, "limit": 2})
        text = self.text_of(r)
        self.assertIn("line 5", text)
        self.assertNotIn("line 4", text)
        self.assertIn("since=7", text)  # how to get more
        self.assertEqual(self.bridge.calls[-1], ("log", {"since": 5, "max": 2}))
        r = self.call("emf_log", {"since": 0, "contains": "line 3"})
        self.assertIn("line 3", self.text_of(r))
        self.assertNotIn("line 2", self.text_of(r))

    def test_wait(self):
        r = self.call("emf_wait", {"for": "world", "timeout_s": 3})
        self.assertFalse(r["isError"])
        self.assertEqual(self.bridge.calls[-1], ("wait", {"for": "world", "timeout_ms": 3000}))
        self.bridge.state_data["world"]["status"] = "none"
        r = self.call("emf_wait", {"for": "world", "timeout_s": 1})
        self.assertTrue(r["isError"])
        self.assertIn("timed out", self.text_of(r))
        self.bridge.add_log("[clock] tick")
        r = self.call("emf_wait", {"for": "log", "match": "[clock]", "since": 0})
        self.assertIn("[clock] tick", self.text_of(r))
        r = self.call("emf_wait", {"for": "log"})
        self.assertTrue(r["isError"])

    def test_screenshot_returns_the_png(self):
        r = self.call("emf_screenshot", {"max_width": 640, "overlay": False})
        self.assertFalse(r["isError"])
        images = [c for c in r["content"] if c["type"] == "image"]
        self.assertEqual(len(images), 1)
        self.assertEqual(images[0]["mimeType"], "image/png")
        self.assertEqual(base64.b64decode(images[0]["data"]), PNG_1X1)
        self.assertIn("640", self.text_of(r))
        self.assertEqual(self.bridge.calls[-1], ("screenshot", {"max_width": 640, "overlay": False}))
        self.call("emf_screenshot")
        self.assertEqual(self.bridge.calls[-1], ("screenshot", {"max_width": emf_mcp.DEFAULT_SHOT_WIDTH}))

    def test_mod_key(self):
        r = self.call("emf_mod_key", {"key": "L"})
        self.assertFalse(r["isError"])
        self.assertIn("9", self.text_of(r))
        r = self.call("emf_mod_key", {"key": "Banana"})
        self.assertTrue(r["isError"])
        self.assertIn("unknown key", self.text_of(r))
        self.call("emf_mod_key", {"key": "G", "action": "down", "hold_ms": 200})
        self.assertEqual(self.bridge.calls[-1], ("mod_key", {"key": "G", "action": "down", "hold_ms": 200}))


class ScenarioToolTests(Base):
    def write(self, folder, name, scenario):
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, name)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(scenario, f)
        return path

    def test_runs_a_scenario_from_tests_game(self):
        self.write(os.path.join(self.repo, "tests", "game"), "s.json",
                   {"name": "s", "steps": [{"do": "hello"}, {"do": "state", "path": "player", "op": "exists"}]})
        for path in ("tests/game/s.json", "s", os.path.join(self.repo, "tests", "game", "s.json")):
            with self.subTest(path=path):
                r = self.call("emf_run_scenario", {"path": path})
                self.assertFalse(r["isError"], self.text_of(r))
                self.assertIn("2 passed", self.text_of(r))

    def test_a_failing_scenario_is_a_tool_error_with_the_steps(self):
        self.write(os.path.join(self.repo, "tests", "game"), "f.json",
                   {"name": "f", "steps": [{"do": "console", "text": "nosuch"}, {"do": "hello"}]})
        r = self.call("emf_run_scenario", {"path": "tests/game/f.json"})
        self.assertTrue(r["isError"])
        text = self.text_of(r)
        self.assertIn("FAIL", text)
        self.assertIn("skipped", text)

    def test_runs_a_scenario_from_the_games_emf_folder(self):
        path = self.write(os.path.join(self.game, "emf", "scenarios"), "g.json", {"name": "g", "steps": [{"do": "hello"}]})
        r = self.call("emf_run_scenario", {"path": path})
        self.assertFalse(r["isError"], self.text_of(r))

    def test_other_places_are_refused(self):
        path = self.write(self.tmp, "elsewhere.json", {"name": "x", "steps": [{"do": "hello"}]})
        inside_repo = self.write(self.repo, "root.json", {"name": "x", "steps": [{"do": "hello"}]})
        for p in (path, inside_repo, "tests/game/../../root.json", "tests/game/missing.json"):
            with self.subTest(path=p):
                r = self.call("emf_run_scenario", {"path": p})
                self.assertTrue(r["isError"])
        self.assertEqual(self.opened, [])

    def test_an_invalid_scenario_is_reported(self):
        self.write(os.path.join(self.repo, "tests", "game"), "bad.json", {"name": "x", "steps": [{"do": "fly"}]})
        r = self.call("emf_run_scenario", {"path": "bad"})
        self.assertTrue(r["isError"])
        self.assertIn("fly", self.text_of(r))
        self.assertEqual(self.opened, [])


class OwnershipTests(Base):
    """World-changing tools act only on the game game_session.ps1 recorded (its pid and start time), unless
    --any-game. Reading works on any game."""

    def test_without_a_recorded_game(self):
        os.remove(self.session)
        for name, args in (("emf_console", {"text": "pos"}), ("emf_mod_key", {"key": "L"})):
            with self.subTest(tool=name):
                r = self.call(name, args)
                self.assertTrue(r["isError"])
                self.assertIn("game_session", self.text_of(r))
                self.assertIn("--any-game", self.text_of(r))
        self.assertEqual([c for c in self.bridge.calls if c[0] in ("console", "mod_key")], [])
        self.assertFalse(self.call("emf_state")["isError"])
        self.assertFalse(self.call("emf_hello")["isError"])

    def test_another_pid_or_a_reused_pid_is_someone_elses_game(self):
        self.write_session({"game_pid": 9999, "game_start_epoch": 1000.0})
        r = self.call("emf_console", {"text": "pos"})
        self.assertTrue(r["isError"])
        self.assertIn("4242", self.text_of(r))
        self.write_session({"game_pid": 4242, "game_start_epoch": 50.0})  # the pid was reused since
        r = self.call("emf_console", {"text": "pos"})
        self.assertTrue(r["isError"])
        self.assertEqual([c for c in self.bridge.calls if c[0] == "console"], [])

    def test_the_recorded_game_and_the_override(self):
        self.assertFalse(self.call("emf_console", {"text": "pos"})["isError"])
        os.remove(self.session)
        self.server = self.make_server(any_game=True)
        self.assertFalse(self.call("emf_console", {"text": "pos"})["isError"])

    def test_run_scenario_is_world_changing(self):
        with open(os.path.join(self.repo, "tests", "game", "s.json"), "w", encoding="utf-8") as f:
            json.dump({"name": "s", "steps": [{"do": "hello"}]}, f)
        os.remove(self.session)
        r = self.call("emf_run_scenario", {"path": "s"})
        self.assertTrue(r["isError"])
        self.assertIn("game_session", self.text_of(r))


class LoadWorldToolTests(Base):
    """emf_load_world: world-changing (the recorded game only), and the world it started must be the LOCAL save asked
    for; another one is ended at once (the recorded game only)."""

    def setUp(self):
        super().setUp()
        self.bridge.at_main_menu()
        self.killed = []
        self.server = self.make_server(kill_game=self.killed.append)

    def load_calls(self):
        return [c[1] for c in self.bridge.calls if c[0] == "load_world"]

    def test_the_defaults_from_the_main_menu(self):
        r = self.call("emf_load_world")
        self.assertFalse(r["isError"], self.text_of(r))
        self.assertEqual(self.load_calls(), [{"slot": 1, "character": 1, "expect_name": "Character 2",
                                              "timeout_ms": 180000}])
        self.assertIn("'My World 1' runs with 'Character 2'", self.text_of(r))
        self.assertIn("LOCAL world slot 1", self.text_of(r))
        self.assertEqual(self.killed, [])

    def test_the_arguments(self):
        self.bridge.local_worlds[2] = "Second"
        self.bridge.local_characters[3] = "Tester"
        r = self.call("emf_load_world", {"slot": 2, "character": 3, "expect_name": "", "timeout_s": 60})
        self.assertFalse(r["isError"], self.text_of(r))
        self.assertEqual(self.load_calls(), [{"slot": 2, "character": 3, "timeout_ms": 60000}])  # "" = any name
        for bad in ({"slot": 0}, {"slot": 11}, {"character": 17}, {"timeout_s": 601}, {"slot": "1"}):
            with self.subTest(args=bad):
                self.assertTrue(self.call("emf_load_world", bad)["isError"])

    def test_a_refusal_is_a_tool_error_in_plain_words(self):
        r = self.call("emf_load_world", {"expect_name": "Someone"})
        self.assertTrue(r["isError"])
        self.assertIn("nothing started", self.text_of(r))
        self.assertIn("not 'Someone'", self.text_of(r))

    def test_only_the_recorded_game(self):
        self.write_session({"game_pid": 9999, "game_start_epoch": 1000.0})
        r = self.call("emf_load_world")
        self.assertTrue(r["isError"])
        self.assertIn("not the one game_session.ps1 started", self.text_of(r))
        self.assertEqual(self.load_calls(), [])

    def test_a_wrong_world_is_ended_at_once(self):
        self.bridge.loaded_slot = 257
        r = self.call("emf_load_world")
        self.assertTrue(r["isError"])
        self.assertIn("WRONG WORLD", self.text_of(r))
        self.assertIn("Steam Cloud", self.text_of(r))
        self.assertIn("ended", self.text_of(r))
        self.assertEqual(self.killed, [4242])

    def test_a_wrong_world_in_a_game_of_your_own_is_not_ended(self):
        os.remove(self.session)
        self.server = self.make_server(any_game=True, kill_game=self.killed.append)
        self.bridge.loaded_slot = 2
        r = self.call("emf_load_world")
        self.assertTrue(r["isError"])
        self.assertIn("WRONG WORLD", self.text_of(r))
        self.assertIn("without saving", self.text_of(r))
        self.assertEqual(self.killed, [])

    def test_an_unknown_slot_after_the_start_is_an_error_but_ends_nothing(self):
        def no_slot():
            if self.bridge.state_data["world"]["status"] == "running":
                self.bridge.state_data["world"]["slot"] = None
        self.bridge.state_hook = no_slot
        r = self.call("emf_load_world")
        self.assertTrue(r["isError"])
        self.assertIn("not confirmed", self.text_of(r))
        self.assertEqual(self.killed, [])

    def test_the_world_running_already(self):
        self.bridge.state_data["world"] = {"status": "running", "kind": "server", "slot": 1, "name": "My World 1"}
        r = self.call("emf_load_world")
        self.assertFalse(r["isError"], self.text_of(r))
        self.assertIn("runs already", self.text_of(r))
        self.bridge.state_data["world"] = {"status": "running", "kind": "server", "slot": 257, "name": "Cloud"}
        r = self.call("emf_load_world")
        self.assertTrue(r["isError"])  # (the LOCAL rule refuses a Steam Cloud world first)
        self.assertEqual(self.load_calls(), [])
        self.assertEqual(self.killed, [])


class LocalWorldRuleTests(Base):
    def test_a_steam_cloud_world_is_refused(self):
        self.bridge.state_data["world"]["slot"] = 257
        for name, args in (("emf_console", {"text": "pos"}), ("emf_mod_key", {"key": "L"})):
            with self.subTest(tool=name):
                r = self.call(name, args)
                self.assertTrue(r["isError"])
                self.assertIn("Steam Cloud", self.text_of(r))
        self.assertEqual([c for c in self.bridge.calls if c[0] in ("console", "mod_key")], [])

    def test_the_override_is_explicit(self):
        self.bridge.state_data["world"]["slot"] = 257
        self.server = self.make_server(allow_cloud_world=True)
        self.assertFalse(self.call("emf_console", {"text": "pos"})["isError"])
        with open(os.path.join(REPO, ".mcp.json"), encoding="utf-8") as f:
            self.assertNotIn("allow-cloud", f.read())


class CancelTests(Base):
    def test_a_cancelled_call_gets_no_reply_and_the_server_goes_on(self):
        import threading
        seen = threading.Event()

        def slow(f):
            seen.set()
            for _ in range(200):  # until the main thread has read the cancellation
                if 7 in self.server.cancelled:
                    break
                import time
                time.sleep(0.01)
            return {"status": "ran", "output": ["late"], "log_seq": 1}
        self.bridge.handlers["console"] = slow
        lines = [json.dumps({"jsonrpc": "2.0", "id": 7, "method": "tools/call",
                             "params": {"name": "emf_console", "arguments": {"text": "slow"}}}),
                 json.dumps({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 7}}),
                 json.dumps({"jsonrpc": "2.0", "id": 8, "method": "ping"})]
        stdout = io.BytesIO()
        self.server.serve(io.BytesIO(("\n".join(lines) + "\n").encode()), stdout)
        replies = [json.loads(l) for l in stdout.getvalue().decode().splitlines()]
        self.assertTrue(seen.is_set())
        self.assertEqual([r["id"] for r in replies], [8])


class ScreenshotCapTests(Base):
    def test_the_width_is_capped(self):
        tool = [t for t in emf_mcp.TOOLS if t["name"] == "emf_screenshot"][0]
        self.assertLessEqual(tool["inputSchema"]["properties"]["max_width"]["maximum"], 1920)
        r = self.call("emf_screenshot", {"max_width": 3840})
        self.assertTrue(r["isError"])

    def test_a_picture_too_big_is_an_error(self):
        old = emf_mcp.MAX_IMAGE_BYTES
        emf_mcp.MAX_IMAGE_BYTES = 10
        try:
            r = self.call("emf_screenshot")
        finally:
            emf_mcp.MAX_IMAGE_BYTES = old
        self.assertTrue(r["isError"])
        self.assertIn("too big", self.text_of(r))
        self.assertLessEqual(emf_mcp.MAX_IMAGE_BYTES, int(3.75 * 1024 * 1024))


class ScenarioRootTests(Base):
    def test_only_the_named_or_recorded_game_is_a_root(self):
        other = os.path.join(self.tmp, "some_folder")
        os.makedirs(os.path.join(other, "emf"))
        with open(os.path.join(other, "emf", "x.json"), "w", encoding="utf-8") as f:
            json.dump({"name": "x", "steps": [{"do": "hello"}]}, f)
        server = self.make_server(game=None)  # the recorded game is self.game, not some_folder
        reply = server.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
            "name": "emf_run_scenario", "arguments": {"path": os.path.join(other, "emf", "x.json")}}})
        self.assertTrue(reply["result"]["isError"])
        with open(os.path.join(self.game, "emf", "ok.json"), "w", encoding="utf-8") as f:
            json.dump({"name": "ok", "steps": [{"do": "hello"}]}, f)
        reply = server.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {
            "name": "emf_run_scenario", "arguments": {"path": os.path.join(self.game, "emf", "ok.json")}}})
        self.assertFalse(reply["result"]["isError"], reply)

    def test_the_game_folder_itself_is_not_a_root(self):
        with open(os.path.join(self.game, "top.json"), "w", encoding="utf-8") as f:
            json.dump({"name": "top", "steps": [{"do": "hello"}]}, f)
        for path in (os.path.join(self.game, "top.json"), "top.json", self.game):
            with self.subTest(path=path):
                self.assertTrue(self.call("emf_run_scenario", {"path": path})["isError"])


class RealFilesTests(unittest.TestCase):
    def test_project_mcp_json_registers_the_server(self):
        with open(os.path.join(REPO, ".mcp.json"), encoding="utf-8") as f:
            cfg = json.load(f)
        server = cfg["mcpServers"]["emf"]
        self.assertTrue(any(a.replace("\\", "/").endswith("tools/mcp/emf_mcp.py") for a in server["args"]))


class CharacterRuleTests(Base):
    """The tools that change the game need a LOCAL character in a running world; reading stays allowed."""

    def test_a_steam_or_unknown_character_refuses_the_tools_that_change_the_game(self):
        for character, words in (({"storage": "steam", "slot": 1, "name": "Steam Hero"},
                                  "the running world uses a Steam character"),
                                 ({"storage": "unknown", "slot": None, "name": None},
                                  "can't tell which character runs: go back to the main menu and use load_world")):
            self.bridge.state_data["character"] = character
            for name, args in (("emf_console", {"text": "pos"}), ("emf_mod_key", {"key": "L"})):
                with self.subTest(tool=name, storage=character["storage"]):
                    r = self.call(name, args)
                    self.assertTrue(r["isError"])
                    self.assertIn(words, self.text_of(r))
            self.assertFalse(self.call("emf_state")["isError"])
            self.assertFalse(self.call("emf_hello")["isError"])
            self.assertFalse(self.call("emf_log")["isError"])
            self.assertFalse(self.call("emf_screenshot")["isError"])
        self.assertEqual([c for c in self.bridge.calls if c[0] in ("console", "mod_key")], [])

    def test_run_scenario_is_refused_too(self):
        with open(os.path.join(self.repo, "tests", "game", "s.json"), "w", encoding="utf-8") as f:
            json.dump({"name": "s", "steps": [{"do": "hello"}]}, f)
        self.bridge.state_data["character"] = {"storage": "steam", "slot": 1, "name": "Steam Hero"}
        r = self.call("emf_run_scenario", {"path": "s"})
        self.assertTrue(r["isError"])
        self.assertIn("Steam character", self.text_of(r))

    def test_load_world_running_already_needs_the_local_character(self):
        killed = []
        self.server = self.make_server(kill_game=killed.append)
        self.bridge.state_data["world"] = {"status": "running", "kind": "server", "slot": 1, "name": "My World 1"}
        self.bridge.state_data["character"] = {"storage": "local", "slot": 2, "name": "Someone"}
        r = self.call("emf_load_world")
        self.assertTrue(r["isError"])
        self.assertIn("not 'Character 2'", self.text_of(r))
        self.assertEqual([c for c in self.bridge.calls if c[0] == "load_world"], [])
        self.assertEqual(killed, [])

    def test_load_world_ends_a_world_started_with_a_steam_character(self):
        killed = []
        self.server = self.make_server(kill_game=killed.append)
        self.bridge.at_main_menu()
        self.bridge.loaded_character = {"storage": "steam", "slot": 1, "name": "Steam Hero"}
        r = self.call("emf_load_world")
        self.assertTrue(r["isError"])
        self.assertIn("WRONG WORLD", self.text_of(r))
        self.assertEqual(killed, [4242])


if __name__ == "__main__":
    unittest.main()
