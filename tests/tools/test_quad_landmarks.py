"""tools/models/quad_landmarks.py: the landmarks of a four-legged body and the height map fit_to_game_skeleton.py uses
(plain Python, no Blender)."""
import collections
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "models"))
import quad_landmarks as ql  # noqa: E402

P = collections.namedtuple("P", "x y z")


def frange(a, b, step=0.05):
    n = int(round((b - a) / step))
    return [a + i * step for i in range(n + 1)]


def quadruped(front_ground=0.0, back_ground=0.0, tail_to=None):
    """A box body (y -1..1, z 0.8..1.4), a head in front (y -1.4), four legs at y -0.8 and 0.7 (x +-0.3) down to their
    ground, and optionally a long tail drooping behind from z 1.2 to `tail_to` at y 2.4 (the game sabretooth's ends at
    0.3; Meshy's panther's nearly at its feet). Forward is -y."""
    pts = []
    for y in frange(-1.0, 1.0):
        for z in frange(0.8, 1.4):
            for x in (-0.35, 0.35):
                pts.append(P(x, y, z))
    for y in frange(-1.5, -1.0):
        for z in frange(1.0, 1.6):
            pts.append(P(0.0, y, z))
    for ly, ground in ((-0.8, front_ground), (0.7, back_ground)):
        for x in (-0.3, 0.3):
            for z in frange(ground, 0.8):
                for dy in (-0.05, 0.0, 0.05):
                    pts.append(P(x, ly + dy, z))
    if tail_to is not None:
        for i, y in enumerate(frange(1.0, 2.4)):
            pts.append(P(0.0, y, 1.2 - (1.2 - tail_to) * i / 28))
    return pts


class LandmarkTests(unittest.TestCase):
    def test_a_level_body(self):
        m = ql.landmarks(quadruped())
        self.assertAlmostEqual(m["front"], -0.8, delta=0.03)
        self.assertAlmostEqual(m["back"], 0.7, delta=0.03)
        self.assertAlmostEqual(m["ground"], 0.0, delta=1e-6)
        self.assertAlmostEqual(m["ground_front"], 0.0, delta=1e-6)
        self.assertAlmostEqual(m["ground_back"], 0.0, delta=1e-6)
        self.assertAlmostEqual(m["nose"], -1.5, delta=1e-6)
        self.assertAlmostEqual(m["top"], 1.4, delta=1e-6)
        self.assertAlmostEqual(m["belly"], 0.8, delta=1e-6)
        self.assertAlmostEqual(m["width"], 0.35, delta=1e-6)

    def test_front_paws_below_the_hind_ones_and_a_long_tail(self):
        """The game sabretooth's rest pose: front paws 0.35 m below the hind ones, a tail drooping far behind."""
        m = ql.landmarks(quadruped(front_ground=-0.35, tail_to=0.3))
        self.assertAlmostEqual(m["front"], -0.8, delta=0.03)
        self.assertAlmostEqual(m["back"], 0.7, delta=0.03)  # not the front paws again, not the tail
        self.assertAlmostEqual(m["ground_front"], -0.35, delta=1e-6)
        self.assertAlmostEqual(m["ground_back"], 0.0, delta=1e-6)
        self.assertAlmostEqual(m["ground"], -0.35, delta=1e-6)

    def test_a_tail_that_droops_lower_than_the_feet_is_not_the_hind_feet(self):
        m = ql.landmarks(quadruped(back_ground=0.02, tail_to=0.0))
        self.assertAlmostEqual(m["back"], 0.7, delta=0.03)
        self.assertAlmostEqual(m["ground_back"], 0.02, delta=1e-6)


class HeightTests(unittest.TestCase):
    def setUp(self):
        self.c = ql.landmarks(quadruped())                         # the creature: level feet
        self.g = ql.landmarks(quadruped(front_ground=-0.35, tail_to=0.3))  # the game body

    def test_each_pair_of_feet_reaches_its_own_ground(self):
        self.assertAlmostEqual(ql.fit_z(0.0, -0.8, self.c, self.g), -0.35, delta=1e-6)
        self.assertAlmostEqual(ql.fit_z(0.0, 0.7, self.c, self.g), 0.0, delta=1e-6)

    def test_the_body_above_the_belly_is_not_stretched(self):
        self.assertAlmostEqual(ql.fit_z(1.4, 0.0, self.c, self.g), 1.4, delta=1e-6)
        self.assertAlmostEqual(ql.fit_z(0.8, -0.8, self.c, self.g), 0.8, delta=1e-6)
        self.assertAlmostEqual(ql.fit_z(1.1, -0.8, self.c, self.g), 1.1, delta=1e-6)

    def test_a_front_leg_stretches_evenly(self):
        # halfway down the leg (0.4 of 0.8 m) maps halfway down the game's longer leg (0.8 + 0.35 m)
        self.assertAlmostEqual(ql.fit_z(0.4, -0.8, self.c, self.g), 0.8 - 0.575, delta=1e-6)

    def test_a_level_game_body_is_a_plain_scale(self):
        g = ql.landmarks(quadruped())
        self.assertAlmostEqual(ql.fit_z(0.4, -0.8, self.c, g), 0.4, delta=1e-6)
        self.assertAlmostEqual(ql.fit_z(0.0, 0.7, self.c, g), 0.0, delta=1e-6)


class WidthTests(unittest.TestCase):
    """A creature stretched to a bigger body keeps its own proportions with "keep" (Meshy's panther, scaled 2x in length
    and height onto the sabretooth, came out half as wide as designed: paper thin in game)."""

    def setUp(self):
        self.c = ql.landmarks(quadruped())   # body 0.7 wide, feet 1.5 apart, 1.4 tall
        big = [P(p.x * 1.2, p.y * 2.0, p.z * 2.0) for p in quadruped()]  # 2x long and tall, only 1.2x wide
        self.g = ql.landmarks(big)

    def test_fit_squeezes_the_width_to_the_game_body(self):
        self.assertAlmostEqual(ql.width_scale(self.c, self.g, "fit"), 1.2, delta=1e-6)

    def test_keep_scales_the_width_with_length_and_height(self):
        self.assertAlmostEqual(ql.width_scale(self.c, self.g, "keep"), 2.0, delta=1e-6)

    def test_a_number_is_used_as_it_is(self):
        self.assertAlmostEqual(ql.width_scale(self.c, self.g, "1.5"), 1.5, delta=1e-6)


if __name__ == "__main__":
    unittest.main()
