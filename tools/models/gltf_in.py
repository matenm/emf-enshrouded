"""Read a skinned .glb (from Blender or gamegltf.py) into encode.py vertex groups in GAME space.

Bones are matched to the game skeleton by name: every bone must be called 'jNNN' or 'jNNN_<anything>' (the names
gamegltf.py exports), NNN being the game joint index. Vertices are taken in the file's bind space and moved into the
game skeleton's bind space with each joint's matrices (identity when the artist kept the game's rest pose).
Materials map by name 'game_material_N' -> N, otherwise in order of first use.
"""
import json
import math
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamegltf  # noqa: E402

COMP = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
NORM_DIV = {5120: 127.0, 5121: 255.0, 5122: 32767.0, 5123: 65535.0}
JOINT_NAME = re.compile(r"^j(\d{1,4})(?:_|$)")


def read_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, _ = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67 or version != 2:
        raise ValueError("%s is not a glTF 2 binary (.glb)" % path)
    off, gltf, binc = 12, None, b""
    while off < len(data):
        length, ctype = struct.unpack_from("<II", data, off)
        chunk = data[off + 8: off + 8 + length]
        if ctype == 0x4E4F534A:
            gltf = json.loads(chunk.decode("utf-8"))
        elif ctype == 0x004E4942:
            binc = chunk
        off += 8 + length
    return gltf, binc


def accessor(gltf, binc, index):
    acc = gltf["accessors"][index]
    fmt, size = COMP[acc["componentType"]]
    width = WIDTH[acc["type"]]
    count = acc["count"]
    if "bufferView" not in acc:
        return [tuple([0] * width) if width > 1 else 0 for _ in range(count)]
    view = gltf["bufferViews"][acc["bufferView"]]
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = view.get("byteStride") or size * width
    norm = acc.get("normalized") and acc["componentType"] in NORM_DIV
    out = []
    for i in range(count):
        vals = struct.unpack_from("<%d%s" % (width, fmt), binc, base + i * stride)
        if norm:
            vals = tuple(v / NORM_DIV[acc["componentType"]] for v in vals)
        out.append(vals if width > 1 else vals[0])
    return out


def mat_from_colmajor(v):
    return [[v[c * 4 + r] for c in range(4)] for r in range(4)]


def mat_mul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def mat_inv_affine(m):
    a = [row[:3] for row in m[:3]]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    inv = [[(a[(c + 1) % 3][(r + 1) % 3] * a[(c + 2) % 3][(r + 2) % 3] - a[(c + 1) % 3][(r + 2) % 3] * a[(c + 2) % 3][(r + 1) % 3]) / det
            for c in range(3)] for r in range(3)]
    t = [m[r][3] for r in range(3)]
    nt = [-sum(inv[r][k] * t[k] for k in range(3)) for r in range(3)]
    return [inv[0] + [nt[0]], inv[1] + [nt[1]], inv[2] + [nt[2]], [0.0, 0.0, 0.0, 1.0]]


def xform_point(m, p):
    return tuple(sum(m[r][k] * p[k] for k in range(3)) + m[r][3] for r in range(3))


def xform_dir(m, v):
    out = [sum(m[r][k] * v[k] for k in range(3)) for r in range(3)]
    n = math.sqrt(sum(c * c for c in out)) or 1.0
    return tuple(c / n for c in out)


