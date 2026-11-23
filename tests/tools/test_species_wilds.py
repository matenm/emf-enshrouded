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
local sp = require("species")
for _, s in ipairs(sp.list) do
  if s.carrier then print(string.format("carrier %s %s %.4f %s", s.id, s.template, s.scale, tostring(s.harness))) end
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
        self.assertEqual(len(beasts), 29)  # 20, and the lion, hyena and jackal with two coats each
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
        self.assertNotIn("wilds_camel", names)  # a pack beast, never wild
        self.assertNotIn("wilds_wolf", names)

    def test_the_game_kinds(self):
        kinds = {line[1] for line in self.lines if line[0] == "pool"}
        self.assertEqual(kinds, {"wolf", "sabretooth", "snow_sabretooth", "goat", "mountain_goat", "yak", "catdeer",
                                 "crocodile"})

    def test_the_wild_ox_stands_in_for_the_yak(self):
        pools = {line[3]: line[1] for line in self.lines if line[0] == "pool"}
        for name in ("wilds_ox", "wilds_ox_white", "wilds_ox_black", "wilds_ox_red"):
            with self.subTest(name=name):
                self.assertEqual(pools.get(name), "yak")


    def test_the_second_set_stands_in_for_the_game_creatures_it_was_fitted_on(self):
        pools = {line[3]: (line[1], line[2]) for line in self.lines if line[0] == "pool"}
        want = {
            "wilds_hyena": ("wolf", "animals"), "wilds_hyena_brown": ("wolf", "coats"),
            "wilds_hyena_pale": ("wolf", "coats"), "wilds_jackal": ("wolf", "animals"),
            "wilds_jackal_dark": ("wolf", "coats"), "wilds_jackal_silver": ("wolf", "coats"),
            "wilds_lion": ("sabretooth", "animals"), "wilds_lion_white": ("sabretooth", "coats"),
            "wilds_lion_dark": ("sabretooth", "coats"),
            "wilds_ibex": ("goat", "animals"), "wilds_ibex_white": ("goat", "coats"),
            "wilds_ibex_dark": ("goat", "coats"),
            "wilds_bison": ("yak", "animals"), "wilds_bison_white": ("yak", "coats"),
            "wilds_bison_black": ("yak", "coats"), "wilds_muskox": ("yak", "animals"),
            "wilds_antelope": ("catdeer", "animals"), "wilds_antelope_sable": ("catdeer", "coats"),
            "wilds_antelope_white": ("catdeer", "coats"), "wilds_mule": ("catdeer", "animals"),
            "wilds_mule_grey": ("catdeer", "coats"), "wilds_mule_black": ("catdeer", "coats"),
            "wilds_lizard": ("crocodile", "animals"),
            "wilds_muskox_white": ("yak", "coats"), "wilds_muskox_dark": ("yak", "coats"),
            "wilds_lizard_green": ("crocodile", "coats"), "wilds_lizard_sand": ("crocodile", "coats"),
            "wilds_lizard_black": ("crocodile", "coats"),
        }
        for name, where in want.items():
            with self.subTest(name=name):
                self.assertEqual(pools.get(name), where)


    def test_the_pack_beasts_are_the_pack_creatures_with_their_template_and_size(self):
        carriers = {line[1]: line[2:] for line in self.lines if line[0] == "carrier"}
        want = {"packllama": ("wilds_packllama", "Item_Beastmaster_Llama_Harness"),
                "packcamel": ("wilds_camel", "Item_Beastmaster_Camel_Saddle")}
        self.assertEqual(set(carriers), set(want))
        for sid, (creature, harness) in want.items():
            with self.subTest(carrier=sid):
                template, scale, item = carriers[sid]
                self.assertEqual(template, self.pack[creature][0])
                self.assertAlmostEqual(float(scale), self.pack[creature][1], places=4)
                self.assertEqual(item, harness)


if __name__ == "__main__":
    unittest.main()
