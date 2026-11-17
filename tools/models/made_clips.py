"""Clips made from a few key turns on top of a pose: a death, a flinch or a lean for a creature whose own animations
lack one.

Keys: [{"t": seconds, "turn": {joint name: [axis, degrees]}, "move": {joint name: [x, y, z]}}, ...], the first at
0 s, times rising. A move shifts a joint (and what hangs on it) by that much along the creature's axes as the pose
has them before the turns, in the skeleton's units; it eases as a turn does (a leap, a hop back). A key's turns
are where those joints stand turned at that time (a joint a key does not name is not turned then); between two keys
every joint's turn eases (smoothstep, slerp) from the one to the other, and the clip ends on the last key. An axis is
"x", "y", "z" or a vector in the creature's own frame (game space: x across, y up, z forward), carried along by the
turns of the joint's parents: a neck turned about x after the body rolled onto its side still nods along the body.

offset() makes a clip for a graph's additive layer (offsets added to the pose, as the game wolf's lean that turns its
neck and head into a turn): per joint a turn about the creature's axis expressed in the joint's own rest frame, no move
and scale 0 (an additive layer adds scales).
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gltf_clips  # noqa: E402
import joint_animation  # noqa: E402
import own_skeleton as osk  # noqa: E402

AXES = {"x": (1.0, 0.0, 0.0), "y": (0.0, 1.0, 0.0), "z": (0.0, 0.0, 1.0)}
IDENTITY = (0.0, 0.0, 0.0, 1.0)


def _turn(axis, degrees):
    v = AXES.get(axis) if isinstance(axis, str) else tuple(float(c) for c in axis)
    if v is None or len(v) != 3 or not any(v):
        raise ValueError("a turn's axis is x, y, z or a vector of three numbers, not %r" % (axis,))
    n = math.sqrt(sum(c * c for c in v))
    a = math.radians(float(degrees)) / 2
    return tuple(c / n * math.sin(a) for c in v) + (math.cos(a),)


def _keys(sk, keys):
    if not keys or keys[0].get("t") != 0:
        raise ValueError("the first key is at t = 0")
    out, last = [], -1.0
    for k in keys:
        t = float(k["t"])
        if t <= last:
            raise ValueError("key times must rise (%s after %s)" % (t, last))
        last = t
        turns = {}
        for name, (axis, degrees) in k.get("turn", {}).items():
            if name not in sk["names"]:
                raise ValueError("no joint %r in the skeleton" % name)
            turns[sk["names"].index(name)] = _turn(axis, degrees)
        moves = {}
        for name, by in k.get("move", {}).items():
            if name not in sk["names"]:
                raise ValueError("no joint %r in the skeleton" % name)
            if len(by) != 3:
                raise ValueError("a move is [x, y, z], not %r" % (by,))
            moves[sk["names"].index(name)] = tuple(float(c) for c in by)
        out.append((t, turns, moves))
    return out


def turned(sk, base, turns, moves=None):
    """The pose base with the turns {joint index: quaternion} added (each about the creature's axes as the joint's
    parents' turns carried them; children follow) and the moves {joint index: (x, y, z)} (along the creature's
    axes as base has them)."""
    parents = sk["parents"]
    world = osk.world(base, parents)
    new_q, delta, out = [None] * len(base), [None] * len(base), []
    for j, (q, t, s) in enumerate(base):
        p = parents[j]
        dp = delta[p] if p is not None else IDENTITY
        wq = osk.qmul(osk.qmul(dp, turns.get(j, IDENTITY)), world[j][0])
        new_q[j] = wq
        delta[j] = osk.qmul(wq, osk.qconj(world[j][0]))
        if moves and j in moves:
            by = moves[j] if p is None else osk.qrot(osk.qconj(world[p][0]), moves[j])
            t = tuple(a + b for a, b in zip(t, by))
        out.append((wq if p is None else osk.qmul(osk.qconj(new_q[p]), wq), t, s))
    return out


OFFSET_FRAMES = 9  # as long as the game wolf's lean offsets


def offset(sk, turns, hierarchy_hash=0):
    """An offset clip: {joint name: [axis, degrees]}, each a turn about the creature's axis in that joint's rest
    frame (the joint's rest world rotation turned by it); every other joint adds nothing."""
    world = osk.world(sk["rest"], sk["parents"])
    deltas = {}
    for name, (axis, degrees) in turns.items():
        if name not in sk["names"]:
            raise ValueError("no joint %r in the skeleton" % name)
        j = sk["names"].index(name)
        w = world[j][0]
        t = _turn(axis, degrees)
        deltas[j] = osk.qmul(osk.qmul(osk.qconj(w), t), w)
    pose = [(deltas.get(j, IDENTITY), (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)) for j in range(len(sk["rest"]))]
    return joint_animation.encode([pose] * (OFFSET_FRAMES + 1), sk["rest"], hierarchy_hash=hierarchy_hash)


def make(sk, base, keys, fps=60, loop=False, hierarchy_hash=0, bases=None):
    """A clip of the keys on top of the pose base (per joint rotation, position, scale), a key every frame; or on
    top of bases, a pose per frame (a moving base: gltf_clips.timeline_poses at the same fps), as long as the keys."""
    ks = _keys(sk, keys)
    length = ks[-1][0]
    if length <= 0:
        raise ValueError("a made clip needs a key after t = 0")
    n = max(1, int(round(length * fps)))
    if bases is not None and len(bases) != n + 1:
        raise ValueError("the base lasts %g s, the keys %g s" % ((len(bases) - 1) / fps, length))
    poses = []
    for f in range(n + 1):
        t = length * f / n
        i = max(i for i in range(len(ks)) if ks[i][0] <= t)
        if i == len(ks) - 1:
            turns, moves = ks[i][1], ks[i][2]
        else:
            (t0, a, ma), (t1, b, mb) = ks[i], ks[i + 1]
            x = (t - t0) / (t1 - t0)
            s = x * x * (3 - 2 * x)
            turns = {j: gltf_clips._slerp(a.get(j, IDENTITY), b.get(j, IDENTITY), s) for j in set(a) | set(b)}
            zero = (0.0, 0.0, 0.0)
            moves = {j: tuple(u + (v - u) * s for u, v in zip(ma.get(j, zero), mb.get(j, zero)))
                     for j in set(ma) | set(mb)}
        poses.append(turned(sk, base if bases is None else bases[f], turns, moves))
    return joint_animation.encode(poses, sk["rest"], fps=fps, loop=loop, hierarchy_hash=hierarchy_hash)
