"""The game's animation clip format (keen::JointAnimation), read and written.

A clip as EML shows it (tostring of the resource's data, a JSON object): hierarchyHash, positionFactor,
positionTangent, scaleFactor, headers (one per block of 63 frames), data (16-byte units), fps, frameCount,
jointCount, flags (1: loops). Poses are per joint (rotation quaternion x, y, z, w; position x, y, z; scale x, y, z),
local to the parent joint, in game space (left-handed; gamegltf.py mirrors X).

A block: three sections (rotations with 4 components, positions and scales with 3), each: a u64 key mask per animated
joint (bit i: a key at frame startTime + i), per animated joint its u16 index and its keys (the values, then the
per-frame tangents, int16 each), per constant joint its u16 index and its values, then a u16 count and the joints
that take the rest pose; each section padded to 8 bytes. After the last block: padding to 16 bytes and 16 more.
Between keys the game samples a cubic Hermite spline and normalises rotations (read from the game's clip sampler,
build 1273043).
"""
import math
import struct

ROT_SCALE = 1.0 / 32767      # rotation values
TANGENT_SCALE = 2.0 ** -17   # rotation and scale tangents (per frame)
BLOCK = 63                   # frames per block (blocks share their boundary frame)
LAST_FRACTION = 0.999        # a clip that does not loop holds its last frame at this fraction
SECTIONS = (("rot", "OrientationJoints", 4), ("pos", "PositionJoints", 3), ("scl", "ScaleJoints", 3))
# encode stores a frame as a key only where the spline between its neighbouring keys misses it by more than this:
# radians for rotations, metres for positions, the factor for scales
TOLERANCE = {"rot": 5e-4, "pos": 5e-5, "scl": 5e-5}

_parsed = {}


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def hierarchy_hash(joint_name_hashes):
    """A clip's hierarchyHash for a skeleton: FNV-1a 64 over its joint name hashes (CRC32 of the lowercase names, as
    the skeleton's jointNames store them), each as a little-endian u32, in joint order. Checked on six game
    skeletons; proportions do not count, so a skeleton with other bone lengths keeps its clips."""
    return fnv1a64(struct.pack("<%dI" % len(joint_name_hashes), *[h & 0xFFFFFFFF for h in joint_name_hashes]))


def _bytes(clip):
    return bytes(x for unit in clip["data"] for x in unit["data"])


def parse(clip):
    """The clip's blocks: [{start, end, rot|pos|scl: (keyed {joint: (mask, keys)}, const {joint: values})}]."""
    hit = _parsed.get(id(clip))
    if hit is not None and hit[0] is clip:
        return hit[1]
    raw = _bytes(clip)
    blocks = []
    for h in clip["headers"]:
        o = h["dataOffset"]
        blk = {"start": h["startTime"], "end": h["endTime"]}
        for name, field, comps in SECTIONS:
            nonconst, used = h["nonConst" + field], h["used" + field]
            masks = struct.unpack_from("<%dQ" % nonconst, raw, o)
            o += 8 * nonconst
            keyed = {}
            for m in masks:
                joint, = struct.unpack_from("<H", raw, o)
                n = bin(m).count("1")
                v = struct.unpack_from("<%dh" % (2 * comps * n), raw, o + 2)
                keyed[joint] = (m, [v[i * 2 * comps:(i + 1) * 2 * comps] for i in range(n)])
                o += 2 + 4 * comps * n
            const = {}
            for _ in range(used - nonconst):
                joint, = struct.unpack_from("<H", raw, o)
                const[joint] = struct.unpack_from("<%dh" % comps, raw, o + 2)
                o += 2 + 2 * comps
            count, = struct.unpack_from("<H", raw, o)
            o += 2 + 2 * count
            o += (-o) % 8
            blk[name] = (keyed, const)
        blocks.append(blk)
    if len(_parsed) > 64:
        _parsed.clear()
    _parsed[id(clip)] = (clip, blocks)
    return blocks


def _hermite(t):
    t2 = t * t
    t3 = t2 * t
    return 2 * t3 - 3 * t2 + 1, t3 - 2 * t2 + t, -2 * t3 + 3 * t2, t3 - t2


