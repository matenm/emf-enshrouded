"""A fake debug bridge for the tools' unit tests: answers Bridge.request(cmd, **fields) the way the game's bridge does
(docs/modding/debug_bridge.md), from data a test sets up. No pipe, no game."""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools", "bridge"))

from emf_bridge import BridgeError, ConnectError  # noqa: E402

# The smallest valid PNG (1x1), what the fake's screenshots contain.
PNG_1X1 = bytes.fromhex(
    "89504e470d0a1a0a0000000d4948445200000001000000010806000000"
    "1f15c4890000000d49444154789c6360000002000154a24f5d0000000049454e44ae426082")


def default_state():
    return {
        "world": {"status": "running", "kind": "server", "tick": 100, "ecs_time_s": 1.5, "time_of_day_h": 13.25,
                  "slot": 1},
        "character": {"storage": "local", "slot": 1, "name": "Character 2"},
        "player": {"id": 5, "position": {"x": 100.5, "y": 50.0, "z": -20.25},
                   "health": {"value": 180, "max": None, "source": "server Health slot 0"}},
        "overlay": {"running": True, "console_open": False, "wheel_open": False,
                    "hud": [{"id": "clock", "text": "13:15", "place": 1},
                            {"id": "my_first_mod.line", "text": "Hello  (1, 2, 3)", "place": 2}]},
        "mods": [{"id": "clock", "name": "Clock", "version": "1.1.0", "status": "loaded", "reason": ""}],
        "hooks": {"total": 11, "emf": 9, "mods": 2},
        "ticks": {"server": {"steps": 50, "interval_ms": 16.7}, "client": None},
        "frame": None,
        "log_seq": 0,
    }


