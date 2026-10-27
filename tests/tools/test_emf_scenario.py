"""Unit tests of tools/bridge/emf_scenario.py against a fake bridge (no game, no pipe).
Run: python -m unittest discover -s tests/tools"""

import glob
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from fake_bridge import REPO, FakeBridge, ConnectError  # noqa: E402

sys.path.insert(0, os.path.join(REPO, "tools", "bridge"))
import emf_scenario  # noqa: E402


class FakeClock:
    def __init__(self):
        self.t = 1000.0
        self.slept = []

    def monotonic(self):
        return self.t

    def sleep(self, s):
        self.slept.append(s)
        self.t += s


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="emf_scenario_test_")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)  # also when setUp fails after this
        self.game = os.path.join(self.tmp, "game")
        self.mod = os.path.join(self.game, "emf", "mods", "my_first_mod")
        os.makedirs(self.mod)
        self.main_lua = os.path.join(self.mod, "main.lua")
        with open(self.main_lua, "wb") as f:
            f.write(b'local shown = nil\n  local text = emf.setting("text")\nreturn text\n')
        self.out = os.path.join(self.tmp, "out")
        self.bridge = FakeBridge(self.game)
        self.clock = FakeClock()
        self.lines = []

    def console_calls(self):
        return [c for c in self.bridge.calls if c[0] == "console"]

    def read_main(self):
        with open(self.main_lua, "rb") as f:
            return f.read()

    def run_steps(self, steps, keep_going=False, name="test"):
        scenario = emf_scenario.validate({"name": name, "steps": steps})
        runner = emf_scenario.Runner(self.bridge, game=self.game, out_dir=self.out, echo=self.lines.append,
                                     keep_going=keep_going, sleep=self.clock.sleep, monotonic=self.clock.monotonic)
        return runner.run(scenario)

    def statuses(self, report):
        return [s["status"] for s in report["steps"]]

    def text(self):
        return "\n".join(self.lines)


class ValidateTests(unittest.TestCase):
    def bad(self, scenario, words):
        with self.assertRaises(emf_scenario.ScenarioError) as e:
            emf_scenario.validate(scenario)
        for w in words:
            self.assertIn(w, str(e.exception))

    def test_needs_name_and_steps(self):
        self.bad({"steps": [{"do": "hello"}]}, ["name"])
        self.bad({"name": "x", "steps": []}, ["steps"])
        self.bad({"name": "x"}, ["steps"])

    def test_unknown_kind_names_the_step(self):
        self.bad({"name": "x", "steps": [{"do": "hello"}, {"do": "fly"}]}, ["step 2", "fly"])

    def test_step_without_do(self):
        self.bad({"name": "x", "steps": [{"text": "pos"}]}, ["step 1", "do"])

    def test_missing_required_field(self):
        self.bad({"name": "x", "steps": [{"do": "console"}]}, ["step 1", "text"])
        self.bad({"name": "x", "steps": [{"do": "wait", "for": "log"}]}, ["match"])

    def test_unknown_field_is_an_error(self):
        self.bad({"name": "x", "steps": [{"do": "console", "text": "pos", "expected": "x"}]}, ["expected"])

    def test_bad_op_and_regex(self):
        self.bad({"name": "x", "steps": [{"do": "state", "path": "a", "op": "~="}]}, ["op"])
        self.bad({"name": "x", "steps": [{"do": "state", "path": "a", "op": "=="}]}, ["value"])
        self.bad({"name": "x", "steps": [{"do": "console", "text": "pos", "expect_re": "("}]}, ["expect_re"])
        self.bad({"name": "x", "steps": [{"do": "state", "path": "a[", "op": "exists"}]}, ["path"])

    def test_wait_for_values(self):
        self.bad({"name": "x", "steps": [{"do": "wait", "for": "lunch"}]}, ["for"])

    def test_edit_file_needs_one_mode(self):
        self.bad({"name": "x", "steps": [{"do": "edit_file", "path": "emf/mods/a/b.lua"}]}, ["find", "write"])
        self.bad({"name": "x", "steps": [{"do": "edit_file", "path": "emf/mods/a/b.lua", "find": "a", "replace": "b",
                                          "write": "c"}]}, ["find", "write"])
        self.bad({"name": "x", "steps": [{"do": "edit_file", "path": "emf/mods/a/b.lua", "find": "a"}]}, ["replace"])

    def test_log_since_mark_needs_a_known_mark(self):
        self.bad({"name": "x", "steps": [{"do": "log_since_mark", "mark": "m", "match": "x"}]}, ["mark", "m"])
        emf_scenario.validate({"name": "x", "steps": [{"do": "log_mark", "name": "m"},
                                                      {"do": "log_since_mark", "mark": "m", "match": "x"}]})

    def test_types_are_checked(self):
        self.bad({"name": "x", "steps": [{"do": "sleep", "s": "2"}]}, ["s"])
        self.bad({"name": "x", "steps": [{"do": "wait", "for": "world", "timeout_s": -1}]}, ["timeout_s"])

    def test_load_world_fields(self):
        emf_scenario.validate({"name": "x", "steps": [{"do": "load_world"}]})
        emf_scenario.validate({"name": "x", "load_world": {"slot": 2, "character": 3, "expect_name": ""},
                               "steps": [{"do": "load_world", "slot": 1, "timeout_s": 600}]})
        self.bad({"name": "x", "steps": [{"do": "load_world", "slot": 0}]}, ["slot", "1-10"])
        self.bad({"name": "x", "steps": [{"do": "load_world", "slot": 11}]}, ["slot", "1-10"])
        self.bad({"name": "x", "steps": [{"do": "load_world", "character": 17}]}, ["character", "1-16"])
        self.bad({"name": "x", "steps": [{"do": "load_world", "timeout_s": 601}]}, ["timeout_s"])
        self.bad({"name": "x", "steps": [{"do": "load_world", "slot": "1"}]}, ["slot"])
        self.bad({"name": "x", "load_world": {"slot": 1, "world": "My World 1"}, "steps": [{"do": "hello"}]},
                 ["load_world", "world"])
        self.bad({"name": "x", "load_world": {"slot": 12}, "steps": [{"do": "hello"}]}, ["load_world", "slot"])
        self.bad({"name": "x", "load_world": "slot 1", "steps": [{"do": "hello"}]}, ["load_world"])

    def test_example_scenarios_are_valid(self):
        files = sorted(glob.glob(os.path.join(REPO, "tests", "game", "*.json")))
        self.assertTrue(files, "tests/game has scenarios")
        for path in files:
            with self.subTest(path=path):
                emf_scenario.load_scenario(path)


