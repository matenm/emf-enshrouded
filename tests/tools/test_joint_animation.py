"""tools/models/joint_animation.py: the game's animation clip format (keen::JointAnimation), read and written."""
import glob
import json
import math
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "models"))
import joint_animation as ja  # noqa: E402

GAME_CLIPS = glob.glob(os.path.join(HERE, "..", "..", "analysis", "model_survey", "anim", "j*_*.json"))
IDENTITY = ((0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0), (1.0, 1.0, 1.0))


def units(raw):
    """Bytes as the clip's data array (16-byte units, as EML shows them)."""
    raw = raw + bytes((-len(raw)) % 16) + bytes(16)
    return [{"data": list(raw[i:i + 16])} for i in range(0, len(raw), 16)]


def header(used_rot, nonconst_rot, used_pos=0, nonconst_pos=0, used_scl=0, nonconst_scl=0, start=0, offset=0):
    return {"dataOffset": offset, "startTime": start, "endTime": start + 63,
            "usedOrientationJoints": used_rot, "nonConstOrientationJoints": nonconst_rot,
            "usedPositionJoints": used_pos, "nonConstPositionJoints": nonconst_pos,
            "usedScaleJoints": used_scl, "nonConstScaleJoints": nonconst_scl}


def angle(a, b):
    """Angle in radians between two rotations (quaternions, normalised first: the game leaves constant rotations as
    quantised, a hair off unit length)."""
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(x * x for x in b))
    d = abs(sum(x * y for x, y in zip(a, b))) / (na * nb)
    return 2 * math.acos(min(1.0, d))


def turn(axis, rad):
    s = math.sin(rad / 2)
    return tuple(c * s for c in axis) + (math.cos(rad / 2),)


class HandMadeClipTests(unittest.TestCase):
    """A clip written byte by byte from the format notes: two joints, one block, two frames."""

    def clip(self):
        # rotations: joint 0 keyed at frames 0 and 2 (mask 0b101): from identity to 90 degrees about z, tangents 0;
        # joint 1 constant (180 degrees about x). Positions and scales: both joints take the rest pose.
        rot = struct.pack("<QH", 0b101, 0)
        rot += struct.pack("<8h", 0, 0, 0, 32767, 0, 0, 0, 0)
        rot += struct.pack("<8h", 0, 0, 23170, 23170, 0, 0, 0, 0)
        rot += struct.pack("<H4h", 1, 32767, 0, 0, 0)
        rot += struct.pack("<H", 0)  # no joint takes the rest rotation
        rot += bytes((-len(rot)) % 8)
        pos = struct.pack("<HHH", 2, 0, 1) + bytes(2)  # both joints: rest position; padding to 8
        scl = struct.pack("<HHH", 2, 0, 1)
        return {"hierarchyHash": 1, "positionFactor": 1e-4, "positionTangent": 1e-6, "scaleFactor": 3e-5,
                "headers": [header(2, 1)], "data": units(rot + pos + scl), "fps": 60, "frameCount": 2,
                "jointCount": 2, "flags": 0}

    def test_keys_are_read_and_a_frame_between_them_is_the_spline(self):
        rest = [((0.0, 0.0, 0.0, 1.0), (0.0, 0.5, 0.0), (1.0, 1.0, 1.0))] * 2
        pose = ja.sample(self.clip(), 1, rest)
        # t = 0.5 with zero tangents: half of each key (2t^3 - 3t^2 + 1 = 0.5), normalised
        q = pose[0][0]
        self.assertAlmostEqual(angle(q, turn((0, 0, 1), math.pi / 4)), 0.0, places=3)
        self.assertAlmostEqual(angle(pose[1][0], (1.0, 0.0, 0.0, 0.0)), 0.0, places=4)
        self.assertEqual(pose[0][1], (0.0, 0.5, 0.0))  # the rest position
        self.assertEqual(pose[1][2], (1.0, 1.0, 1.0))

    def test_the_last_frame_is_sampled_just_before_the_end_key(self):
        # a clip that does not loop holds its last frame (frameCount - 1) at fraction 0.999
        rest = [IDENTITY] * 2
        q = ja.sample(self.clip(), 5, rest)[0][0]
        self.assertLess(angle(q, turn((0, 0, 1), math.pi / 2)), 1e-3)  # frame 1 itself would be 45 degrees