class FakeBridge:
    """Answers like the game's bridge. Tests change .state_data, .console_replies, .log_lines, .mods_list, or put a
    function into .handlers[cmd] (fields -> reply dict) to answer one command differently."""

    def __init__(self, game=None):
        self.game = game
        self.calls = []
        self.handlers = {}
        self.state_data = default_state()
        self.state_hook = None  # called before each state reply (to change the state over time)
        self.console_replies = {"pos": ["player 5 at (100.5, 50.0, -20.2) m"]}
        self.console_hook = None  # text -> None (side effects, e.g. a hud line appears)
        self.log_lines = []  # [(seq, line)]
        self.mods_list = [
            {"id": "clock", "name": "Clock", "version": "1.1.0", "status": "loaded", "reason": ""},
            {"id": "lua", "name": "Lua", "version": "1.3.0", "status": "loaded", "reason": ""},
            {"id": "quick_stack", "name": "Quick Stack", "version": "1.0.0", "status": "disabled",
             "reason": "switched off"},
        ]
        self.screens = 0
        self.closed = False
        self.lost_after = None  # raise ConnectError on this many-th request (1-based)
        # load_world: the LOCAL saves (1-based slot -> name); after a start the state shows .loaded_slot (default: the
        # slot asked for; a test sets another one, e.g. 257, to make a wrong world come up) and .loaded_name
        self.local_worlds = {1: "My World 1"}
        self.local_characters = {1: "Character 2"}
        self.loaded_slot = None
        self.loaded_name = None
        self.loaded_character = None  # the state's character after a start (default: the LOCAL one asked for)

    def at_main_menu(self):
        """The state of a game at the main menu (no world, no player)."""
        self.state_data["world"] = {"status": "none", "kind": None, "tick": None, "slot": None, "name": None}
        self.state_data["player"] = None

    # the log
    def add_log(self, line):
        seq = (self.log_lines[-1][0] if self.log_lines else 0) + 1
        self.log_lines.append((seq, line))
        return seq

    def last_seq(self):
        return self.log_lines[-1][0] if self.log_lines else 0

    def close(self):
        self.closed = True

    def request(self, cmd, **fields):
        self.calls.append((cmd, fields))
        if self.lost_after is not None and len(self.calls) >= self.lost_after:
            raise ConnectError("the game closed the connection")
        handler = self.handlers.get(cmd) or getattr(self, "_" + cmd, None)
        if handler is None:
            reply = {"ok": False, "code": "unknown_command", "error": "unknown command '%s'" % cmd}
        else:
            reply = handler(fields)
        reply.setdefault("ok", True)
        if not reply["ok"]:
            raise BridgeError(reply)
        return reply

    # commands
    def _hello(self, f):
        return {"emf": "0.3.0", "api": 16, "game_build": 1273043, "process": "client", "pid": 4242, "protocol": 1}

    def _console(self, f):
        text = f["text"]
        if self.console_hook:
            self.console_hook(text)
        if text in self.console_replies:
            out = self.console_replies[text]
            if isinstance(out, dict):
                return dict(out)
            return {"status": "ran", "output": list(out), "log_seq": self.last_seq()}
        name = text.split()[0] if text.split() else ""
        return {"status": "unknown", "output": ["unknown command '%s' (type help)" % name], "log_seq": self.last_seq()}

    def _state(self, f):
        if self.state_hook:
            self.state_hook()
        s = dict(self.state_data)
        s["log_seq"] = self.last_seq()
        return s

    def _mods(self, f):
        return {"mods": list(self.mods_list)}

    def _log(self, f):
        since = f.get("since")
        mx = f.get("max", 1000 if since is not None else 100)
        if since is None:
            last = self.last_seq()
            since = max(0, last - mx)
        lines = [(s, l) for (s, l) in self.log_lines if s > since]
        more = len(lines) > mx
        lines = lines[:mx]
        last = lines[-1][0] if lines else since
        return {"lines": [{"seq": s, "line": l} for (s, l) in lines], "last": last, "more": more, "following": False}

    def _wait(self, f):
        what = f["for"]
        if what == "world":
            if self.state_data["world"]["status"] == "running":
                return {}
        elif what == "player":
            if self.state_data["world"]["status"] == "running" and self.state_data["player"]:
                return {}
        elif what == "log":
            since = f.get("since", self.last_seq())
            for s, l in self.log_lines:
                if s > since and f["match"] in l:
                    return {"seq": s, "line": l}
        else:
            return {"ok": False, "code": "bad_request", "error": "wait needs \"for\": world, player or log"}
        return {"ok": False, "code": "timeout", "error": "timed out"}

    def _screenshot(self, f):
        self.screens += 1
        folder = os.path.join(self.game, "emf", "screens")
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, "2026-10-26_20-15-%02d-000.png" % self.screens)
        with open(path, "wb") as out:
            out.write(PNG_1X1)
        w = f.get("max_width", 3840)
        return {"path": path, "width": w, "height": w * 9 // 16, "hdr": True}

    def _load_world(self, f):
        """Like the game: from the main menu only, LOCAL slots that exist, the expected name; then the world runs."""
        if self.state_data["world"]["status"] != "none":
            return {"ok": False, "code": "unavailable",
                    "error": "not at the main menu: a world is running (quit to the menu first)"}
        slot, character = f["slot"], f["character"]
        if slot not in self.local_worlds:
            return {"ok": False, "code": "refused", "error": "LOCAL world slot %d does not exist" % slot}
        if character not in self.local_characters:
            return {"ok": False, "code": "refused", "error": "LOCAL character slot %d does not exist" % character}
        name = self.local_characters[character]
        if f.get("expect_name") and f["expect_name"] != name:
            return {"ok": False, "code": "refused",
                    "error": "LOCAL character slot %d is '%s', not '%s'" % (character, name, f["expect_name"])}
        world = self.loaded_name or self.local_worlds[slot]
        self.state_data["world"] = {"status": "running", "kind": "server", "tick": 1, "ecs_time_s": 0.1,
                                    "time_of_day_h": 9.0,
                                    "slot": self.loaded_slot if self.loaded_slot is not None else slot,
                                    "name": world}
        self.state_data["player"] = {"id": 5, "position": {"x": 1.0, "y": 2.0, "z": 3.0}, "health": None}
        self.state_data["character"] = (dict(self.loaded_character) if self.loaded_character is not None else
                                        {"storage": "local", "slot": character, "name": name})
        return {"slot": slot, "character": character, "world": self.local_worlds[slot], "character_name": name,
                "ms": 12109}

    def _mod_key(self, f):
        key = f["key"]
        if key == "Banana":
            return {"ok": False, "code": "bad_request", "error": "unknown key 'Banana'"}
        reads = 9 if key == "L" else 0
        return {"key": key, "vk": ord(key[0]), "action": f.get("action", "press"), "mod_reads": reads}