class PathTests(unittest.TestCase):
    def test_parse_paths(self):
        self.assertEqual(emf_scenario.parse_path("player.position.x"), ["player", "position", "x"])
        self.assertEqual(emf_scenario.parse_path("overlay.hud[id=my_first_mod.line].text"),
                         ["overlay", "hud", ("sel", "id", "my_first_mod.line"), "text"])
        self.assertEqual(emf_scenario.parse_path("mods[0].id"), ["mods", ("idx", 0), "id"])
        self.assertEqual(emf_scenario.parse_path("mods[-1]"), ["mods", ("idx", -1)])
        for bad in ("", "a..b", "a[", "a[]", "a]b", ".a"):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    emf_scenario.parse_path(bad)

    def test_resolve(self):
        data = {"player": None, "overlay": {"hud": [{"id": "clock", "place": 1}, {"id": "a.b", "place": 2}]}}
        self.assertEqual(emf_scenario.resolve(data, "overlay.hud[id=a.b].place"), (True, 2, ""))
        self.assertEqual(emf_scenario.resolve(data, "overlay.hud[place=1].id"), (True, "clock", ""))
        found, value, why = emf_scenario.resolve(data, "player.position.x")
        self.assertFalse(found)
        self.assertIn("player is null", why)
        found, value, why = emf_scenario.resolve(data, "overlay.hud[id=probe]")
        self.assertFalse(found)
        self.assertIn("id=probe", why)
        self.assertIn("clock", why)
        found, value, why = emf_scenario.resolve(data, "overlay.nothing")
        self.assertFalse(found)
        self.assertIn("nothing", why)


