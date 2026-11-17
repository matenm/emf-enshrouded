"""tools/models/made_clips.py: clips made from a few key turns on top of a pose (a death, a flinch), and
gltf_clips' speed (an animation played faster)."""
import math
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "models"))
import gltf_clips as gc  # noqa: E402
import gltf_in  # noqa: E402
import joint_animation as ja  # noqa: E402
import made_clips as mc  # noqa: E402
import new_skeleton as ns  # noqa: E402
import own_skeleton as osk  # noqa: E402
import test_gltf_clips as tclips  # noqa: E402
import test_new_skeleton as rig  # noqa: E402


def about(axis, deg):
    a = math.radians(deg) / 2
    v = {"x": (1, 0, 0), "y": (0, 1, 0), "z": (0, 0, 1)}[axis]
    return tuple(c * math.sin(a) for c in v) + (math.cos(a),)


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        path = rig.small_rig(os.path.join(self.tmp.name, "rig.glb"))
        self.gltf, self.binc = tclips.add_animation(path, tclips.Fixture.CHANNELS)
        self.sk = ns.build(self.gltf, self.binc)
        self.rest_world = osk.world(self.sk["rest"], self.sk["parents"])

    def tearDown(self):
        self.tmp.cleanup()

    def world(self, clip, frame):
        pose = ja.sample_end(clip, self.sk["rest"]) if frame == clip["frameCount"] else ja.sample(clip, frame,
                                                                                                     self.sk["rest"])
        return osk.world(pose, self.sk["parents"])


class MadeTests(Fixture):
    KEYS = [{"t": 0.0}, {"t": 1.0, "turn": {"Hips": ["z", 90]}}]

    def test_a_turn_eases_in_and_children_follow(self):
        clip = mc.make(self.sk, self.sk["rest"], self.KEYS, hierarchy_hash=5)
        self.assertEqual((clip["frameCount"], clip["flags"], clip["hierarchyHash"]), (60, 0, 5))
        hips = self.sk["names"].index("Hips")
        head = self.sk["names"].index("Head")
        for frame, deg in ((0, 0), (30, 45), (60, 90)):
            w = self.world(clip, frame)
            turn = about("z", deg)
            want_q = osk.qmul(turn, self.rest_world[hips][0])
            self.assertTrue(rig.same_rotation(w[hips][0], want_q, 1e-5), (frame, w[hips][0]))
            off = tuple(a - b for a, b in zip(self.rest_world[head][1], self.rest_world[hips][1]))
            want_p = tuple(a + b for a, b in zip(self.rest_world[hips][1], osk.qrot(turn, off)))
            for a, b in zip(w[head][1], want_p):
                self.assertAlmostEqual(a, b, delta=1e-3)
        start = self.world(clip, 0)
        for j in range(len(start)):
            for a, b in zip(start[j][1], self.rest_world[j][1]):
                self.assertAlmostEqual(a, b, delta=1e-4)

    def test_keys_go_back_to_the_pose(self):
        keys = [{"t": 0.0}, {"t": 0.25, "turn": {"Head": ["x", -20]}}, {"t": 0.5}]
        clip = mc.make(self.sk, self.sk["rest"], keys)
        head = self.sk["names"].index("Head")
        mid = self.world(clip, 15)
        end = self.world(clip, clip["frameCount"])
        self.assertTrue(rig.same_rotation(mid[head][0], osk.qmul(about("x", -20), self.rest_world[head][0]), 1e-5))
        self.assertTrue(rig.same_rotation(end[head][0], self.rest_world[head][0], 1e-5))

    def test_a_moving_base(self):
        """bases: a pose per frame instead of one pose; the turns go on top of each."""
        moving = gc.timeline_poses(self.gltf, self.binc, self.sk, [[0.0, "Move", 0.0], [0.5, "Move", 1.0]])
        self.assertEqual(len(moving), 31)
        clip = mc.make(self.sk, None, [{"t": 0.0}, {"t": 0.5, "turn": {"Head": ["x", -20]}}], bases=moving)
        plain = gc.timeline(self.gltf, self.binc, self.sk, [[0.0, "Move", 0.0], [0.5, "Move", 1.0]], 0)
        head = self.sk["names"].index("Head")
        for frame, deg in ((0, 0), (30, -20)):
            got, was = self.world(clip, frame), self.world(plain, frame)
            self.assertTrue(rig.same_rotation(got[head][0], osk.qmul(about("x", deg), was[head][0]), 1e-4), frame)
        with self.assertRaises(ValueError):
            mc.make(self.sk, None, [{"t": 0.0}, {"t": 1.0}], bases=moving)

    def test_a_move_shifts_a_joint_and_its_children(self):
        keys = [{"t": 0.0}, {"t": 0.5, "move": {"Hips": [0.0, 0.2, 0.1]}}, {"t": 1.0}]
        clip = mc.make(self.sk, self.sk["rest"], keys)
        for frame, by in ((0, (0, 0, 0)), (30, (0.0, 0.2, 0.1)), (60, (0, 0, 0))):
            w = self.world(clip, frame)
            for name in ("Hips", "Head"):
                j = self.sk["names"].index(name)
                for a, b, c in zip(w[j][1], self.rest_world[j][1], by):
                    self.assertAlmostEqual(a, b + c, delta=2e-3, msg=(frame, name))
        with self.assertRaises(ValueError):
            mc.make(self.sk, self.sk["rest"], [{"t": 0.0}, {"t": 1.0, "move": {"Hips": [1, 2]}}])

    def test_mistakes_are_refused(self):
        for keys in ([{"t": 0.5}], [{"t": 0.0}, {"t": 1.0, "turn": {"Wing": ["z", 9]}}],
                     [{"t": 0.0}, {"t": 1.0, "turn": {"Hips": ["w", 9]}}], [{"t": 0.0}, {"t": 0.0}]):
            with self.assertRaises(ValueError, msg=str(keys)):
                mc.make(self.sk, self.sk["rest"], keys)


