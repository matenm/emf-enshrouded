"""Unit tests for tools/models (encoder against the decoder; byte-exact against a game model when one is dumped)."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "models"))
import encode  # noqa: E402
import make_mod  # noqa: E402
import rendermodel  # noqa: E402

DUMP = os.path.join(HERE, "..", "..", "analysis", "model_survey", "dump", "pet_dog_01")


def tiny_group():
    pos = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.5), (1.0, 1.0, 1.0)]
    nrm = [(0.0, 0.0, 1.0)] * 4
    tan = [(1.0, 0.0, 0.0, 1.0), (1.0, 0.0, 0.0, -1.0), (0.0, 1.0, 0.0, 1.0), (0.0, 1.0, 0.0, -1.0)]
    uv = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)]
    skin = [([3], [1.0]), ([3, 7], [0.5, 0.5]), ([7, 3, 9], [0.6, 0.3, 0.1]), ([9], [1.0])]
    return {"material": 0, "pos": pos, "nrm": nrm, "tan": tan, "col": None, "uv": uv, "skin": skin,
            "lods": [[(0, 1, 2), (2, 1, 3)]]}


class EncodeTests(unittest.TestCase):
    def test_weights_sum_to_255(self):
        for ws in ([1.0], [0.5, 0.5], [0.6, 0.3, 0.1], [0.34, 0.33, 0.33], [0.25] * 4):
            self.assertEqual(sum(encode.pack_weights(ws)), 255)

    def test_tiny_model_decodes_back(self):
        blob, f = encode.build([tiny_group()])
        model = dict(f, debugName="tiny", jointCount=f["jointCount"])
        d = rendermodel.decode_mesh(model, blob, 0)
        g = tiny_group()
        for a, b in zip(d["pos"], g["pos"]):
            for k in range(3):
                self.assertAlmostEqual(a[k], b[k], places=5)
        self.assertEqual([t[3] for t in d["tan"]], [1.0, -1.0, 1.0, -1.0])
        self.assertEqual(d["tris"], [(0, 1, 2), (2, 1, 3)])
        self.assertEqual(f["skinningMap"], [3, 7, 9])
        self.assertEqual(d["skin"][2][0][:3], [7, 3, 9])
        self.assertEqual(len(blob) % 4, 0)

    @unittest.skipUnless(os.path.exists(DUMP + ".render.bin"), "needs a dumped game model (mods/model_survey)")
    def test_game_dog_round_trip_is_byte_exact(self):
        model = rendermodel.load_record(DUMP + ".json")
        with open(DUMP + ".render.bin", "rb") as f:
            orig = f.read()
        groups = encode.groups_from_model(model, orig)
        box = ([model["positionOffset"][k] for k in "xyz"], [model["positionScale"][k] for k in "xyz"])
        blob, f = encode.build(groups, palette=model["skinningMap"], box=box)
        self.assertEqual(len(blob), len(orig))
        self.assertEqual(blob, orig)
        for key in ("vertexData", "skinningData", "indexData", "meshletData"):
            self.assertEqual(f[key], model[key], key)
        for mine, theirs in zip(f["meshes"], model["meshes"]):
            for key in ("materialIndex", "vertexCount", "indexOffset", "indexCount", "vertexDataByteOffset",
                        "skinVertexOffset"):
                self.assertEqual(mine[key], theirs[key], key)
        self.assertEqual([(l["meshOffset"], l["meshCount"]) for l in f["lods"]],
                         [(l["meshOffset"], l["meshCount"]) for l in model["lods"]])


BEAR = {
    "name": "beast_bear", "model_guid": "m-1", "template_guid": "t-1", "source_model_guid": "src-model",
    "source_template": "Enemy_Wildbeast_wolf_earlygame", "template_name": "Beast_Bear",
    "template_set": {"DamageMod": {"dataStorage[2]": 1.5}, "keen::ecs::Movement": {"dataStorage[4]": 5.1, "dataStorage[5]": {"mul": 0.85}}},
    "template_add": [{"type": "ScaleComponent", "value": {"uniformScale": 1.4}}],
}

STUB_DATA = """return {debugName = "beast_bear", boundingBox = {}, boundingSphere = {}, positionScale = {},
  positionOffset = {}, wholeModelRadius = 0.5, meshes = {{}}, lods = {{}}, skinningMap = {1}, jointCount = 1,
  indexData = {}, meshletData = {}, vertexData = {}, skinningData = {}, blendShapeData = {}}
