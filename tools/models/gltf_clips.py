"""glTF animations made into game clips (keen::JointAnimation) for a skeleton from new_skeleton.py.

A clip has a key at every frame at 60 fps (the rate of every game clip) from the animation's first key to its last:
N frames (the duration times the rate, rounded), the animation sampled at N + 1 evenly spaced times, so a duration
that is not a whole number of frames plays slightly faster or slower and still ends on its last key. Channels are
sampled as glTF defines them (LINEAR: linear, rotations by slerp; STEP: held; CUBICSPLINE: Hermite with the stored
tangents), nodes without a channel keep their own pose, and every frame is a pose of the skeleton
(new_skeleton.pose: each joint as in the file's world, in game space).
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gltf_in  # noqa: E402
import joint_animation as ja  # noqa: E402
import new_skeleton as ns  # noqa: E402

PATHS = ("translation", "rotation", "scale")


def names(gltf):
    return [a.get("name", "animation%d" % i) for i, a in enumerate(gltf.get("animations", []))]


def _closed(channels):
    """Whether every channel ends on its first value (a loop exported with its closing frame)."""
    for _, path, interp, _, values in channels:
        a, b = (values[1], values[-2]) if interp == "CUBICSPLINE" else (values[0], values[-1])
        if path == "rotation":
            if 1 - abs(sum(x * y for x, y in zip(a, b))) > 1e-6:
                return False
        elif max(abs(x - y) for x, y in zip(a, b)) > 1e-6:
            return False
    return True


def _step(channels):
    """The animation's key step: the most common time between keys."""
    steps = {}
    for c in channels:
        for t0, t1 in zip(c[3], c[3][1:]):
            k = round(t1 - t0, 5)
            steps[k] = steps.get(k, 0) + 1
    return max(steps, key=lambda k: (steps[k], k)) if steps else 0.0


def _travel(gltf, channels, node):
    """The node's travel along the ground over the animation (glTF world, y up: x and z only), from its first key to
    its last."""
    start, end = _span(channels)
    w0 = ns.node_worlds(gltf, _frame(channels, start))
    w1 = ns.node_worlds(gltf, _frame(channels, end))
    return (w1[node][0][3] - w0[node][0][3], 0.0, w1[node][2][3] - w0[node][2][3]), w0


def _take_travel_out(gltf, channels, node):
    """The channels with the node's travel along the ground taken out of its translation keys, evenly over the
    animation, and the node brought back over its resting spot where it starts away from it (the last part of a
    charge starts where the run before it ended); in its parent's space at the first key (a parent that holds
    still). Its bobbing and swaying stay."""
    d, w0 = _travel(gltf, channels, node)
    rest = ns.node_worlds(gltf)[node]
    off = (w0[node][0][3] - rest[0][3], 0.0, w0[node][2][3] - rest[2][3])
    start, end = _span(channels)
    length = end - start
    parent = ns.parents_of(gltf).get(node)
    inv = gltf_in.mat_inv_affine(w0[parent]) if parent is not None else [[float(r == c) for c in range(4)]
                                                                         for r in range(4)]
    local = tuple(sum(inv[r][c] * d[c] for c in range(3)) for r in range(3))
    local_off = tuple(sum(inv[r][c] * off[c] for c in range(3)) for r in range(3))
    out, found = [], False
    for nd, path, interp, times, values in channels:
        if nd == node and path == "translation" and length > 0:
            found = True
            values = [tuple(v) for v in values]
            if interp == "CUBICSPLINE":
                new = []
                for k, t in enumerate(times):
                    a, v, b = values[3 * k:3 * k + 3]
                    f = (t - start) / length
                    new += [tuple(x - y / length for x, y in zip(a, local)),
                            tuple(x - y * f - o for x, y, o in zip(v, local, local_off)),
                            tuple(x - y / length for x, y in zip(b, local))]
                values = new
            else:
                values = [tuple(x - y * (t - start) / length - o for x, y, o in zip(v, local, local_off))
                          for t, v in zip(times, values)]
        out.append((nd, path, interp, times, values))
    if not found and any(abs(c) > 1e-9 for c in d + off):
        raise ValueError("node %s travels but has no translation keys of its own: name the joint that carries the "
                         "travel" % gltf["nodes"][node].get("name", node))
    return out


def _node_of(sk, joint):
    if joint not in sk["names"]:
        raise KeyError("no joint %r in the skeleton" % joint)
    return sk["nodes"][sk["names"].index(joint)]