class OffsetTests(Fixture):
    def test_an_offset_turns_a_joint_about_the_creatures_axis(self):
        """An offset (for a graph's additive layer: the wolf looking into a turn) holds, per joint, the turn about the
        creature's axis in the joint's own frame, with no move and scale 0; other joints add nothing."""
        clip = mc.offset(self.sk, {"Head": ["y", 30]}, hierarchy_hash=5)
        self.assertEqual((clip["frameCount"], clip["flags"], clip["hierarchyHash"]), (9, 0, 5))
        head = self.sk["names"].index("Head")
        for frame in (0, 4):
            pose = ja.sample(clip, frame, self.sk["rest"])
            for j, (q, t, s) in enumerate(pose):
                self.assertEqual(t, (0.0, 0.0, 0.0))
                self.assertEqual(s, (0.0, 0.0, 0.0))
                if j != head:
                    self.assertTrue(rig.same_rotation(q, (0.0, 0.0, 0.0, 1.0), 1e-6), (j, q))
            w = self.rest_world[head][0]
            turned = osk.qmul(w, pose[head][0])  # the joint's rest frame with the offset added in its own frame
            self.assertTrue(rig.same_rotation(turned, osk.qmul(about("y", 30), w), 1e-4))

    def test_offset_mistakes(self):
        for turn in ({"Wing": ["y", 5]}, {"Head": ["w", 5]}):
            with self.assertRaises(ValueError, msg=str(turn)):
                mc.offset(self.sk, turn)


class SpeedTests(Fixture):
    def test_faster_is_shorter_and_plays_the_same_poses(self):
        slow = gc.clip(self.gltf, self.binc, self.sk, "Move", 1)
        fast = gc.clip(self.gltf, self.binc, self.sk, "Move", 1, speed=2.0)
        self.assertEqual((slow["frameCount"], fast["frameCount"]), (60, 30))
        head = self.sk["names"].index("Head")
        a, b = self.world(slow, 40), self.world(fast, 20)
        self.assertTrue(rig.same_rotation(a[head][0], b[head][0], 1e-6))


if __name__ == "__main__":
    unittest.main()