def _keyed(mask, keys, start, f, frac, comps, vscale, tscale):
    b = min(f - start + 1, BLOCK)
    below = mask & ((1 << b) - 1)
    above = mask >> b << b
    f0 = start + below.bit_length() - 1
    f1 = start + (above & -above).bit_length() - 1
    i = bin(below).count("1") - 1
    k0, k1 = keys[i], keys[i + 1]
    dt = f1 - f0
    a, bb, c, d = _hermite((f + frac - f0) / dt)
    return tuple(k0[n] * vscale * a + k0[comps + n] * tscale * dt * bb + k1[n] * vscale * c
                 + k1[comps + n] * tscale * dt * d for n in range(comps))


def _sample(clip, f, frac, rest):
    blocks = parse(clip)
    blk = next((x for x in blocks if f < x["end"]), blocks[-1])
    scales = {"rot": (ROT_SCALE, TANGENT_SCALE), "pos": (clip["positionFactor"], clip["positionTangent"]),
              "scl": (clip["scaleFactor"], TANGENT_SCALE)}
    out = []
    for j in range(clip["jointCount"]):
        pose = list(rest[j])
        for n, (name, _, comps) in enumerate(SECTIONS):
            keyed, const = blk[name]
            vscale, tscale = scales[name]
            if j in keyed:
                v = _keyed(*keyed[j], blk["start"], f, frac, comps, vscale, tscale)
                if name == "rot":
                    norm = math.sqrt(sum(x * x for x in v)) or 1.0
                    v = tuple(x / norm for x in v)
                pose[n] = v
            elif j in const:
                pose[n] = tuple(x * vscale for x in const[j])
        out.append(tuple(tuple(p) for p in pose))
    return out


def sample(clip, frame, rest):
    """The pose at a whole frame, as the game samples it: a looping clip wraps, another holds its last frame.
    rest: per joint (rotation, position, scale), for joints the clip leaves at the rest pose."""
    n = clip["frameCount"]
    if clip["flags"] == 1:
        return _sample(clip, frame % n, 0.0, rest)
    if frame >= n:
        return _sample(clip, n - 1, LAST_FRACTION, rest)
    return _sample(clip, frame, 0.0, rest)


def sample_end(clip, rest):
    """The pose of the clip's end key (frame frameCount), which the game blends towards but never shows whole."""
    return _sample(clip, clip["frameCount"] - 1, 1.0, rest)