def travel_speed(gltf, binc, sk, name, joint):
    """How fast the joint travels along the ground in the animation (metres a second, times the skeleton's scale):
    the speed the game must move the creature at for its feet not to slide, once the travel is taken out."""
    channels = _read(gltf, binc, name)
    start, end = _span(channels)
    d, _ = _travel(gltf, channels, _node_of(sk, joint))
    return math.hypot(d[0], d[2]) * sk["scale"] / (end - start) if end > start else 0.0


def _channels(gltf, binc, name, loop=False, in_place=None):
    """[(node, path, interpolation, times, values)] of the animation called name; in_place: a node whose travel
    along the ground is taken out (_take_travel_out). For a loop exported without its closing frame (its last key is
    not its first again), every channel gets one more key a key step after the last, with its first value: the loop
    then wraps over that step."""
    out = _read(gltf, binc, name)
    if in_place is not None:
        out = _take_travel_out(gltf, out, in_place)
    if loop and not _closed(out):
        start, end = _span(out)
        end += _step(out)
        out = [(node, path, interp, list(times) + [end], list(values) + list(values[:3] if interp == "CUBICSPLINE"
                                                                              else values[:1]))
               for node, path, interp, times, values in out]
    return out


def _read(gltf, binc, name):
    for i, a in enumerate(gltf.get("animations", [])):
        if a.get("name", "animation%d" % i) == name:
            break
    else:
        raise KeyError("no animation %r (the file has %s)" % (name, ", ".join(names(gltf)) or "none"))
    out = []
    for ch in a["channels"]:
        target = ch["target"]
        if target.get("path") not in PATHS or "node" not in target:
            continue
        s = a["samplers"][ch["sampler"]]
        times = gltf_in.accessor(gltf, binc, s["input"])
        values = gltf_in.accessor(gltf, binc, s["output"])
        out.append((target["node"], target["path"], s.get("interpolation", "LINEAR"), times, values))
    if not out:
        raise ValueError("animation %r moves no node" % name)
    return out


def _span(channels):
    return min(c[3][0] for c in channels), max(c[3][-1] for c in channels)


def duration(gltf, binc, name, loop=False):
    start, end = _span(_channels(gltf, binc, name, loop))
    return end - start


def frame_count(gltf, binc, name, fps=60, loop=False):
    return max(1, int(round(duration(gltf, binc, name, loop) * fps)))


def _slerp(a, b, s):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0:
        b, d = tuple(-x for x in b), -d
    if d > 0.9995:
        q = tuple(x + (y - x) * s for x, y in zip(a, b))
    else:
        th = math.acos(min(1.0, d))
        wa, wb = math.sin((1 - s) * th) / math.sin(th), math.sin(s * th) / math.sin(th)
        q = tuple(wa * x + wb * y for x, y in zip(a, b))
    n = math.sqrt(sum(x * x for x in q))
    return tuple(x / n for x in q)


def _sample(interp, times, values, t, rotation):
    cubic = interp == "CUBICSPLINE"

    def value(k):
        return tuple(values[3 * k + 1] if cubic else values[k])

    if t <= times[0] or len(times) == 1:
        v = value(0)
    elif t >= times[-1]:
        v = value(len(times) - 1)
    else:
        k = max(i for i in range(len(times) - 1) if times[i] <= t)
        d = times[k + 1] - times[k]
        s = (t - times[k]) / d if d > 0 else 0.0
        if interp == "STEP":
            v = value(k)
        elif cubic:
            s2, s3 = s * s, s * s * s
            out_tan, in_tan = values[3 * k + 2], values[3 * (k + 1)]
            v = tuple((2 * s3 - 3 * s2 + 1) * a + (s3 - 2 * s2 + s) * d * b + (-2 * s3 + 3 * s2) * c
                      + (s3 - s2) * d * e for a, b, c, e in zip(value(k), out_tan, value(k + 1), in_tan))
        elif rotation:
            return _slerp(value(k), value(k + 1), s)
        else:
            v = tuple(a + (b - a) * s for a, b in zip(value(k), value(k + 1)))
    if rotation:
        n = math.sqrt(sum(x * x for x in v)) or 1.0
        v = tuple(x / n for x in v)
    return v


def _frame(channels, t):
    pose = {}
    for node, path, interp, times, values in channels:
        pose.setdefault(node, {})[path] = _sample(interp, times, values, t, path == "rotation")
    return pose