def tangents_from_uv(pos, nrm, uv, tris):
    """Simple per-vertex tangents (accumulated per triangle, Gram-Schmidt) when the file has none."""
    acc = [[0.0, 0.0, 0.0] for _ in pos]
    bacc = [[0.0, 0.0, 0.0] for _ in pos]
    for a, b, c in tris:
        e1 = [pos[b][k] - pos[a][k] for k in range(3)]
        e2 = [pos[c][k] - pos[a][k] for k in range(3)]
        du1, dv1 = uv[b][0] - uv[a][0], uv[b][1] - uv[a][1]
        du2, dv2 = uv[c][0] - uv[a][0], uv[c][1] - uv[a][1]
        r = du1 * dv2 - du2 * dv1
        if abs(r) < 1e-12:
            continue
        r = 1.0 / r
        t = [(e1[k] * dv2 - e2[k] * dv1) * r for k in range(3)]
        bt = [(e2[k] * du1 - e1[k] * du2) * r for k in range(3)]
        for i in (a, b, c):
            for k in range(3):
                acc[i][k] += t[k]
                bacc[i][k] += bt[k]
    out = []
    for i, n in enumerate(nrm):
        t = acc[i]
        d = sum(t[k] * n[k] for k in range(3))
        t = [t[k] - n[k] * d for k in range(3)]
        ln = math.sqrt(sum(c * c for c in t))
        if ln < 1e-9:
            t = [1.0, 0.0, 0.0] if abs(n[0]) < 0.9 else [0.0, 1.0, 0.0]
            d = sum(t[k] * n[k] for k in range(3))
            t = [t[k] - n[k] * d for k in range(3)]
            ln = math.sqrt(sum(c * c for c in t))
        t = [c / ln for c in t]
        cr = [n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]]
        w = -1.0 if sum(cr[k] * bacc[i][k] for k in range(3)) < 0 else 1.0
        out.append((t[0], t[1], t[2], w))
    return out


