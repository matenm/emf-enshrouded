"""tools/models/own_creature.py: a creature's skeleton copy and the game creature's clips retargeted to it."""
import json
import math
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "models"))
import joint_animation as ja  # noqa: E402
import own_creature as oc  # noqa: E402
import own_skeleton as osk  # noqa: E402

I = (0.0, 0.0, 0.0, 1.0)
ONE = (1.0, 1.0, 1.0)
# a small four-legged skeleton (game space, y up, facing -z): root, pelvis, chest, head, and two-bone legs
NAMES = ["j000_Root", "j001_Pelvis", "j002_Chest", "j003_Head", "j004", "j005", "j006", "j007", "j008", "j009",
         "j010", "j011"]
PARENTS = [None, 0, 1, 2, 2, 4, 2, 6, 1, 8, 1, 10]
REST = [(I, (0.0, 0.0, 0.0), ONE), (I, (0.0, 1.0, 0.5), ONE), (I, (0.0, 0.0, -1.0), ONE), (I, (0.0, 0.3, -0.4), ONE),
        (I, (0.3, -0.5, 0.0), ONE), (I, (0.0, -0.5, 0.0), ONE), (I, (-0.3, -0.5, 0.0), ONE), (I, (0.0, -0.5, 0.0), ONE),
        (I, (0.3, -0.5, 0.0), ONE), (I, (0.0, -0.5, 0.0), ONE), (I, (-0.3, -0.5, 0.0), ONE), (I, (0.0, -0.5, 0.0), ONE)]


def hierarchy():
    joints = [{"orientation": dict(zip("xyzw", q)), "position": dict(zip("xyz", t)), "scale": dict(zip("xyz", s))}
              for q, t, s in REST]
    return {"hierarchyId": "11111111-1111-4111-8111-111111111111", "jointCount": len(REST),
            "jointNames": list(range(100, 100 + len(REST))), "jointParentIndices": [65535 if p is None else p for p in PARENTS],
            "defaultPoseJoints": joints}


def sidecar(stretch=1.3):
    """The creature: the same joints with longer legs (the joints below the shoulders and hips further down) and the
    head a little forward, in the fitter's space (Blender: forward -y, up +z; game x mirrored)."""
    world = [w[1] for w in osk.world(REST, PARENTS)]
    own = []
    for j, p in enumerate(world):
        q = list(p)
        if j in (5, 7, 9, 11):  # feet
            q[1] = world[PARENTS[j]][1] - 0.5 * stretch
        if j == 3:
            q[2] -= 0.2
        own.append(q)
    to_blender = lambda p: [-p[0], -p[2], p[1]]  # the inverse of own_skeleton.blender_to_game
    return {"ground": 0.0, "scale": 1.0, "joints": [{"name": n, "game": to_blender(w), "own": to_blender(o)}
                                                     for n, w, o in zip(NAMES, world, own)]}


STAND = "aaaaaaaa-0000-4000-8000-000000000001"  # the clip whose first frame is the standing reference
WALK = "bbbbbbbb-0000-4000-8000-000000000002"


def turn(axis, rad):
    s = math.sin(rad / 2)
    return tuple(c * s for c in axis) + (math.cos(rad / 2),)


def clip(poses, loop=True):
    return ja.encode(poses, REST, fps=60, loop=loop, hierarchy_hash=ja.hierarchy_hash(list(range(100, 112))))


class Fixture(unittest.TestCase):
    """A sidecar and two clips (standing, walking) in a temporary folder."""
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        d = self.dir.name
        self.side = os.path.join(d, "own.json")
        with open(self.side, "w", encoding="utf-8") as f:
            json.dump(sidecar(), f)
        stand = [REST[j] for j in range(len(REST))]
        walk = [[(turn((1, 0, 0), 0.3 * math.sin(f / 3)) if j in (4, 8) else q, t, s) for j, (q, t, s) in enumerate(REST)]
                for f in range(21)]
        for name, poses in ((STAND, [stand] * 11), (WALK, walk)):
            with open(os.path.join(d, "beast_%s.json" % name), "w", encoding="utf-8") as f:
                json.dump(clip(poses), f)
        self.cfg = {"sidecar": self.side, "skeleton_guid": "22222222-2222-4222-8222-222222222222",
                    "reference_clip": os.path.join(d, "beast_%s.json" % STAND),
                    "clips": os.path.join(d, "beast_*.json"), "graph": "33333333-3333-4333-8333-333333333333",
                    "graph_guid": "44444444-4444-4444-8444-444444444444"}

    def tearDown(self):
        self.dir.cleanup()


