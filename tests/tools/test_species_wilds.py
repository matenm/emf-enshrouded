"""mods/beastmaster/species_wilds.lua (the wilds pack's beasts and the game creatures they replace) against the pack,
tools/models/packs/beastmaster_wilds.json: the GUIDs and scales are the pack's, never made up."""
import json
import pathlib
import shutil
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]

DUMP = r"""
package.path = FOLDER .. "/?.lua;" .. package.path
local w = require("species_wilds")
for _, b in ipairs(w.beasts) do
  print(string.format("beast %s %s %s %.4f %s", b.id, b.creature, b.template, b.scale, tostring(b.wilds)))
end
for _, k in ipairs(w.kinds) do
  for _, label in ipairs({ "animals", "coats" }) do
    for _, e in ipairs(k[label]) do print(string.format("pool %s %s %s %s", k.id, label, e.name, e.template)) end
  end
end
"""


def pack_creatures():
    pack = json.loads((REPO / "tools" / "models" / "packs" / "beastmaster_wilds.json").read_text(encoding="utf-8"))
    out = {}
    for c in pack["creatures"]:
        scale = 1.0
        for t in c.get("template_add", []):
            if t.get("type") == "ScaleComponent":
                scale = t["value"]["uniformScale"]
        out[c["name"]] = (c["template_guid"], scale)
    return out


@unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
class SpeciesWildsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        folder = (REPO / "mods" / "beastmaster").as_posix()
        code = 'FOLDER = "%s"\n' % folder + DUMP
        out = subprocess.run(["lua", "-e", code], capture_output=True, text=True, timeout=60,
                             stdin=subprocess.DEVNULL)
        if out.returncode != 0:
            raise AssertionError(out.stdout + out.stderr)
        cls.lines = [line.split() for line in out.stdout.splitlines()]
        cls.pack = pack_creatures()

    def test_every_beast_is_a_pack_creature_with_its_template_and_scale(self):
        beasts = [line for line in self.lines if line[0] == "beast"]
        self.assertEqual(len(beasts), 20)
        for _, sid, creature, template, scale, wilds in beasts:
            with self.subTest(beast=sid):
                self.assertIn(creature, self.pack)
                self.assertEqual(template, self.pack[creature][0])
                self.assertAlmostEqual(float(scale), self.pack[creature][1], places=4)
                self.assertEqual(wilds, "true")

    def test_every_pool_entry_is_a_pack_creature_with_its_template(self):
        pools = [line for line in self.lines if line[0] == "pool"]
        self.assertGreater(len(pools), 30)
        for _, kind, label, name, template in pools:
            with self.subTest(kind=kind, name=name):
                self.assertIn(name, self.pack)
                self.assertEqual(template, self.pack[name][0])

    def test_the_llama_and_the_plain_wolf_never_replace_a_game_creature(self):
        names = {line[3] for line in self.lines if line[0] == "pool"}
        self.assertNotIn("wilds_llama", names)
        self.assertNotIn("wilds_wolf", names)

    def test_the_five_game_kinds(self):
        kinds = {line[1] for line in self.lines if line[0] == "pool"}
        self.assertEqual(kinds, {"wolf", "sabretooth", "snow_sabretooth", "goat", "mountain_goat"})


if __name__ == "__main__":
    unittest.main()