def pose_at(gltf, binc, sk, name, t, loop=False, in_place=None):
    """The skeleton's pose (per joint local rotation, position, scale, game space) t seconds into the animation;
    in_place: a joint whose travel along the ground is taken out (clip)."""
    channels = _channels(gltf, binc, name, loop, _node_of(sk, in_place) if in_place else None)
    start, _ = _span(channels)
    return ns.pose(sk, ns.node_worlds(gltf, _frame(channels, start + t)), gltf)


def clip(gltf, binc, sk, name, hierarchy_hash, loop=False, fps=60, speed=1.0, in_place=None):
    """The animation as a game clip for the skeleton (hierarchy_hash: new_skeleton.clip_hash of its hierarchy); a
    loop ends on its first pose; speed above 1 plays it faster (the same poses in fewer frames). in_place: the joint
    that carries the creature forward in an animation with root motion (a walk that travels): its travel along the
    ground is taken out, the game moving creatures itself (travel_speed: how fast it went)."""
    channels = _channels(gltf, binc, name, loop, _node_of(sk, in_place) if in_place else None)
    start, end = _span(channels)
    n = max(1, int(round((end - start) / float(speed) * fps)))
    poses = [ns.pose(sk, ns.node_worlds(gltf, _frame(channels, start + (end - start) * f / n)), gltf)
             for f in range(n + 1)]
    return ja.encode(poses, sk["rest"], fps=fps, loop=loop, hierarchy_hash=hierarchy_hash)


def _blend(a, b, s):
    """Pose a turned s of the way into pose b: rotations by slerp, positions and scales in a straight line."""
    return [(_slerp(qa, qb, s), tuple(x + (y - x) * s for x, y in zip(pa, pb)),
             tuple(x + (y - x) * s for x, y in zip(sa, sb)))
            for (qa, pa, sa), (qb, pb, sb) in zip(a, b)]


def timeline_poses(gltf, binc, sk, keys, fps=60):
    """The poses, one per frame, of a clip that times an animation's moments (a swing that must land when the game's
    attack hits, two swings in one combo): keys [at, animation, time], seconds into the clip, the glb's animation and
    seconds into it; the first at 0, each at or after the one before, the last at the clip's length. Between two keys
    forward through one animation the clip plays that stretch of it, faster or slower; between any other two (back in
    time, another animation) the first pose blends into the second, eased in and out (3u^2 - 2u^3); two keys at one
    moment cut."""
    if len(keys) < 2:
        raise ValueError("a timeline needs two keys or more")
    if any(len(k) != 3 for k in keys):
        raise ValueError("a timeline key is [at, animation, time]: %s" % ([k for k in keys if len(k) != 3][0],))
    keys = [(float(at), name, float(t)) for at, name, t in keys]
    ats = [k[0] for k in keys]
    if ats[0] != 0.0:
        raise ValueError("a timeline starts at 0, not %g" % ats[0])
    if any(b < a for a, b in zip(ats, ats[1:])):
        raise ValueError("a timeline's keys go forward in time: %s" % ats)
    length = ats[-1]
    if length <= 0.0:
        raise ValueError("a timeline lasts longer than 0 s")
    channels = {}
    for _, name, _ in keys:
        if name not in channels:
            channels[name] = _channels(gltf, binc, name)
    held = {}

    def pose(name, t):
        if (name, t) not in held:
            ch = channels[name]
            held[(name, t)] = ns.pose(sk, ns.node_worlds(gltf, _frame(ch, _span(ch)[0] + t)), gltf)
        return held[(name, t)]

    n = max(1, int(round(length * fps)))
    poses = []
    for f in range(n + 1):
        t = length * f / n
        i = max(i for i in range(len(keys) - 1) if ats[i] <= t)
        (a0, n0, t0), (a1, n1, t1) = keys[i], keys[i + 1]
        u = (t - a0) / (a1 - a0) if a1 > a0 else 1.0
        if n0 == n1 and t1 >= t0:
            ch = channels[n0]
            poses.append(ns.pose(sk, ns.node_worlds(gltf, _frame(ch, _span(ch)[0] + t0 + (t1 - t0) * u)), gltf))
        else:
            poses.append(_blend(pose(n0, t0), pose(n1, t1), u * u * (3 - 2 * u)))
    return poses


def timeline(gltf, binc, sk, keys, hierarchy_hash, fps=60):
    """timeline_poses as a clip."""
    return ja.encode(timeline_poses(gltf, binc, sk, keys, fps), sk["rest"], fps=fps, loop=False,
                     hierarchy_hash=hierarchy_hash)
