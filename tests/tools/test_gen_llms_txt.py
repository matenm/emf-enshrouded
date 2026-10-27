"""Unit tests of tools/gen_llms_txt.py, and the check that docs/modding/llms.txt is current.
Run: python -m unittest discover -s tests/tools"""

import io
import json
import os
import re
import sys
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools"))
import gen_llms_txt as g  # noqa: E402

SMALL_HEADER = """
typedef struct EmfApi {
    uint32_t version; /* EMF_API_VERSION of the core */
    uint32_t size;    /* sizeof(EmfApi) of the core */

    /* printf-style line into emf/emf_core.log, prefixed with the mod name. Thread-safe. */
    void (*log)(const char* mod, const char* fmt, ...);

    /* Most recently ticking world of a kind, or NULL (no server world on a client). */
    EmfWorld* (*server_world)(void);
    EmfWorld* (*client_world)(void);

    /* ---- v2 ---- */
    /* Wraps an ECS program's update function by program name (see docs/ecs_programs.md):
     * pre(ctx,user) runs before the game's update, post(ctx,user) after. Runs on game worker threads. */
    bool (*hook_program)(const char* name, EmfProgramCallback pre, EmfProgramCallback post,
                         void* user);
} EmfApi;
"""


class CApiTests(unittest.TestCase):
    def test_small_header(self):
        fields = g.parse_c_api(SMALL_HEADER)
        self.assertEqual([f[1] for f in fields], ["version", "size", "log", "server_world", "client_world",
                                                  "hook_program"])
        self.assertEqual([f[0] for f in fields], [1, 1, 1, 1, 1, 2])
        by = {f[1]: f for f in fields}
        self.assertEqual(by["version"][3], "EMF_API_VERSION of the core")
        self.assertEqual(by["log"][2], "void log(const char* mod, const char* fmt, ...)")
        self.assertEqual(by["log"][3], "printf-style line into emf/emf_core.log, prefixed with the mod name.")
        self.assertEqual(by["client_world"][3], "")  # no comment of its own
        self.assertEqual(by["hook_program"][2],
                         "bool hook_program(const char* name, EmfProgramCallback pre, EmfProgramCallback post, "
                         "void* user)")
        self.assertTrue(by["hook_program"][3].startswith("Wraps an ECS program's update function"))

    def test_real_header_has_every_function(self):
        header = g.read(os.path.join("sdk", "include", "emf", "emf_api.h"))
        fields = g.parse_c_api(header)
        names = [f[1] for f in fields]
        body = header[header.index("typedef struct EmfApi {"):header.index("} EmfApi;")]
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)  # "(*size_out = 2088)" in a comment is no field
        self.assertEqual(len([n for n in names if n not in ("version", "size")]), body.count("(*"))
        for n in ("log", "server_world", "hook_pattern", "put_items", "hud_set", "command_sender",
                  "program_write_other"):
            self.assertIn(n, names)
        versions = [f[0] for f in fields]
        self.assertEqual(versions, sorted(versions))
        self.assertEqual(versions[-1], int(re.search(r"#define EMF_API_VERSION (\d+)", header).group(1)))

    def test_first_sentence(self):
        self.assertEqual(g.first_sentence("One. Two."), "One.")
        self.assertEqual(g.first_sentence("Name (e.g. \"a.b\"). Rest."), "Name (e.g. \"a.b\").")
        self.assertTrue(g.first_sentence("word " * 100).endswith("..."))

    def test_defines_and_structs(self):
        header = g.read(os.path.join("sdk", "include", "emf", "emf_api.h"))
        defines = dict(g.parse_defines(header))
        self.assertEqual(defines["EMF_WORLD_SERVER"], "0")
        self.assertEqual(defines["EMF_HUD_BOTTOM_LEFT"], "2u")
        structs, callbacks = g.parse_structs(header)
        names = [s[0] for s in structs]
        self.assertIn("EmfItemsRequest", names)
        self.assertNotIn("EmfApi", names)
        self.assertTrue(any("EmfWorldCallback" in c for c in callbacks))


