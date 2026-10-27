"""Unit tests of tools/bridge/emf_bridge.py's reading: a reply that never comes ends with an error after the command's
own time plus a margin, instead of hanging. A real named pipe served by this test (no game).
Run: python -m unittest discover -s tests/tools"""

import _winapi
import contextlib
import io
import json
import os
import sys
import threading
import time
import unittest
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools", "bridge"))
import emf_bridge  # noqa: E402


class PipeServer:
    """One client; answers each request line with `reply(line)` after `delay` s, or never when reply returns None."""

    def __init__(self, reply, delay=0.0):
        self.path = r"\\.\pipe\emf-test-" + uuid.uuid4().hex
        self.reply = reply
        self.delay = delay
        self.handle = _winapi.CreateNamedPipe(
            self.path, _winapi.PIPE_ACCESS_DUPLEX, _winapi.PIPE_WAIT,  # byte mode (PIPE_TYPE_BYTE = PIPE_READMODE_BYTE = 0)
            1, 65536, 65536, 0, _winapi.NULL)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        try:
            _winapi.ConnectNamedPipe(self.handle, False)
        except OSError:
            pass
        buf = b""
        while not self.stop.is_set():
            try:
                data, _ = _winapi.ReadFile(self.handle, 65536)
            except OSError:
                return
            if not data:
                return
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                answer = self.reply(line.decode())
                if answer is not None:
                    time.sleep(self.delay)
                    _winapi.WriteFile(self.handle, (answer + "\n").encode())

    def close(self):
        self.stop.set()
        try:
            _winapi.CloseHandle(self.handle)
        except OSError:
            pass


class ReadDeadlineTests(unittest.TestCase):
    def connect(self, server):
        return emf_bridge.Bridge.from_file(emf_bridge._open_pipe(server.path, 5), server.path)

    def test_a_reply_that_never_comes_is_an_error(self):
        server = PipeServer(lambda line: None)
        b = self.connect(server)
        try:
            t0 = time.monotonic()
            with self.assertRaises(emf_bridge.ConnectError) as e:
                b.send("hello", reply_timeout=0.5)
            self.assertLess(time.monotonic() - t0, 3)
            self.assertIn("no reply", str(e.exception))
        finally:
            b.close()
            server.close()

    def test_a_late_reply_within_the_time_is_read(self):
        server = PipeServer(lambda line: '{"id":1,"ok":true,"emf":"x"}', delay=0.3)
        b = self.connect(server)
        try:
            self.assertEqual(b.send("hello", reply_timeout=5)["emf"], "x")
        finally:
            b.close()
            server.close()

    def test_the_time_comes_from_the_command(self):
        os.environ["EMF_BRIDGE_REPLY_MARGIN_MS"] = "15000"
        try:
            self.assertAlmostEqual(emf_bridge.reply_timeout_s("hello", {}), 15.0)
            self.assertAlmostEqual(emf_bridge.reply_timeout_s("wait", {"timeout_ms": 120000}), 135.0)
            self.assertAlmostEqual(emf_bridge.reply_timeout_s("wait", {}), 45.0)
            self.assertAlmostEqual(emf_bridge.reply_timeout_s("console", {}), 25.3)
            self.assertAlmostEqual(emf_bridge.reply_timeout_s("console", {"timeout_ms": 2000, "settle_ms": 1000}), 18.0)
            self.assertAlmostEqual(emf_bridge.reply_timeout_s("screenshot", {}), 20.0)
            self.assertAlmostEqual(emf_bridge.reply_timeout_s("mod_key", {"hold_ms": 1000}), 16.0)
            os.environ["EMF_BRIDGE_REPLY_MARGIN_MS"] = "500"
            self.assertAlmostEqual(emf_bridge.reply_timeout_s("hello", {}), 0.5)
        finally:
            del os.environ["EMF_BRIDGE_REPLY_MARGIN_MS"]

    def test_the_default_send_uses_the_command_time(self):
        os.environ["EMF_BRIDGE_REPLY_MARGIN_MS"] = "300"
        server = PipeServer(lambda line: None)
        b = self.connect(server)
        try:
            t0 = time.monotonic()
            with self.assertRaises(emf_bridge.ConnectError):
                b.send("hello")
            self.assertLess(time.monotonic() - t0, 3)
        finally:
            del os.environ["EMF_BRIDGE_REPLY_MARGIN_MS"]
            b.close()
            server.close()