class StepTests(Base):
    def test_hello(self):
        r = self.run_steps([{"do": "hello"}])
        self.assertTrue(r["ok"])
        self.assertIn("EMF 0.3.0", self.text())
        r = self.run_steps([{"do": "hello", "expect": {"process": "dedicated server"}}])
        self.assertFalse(r["ok"])
        self.assertIn("process", r["steps"][0]["detail"])

    def test_wait(self):
        r = self.run_steps([{"do": "wait", "for": "world", "timeout_s": 5}, {"do": "wait", "for": "player"}])
        self.assertEqual(self.statuses(r), ["ok", "ok"])
        self.assertEqual(self.bridge.calls[0], ("wait", {"for": "world", "timeout_ms": 5000}))
        self.bridge.state_data["world"]["status"] = "none"
        r = self.run_steps([{"do": "wait", "for": "world", "timeout_s": 1}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("timed out", r["steps"][0]["detail"])

    def test_wait_log(self):
        self.bridge.add_log("[clock] hello")
        r = self.run_steps([{"do": "wait", "for": "log", "match": "[clock]", "since": 0}])
        self.assertEqual(self.statuses(r), ["ok"])
        self.assertIn("[clock] hello", r["steps"][0]["detail"])

    def test_console_expect(self):
        r = self.run_steps([{"do": "console", "text": "pos", "expect": "player 5 at"},
                            {"do": "console", "text": "pos", "expect": ["player", "m"]},
                            {"do": "console", "text": "pos", "expect_re": r"player \d+ at \("}])
        self.assertEqual(self.statuses(r), ["ok", "ok", "ok"])
        self.assertEqual(self.console_calls()[0][1]["text"], "pos")

    def test_console_expect_missing_shows_output(self):
        r = self.run_steps([{"do": "console", "text": "pos", "expect": "nowhere"}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("nowhere", r["steps"][0]["detail"])
        self.assertIn("player 5 at", r["steps"][0]["detail"])

    def test_console_status(self):
        r = self.run_steps([{"do": "console", "text": "nosuch"}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("unknown", r["steps"][0]["detail"])
        r = self.run_steps([{"do": "console", "text": "nosuch", "status": "unknown"}])
        self.assertEqual(self.statuses(r), ["ok"])

    def test_console_timeout_and_options(self):
        self.bridge.handlers["console"] = lambda f: {"ok": False, "code": "timeout", "error": "timed out",
                                                     "output": ["half"]}
        r = self.run_steps([{"do": "console", "text": "slow", "timeout_s": 2, "settle_ms": 50}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("timed out", r["steps"][0]["detail"])
        self.assertIn("half", r["steps"][0]["detail"])
        self.assertEqual(self.console_calls()[0][1], {"text": "slow", "timeout_ms": 2000, "settle_ms": 50})

    def test_state_ops(self):
        r = self.run_steps([
            {"do": "state", "path": "player.position", "op": "exists"},
            {"do": "state", "path": "player.position.x", "op": "==", "value": 100.5},
            {"do": "state", "path": "world.slot", "op": "==", "value": 1.0},
            {"do": "state", "path": "world.status", "op": "!=", "value": "none"},
            {"do": "state", "path": "player.health.value", "op": ">", "value": 0},
            {"do": "state", "path": "player.health.value", "op": "<=", "value": 180},
            {"do": "state", "path": "overlay.hud", "op": "contains", "value": {"id": "clock"}},
            {"do": "state", "path": "overlay.hud[id=my_first_mod.line].text", "op": "contains", "value": "Hello"},
            {"do": "state", "path": "overlay.hud[id=my_first_mod.line].text", "op": "matches", "value": r"\(\d"},
            {"do": "state", "path": "overlay.hud[id=probe]", "op": "missing"},
            {"do": "state", "path": "player.health.max", "op": "missing"},
            {"do": "state", "path": "overlay", "op": "contains", "value": "hud"},
        ])
        self.assertEqual(self.statuses(r), ["ok"] * 12, self.text())

    def test_state_failures_say_why(self):
        self.bridge.state_data["player"] = None
        r = self.run_steps([{"do": "state", "path": "player.position.x", "op": "exists"}])
        self.assertIn("player is null", r["steps"][0]["detail"])
        self.bridge.state_data["player"] = {"id": 5, "position": {"x": "a"}}
        r = self.run_steps([{"do": "state", "path": "player.position.x", "op": ">", "value": 1}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("not a number", r["steps"][0]["detail"])
        r = self.run_steps([{"do": "state", "path": "world.status", "op": "==", "value": "none"}])
        self.assertIn("running", r["steps"][0]["detail"])

    def test_state_polls_until_timeout(self):
        calls = {"n": 0}

        def later():
            calls["n"] += 1
            if calls["n"] == 4:
                self.bridge.state_data["overlay"]["hud"].append({"id": "probe", "text": "text", "place": 2})
        self.bridge.state_hook = later
        r = self.run_steps([{"do": "state", "path": "overlay.hud[id=probe].text", "op": "==", "value": "text",
                             "timeout_s": 5}])
        self.assertEqual(self.statuses(r), ["ok"])
        self.assertEqual(calls["n"], 4)
        self.assertTrue(self.clock.slept)
        self.bridge.state_hook = None
        r = self.run_steps([{"do": "state", "path": "overlay.hud[id=never]", "op": "exists", "timeout_s": 2}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("2", r["steps"][0]["detail"])

    def test_mods(self):
        r = self.run_steps([{"do": "mods", "id": "clock"}, {"do": "mods", "id": "clock", "status": "loaded"}])
        self.assertEqual(self.statuses(r), ["ok", "ok"])
        r = self.run_steps([{"do": "mods", "id": "quick_stack", "status": "loaded"}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("disabled", r["steps"][0]["detail"])
        r = self.run_steps([{"do": "mods", "id": "my_first_mod"}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("clock", r["steps"][0]["detail"])  # lists what is there

    def test_mod_key(self):
        r = self.run_steps([{"do": "mod_key", "key": "L", "min_reads": 1}])
        self.assertEqual(self.statuses(r), ["ok"])
        self.assertIn("9", r["steps"][0]["detail"])
        r = self.run_steps([{"do": "mod_key", "key": "K", "min_reads": 1}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("0", r["steps"][0]["detail"])
        r = self.run_steps([{"do": "mod_key", "key": "Banana"}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("unknown key", r["steps"][0]["detail"])
        self.run_steps([{"do": "mod_key", "key": "G", "action": "down"}])
        self.assertEqual(self.bridge.calls[-1], ("mod_key", {"key": "G", "action": "down"}))

    def test_screenshot_copies_into_out(self):
        r = self.run_steps([{"do": "screenshot", "name": "main menu!", "max_width": 1280, "overlay": False}])
        self.assertEqual(self.statuses(r), ["ok"])
        copied = r["steps"][0]["data"]["copy"]
        self.assertTrue(copied.startswith(self.out))
        self.assertTrue(os.path.isfile(copied))
        self.assertTrue(os.path.basename(copied).endswith("main_menu_.png"))
        self.assertEqual(self.bridge.calls[0], ("screenshot", {"max_width": 1280, "overlay": False}))

    def test_sleep(self):
        r = self.run_steps([{"do": "sleep", "s": 1.5}])
        self.assertEqual(self.statuses(r), ["ok"])
        self.assertIn(1.5, self.clock.slept)


class LoadWorldStepTests(Base):
    """The load_world step: a LOCAL save from the main menu through the bridge, then checked in the state."""

    def setUp(self):
        super().setUp()
        self.bridge.at_main_menu()

    def run_scenario(self, scenario):
        runner = emf_scenario.Runner(self.bridge, game=self.game, out_dir=self.out, echo=self.lines.append,
                                     sleep=self.clock.sleep, monotonic=self.clock.monotonic)
        return runner.run(emf_scenario.validate(scenario))

    def load_calls(self):
        return [c[1] for c in self.bridge.calls if c[0] == "load_world"]

    def test_from_the_main_menu_with_the_defaults(self):
        r = self.run_steps([{"do": "load_world"}, {"do": "state", "path": "world.name", "op": "==",
                                                    "value": "My World 1"}])
        self.assertEqual(self.statuses(r), ["ok", "ok"], self.text())
        self.assertEqual(self.load_calls(), [{"slot": 1, "character": 1, "expect_name": "Character 2",
                                              "timeout_ms": 180000}])
        detail = r["steps"][0]["detail"]
        self.assertIn("'My World 1' runs with 'Character 2'", detail)
        self.assertIn("LOCAL world slot 1", detail)
        self.assertEqual(r["steps"][0]["data"]["world"], "My World 1")

    def test_the_scenario_fields_are_the_defaults_and_a_step_overrides_them(self):
        self.bridge.local_worlds[2] = "Second"
        self.bridge.local_characters[3] = "Tester"
        r = self.run_scenario({"name": "x", "load_world": {"slot": 2, "character": 3, "expect_name": "Tester"},
                               "steps": [{"do": "load_world", "timeout_s": 60}]})
        self.assertTrue(r["ok"], self.text())
        self.assertEqual(self.load_calls()[-1], {"slot": 2, "character": 3, "expect_name": "Tester",
                                                 "timeout_ms": 60000})
        self.bridge.at_main_menu()
        r = self.run_scenario({"name": "x", "load_world": {"slot": 2, "character": 3, "expect_name": "Tester"},
                               "steps": [{"do": "load_world", "slot": 1, "character": 1, "expect_name": ""}]})
        self.assertTrue(r["ok"], self.text())
        self.assertEqual(self.load_calls()[-1], {"slot": 1, "character": 1, "timeout_ms": 180000})  # "" = any name

    def test_a_refusal_fails_in_plain_words(self):
        r = self.run_steps([{"do": "load_world", "expect_name": "Someone"}, {"do": "hello"}])
        self.assertEqual(self.statuses(r), ["fail", "skipped"])
        self.assertIn("nothing started", r["steps"][0]["detail"])
        self.assertIn("not 'Someone'", r["steps"][0]["detail"])
        r = self.run_steps([{"do": "load_world", "slot": 9}])
        self.assertIn("LOCAL world slot 9 does not exist", r["steps"][0]["detail"])

    def test_a_wrong_world_after_the_start_fails(self):
        self.bridge.loaded_slot = 257  # the game said it started slot 1, the state shows a Steam Cloud world
        r = self.run_steps([{"do": "load_world"}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("WRONG WORLD", r["steps"][0]["detail"])
        self.assertIn("Steam Cloud", r["steps"][0]["detail"])
        self.bridge.at_main_menu()
        self.bridge.loaded_slot = None
        self.bridge.loaded_name = "Not My World"
        r = self.run_steps([{"do": "load_world"}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("Not My World", r["steps"][0]["detail"])

    def test_the_world_asked_for_running_already_passes_without_a_start(self):
        self.bridge.state_data["world"] = {"status": "running", "kind": "server", "slot": 1, "name": "My World 1"}
        r = self.run_steps([{"do": "load_world"}])
        self.assertEqual(self.statuses(r), ["ok"], self.text())
        self.assertIn("runs already", r["steps"][0]["detail"])
        self.assertEqual(self.load_calls(), [])

    def test_another_world_running_fails_without_a_start(self):
        for world in ({"status": "running", "kind": "server", "slot": 257, "name": "Cloud"},
                      {"status": "running", "kind": "server", "slot": 2, "name": "Other"},
                      {"status": "running", "kind": "client", "slot": None, "name": None},
                      {"status": "loading", "kind": "server", "slot": None, "name": None}):
            with self.subTest(world=world):
                self.bridge.state_data["world"] = dict(world)
                r = self.run_steps([{"do": "load_world"}])
                self.assertEqual(self.statuses(r), ["fail"])
                self.assertIn("main menu", r["steps"][0]["detail"])
        self.assertEqual(self.load_calls(), [])


class EditFileTests(Base):
    def read(self, path=None):
        with open(path or self.main_lua, "rb") as f:
            return f.read()

    def test_find_replace_then_restored_at_the_end(self):
        original = self.read()
        seen = {}

        def look(text):
            seen["during"] = self.read()
        self.bridge.console_hook = look
        r = self.run_steps([
            {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua",
             "find": '  local text = emf.setting("text")',
             "replace": '  local text = "[" .. emf.setting("text") .. "]"'},
            {"do": "console", "text": "pos"},
        ])
        self.assertTrue(r["ok"], self.text())
        self.assertIn(b'"[" .. emf.setting("text") .. "]"', seen["during"])
        self.assertEqual(self.read(), original)
        self.assertIn("restored", self.text())
        self.assertEqual(r["restored"], ["emf/mods/my_first_mod/main.lua"])
        backups = os.listdir(os.path.join(self.out, "backups"))
        self.assertEqual(len(backups), 1)

    def test_restored_after_a_failure_and_after_lost_connection(self):
        original = self.read()
        r = self.run_steps([
            {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "shown", "replace": "gone"},
            {"do": "console", "text": "pos", "expect": "never"},
        ])
        self.assertFalse(r["ok"])
        self.assertEqual(self.read(), original)
        self.bridge.lost_after = len(self.bridge.calls) + 2  # (1: the edit's world check; 2: the console's)
        r = self.run_steps([
            {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "shown", "replace": "gone"},
            {"do": "console", "text": "pos"},
            {"do": "console", "text": "pos"},
        ])
        self.assertEqual(self.statuses(r), ["ok", "fail", "skipped"])
        self.assertIn("connection", r["steps"][1]["detail"])
        self.assertEqual(self.read(), original)

    def test_restored_on_interrupt(self):
        original = self.read()

        def interrupt(f):
            raise KeyboardInterrupt()
        self.bridge.handlers["console"] = interrupt
        with self.assertRaises(KeyboardInterrupt):
            self.run_steps([
                {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "shown", "replace": "gone"},
                {"do": "console", "text": "pos"},
            ])
        self.assertEqual(self.read(), original)

    def test_write_creates_and_removes(self):
        new = os.path.join(self.mod, "extra.lua")
        r = self.run_steps([
            {"do": "edit_file", "path": "emf\\mods\\my_first_mod\\extra.lua", "write": "emf.log('x')\n"},
            {"do": "state", "path": "world.status", "op": "==", "value": "running"},
        ])
        self.assertTrue(r["ok"], self.text())
        self.assertFalse(os.path.exists(new))

    def test_restore_file_step(self):
        original = self.read()
        r = self.run_steps([
            {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "shown", "replace": "gone"},
            {"do": "restore_file", "path": "emf/mods/my_first_mod/main.lua"},
        ])
        self.assertTrue(r["ok"], self.text())
        self.assertEqual(self.read(), original)
        self.assertEqual(r["restored"], ["emf/mods/my_first_mod/main.lua"])
        r = self.run_steps([{"do": "restore_file", "path": "emf/mods/my_first_mod/main.lua"}])
        self.assertEqual(self.statuses(r), ["fail"])

    def test_edits_twice_keep_the_first_original(self):
        original = self.read()
        r = self.run_steps([
            {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "shown", "replace": "gone"},
            {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "gone", "replace": "again"},
        ])
        self.assertTrue(r["ok"], self.text())
        self.assertEqual(self.read(), original)

    def test_only_inside_a_mod_folder(self):
        outside = os.path.join(self.game, "emf", "emf_core.ini")
        with open(outside, "w") as f:
            f.write("[debug]\nbridge=1\n")
        for path in ("emf/emf_core.ini", "emf/mods/../emf_core.ini", "emf/mods/x.lua", "../evil.lua",
                     os.path.join(self.tmp, "abs.lua"), "emf/mods/my_first_mod"):
            with self.subTest(path=path):
                r = self.run_steps([{"do": "edit_file", "path": path, "write": "x"}])
                self.assertEqual(self.statuses(r), ["fail"])
                self.assertIn("emf\\mods\\<id>\\", r["steps"][0]["detail"])
        with open(outside) as f:
            self.assertEqual(f.read(), "[debug]\nbridge=1\n")

    def test_find_must_match_once(self):
        r = self.run_steps([{"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "nope",
                             "replace": "x"}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("not found", r["steps"][0]["detail"])
        r = self.run_steps([{"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "text",
                             "replace": "x"}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("3 times", r["steps"][0]["detail"])
        r = self.run_steps([{"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "text",
                             "replace": "x", "all": True}])
        self.assertEqual(self.statuses(r), ["ok"])

    def test_crlf_files(self):
        with open(self.main_lua, "wb") as f:
            f.write(b"a = 1\r\nb = 2\r\n")
        seen = {}
        self.bridge.console_hook = lambda t: seen.setdefault("during", self.read())
        r = self.run_steps([
            {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "a = 1\nb = 2",
             "replace": "a = 3\nb = 4"},
            {"do": "console", "text": "pos"},
        ])
        self.assertTrue(r["ok"], self.text())
        self.assertEqual(seen["during"], b"a = 3\r\nb = 4\r\n")
        self.assertEqual(self.read(), b"a = 1\r\nb = 2\r\n")

    def test_without_a_game_folder(self):
        scenario = emf_scenario.validate({"name": "x", "steps": [
            {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "write": "x"}]})
        runner = emf_scenario.Runner(self.bridge, game=None, out_dir=self.out, echo=self.lines.append)
        r = runner.run(scenario)
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("game folder", r["steps"][0]["detail"])


class JournalTests(Base):
    """A run that is killed (no finally) leaves a journal; the next run puts the files back."""

    def test_the_journal_lives_while_files_are_changed(self):
        seen = {}

        def look(text):
            seen["journal"] = os.path.isfile(os.path.join(self.out, "restore.json"))
        self.bridge.console_hook = look
        r = self.run_steps([
            {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "shown", "replace": "gone"},
            {"do": "console", "text": "pos"},
        ])
        self.assertTrue(r["ok"], self.text())
        self.assertTrue(seen["journal"])
        self.assertFalse(os.path.exists(os.path.join(self.out, "restore.json")))

    def killed_run(self, steps):
        """Runs the steps, then pretends the process died before putting files back."""
        scenario = emf_scenario.validate({"name": "killed", "steps": steps})
        runner = emf_scenario.Runner(self.bridge, game=self.game, out_dir=self.out, echo=self.lines.append)
        runner._restore_all = lambda report: None  # killed: the finally never ran
        runner.run(scenario)
        self.assertTrue(os.path.isfile(os.path.join(self.out, "restore.json")))

    def test_recover_puts_back_what_a_killed_run_changed(self):
        original = self.read_main()
        new = os.path.join(self.mod, "new.lua")
        self.killed_run([
            {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "shown", "replace": "gone"},
            {"do": "edit_file", "path": "emf/mods/my_first_mod/new.lua", "write": "x"},
        ])
        self.assertNotEqual(self.read_main(), original)
        lines = []
        emf_scenario.recover(os.path.dirname(self.out), echo=lines.append, alive=lambda pid: True)
        self.assertNotEqual(self.read_main(), original, "a live run's files are left alone")
        emf_scenario.recover(os.path.dirname(self.out), echo=lines.append, alive=lambda pid: False)
        self.assertEqual(self.read_main(), original)
        self.assertFalse(os.path.exists(new))
        self.assertFalse(os.path.exists(os.path.join(self.out, "restore.json")))
        self.assertIn("put back", "\n".join(lines))

    def test_recover_leaves_a_file_changed_since(self):
        self.killed_run([
            {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "shown", "replace": "gone"}])
        with open(self.main_lua, "wb") as f:
            f.write(b"the modder's own work\n")
        lines = []
        emf_scenario.recover(os.path.dirname(self.out), echo=lines.append, alive=lambda pid: False)
        self.assertEqual(self.read_main(), b"the modder's own work\n")
        self.assertIn("changed since", "\n".join(lines))


class LogMarkTests(Base):
    def test_match_after_the_mark_only(self):
        self.bridge.add_log("[my_first_mod] reloaded (its files changed)")  # before the mark: does not count

        def edit(text):
            self.bridge.add_log("[my_first_mod] reloaded (its files changed)")
        r = self.run_steps([
            {"do": "log_mark", "name": "m"},
            {"do": "log_since_mark", "mark": "m", "match": "reloaded"},
        ])
        self.assertEqual(self.statuses(r), ["ok", "fail"])
        self.bridge.console_hook = edit
        r = self.run_steps([
            {"do": "log_mark", "name": "m"},
            {"do": "console", "text": "pos"},
            {"do": "log_since_mark", "mark": "m", "match": "[my_first_mod] reloaded", "timeout_s": 5},
            {"do": "log_since_mark", "mark": "m", "match_re": r"reloaded \(.*\)"},
            {"do": "log_since_mark", "mark": "m", "match": "error", "absent": True},
        ])
        self.assertEqual(self.statuses(r), ["ok"] * 5, self.text())
        wait_calls = [c for c in self.bridge.calls if c[0] == "wait"]
        self.assertEqual(wait_calls[0][1]["since"], 1)

    def test_absent_fails_when_present(self):
        def edit(text):
            self.bridge.add_log("[lua] my_first_mod/main.lua:12: error near 'local'")
        self.bridge.console_hook = edit
        r = self.run_steps([
            {"do": "log_mark", "name": "m"},
            {"do": "console", "text": "pos"},
            {"do": "log_since_mark", "mark": "m", "match": "error", "absent": True},
        ])
        self.assertEqual(self.statuses(r), ["ok", "ok", "fail"])
        self.assertIn("main.lua:12", r["steps"][2]["detail"])

    def test_regex_waits_by_polling(self):
        n = {"calls": 0}

        def later(f):
            n["calls"] += 1
            if n["calls"] == 3:
                self.bridge.add_log("[x] value 42")
            return FakeBridge._log(self.bridge, f)
        self.bridge.handlers["log"] = later
        r = self.run_steps([
            {"do": "log_mark", "name": "m"},
            {"do": "log_since_mark", "mark": "m", "match_re": r"value \d+", "timeout_s": 5},
        ])
        self.assertEqual(self.statuses(r), ["ok", "ok"], self.text())


class FlowTests(Base):
    def test_first_failure_skips_the_rest(self):
        r = self.run_steps([{"do": "console", "text": "nosuch"}, {"do": "hello"}, {"do": "hello"}])
        self.assertEqual(self.statuses(r), ["fail", "skipped", "skipped"])
        self.assertFalse(r["ok"])
        self.assertEqual((r["passed"], r["failed"], r["skipped"]), (0, 1, 2))
        self.assertIn("FAIL", self.text())
        self.assertIn("1 failed", self.text())

    def test_an_unexpected_error_is_a_failure(self):
        original = self.read_main()

        def broken(f):
            raise RuntimeError("a reply the runner did not expect")
        self.bridge.handlers["mods"] = broken
        r = self.run_steps([
            {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "shown", "replace": "gone"},
            {"do": "mods", "id": "clock"},
            {"do": "hello"},
        ])
        self.assertEqual(self.statuses(r), ["ok", "fail", "skipped"])
        self.assertIn("internal error", r["steps"][1]["detail"])
        self.assertIn("did not expect", r["steps"][1]["detail"])
        self.assertEqual(self.read_main(), original)

    def test_keep_going(self):
        r = self.run_steps([{"do": "console", "text": "nosuch"}, {"do": "hello"}], keep_going=True)
        self.assertEqual(self.statuses(r), ["fail", "ok"])

    def test_labels_are_printed(self):
        self.run_steps([{"do": "hello", "label": "the game answers"}])
        self.assertIn("the game answers", self.text())
        self.assertTrue(self.lines[0].startswith("ok "))


class MainTests(Base):
    def write_scenario(self, scenario, name="s.json"):
        path = os.path.join(self.tmp, name)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(scenario, f)
        return path

    def main(self, argv, factory=None):
        out = io.StringIO()
        err = io.StringIO()
        code = emf_scenario.main(argv, bridge_factory=factory or (lambda game, pid: self.bridge), stdout=out,
                                 stderr=err)
        return code, out.getvalue(), err.getvalue()

    def test_exit_codes(self):
        good = self.write_scenario({"name": "good", "steps": [{"do": "hello"}]})
        bad = self.write_scenario({"name": "bad", "steps": [{"do": "console", "text": "nosuch"}]}, "b.json")
        code, out, _ = self.main([good, "--game", self.game, "--out", self.out])
        self.assertEqual(code, 0, out)
        self.assertIn("1 passed", out)
        code, out, _ = self.main([bad, "--game", self.game, "--out", self.out])
        self.assertEqual(code, 1)

        def no_game(game, pid):
            raise ConnectError("the game is not running")
        code, out, err = self.main([good, "--out", self.out], factory=no_game)
        self.assertEqual(code, 2)
        self.assertIn("not running", err)

    def test_invalid_scenario_exits_3_before_connecting(self):
        broken = self.write_scenario({"name": "x", "steps": [{"do": "fly"}]})
        called = []
        code, _, err = self.main([broken], factory=lambda g, p: called.append(1))
        self.assertEqual(code, 3)
        self.assertIn("fly", err)
        self.assertFalse(called)
        path = os.path.join(self.tmp, "notjson.json")
        with open(path, "w") as f:
            f.write("{nope")
        code, _, err = self.main([path])
        self.assertEqual(code, 3)
        code, _, err = self.main([os.path.join(self.tmp, "missing.json")])
        self.assertEqual(code, 3)

    def test_recover_only(self):
        run = os.path.join(self.tmp, "runs", "old_run")
        os.makedirs(os.path.join(run, "backups"))
        backup = os.path.join(run, "backups", "05_main.lua")
        with open(backup, "wb") as f:
            f.write(b"the original\n")
        with open(self.main_lua, "wb") as f:
            f.write(b"what the run wrote\n")
        import hashlib
        with open(os.path.join(run, "restore.json"), "w", encoding="utf-8") as f:
            json.dump({"pid": 999999, "game": self.game, "files": [  # 999999: no Windows pid (they are multiples of 4)
                {"rel": "emf/mods/my_first_mod/main.lua", "full": self.main_lua, "backup": backup,
                 "written": hashlib.sha256(b"what the run wrote\n").hexdigest()}]}, f)
        os.environ["EMF_SCENARIO_ROOT"] = os.path.join(self.tmp, "own_root")
        try:
            code, out, _ = self.main(["--recover", "--out", run])  # that run folder's own journal
        finally:
            del os.environ["EMF_SCENARIO_ROOT"]
        self.assertEqual(code, 0)
        self.assertIn("put back emf/mods/my_first_mod/main.lua", out)
        self.assertEqual(self.read_main(), b"the original\n")

    def test_check_only(self):
        good = self.write_scenario({"name": "good", "steps": [{"do": "hello"}]})
        called = []
        code, out, _ = self.main(["--check", good], factory=lambda g, p: called.append(1))
        self.assertEqual(code, 0)
        self.assertIn("valid", out)
        self.assertFalse(called)

    def test_json_report(self):
        good = self.write_scenario({"name": "good", "steps": [{"do": "hello"}, {"do": "screenshot", "name": "s"}]})
        report_path = os.path.join(self.tmp, "report.json")
        code, out, _ = self.main([good, "--game", self.game, "--out", self.out, "--json", report_path])
        self.assertEqual(code, 0, out)
        with open(report_path, encoding="utf-8") as f:
            report = json.load(f)
        self.assertEqual(report["scenario"], "good")
        self.assertTrue(report["ok"])
        self.assertEqual([s["do"] for s in report["steps"]], ["hello", "screenshot"])
        self.assertEqual(report["out_dir"], self.out)
        self.assertEqual(report["exit_code"], 0)

    def test_json_to_stdout_moves_the_text_to_stderr(self):
        good = self.write_scenario({"name": "good", "steps": [{"do": "hello"}]})
        code, out, err = self.main([good, "--game", self.game, "--out", self.out, "--json", "-"])
        self.assertEqual(code, 0)
        self.assertTrue(json.loads(out)["ok"])
        self.assertIn("ok ", err)

    def test_game_folder_from_the_bridge(self):
        self.assertEqual(emf_scenario.game_from_exe(r"C:\Games\Enshrouded\enshrouded.exe"), r"C:\Games\Enshrouded")
        self.assertIsNone(emf_scenario.game_from_exe(""))

    def test_closes_the_bridge(self):
        good = self.write_scenario({"name": "good", "steps": [{"do": "hello"}]})
        self.main([good, "--game", self.game, "--out", self.out])
        self.assertTrue(self.bridge.closed)


def sha(data):
    import hashlib
    return hashlib.sha256(data).hexdigest()


class ModFileRuleTests(Base):
    def test_streams_devices_and_trailing_dots_are_refused(self):
        for path in ("emf/mods/my_first_mod/main.lua:ads", "emf/mods/my_first_mod/main.lua::$DATA",
                     "emf/mods/my_first_mod/CON", "emf/mods/my_first_mod/con.txt", "emf/mods/my_first_mod/LPT1.lua",
                     "emf/mods/my_first_mod/main.lua.", "emf/mods/my_first_mod/main.lua ", "emf/mods/my_first_mod. /x.lua"):
            with self.subTest(path=path):
                r = self.run_steps([{"do": "edit_file", "path": path, "write": "x"}])
                self.assertEqual(self.statuses(r), ["fail"])
                self.assertIn("emf\\mods\\<id>\\", r["steps"][0]["detail"])

    def test_a_folder_named_dot_dot_x_is_a_mod_folder(self):
        os.makedirs(os.path.join(self.game, "emf", "mods", "..x"))
        r = self.run_steps([{"do": "edit_file", "path": "emf/mods/..x/a.lua", "write": "x"}])
        self.assertEqual(self.statuses(r), ["ok"], self.text())


class RecoverRuleTests(Base):
    """recover() trusts nothing in a journal: each file must be a mod file of the journal's game, each backup must be
    in that run's own backups folder, each hash must be a SHA-256."""

    def journal(self, run, files, pid=999999):
        os.makedirs(os.path.join(run, "backups"), exist_ok=True)
        with open(os.path.join(run, "restore.json"), "w", encoding="utf-8") as f:
            json.dump({"pid": pid, "game": self.game, "files": files}, f)

    def backup(self, run, name, data):
        path = os.path.join(run, "backups", name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def recover(self, root):
        lines = []
        failures = emf_scenario.recover(root, echo=lines.append, alive=lambda pid: False)
        return failures, "\n".join(lines)

    def test_recover_refuses_a_file_outside_the_mods(self):
        root = os.path.join(self.tmp, "root")
        run = os.path.join(root, "run1")
        outside = os.path.join(self.tmp, "victim.txt")
        with open(outside, "wb") as f:
            f.write(b"what a run wrote")
        backup = self.backup(run, "01_victim.txt", b"attacker's text")
        self.journal(run, [{"rel": "x", "full": outside, "backup": backup, "written": sha(b"what a run wrote")}])
        failures, text = self.recover(root)
        with open(outside, "rb") as f:
            self.assertEqual(f.read(), b"what a run wrote")
        self.assertEqual(failures, 1)
        self.assertIn("not a mod file", text)

    def test_recover_refuses_a_backup_from_elsewhere(self):
        root = os.path.join(self.tmp, "root")
        run = os.path.join(root, "run1")
        with open(self.main_lua, "wb") as f:
            f.write(b"what a run wrote")
        elsewhere = os.path.join(self.tmp, "elsewhere.lua")
        with open(elsewhere, "wb") as f:
            f.write(b"some other file")
        self.journal(run, [{"rel": "emf/mods/my_first_mod/main.lua", "full": self.main_lua, "backup": elsewhere,
                            "written": sha(b"what a run wrote")}])
        failures, text = self.recover(root)
        self.assertEqual(self.read_main(), b"what a run wrote")
        self.assertEqual(failures, 1)
        self.assertIn("backup", text)

    def test_recover_refuses_a_bad_hash(self):
        root = os.path.join(self.tmp, "root")
        run = os.path.join(root, "run1")
        backup = self.backup(run, "01_main.lua", b"original")
        for written in (None, "", "abc", "Z" * 64):
            with self.subTest(written=written):
                self.journal(run, [{"rel": "emf/mods/my_first_mod/main.lua", "full": self.main_lua, "backup": backup,
                                    "written": written}])
                before = self.read_main()
                failures, _ = self.recover(root)
                self.assertEqual(self.read_main(), before)
                self.assertEqual(failures, 1)

    def test_recover_only_looks_in_its_own_root(self):
        elsewhere = os.path.join(self.tmp, "elsewhere")
        run = os.path.join(elsewhere, "killed")
        with open(self.main_lua, "wb") as f:
            f.write(b"what a run wrote")
        backup = self.backup(run, "01_main.lua", b"the original")
        self.journal(run, [{"rel": "emf/mods/my_first_mod/main.lua", "full": self.main_lua, "backup": backup,
                            "written": sha(b"what a run wrote")}])
        os.environ["EMF_SCENARIO_ROOT"] = os.path.join(self.tmp, "own_root")
        try:
            scenario = self.write_scenario({"name": "s", "steps": [{"do": "hello"}]})
            code, out, err = MainTests.main(self, [scenario, "--game", self.game, "--out",
                                                   os.path.join(elsewhere, "new")])
            self.assertEqual(code, 0, out + err)
            self.assertEqual(self.read_main(), b"what a run wrote", "a journal next to --out is not scanned")
            code, out, _ = MainTests.main(self, ["--recover", "--out", run])
            self.assertEqual(code, 0, out)
            self.assertEqual(self.read_main(), b"the original")
        finally:
            del os.environ["EMF_SCENARIO_ROOT"]

    def test_recover_exits_1_when_it_cannot_put_a_file_back(self):
        run = os.path.join(self.tmp, "killed")
        with open(self.main_lua, "wb") as f:
            f.write(b"what a run wrote")
        self.journal(run, [{"rel": "emf/mods/my_first_mod/main.lua", "full": self.main_lua,
                            "backup": os.path.join(run, "backups", "01_main.lua"),  # missing
                            "written": sha(b"what a run wrote")}])
        os.environ["EMF_SCENARIO_ROOT"] = os.path.join(self.tmp, "own_root")
        try:
            code, out, _ = MainTests.main(self, ["--recover", "--out", run])
        finally:
            del os.environ["EMF_SCENARIO_ROOT"]
        self.assertEqual(code, 1, out)

    write_scenario = MainTests.write_scenario


class RestoreCheckTests(Base):
    def test_a_file_changed_during_the_run_is_kept_and_reported(self):
        def someone_edits(text):
            with open(self.main_lua, "wb") as f:
                f.write(b"the modder saved this meanwhile\n")
        self.bridge.console_hook = someone_edits
        r = self.run_steps([
            {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "shown", "replace": "gone"},
            {"do": "console", "text": "pos"},
        ])
        self.assertEqual(self.read_main(), b"the modder saved this meanwhile\n")
        self.assertFalse(r["ok"])
        self.assertEqual(r["restore_failed"], ["emf/mods/my_first_mod/main.lua"])
        self.assertIn("changed during the run", self.text())

    def test_restore_file_keeps_a_file_changed_during_the_run(self):
        def someone_edits(text):
            with open(self.main_lua, "wb") as f:
                f.write(b"theirs\n")
        self.bridge.console_hook = someone_edits
        r = self.run_steps([
            {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "shown", "replace": "gone"},
            {"do": "console", "text": "pos"},
            {"do": "restore_file", "path": "emf/mods/my_first_mod/main.lua"},
        ])
        self.assertEqual(self.statuses(r), ["ok", "ok", "fail"])
        self.assertEqual(self.read_main(), b"theirs\n")

    def test_the_journal_is_written_before_the_file_changes(self):
        order = []
        real_write = emf_scenario.Runner._write

        def watch(full, data):
            if full == self.main_lua:
                order.append(os.path.isfile(os.path.join(self.out, "restore.json")))
            return real_write(full, data)
        emf_scenario.Runner._write = staticmethod(watch)
        try:
            self.run_steps([{"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "shown",
                             "replace": "gone"}])
        finally:
            emf_scenario.Runner._write = staticmethod(real_write)
        self.assertEqual(order[0], True, "the journal exists when the mod file is first changed")


class OutputFolderTests(unittest.TestCase):
    def test_each_run_gets_its_own_folder(self):
        self.assertNotEqual(emf_scenario.default_out_dir("s"), emf_scenario.default_out_dir("s"))


class LocalWorldRuleTests(Base):
    """World-changing steps (console, mod_key, edit_file) refuse a world that is not a LOCAL save."""

    def test_a_steam_cloud_world_is_refused(self):
        self.bridge.state_data["world"]["slot"] = 257  # 0x0101: slot 1 of the Steam Cloud saves
        for step in ({"do": "console", "text": "pos"}, {"do": "mod_key", "key": "L"},
                     {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "shown", "replace": "x"}):
            with self.subTest(step=step["do"]):
                r = self.run_steps([step])
                self.assertEqual(self.statuses(r), ["fail"])
                self.assertIn("Steam Cloud", r["steps"][0]["detail"])
        self.assertEqual(self.console_calls(), [])

    def test_an_unknown_slot_in_a_running_world_is_refused(self):
        self.bridge.state_data["world"]["slot"] = None
        r = self.run_steps([{"do": "console", "text": "pos"}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("cannot tell", r["steps"][0]["detail"])

    def test_the_main_menu_and_a_local_world_are_allowed(self):
        r = self.run_steps([{"do": "console", "text": "pos"}])
        self.assertEqual(self.statuses(r), ["ok"])
        self.bridge.state_data["world"] = {"status": "none", "kind": None, "slot": 257}
        r = self.run_steps([{"do": "console", "text": "pos"}])
        self.assertEqual(self.statuses(r), ["ok"])

    def test_the_override(self):
        self.bridge.state_data["world"]["slot"] = 257
        scenario = emf_scenario.validate({"name": "x", "steps": [{"do": "console", "text": "pos"}]})
        runner = emf_scenario.Runner(self.bridge, game=self.game, out_dir=self.out, echo=self.lines.append,
                                     allow_cloud_world=True)
        self.assertTrue(runner.run(scenario)["ok"])
        local = {"storage": "local", "slot": 1, "name": "Character 2"}
        self.assertEqual(emf_scenario.world_change_refusal({"world": {"status": "running", "slot": 1},
                                                            "character": local}), None)
        self.assertIn("can't tell which character", emf_scenario.world_change_refusal(
            {"world": {"status": "running", "slot": 1}}))  # no character field: an older EMF
        self.assertIn("Steam Cloud", emf_scenario.world_change_refusal({"world": {"status": "running", "slot": 258}}))


class CharacterRuleTests(Base):
    """World-changing steps need the running world's character to be a LOCAL one (state.character); a Steam
    character, or one the bridge cannot tell, refuses them. Read-only steps stay allowed."""

    WORLD_CHANGING = ({"do": "console", "text": "pos"}, {"do": "mod_key", "key": "L"},
                      {"do": "edit_file", "path": "emf/mods/my_first_mod/main.lua", "find": "shown", "replace": "x"})

    def refused_with(self, words):
        for step in self.WORLD_CHANGING:
            with self.subTest(step=step["do"]):
                r = self.run_steps([step])
                self.assertEqual(self.statuses(r), ["fail"])
                self.assertIn(words, r["steps"][0]["detail"])
        self.assertEqual(self.console_calls(), [])
        self.assertEqual([c for c in self.bridge.calls if c[0] == "mod_key"], [])

    def test_a_steam_character_is_refused(self):
        self.bridge.state_data["character"] = {"storage": "steam", "slot": 1, "name": "Steam Hero"}
        self.refused_with("the running world uses a Steam character")

    def test_an_unknown_or_missing_character_is_refused(self):
        self.bridge.state_data["character"] = {"storage": "unknown", "slot": None, "name": None}
        self.refused_with("can't tell which character runs: go back to the main menu and use load_world")
        del self.bridge.state_data["character"]  # an EMF build without the field
        self.refused_with("can't tell which character runs")

    def test_read_only_steps_stay_allowed(self):
        self.bridge.state_data["character"] = {"storage": "steam", "slot": 1, "name": "Steam Hero"}
        r = self.run_steps([{"do": "hello"}, {"do": "state", "path": "world.status", "op": "==", "value": "running"},
                            {"do": "wait", "for": "world"}, {"do": "mods", "id": "clock"},
                            {"do": "screenshot", "name": "s"}, {"do": "log_mark", "name": "m"}])
        self.assertTrue(r["ok"], self.text())

    def test_the_main_menu_and_the_dedicated_server(self):
        self.bridge.at_main_menu()
        self.bridge.state_data["character"] = {"storage": "steam", "slot": 1, "name": "Steam Hero"}
        self.assertEqual(self.statuses(self.run_steps([{"do": "console", "text": "pos"}])), ["ok"])  # no world
        self.bridge.state_data["world"] = {"status": "running", "kind": "dedicated", "slot": 1, "name": None}
        self.bridge.state_data["character"] = None  # the server has no character of its own
        self.assertEqual(self.statuses(self.run_steps([{"do": "console", "text": "pos"}])), ["ok"])

    def test_the_scenarios_expected_name(self):
        self.bridge.state_data["character"] = {"storage": "local", "slot": 3, "name": "Someone"}
        scenario = {"name": "x", "steps": [{"do": "console", "text": "pos"}]}
        runner = emf_scenario.Runner(self.bridge, game=self.game, out_dir=self.out, echo=self.lines.append)
        self.assertTrue(runner.run(emf_scenario.validate(dict(scenario)))["ok"])  # no name given: LOCAL is enough
        scenario["load_world"] = {"expect_name": "Character 2"}
        runner = emf_scenario.Runner(self.bridge, game=self.game, out_dir=self.out, echo=self.lines.append)
        r = runner.run(emf_scenario.validate(scenario))
        self.assertFalse(r["ok"])
        self.assertIn("not 'Character 2'", r["steps"][0]["detail"])

    def test_the_override_lifts_it(self):
        self.bridge.state_data["character"] = {"storage": "steam", "slot": 1, "name": "Steam Hero"}
        runner = emf_scenario.Runner(self.bridge, game=self.game, out_dir=self.out, echo=self.lines.append,
                                     allow_cloud_world=True)
        self.assertTrue(runner.run(emf_scenario.validate({"name": "x", "steps": [{"do": "console", "text": "pos"}]}))["ok"])

    def test_load_world_running_already_needs_the_local_character(self):
        self.bridge.state_data["world"] = {"status": "running", "kind": "server", "slot": 1, "name": "My World 1"}
        for character, words in (({"storage": "steam", "slot": 1, "name": "Steam Hero"}, "Steam character"),
                                 ({"storage": "unknown", "slot": None, "name": None}, "can't tell which character"),
                                 ({"storage": "local", "slot": 2, "name": "Someone"}, "not 'Character 2'")):
            with self.subTest(character=character):
                self.bridge.state_data["character"] = character
                r = self.run_steps([{"do": "load_world"}])
                self.assertEqual(self.statuses(r), ["fail"])
                self.assertIn(words, r["steps"][0]["detail"])
        self.assertEqual([c for c in self.bridge.calls if c[0] == "load_world"], [])

    def test_load_world_checks_the_character_after_the_start(self):
        self.bridge.at_main_menu()
        self.bridge.loaded_character = {"storage": "steam", "slot": 1, "name": "Steam Hero"}
        r = self.run_steps([{"do": "load_world"}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertIn("WRONG WORLD", r["steps"][0]["detail"])
        self.assertIn("Steam character", r["steps"][0]["detail"])
        self.bridge.at_main_menu()
        self.bridge.loaded_character = {"storage": "unknown", "slot": None, "name": None}
        r = self.run_steps([{"do": "load_world"}])
        self.assertEqual(self.statuses(r), ["fail"])
        self.assertNotIn("WRONG WORLD", r["steps"][0]["detail"])
        self.assertIn("can't tell which character", r["steps"][0]["detail"])


if __name__ == "__main__":
    unittest.main()