def motion(frames, joints=3):
    """A smooth test motion per frame: joint j turns about its own axis, moves and scales a little."""
    axes = ((0, 0, 1), (1, 0, 0), (0, 1, 0))
    out = []
    for f in range(frames + 1):
        t = f / frames
        pose = []
        for j in range(joints):
            q = turn(axes[j % 3], 0.8 * math.sin(2 * math.pi * t + j))
            p = (0.1 * j, 0.3 + 0.05 * math.sin(2 * math.pi * t), -0.02 * j * math.cos(2 * math.pi * t))
            s = (1.0, 1.0, 1.0) if j != 2 else (1.0 + 0.1 * math.sin(2 * math.pi * t),) * 3
            pose.append((q, p, s))
        out.append(pose)
    return out


class RoundTripTests(unittest.TestCase):
    def check(self, poses, clip, rest, frames):
        for f in range(frames):
            got = ja.sample(clip, f, rest)
            for j, (q, p, s) in enumerate(poses[f]):
                self.assertLess(angle(got[j][0], q), 1e-3, "frame %d joint %d" % (f, j))
                for a, b in zip(got[j][1], p):
                    self.assertAlmostEqual(a, b, delta=1e-4)
                for a, b in zip(got[j][2], s):
                    self.assertAlmostEqual(a, b, delta=1e-4)

    def test_a_looping_clip_comes_back(self):
        poses = motion(60)
        rest = [IDENTITY] * 3
        clip = ja.encode(poses, rest, fps=60, loop=True, hierarchy_hash=7)
        self.assertEqual((clip["frameCount"], clip["flags"], clip["jointCount"]), (60, 1, 3))
        self.check(poses, clip, rest, 60)
        # frame 60 of a 60-frame loop is frame 0 again
        self.assertLess(angle(ja.sample(clip, 60, rest)[0][0], poses[0][0][0]), 1e-3)

    def test_a_long_clip_is_cut_into_blocks_of_63_frames(self):
        poses = motion(130)
        rest = [IDENTITY] * 3
        clip = ja.encode(poses, rest, fps=60, loop=False, hierarchy_hash=7)
        self.assertEqual([(h["startTime"], h["endTime"]) for h in clip["headers"]], [(0, 63), (63, 126), (126, 189)])
        self.check(poses, clip, rest, 130)

    def test_still_joints_are_constant_or_take_the_rest_pose(self):
        poses = [[(turn((0, 0, 1), 0.5), (0.0, 0.2, 0.0), (1.0, 1.0, 1.0)), IDENTITY] for _ in range(11)]
        rest = [IDENTITY, IDENTITY]
        clip = ja.encode(poses, rest, fps=60, loop=False, hierarchy_hash=7)
        h = clip["headers"][0]
        self.assertEqual((h["usedOrientationJoints"], h["nonConstOrientationJoints"]), (1, 0))
        self.assertEqual((h["usedPositionJoints"], h["nonConstPositionJoints"]), (1, 0))
        self.assertEqual((h["usedScaleJoints"], h["nonConstScaleJoints"]), (0, 0))
        self.check(poses, clip, rest, 10)

    def test_a_joint_at_the_rest_pose_does_not_coarsen_the_others(self):
        # joint 1 stays at a rest position 100 m out: the position scale follows joint 0's small motion
        rest = [IDENTITY, ((0.0, 0.0, 0.0, 1.0), (100.0, 0.0, 0.0), (1.0, 1.0, 1.0))]
        poses = [[((0.0, 0.0, 0.0, 1.0), (0.01 * math.sin(f / 3), 0.0, 0.0), (1.0, 1.0, 1.0)), rest[1]]
                 for f in range(21)]
        clip = ja.encode(poses, rest, fps=60, loop=False, hierarchy_hash=7)
        self.assertLess(clip["positionFactor"], 0.011 / 32767)
        self.check(poses, clip, rest, 20)

    def test_a_smooth_motion_keeps_a_fraction_of_its_frames_as_keys(self):
        # the game samples a Hermite spline between keys: a frame the spline already gives is not stored (EML holds
        # two Lua references per 16 bytes of clip data while it builds content, and all mods share 1,000,000)
        poses = motion(60)
        rest = [IDENTITY] * 3
        clip = ja.encode(poses, rest, fps=60, loop=True, hierarchy_hash=7)
        blocks = ja.parse(clip)
        keys = sum(len(k) for blk in blocks for name in ("rot", "pos", "scl") for _, k in blk[name][0].values())
        tracks = sum(len(blk[name][0]) for blk in blocks for name in ("rot", "pos", "scl"))
        self.assertLess(keys, tracks * 61 / 3)  # under a third of the 61 frames per animated track
        self.check(poses, clip, rest, 60)

    def test_a_sudden_turn_keeps_the_keys_it_needs(self):
        # still, then 90 degrees in two frames, then still: the frames around the turn stay keys
        def at(f):
            return turn((0, 0, 1), 0.0 if f < 20 else (math.pi / 4 if f == 20 else math.pi / 2))
        poses = [[(at(f), (0.0, 0.0, 0.0), (1.0, 1.0, 1.0))] for f in range(41)]
        clip = ja.encode(poses, [IDENTITY], fps=60, loop=False, hierarchy_hash=7)
        self.check(poses, clip, [IDENTITY], 40)

    def test_a_quaternion_that_flips_sign_is_not_a_spin(self):
        # q and -q are the same rotation: the encoder keeps neighbouring keys on the same side
        poses = [[(tuple(c * (1 if f % 2 else -1) for c in turn((0, 1, 0), 0.01 * f)), (0, 0, 0), (1, 1, 1))]
                 for f in range(21)]
        clip = ja.encode(poses, [IDENTITY], fps=60, loop=False, hierarchy_hash=7)
        self.check(poses, clip, [IDENTITY], 20)