class LoadWorldTests(unittest.TestCase):
    """Bridge.load_world: the request's fields, a reply deadline that fits a world start, and plain-words errors."""

    def connect(self, server):
        return emf_bridge.Bridge.from_file(emf_bridge._open_pipe(server.path, 5), server.path)

    def test_the_reply_time_fits_the_start(self):
        os.environ["EMF_BRIDGE_REPLY_MARGIN_MS"] = "15000"
        try:
            # the game's own 180 s, its 10 s backstop, then the margin
            self.assertAlmostEqual(emf_bridge.reply_timeout_s("load_world", {}), 205.0)
            self.assertAlmostEqual(emf_bridge.reply_timeout_s("load_world", {"timeout_ms": 60000}), 85.0)
        finally:
            del os.environ["EMF_BRIDGE_REPLY_MARGIN_MS"]

    def test_the_fields_and_the_reply(self):
        seen = []

        def answer(line):
            req = json.loads(line)
            seen.append(req)
            return json.dumps({"id": req["id"], "ok": True, "slot": req["slot"], "character": req["character"],
                               "world": "My World 1", "character_name": "Character 2", "ms": 12109})
        server = PipeServer(answer)
        b = self.connect(server)
        try:
            r = b.load_world(1, 1, "Character 2")
            self.assertEqual((r["world"], r["character_name"], r["ms"]), ("My World 1", "Character 2", 12109))
            b.load_world(2, 3, timeout_ms=5000)
        finally:
            b.close()
            server.close()
        self.assertEqual({k: v for k, v in seen[0].items() if k != "id"},
                         {"cmd": "load_world", "slot": 1, "character": 1, "expect_name": "Character 2"})
        self.assertEqual({k: v for k, v in seen[1].items() if k != "id"},
                         {"cmd": "load_world", "slot": 2, "character": 3, "timeout_ms": 5000})

    def test_a_refusal_is_a_load_world_error_in_plain_words(self):
        server = PipeServer(lambda line: json.dumps({"id": json.loads(line)["id"], "ok": False, "code": "refused",
                                                     "error": "LOCAL world slot 9 does not exist"}))
        b = self.connect(server)
        try:
            with self.assertRaises(emf_bridge.LoadWorldError) as e:
                b.load_world(9, 1)
        finally:
            b.close()
            server.close()
        self.assertIsInstance(e.exception, emf_bridge.BridgeError)
        self.assertEqual(e.exception.code, "refused")
        self.assertIn("LOCAL world slot 9 does not exist", str(e.exception))
        self.assertIn("nothing started", str(e.exception))

    def test_every_code_has_its_message(self):
        for code, words in (("refused", "nothing started"), ("unavailable", "main menu"), ("busy", "already"),
                            ("timeout", "may still start"), ("bad_request", "wrong"),
                            ("unknown_command", "no load_world"), ("something_new", "something_new")):
            with self.subTest(code=code):
                text = emf_bridge.load_world_message(code, "the game's words")
                self.assertIn(words, text)
                if code != "unknown_command":
                    self.assertIn("the game's words", text)


class LoadWorldCommandLineTests(unittest.TestCase):
    """python emf_bridge.py load_world <slot> <character> [name] [--timeout-ms n]."""

    class FakeBridge:
        def __init__(self, reply):
            self.reply = reply
            self.sent = []
            self.last_line = json.dumps(reply)

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

        def send(self, cmd, **fields):
            self.sent.append((cmd, fields))
            return dict(self.reply)

    def run_cli(self, argv, reply):
        fake = self.FakeBridge(reply)
        saved = emf_bridge.Bridge
        emf_bridge.Bridge = lambda game=None, pid=None: fake
        out, err = io.StringIO(), io.StringIO()
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = emf_bridge._main(argv)
        finally:
            emf_bridge.Bridge = saved
        return code, out.getvalue(), err.getvalue(), fake.sent

    def test_a_start(self):
        code, out, err, sent = self.run_cli(
            ["load_world", "1", "1", "Character 2", "--timeout-ms", "60000"],
            {"id": 1, "ok": True, "slot": 1, "character": 1, "world": "My World 1", "character_name": "Character 2",
             "ms": 12109})
        self.assertEqual(code, 0, err)
        self.assertEqual(sent, [("load_world", {"slot": 1, "character": 1, "expect_name": "Character 2",
                                                "timeout_ms": 60000})])
        self.assertIn("'My World 1' runs with 'Character 2' (LOCAL world slot 1, character 1, 12109 ms)", out)

    def test_a_refusal_exits_1_with_the_message(self):
        code, out, err, _ = self.run_cli(["load_world", "1", "1", "Someone"],
                                         {"id": 1, "ok": False, "code": "refused",
                                          "error": "LOCAL character slot 1 is 'Character 2', not 'Someone'"})
        self.assertEqual(code, 1)
        self.assertIn("nothing started", err)
        self.assertIn("not 'Someone'", err)

    def test_usage(self):
        with self.assertRaises(SystemExit) as e:
            self.run_cli(["load_world", "1"], {"ok": True})
        self.assertIn("load_world", str(e.exception.code))


class ProcessStartTests(unittest.TestCase):
    def test_start_time_of_a_running_and_a_missing_process(self):
        started = emf_bridge.process_start_epoch(os.getpid())
        self.assertIsNotNone(started)
        self.assertLess(abs(time.time() - started), 600)  # this test's own process started minutes ago at most
        self.assertIsNone(emf_bridge.process_start_epoch(999999))  # no Windows pid (they are multiples of 4)


if __name__ == "__main__":
    unittest.main()