def load_groups(path, hier, material_count=None, warn=print):
    """Return (groups, report). Groups are in game space with game joint indices."""
    gltf, binc = read_glb(path)
    nodes = gltf.get("nodes", [])
    game_ibm = [gamegltf.mirror_mat(gamegltf.inverse_bind_rows(e)) for e in hier["skinToJointMatrices"][:hier["jointCount"]]]
    game_bind = [mat_inv_affine(m) for m in game_ibm]  # joint -> bind space (glTF side of the mirror)
    report = {"meshes": 0, "max_rest_difference": 0.0, "unmapped_bones": []}
    material_ids = {}
    by_material = {}
    for node in nodes:
        if "mesh" not in node or "skin" not in node:
            continue
        skin = gltf["skins"][node["skin"]]
        joint_nodes = skin["joints"]
        to_game = []
        for jn in joint_nodes:
            m = JOINT_NAME.match(nodes[jn].get("name", ""))
            if not m or int(m.group(1)) >= hier["jointCount"]:
                to_game.append(None)
                report["unmapped_bones"].append(nodes[jn].get("name", "?"))
            else:
                to_game.append(int(m.group(1)))
        if "inverseBindMatrices" in skin:
            file_ibm = [mat_from_colmajor(v) for v in accessor(gltf, binc, skin["inverseBindMatrices"])]
        else:
            file_ibm = [[[1.0 if r == c else 0.0 for c in range(4)] for r in range(4)] for _ in joint_nodes]
        # per file joint: file bind space -> game bind space (glTF handedness)
        fix = []
        for fi, gi in enumerate(to_game):
            if gi is None:
                fix.append(None)
                continue
            f = mat_mul(game_bind[gi], file_ibm[fi])
            diff = max(abs(f[r][c] - (1.0 if r == c else 0.0)) for r in range(3) for c in range(4))
            report["max_rest_difference"] = max(report["max_rest_difference"], diff)
            fix.append(f)
        for prim in gltf["meshes"][node["mesh"]]["primitives"]:
            if prim.get("mode", 4) != 4:
                warn("skipping a primitive that is not a triangle list")
                continue
            a = prim["attributes"]
            pos = accessor(gltf, binc, a["POSITION"])
            n = len(pos)
            nrm = accessor(gltf, binc, a["NORMAL"]) if "NORMAL" in a else [(0.0, 1.0, 0.0)] * n
            uv = accessor(gltf, binc, a["TEXCOORD_0"]) if "TEXCOORD_0" in a else [(0.0, 0.0)] * n
            col = None
            if "COLOR_0" in a:
                cs = accessor(gltf, binc, a["COLOR_0"])
                col = [tuple(int(round(max(0.0, min(1.0, c[k] if k < len(c) else 1.0)) * 255)) for k in range(4)) for c in cs]
            joints = accessor(gltf, binc, a["JOINTS_0"]) if "JOINTS_0" in a else None
            weights = accessor(gltf, binc, a["WEIGHTS_0"]) if "WEIGHTS_0" in a else None
            if joints is None or weights is None:
                raise ValueError("mesh '%s' is not skinned (no JOINTS_0/WEIGHTS_0)" % node.get("name"))
            idx = accessor(gltf, binc, prim["indices"]) if "indices" in prim else list(range(n))
            tris = [tuple(idx[i:i + 3]) for i in range(0, len(idx) - 2, 3)]
            tan = accessor(gltf, binc, a["TANGENT"]) if "TANGENT" in a else None
            gpos, gnrm, gtan, gskin = [], [], [], []
            for i in range(n):
                js = [to_game[j] for j in joints[i]]
                ws = list(weights[i])
                pairs = [(w, j, fj) for w, j, fj in zip(ws, js, joints[i]) if w > 0 and j is not None]
                if not pairs:
                    raise ValueError("vertex %d of '%s' has no weight on a game bone" % (i, node.get("name")))
                tot = sum(w for w, _, _ in pairs)
                # blended file->game bind fix (identity when the rest pose matches)
                m = [[sum(w / tot * fix[fj][r][c] for w, _, fj in pairs) for c in range(4)] for r in range(4)]
                p = xform_point(m, pos[i])
                nn = xform_dir(m, nrm[i])
                gpos.append(gamegltf.mirror_vec(p))
                gnrm.append(gamegltf.mirror_vec(nn))
                if tan is not None:
                    t = xform_dir(m, tan[i][:3])
                    gtan.append((-t[0], t[1], t[2], -tan[i][3]))
                gskin.append(([j for _, j, _ in pairs], [w / tot for w, _, _ in pairs]))
            if tan is None:
                # tangents in glTF space from the file's (mirrored back) data
                rh_pos = [gamegltf.mirror_vec(p) for p in gpos]
                rh_nrm = [gamegltf.mirror_vec(v) for v in gnrm]
                rh_tan = tangents_from_uv(rh_pos, rh_nrm, uv, tris)
                gtan = [(-t[0], t[1], t[2], -t[3]) for t in rh_tan]
            mat_key = prim.get("material", -1)
            if mat_key not in material_ids:
                name = gltf["materials"][mat_key].get("name", "") if mat_key >= 0 else ""
                mm = re.match(r"^game_material_(\d+)", name)
                material_ids[mat_key] = int(mm.group(1)) if mm else len(material_ids)
            mid = material_ids[mat_key]
            if material_count is not None and mid >= material_count:
                raise ValueError("material %d does not exist on the source model (it has %d)" % (mid, material_count))
            g = by_material.setdefault(mid, {"material": mid, "pos": [], "nrm": [], "tan": [], "col": [] if col else None,
                                             "uv": [], "skin": [], "lods": [[]]})
            base = len(g["pos"])
            g["pos"] += gpos
            g["nrm"] += gnrm
            g["tan"] += gtan
            g["uv"] += [tuple(t) for t in uv]
            g["skin"] += gskin
            if g["col"] is not None:
                g["col"] += col or [(255, 255, 255, 255)] * n
            g["lods"][0] += [(a_ + base, b_ + base, c_ + base) for a_, b_, c_ in tris]
            report["meshes"] += 1
    if not by_material:
        raise ValueError("no skinned meshes in %s" % path)
    groups = [by_material[k] for k in sorted(by_material)]
    for g in groups:
        if g["col"] is None:
            g.pop("col")
    if report["max_rest_difference"] > 1e-3:
        warn("the file's rest pose differs from the game skeleton (max %.4f); vertices were moved into the game's "
             "rest pose, but the game's animations will drive the game skeleton, not yours" % report["max_rest_difference"])
    if report["unmapped_bones"]:
        warn("bones without a game joint (named jNNN...) were ignored: %s" % ", ".join(sorted(set(report["unmapped_bones"]))[:10]))
    return groups, report