SABRETOOTH = os.path.join(HERE, "..", "..", "analysis", "model_survey", "hierarchies",
                          "ef2d7e9f-1a5e-4d7e-a3e5-d47448168385.json")


class HierarchyHashTests(unittest.TestCase):
    """A clip's hierarchyHash names its skeleton: FNV-1a 64 over the joint name hashes (u32 each, in joint order)."""

    def test_fnv1a_64(self):
        self.assertEqual(ja.fnv1a64(b"a"), 0xaf63dc4c8601ec8c)  # the published FNV-1a 64 test vector
        self.assertEqual(ja.fnv1a64(b""), 0xcbf29ce484222325)

    def test_a_skeleton_of_two_joints(self):
        self.assertEqual(ja.hierarchy_hash([1, 2]), ja.fnv1a64(struct.pack("<II", 1, 2)))

    @unittest.skipUnless(os.path.exists(SABRETOOTH), "needs the sabretooth skeleton dumped by mods/model_survey")
    def test_the_sabretooths_clips_name_its_skeleton(self):
        with open(SABRETOOTH, encoding="utf-8") as f:
            hier = json.load(f)
        h = ja.hierarchy_hash(hier["jointNames"][:hier["jointCount"]])
        self.assertEqual(h - (1 << 64) if h >= 1 << 63 else h, -1033468943642191884)  # its clips' hierarchyHash


@unittest.skipUnless(GAME_CLIPS, "needs game clips dumped by mods/model_survey (analysis/model_survey/anim)")
class GameClipTests(unittest.TestCase):
    def test_a_game_clip_re_encoded_samples_the_same(self):
        for path in GAME_CLIPS:
            with open(path, encoding="utf-8") as f:
                clip = json.load(f)
            rest = [IDENTITY] * clip["jointCount"]
            frames = clip["frameCount"]
            poses = [ja.sample(clip, f, rest) for f in range(frames)]
            poses.append(ja.sample(clip, frames, rest) if clip["flags"] == 1 else ja.sample_end(clip, rest))
            again = ja.encode(poses, rest, fps=clip["fps"], loop=clip["flags"] == 1,
                              hierarchy_hash=clip["hierarchyHash"])
            for f in range(frames):
                a, b = poses[f], ja.sample(again, f, rest)
                for j in range(clip["jointCount"]):
                    self.assertLess(angle(a[j][0], b[j][0]), 2e-3, "%s frame %d joint %d" % (path, f, j))
                    for x, y in zip(a[j][1], b[j][1]):
                        self.assertAlmostEqual(x, y, delta=1e-3)


if __name__ == "__main__":
    unittest.main()