def _close(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def _tangents(values, loop):
    """Per-frame tangents of one channel (frames 0..N): central differences; a loop wraps around."""
    n = len(values) - 1
    out = []
    for i in range(n + 1):
        if 0 < i < n:
            a, b = values[i - 1], values[i + 1]
            out.append(tuple((y - x) / 2 for x, y in zip(a, b)))
        elif loop and n > 1:
            a, b = values[n - 1], values[1]
            out.append(tuple((y - x) / 2 for x, y in zip(a, b)))
        elif i == 0:
            out.append(tuple(y - x for x, y in zip(values[0], values[1])) if n else (0.0,) * len(values[0]))
        else:
            out.append(tuple(y - x for x, y in zip(values[n - 1], values[n])))
    return out


def _q16(v, scale):
    return max(-32767, min(32767, int(round(v / scale))))


def _miss(name, got, want):
    """How far a spline value is from the frame's own: the angle between two rotations (the spline's normalised, as
    the game does), else the largest component difference."""
    if name == "rot":
        ng = math.sqrt(sum(x * x for x in got)) or 1.0
        nw = math.sqrt(sum(x * x for x in want)) or 1.0
        return 2 * math.acos(min(1.0, abs(sum(x * y for x, y in zip(got, want))) / (ng * nw)))
    return max(abs(x - y) for x, y in zip(got, want))


def _key_frames(name, values, tangents, start, last):
    """The frames of start..last stored as keys: the first, the last, and each next key as far on as the spline from
    the key before still gives every frame between within TOLERANCE (the game samples that spline: _keyed)."""
    tol = TOLERANCE[name]
    keys = [start]
    while keys[-1] < last:
        a = keys[-1]
        good = a + 1
        for b in range(a + 2, last + 1):
            dt = b - a
            if any(_miss(name, tuple(values[a][n] * h0 + tangents[a][n] * dt * h1 + values[b][n] * h2
                                     + tangents[b][n] * dt * h3 for n in range(len(values[a]))), values[f]) > tol
                   for f in range(a + 1, b) for h0, h1, h2, h3 in (_hermite((f - a) / dt),)):
                break
            good = b
        keys.append(good)
    return keys


def encode(poses, rest, fps=60, loop=False, hierarchy_hash=0):
    """A clip from poses for frames 0..N (N + 1 poses; a loop's last pose is its first again), N = frameCount.
    A frame is a key where the spline between the keys around it would miss it (_key_frames, TOLERANCE); every key
    keeps the frame's own tangent. rest: per joint (rotation, position, scale): joints that stay at it in a block are
    left to the rest pose. hierarchy_hash: the skeleton's (copy it from a game clip on the same skeleton)."""
    n = len(poses) - 1
    joints = len(rest)
    channels = {"rot": [], "pos": [], "scl": []}
    for j in range(joints):
        qs = []
        for p in poses:
            q = tuple(p[j][0])
            if qs and sum(a * b for a, b in zip(q, qs[-1])) < 0:
                q = tuple(-x for x in q)
            qs.append(q)
        channels["rot"].append(qs)
        channels["pos"].append([tuple(p[j][1]) for p in poses])
        channels["scl"].append([tuple(p[j][2]) for p in poses])
    tangents = {k: [_tangents(v, loop) for v in vals] for k, vals in channels.items()}

    # each block's joints per channel: keyed, constant, or left at the rest pose
    ranges = []
    start = 0
    while True:
        last = min(start + BLOCK, n)
        ranges.append((start, last))
        if last >= n:
            break
        start += BLOCK
    kinds = []
    for start, last in ranges:
        per = {}
        for name, _, _ in SECTIONS:
            keyed, const, still = [], [], []
            for j in range(joints):
                vals = channels[name][j][start:last + 1]
                restv = rest[j][("rot", "pos", "scl").index(name)]
                if name == "rot":
                    same_as_rest = all(abs(sum(a * b for a, b in zip(v, restv))) >= 1 - 1e-12 for v in vals)
                else:
                    same_as_rest = all(_close(v, restv, 1e-7) for v in vals)
                if same_as_rest:
                    still.append(j)
                elif all(_close(v, vals[0], 1e-7) for v in vals):
                    const.append(j)
                else:
                    keyed.append(j)
            per[name] = (keyed, const, still)
        kinds.append(per)

    def biggest(name, of_tangents=False):
        """The largest stored component of a channel (only joints a block stores: keyed, or constant for values)."""
        top = 0.0
        for (start, last), per in zip(ranges, kinds):
            keyed, const, _ = per[name]
            src = tangents if of_tangents else channels
            for j in keyed + ([] if of_tangents else const):
                for frame in src[name][j][start:last + 1]:
                    top = max(top, max(abs(x) for x in frame))
        return top

    pos_factor = max(biggest("pos"), 1e-6) / 32767
    pos_tangent = max(biggest("pos", True), 1e-9) / 32767
    scl_factor = max(biggest("scl"), 1e-6) / 32767
    scales = {"rot": (ROT_SCALE, TANGENT_SCALE), "pos": (pos_factor, pos_tangent), "scl": (scl_factor, TANGENT_SCALE)}

    raw = bytearray()
    headers = []
    for (start, last), per in zip(ranges, kinds):
        h = {"dataOffset": len(raw), "startTime": start, "endTime": start + BLOCK}
        for name, field, comps in SECTIONS:
            vscale, tscale = scales[name]
            keyed, const, still = per[name]
            frames = {j: _key_frames(name, channels[name][j], tangents[name][j], start, last) for j in keyed}
            raw += struct.pack("<%dQ" % len(keyed), *[sum(1 << (f - start) for f in frames[j]) for j in keyed])
            for j in keyed:
                raw += struct.pack("<H", j)
                for f in frames[j]:
                    v, m = channels[name][j][f], tangents[name][j][f]
                    raw += struct.pack("<%dh" % (2 * comps), *([_q16(x, vscale) for x in v]
                                                              + [_q16(x, tscale) for x in m]))
            for j in const:
                raw += struct.pack("<H%dh" % comps, j, *[_q16(x, vscale) for x in channels[name][j][start]])
            raw += struct.pack("<H%dH" % len(still), len(still), *still)
            raw += bytes((-len(raw)) % 8)
            h["used" + field] = len(keyed) + len(const)
            h["nonConst" + field] = len(keyed)
        headers.append(h)
    raw += bytes((-len(raw)) % 16) + bytes(16)
    return {"hierarchyHash": hierarchy_hash, "positionFactor": pos_factor, "positionTangent": pos_tangent,
            "scaleFactor": scl_factor, "headers": headers,
            "data": [{"data": list(raw[i:i + 16])} for i in range(0, len(raw), 16)],
            "fps": fps, "frameCount": n, "jointCount": joints, "flags": 1 if loop else 0}