"""


class MakeModTests(unittest.TestCase):
    def test_template_edits_become_lua_entries(self):
        entry = make_mod.listing_entry(dict(BEAR))
        self.assertEqual(entry["template_set"], [
            {"component": "keen::ecs::DamageMod", "path": ["dataStorage", 3], "value": 1.5},
            {"component": "keen::ecs::Movement", "path": ["dataStorage", 5], "value": 5.1},
            {"component": "keen::ecs::Movement", "path": ["dataStorage", 6], "mul": 0.85}])
        self.assertEqual(entry["template_add"], [{"type": "keen::ecs::ScaleComponent", "value": {"uniformScale": 1.4}}])
        text = make_mod.lua_value(entry)
        self.assertIn('{component = "keen::ecs::DamageMod", path = {"dataStorage", 3}, value = 1.5}', text)
        self.assertIn('{type = "keen::ecs::ScaleComponent", value = {uniformScale = 1.4}}', text)

    def test_a_bad_field_path_is_refused(self):
        for bad in ("data storage", "dataStorage[-1]", "dataStorage[x]", ""):
            with self.assertRaises(SystemExit, msg=bad):
                make_mod.listing_entry(dict(BEAR, template_set={"DamageMod": {bad: 1}}))
        for bad in ({"mul": "2"}, {"times": 2}, [1, 2]):  # a value is a number, text, true/false or {"mul": n}
            with self.assertRaises(SystemExit, msg=str(bad)):
                make_mod.listing_entry(dict(BEAR, template_set={"DamageMod": {"dataStorage[2]": bad}}))

    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_the_generated_mod_applies_the_edits(self):
        with tempfile.TemporaryDirectory() as d:
            make_mod.write_scripts(d, {"id": "test_pack", "title": "Test"}, [make_mod.listing_entry(dict(BEAR))])
            with open(os.path.join(d, "beast_bear_data.lua"), "w", encoding="utf-8") as f:
                f.write(STUB_DATA)
            with open(os.path.join(d, "beast_bear_blob.lua"), "w", encoding="utf-8") as f:
                f.write('return "blob"\n')
            mock = os.path.join(HERE, "..", "..", "tools", "models", "mock_eml.lua")
            out = subprocess.run(["lua", mock, d], capture_output=True, text=True, timeout=60)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("template Beast_Bear keen::ecs::DamageMod dataStorage 0 0.5 1.5 0", out.stdout)
            self.assertIn("template Beast_Bear keen::ecs::Movement dataStorage 0 1 0.5 2.3 5.1 8.5 1.8", out.stdout)
            self.assertIn("template Beast_Bear keen::ecs::ScaleComponent uniformScale 1.4", out.stdout)
            # the source template is left as it was
            self.assertIn("template Enemy_Wildbeast_wolf_earlygame keen::ecs::DamageMod dataStorage 0 0.5 1 0",
                          out.stdout)
            # the source's cloth simulation runs over the source's own vertices: never on our geometry (seen in game:
            # the lich and the bone golem, on the mage's and the knight's models, glitched as they walked)
            self.assertIn("model %s cloth none" % BEAR["model_guid"], out.stdout)
            self.assertIn("model %s cloth kept" % BEAR["source_model_guid"], out.stdout)


if __name__ == "__main__":
    unittest.main()