class OtherSourcesTests(unittest.TestCase):
    def test_builtin_commands_join_split_strings(self):
        cmds = dict(g.parse_builtin_commands(g.read(os.path.join("src", "core", "builtin_commands.cpp"))))
        self.assertIn("help", cmds)
        self.assertTrue(cmds["put"].endswith("yours by default (host)"))
        self.assertTrue(cmds["pos"].startswith(":"))

    def test_other_core_commands(self):
        cmds = dict(g.parse_core_commands(g.REPO))
        self.assertIn("profile", cmds)  # src/core/profile_commands.cpp, not in the built-in table
        self.assertNotIn("help", cmds)  # the built-in table is read on its own
        self.assertTrue(cmds["profile"].startswith("[save <name>"))
        self.assertIn("- profile [save <name>", g.generate())

    def test_mod_commands(self):
        cmds = [(m, n) for m, n, _ in g.parse_mod_commands(g.REPO)]
        self.assertIn(("lua", "lua.status"), cmds)
        for name in ("unstuck", "home", "tp", "bring", "tp.pos", "tp.back", "tp.list"):  # through a helper
            self.assertIn(("teleports", name), cmds)
        # mods the release leaves out (scripts/package.ps1) are not listed
        for mod in ("raids", "villager_jobs", "beastmaster", "necromancer"):
            self.assertNotIn(mod, {m for m, _ in cmds})
        for name in ("quick_stack", "qs", "qs.chests", "qs.mark", "qs.list"):  # through a helper
            self.assertIn(("quick_stack", name), cmds)
        self.assertIn(("loot_magnet", "loot.toggle"), cmds)
        self.assertIn(("champion", "champion.where"), cmds)  # a Lua mod's emf.command, with its id in front
        self.assertIn(("kill_counter", "kill_counter.reset"), cmds)

    def test_every_literal_command_name_of_a_shipped_mod_is_listed(self):
        """Drift check: any "name.word", "help" pair in a shipped mod's sources must come out as a command."""
        listed = {n for _, n, _ in g.parse_mod_commands(g.REPO)}
        pkg = g.read(os.path.join("scripts", "package.ps1"))
        mods = re.findall(r'"(\w+)"', re.search(r"\$playerMods = @\(([^)]*)\)", pkg).group(1))
        for mod in mods:
            folder = os.path.join(g.REPO, "mods", mod)
            for fn in os.listdir(folder):
                if fn.endswith(".cpp"):
                    src = g.read(os.path.join("mods", mod, fn))
                    for name in re.findall(r'\(\s*"(\w+\.[\w.]+)",\s*"[^"]*:', src):
                        with self.subTest(mod=mod, name=name):
                            self.assertIn(name, listed)

    def test_main_menu_commands_come_from_the_sources(self):
        menu = g.parse_menu_commands(g.REPO)
        for name in ("help", "mods", "settings", "set", "hooks", "pattern", "profile"):
            self.assertIn(name, menu)
        self.assertNotIn("give", menu)
        text = g.generate()
        self.assertIn(", ".join(menu) + " also work at the main menu", text)

    def test_the_lua_version_comes_from_the_lua_mod(self):
        with open(os.path.join(g.REPO, "mods", "lua", "mod.json"), encoding="utf-8") as f:
            version = json.load(f)["version"]
        major_minor = ".".join(version.split(".")[:2])
        self.assertIn('"depends": {"lua": ">=%s"}' % major_minor, g.generate())

    def test_wait_and_log_keep_their_details(self):
        text = g.generate()
        bridge = text[text.index("## Debug bridge"):]
        log_line = [l for l in bridge.splitlines() if l.startswith("- log:")][0]
        wait_line = [l for l in bridge.splitlines() if l.startswith("- wait:")][0]
        for word in ("since", "max", "follow", "missed"):
            self.assertIn(word, log_line)
        for word in ("player", "match", "since", "timeout_ms"):
            self.assertIn(word, wait_line)

    def test_lua_registrations_and_docs(self):
        fns, methods = g.parse_lua_registrations(g.read(os.path.join("src", "lua_host", "host.cpp")),
                                                 g.read(os.path.join("src", "lua_host", "bindings.cpp")))
        for n in ("log", "on_key", "key_down", "item", "wheel"):
            self.assertIn(n, fns)
        self.assertIn("account", methods)
        self.assertEqual(g.check_lua_documented(), [], "lua.md describes every function the Lua host registers")

    def test_md_table_rows(self):
        md = "intro\n**t:**\n\n| a | b |\n|---|---|\n| `x` | one \\| two |\n| y | z |\n\nafter | not a row\n"
        self.assertEqual(g.md_table_rows(md, "**t:**"), [["`x`", "one | two"], ["y", "z"]])

    def test_bridge_doc_describes_every_command(self):
        described = [c[0] for c in g.parse_bridge_doc(g.read(os.path.join("docs", "modding", "debug_bridge.md")))]
        core = g.bridge_commands_in_core(g.read(os.path.join("src", "core", "bridge", "service.cpp")))
        self.assertEqual(sorted(described), sorted(core))
        for name, examples, desc in g.parse_bridge_doc(g.read(os.path.join("docs", "modding", "debug_bridge.md"))):
            with self.subTest(command=name):
                self.assertTrue(desc)


class OutputTests(unittest.TestCase):
    def test_generated_text(self):
        text = g.generate()
        self.assertEqual(text, g.generate())  # deterministic
        size = len(text.encode("utf-8"))
        self.assertGreater(size, 15 * 1024)
        self.assertLess(size, 46 * 1024)
        for heading in ("## C API", "## Lua", "## mod.json", "## Console commands", "## Debug bridge",
                        "## Tools for agents"):
            self.assertIn(heading, text)
        self.assertIn("- void log(const char* mod, const char* fmt, ...)", text)
        self.assertIn("emf.on_key", text)
        self.assertIn("enabledWhen", text)

    def test_committed_file_is_current(self):
        out = io.StringIO()
        with redirect_stdout(out):
            code = g.main(["--check"])
        self.assertEqual(code, 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
