"""Unit tests for tools/models (encoder against the decoder; byte-exact against a game model when one is dumped)."""
import json
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
    def test_a_group_over_the_vertex_limit_is_split_by_triangles(self):
        """A mesh holds at most 65535 vertices (u16 indices): the second Stable's thatch had 84079."""
        tris, pos = [], []
        for t in range(5):  # five triangles that share no vertex
            base = len(pos)
            pos += [(t, 0.0, 0.0), (t, 1.0, 0.0), (t, 0.0, 1.0)]
            tris.append((base, base + 1, base + 2))
        g = {"material": 2, "pos": pos, "nrm": [(0.0, 1.0, 0.0)] * 15, "tan": [(1.0, 0.0, 0.0, 1.0)] * 15,
             "uv": [(0.1 * i, 0.0) for i in range(15)], "skin": None, "lods": [tris]}
        parts = encode.split_groups([g], max_verts=7)
        self.assertGreater(len(parts), 1)
        self.assertTrue(all(len(p["pos"]) <= 7 and p["material"] == 2 and p["skin"] is None for p in parts))
        got = sorted(tuple(p["pos"][i] for i in t) for p in parts for t in p["lods"][0])
        self.assertEqual(got, sorted(tuple(pos[i] for i in t) for t in tris))  # every triangle once, as it was
        for p in parts:
            self.assertEqual(len(p["uv"]), len(p["pos"]))
            self.assertTrue(all(0 <= i < len(p["pos"]) for t in p["lods"][0] for i in t))
        small = encode.split_groups([g], max_verts=100)
        self.assertEqual(small, [g])

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

    def test_a_build_can_leave_creatures_out(self):
        """--leave-out <name>: the pack built without that creature (a creature whose licence is not settled yet);
        the mod's description is the pack's and each kept creature's own, and the pack's creatures are the same
        objects, so GUIDs written into the kept ones reach the pack file."""
        bear, wolf = dict(BEAR), dict(BEAR, name="beast_wolf", description="And a wolf (credit: someone).")
        pack = {"id": "p", "description": "New beasts.", "creatures": [bear, wolf]}
        kept = make_mod.leave_out(pack, [])
        self.assertEqual([c["name"] for c in kept["creatures"]], ["beast_bear", "beast_wolf"])
        self.assertEqual(kept["description"], "New beasts. And a wolf (credit: someone).")
        kept = make_mod.leave_out(pack, ["beast_wolf"])
        self.assertEqual([c["name"] for c in kept["creatures"]], ["beast_bear"])
        self.assertEqual(kept["description"], "New beasts.")
        self.assertIs(kept["creatures"][0], bear)
        self.assertEqual(len(pack["creatures"]), 2)  # the pack itself keeps both
        with self.assertRaises(SystemExit) as e:
            make_mod.leave_out(pack, ["beast_lion"])
        self.assertIn("beast_lion", str(e.exception))
        self.assertIn("beast_bear, beast_wolf", str(e.exception))

    def test_leave_out_names_come_from_the_command_line(self):
        self.assertEqual(make_mod.parse_args(["pack.json"]), ("pack.json", []))
        self.assertEqual(make_mod.parse_args(["pack.json", "--leave-out", "a", "--leave-out", "b"]),
                         ("pack.json", ["a", "b"]))
        for bad in ([], ["pack.json", "--leave-out"], ["pack.json", "--frobnicate"]):
            with self.assertRaises(SystemExit, msg=str(bad)):
                make_mod.parse_args(bad)

    def test_a_rebuild_removes_the_files_of_creatures_the_pack_no_longer_has(self):
        with tempfile.TemporaryDirectory() as d:
            for n in ("old_data.lua", "old_blob.lua", "old_albedo.lua", "kept_data.lua", "kept_blob.lua", "notes.txt"):
                open(os.path.join(d, n), "w").close()
            make_mod.remove_stale(d, {"kept"})
            self.assertEqual(sorted(os.listdir(d)), ["kept_blob.lua", "kept_data.lua", "notes.txt"])

    def test_content_scripts_are_copied_with_the_helper_and_stale_ones_go(self):
        """A pack's "content_scripts" (Beastmaster's Stable) run with EMF's content helper next to them."""
        lib = os.path.join(HERE, "..", "..", "content", "lib", "emf_content.lua")
        with tempfile.TemporaryDirectory() as d, tempfile.TemporaryDirectory() as packs:
            script = os.path.join(packs, "beastmaster_stable.lua")
            with open(script, "w", encoding="utf-8") as f:
                f.write("print('stable')\n")
            for n in ("content_old.lua", "beast_bear_data.lua"):
                open(os.path.join(d, n), "w").close()
            names = make_mod.copy_content(d, ["beastmaster_stable.lua"], packs)
            self.assertEqual(names, ["content_beastmaster_stable"])
            self.assertEqual(sorted(os.listdir(d)), ["beast_bear_data.lua", "content_beastmaster_stable.lua",
                                                     "emf_content.lua"])
            with open(os.path.join(d, "emf_content.lua"), "rb") as a, open(lib, "rb") as b:
                self.assertEqual(a.read(), b.read())
            with open(os.path.join(d, "content_beastmaster_stable.lua"), encoding="utf-8") as f:
                self.assertEqual(f.read(), "print('stable')\n")
            self.assertEqual(make_mod.copy_content(d, [], packs), [])  # none left: the helper goes too
            self.assertEqual(os.listdir(d), ["beast_bear_data.lua"])

    def test_a_content_script_name_is_lowercase_letters_digits_and_underscores(self):
        with tempfile.TemporaryDirectory() as d, tempfile.TemporaryDirectory() as packs:
            for bad in ("Stable.lua", "my-stable.lua", "stable.txt"):
                open(os.path.join(packs, bad), "w").close()
                with self.assertRaises(SystemExit, msg=bad):
                    make_mod.copy_content(d, [bad], packs)
            with self.assertRaises(SystemExit):
                make_mod.copy_content(d, ["missing.lua"], packs)

    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_the_generated_mod_runs_its_content_scripts_after_the_creatures(self):
        """A content script that fails is logged and leaves the creatures and the other scripts in place."""
        with tempfile.TemporaryDirectory() as d:
            make_mod.write_scripts(d, {"id": "test_pack", "title": "Test"}, [make_mod.listing_entry(dict(BEAR))],
                                   ["content_bad", "content_good"])
            with open(os.path.join(d, "beast_bear_data.lua"), "w", encoding="utf-8") as f:
                f.write(STUB_DATA)
            with open(os.path.join(d, "beast_bear_blob.lua"), "w", encoding="utf-8") as f:
                f.write('return "blob"\n')
            with open(os.path.join(d, "content_bad.lua"), "w", encoding="utf-8") as f:
                f.write("error('no such item')\n")
            with open(os.path.join(d, "content_good.lua"), "w", encoding="utf-8") as f:
                f.write("print('good content ran')\n")
            mock = os.path.join(HERE, "..", "..", "tools", "models", "mock_eml.lua")
            out = subprocess.run(["lua", mock, d], capture_output=True, text=True, timeout=60)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("content_bad: not added:", out.stdout)
            # said with EML's warn, so the build's reply lists it and the player is told
            self.assertTrue(any(l.startswith("warning:") and "content_bad: not added:" in l
                                for l in out.stdout.splitlines()), out.stdout)
            self.assertIn("no such item", out.stdout)
            self.assertIn("good content ran", out.stdout)
            self.assertLess(out.stdout.index("added beast_bear"), out.stdout.index("good content ran"))

    def test_a_prop_entry_needs_its_name_source_and_model(self):
        prop = {"name": "stable_prop", "glb": "stable.glb", "source_model_guid": "src-model",
                "source_model_json": "bed.json", "model_guid": "p-1"}
        self.assertEqual(make_mod.prop_entry(dict(prop)),
                         {"name": "stable_prop", "source_model_guid": "src-model", "model_guid": "p-1"})
        for key in ("glb", "source_model_guid", "source_model_json", "model_guid"):
            with self.assertRaises(SystemExit, msg=key):
                make_mod.prop_entry({k: v for k, v in prop.items() if k != key})
        with self.assertRaises(SystemExit):
            make_mod.prop_entry(dict(prop, name="Stable-Prop"))

    def test_a_prop_is_built_from_its_still_glb(self):
        import test_gltf_static
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "src")
            os.makedirs(src)
            test_gltf_static.prop_glb(os.path.join(d, "stable.glb"))
            with open(os.path.join(d, "bed.json"), "w", encoding="utf-8") as f:
                json.dump({"debugName": "bed", "materials": [{"material": "a"}, {"material": "b"}, {"material": "c"}]}, f)
            prop = {"name": "stable_prop", "glb": "stable.glb", "source_model_guid": "src-model",
                    "source_model_json": "bed.json", "model_guid": "p-1", "width": 4.0, "material": 2}
            open(os.path.join(src, "stable_prop_albedo.lua"), "w").close()  # an earlier build's texture of ours
            entry, changed = make_mod.build_prop(prop, src, lambda p: os.path.join(d, p))
            self.assertEqual(entry, {"name": "stable_prop", "source_model_guid": "src-model", "model_guid": "p-1"})
            self.assertFalse(changed)
            self.assertEqual(sorted(os.listdir(src)), ["stable_prop_blob.lua", "stable_prop_data.lua"])
            with open(os.path.join(src, "stable_prop_data.lua"), encoding="utf-8") as f:
                data = f.read()
            self.assertIn("jointCount = 0", data)
            self.assertIn("skinningMap = {}", data)
            self.assertIn("materialIndex = 2", data)

    def test_a_prop_names_the_game_material_of_each_slot(self):
        prop = {"name": "stable_prop", "glb": "stable.glb", "source_model_guid": "src-model",
                "source_model_json": "bed.json", "model_guid": "p-1", "materials": {"1": "game-wood", "0": "game-straw"}}
        self.assertEqual(make_mod.prop_entry(dict(prop))["materials"],
                         [{"slot": 0, "material": "game-straw"}, {"slot": 1, "material": "game-wood"}])
        with self.assertRaises(SystemExit):
            make_mod.prop_entry(dict(prop, materials={"x": "game-wood"}))
        with self.assertRaises(SystemExit):
            make_mod.prop_entry(dict(prop, materials={"0": "game-straw"}, texture={"albedo_dir": "t", "slot": 0}))

    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_the_generated_mod_points_a_props_slots_at_the_game_materials(self):
        with tempfile.TemporaryDirectory() as d:
            prop = {"name": "stable_prop", "source_model_guid": BEAR["source_model_guid"], "model_guid": "p-1",
                    "materials": [{"slot": 0, "material": "game-straw"}, {"slot": 1, "material": "game-wood"}]}
            make_mod.write_scripts(d, {"id": "test_pack", "title": "Test"}, [make_mod.listing_entry(dict(BEAR))],
                                   props=[prop])
            for name in ("beast_bear", "stable_prop"):
                with open(os.path.join(d, name + "_data.lua"), "w", encoding="utf-8") as f:
                    f.write(STUB_DATA.replace("beast_bear", name))
                with open(os.path.join(d, name + "_blob.lua"), "w", encoding="utf-8") as f:
                    f.write('return "blob"\n')
            mock = os.path.join(HERE, "..", "..", "tools", "models", "mock_eml.lua")
            out = subprocess.run(["lua", mock, d], capture_output=True, text=True, timeout=60)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("stable_prop: slot 0 wears the game material game-straw", out.stdout)
            self.assertIn("stable_prop: slot 1 wears the game material game-wood", out.stdout)
            self.assertIn("added prop stable_prop: model p-1", out.stdout)

    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_the_generated_mod_makes_the_props_models_before_its_content_scripts(self):
        """A prop is a model of its own with no template (a content script's object uses it)."""
        with tempfile.TemporaryDirectory() as d:
            prop = {"name": "stable_prop", "source_model_guid": BEAR["source_model_guid"], "model_guid": "p-1"}
            make_mod.write_scripts(d, {"id": "test_pack", "title": "Test"}, [make_mod.listing_entry(dict(BEAR))],
                                   ["content_good"], props=[prop])
            for name in ("beast_bear", "stable_prop"):
                with open(os.path.join(d, name + "_data.lua"), "w", encoding="utf-8") as f:
                    f.write(STUB_DATA.replace("beast_bear", name))
                with open(os.path.join(d, name + "_blob.lua"), "w", encoding="utf-8") as f:
                    f.write('return "blob"\n')
            with open(os.path.join(d, "content_good.lua"), "w", encoding="utf-8") as f:
                f.write("print('good content ran')\n")
            mock = os.path.join(HERE, "..", "..", "tools", "models", "mock_eml.lua")
            out = subprocess.run(["lua", mock, d], capture_output=True, text=True, timeout=60)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("added prop stable_prop: model p-1", out.stdout)
            self.assertIn("model p-1 cloth none", out.stdout)
            self.assertIn("added beast_bear", out.stdout)  # the creatures as before
            self.assertLess(out.stdout.index("added prop stable_prop"), out.stdout.index("good content ran"))
            server = subprocess.run(["lua", mock, d], capture_output=True, text=True, timeout=60,
                                    env=dict(os.environ, MOCK_SERVER="1"))
            self.assertEqual(server.returncode, 0, server.stderr)
            self.assertIn("stable_prop: no render data here", server.stdout)
            self.assertNotIn("stable_prop: not added", server.stdout)

    def test_skeleton_and_clip_files_go_when_a_creature_has_no_skeleton_of_its_own(self):
        with tempfile.TemporaryDirectory() as d:
            for n in ("own_skeleton.lua", "own_clips.lua", "plain_skeleton.lua", "plain_clips.lua", "plain_data.lua"):
                open(os.path.join(d, n), "w").close()
            make_mod.remove_stale(d, {"own", "plain"}, own={"own"})
            self.assertEqual(sorted(os.listdir(d)), ["own_clips.lua", "own_skeleton.lua", "plain_data.lua"])

    def test_a_texture_for_a_slot_the_model_does_not_draw_is_refused(self):
        """The panther's mesh drew with the sabretooth's slot 1 while its texture replaced slot 0: in game it wore
        the sabretooth's own fur through the panther's UVs."""
        groups = [{"material": 1, "pos": []}]
        with self.assertRaises(SystemExit) as e:
            make_mod.check_texture_slot("panther2", 0, groups)
        self.assertIn("slot 0", str(e.exception))
        self.assertIn("1", str(e.exception))
        make_mod.check_texture_slot("panther2", 1, groups)
        make_mod.check_texture_slot("bear", 0, [{"material": 0}, {"material": 1}])

    def test_own_proportions_needs_the_game_skeleton_dump(self):
        cfg = {k: "x" for k in make_mod.OWN_KEYS}
        with self.assertRaises(SystemExit):
            make_mod.listing_entry(dict(BEAR, own_proportions=cfg))  # no "hierarchy"
        make_mod.listing_entry(dict(BEAR, own_proportions=cfg, hierarchy="wolf.hierarchy.json"))

    def test_a_colour_version_shares_the_model_data_of_the_creature_with_the_same_glb(self):
        fox = dict(BEAR, name="fox", glb="wilds/fox.glb", hierarchy="wolf.hierarchy.json", source_model_json="wolf.json")
        arctic = dict(fox, name="fox_arctic")
        silver = dict(fox, name="fox_silver", glb=os.path.join("wilds", "fox.glb"))
        wolf = dict(fox, name="wolf", glb="wilds/wolf.glb")
        other_bones = dict(fox, name="fox_on_boar", hierarchy="boar.hierarchy.json")
        own = dict(fox, name="fox_own", own_proportions={"sidecar": "x"})
        self.assertEqual(make_mod.shared_geometry([fox, arctic, silver, wolf, other_bones, own]),
                         {"fox_arctic": "fox", "fox_silver": "fox"})

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

    def plain_mod(self, d, env):
        """The bear pack's creature (a model on the source's skeleton, with a texture) run through the mock."""
        bear = dict(BEAR, texture={"albedo_dir": "x", "slot": 0, "material_guid": "mat-ours", "flat_size": 4})
        make_mod.write_scripts(d, {"id": "test_pack", "title": "Test"}, [make_mod.listing_entry(bear)])
        with open(os.path.join(d, "beast_bear_data.lua"), "w", encoding="utf-8") as f:
            f.write(STUB_DATA)
        with open(os.path.join(d, "beast_bear_blob.lua"), "w", encoding="utf-8") as f:
            f.write('return "blob"\n')
        with open(os.path.join(d, "beast_bear_albedo.lua"), "w", encoding="utf-8") as f:
            f.write("return {}\n")
        mock = os.path.join(HERE, "..", "..", "tools", "models", "mock_eml.lua")
        return subprocess.run(["lua", mock, d], capture_output=True, text=True, timeout=60, env=dict(os.environ, **env))

    def test_normal_and_material_maps_beside_the_albedo_are_written(self):
        """normal_mipNN.png and material_mipNN.png in the albedo's folder become <name>_normal.lua and
        <name>_material.lua; a map the folder no longer has loses its file."""
        with tempfile.TemporaryDirectory() as d:
            folder, src = os.path.join(d, "tex"), os.path.join(d, "src")
            os.makedirs(folder)
            os.makedirs(src)
            for n in ("albedo_mip00.png", "albedo_mip01.png", "normal_mip00.png"):
                with open(os.path.join(folder, n), "wb") as f:
                    f.write(b"PNG")
            open(os.path.join(src, "bear_material.lua"), "w").close()  # an earlier build's map
            self.assertEqual(make_mod.write_maps(src, "bear", folder), ["normal"])
            self.assertEqual(sorted(os.listdir(src)), ["bear_albedo.lua", "bear_normal.lua"])
            with open(os.path.join(src, "bear_normal.lua"), encoding="ascii") as f:
                self.assertIn("1 PNG levels", f.read())
            with self.assertRaises(SystemExit):
                make_mod.write_maps(src, "bear", src)  # no albedo there

    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_the_generated_mod_writes_our_normal_map_and_keeps_the_other_maps_flat(self):
        png16 = make_mod.lua_bytes(b"\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR\x00\x00\x00\x10\x00\x00\x00\x10")
        with tempfile.TemporaryDirectory() as d:
            entry = make_mod.listing_entry(dict(BEAR))
            entry["texture"] = {"slot": 0, "material_guid": "mat-ours-0001", "flat_size": 4, "maps": ["normal"]}
            make_mod.write_scripts(d, {"id": "test_pack", "title": "Test"}, [entry])
            for name, text in (("data", STUB_DATA), ("blob", 'return "blob"\n'),
                               ("albedo", "return { %s }\n" % png16), ("normal", "return { %s }\n" % png16)):
                with open(os.path.join(d, "beast_bear_%s.lua" % name), "w", encoding="utf-8") as f:
                    f.write(text)
            mock = os.path.join(HERE, "..", "..", "tools", "models", "mock_eml.lua")
            out = subprocess.run(["lua", mock, d], capture_output=True, text=True, timeout=60)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("mat-ours-0001 image 1 BC7_srgb_block 16x16", out.stdout)
            self.assertIn("mat-ours-0001 image 2 BC5_unorm_block 16x16", out.stdout)   # our normal map
            self.assertIn("mat-ours-0001 image 3 BC1_RGB_unorm_block 4x4", out.stdout)  # flat: no material map of ours
            self.assertIn("flat material", out.stdout)
            self.assertNotIn("flat normal", out.stdout)

    def run_bear(self, d, bear):
        make_mod.write_scripts(d, {"id": "test_pack", "title": "Test"}, [make_mod.listing_entry(bear)])
        for name, text in (("data", STUB_DATA), ("blob", 'return "blob"\n')):
            with open(os.path.join(d, "beast_bear_%s.lua" % name), "w", encoding="utf-8") as f:
                f.write(text)
        mock = os.path.join(HERE, "..", "..", "tools", "models", "mock_eml.lua")
        return subprocess.run(["lua", mock, d], capture_output=True, text=True, timeout=60)

    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_a_creature_slot_wears_a_game_material_found_by_name(self):
        """A fur-card mesh draws with a game fur material: the pack names it by its debugName or its GUID."""
        with tempfile.TemporaryDirectory() as d:
            out = self.run_bear(d, dict(BEAR, materials={"1": "name:mock_mat-1"}))
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("beast_bear: slot 1 wears the game material mat-1", out.stdout)
        with tempfile.TemporaryDirectory() as d:
            out = self.run_bear(d, dict(BEAR, materials={"0": "mat-1"}))
            self.assertIn("beast_bear: slot 0 wears the game material mat-1", out.stdout)
        with tempfile.TemporaryDirectory() as d:  # a name the game does not have: this creature is not added
            out = self.run_bear(d, dict(BEAR, materials={"1": "name:gone"}))
            self.assertIn("no game material named gone", out.stdout + out.stderr)
            self.assertNotIn("added beast_bear", out.stdout)

    def test_a_slot_takes_a_game_material_or_our_texture(self):
        with self.assertRaises(SystemExit) as e:
            make_mod.listing_entry(dict(BEAR, materials={"1": "name:fur"}, texture={"albedo_dir": "x", "slot": 1}))
        self.assertIn("slot 1", str(e.exception))
        make_mod.listing_entry(dict(BEAR, materials={"0": "name:fur"}, texture={"albedo_dir": "x", "slot": 1}))
        with self.assertRaises(SystemExit):
            make_mod.listing_entry(dict(BEAR, materials={"fur": "name:fur"}))

    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_on_a_dedicated_server_the_template_is_made_without_the_model(self):
        """A dedicated server's data has no render data: the template copy and its edits are made, the model and the
        material are left out (the template still names the model, as the game's own templates do there)."""
        with tempfile.TemporaryDirectory() as d:
            out = self.plain_mod(d, {"MOCK_SERVER": "1"})
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("beast_bear: no render data here (a dedicated server): the model and its material are left "
                          "out", out.stdout)
            self.assertNotIn("not added", out.stdout)
            self.assertIn("template Beast_Bear keen::ecs::ModelResource model m-1", out.stdout)
            self.assertIn("template Beast_Bear keen::ecs::DamageMod dataStorage 0 0.5 1.5 0", out.stdout)
            self.assertNotIn("keen::RenderModel", out.stdout)
            self.assertNotIn("keen::RenderMaterialResource", out.stdout)

    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_a_client_without_the_source_model_still_stops_the_creature(self):
        """Where the game has render data, a source model it no longer has is a game change: the creature is not
        added."""
        with tempfile.TemporaryDirectory() as d:
            out = self.plain_mod(d, {"MOCK_MISSING": "src-model"})
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("beast_bear: not added: ", out.stdout)
            self.assertTrue(any(l.startswith("warning:") and "beast_bear: not added: " in l
                                for l in out.stdout.splitlines()), out.stdout)
            self.assertIn("the source model was not found", out.stdout)
            self.assertNotIn("template Beast_Bear", out.stdout)

    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_own_proportions_on_a_dedicated_server(self):
        """own_proportions on a server: the skeleton copy, the clips and the graph copy are made, the model is not."""
        with tempfile.TemporaryDirectory() as d, tempfile.TemporaryDirectory() as src:
            self.own_mod(d, src)
            mock = os.path.join(HERE, "..", "..", "tools", "models", "mock_eml.lua")
            out = subprocess.run(["lua", mock, d], capture_output=True, text=True, timeout=60,
                                 env=dict(os.environ, MOCK_SERVER="1"))
            self.assertEqual(out.returncode, 0, out.stderr)
            text = out.stdout
            self.assertNotIn("not added", text)
            self.assertIn("skeleton 22222222-2222-4222-8222-222222222222 joints 12 id 22222222-2222-4222-8222-222222222222",
                          text)
            self.assertEqual(text.count("clip copy "), 2)
            self.assertIn("graph 44444444-4444-4444-8444-444444444444 hierarchy 22222222-2222-4222-8222-222222222222 "
                          "ours 4 left 1", text)
            self.assertNotIn("keen::RenderModel", text)


    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_a_creature_on_a_skeleton_of_its_own(self):
        """own_proportions: a skeleton copy, the model on it, the retargeted clips, a graph copy whose clip
        references (clip nodes and blend-space items) are ours, and the template on that graph."""
        import own_creature
        if HERE not in sys.path:
            sys.path.insert(0, HERE)  # the fixtures of test_own_creature.py
        import test_own_creature as toc
        with tempfile.TemporaryDirectory() as d, tempfile.TemporaryDirectory() as src:
            with open(os.path.join(src, "own.json"), "w", encoding="utf-8") as f:
                json.dump(toc.sidecar(), f)
            stand = [toc.REST] * 11
            with open(os.path.join(src, "beast_aaaaaaaa-0000-4000-8000-000000000001.json"), "w", encoding="utf-8") as f:
                json.dump(toc.clip(stand), f)
            with open(os.path.join(src, "beast_bbbbbbbb-0000-4000-8000-000000000002.json"), "w", encoding="utf-8") as f:
                json.dump(toc.clip(stand, loop=False), f)
            cfg = {"sidecar": os.path.join(src, "own.json"), "skeleton_guid": "22222222-2222-4222-8222-222222222222",
                   "reference_clip": os.path.join(src, "beast_aaaaaaaa-0000-4000-8000-000000000001.json"),
                   "clips": os.path.join(src, "beast_*.json"), "graph": "33333333-3333-4333-8333-333333333333",
                   "graph_guid": "44444444-4444-4444-8444-444444444444"}
            built = own_creature.build(cfg, toc.hierarchy())
            bear = dict(BEAR, own_proportions=cfg, hierarchy="wolf.hierarchy.json")
            make_mod.write_scripts(d, {"id": "test_pack", "title": "Test"}, [make_mod.listing_entry(bear)])
            make_mod.write_own(d, bear["name"], built)
            with open(os.path.join(d, "beast_bear_data.lua"), "w", encoding="utf-8") as f:
                f.write(STUB_DATA)
            with open(os.path.join(d, "beast_bear_blob.lua"), "w", encoding="utf-8") as f:
                f.write('return "blob"\n')
            mock = os.path.join(HERE, "..", "..", "tools", "models", "mock_eml.lua")
            out = subprocess.run(["lua", mock, d], capture_output=True, text=True, timeout=60)
            self.assertEqual(out.returncode, 0, out.stderr)
            text = out.stdout
            self.assertIn("skeleton 22222222-2222-4222-8222-222222222222 joints 12 id 22222222-2222-4222-8222-222222222222",
                          text)
            self.assertIn("model %s hierarchy 22222222-2222-4222-8222-222222222222" % BEAR["model_guid"], text)
            self.assertEqual(text.count("clip copy "), 2)
            # the mock graph: a clip node per source clip and a blend space with both clips and one other
            self.assertIn("graph 44444444-4444-4444-8444-444444444444 hierarchy 22222222-2222-4222-8222-222222222222 "
                          "ours 4 left 1", text)
            self.assertIn("template Beast_Bear keen::ecs::AnimationGraphResourceComponent2 animationGraph "
                          "44444444-4444-4444-8444-444444444444", text)
            # the joints' rest pose and inverse binds as built
            q = built["bind"][3][0]
            self.assertIn("pose 4 %.4f %.4f %.4f %.4f" % q, text)
            self.assertIn("position 12 %.4f %.4f %.4f" % built["bind"][11][1], text)

    def own_mod(self, d, src):
        """A pack with one creature on a skeleton of its own (two game clips) written into d; returns the mock's run."""
        import own_creature
        if HERE not in sys.path:
            sys.path.insert(0, HERE)
        import test_own_creature as toc
        with open(os.path.join(src, "own.json"), "w", encoding="utf-8") as f:
            json.dump(toc.sidecar(), f)
        for g, loop in (("aaaaaaaa-0000-4000-8000-000000000001", True), ("bbbbbbbb-0000-4000-8000-000000000002", False)):
            with open(os.path.join(src, "beast_%s.json" % g), "w", encoding="utf-8") as f:
                json.dump(toc.clip([toc.REST] * 11, loop=loop), f)
        cfg = {"sidecar": os.path.join(src, "own.json"), "skeleton_guid": "22222222-2222-4222-8222-222222222222",
               "reference_clip": os.path.join(src, "beast_aaaaaaaa-0000-4000-8000-000000000001.json"),
               "clips": os.path.join(src, "beast_*.json"), "graph": "33333333-3333-4333-8333-333333333333",
               "graph_guid": "44444444-4444-4444-8444-444444444444"}
        built = own_creature.build(cfg, toc.hierarchy())
        bear = dict(BEAR, own_proportions=cfg, hierarchy="wolf.hierarchy.json")
        make_mod.write_scripts(d, {"id": "test_pack", "title": "Test"}, [make_mod.listing_entry(bear)])
        make_mod.write_own(d, bear["name"], built)
        with open(os.path.join(d, "beast_bear_data.lua"), "w", encoding="utf-8") as f:
            f.write(STUB_DATA)
        with open(os.path.join(d, "beast_bear_blob.lua"), "w", encoding="utf-8") as f:
            f.write('return "blob"' + chr(10))

    def run_mock(self, d, missing=""):
        mock = os.path.join(HERE, "..", "..", "tools", "models", "mock_eml.lua")
        return subprocess.run(["lua", mock, d], capture_output=True, text=True, timeout=60,
                              env=dict(os.environ, MOCK_MISSING=missing))

    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_a_game_clip_that_is_gone_is_skipped(self):
        """After a game update removed a clip: that clip stays the game's in the graph copy (it plays on the copy:
        the same skeleton hash); the creature and the other clips are still added."""
        with tempfile.TemporaryDirectory() as d, tempfile.TemporaryDirectory() as src:
            self.own_mod(d, src)
            out = self.run_mock(d, "bbbbbbbb-0000-4000-8000-000000000002")
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("the game clip bbbbbbbb-0000-4000-8000-000000000002 is gone: it stays the game's", out.stdout)
            self.assertEqual(out.stdout.count("clip copy "), 1)
            self.assertIn("template Beast_Bear keen::ecs::AnimationGraphResourceComponent2 animationGraph "
                          "44444444-4444-4444-8444-444444444444", out.stdout)

    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_a_game_graph_that_is_gone_skips_only_that_creature(self):
        with tempfile.TemporaryDirectory() as d, tempfile.TemporaryDirectory() as src:
            self.own_mod(d, src)
            out = self.run_mock(d, "33333333-3333-4333-8333-333333333333")
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("beast_bear: not added:", out.stdout)
            self.assertNotIn("template Beast_Bear", out.stdout)

    @unittest.skipUnless(shutil.which("lua"), "needs lua 5.4 on PATH")
    def test_a_skeleton_whose_joint_count_is_not_a_multiple_of_four(self):
        """The game pads the default pose's rotation vectors to a multiple of 4 joints (the boar: 58 joints, 60
        slots): positions start after the padding."""
        import own_creature
        if HERE not in sys.path:
            sys.path.insert(0, HERE)
        import joint_animation as ja
        import test_own_creature as toc
        n = 10
        hier = toc.hierarchy()
        hier["jointCount"] = n
        side = toc.sidecar()
        side["joints"] = side["joints"][:n]
        rest = toc.REST[:n]
        clip = ja.encode([rest] * 11, rest, fps=60, loop=True, hierarchy_hash=ja.hierarchy_hash(list(range(100, 110))))
        with tempfile.TemporaryDirectory() as d, tempfile.TemporaryDirectory() as src:
            with open(os.path.join(src, "own.json"), "w", encoding="utf-8") as f:
                json.dump(side, f)
            with open(os.path.join(src, "beast_aaaaaaaa-0000-4000-8000-000000000001.json"), "w", encoding="utf-8") as f:
                json.dump(clip, f)
            cfg = {"sidecar": os.path.join(src, "own.json"), "skeleton_guid": "22222222-2222-4222-8222-222222222222",
                   "reference_clip": os.path.join(src, "beast_aaaaaaaa-0000-4000-8000-000000000001.json"),
                   "clips": os.path.join(src, "beast_*.json"), "graph": "33333333-3333-4333-8333-333333333333",
                   "graph_guid": "44444444-4444-4444-8444-444444444444"}
            built = own_creature.build(cfg, hier)
            bear = dict(BEAR, own_proportions=cfg, hierarchy="wolf.hierarchy.json")
            make_mod.write_scripts(d, {"id": "test_pack", "title": "Test"}, [make_mod.listing_entry(bear)])
            make_mod.write_own(d, bear["name"], built)
            with open(os.path.join(d, "beast_bear_data.lua"), "w", encoding="utf-8") as f:
                f.write(STUB_DATA)
            with open(os.path.join(d, "beast_bear_blob.lua"), "w", encoding="utf-8") as f:
                f.write('return "blob"\n')
            mock = os.path.join(HERE, "..", "..", "tools", "models", "mock_eml.lua")
            out = subprocess.run(["lua", mock, d], capture_output=True, text=True, timeout=60)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("position 10 %.4f %.4f %.4f" % built["bind"][9][1], out.stdout)


if __name__ == "__main__":
    unittest.main()