class BuildTests(Fixture):
    def test_the_rest_pose_can_be_the_standing_reference(self):
        """reference_clip "rest": the creature was made in the game skeleton's rest pose (from pictures of the game
        body), so motions are taken relative to that pose, not to a clip's first frame."""
        want = oc.build(self.cfg, hierarchy())["clips"]        # the fixture's standing clip is the rest pose
        got = oc.build(dict(self.cfg, reference_clip="rest"), hierarchy())["clips"]
        self.assertEqual([c["clip"] for c in got], [c["clip"] for c in want])

    def test_the_bind_pose_stands_where_the_sidecar_says(self):
        built = oc.build(self.cfg, hierarchy())
        bind = built["bind"]
        world = osk.world(bind, PARENTS)
        for j, joint in enumerate(sidecar()["joints"]):
            for a, b in zip(world[j][1], osk.blender_to_game(joint["own"])):
                self.assertAlmostEqual(a, b, places=9)

    def test_the_bones_keep_the_game_skeletons_frames(self):
        """Only the joints move: the game graph's own nodes (foot IK, leaning, aiming) turn bones about their axes as
        they would on the game creature."""
        built = oc.build(self.cfg, hierarchy())
        for (q, _, _), (q0, _, _) in zip(built["bind"], REST):
            self.assertEqual(q, q0)

    def test_the_inverse_binds_undo_the_bind_pose(self):
        built = oc.build(self.cfg, hierarchy())
        for (q, p, s), m in zip(osk.world(built["bind"], PARENTS), built["skeleton"]["skinToJointMatrices"]):
            # the inverse bind takes the joint's world position to the origin
            r = m["rot"]
            back = [r["x"][k] * p[0] + r["y"][k] * p[1] + r["z"][k] * p[2] + m["pos"][k] for k in "xyz"]
            for a in back:
                self.assertAlmostEqual(a, 0.0, places=9)

    def test_clips_get_fixed_guids_of_their_own(self):
        a = oc.build(self.cfg, hierarchy())
        b = oc.build(self.cfg, hierarchy())
        self.assertEqual([c["guid"] for c in a["clips"]], [c["guid"] for c in b["clips"]])
        self.assertEqual(sorted(c["source"] for c in a["clips"]), [STAND, WALK])
        other = oc.build(dict(self.cfg, skeleton_guid="55555555-5555-4555-8555-555555555555"), hierarchy())
        self.assertNotEqual(a["clips"][0]["guid"], other["clips"][0]["guid"])

    def test_the_standing_reference_becomes_the_creatures_own_pose(self):
        built = oc.build(self.cfg, hierarchy())
        stand = next(c for c in built["clips"] if c["source"] == STAND)
        pose = ja.sample(stand["clip"], 0, built["bind"])
        for (q, t, _), (q0, t0, _) in zip(pose, built["bind"]):
            self.assertLess(2 * math.acos(min(1.0, abs(sum(a * b for a, b in zip(q, q0))))), 1e-3)
            for a, b in zip(t, t0):
                self.assertAlmostEqual(a, b, delta=1e-4)


class CheckTests(Fixture):
    """Clips and the sidecar are checked against the skeleton before anything is built."""

    def test_the_clip_guid_is_the_end_of_the_file_name(self):
        d = self.dir.name
        os.rename(os.path.join(d, "beast_%s.json" % WALK), os.path.join(d, "wild_beast_cccccccc-0000-4000-8000-000000000000.json"))
        built = oc.build(dict(self.cfg, clips=os.path.join(d, "*_*.json")), hierarchy())
        self.assertIn("cccccccc-0000-4000-8000-000000000000", [c["source"] for c in built["clips"]])

    def test_a_file_name_without_a_guid_is_refused(self):
        d = self.dir.name
        os.rename(os.path.join(d, "beast_%s.json" % WALK), os.path.join(d, "beast_walk.json"))
        with self.assertRaises(SystemExit):
            oc.build(dict(self.cfg, clips=os.path.join(d, "beast_*.json")), hierarchy())

    def test_a_clip_of_another_skeleton_is_refused(self):
        d = self.dir.name
        other = clip([REST] * 11)
        other["hierarchyHash"] = 12345
        with open(os.path.join(d, "beast_dddddddd-0000-4000-8000-000000000000.json"), "w", encoding="utf-8") as f:
            json.dump(other, f)
        with self.assertRaises(SystemExit):
            oc.build(self.cfg, hierarchy())

    def test_sidecar_joints_are_matched_by_name(self):
        side = sidecar()
        side["joints"].reverse()  # any order: j004 is joint 4 by its name
        with open(self.side, "w", encoding="utf-8") as f:
            json.dump(side, f)
        a = oc.build(self.cfg, hierarchy())
        with open(self.side, "w", encoding="utf-8") as f:
            json.dump(sidecar(), f)
        b = oc.build(self.cfg, hierarchy())
        self.assertEqual(a["bind"], b["bind"])


ANALYSIS = os.path.join(HERE, "..", "..", "analysis")
PANTHER = os.path.join(ANALYSIS, "models", "panther_own.json")
SABRE = os.path.join(ANALYSIS, "model_survey", "hierarchies", "ef2d7e9f-1a5e-4d7e-a3e5-d47448168385.json")
SABRE_IDLE = os.path.join(ANALYSIS, "models", "anim", "export_2026-11-01b", "clips",
                          "sabretooth_05a3f6c9-774f-4309-bb8b-6b7d9fa3570c.json")


@unittest.skipUnless(all(os.path.exists(p) for p in (PANTHER, SABRE, SABRE_IDLE)),
                     "needs the panther's fit and the sabretooth's dumps in analysis/")
class PantherTests(unittest.TestCase):
    def test_bones_that_sit_on_their_parent_do_not_turn(self):
        """The sabretooth's pelvis and chest each have a child at their own place: their frames stay as they are."""
        with open(SABRE, encoding="utf-8") as f:
            hier = json.load(f)
        cfg = {"sidecar": PANTHER, "skeleton_guid": "22222222-2222-4222-8222-222222222222",
               "reference_clip": SABRE_IDLE, "clips": SABRE_IDLE, "graph": "g", "graph_guid": "h"}
        built = oc.build(cfg, hier)
        rest, _ = osk.rest_of(hier)
        for j, ((q, _, _), (q0, _, _)) in enumerate(zip(built["bind"], rest)):
            self.assertEqual(q, q0, j)


if __name__ == "__main__":
    unittest.main()
